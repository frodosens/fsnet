# Runtime code review — 2026-10-24

审查基线：`130ef7a`，分支 `refactor/script-runtime`。原审查提交 `d6decc2` 记录问题；下面的复现数据保留为修复前证据。

## 修复状态

七项已确认缺陷均已修复，关键路径补上正式回归测试，输入缓冲释放另以原探针复核；验证结果见文末。

| 问题 | 处理方式 |
| --- | --- |
| Node 回调内销毁 | poll/run 保留 Impl 生命周期；停止后的取消回调在外层驱动退出后排空，禁止递归驱动 |
| RPC 策略不一致 | transport 更新成功后更新共享 Method；排队、挂起及持久化入口执行前/提交前检查当前权限；缺少调用者上下文的旧路由拒绝动态设置策略（该路由实现现已删除） |
| 收据重放漏激活 | 重放归为协议操作，不能把新 owner 标记为已激活 |
| 不确定提交丢通知 | 使订阅失效、提升 handle generation、清除临时缓存及激活标志；重订阅从存储恢复，重试仍走收据/inbox 去重 |
| Lua 耗时模式匹配 | 固定版本 Lua 字符串库在匹配循环内计费，find/match/gmatch/gsub 共用每次 resume 的预算；pcall 不能掩盖超限，返回后也检查 deadline |
| Lua 输入容量保留 | 输入复制入 VM 后释放 native string capacity |
| 收帧内存放大 | token 握手帧严格限长；所有连接共享默认 16MiB 接收帧体预算并提供统计，读取完成或取消后释放 |

另外修复 `send_entity` 回调抛异常时可能二次调用的问题，以及 Gateway 恢复订阅后旧路由缓存影响紧随其后的 RPC 的问题。Gateway 对已过期、无指定 placement 的实体记录本轮失败端点，避免慢轮询时反复选择同一离线节点；有效租约及指定交接目标仍须遵守。投递扫描改为一整轮结束后退避，槽位释放后立即继续本轮，避免每个 32-source 批次都等待 100ms；目录路径共享只读视图，避免复制实体状态和解码投递映射。

仍保留的架构限制：历史 registry/channel 水位暂无自动退休协议；Storage.get 仍读取完整记录，实体事务仍持久化整份快照；Lua 的模式匹配已有工作预算，其他 C 原语不构成可抢占的敌对脚本沙箱。以上不应解读为无限容量、恒定尾延迟或公网生产验收已完成。

范围：重点追踪 Node/C API、客户端与 Gateway、属性订阅、实体事务/路由/迁移/持久化投递、存储异常、Lua 调度与属性代理/预测的关键路径；同时检查 ICE/TURN 的交付边界。属于静态审查加定向故障复现，不是完整形式验证或公网压力测试。P1 表示建议在生产交付前修复；P2 表示明确的资源约束缺口。

## 已确认的问题

### 1. P1 — 原生 Node 在 RPC 回调中销毁会发生 use-after-free

