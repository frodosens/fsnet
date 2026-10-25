# 脚本 API 与宿主合同

本文保留机制说明与显式 legacy 夹具；当前业务接入以 [API 指南](API_GUIDE.md) 和
[全局实体](GLOBAL_ENTITIES.md) 为准，兼容入口见 [legacy 适配器](LEGACY_ADAPTERS.md)。

业务逻辑推荐使用 [面向对象的实体 API](OBJECT_API.md)：类、继承、本地方法、显式 RPC、原生属性和实体引用。下文保留底层宿主合同与兼容接口，旧示例不等于推荐业务写法。

## 分层

`ScriptHost` 是不含脚本 VM 类型的 C++ 能力接口：显式路由注册/注销、连接、带 deadline 的异步 RPC。跨边界的数据是拥有所有权的二进制字符串、peer 句柄和 `Status`，不传递裸 C++ 指针、Lua 对象。

`ScriptEngine` 定义 load/poll/stop；当前嵌入式实现是 `LuaEngine`。`NodeScriptHost` 将这些能力连接到 Node。脚本与网络共享消息及错误合同，不跨边界传递 VM 对象。

Lua 实体 RPC 已提供 C++ 调度的单写者 mailbox；`runtime.entity` 进一步提供不透明状态、持久化、tick、带代数/宿主身份的句柄和卸载交接。

Lua 的 `instructions_per_resume` 同时计入 VM 指令和字符串模式匹配的原生工作量。固定版本 Lua 的 `find/match/gmatch/gsub` 在回溯、重复、字符类和扫描循环中检查共享预算与任务 deadline，超额会终止当前 RPC（overloaded/timeout）；`pcall` 不能把超额任务变成成功。普通字符串查找按保守的最坏工作量预扣预算，因此大输入/长模式可能提前拒绝。一次 resume 的多个模式调用共用预算，gmatch 迭代不会重置预算。任务返回后还会复查 deadline。输入复制到 Lua 后释放原生排队缓冲容量，挂起任务不会再持有这份重复输入。

这些约束用于可信业务脚本的故障隔离，不提供敌对代码的进程安全边界。其他 C 库操作（如字符串构造、排序）仍受内存与返回后的 deadline 检查约束，不能在任意 C 调用中强制抢占；敌对脚本需要独立进程隔离。

## Lua API

```lua
local peer
runtime.legacy_rpc('gateway.forward', function(payload)
    peer = peer or runtime.connect('127.0.0.1', 7002)
    return runtime.request(peer, 'backend.handle', payload, 1000)
end)
```

| API | 合同 |
| --- | --- |
| `runtime.legacy_rpc(route, function(payload) ... end)` | 仅在加载时注册；路由非空、无 NUL、最多 256 字节；重复/已有宿主路由导致加载失败 |
| `runtime.connect(address, port)` | 仅在请求协程中使用；返回异步连接句柄，真正连接失败通过 RPC 状态返回；不把句柄存在视为连接就绪 |
| `runtime.await(peer, route, bytes, timeout_ms=1000)` | 让出协程；恢复时返回 `bytes, status`；状态 0 表示成功，deadline 为 1–30000 ms |
| `runtime.request(...)` | await 的顺序式包装；成功返回字符串，非零状态抛 Lua 错误；业务可用 pcall 处理 |

`runtime.status` 提供 ok/not_found/overloaded/timeout/disconnected/rejected/internal（0–6）常量。handler 必须返回一个字符串；Lua 不自动编码表。脚本错误与错误返回类型得到 `Status::internal`，不会把异常文本发送给客户端。`last_error()` 可供宿主记录诊断。超时不是撤销已经发生的远端写入；写入仍需要幂等与 fencing 语义。

加载为单次操作：先编译并在 VM 内收集路由，整个 chunk 成功且宿主无冲突后才发布。语法错误、加载异常、加载死循环不会留下半套路由。停止后不能重新 load；未来热更新应创建新引擎并按明确的状态迁移协议切换。

`fsnet-script script.lua [port]` 是通用启动器，加载脚本、监听 loopback、推进 Node、实体宿主和引擎；没有玩家或场景业务实现。`FSNET_DATABASE_PATH` 选择 SQLite 持久化，未设置时使用内存。`tests/fixtures/lua/stateful.lua` 展示托管状态；`player.lua` 是旧的内存示例。

## 所有权、调度与限制

