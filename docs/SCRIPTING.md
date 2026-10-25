# 脚本宿主与 Lua 运行时

C++ 承担网络、调度、存储、权限与实体承载，玩法写在 Lua。
业务入口为 [实体类](OBJECT_API.md) 和全局 `EntityId`；每个 RPC 必须显式声明调用角色。
服务启动任务也通过 `Class:spawn` 创建 Hub/Manager 实体，不注册独立 login 路由。
完整接入见 [API 指南](API_GUIDE.md) 与 [示例](../examples/README.md)。

## 宿主与执行

`ScriptHost` 是语言无关边界：负载为拥有生命周期的二进制字符串，VM 指针和脚本对象不能跨边界。
`NodeScriptHost` 适配网络，`EntityScriptHost` 提供 mailbox、schema、租约/fence 与持久提交，
`RoutedScriptHost` 提供全局创建/调用路由；客户端使用 `ClientScriptHost`。
这些组件共用公共属性模型、patch 编解码和连接引用。Node 必须比所有适配器存活更久。

引擎由单线程拥有；回调排队后由 `poll()` 恢复协程，立即完成的宿主调用也不会重入 VM。
同一实体 mailbox 按 FIFO 串行执行，等待外部 RPC 时仍占用 mailbox；其他实体可以继续执行。
不要让实体等待自己的请求或同步生命周期操作，跨实体依赖环仍需业务避免。

类、方法与 schema 在加载期收集，成功后发布。非法声明或加载失败不留下半个注册批次。
`runtime.rpc` 和 `runtime.entity` 不提供业务注册入口；底层 `_` 函数属于实体包装器内部实现。
当前一次加载一个脚本块，无模块资源管理、在线热更新和状态迁移协议。

## 异步 API

| API | 合同 |
| --- | --- |
| `runtime.start(function() ... end)` | 加载期声明有预算的后台任务 |
| `runtime.sleep(ms)` | 挂起协程，1–30000ms；不是持久计时器 |
| `fsnet.ref(id):call(method, bytes, options)` | 全局实体 RPC，失败抛错；负载与返回值为 bytes |
| `Class:spawn(values, options)` | 全局创建，creation_key 重试保持身份 |
| `runtime.connect(address, port)` | 底层异步连接句柄，不代表连接已就绪 |
| `runtime.await(peer, route, bytes, timeout_ms)` | 底层协议控制调用，返回 bytes/status |
| `runtime.request(...)` | await 的抛错包装；业务使用实体引用 |

`runtime.status` 含 ok/not_found/overloaded/timeout/disconnected/rejected/internal。
超时不撤销已经提交的远端请求；可靠命令使用 [持久投递](ENTITY_DELIVERY.md)。
闭包不做序列化，重启后继续的延迟/周期工作使用 [持久计时器](ENTITY_TIMERS.md)。

## 资源与取消

`ScriptLimits` 限制源码、VM/原生对象内存、队列请求/字节、活动协程、路由数、每次恢复指令和任务期限。
不提供 os/io/debug/package/require。指令预算耗尽不能通过 pcall 继续运行；内存耗尽与脚本错误
使本次请求失败并回滚候选状态。结束协程时以专用 teardown hook 阻止用户 close 代码执行，再 resetthread 释放失败栈上的对象，后续健康请求可继续。
正常函数完成的 Lua close 按语言语义执行；取消/超时/预算终止不保证脚本清理回调执行。
这是受信任游戏脚本的故障约束，不是敌对代码的进程安全隔离。

实例 self 仅在当前请求有效，跨请求保存 EntityId 或逻辑引用。取消、超时或结束后旧 self 不可读写。
客户端确认副本由订阅管理；停止和 GC 释放订阅，晚到结果也会清理。
停止顺序为入口停止接单、持续 poll 并 drain、停止服务/VM、存储/Node；析构不隐式执行异步 hook。
预测和 batch 的 reducer 禁止等待、联网、打印及环境随机副作用，见 [预测](PREDICTION.md)。

启动器 `fsnet-script` / `fsnet-client` 和多节点 `fsnet-service` 已接好上述组件。
当前验证对应 `fsnet-current-runtime`、`fsnet-current-lua-limits`、`fsnet-current-prediction`。
