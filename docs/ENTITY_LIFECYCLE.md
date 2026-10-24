# 服务端实体生命周期

服务端属性类现在支持 `on_create`、`on_activate`、`on_deactivate(reason)`、`on_destroy(reason)`。
这些是实体宿主内部入口，不注册公开网络路由；客户端表现 hook 仍是另一套对象生命周期。

```lua
local Actor=fsnet.Entity:extend('Actor',{owner='id'})
Actor:attr('created',{type='bool',default=false,sync='owner'})
Actor:attr('visits',{type='int32',default=0,sync='owner'})
function Actor:on_create() self.created=true end
function Actor:on_activate() self.visits=self.visits+1 end
function Actor:on_deactivate(reason)
    -- reason 为 explicit、idle 或 destroy。
    -- 保存最后的业务属性；不要依赖必须执行的外部副作用。
end
function Actor:on_destroy(reason) end
Actor:register()
-- 在服务端无状态 RPC/后台任务中：
-- Actor:spawn_or_restore('alice')
-- Actor:deactivate('alice')
-- Actor:destroy('alice')
```

## 提交合同

新实体先获得有 fencing 的租约，内部记录初始化未完成。首个操作执行 create → activate → 业务方法，
所有属性修改与初始化完成标志在同一个最终 CAS 中保存。任一步失败不发布这些属性；重试/接管会重新执行未完成 create。
旧 FER1 记录视为已初始化，不重新执行 create；新建而未完成初始化的记录使用 FER2 标记，成功后恢复 FER1。
旧版程序不能读取 FER2，滚动升级必须先排空旧写者，不能混用不识别该格式的 owner。

activate 在当前驻留实例首次成功执行、恢复或重新取得不同 ownership fencing 时执行；普通租约续期不重复触发。
普通业务拒绝不会把健康的驻留实例重新激活。失败的首次激活可重试；hook 与业务修改共同提交，不单独发布半个激活结果。

卸载执行 deactivate hook → 保存最终持久属性并清空 owner/租约的单记录 CAS → 移除本地句柄。
失败保留原持久状态与实例，可重试。idle 超时使用相同路径；瞬态值仍在实例移除后清空。
删除执行 deactivate（reason=destroy）→ destroy → 按当前存储版本 erase；删除后的同 ID 重建使用新存储版本，旧句柄失效。
尚未成功激活的初始化失败记录可以释放/删除，但不会调用未激活实例的卸载 hook。

`Class:deactivate(id)` / `Class:destroy(id)` 仅服务端已注册类的异步任务可用，目标必须驻留且空闲；
有正在执行或排队的操作时拒绝。不能在目标自己的实体方法中同步等待自己的生命周期操作。
不存在的目标返回 not_found；永久删除只删除实体记录，不是跨实体业务事务或外部资源级联清理。

所有 hook 都可能在存储失败、宕机、超时后重试。属性初始化只在成功提交后生效；这不保证外部副作用恰好一次。
SIGKILL、失去所有权、预算耗尽或进程直接 stop 不保证卸载 hook 执行。必要外部操作仍需要幂等/持久化协议。

## 优雅关闭

原生 `EntityScriptHost::drain(reply)` 停止接收新实体操作，空闲实例通过相同 deactivate 路径保存并释放租约。
若尚有活动/排队请求则返回 overloaded；继续 poll 至请求结束后可重试 drain。进入 drain 后不恢复接单。
关闭顺序应为停止入口接单 → drain 并持续 poll → 结束复制服务/Lua/存储/Node。
`stop()` 保留快速取消语义，不在析构路径里隐式运行异步 Lua hook。

低层 `runtime.entity` 可使用 `lifecycle={create=...,activate=...,deactivate=...,destroy=...}`，
属性 handler 签名为 `(self,ctx,payload)`、返回响应字节；推荐 OO 包装，避免混淆 Lua 门面每请求构造与真实实体激活。
原生 `deactivate(handle,reply)` 继续检查 owner/generation，旧句柄不能卸载新实例。

`fsnet-lifecycle` 覆盖失败/重试、创建标志跨宿主恢复、激活次数、卸载失败回滚、空闲卸载、删除重建、句柄 fencing 和 drain。
