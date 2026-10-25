# 全局实体与调用合同

当前实现位于 refactor/global-entities。历史阶段记录保存在 Git；当前页仅描述可用 API。
最新授权与计时器分别见 [连接授权](CONNECTION_AUTHORITY_AND_TIMERS.md) 和 [持久计时器](ENTITY_TIMERS.md)。

## 身份与承载

Node 是服务/网络载体，实体可以在不同 Node 间迁移。EntityId 由共享 CAS 分配号段，
是集群内非零 u64，不靠业务选择字符串或每节点自增。持久引用和 Lua 使用 16 位小写十六进制，
协议使用大端 8 字节；迁移/重启不会更换 ID。实体类型、所在 Node、授权连接不属于 ID。

FER5 全局记录按 fsnet/entity/v2/<EntityId> 保存类型、创建摘要、owner_connection、
owner_revision、placement、租约/fence 与快照。授权连接不是运行实体的 owner Node。
客户端重连得到新 ConnectionRef；同账号不自动继承旧连接授权，账号/角色映射由业务负责。
服务端可对多个实体绑定同一连接。可见性、授权和当前操控对象分别管理。

## 创建与找回

```lua
local player=Player:spawn({},{
    creation_key='player:'..self.context.subject,
    target_node='map-02',
    persistence='durable',
})
player:bind_owner(self.context.connection)
player:show_to(self.context.connection)
player:possess(self.context.connection)
local saved_id=player.id
local restored=fsnet.ref(saved_id)
```

实体内 scope 是调用实体 ID，启动任务使用 bootstrap_scope。重试 key 和初始参数必须相同；
变更 owner/名字等运行状态应另调用实体方法，不能修改原创建 claim 的参数。
已销毁实体保留身份和创建 claim，不因重试复活。创建 ACK 丢失仍能找回同一个 ID。
rebuildable 允许以初始状态重建缓存，不承诺业务状态持久化，也不能安排持久计时器/持久投递。

## 业务 RPC 与 Gateway

普通调用是 @ + FSE3，其中含 EntityId、方法、可选 request_id/expected_type 和 bytes。
服务端读取权威类型，执行 schema 的 client/server/owner 策略；不同类型的错误断言会拒绝。
客户端选择 LocalPlayer/RemotePlayer 只影响表现，不影响服务器 Player 的类型。
Gateway 必须通过 bind_entity / entity 配置允许的方法，不配置即拒绝；此白名单与实体策略叠加。

```lua
local login_id=fsnet.ref(manager_id):call('discover','')
local player_id=fsnet.ref(login_id):call('login','Alice')
fsnet.ref(player_id):call('move','1')
```

完整 Manager → 多 Node LoginEntity → Player → 地图示例在 examples/entities。
入口 ManagerId 从可信启动/服务发现提供，不通过客户端自行声明账号或 socket ID 得到权限。
各 LoginEntity 的创建 scope 独立；账号唯一角色映射是业务注册表/账号实体的职责。

## 迁移、投递、计时器

指定目标交接保留 EntityId，提交 placement/fence 后由目标接管；Gateway 恢复原客户端投影。
持久 outbox/inbox 与回执提供可靠实体命令；普通无回执 RPC 不承诺自动重发。
持久任务携带实体快照移动，调度器使用持久索引发现未激活任务并按租约/fence 执行。
见 [交接](ENTITY_TRANSFER.md)、[投递](ENTITY_DELIVERY.md)、[计时器](ENTITY_TIMERS.md)。

## 公共机制与运行边界

两端共用 EntityId、连接引用、FSE3、属性值/patch、计时器状态与周期推进算法。
授权、持久提交与发现调度在服务端边界执行，客户端不能因此获得 server-only 权限。
Lua 类、SDK、Gateway 和宿主都要求全局 ID。任意字符串 ID 创建、账号路由、
Type.method 业务路由及适配器已经删除。Node 的底层控制路由仍用于协议基础设施。
当前记录 writer 使用 FER5，授权只认逻辑连接；本次没有改写磁盘与线协议格式。

验证范围、当前修复与尚保留的成本见 [验证](VALIDATION.md) 和
[本轮修复](GLOBAL_REVIEW_FIXES_2026-10-25.md)。历史性能基线见
[全局实体测量](BENCHMARK_GLOBAL_ENTITIES_2026-10-25.md)，不能将历史版本数字当作当前性能。
