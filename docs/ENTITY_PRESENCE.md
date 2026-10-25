# 服务端创建与客户端实体表现

服务端只有一个 `Player` 实体类型；客户端根据服务器确认的归属，把它包装为
`LocalPlayer` 或 `RemotePlayer`。表现类不改变服务端实体类型或权限。
C++ 提供实体、可见性和同步载体；登录、场景、AOI、镜头和输入策略留在 Lua/应用适配层。

## 登录与服务端创建

```lua
LoginEntity:rpc('login',{callers={'client'}})
function LoginEntity:login(name)
    local connection=self.context.connection
    local player=Player:spawn({},{creation_key='player:'..self.context.subject,target_node='map-02'})
    player:bind_owner(connection)
    player:show_to(connection)
    player:possess(connection)
    return player.id
end
```

启动时创建 Manager/LoginEntity，并从可信发现渠道提供入口 EntityId。客户端先调用
`fsnet.ref(login_id):call('login','Alice')`。业务决定账号与角色映射，引擎只负责全局 ID 和连接授权。
创建初始参数与 creation_key 必须稳定，重连后换绑当前 ConnectionRef；后续配置通过实体方法修改。
创建确认丢失可用相同 key 找回原实体，已销毁实体不会复活。

## 可见性与控制

返回引用提供以下服务端接口，均等待本次操作的结果：

| 接口 | 合同 |
| --- | --- |
| `player:show_to(connection)` | 为已提交且驻留的实体授予当前认证连接观察权限，并推送过滤后的初始快照；重复调用不重复创建表现 |
| `player:hide_from(connection)` | 撤销该连接的自动订阅，通知客户端移除；可重复，不删除服务端记录 |
| `player:possess(connection)` | 将此连接的控制目标切换到这个可见且归其所有的实体；旧目标先解除控制 |
| `player:unpossess(connection)` | 解除此实体的控制，保留可见性；可重复 |

connection 来自可信 `self.context.connection`，不得采用负载声明的 owner 或 socket ID。
可见性和控制与实体归属分别管理；possess 只允许当前连接拥有且可见的实体。
跨节点 Gateway 将逻辑连接委托给 owner，并转发属性与表现通知。

## 客户端工厂与生命周期

共享实体类在客户端只 define，不 register 服务端路由：

```lua
local LocalPlayer = {}
function LocalPlayer:on_spawn()
    self.model = models:create('player') -- 由客户端引擎提供的适配接口
end
function LocalPlayer:on_possess()
    self.prediction = self.entity:prediction()
    camera:follow(self.model)
    input:bind(self)
end
function LocalPlayer:on_unpossess()
    input:unbind(self)
    camera:unfollow(self.model)
    self.prediction:close()
end
function LocalPlayer:on_despawn()
    models:destroy(self.model)
end

local RemotePlayer = {}
function RemotePlayer:on_spawn()
    self.model = models:create('player')
    -- 给 model 安装读取 self.entity.x 的插值逻辑。
end
function RemotePlayer:on_despawn() models:destroy(self.model) end

fsnet.entities:register(Player, {owned=LocalPlayer, observed=RemotePlayer})
fsnet.entities:start()
runtime.start(function()
    fsnet.ref(login_id):call('login', 'Alice')
end)
```

models/camera/input 只是图形引擎适配接口示意，不是 fsnet 内置模块。完整的无图形依赖示例见
[共享类](../examples/scene/shared.lua)、[服务端场景](../examples/scene/server.lua)、
[客户端场景](../examples/scene/client.lua)。

工厂在脚本加载时注册并 start，由一个有预算的后台协程按接收顺序分发事件。prototype 是
普通 Lua 表，可以用普通 Lua 继承组织公共表现逻辑。框架填入：

- `entity`：原生只读确认副本；可创建独立预测器。
- `id`、`type`：服务端实体身份。
- `owned`：服务器确认的归属，选择哪个 prototype 的依据。
- `controlled`：当前是否被此连接控制。
- `subscription`：当前副本代数标识，仅供关联与诊断，不授予权限。

`on_spawn` 在初始快照就绪后执行；随后根据控制消息调用 `on_possess`。
控制切换先 `on_unpossess`，再对新目标 `on_possess`。
移除先解除控制，再 `on_despawn`，最后释放副本。移除回调不能继续读取已撤销的 entity；
销毁模型和镜头应使用表现对象此前保存的引用或 id。普通 hook 异常会停止分发并尽力清理
所有已托管的表现与订阅。指令/内存预算耗尽或进程退出不能保证任意业务清理代码执行，
图形引擎还需提供整体场景卸载机制。

`fsnet.entities:get(type,id)` 返回仍有效的表现对象，没有或已撤销则返回 nil；权限撤销
立即生效，生命周期清理由分发协程随后处理。移除与重新进入视野会创建新的表现实例。
在 Lua 处理前就已撤销的创建消息会被忽略，不构造失效实体。
`fsnet.entities:stop()` 停止分发并清理当前所有表现及尚未分发的自动订阅，
后到的自动创建将退订；手动 watch 和整个 Node 保持可用。退出登录应先让
服务端撤销可见性。停止后当前管理器不能重新 start，重新加载需新的引擎。


## 同步与资源边界

自动创建/控制/移除通过 `system.entities.presence` 的一向通知传输；初始快照含订阅 ID、
实体身份、归属、控制标志与租期。随后属性变化复用 `system.attributes.update`，同一 TCP
连接保证发送顺序。控制消息只更改元数据，不覆盖确认属性或倒退版本。来自非配置服务器
连接的通知会被忽略。协议/身份不一致、预算溢出或发送背压会关闭连接，要求重新登录。

自动订阅和手动 watch 共用默认服务器总量 1024、每连接 32、投影内存 16 MiB 限额。
客户端共用 256 个待完成/活动订阅和 16 MiB 投影预算；原生表现事件队列最多 512 条，
超限断开并报告错误。Lua 表、表现对象和预测状态继续使用 VM 内存和指令预算。
表现分发空闲时等待 16 ms，图形渲染/插值仍由引擎自身帧循环推进。

服务端创建/激活/卸载和删除 hook 另见 [生命周期](ENTITY_LIFECYCLE.md)。尚未包含场景持久化、自动几何 AOI、
断线会话的业务回收、热更新或输入自动重发。Gateway 属性转发与 owner 迁移已实现，见 CLIENT_GATEWAY.md。

## 验证

`fsnet-global-flow` 验证多 Node 创建、Gateway 权限/订阅、连接换绑、迁移与控制、持久命令和计时器。
`fsnet-client-sdk` 验证连接代数与失效写入；scene 的 Lua/原生示例运行完整登录和移动流程。
本机验证不等同于公网、自动 AOI 或图形引擎集成验收。