- Node 必须比 NodeScriptHost 与脚本引擎活得更久；引擎停止不停止 Node，不注销其他模块的路由。`system.health` 保留给核心。
- 单个引擎由一个线程拥有。网络回调只将请求排入队列；`engine.poll()` 创建/恢复协程。即使宿主立即完成一次 RPC，也先排队恢复，避免 VM 重入。
- 协程以 registry reference 保根，等待期间不会依赖临时 C++ 栈或包缓存。停止时先注销自身路由，再将排队/活动请求以 disconnected 结束，释放 VM；晚到或重复的完成回调不再恢复任务。
- 默认 Lua 与受计费原生资源合计预算 32 MiB、源码最多 1 MiB、256 个注册路由、128 个排队请求、128 个活动协程。入站排队与完成排队分别最多 4 MiB；这些原生队列不计入上述 32 MiB。
- 每次 resume 默认 100,000 条 Lua 指令，按 1,000 条指令粒度检查；耗尽时 hook 让出协程，宿主结束任务，不能用 pcall 吞掉预算错误后继续死循环。整个请求含排队/等待最多 30 秒，可通过 ScriptLimits 调低；宿主必须持续 poll 才能推进超时。
- Lua 分配器拒绝超出预算的分配。OOM、预算超限和排队超限各有测试；一个失败任务不应损坏后续请求。
- `memory_bytes` 同时计入 schema 默认值/类型描述和原生属性、预测载体的保守费用；`memory_used()` 返回合计。注册失败会退还 schema 费用，停止后归零。原生额度不足时先回收不可达 Lua 对象再拒绝，不会在每次预测输入上强制完整 GC。
- watch 成功至 Lua 对象接管之间由任务持有订阅资源；超时、停止、完成队列拒绝或对象构造失败都会释放。对象成功接管后由 close/GC 负责，清理只执行一次。
- Lua 从校验后的源代码以 C++ 模式构建，使用上游的 C++ 异常展开路径，避免 longjmp 跨过宿主 C++ 对象。Lua 类型与头文件不暴露在公共适配器接口中。

默认开放 base/table/string/math/utf8，不开放 io/os/debug/package/coroutine，不提供 dofile/loadfile/load/collectgarbage。当前只加载文本 chunk；多文件模块资源加载尚未实现。

这些限制面向**可信游戏脚本的错误隔离**，不是运行敌对上传代码的安全沙箱。指令 hook 不能抢占长时间运行的原生库函数，原生内存/最终器也不能用 Lua 指令计数完整隔离。需要执行不可信脚本时必须使用独立进程与操作系统资源限制。

## 宿主合同验收

`fsnet-script-contract` 启动实际 Lua 服务进程，由 C ABI 客户端发送包含 NUL 和非 UTF-8
的消息，验证字节所有权、显式路由、脚本异常、等待后端 RPC 和超时恢复。C++ 后端只是
传输测试夹具。`fsnet-lua` 另测加载原子性、协程根、资源限制、停止、重复/晚到完成和真实 TCP 转发。

`FSNET_WITH_LUA` 控制嵌入式 VM/宿主；关闭时仍可构建核心、echo/relay 工具和核心测试。

## Lua 实体串行执行

```lua
runtime.entity_rpc('player', 'update', function(id, payload)
    -- 在这里读取、修改脚本中的实体状态。
    local result = runtime.request(backend, 'storage.write', payload, 1000)
    return result
end)
-- 在另一个 RPC 协程中调用：
-- runtime.entity_request(peer, 'player', '42', 'update', payload, 1000)
```

同一 LuaEngine 中，实体类型 + 二进制 ID 唯一标识 mailbox。同一实体的所有方法按接收 FIFO 顺序执行；等待外部 RPC 时仍占有 mailbox，后续请求不能交错修改状态。其他实体可继续执行。普通 `runtime.legacy_rpc` 是无状态服务接口，不提供实体隔离；实体状态只能由对应的实体处理器修改，不能从其他实体或普通 RPC 绕过访问。

成功、异常、执行预算超限、任务超时都会结束当前协程并释放 mailbox。超时后的迟到回包被忽略。排队时间计入任务期限，队列受宿主全局请求数和字节预算限制。停止宿主会以 disconnected 结束活动和排队请求。调用方超时不等于服务端撤销，也不会回滚已发生的脚本状态修改或外部写入；持久化操作仍需要幂等性和 fencing。

严格串行意味着持有 mailbox 时不能同步等待同一实体的新请求；实体之间的循环等待也应避免，否则只能靠超时终止。这里没有可重入执行或 await 时自动解锁。

网络路由为 `类型.方法`。实体类型/方法各为 1–127 字节且不含点或 NUL；ID 为 1–256 字节。消息格式为 `FSE1` + 大端 uint16 ID 长度 + ID + 原始 payload。原生调用方可用 `fsnet::entity_message(id, payload)` 构造封包；业务 payload 格式由脚本决定。

保证范围是单个 LuaEngine 内的实体执行，不是跨节点分布式锁。实体需要路由到唯一 owner；跨节点接管仍依赖 EntityStore 的 fencing 和后续迁移编排。

## 托管状态、快照与接管

```lua
runtime.legacy_entity('counter', {
    initial = '0',
    methods = {
        add = function(state, id, payload)
            local next_state = tostring(tonumber(state) + tonumber(payload))
            return next_state, next_state
        end,
        get = function(state) return state, state end,
    },
    -- tick = function(state, id, interval_ms) return state, '' end,
})
```

`EntityScriptHost` 装饰通用 ScriptHost，通过 AsyncDatabase 执行存储。每次操作按以下步骤执行：读取持久化租约与快照；使用单记录 CAS 获取/续租，并保留快照对应版本；将状态字节交给 Lua；检查租约期限，并用同一个实体记录的版本 CAS 同时校验所有权并保存快照；提交成功才发送业务响应。脚本失败不提交返回状态，下一次操作从已提交快照恢复。状态可表示单个实体，也可表示脚本定义的场景聚合；C++ 不解释状态结构。

