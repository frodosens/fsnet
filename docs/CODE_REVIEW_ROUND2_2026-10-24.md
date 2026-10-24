# 第二轮架构与实现审查

基线：`9de32185f6c684b4e3dd4f1dae9efbc7889f8dc9`，2026-10-24。
本轮只审查、复现和保存结论，**没有修复下列实现问题**。第一轮已修复的问题不重复计入。

重点覆盖多进程共享存储、线程与进程停止、迁移和控制权交错、超时清理、Lua 原生资源预算、
真实属性接收路径。P1 建议在生产交付前优先修复；P2 是明确的功能、资源或容量缺口。
既有 CI 全绿说明现有断言通过，未覆盖下面这些交错。测试使用本机回环和隔离存储，
不能代替跨机器 HA、网络分区、时钟漂移和长稳验收。

## 优先修复的五项问题

### 1. P1：停止实体宿主时与存储线程发生数据竞争

位置：[stop](../src/entity_host.cpp#L899)、[complete](../src/entity_host.cpp#L127)、[获取租约结果](../src/entity_host.cpp#L374)。

`stop()` 在第 906 行先调用活动任务的 complete，直到第 917 行才调用 storage.stop 并 join
工作线程。complete 会读取 fence、metadata_only 等字段；存储线程仍可能写这些字段。
这是同一个非原子 Job 被两个线程使用，没有完成队列带来的同步保护。

复现：阻塞一次 Storage.get，在所属线程调用 host.stop，再让存储工作返回。
ThreadSanitizer 明确报告 `job->fence` 的工作线程写与 complete 的主线程读存在 data race。
测试没有跨线程调用 EntityScriptHost；额外线程只用于释放模拟存储等待。

影响：正常关闭/进程排空也可能进入 C++ 未定义行为。多进程 CAS/fencing 保证不能解决这个
进程内部的共享对象竞争；ASAN/UBSAN 也不能代替 TSAN 检查。

建议：存储操作输出独立不可变结果，仅在 completion 中交给所属线程；或者在取消逻辑读取
这些字段前先完成必要的 worker 同步。补 get/CAS 被阻塞期间 stop 的回归。

### 2. P1：卸载/销毁入队失败仍锁住实体，后续无法重试

位置：[显式卸载](../src/entity_host.cpp#L743)、[空闲卸载](../src/entity_host.cpp#L709)、[销毁](../src/entity_host.cpp#L887)。

这些路径在调用 enqueue 后无条件设置 `slot.releasing=true`。当全局请求/字节额度不足时，
enqueue 已同步返回 overloaded，实际没有控制任务入队。释放额度后，所有新操作仍被
releasing 拒绝，又没有任务可以清除标志。

确定性复现：requests=1，两个空闲实体，直接 drain 两次，无需挂起处理器或并发客户端：

```text
first_drain=2 retry_drain=5 entities=1 pending=0
```

显式卸载复现也得到 `unload=2, pending_after_release=0, read_after_release=5, unload_retry=5`。
2=overloaded，5=rejected。默认最多 4096 个驻留实体、1024 个请求，批量 drain 可以触发相同问题。
现代 Lua 类注册了卸载 hook，因此不限于旧原生适配器。

建议：由真正入队成功的控制任务取得 releasing 状态，所有拒绝/异常路径回滚；不要仅靠
同步回复后再检查计数。handoff 路径已有对应保护，可统一控制请求的入队机制。

### 3. P1：Gateway 超时请求退出计费，却仍保留整份输入

位置：[finish](../src/client_gateway.cpp#L108)、[共享查询等待者](../src/client_gateway.cpp#L141)、[捕获请求的回调](../src/client_gateway.cpp#L199)。

finish 从请求表移除并归还 queued_bytes，但 `data.clear()` 保留 string 容量。忙碌查询的
waiters 持有强引用回调，回调又持有 Request；超时不会取消等待者。后续请求重新使用已经
归还的预算，继续追加到同一个慢查询上。

复现配置：4 个请求、1MiB Gateway 队列、50ms 超时，阻塞一次查询；每批四个 240KiB 请求，
全部收到超时回复后再开始下一批。三个独立运行，30 批后 RSS 增量为 **29,680–29,744KiB**，
每批边界却都显示 `pending=0, queued_bytes=0`。最后解除阻塞并正常退出。

不需要永久挂起：有限的慢 I/O 已足以超出预算。单存储线程还可能排队多个慢操作，不能把
单次 SQL/文件锁超时当成整个 Gateway 查询的等待上限。这里没有绕过身份认证。

建议：超时应取消或解除对应 waiter；可使用弱请求引用、独立取消令牌和等待者预算。
仅换成 swap 释放 payload 仍会留下持续增长的回调队列。

### 4. P1：PostgreSQL 可能先完成明文认证，再拒绝不安全远端连接

位置：[连接选项预检查](../src/postgres_database.cpp#L53)、[建立连接](../src/postgres_database.cpp#L73)、[连接后 TLS 检查](../src/postgres_database.cpp#L34)。

预检查只看显式解析出的 host/hostaddr；若目标由 PGHOST 等 libpq 解析来源提供，
`postgres_database("sslmode=disable")` 仍会先执行 PQconnectdbParams。
最终 ready 虽然拒绝该远端连接，但认证已经发生。

使用 libpq 17.11、固定虚构口令和本机 `127.0.0.2` 模拟端点复现。该地址被当前适配器判为
需要验证 TLS 的远端。模拟端点观察到：

```text
startup_received_without_tls=1 password_message=1
dummy_password_received=1 read_write_probe=1
rejected=remote PostgreSQL requires verified TLS
```

这是连接配置与环境解析边界的问题，不表示网络客户端能修改服务进程环境。
默认强制 verify-full 的安全路径不受此特定配置触发影响。复现没有使用真实凭据或外部服务器。

建议：认证开始前就保证最终连接参数符合 TLS 政策；如果不能预先证明是允许的本地目标，
应强制 verify-full 或直接拒绝。连接建立后的 PQhost/PQsslInUse 检查只能作为补充校验。

### 5. P1：迁移期间切换控制目标，可能断开整个客户端或移除有效副本

位置：[解除旧控制及错误处理](../src/client_gateway.cpp#L268)、[恢复时捕获控制状态](../src/client_gateway.cpp#L297)、[确认校验](../src/client_gateway.cpp#L305)。

控制意图没有代次，与迁移恢复没有统一的串行关系。确认拿发送时的状态，与当前可变的
sub.controlled 比较，把合法的中途切换误判成协议错误。

两种复现：

- 真实 Lua/EntityHost 路径：Actor 从 A 向 B 迁移，在 B 的 activate 暂停 250ms 时合法 possess Pet。
  Pet 登录成功，但 Gateway 向旧 Actor owner 发出的 unpossess 失败后关闭前端连接。
  观测为 `connected_before_possess=1, pet_login_status=0, connected_after_possess=0`。
- 可控协议夹具：挂起 Actor 的 controlled=true 恢复确认；Pet 先恢复并获得控制，正常使
  Actor.controlled=false；随后 Actor 的原确认返回，健康的 Actor 副本被 rejected 移除。
  Debug/Release 均得到 `actor_before_confirmation=1, actor_after_confirmation=0, actor_leaves=1`。

上一轮新增最终恢复确认解决了中间通知提前完成，但没有解决恢复与控制意图变化的交错。

建议：给每个客户端的控制意图及恢复尝试加代次，过期确认只做必要清理/重新协调，不能把
业务允许的切换当成非法包。旧 owner 上已消失的控制目标应按幂等撤销处理，不应因此关闭
整个合法会话。补双实体、不同 owner、恢复尚未完成时切换控制目标的测试。

## 其他已确认的实现遗漏

### 6. P2：手动订阅迁移成功后没有清除旧路由缓存

位置：[手动迁移完成](../src/client_gateway.cpp#L313)。自动恢复的缓存失效位于第 256 行附近，
手动 watch 的完成分支缺少同一步骤。

真实 Host/Lua 的手动 watch 从 A 故障恢复到 B 后，立即 RPC 仍选中过期的 A。
Debug/Release 复现均为 `manual_migrations=1 followup_status=4`（disconnected）。
夹具使用有效的 500ms 租约、2s 缓存；本次不是恢复失败，而是完成后继续使用旧目录结果。
建议抽出公共迁移完成逻辑，同时更新订阅映射、快照、控制状态和路由缓存。

### 7. P2：不存在记录的只读查询永久生成锁文件

位置：[创建锁文件](../src/file_storage.cpp#L60)、[get](../src/file_storage.cpp#L131)、[Gateway watch 查询](../src/client_gateway.cpp#L205)。

get 也使用带 O_CREAT 的逐 key 锁。4096 个不同 key 的查询 miss 产生 **4096 个 .lock 文件、
256 个目录、0 个 record**。有认证的 Gateway watch 先按客户端提供的 type/id 查目录，之后
才判断端点和后端权限；连续 miss 因此可持续消耗历史 inode，当前并发额度不能限制它。
这与为了防 ABA 保留的真实记录 tombstone 是不同问题。

建议：对不存在记录的读取不创建逐 key 持久锁；例如利用原子 rename 后不可变记录的 fd
读取语义，或固定数量的分片锁。不能直接删除正在使用的稳定锁文件，否则会破坏跨进程互斥。

### 8. P2：Lua watch 成功排入恢复队列后，任务超时会泄漏订阅

位置：[watch 完成回调](../src/lua_engine.cpp#L1494)、[先到期任务再处理恢复队列](../src/lua_engine.cpp#L1635)。

晚到回调有 unwatch 保护，但“回调已成功排队、Lua 尚未创建 userdata、任务随后到期”没有
资源所有者。订阅 ID 已编码为普通字节；丢弃恢复事件时不 unwatch，finish/engine.stop 也不认得它。

10ms 任务先收到 handle=123，再停顿 20ms 轮询，得到 `active=0, unwatches=0`；engine.stop 后
仍为 0。真实 ClientScriptHost 的 AttributeClient 会继续保留并续租这个投影，直到 host 停止或断连。
建议让 pending watch 持有可取消的原生资源令牌，只有 Lua 对象成功接管后才转移所有权；
超时、停止、OOM 和完成队列拒绝都清理令牌。

### 9. P2：Lua schema 的原生默认值未纳入内存预算

位置：[保存 schema](../src/lua_engine.cpp#L353)。引擎持有 schema 及展开后的默认值，
却没有向 used/memory_bytes 计费。

配置 8MiB 内存、默认指令额度，定义 64 个类，每类一个合法的 15×256 int32 固定数组：
加载成功，memory_used 约 **229KB**，独立运行的 RSS 峰值从约 **5MiB 增至 21MiB**
（Lua 探针使用 ru_maxrss，启动方式会影响初始峰值）。
这是受信脚本的资源核算遗漏；不据此宣称脚本宿主提供敌对代码安全隔离。
建议将 schema/default/type 描述的原生拥有量统一计入预算，并限制定义数量，失败时原子回滚。

### 10. P2：预测克隆中的不可达原生对象无法及时释放预算

位置：[prediction_clone](../src/lua_engine.cpp#L846)。只有 batch=true 才在原生预算不足时
尝试 GC；普通预测不会处理 Lua GC 看不见的大量原生载体内存。

默认 32MiB/100k 指令、同样合法的 15×256 数组、无操作 reducer：前三次 push 成功，
第四次及随后 12 次均报 prediction memory budget，pending 停在 3。没有保留旧状态视图，
正常 live graph 只有确认与当前预测；memory_used 约 30.76MiB，RSS 峰值约 9.3MiB。
重复创建并 close 本地对象也能出现类似情况。现有字段访问和 batch 已有针对该问题的 GC 处理。

建议让外部原生分配推动 GC/释放，或在统一的有界资源预留入口回收不可达载体后再判额度。
避免在每个输入上无条件做 full GC，同时保持低延迟。

### 11. P2：原生 opaque 实体可以接受永远无法完成 ACK 的持久命令

位置：[send_entity](../src/entity_host.cpp#L775)、[ACK 入口](../src/entity_host.cpp#L852)、[schema 建立内部路由](../src/entity_host.cpp#L621)。

受支持的原生 bind_stateful 源实体可以在事务中成功 send 并提交 outbox/epoch。
目标已应用奖励和 inbox 后，源 ACK 固定走 Source.$snapshot，而该内部路由只由 define_schema 创建。
源因此永远 pending，不能正常查询完成并清理。源为 typed、目标 opaque 也有接收路由的同类假设。

复现：`source_issue=0 receive=0 target_result=reward applied ack=1 pending=1`，1=not_found。
现代 Lua send 显式要求 schema，正常现代 Lua 类不受此源端问题影响；缺口在原生宿主 API 的
接受合同与内部协议假设不一致。
建议元数据操作独立于属性 schema，或在接受命令前明确拒绝不支持的源/目标并补齐文档。

## 性能和容量问题

### 12. 原生客户端接收差量无条件深复制完整状态

位置：[接收候选](../src/attribute_replication.cpp#L392)、[触发深复制](../src/attribute_wire.cpp#L458)。

每次接收先复制 AttributeReplica，导致内部图像必定被共享，apply 再进入深 COW。
即使应用没有持有旧快照、也没有 Lua，这条正式接收路径也无法走唯一图像的快速路径。

Release 三次测量，1000 个标量更新，旁边有不变的 256×512 字节字符串数组：

| 路径 | 每次分配次数 | 每次累计分配字节 |
| --- | ---: | ---: |
| 同一模型直接 apply | 2 | 240 |
| 正式接收中的候选复制模式 | 262 | 146256 |
| 真实 Native AttributeClient 接收 | 266 | 146471 |
| 真实接收，没有大数组 | 9 | 807 |

标量属性更新编码仅 **37 字节**。原生接收耗时中位观测约 16.3µs/次，包含 Node 和解码，
不能用它与直接 apply 的约 1µs 宣称纯实现倍数提升。最强证据是稳定的分配次数与字节量。
此前直接 apply 的 benchmark 没有覆盖这个边界。

建议把原子应用和预算预检查合并到公共引擎路径，去掉人为制造的无条件共享读者；
保留失败不发布状态的保证，不能通过删掉预算/原子校验换性能。

### 13. 默认配置下，一个突发批次只允许两笔实体获取进入执行器

位置：[读取额度预留](../src/entity_host.cpp#L287)、[执行器默认预算](../include/fsnet/async_database.hpp#L8)。

每笔按 configured state+receipt+delivery 上限保守预留，默认合计约 7MiB；执行器只有 16MiB。
completed 作业也要等 poll 才归还额度。因此，一次空闲宿主收到 64 个独立、极小的实体请求：
先全部进入 mailbox，首次 poll 就有 62 个 overloaded，最终仅 2 个成功。无网络、Lua 或磁盘瓶颈。

这是资源策略/容量配置交互，不是 CAS 正确性失败，但“1024 个请求/4096 实体”的上限不能
当成可用突发容量。建议存储额度暂缺时保留已接纳 mailbox 任务并在后续 poll 尝试，仍受原
deadline 限制；或者明确配置关系，并按真实实体获取/提交路径压测，而非只测 Node echo。

## 补充接口边界

原生 Node 析构取消未完成请求时，若应用 completion 抛异常，会穿过 stop 进入 noexcept
析构并 terminate：[destructor](../src/node.cpp#L331)、[stop](../src/node.cpp#L448)、[finish](../src/node.cpp#L99)。
独立子进程用自定义 terminate handler 观测到 `completion_status=4, terminate_during_node_destructor=1`。
Client SDK 的 guarded 回调有异常隔离，此处限定原生 Node；应明确回调 no-throw 合同，
并保证析构清理不会被一个异常打断，其他取消回调仍最多完成一次。

## 设计缺口与不重复列为新缺陷的限制

- 脚本模块资源、版本迁移/热更新、加密 P2P 会话与后备链路仍在路线图，未作为实现完成。
- 跨机器 HA 仍需共享一致 CAS 后端、时钟条件和部署验收。文件后端并不自动复制。
  本轮未发现预期部署下新的 delete/recreate 版本复用缺陷；tombstone/SQLite revision/PG sequence 有对应保护。
- 持久投递仍按 owner 扫全局源索引；历史 channel/registry 暂无安全退休协议。
  上轮已经修复了每 32 项额外等待 100ms 的问题，不能继续引用旧的空索引 12.8 秒推算。
  黑洞目标占满所有在途槽时，健康目标仍可能有很长尾延迟；需要按目标隔离、ready 索引和分片调度。
- 网络字段 patch 不等于存储局部更新；完整记录读取、完整快照 CAS 和 Lua 投影重建仍有成本。
- Node 发队列按连接限额，默认 4MiB×1024 是约 4GiB 的配置容量；Gateway 另有客户端限制。
  这是有界的整体内存规划风险，与第 3 项超时后脱离计费的持续保留不同。
- 外部同步 I/O 不能由脚本 deadline 抢占。SQL statement_timeout 限制服务端执行，不限制所有
  网络黑洞等待；worker join 的最终关闭时间仍需要单独设计和验收。

## 复现与交付

独立探针和原始性能观测保存在 [tools/review/round2](../tools/review/round2/)。
它们报告观测，不作为“缺陷仍存在即通过”的默认 CTest；输出退出码 0 仅表示诊断正常完成。
本轮没有重建共享 preset 或修改引擎，仅编译独立可执行程序；TSAN 额外编译了原始 entity_host.cpp。
既有异步存储、文件合同与独立进程 CAS/SIGKILL 测试也通过。

```sh
. tools/dev/activate.sh
# 已构建 Lua ON 的 Release 库；多个 case 可分别选择
python3 tools/review/round2/run.py --build .dev/build/release --case lifecycle_admission
python3 tools/review/round2/run.py --build .dev/build/release --case actual_control_migration
python3 tools/review/round2/run.py --build .dev/build/release --case gateway_retention
python3 tools/review/round2/run.py --build .dev/build/debug --case stop_race --tsan
# 可选：安装与 libpq 匹配的公开开发头文件，只连接固定本机模拟端点
python3 tools/review/round2/run.py --case postgres_connect --postgres-include /path/to/libpq/include
```

计时/RSS/分配计数是本机诊断观测，不是部署容量承诺。TSAN 的非零退出与 Node 的退出码 73
是所记录问题的预期观测；必须把对应探针改成“正确行为”的正式断言后，才能作为修复回归。

建议修复顺序：停止同步与生命周期入队 → Gateway 超时回收/控制意图协调 → PG 连接前 TLS 政策 →
手动迁移缓存与 Lua 资源所有权 → 内存核算/回收 → 真实接收路径和执行器背压优化。
