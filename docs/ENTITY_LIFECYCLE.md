# 服务端实体生命周期

服务端属性类现在支持 `on_create`、`on_activate`、`on_deactivate(reason)`、`on_destroy(reason)`。
这些是实体宿主内部入口，不注册公开网络路由；客户端表现 hook 仍是另一套对象生命周期。

```lua
local Actor=fsnet.Entity:extend('Actor')
Actor:attr('created',{type='bool',default=false,sync='owner'})
Actor:attr('visits',{type='int32',default=0,sync='owner'})
function Actor:on_create() self.created=true end
function Actor:on_activate() self.visits=self.visits+1 end
function Actor:on_deactivate(reason)
    -- reason 为 explicit、idle、transfer 或 destroy。
    -- 保存最后的业务属性；不要依赖必须执行的外部副作用。
end
function Actor:on_destroy(reason) end
Actor:register()
-- 在服务端后台任务或另一个实体的方法中：
-- local actor=Actor:spawn({},{creation_key='actor',bootstrap_scope='demo'})
-- Actor:deactivate(actor.id)
-- Actor:destroy(actor.id)
-- local status=Actor:transfer(actor.id,'owner-b')
```

## 提交合同

新实体先获得有 fencing 的租约，内部记录初始化未完成。首个操作执行 create → activate → 业务方法，
所有属性修改与初始化完成标志在同一个最终 CAS 中保存。任一步失败不发布这些属性；重试/接管会重新执行未完成 create。
全局实体记录保存元数据与初始化状态，创建 key 重试找回同一 EntityId。

activate 在当前驻留实例首次成功执行、恢复或重新取得不同 ownership fencing 时执行；普通租约续期不重复触发。
普通业务拒绝不会把健康的驻留实例重新激活。失败的首次激活可重试；hook 与业务修改共同提交，不单独发布半个激活结果。

卸载执行 deactivate hook → 保存最终持久属性并清空 owner/租约的单记录 CAS → 移除本地句柄。
失败保留原持久状态与实例，可重试。idle 超时使用相同路径；瞬态值仍在实例移除后清空。
删除执行 deactivate（reason=destroy）→ destroy → 单记录 CAS 保存销毁标记；保留 EntityId 与创建 claim，重试不会复活。
尚未成功激活的初始化失败记录可以释放/删除，但不会调用未激活实例的卸载 hook。

`Class:deactivate(id)` / `Class:destroy(id)` 仅服务端已注册类的异步任务可用，目标必须驻留且空闲；
有正在执行或排队的操作时拒绝。不能在目标自己的实体方法中同步等待自己的生命周期操作。
不存在的目标返回 not_found；销毁会保留身份墓碑，不是跨实体业务事务或外部资源级联清理。

所有 hook 都可能在存储失败、宕机、超时后重试。属性初始化只在成功提交后生效；这不保证外部副作用恰好一次。
SIGKILL、失去所有权、预算耗尽或进程直接 stop 不保证卸载 hook 执行。必要外部操作仍需要幂等/持久化协议。

## 优雅关闭

原生 `EntityScriptHost::drain(reply)` 停止接收新实体操作，空闲实例通过相同 deactivate 路径保存并释放租约。
若尚有活动/排队请求则返回 overloaded；继续 poll 至请求结束后可重试 drain。进入 drain 后不恢复接单。
关闭顺序应为停止入口接单 → drain 并持续 poll → 结束复制服务/Lua/存储/Node。
`stop()` 保留快速取消语义，不在析构路径里隐式运行异步 Lua hook。

原生 `deactivate(handle,reply)` 检查 owner/generation，旧句柄不能卸载新实例。
当前覆盖见 `fsnet-current-runtime`、`fsnet-global-flow` 与 `fsnet-timers`。

## 指定目标交接

`Class:transfer(id, target_node)` 将持久状态交给指定稳定节点，目标确认激活后返回状态。
`on_deactivate` 收到 `transfer`，目标执行 `on_activate`；不能在目标自身 mailbox 中等待迁移。
全局记录保存 placement，普通提交继续保留目标。详见[交接协议与升级约束](ENTITY_TRANSFER.md)。

持久命令启用时，存在 pending outbox 会拒绝永久删除；销毁 hook 本次暂存的新命令也会使
销毁回滚，不能一边接受发送意图一边删除它。需要发消息再删除时，先在普通业务事务中
提交命令，确认完成后再删除。后台 delivery ACK 不触发激活/卸载，续租保留驻留临时状态。
