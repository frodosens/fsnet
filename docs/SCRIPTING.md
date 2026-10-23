# 脚本宿主合同（阶段 1）

## 分层

`ScriptHost` 是不含 Lua/Ruby 类型的 C++ 能力接口：显式路由注册/注销、连接、带 deadline 的异步 RPC。跨边界的数据是拥有所有权的二进制字符串、peer 句柄和 `Status`，不传递裸 C++ 指针、Ruby VALUE 或 Lua 对象。

`ScriptEngine` 定义 load/poll/stop；当前嵌入式实现是 `LuaEngine`。`NodeScriptHost` 将这些能力连接到 Node。Ruby 继续作为独立进程中的 Fiddle/C ABI 适配器，不是嵌入 C++ 的第二个 VM。两者共享网络/错误/消息合同，不强行共享语言内部对象、GC 或同一份业务代码。

Lua 实体 RPC 已提供 C++ 调度的单写者 mailbox；`runtime.entity` 进一步提供不透明状态、持久化、tick、带代数/宿主身份的句柄和卸载交接。Ruby 当前仍使用请求 Fiber，尚未接入实体 mailbox。

## Lua API

```lua
local peer
runtime.rpc('gateway.forward', function(payload)
    peer = peer or runtime.connect('127.0.0.1', 7002)
    return runtime.request(peer, 'backend.handle', payload, 1000)
end)
```

| API | 合同 |
| --- | --- |
| `runtime.rpc(route, function(payload) ... end)` | 仅在加载时注册；路由非空、无 NUL、最多 256 字节；重复/已有宿主路由导致加载失败 |
| `runtime.connect(address, port)` | 仅在请求协程中使用；返回异步连接句柄，真正连接失败通过 RPC 状态返回；不把句柄存在视为连接就绪 |
| `runtime.await(peer, route, bytes, timeout_ms=1000)` | 让出协程；恢复时返回 `bytes, status`；状态 0 表示成功，deadline 为 1–30000 ms |
| `runtime.request(...)` | await 的顺序式包装；成功返回字符串，非零状态抛 Lua 错误；业务可用 pcall 处理 |

`runtime.status` 提供 ok/not_found/overloaded/timeout/disconnected/rejected/internal（0–6）常量。handler 必须返回一个字符串；Lua 不自动编码表。脚本错误与错误返回类型得到 `Status::internal`，不会把异常文本发送给客户端。`last_error()` 可供宿主记录诊断。超时不是撤销已经发生的远端写入；写入仍需要事务/幂等语义。

加载为单次操作：先编译并在 VM 内收集路由，整个 chunk 成功且宿主无冲突后才发布。语法错误、加载异常、加载死循环不会留下半套路由。停止后不能重新 load；未来热更新应创建新引擎并按明确的状态迁移协议切换。

`fsnet-script script.lua [port]` 是通用启动器，加载脚本、监听 loopback、推进 Node、实体宿主和引擎；没有玩家或场景业务实现。`FSNET_DATABASE_PATH` 选择 SQLite 持久化，未设置时使用内存。`examples/lua/stateful.lua` 展示托管状态；`player.lua` 是旧的内存示例。

## 所有权、调度与限制

- Node 必须比 NodeScriptHost 与脚本引擎活得更久；引擎停止不停止 Node，不注销其他模块的路由。`system.health` 保留给核心。
- 单个引擎由一个线程拥有。网络回调只将请求排入队列；`engine.poll()` 创建/恢复协程。即使宿主立即完成一次 RPC，也先排队恢复，避免 VM 重入。
- 协程以 registry reference 保根，等待期间不会依赖临时 C++ 栈或包缓存。停止时先注销自身路由，再将排队/活动请求以 disconnected 结束，释放 VM；晚到或重复的完成回调不再恢复任务。
- 默认 VM 分配预算 32 MiB、源码最多 1 MiB、256 个注册路由、128 个排队请求、128 个活动协程。入站排队与完成排队分别最多 4 MiB；这些原生队列不计入 VM 的 32 MiB。
- 每次 resume 默认 100,000 条 Lua 指令，按 1,000 条指令粒度检查；耗尽时 hook 让出协程，宿主结束任务，不能用 pcall 吞掉预算错误后继续死循环。整个请求含排队/等待最多 30 秒，可通过 ScriptLimits 调低；宿主必须持续 poll 才能推进超时。
- Lua 分配器拒绝超出预算的分配。OOM、预算超限和排队超限各有测试；一个失败任务不应损坏后续请求。
- Lua 从校验后的源代码以 C++ 模式构建，使用上游的 C++ 异常展开路径，避免 longjmp 跨过宿主 C++ 对象。Lua 类型与头文件不暴露在公共适配器接口中。

默认开放 base/table/string/math/utf8，不开放 io/os/debug/package/coroutine，不提供 dofile/loadfile/load/collectgarbage。当前只加载文本 chunk；多文件模块资源加载尚未实现。

这些限制面向**可信游戏脚本的错误隔离**，不是运行敌对上传代码的安全沙箱。指令 hook 不能抢占长时间运行的原生库函数，原生内存/最终器也不能用 Lua 指令计数完整隔离。需要执行不可信脚本时必须使用独立进程与操作系统资源限制。

