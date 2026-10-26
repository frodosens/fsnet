# 持久化实体命令

服务端托管实体事务内使用 `Target:ref(id):send(method, bytes)` 暂存命令。
源业务状态、发送序号及 outbox 在同一次实体记录 CAS 中提交后，命令才被接受；
返回给脚本的 ticket 在当前事务提交之前是暂存值。源方法失败则全部回滚。
同步 `call` 保留原语义，不会自动变成持久命令。

```lua
function Quest:complete()
    if self.completed then return self.reward_ticket end
    self.reward_ticket=Player:ref(self.player_id):send('grant_reward','100')
    self.completed=true
    return self.reward_ticket
end
-- 同一源实体的后续方法中查询，不能在未提交的源事务里等待投递：
local phase,status,response=runtime.delivery_status(self.reward_ticket)
if phase=='succeeded' or phase=='failed' then
    -- 先处理结果，再在当前事务中显式清理；pending 不允许清理。
    runtime.delivery_forget(self.reward_ticket)
end
```

接收方必须注册允许 server 调用的实体方法，业务规则仍在 Lua。
查询 phase 为 pending/succeeded/failed/missing；只有非 pending 才是完成结果。
成功与明确失败结果均保留，必须显式清理；不会通过容量淘汰静默丢失。
目标不存在/业务 reject/方法或权限拒绝作为失败结果保存。断线、超时、过载、
存储失败及内部异常不删除待发命令，投递器后续继续尝试，没有待发 TTL。

## 机制

- 接收方把业务状态和 inbox 水位在同一个 CAS 提交，再返回确认。
- 序号按源实体实例、目标实体实例分通道递增。持久水位不使用普通 64 条回执淘汰窗口。
- 源投递器每次投递第一个未完成命令，提供每源 FIFO；前一个不可用时会阻塞该源后续命令。
- 确认经源实体 mailbox 提交为完成结果，避免与源业务 CAS 并发修改协议状态。
- 源/目标迁移时快照保留 outbox/inbox/序号；旧节点不能绕过现有租约与 fencing。
- 实体实例 epoch 使用存储版本生成，删除重建不复用。旧实例的待发奖励不能作用于新实例。
- 有未完成 outbox 时拒绝永久删除源实体；销毁 hook 新建待发消息也会使本次销毁回滚，避免消息被同一次删除抹掉。卸载、迁移和进程停止保留 outbox。
- 后台确认只更新协议状态；续租保留驻留临时属性及激活标记，不隐式运行生命周期。
- 原生 `EntityDelivery` 安装在每个 owner，持续 poll，复用现有目录和端点选择。
  内部 receive/ack 路由仅允许可信 server，接收方还核对源存储中的已提交发送意图。
- `fsnet/delivery/sources` 是持久源索引，在源最终 CAS 前登记；崩溃最多留下空索引项，
  不会留下已接受却无法发现的消息。索引不依赖存储扫描接口，不自动删除登记项。
  多个 owner 可扫描同一索引；重复投递通过接收方水位去重。

## 启用、容量与升级

原生组合 `EntityDelivery(node, host, database, endpoints, trusted_owner)`，随后每轮
`node.poll()`、`host->poll()`、`engine.poll()`、`delivery.poll()`。
先停止 delivery，再停止 host/Node。必须使用满足共享持久 CAS 合同的存储。

`fsnet-service owner` 配置 `FSNET_ENTITY_ENDPOINTS` 和 `FSNET_OWNER_PINS` 时，
同时启用交接及持久投递。证书与信任配置沿用 [指定目标交接](ENTITY_TRANSFER.md)。
目标必须已有成功初始化且由新运行时写入协议 epoch 的记录；旧目标需先通过新 owner
激活/恢复一次，否则暂存 send 返回失败，不会谎报消息已接受。目标可不在线。

默认每实体最多 64 条待发/完成记录、256 个发送通道和接收水位；协议预算 2 MiB，
每个 pending 另预留 64 KiB 结果容量；单消息和结果上限 64 KiB。预算不足时明确拒绝暂存。
索引默认允许 4096 个源，最多可配置 65536；不自动淘汰水位或索引，不是无限消息队列。
投递器默认最多 32 个在途源、完整扫描之间停顿 100 ms、1 秒索引刷新、单次网络等待 3 秒。
单次扫描按 poll budget 和在途上限分块，异步读取完成后即可继续下一块，不再每 32 个源额外等待 100 ms。
这仍不是单条消息的延迟保证：历史源读取、存储速度、黑洞目标占用在途槽位和整个索引的规模都会影响尾延迟。
这些是资源限制，不是生产容量或时延承诺；当前全索引扫描与重复投递需要按实际规模评估。

协议状态使用 FSP2 嵌入原有 FER 实体记录。新版本继续读取 FSP1；启用前须升级全部
owner、Gateway、目录读者及工具，旧版本不能读取 FSP2。公开 C++ SDK 需与本次库一起重新编译，不能混用旧布局头文件。关闭投递器不会删除已有协议状态。

保证覆盖已接受命令的实体持久状态；不自动回滚邮件、支付、普通外部 RPC 等外部副作用。
新一笔业务事件仍需业务持久标志，避免把两个独立的发送当成同一奖励。
不同源之间不承诺全局顺序，也不提供跨实体原子提交；不在线的 owner/目标、耗尽的容量、
不兼容部署会让消息继续 pending，需要恢复服务或调整配置，不保证无条件最终成功。

## 当前验证

`fsnet-current-delivery` / `fsnet-current-delivery-process` 验证全局实体 outbox 回滚、角色、inbox 去重及提交后 ACK 前崩溃恢复。
`fsnet-global-flow` 验证多 Node Gateway、定向迁移与持久投递。完整配置见 [删除适配层验证](REMOVE_ADAPTERS_2026-10-25.md)。

## 历史验证记录

以下数字对应 2026-10-24 提交；其中旧接口整合夹具已删除，不能视为当前检出版本的测试清单。

已通过专项：原生 FSP1/FSP2/消息/确认编解码、截断与重复拒绝；源状态/outbox 回滚、
源与目标 CAS 故障、容量拒绝、业务拒绝时连带回滚后续发送、超过 64 条后的重放隔离、
失败结果保存、双向实体迁移、持久索引重新发现、实例删除重建、来源内容校验与角色隔离、后台确认续租的临时属性/激活连续性、销毁 hook 暂存消息时的拒绝。
独立进程测试已验证源提交后 SIGKILL 自动补投、接收方提交后丢失确认并 SIGKILL、
重启恢复后奖励只增加一次。真实 mTLS 的独立 owner/Gateway 启动器也已验证提交后投递与客户端属性确认。测试为本机共享 POSIX 文件，不代表跨机 HA。

收尾验证（2026-10-24）已完成：Debug 39/39、ASan/UBSan 23/23、Lua OFF 核心 13/13。
包含真实 mTLS 启动器投递、后台确认续租、销毁 hook 暂存消息拒绝与完整安装包验收。
复现命令：

```sh
. tools/dev/activate.sh
cmake --build --preset debug
ctest --preset debug
cmake --build --preset asan
ctest --preset asan
```

本地日志在忽略目录 `.dev/delivery-*.log`，不随提交保存；结果也记录于 [VALIDATION.md](VALIDATION.md)。
