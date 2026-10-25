# 全局实体重构残留与复制成本审查

审查基线：`66d340b`，分支 `refactor/global-entities`，2026-10-25。
本次记录发现和诊断证据；建议尚未改入引擎。范围包括公共 API、Lua 桥接、Gateway、
实体路由/宿主、属性权限与同步、持久计时器及示例/部署文档，不是全仓安全或生产容量验收。

结论：全局 EntityId、连接授权和持久计时器主链路已经接通，但旧业务寻址仍是正式 API，
尚未收口到单一实体模型。可避免的复制存在，优先级高于机械替换参数的是新旧入口的策略一致性、
计时器发现容量，以及跨层重复封包。发现 3 项 P1、4 项 P2；下面均说明可触发条件。

## 应先修复的问题

### R1 · P1：外节点任务占用跟踪容量，本节点已提交任务可持续饥饿

位置：[entity_timers.cpp:39](../src/entity_timers.cpp#L39)、
[entity_timers.cpp:94](../src/entity_timers.cpp#L94)。

每个 owner 扫描同一份全局索引，先把条目放进 `watches` 并消耗 `limits.tracked`，
到 `scan` 读取实体记录后才判断 placement/租约归属。外节点条目仍留在 watches 中。
容量满后，后扫描到的本地条目只增加 capacity_failures，没有公平替换或本地预留。
任务注册端也不知道该发现队列已饱和，因此会出现“实体与任务已提交，但本机始终不调度”。
未知 schema 的条目也会先占容量，之后仅延后调度。

诊断在两个不同分片登记一个 foreign 和一个 local 到期任务；两者使用同一已注册 schema。
foreign 先被扫描。`tracked=1` 时，三轮均观察到 local 读取 0 次、回调 0 次、容量拒绝 9 次；
对照 `tracked=2` 时，本地任务均成功回调并提交 1 次。不是靠改变回调代码制造的失败。
缩小容量用于验证算法边界；默认 65536 是所有被扫描实体的容量，不能理解为每个节点
可独立承载 65536 个本地计时器实体。它也小于索引的全局理论条目上限。

建议：发现索引携带受 fence/版本约束的 placement 提示，并为本地到期任务提供公平准入；
远端/未知类型任务不应永久占满本地执行缓存。不能直接丢弃远端索引，否则会破坏迁移、
接管和 pending 写前登记的恢复合同。补充“缓存满但本地任务仍取得执行机会”的回归。

### R2 · P1：新 `@` 入口绕过旧 Gateway 方法白名单合同

位置：[client_gateway.cpp:411](../src/client_gateway.cpp#L411)、
[client_gateway.cpp:421](../src/client_gateway.cpp#L421)、
[service.cpp:104](../examples/server/service.cpp#L104)。

Gateway 构造时无条件绑定 `@`，按实体 ID 转发，没有应用 `bind_entity(type, methods)`
的配置。后者只注册旧的 `Type.method` 路由。`routes.tsv` 仍声称“仅暴露列出的业务入口”，
[接入文档](CLIENT_GATEWAY.md) 也把 bind_entity 描述为转发白名单。

诊断只允许 `Probe.allowed_method`，未允许 `Probe.public_method`。三轮结果相同：
旧路由调用 public_method 返回 not_found；FSE3/`@` 调用同一方法返回 ok / reachable。
同一探针的 server-only 方法经 `@` 返回 rejected：**这里没有绕过实体 caller/owner 校验，
绕过的是部署者配置的额外暴露白名单**。

建议：采用统一的实体方法暴露策略，`@` 解析并取得权威类型后也经过相同策略。
若产品设计决定所有 client RPC 自动可达，应删除旧白名单承诺并显式迁移配置语义；
当前两种合同并存会让部署者误判暴露范围。补充未列方法、类型断言和 server-only 检查。

### R3 · P1：新 LoginEntity 示例同账号重连会失败

位置：[entities/server.lua:10](../examples/entities/server.lua#L10)、
[entity_catalog.cpp:25](../src/entity_catalog.cpp#L25)、
[entity_catalog.cpp:67](../src/entity_catalog.cpp#L67)。

示例按 `player:<subject>` 重用创建 key，却把 `ctx.connection` 放进每次 spawn 的初始参数。
同一 LoginEntity 上的重连具有相同创建作用域/key，但 ConnectionRef 已变化；
creation_digest 包含初始 owner_connection，因此第二次是不同创建操作，必然冲突。
改变显示名称也会改变初始 state，触发同一问题。跨不同 LoginEntity 的创建作用域不同，
也不会自动提供账号唯一角色映射；该业务策略需明确说明。

复现直接加载已提交的 entities/shared.lua 和 server.lua，只把一个 LoginEntity 放在本地，
跳过地图迁移以隔离变量。两个顺序建立的客户端连接都认证为 alice，传入相同 Alice：
第一次返回 Player ID `0000000000000002`，第二次返回 `RPC failed: 6`，即外层 internal。
底层创建冲突为 rejected，示例的 spawn 断言使 login 业务方法失败。

建议：业务保存账号/角色到稳定 PlayerId 的映射，创建时使用稳定初始参数；重连查回引用，
验证身份后显式 `bind_owner(ctx.connection)`，再 show/possess。
不要放宽 Catalog 的参数冲突校验来掩盖示例问题。

## 旧结构与复制成本

### R4 · P2：旧业务身份/寻址仍在默认入口中，没有完成 API 收口

以下是可执行代码和当前导航，不只是测试夹具：

| 残留 | 位置 | 影响 |
| --- | --- | --- |
| 自定义字符串 ID、按 subject 创建 | [Class:spawn_or_restore](../scripts/fsnet/entity.lua#L191)、scene/server.lua:8、attributes/server.lua:4 | 可绕过引擎全局 ID 分配，旧 ID 仍按类型形成身份 |
| owner_from_id / subject 归属 | [entity_host.cpp:127](../src/entity_host.cpp#L127)、[entity_schema.hpp:24](../include/fsnet/entity_schema.hpp#L24) | 账号与 ID 相等的旧授权仍是可选引擎合同 |
| `runtime.rpc('login')`、server_request | scene/attributes 示例、[bind_account](../src/client_gateway.cpp#L425)、deploy/routes.tsv.example | 业务仍可走独立服务路由，与统一实体 RPC 并行 |
| FSE1/2 与 Type.method 对外入口 | [Client::call](../src/client.cpp#L165)、[bind_stateful_context](../src/entity_host.cpp#L951) | SDK/Host/Gateway 都维护两套寻址分支 |
| FER1/2/3 及按类型存储 key | [entity_record.hpp:62](../include/fsnet/entity_record.hpp#L62) | 原生夹具之外仍可写入旧命名空间与非全局记录 |
| subject-only Gateway 委托 | [client_gateway.cpp:48](../src/client_gateway.cpp#L48) | 有可信边界，但仍维护旧身份协议和回归负担 |
| 当前指南仍主推 owner='id' | [API_GUIDE.md](API_GUIDE.md)、[PRODUCTION.md](PRODUCTION.md) | 新使用者会继续接入旧路径 |
| 历史阶段代码块可被复制为当前 API | [GLOBAL_ENTITIES.md:66](GLOBAL_ENTITIES.md#L66) 的 owner_account_id | 页首虽注明历史，代码块本身已经不能运行 |

还有 account()/Slot.account/owner_subject 等旧命名：对全局实体这些字段实际保存连接 key，
不是账号。这属于命名债务，不代表全局元数据又保存了账号。FER5 的 owner_connection 合同已生效，
错误类型/缺少元数据的全局记录也会被宿主拒绝；未据旧接口存在推断出权限绕过。

建议先迁移交付示例、部署配置和指南；把允许任意 ID 的原生夹具接入单独适配层，再删除
业务 SDK/Lua 默认面的旧入口。保留 system.health、属性控制等引擎控制协议并不违背“业务统一实体化”。
无需为了删除历史 API，把公共 codec/差量算法重新实现两份。

### R5 · P2：全局 RPC 在跨层转接时恢复成旧封包，重复复制 payload

位置：[entity_router.cpp:184](../src/entity_router.cpp#L184)、
[entity_router.cpp:130](../src/entity_router.cpp#L130)、
[entity_host.cpp:188](../src/entity_host.cpp#L188)、
[entity_host.cpp:334](../src/entity_host.cpp#L334)。

RoutedScriptHost 已持有 FSE3 string，却从借用解析结果构造独立 method/payload/request/type
字符串，再交给 Router 编回 FSE3。对较大 payload，临时 payload 副本可以避免。
Owner 的全局 dispatcher 又把 FSE3 转成 Type.method + FSE1/2，enqueue 随后执行 owning parse。
这使旧结构残留直接成为新路径的复制成本，而非只有 API 数量增加。

局部机制探针比较当前转接构造与“借用字段、仍重新编码一次”的候选。三轮中位数：

| payload | 当前分配/操作 | 候选分配/操作 | 当前时间 | 候选时间 |
| --- | ---: | ---: | ---: | ---: |
| 128 B | 2 | 1 | 60.09 ns | 51.59 ns |
| 4 KiB | 2 | 1 | 125.14 ns | 74.20 ns |
| 64 KiB | 2 | 1 | 3.116 µs | 1.505 µs |

64 KiB 的分配请求字节合计为 131094 → 65557。这里统计 new 请求字节，不是 RSS/实际堆占用。
测试没有运行完整 Router/存储/TLS/Lua 调度，不把这约 1.61 µs 的局部差异当成请求总延迟收益。
小包耗时容易受调度和编译器影响，稳定结论是一份 payload 临时对象及其分配可以去掉。

建议 Router 接收并移动持有原始已校验封包，或者提供只在同步编码阶段借用的入口；
宿主队列使用结构化 Invocation，统一新旧适配器后去掉 FSE3 → FSE1 → owning parse 的中转。
队列仍需持有输入，不得把指向调用栈/Lua 字符串的 view 留到异步回调。

### R6 · P2：权限比较反复格式化连接 key，且同一次决策重复复查身份

位置：[ConnectionRef::key](../include/fsnet/connection.hpp#L33)、
[CallIdentity::owns / RpcPolicy::allows](../include/fsnet/entity_schema.hpp#L24)、
[EntityAttributes::visible](../src/entity_schema.cpp#L138)、
[Session::principal](../src/node.cpp#L147)。

`owns(owner)` 每次拼接 connection/<32hex>/<generation> 再比较；当前 libstdc++ 探针每次
2 次分配、131 字节分配请求。使用相同 canonical 校验与代数比较的本地借用候选为 0 次分配。
三轮局部中位数为 88.63 → 47.25 ns；候选没有实现完整身份/传输路径，只证明格式化可避免。
AttributeView 的构造还会复制 subject 和 ConnectionRef.id，visible 又按属性重复构造 key。

单次 owner-only `RpcPolicy::allows` 调用 `live()` 两次；stage 的 client 写入也逐字段复查。
真实 live 回调会重新运行身份解析器，并可能构造/复制 subject、连接 ID 和函数载体，
所以它不只是一个布尔读取。

建议增加共用的 `ConnectionRef::matches_key(string_view)` 或使用明确的二进制归属值；
同步属性投影使用借用视图、一次计算 owned。仅合并同一决策内无回调间隔的重复检查；
排队、挂起恢复、提交前等授权边界必须保留复查。owner_subject 返回拥有字符串也有跨重入
存活的理由，不应直接改成可能随 slot 卸载失效的引用。

### R7 · P2：计时器发现与持久化有显著的扫描、整片 CAS 和全状态复制成本

位置：[entity_timers.cpp:119](../src/entity_timers.cpp#L119)、
[entity_timer_index.cpp:40](../src/entity_timer_index.cpp#L40)、
[entity_host.cpp:450](../src/entity_host.cpp#L450)、
[entity_host.cpp:684](../src/entity_host.cpp#L684)。

没有任何任务时，每个 owner 仍周期读取全部 64 片索引。三轮各 1 秒的空载观察都得到
576 次索引 get；实际频率受刷新和扫完整轮的耗时影响，不是严格固定 640 次/秒。
共享远端数据库时，成本随 owner 数放大；不宜把已有本地文件测试当成多节点容量证据。

每次 timer_dirty 提交先 arm：读取实体记录、读取所在分片、解码/编码整片并 CAS，
随后再 CAS 实体。不同实体若落同片也竞争；确认/清理还需索引 CAS。一个分片最多 4096 项、
1 MiB；这是索引成本，不是给客户端下发整份属性，属性网络仍走公共 delta。

宿主还有 `job->timers=current.snapshot.timers` 的深复制、组装 committed 时再次复制 timers，
以及多处 `encode_entity_timers(...).size()` 为计费完整编码。单次注册/取消为强异常保证
先复制整个 timer state。这些成本随任务数和 payload 增长，通常比复制一个短方法名更值得关注。
持久实体依旧全记录 CAS；字段网络 patch 不代表存储也是字段写入。

建议先计数/暴露索引读写和冲突，再做按 placement 分区的公平发现、空载退避、可校验的刷新提示；
保留周期性持久发现兜底。热路径可先移动已不再使用的 decoded timers，缓存准确编码大小，
Lua 的查询返回仅需元数据时避免复制 payload。索引结构优化必须保留 pending → 实体提交的
写前顺序、源版本判断和 fence，不能删除 CAS/fsync 或按超时清除 pending 来省成本。

## 哪些 std::string 参数应保留

| 边界 | 当前判断 |
| --- | --- |
| Node::call / notify、Client::request 的 route/payload | 已是 string_view；同步组帧后由输出队列拥有帧，无需再机械改签名 |
| EntityRouter::Call、Host Request/Job、Lua Await、存储工作闭包 | 跨 poll/线程/重试，必须持有数据；值参接收后 move 是合理做法 |
| bind 的 route、连接配置、schema 元数据 | 注册后长期保存，拥有字符串是正确的；移动初始化通常不复制字符 |
| 同步解析/长度计算/查询/相等比较 | 优先 string_view/const 引用，避免仅为查找/比较构造临时字符串 |
| Node 接收的 payload | 当前接收 buffer 会释放，handler 可能挂起，拥有数据有必要；要进一步省复制需共享 frame 生命周期 |

`const std::string&` 本身不复制，`std::string` 值参也不等于必然深复制：右值通常移动。
反过来，即使形参是引用，调用方先 `std::string(view)` 仍然会分配。应沿调用链检查
分配点、编码点和生命周期，而不是按签名中的 string 个数判断效率。

## 证据、复现与建议顺序

原始三轮数据与两个连接的登录观察保存在
[global-structure-copy-review-2026-10-25.json](validation/global-structure-copy-review-2026-10-25.json)。
原生探针直接链接 Release libfsnet，计数普通 new/new[] 请求；不统计 malloc/对齐分配和 Lua VM 分配。
计时器探针使用真实宿主和调度器、最小原生回调/内存存储，实体时钟固定，索引刷新用 steady_clock。
登录探针使用真实 Lua 宿主和客户端。均为本机诊断，不是修复后的回归断言。
另复查现有 Debug 的 connections、global-entities、timers、global-flow 和 example-timers，
5/5 通过；这些用例尚未覆盖上面新复现的饱和准入、白名单一致性和示例重连。
诊断 C++ 通过 `-Wall -Wextra -Wpedantic -Werror` 语法检查，Python/JSON/本地文档链接检查通过。

```sh
. tools/dev/activate.sh
cmake --preset release
cmake --build --preset release
python3 tools/review/global_entities/run.py --build .dev/build/release --runs 3 --output /tmp/global-review.json
```

采样时没有并行编译或运行其他测试，数据记录源提交和共享库 SHA256；比较候选只在探针内，
尚未应用到产品。结果可随机器/标准库/编译器变化，不给出整个框架吞吐提升比例。

建议实施顺序：先 R1 的调度活性、R2 的入口策略和 R3 的登录示例；随后迁移 R4 的
默认入口/文档，消除 R5 的重复转接和 R6 的格式化分配；最后以目标存储部署测量 R7 的
发现/提交放大。必要的持有、授权复查、回滚和持久性合同在优化中继续保留。