## Ruby 接口与共同验收

Ruby `rpc_bytes` / `call_bytes` / Fiber 内 `request_bytes` 直接传递相同的二进制 payload；原来的 `rpc` / `call` / `request` 保留为 JSON 包装。JSON 响应解码失败会完成等待并返回错误，不会留下悬挂 Fiber。Ruby 的资源与执行限制不等同于 Lua 的 VM 预算。

`fsnet-script-contract` 启动实际 Lua、可选 Ruby 服务进程，由同一 C ABI 客户端发送包含 NUL 和非 UTF-8 的消息，验证：字节所有权、显式路由、脚本异常、等待后端 RPC、超时恢复。C++ 后端只作为传输测试夹具。`fsnet-lua` 另测加载原子性、协程根、资源限制、停止、重复/晚到完成和真实 TCP 转发。

构建开关：`FSNET_WITH_LUA` 控制嵌入式 VM/宿主；`FSNET_WITH_RUBY` 控制外部 Ruby 适配器发现与测试。两者关闭时核心、echo/relay 工具和核心测试仍然可构建；无脚本引擎时不运行游戏业务。

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

同一 LuaEngine 中，实体类型 + 二进制 ID 唯一标识 mailbox。同一实体的所有方法按接收 FIFO 顺序执行；等待外部 RPC 时仍占有 mailbox，后续请求不能交错修改状态。其他实体可继续执行。普通 `runtime.rpc` 是无状态服务接口，不提供实体隔离；实体状态只能由对应的实体处理器修改，不能从其他实体或普通 RPC 绕过访问。

成功、异常、执行预算超限、任务超时都会结束当前协程并释放 mailbox。超时后的迟到回包被忽略。排队时间计入任务期限，队列受宿主全局请求数和字节预算限制。停止宿主会以 disconnected 结束活动和排队请求。调用方超时不等于服务端撤销，也不会回滚已发生的脚本状态修改或外部写入；持久化操作仍需要幂等性和 fencing。

严格串行意味着持有 mailbox 时不能同步等待同一实体的新请求；实体之间的循环等待也应避免，否则只能靠超时终止。这里没有可重入执行或 await 时自动解锁。

网络路由为 `类型.方法`。实体类型/方法各为 1–127 字节且不含点或 NUL；ID 为 1–256 字节。消息格式为 `FSE1` + 大端 uint16 ID 长度 + ID + 原始 payload。原生调用方可用 `fsnet::entity_message(id, payload)` 构造封包；业务 payload 格式由脚本决定。

保证范围是单个 LuaEngine 内的实体执行，不是跨节点分布式锁。实体需要路由到唯一 owner；跨节点接管仍依赖 EntityStore 的 fencing 和后续迁移编排。Ruby 尚无此保证，不应作为实体逻辑执行入口。

## 托管状态、快照与接管

```lua
runtime.entity('counter', {
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

`EntityScriptHost` 装饰通用 ScriptHost，通过 AsyncDatabase 执行存储。每次操作按以下步骤执行：读取持久化租约与快照；CAS 获取/续租并验证快照版本；将状态字节交给 Lua；检查租约期限，并把 fencing 检查与新快照写入同一事务；提交成功才发送业务响应。脚本失败不提交返回状态，下一次操作从已提交快照恢复。状态可表示单个实体，也可表示脚本定义的场景聚合；C++ 不解释状态结构。

处理器必须返回两个字符串：`next_state, response`，可以等待后端 RPC。状态不可通过全局表共享或绕过对应 mailbox 修改。外部 RPC 写入不在本地状态事务内，仍需幂等/事务性 outbox；框架不会回滚已经发出的外部副作用。不能混用同一类型的托管方法与非托管 entity_rpc。

可选 tick 由宿主内部排入同一 mailbox，不能从网络调用 `$tick`。间隔默认 100 ms，每次交付一个固定步长；忙碌时合并缺失 tick，不做无限追赶。tick 的状态同样持久化。高频场景应按需要调大间隔，或将短暂状态与可靠检查点分开，不能将逐 tick 同步持久化的性能推断为完整战斗服务器容量。

默认最多 4096 个本地实体、1024 个待完成操作、16 MiB 请求预算，每实体状态最多 1 MiB；存储队列另有独立预算。空闲实体默认 5 分钟后卸载本地句柄，持久化状态保留。C++ `deactivate(handle, reply)` 只允许空闲实体卸载，用 fenced CAS 释放租约，随后另一宿主可立即恢复；旧句柄的代数或宿主身份不匹配会被拒绝。停机不提前释放仍执行中的租约，避免已接受的写入与接管交错。

每次进程启动的 owner_id 必须全局唯一。默认租约 60 秒，请求期限 30 秒，租约必须大于请求期限。运行中的 handler 不会被宿主强制解锁；LuaEngine 自己的任务预算会结束悬挂协程，然后宿主才可推进同一实体。多节点需要共享线性一致 Database；多份独立 SQLite 文件不构成分布式数据库。本地跨进程测试使用同一 SQLite 文件验证进程退出后的恢复。自动过期接管依赖节点时钟同步，fencing 事务负责最终写入排他，不能把租约当成外部副作用的分布式锁。
