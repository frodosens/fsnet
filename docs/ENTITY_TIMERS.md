# 持久实体计时器

计时器属于持久 Entity，保存回调方法名、bytes 参数、UTC 到期时间、周期和剩余次数。
实体卸载、进程重启或迁移后继续使用原 EntityId 与计时器状态。
`fsnet-script` 和 `fsnet-service owner` 已安装调度服务；自定义宿主的接入方法见文末。

## Lua API

在已经注册的服务端实体方法或生命周期 hook 内使用：

```lua
Player:attr('seconds',{type='int64',default=0})

function Player:on_create()
    self:after(5000,'ready','',{key='ready'})
    self:every(1000,'second','',{
        key='clock',count=0,missed='coalesce',mode='fixed_rate',
    })
end

function Player:ready(payload,timer)
    -- 业务变化随本次触发一起提交。
    self.seconds=self.seconds+5
end

function Player:second(payload,timer)
    self.seconds=self.seconds+1
    if self.seconds>=60 then self:cancel_timer(timer.id) end
end

Player:register()
```

| 调用 | 合同 |
| --- | --- |
| `self:after(delay_ms, method, payload, opts)` | 延迟一次；delay 可以为零，提交后才可触发 |
| `self:at(utc_ms, method, payload, opts)` | 指定非负 UTC 毫秒时间，已过期则尽快执行 |
| `self:every(interval_ms, method, payload, opts)` | 周期大于零；默认第一次等待一个周期 |
| `self:cancel_timer(id)` | 取消当前 key；返回是否存在 |
| `self:timer(id)` | 返回当前任务信息，不存在则为 nil |

注册返回字符串 id。`opts.key` 可选；省略时引擎生成当前实体内的 key。
相同显式 key 再次注册会替换任务，产生新的 generation；已排队的旧触发不能执行替换后的任务。
取消并重建同一个 key 也遵守这条规则。替换是重新计时，不是旧 RPC 的永久去重合同。
客户端通过业务 RPC 请求安排工作；上述 API 不可在客户端副本、预测 reducer、batch 草稿或无实体任务中使用。

`after` / `at` 的 opts 仅支持 key；`every` 额外支持：

| 参数 | 默认 | 含义 |
| --- | --- | --- |
| `count` | 0 | 实际成功回调次数，0 表示无限；合并错过的周期消耗一次 |
| `delay_ms` | interval_ms | 第一次触发前的延迟，可以为零 |
| `missed` | `coalesce` | `catch_up` 按原到期时间逐次补跑 |
| `mode` | `fixed_rate` | `fixed_delay` 从回调成功返回时重新等待一个周期 |

fixed_delay 与 catch_up 不组合使用。fixed_rate/coalesce 保留原时间轴：例如每秒一次，
停机十秒后回调一次，再取原时间轴上下一次未来到期点。catch_up 逐次追赶，受普通 mailbox、
每轮调度预算和并发限制约束；不会把积压任务一次塞进无界队列。

回调的第二个参数包含 `id`、`generation`、`method`、`scheduled_at`、`interval_ms`、`remaining`，
以及本次调度的 `fired_at`、`late_ms`。remaining 包含当前这一次；0 表示无限。
查询接口返回前六项。generation 是不透明字符串，不用作业务计数器。
回调只能引用注册类上存在、可在服务端执行的方法；不保存 Lua closure 或协程。

## 提交、失败与恢复

注册、替换、取消首先修改当前事务的草稿。任务状态与实体属性、RPC 回执和持久 outbox
进入同一实体记录的 fenced CAS。业务方法失败，已经暂存的任务也不提交。
计时器触发在同一个实体 mailbox 中执行；回调成功后才推进到期时间或删除一次性任务。
回调可以取消自己、替换同 key，或者注册其他任务，自动推进不会覆盖这些显式修改。

回调异常、资源压力、租约冲突或超时会保留原任务，由调度器退避重试。
回调内已经发往网络、文件或其他系统的外部副作用可能重试；需要可靠外发时使用已配置的
EntityDelivery/outbox，外部系统另按其幂等合同处理。这里保证的是实体已提交状态的边界，
不宣称跨外部系统 exactly-once。

连接断开或 owner 换绑不销毁实体，因此不自动取消它的计时器；实体销毁则清除所有任务。
重建模式实体的业务状态允许丢失，不提供此持久计时器 API；需要这些任务的实体应选择 durable。
停机期间原 owner 租约可能尚未过期，恢复会等待或重试，保持既有 fencing 合同。
时间使用 UTC 毫秒，回拨时未到期的任务继续等待；向前跳变按所选 missed 策略处理。
这是 poll 驱动的调度，存储、mailbox、脚本耗时和发现刷新会增加延迟，不提供硬实时保证。

## 持久发现与资源边界

FSP3 在实体快照内保存公共 FST1 计时器状态；不向客户端属性流暴露回调参数。
独立的分片 FTI1 索引用于找回尚未激活的实体。实体提交前先登记 pending 索引，记录
拟提交 token、源版本和 fence；恢复时以实体记录为准确认、修正或清理。
pending 不能仅凭时间超时删除：生产者可能仍在提交。未完成的源版本通过串行唤醒/租约接管推进，
避免“索引已提交、实体未提交”造成丢任务，也避免旧 owner 覆盖新的索引声明。

任务到期使用有序队列，每个被跟踪实体最多保留一个队列项。索引按固定 64 个分片读取，
每片最多 4096 个实体、1 MiB；取消最后一个任务或销毁后可以清理。
默认每实体最多 64 个任务、256 KiB 状态，单条 payload 最多 64 KiB。
调度服务默认跟踪最多 65536 个实体、最多 16 项在途工作，发现刷新和失败重试各 100 ms。
这些是可配置/显式拒绝的资源上限，不是已经验证的在线人数或生产容量。
首次注册由持久索引刷新发现；已知任务按到期时间排队。每个 owner 服务都要运行调度器，
共享存储的索引元数据刷新成本需要在目标部署中测量。

## 原生宿主接入

```cpp
auto host=std::make_shared<fsnet::EntityScriptHost>(transport,database,owner_id);
fsnet::LuaEngine scripts(host);
fsnet::EntityTimers timers(host,database);
// 加载脚本后，持续在同一个 owner 线程上推进：
node.poll();
host->poll();
scripts.poll();
timers.poll();
// 停止新调度，再 drain/停止脚本与宿主。
timers.stop();
```

公共状态、编解码和周期推进位于 `entity_timer_state`，不依赖 VM 或存储；
EntityTimers 是持久发现/调度适配器，EntityScriptHost 负责授权、串行执行和提交。
这复用同一引擎机制，不增加 Lua/C++ 两套计时算法。

## 验证

`fsnet-timers` 覆盖三种周期策略、有限次数、取消/替换、过期触发去重、回调回滚与重试、
自取消/同 key 重排、容量、销毁、卸载唤醒、文件存储重新打开以及两个 ACK 丢失边界。
`fsnet-global-flow` 覆盖计时器随 Player 从 B 迁移到 A 后触发，并更新原客户端投影。
`fsnet-timer-process` 在真实宿主上注册任务、SIGKILL、重启并验证到期延迟/周期/取消状态，
再次 SIGKILL/重启后不重复已经完成的任务。
