# 脚本宿主合同（阶段 1）

## 分层

`ScriptHost` 是不含 Lua/Ruby 类型的 C++ 能力接口：显式路由注册/注销、连接、带 deadline 的异步 RPC。跨边界的数据是拥有所有权的二进制字符串、peer 句柄和 `Status`，不传递裸 C++ 指针、Ruby VALUE 或 Lua 对象。

`ScriptEngine` 定义 load/poll/stop；当前嵌入式实现是 `LuaEngine`。`NodeScriptHost` 将这些能力连接到 Node。Ruby 继续作为独立进程中的 Fiddle/C ABI 适配器，不是嵌入 C++ 的第二个 VM。两者共享网络/错误/消息合同，不强行共享语言内部对象、GC 或同一份业务代码。

当前业务载体是 RPC 请求协程，不是完整的场景/实体 actor。单写者 mailbox、场景 tick、句柄代数与迁移钩子属于下一阶段。

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

`fsnet-script script.lua [port]` 是通用启动器，仅从文件加载脚本、监听 loopback、推进 Node 和引擎；没有玩家或场景业务实现。示例 `examples/lua/player.lua` 的业务状态仅在内存中，未接入持久化。

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
