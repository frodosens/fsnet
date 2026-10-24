# 共享实体与 Lua 客户端

同一份类定义可用于三种载体：

| 载体 | 创建方式 | 属性语义 |
| --- | --- | --- |
| 服务端请求门面 | `Class:register()` 后由请求激活 | server 权限，mailbox、fencing 与提交后同步 |
| 纯本地实例 | `Class:spawn_local(values, {id=...})` | 只有 `authority='driver'` 的属性可写，无持久化或联网 |
| 客户端确认副本 | `Class:watch(id, {timeout_ms=...})` | 自动读取原生同步值；有写权限时赋值自动提交提案，其他写入拒绝 |

```lua
local Circle = fsnet.Entity:extend('Circle', {owner='id'})
Circle:attr('radius', {type='number', default=0, authority='driver', sync='observers'})
function Circle:advance(dt) self.radius = self.radius + dt * 2 end
function Circle:contains(x, y) return x*x + y*y <= self.radius*self.radius end
Circle:define()
```

`:define()` 在加载时编译并封存共享模型，不注册网络入口；服务端再调用 `:register()`。
每个要实例化/订阅的类应在加载期间 define 或 register，不能在请求期间声明新类。
属性类型、默认值与权限在原生层验证；所有载体都是 userdata，不能 rawset 绕过 setter。

方法默认共享。可通过 `Class:method(name, {runs_on='server'|'client'|'shared'})` 限定
执行位置；本地实例和确认副本属于 client。执行位置与 RPC callers 独立，只有显式
`:rpc()` 声明产生网络入口。收到同步值不会自动重新调用玩法方法或产生玩法副作用。

## 客户端任务、同步与写入

```lua
runtime.start(function()
    local offline = Circle:spawn_local({radius=2})
    offline:advance(0.1)
    offline:close()

    local circle = Circle:watch('alice')
    while circle.radius < 10 do
        print(circle:contains(0, 0))
        runtime.sleep(16)
    end
    circle:close()
end)
```

`runtime.start` 只在加载时注册后台任务。任务使用同一协程、内存、队列和每次 resume
指令预算。`runtime.sleep(1..30000)` 挂起任务，不阻塞事件循环；普通实体请求 sleep
仍持有 mailbox。后台任务每次等待更新活动期限，不能通过 CPU 死循环绕过指令预算。
停止会取消任务，晚到 watch 结果自动解除订阅。

确认副本 getter/本地查询方法获取最新已确认属性；不需要业务 RPC。没有收到的字段
读取时拒绝，不会用默认值冒充服务器私有属性。断线、owner/观察权限变化和注销订阅后
访问会拒绝；`close` 在撤销后仍可调用，并可重复。GC/引擎停止也会释放订阅。

可写客户端属性的赋值会自动提交提案（需要异步任务）；需要控制重试参数时显式调用：

```lua
replica:write({display_name='Alice'}, {
    expected_revision=replica.revision,
    request_id='rename-123', timeout_ms=1000,
})
```

成功返回空响应，失败抛出错误，可用 pcall 处理。默认使用当前确认版本并由原生 SDK
生成请求 ID；不确定结果的重试要保留原版本、ID 和属性值。提案不预先修改确认副本，
沿用 owner、client ACL、validate_write 和提交后发布合同。

## 嵌入与示例

C++ 客户端用 `ClientScriptHost(node, server_peer)` 承载 LuaEngine，并持续推进 Node、
ClientScriptHost 和 LuaEngine；每个 Node 一个属性客户端。服务端用 EntityScriptHost
和 AttributeReplication。两个启动器 `fsnet-script` / `fsnet-client` 已接好这些组件。
客户端未指定 peer 的实体引用通过 ClientScriptHost 发送到其配置的服务器连接；
这仍是直连 owner，不包含 Gateway 身份委托或自动 owner 发现。

完整魔法阵类见 [circle_entity.lua](../tests/fixtures/lua/shared/circle_entity.lua)。
启动步骤见 [examples](../examples/README.md)。`FSNET_DEMO_SUBJECT=alice` 是 loopback
演示身份，非回环监听会拒绝启用。生产身份来自可信登录/证书映射，不能由 payload 指定。
当前直连 owner 节点；Gateway 转发、模块和完整生命周期将在后续阶段接入。

独立预测状态、有界输入历史、服务器校正与重放见 [客户端预测](PREDICTION.md)。

服务端登录后自动创建客户端表现、LocalPlayer/RemotePlayer 工厂与控制目标同步见
[实体存在性与表现](ENTITY_PRESENCE.md)。

结构、固定数组与 `self.position.x = ...` 的自动提交规则见
[结构属性](STRUCTURED_ATTRIBUTES.md)。