处理器必须返回两个字符串：`next_state, response`，可以等待后端 RPC。状态不可通过全局表共享或绕过对应 mailbox 修改。同步外部 RPC 写入不在本地状态事务内，框架不会回滚已经发出的外部副作用；需要提交后补投的实体命令使用 `ref:send`，详见 ENTITY_DELIVERY.md。不能混用同一类型的托管方法与非托管 entity_rpc。

可选 tick 由宿主内部排入同一 mailbox，不能从网络调用 `$tick`。间隔默认 100 ms，每次交付一个固定步长；忙碌时合并缺失 tick，不做无限追赶。tick 的状态同样持久化。高频场景应按需要调大间隔，或将短暂状态与可靠检查点分开，不能将逐 tick 同步持久化的性能推断为完整战斗服务器容量。

默认最多 4096 个本地实体、1024 个待完成操作、16 MiB 请求预算，每实体状态最多 1 MiB；存储队列另有独立预算。空闲实体默认 5 分钟后卸载本地句柄，持久化状态保留。C++ `deactivate(handle, reply)` 只允许空闲实体卸载，用 fenced CAS 释放租约，随后另一宿主可立即恢复；旧句柄的代数或宿主身份不匹配会被拒绝。停机不提前释放仍执行中的租约，避免已接受的写入与接管交错。

每次进程启动的 owner_id 必须全局唯一。默认租约 60 秒，请求期限 30 秒，租约必须大于请求期限。运行中的 handler 不会被宿主强制解锁；LuaEngine 自己的任务预算会结束悬挂协程，然后宿主才可推进同一实体。多节点需要共享满足单记录 CAS 的 Storage；文件后端可用于本机多进程，多份独立本地文件不构成跨机一致存储。本地跨进程测试使用同一 SQLite 文件验证进程退出后的恢复。自动过期接管依赖节点时钟同步，单记录 CAS 与 fencing 负责最终写入排他，不能把租约当成外部副作用的分布式锁。

## 所有权路由与重试

`EntityRouter` 使用共享 Database 中的租约查找当前 owner。节点配置包含稳定 node ID、地址、端口与实体类型，owner incarnation 为 `node-id/unique-boot-id`。租约有效时只向 owner 发送；过期或未创建时，以稳定节点 ID 做 rendezvous 选择，故障节点短暂退避。失败不会提前授予所有权，最终仍由 EntityScriptHost 的 CAS 决定。示例宿主支持 `FSNET_NODE_ID`，每次启动自动追加随机 incarnation。

脚本宿主可组合 `RoutedScriptHost`，然后使用 `runtime.entity_call(type, id, method, payload, timeout_ms, request_id)`，无需持有节点连接句柄；`runtime.entity_request(peer, ...)` 仍用于显式目标。两个接口最后一个参数均可选。路由器必须与 Node、实体宿主和 LuaEngine 一起 poll；其生命周期短于 Node。

带 request_id 的托管调用会把请求 ID、方法、输入与响应一起写入实体快照，和状态在同一次单记录 CAS 中提交。重复请求返回已提交响应，不再执行脚本；同 ID 不同方法/输入被拒绝。路由器只对带 ID 的请求自动重试断连、超时、拒绝与过载，所有尝试共用总 deadline；无 ID 只尝试一次。推荐 ID 使用随机 UUID 或上游持久化操作 ID，并在整个重试过程中复用。

去重是有界合同：默认保留每实体最近 64 个带 ID 的结果，总计最多 4 MiB，先到先淘汰。超出保留窗口后重试可能再次执行；长期可靠的实体命令可使用 `ref:send` 的持久 outbox/inbox；跨外部系统的金融/交易仍需持久化业务流水和幂等协议。去重只保证已提交的实体状态转换，不保证提交之前发出的外部 RPC 副作用 exactly-once。入站、存储与驻留状态分别有预算，默认驻留快照/结果总预算 32 MiB。

快照存储采用 `FSP1`（状态 + 普通回执）或 `FSP2`（另含持久命令元数据）。快照与 owner/租约一起包装在 FER1/FER2/FER3 单记录中；此前双 key 实验格式需要停写后离线迁移，不自动初始化或覆盖旧数据。

服务端 `Class:legacy_spawn_or_restore`、连接可见性和客户端自动表现工厂见
[ENTITY_PRESENCE.md](ENTITY_PRESENCE.md)。服务端生命周期见 [ENTITY_LIFECYCLE.md](ENTITY_LIFECYCLE.md)，模块热更新尚未实现。

服务端托管实体可用 `ref:send(method,payload)` 提交持久命令，随后在源实体事务中以
`runtime.delivery_status(ticket)` 查询并以 `runtime.delivery_forget(ticket)` 清理完成结果。
它与源状态共同提交，重启/迁移后补投，接收方持久水位去重；同步 call 的语义不变。
安装、升级、容量与当前验证进度见 [ENTITY_DELIVERY.md](ENTITY_DELIVERY.md)。
