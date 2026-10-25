# 当前 API 使用指南

当前合同：全局 EntityId、逻辑连接授权、公共属性差量与持久计时器。
可直接运行的程序见 [examples](../examples/README.md)。业务入口统一使用全局实体，不提供旧接口适配层。

## 类、属性与 RPC

```lua
local Item=fsnet.struct('Item',{id='string',count='int32'})
Player=fsnet.Entity:extend('Player')
Player:attr('name',{type='string',default='',sync='observers'})
Player:attr('position',{type='vec3',authority='driver',sync='observers'})
Player:attr('items',{type=fsnet.map(Item,128),sync='owner'})
Player:rpc('move',{callers={'client'},access='owner'})
function Player:move(input)
    local delta=assert(tonumber(input))
    if math.abs(delta)>8 then self:reject() end
    self.position.x=self.position.x+delta
    return tostring(self.position.x)
end
Player:define()
```

共享定义供两端加载；服务端 `Player:register()`，客户端注册自己的表现组件。
RPC 可声明 callers 为 client、server 或两者，access='owner' 额外校验当前逻辑连接。
只有后端可以创建实体、授予/撤销 owner 和驱动持久计时器；客户端 Node 不取得服务端角色。
结构、vec3、mat4x4、固定数组、list/map/set 及字段赋值都复用公共值模型与 patch 引擎。
复杂值可整值赋值或修改字段；batch 只暂存本地草稿，结束时提交，失败不发布半份状态。
客户端可写属性仍需 schema 授权和服务器校验，不能把预测副本当成已确认状态。

## 创建、重连和引用

在已注册的服务端实体 RPC/生命周期方法中：

```lua
-- ctx = self.context；账号验证与角色映射由业务负责。
local player=Player:spawn({},{creation_key='player:'..ctx.subject})
player:bind_owner(ctx.connection)
player:show_to(ctx.connection)
player:possess(ctx.connection)
return player.id
```

创建 key 的作用域默认是调用实体的 ID；启动任务另传 bootstrap_scope。
重复创建使用相同 key 和相同初始参数；初始 owner、属性、目标和持久模式都属于参数。
重连具有新的 ConnectionRef，应查回稳定 PlayerId 后 bind_owner，而非改变原创建操作。
上面的空初始参数示范固定创建描述；名字等可变信息通过 server-only 实体方法更新。
同一账号跨多个 LoginEntity 的角色唯一策略需业务注册表/账号实体协调。

客户端从可信接入配置得到 Manager/LoginHub 的 EntityId 后调用实体：

```lua
local login_id=fsnet.ref(manager_id):call('discover','')
local player_id=fsnet.ref(login_id):call('login','Alice')
fsnet.ref(player_id):call('move','2')
```

EntityId 是非零 u64，Lua/持久引用使用 16 位小写十六进制字符串；网络使用固定 8 字节。
ID 不包含类型或物理 Node。`fsnet.ref(id)` 无需客户端猜测类型；`Player:ref(id)` 附带类型断言。
未知或已销毁的 ID 不因 RPC 自动创建实体。服务端原生代码也可使用 `EntityRouter::call(EntityId,...)`。
RPC payload/response 为 bytes，不隐式使用 JSON。普通请求无跨重连自动重发；需要可靠实体间
命令时使用 [持久 outbox/inbox](ENTITY_DELIVERY.md)。

## 客户端表现与确认/预测

服务端只定义一个 Player 类型。客户端 `fsnet.entities:register(Player,{owned=LocalPlayer,observed=RemotePlayer})`
并 start；服务端 show 后自动产生副本，possess 决定当前控制对象。
LocalPlayer/RemotePlayer 只处理镜头、输入和渲染。owner 授权、可见性与控制分别管理。
账号不等于实体 ID，一个逻辑连接可以持有多个实体授权。
完整输入、预测、校正和退出流程见 [scene](../examples/scene/client.lua)。

## 持久计时器与生命周期

```lua
self:after(5000,'timeout','bytes',{key='timeout'})
self:every(1000,'tick','',{key='clock',count=10,missed='coalesce'})
self:cancel_timer('clock')
```

只保存方法名和 bytes，不保存闭包。任务属于 durable 实体，与状态同事务提交；
卸载、重启和迁移后恢复。完整 options、指定时间、替换/失败规则见 [实体计时器](ENTITY_TIMERS.md)。
生命周期为 on_create、on_activate、on_deactivate、on_destroy；激活可重复，创建只执行一次。
迁移保留 ID，客户端经 Gateway 恢复投影。计时器/回调的外部副作用仍需幂等。

## 运行

```sh
python3 examples/run.py scene
python3 examples/run.py scene --native
python3 examples/run.py attributes
python3 examples/run.py timers
```

启动器启动 SceneHub/ProfileHub，取得全局 ID 后注入客户端配置；所有业务请求走实体。
Gateway 的 `entity Type method1,method2` 配置对 `@` 使用权威类型检查同一份方法白名单。
更多合同见 [连接授权](CONNECTION_AUTHORITY_AND_TIMERS.md)、[客户端 SDK](CLIENT_SDK.md)、
[全局实体](GLOBAL_ENTITIES.md) 与 [部署](PRODUCTION.md)。