位置：[Node::poll/run](../src/node.cpp#L414)。

触发：在 `Node::call` 的完成回调中释放持有该 Node 的 `unique_ptr`。`poll()` 没有在栈上保留 Impl 的强引用，析构会释放仍在执行的 Asio context/services。后续 read handler 释放 Session/timer 时访问已释放的 timer service。

复现：ASAN 构建执行 `destroy`，稳定报告 `heap-use-after-free`，栈包含 `Session::~Session`、`io_object_impl::~io_object_impl`、`Node::poll`。这不是跨线程用法。公共接口只规定 owner thread，没有禁止回调内销毁。

建议：让运行中的 poll/run 持有完整实现生命周期，检查 stop/析构的回调重入，并覆盖 call、handler、close 回调中销毁/停止的组合。已有 SDK/C 包装层的防护不能替代原生 Node 的保证。

### 2. P1 — 修改 RPC 权限后，持久化投递仍使用旧权限

位置：[rpc_policy](../src/entity_host.cpp#L554)、[投递执行前检查](../src/entity_host.cpp#L370)。

`EntityScriptHost::rpc_policy` 只更新 parent transport，未更新保存的 Method policy；`receive_delivery` 则检查该缓存。先注册允许 server 的 `Player.reward`，再撤销 server 权限，正常 transport policy 已拒绝，持久化入口仍执行 reward。

复现输出：`transport_server_allowed=0 handler_executed=1 status=0`。

影响：运行时撤销权限无法一致生效。复现中的调用方仍是可信 server；这不意味着匿名客户端能伪造 server 身份。`Node::rpc_policy` 会拒绝 owner_only 策略，不能据此声称 owner_only 能动态切换成功。

建议：统一更新与读取策略的来源，在执行和提交边界明确处理已经排队/挂起的调用；补普通 RPC、持久化 RPC 的权限撤销交叉测试。

### 3. P1 — 普通 RPC 收据重放会让新 owner 永久跳过 on_activate

位置：[收据重放提前返回](../src/entity_host.cpp#L387)、[成功完成标记 activated](../src/entity_host.cpp#L126)。

触发：A 已提交 request_id=one，然后释放实体；B 收到相同 request_id，命中收据，只返回旧结果，但 complete 把 B 的 slot 标记为已激活。下一次真正执行的 RPC 也跳过 `on_activate`。

复现：`first=1 replay=1 active_after_replay=1 next=1 expected_next=2`。测试脚本在 on_activate 中递增持久化计数，B 的激活没有发生。

影响：故障切换/ACK 丢失后，依赖激活建立的本地定时器、场景登记、临时状态可能始终缺失。Gateway 预先读取属性的常见流程会掩盖此问题，直接路由重试仍可触发。

建议：区分“返回已提交结果”和“本 owner 执行过激活”；重放不能凭空设置激活状态。补收据重放作为新 owner 首个请求的测试。

### 4. P1 — 存储已经提交但返回异常，重试成功后属性通知仍可能永久缺失

位置：[CAS 异常完成](../src/entity_host.cpp#L535)、[仅成功时发布变化](../src/entity_host.cpp#L135)、[订阅轮询](../src/attribute_replication.cpp#L287)。

触发：存储实际提交 reward 的属性和 inbox，但提交返回前抛出异常。host 返回 internal，因此不发布属性变化。随后持久化重试命中 inbox，仅返回成功 ACK；后续 read_attributes 的 previous/current 已相等，也不补发。

复现使用“成功执行底层 CAS 后抛异常”的 Storage 包装，输出：`first_status=6 retry_status=0 stored_gold=1 attribute_events=0`。验证了持久值存在而变化事件为零；订阅端续租和 poll 不核对存储 revision，因此静态追踪显示现有在线副本可一直保留旧值，直到重新订阅或发生其他变化。

这是真实存储可能出现的语义：[文件 rename 成功后目录 fsync 失败](../src/file_storage.cpp#L118)，或者 PostgreSQL COMMIT 成功但连接丢失。不能把所有异常理解为“没有提交”。

建议：为提交结果不确定设计读回/校对协议，或使相关订阅失效并重新取快照；不要盲目重发业务处理。补 commit-then-throw 与订阅恢复测试，现有 fail-before-write 测试不足。

### 5. P1 — Lua C 标准库调用可绕过指令预算并阻塞整个 owner 线程

位置：[instruction hook](../src/lua_engine.cpp#L1186)、[开放 string 库](../src/lua_engine.cpp#L1197)、[resume 返回处理](../src/lua_engine.cpp#L1426)。

Lua hook 只统计 VM 指令，`string.find` 的 C 匹配过程不能被该 hook 抢占。设置每次 resume 1000 指令、任务超时 10ms，执行：

```lua
runtime.rpc('probe', function()
    local s = string.rep('a', 100)
    return tostring(s:find('a*a*a*a*b'))
end)
```

实测约 **1113ms**，仍返回 `Status::ok`；长度 200 的版本运行超过 3 秒，由外部进程终止。耗时标准库执行期间，owner 线程不能处理网络、租约和其他任务，返回成功前也未重新检查 deadline。

项目明确定位可信脚本宿主，不能把它宣称为敌对代码安全沙箱；但固定脚本使用外部可控字符串，也可能意外触发此可用性问题。

建议：约束匹配输入/模式长度，替换可能超线性回溯的原语；对敌对脚本采用独立进程。恢复后复查 deadline 可以修正结果语义，但不能解决 C 调用中的阻塞。

### 6. P2 — 已运行 Lua 任务保留未计费的输入缓冲区

位置：[队列转为活动任务](../src/lua_engine.cpp#L1629)、[clear 输入](../src/lua_engine.cpp#L1639)。

输入已经拷进 Lua 后，`task.data.clear()` 保留 std::string capacity；同时 queued_bytes 已扣减。任务 await/sleep 时，该 native 内存既不在队列计费，也不在 Lua allocator 的 memory_used 中。

复现：VM 上限 512KiB，64 个任务各携带 256KiB 输入，脚本丢弃参数并 sleep；`active=64`、`vm_bytes=445481`，RSS 从约 9.8MiB 增到 26.9MiB。VM 限额本来就不等于进程 RSS，但这里的整份重复输入已经无用，且逃出了接收队列的资源统计。EntityHost 的额外预算可以缓解实体 RPC，不能覆盖所有无状态 RPC。

建议：复制成功后释放 native 输入容量，或在整个活动任务生命周期持续计费；补等待任务并发下的 native buffer 统计测试。

### 7. P2 — 收帧前预分配缺少全局预算，应用鉴权前可放大内存

位置：[read_header/body.resize](../src/node.cpp#L178)、[默认 Limits](../include/fsnet/node.hpp#L21)。

读取合法的 4 字节长度头后立即 resize 整个 body，尚未收到正文/token，也未执行应用 authorizer。默认每帧 1MiB、1024 个连接，没有全局接收缓冲字节配额。

复现：64 个 loopback 连接，只发 1MiB 长度头，不发正文；`authorize_calls=0`，RSS 从 9968KiB 增到 76124KiB。默认上限组合允许约 1GiB 的 body 占用，连接 idle timeout 只限制持续时间。

范围：明文接口被限制为 loopback；公网 mTLS 需要有效客户端证书。这不是 TLS 身份认证绕过，但持有效证书的普通客户端仍可在应用鉴权前消耗资源。

建议：增加全局接收字节预算、按身份的连接/速率限制；在首次应用认证完成前采用小帧上限或分段、按需收取 body。

## 性能与容量风险

以下是代码路径分析与默认值推算，不是吞吐实测，也不全部属于新发现的正确性缺陷。

- **持久化投递不是 100ms 延迟保证。** [EntityDelivery::poll](../src/entity_delivery.cpp#L98) 每轮扫描 sources 并占用 requests 槽。按默认每 100ms 一轮、每轮仅能处理 32 个 source 推算，4096 个 source 一次完整扫描约 12.8 秒；真实值还受 poll budget、异步完成与 I/O 影响。黑洞目标占用槽直到默认 3 秒超时。每个 owner 重复全局扫描，历史空 source 也消耗读取。建议引入可通知的 ready 队列/分片索引，并测尾延迟。
- **source registry/inbox/sent 是历史累计容量。** source index 默认 4096，已完成甚至未最终提交的发送者也可能留在索引；单实体 inbox/sent 各有 256 个历史 channel 上限。遗留去重水位不能直接删除，否则会重复执行旧消息。需要明确 epoch-aware 回收/退休协议及容量告警；否则长期运行后新来源可能一直无法投递。现有文档已有这些容量约束，不能当作“无限可靠消息队列”。
- **路由读取放大。** [EntityDirectory](../include/fsnet/entity_directory.hpp#L8) 为 owner/lease 解码整个 EntityRecord，包括状态和投递数据；Gateway/迁移刷新放大热实体读成本。应分离轻量 placement 元数据，或提供避免全量解码的共享读取接口。
- **字段 patch 节省网络，不代表端到端 O(改动字段数)。** Lua replica 刷新仍重建属性模型，部分快照持有场景会触发写时复制；实体事务持久化仍序列化/CAS 整份记录。大结构、高频移动、宽扇出需要分别测分配、编码、存储和客户端解码成本。
- 文件后端每次事务 fsync，不适合把 FPS 每个输入都当高频持久化事务；内存数据库的整库复制实现是开发后端，也不能直接代替生产方案。P2P 原语验证不等于完整公网安全会话交付。

## 验证与复现

本次 Debug 全套首次执行 **38/39 通过**；`fsnet-delivery` 在迁移恢复场景超时，客户端日志为 `RPC failed: 4`，Gateway 当时 `migrating=1, failures=1`。单独复跑该测试 **通过（6.14s）**。因此记录为未定位的间歇性恢复失败，不能报告全绿，也不能仅凭一次日志归因到上述某个缺陷。

上述 7 项均有独立复现。原生销毁问题另外使用 ASAN/UBSAN 构建确认；未把所有既有测试重新运行于 sanitizer。耗时/RSS 是本机定向观测，不是跨环境性能承诺。

```bash
. tools/dev/activate.sh
# 需要已构建开启 Lua 的对应 preset；Linux，frame 使用 loopback TCP
python3 tools/review/run_runtime_review.py --build .dev/build/debug
# 最后一项故意触发 ASAN 错误，在独立子进程中运行
python3 tools/review/run_runtime_review.py --build .dev/build/asan --asan --include-crash
```

工具输出每个 case 的 JSON 观测（stdout、stderr、退出码），每项外部超时 8 秒。`returncode=0` 仅表示探针运行完成，**不表示不存在缺陷**。未接入默认 CTest，避免把“缺陷仍可复现”误记为通过的回归测试。工具只写临时构建文件，不修改业务存储或生产服务；frame 最多建立 64 个本地连接。

建议修复顺序：Node 生命周期安全 → RPC 权限一致性 → 重放/不确定提交恢复 → Lua 可用性与资源预算 → 全局收帧预算 → 投递索引与高频同步性能。每个修复应将相应探针转换为断言正确行为的正式回归测试。

## 修复后验证（2026-10-24）

最终 Debug **39/39**、ASAN/UBSAN **23/23**、Lua OFF **13/13** 全部通过。Gateway 与真实 mTLS 投递恢复测试各连续运行三次通过；不能据此承诺所有部署环境都不会出现恢复失败，但已补上慢轮询时的确定性回归。Python 夹具语法和 `git diff --check` 通过。

原探针再次运行的 Debug 观测：权限撤销后 `handler_executed=0`；收据重放后 `active_after_replay=0, next=2`；不确定提交得到 `stored_gold=1, invalidations=1`，真实 AttributeClient 回归确认旧副本移除、重新订阅后恢复新值，重试不重复扣血。64 个挂起 Lua 请求的 RSS 增量约 **1.05MiB**（原约 17MiB）；64 个仅长度头连接全部关闭，RSS 增量约 **196KiB**。同一回溯模式约 **1.22ms** 返回 overloaded，回调中销毁正常完成，ASAN 独立探针也未再报告 UAF。RSS 与时间仅为本机观测，ASAN 隔离区会影响 RSS，不能直接与普通构建比较。

```sh
. tools/dev/activate.sh
cmake --build --preset debug
ctest --preset debug
cmake --build --preset asan
ctest --preset asan
cmake --build --preset core
ctest --preset core
ctest --preset debug -R '^fsnet-(gateway|delivery)$' --repeat until-fail:3
python3 tools/review/run_runtime_review.py --build .dev/build/debug --include-destroy
```

Node 的公开 Limits/TransportStats 新增字段，C++ 使用方应与库一起重新编译。Lua 不提供敌对代码进程隔离；历史去重水位的安全回收、大记录存储拆分及公网交付验证仍为独立后续工作。

后续 Actions 的 Release 配置暴露了 show/possess 中间通知提前完成恢复及重复恢复缺少确认的问题，另有独立进程夹具的目录清理次序竞争。已改为最终快照/控制状态确认，并先回收子进程再删目录；详见 [Actions 修复验证](VALIDATION.md#actions-迁移恢复与进程清理修复2026-10-24)。
