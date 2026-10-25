# 逻辑连接授权与持久计时器

分支 `refactor/global-entities`，2026-10-25。按阶段记录已验证能力。

## 设计合同

引擎以逻辑连接授予实体权限，账号与角色的关联由脚本保存。ConnectionRef 表示一次客户端会话，
与物理 socket、本机 peer 数字、EntityId 和承载实体的 Node 分开。
客户端和服务器共用 Node 协议，但客户端角色不能取得 server-only 权限或服务端租约。
实体可持续存活；客户端重连产生新的连接身份，业务验证后显式重新绑定。
实体授权、客户端可见性和当前操控对象分别管理，一个连接可获得多个实体授权。

计时器保存所属实体、稳定 key、回调方法名、bytes 参数、UTC 到期时间、周期和重复策略。
计时器的注册/取消、业务状态变化和触发后的推进使用所属实体的同一次 fenced CAS。
不持久化 Lua closure、协程栈或物理连接；回调方法由恢复后加载的脚本提供。
过期延迟任务恢复后执行；周期任务默认合并错过的触发为一次，再继续原时间轴。
提供逐次追赶模式，并通过每轮调度预算限制恢复突发。失败保留本次到期任务并退避重试。
状态提交具有去重边界，回调对网络或外部系统的副作用不因此获得跨系统 exactly-once。
需要可靠外发时使用既有持久 outbox。重建模式实体不承诺持久计时器。

## 阶段一：公共连接身份

已接入公共 ConnectionRef、Node 连接身份和断线/身份变化复查。
ConnectionRef 由可信传输产生 128 位随机会话 ID 和非零代数，文本编码为
`connection/<32 位十六进制 ID>/<generation>`。它是引用，不是可自行声明权限的 bearer token。
同一账号的两个连接仍有不同身份；连接关闭后旧调用上下文失效。

Gateway 使用有界 FSCN1 委托 envelope 转发原始逻辑连接，owner 端仅接受可信 Gateway 的委托，
并将角色固定为 client。跨 owner 链路不重新分配客户端身份。
旧的原生 subject-only 委托夹具暂时保留，不作为新全局实体授权来源。

专项验证覆盖两个连接区分、序列化、连接关闭、重连不继承授权、Gateway 原会话传递及断线失效。

## 阶段二：实体授权

全局记录升级为 FER5，保存 owner_connection 与 owner_revision，不再保存账号归属。
FER4 全局记录不自动解释为连接授权；此分支仍不提供线上旧数据转换。
RPC owner 检查、私有属性读写、控制权和 Gateway 恢复都按原逻辑连接校验。
旧的非全局 ID 原生夹具保留其显式 subject 映射，不作为新 API 的账号模型。

```lua
local player=Player:spawn(initial_values,{
    target_node='map-02',
    creation_key=stable_creation_key,
    owner=self.context.connection,
})
player:show_to(self.context.connection)
player:possess(self.context.connection)
```

远端创建后的 show/possess 通过原客户端的可信 Gateway 链路转发至目标 Node；
请求受现有容量、超时和委托校验约束。客户端前端不暴露这些控制路由。

`ref:bind_owner(connection)` 显式换绑，`ref:bind_owner(nil)` 撤销授权；需要服务端异步任务，
通过目标实体 mailbox 和单记录 CAS 更新。换绑确认后，后续旧连接调用拒绝，旧投影撤销；
已完成的业务操作不会回滚，已进入底层存储提交的操作仍遵守原提交合同。
断线依据传输检测；不能宣称在未检测到网络断开时已经全局瞬时撤权。

创建 key 对应的初始参数包含初始授权。若业务需要跨重连找回同一个实体，可以保存 EntityId，
在身份验证后对既有引用换绑；也可先以不带 owner 的稳定参数创建/重试，再显式绑定当前连接。
不得把新连接加入原创建参数并指望同一 key 悄悄改变已经登记的创建操作。

两节点验收补充覆盖：同账号第二连接被拒、跨节点换绑、旧客户端投影失效、新连接调用成功，
以及直接在远端 Node 创建 Player 并向原连接自动投影/授予控制。

## 接入阶段

计时器状态、宿主事务、Lua API、持久发现和启动器已接入，使用合同和验证范围见
[实体计时器](ENTITY_TIMERS.md)。延迟、绝对时间、固定周期/固定延迟、补跑策略、次数、
取消/替换、重启、迁移和 ACK 丢失均有专项覆盖。
Debug 与 Release 各 48/48、ASan/UBSan 31/31、Lua OFF 核心 17/17 通过；复现配置与范围见
[验证记录](VALIDATION.md)。
