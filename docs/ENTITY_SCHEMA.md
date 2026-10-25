# 实体 schema、权限与属性提交

所有业务方法使用 `Class:rpc` 显式声明角色；未声明的方法仅供本地调用。

```lua
local Player=fsnet.Entity:extend('Player')
Player:attr('hp',{type='int32',default=100,client='r',sync='observers'})
Player:attr('name',{type='string',default='',client='rw',sync='owner'})
Player:rpc('rename',{callers={'client'},access='owner'})
function Player:rename(name) self.name=name end
Player:rpc('damage',{callers={'server'}})
function Player:damage(amount) self.hp=math.max(0,self.hp-tonumber(amount)) end
function Player:validate_write(proposal)
    return not proposal.name or (#proposal.name>0 and #proposal.name<=32)
end
Player:register()
```

`callers` 可含 client、server；owner 策略要求当前认证的逻辑连接匹配实体记录。
相同 subject 的另一条连接不会继承授权。Gateway 保留原客户端角色和 ConnectionRef，
后端连接的 server 身份不提升原调用者权限。负载不能设置认证上下文。
权限在排队、等待恢复和提交边界复查；回执查询也不能绕过当前权限。

## 公共属性载体

`EntitySchema` 描述字段类型、server/client 读写、同步范围、持久性与 RPC 策略。
`EntityAttributes` 在所属运行时线程保存值和单调 revision。支持严格整数范围、有限 double、
bool、string、结构、固定数组和有界 list/map/set；未知字段、非法类型、容量溢出均拒绝。
结构与容器见 [结构](STRUCTURED_ATTRIBUTES.md)、[容器](CONTAINER_ATTRIBUTES.md)。

原生 `stage` 生成独立候选，完成带 fence 的实体记录 CAS 后才能 `publish`。
失败、旧版本、脚本拒绝和回执重放不发布属性事件。`persistent()` 排除瞬态值；
瞬态值仅在当前驻留期保留，不承诺跨迁移或崩溃恢复。
`on_attributes` 是可信框架回调，完整镜像可含私有字段，必须按连接过滤后转发。
回调异常不能撤回已经持久提交的状态。

## 客户端提案和同步

客户端字段可写仍需归属当前连接，写入经过同一实体 mailbox、版本检查和 `validate_write`。
校验器收到所提交顶层属性的最终完整值；应返回 true，避免执行外部副作用。
错误、超时、拒绝、fence 丢失或 CAS 失败不会发布本次候选。网络上的远端副作用不会自动撤销。

两端共用 `attribute_delta`、`attribute_write_delta` 和 `AttributeReplica`。
FSA1 表达完整值，FSA2 表达容器编辑，FSA3 表达字段路径；生成器按大小选择表示。
修改 `position.x` 不要求下发整个 position。上行应用先检查所有根字段权限，再在独立副本上
应用 patch 并校验最终 schema，不能部分提交。显式 request_id 保留稳定的完整提案字节用于重试。

确认副本只有收到服务端确认才更新；预测状态独立保存。初次订阅或可见性变化使用新快照，
不允许用无关订阅的差量补基准。过期差量不能倒退 revision；断线或撤权后副本不可继续访问。
服务端发布不会再次生成客户端写提案，因此不会产生同步回环。

见 [共用差量](SHARED_ATTRIBUTE_DELTAS.md)、[连接授权](CONNECTION_AUTHORITY_AND_TIMERS.md)、
[全局实体](GLOBAL_ENTITIES.md) 与 [原生 SDK](CLIENT_SDK.md)。
