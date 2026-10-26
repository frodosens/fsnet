# 生产审查修复记录（2026-10-26）

对应 [生产审查](PRODUCTION_REVIEW_2026-10-26.md) 的 R01–R15。原报告保留基线证据，当前实现与验证以本记录为准。
本轮处理代码缺陷和安全回收范围，没有把热更新、完整 P2P、数据库 HA 或目标环境长稳宣称为已交付。

## 修复与回归对应

| 问题 | 实现 | 主要回归 |
| --- | --- | --- |
| R01 未授权激活 | 先只读元数据授权，lease 前比较 owner，生命周期与提交前复核；保活订阅并检查回调重入 | production-authorization、production-entity、global-flow |
| R02 连接换绑后私有流 | 请求、通知、轮询和迟到结果统一检查完整 ConnectionRef，撤销会话/委托/订阅 | production-authorization；含 identity/base/AOI 回调重入 stop |
| R03 poll 长时间占用 | poll_one 同时受事件数和时间预算限制，run 保留连续驱动语义 | production-transport：合法 v2 握手后持续通知 |
| R04 Lua GC 停机 | 禁止脚本注册用户 __gc；保留原生 userdata 清理 | production-lua：元表重挂、普通 GC、加载失败、stop、原生 watch 释放 |
| R05 原生临时图 | 展开前核对节点与字节，预留候选/schema/Await 内存，减少隐式深复制 | production-lua：2 MiB 配额、重复别名、schema 默认值、回滚后健康调用与费用释放 |
| R06 PG 黑洞 | 非阻塞 libpq，连接/锁/发送/接收共用绝对期限；DNS 地址预检；未知写结果不重放 | postgres：启动/读写黑洞、共享连接锁等待、已提交但丢回复、恢复重连、TLS 名称 |
| R07 空实体回执 | 驻留 fence 与 transient 属性内容分开 | production-entity：durable/rebuildable 空 schema、续租和卸载 |
| R08 Router 期限 | 业务调用使用原期限的剩余时间，业务超时不关闭共享连接，无键不盲重试 | production-client-router：有/无键慢调用、超时、断线与替代连接 |
| R09 活动任务超时 | 过期活动/排队任务收尾；具体执行 scope 失效，Lua 停止旧协程与副作用；worker CAS 等待结果 | production-entity、production-lua：迟到回复、旧 timer 不污染新事务、长等待取消、pending CAS |
| R10 完成链异常 | 执行与提交规划异常可靠收尾，storage admission 抛出也不搁置 mailbox | production-entity：完成路径及 worker admission 分配故障后恢复 |
| R11 worker 完成分配 | admission 分配 list 节点，worker/completed 用 splice 无分配交接 | production-storage：禁止分配仍完成、拒绝 admission 不占额度、取消队列仍回调 |
| R12 投影选择 | 两个查询入口使用共同选择函数，自动优先，同类按最早存活订阅 | production-client-router：创建顺序、重哈希与自动视图隐藏 |
| R13 初始化就绪 | 有限 start 与长期 background 分开；关键失败 sticky，ready 与初始化联动，排空期间失败同样非零退出 | production-lua、service-readiness：等待/成功/失败、真实 mTLS health、SIGTERM 交错 |
| R14 易失存储 | Storage::persistent 默认保守；SQLite 按实际连接文件判断，service 强制持久实例 | storage-capabilities、service-readiness：memory/临时 SQLite 拒绝，文件后端重开 |
| R15 token 反射 | v2 双随机数、HMAC-SHA256、角色域与常量时间比较，认证帧优先 | production-transport、tls、core：回显/反射/重放拒绝，server proof 前无业务负载 |

框架组件停止时使用 AsyncDatabase::StopMode::cancel_queued：未开始的操作取消并保留完成回调，当前已开始操作等待后端返回。
默认 stop() 仍为排空合同；该接口不能撤销已提交写入，也不能强行打断任意自定义 Storage 或操作系统文件 I/O。

执行 scope 与连接 current 分开：具体任务超时后不会因相同 EntityId 的下一笔事务而恢复有效；
仍然存活的逻辑连接可以由脚本跨 RPC 保存和复用。原生适配器也应遵守调用身份的 live()，外部副作用继续需要幂等。

## C01：已销毁源的安全退休

投递扫描器只退休具有匹配类型、永久 destroyed 元数据且没有 pending outbox 的全局源。
索引更新使用版本 CAS，冲突后重读，保留其他 owner 并发登记；活实体的空 outbox 和缺失记录都不会据此退休。
这是为了保护“先登记、后源 CAS”之间的窗口，防止消息已接受却失去发现索引。

current-delivery 回归覆盖容量设为 2 时完成/销毁两个源后第三源可投递，以及双扫描器和并发登记 CAS 冲突。
活源、发送通道和 inbox 水位仍有容量限制；不提供无限队列、任意活源退休或水位淘汰。
Memory 后端仍是开发用途的全表候选提交；实体持久化仍为完整记录 CAS，网络字段 patch 不等于磁盘字段写。

## 接入与部署变化

- 所有互连程序使用传输 v2；旧 token 首帧不兼容，不自动降级。头文件、库、脚本和进程来自同一构建。
- 脚本 __gc 不再允许注册；显式生命周期和原生资源终结器各自保留。
- runtime.start 是有限关键初始化，整个任务受期限约束；长期事件循环改用 runtime.background。
- PostgreSQL DNS host 必须配置数字 hostaddr，host 用于 TLS 验名；localhost 固定 IPv4 loopback，service 需明确目标。
- 自定义持久后端应显式实现 persistent() 能力声明，并满足原有 CAS/持久化合同；生产启动器拒绝未知或易失实例。

初始化 ready 是流量与健康信号；业务接入仍需负载均衡/授权策略遵循它。单个原生回调不可抢占，
文件系统和自定义后端也需自身的有界等待合同。具体部署限制见 [部署手册](PRODUCTION.md)。

## 验证记录

修复源码提交：`ec6cbdf3e19f55784360bc6c5fab303cea87089e`。

| 配置 | 结果 |
| --- | --- |
| Debug | 34/34 |
| Release | 34/34 |
| ASan / UBSan | 24/24 |
| Lua OFF | 20/20 |
| PostgreSQL 专项 | 1/1 |

全部通过；测试逐项结果、构建产物 SHA256 与问题对应关系见 [结构化验证](validation/production-fixes-2026-10-26.json)。
本机测试不是目标环境多机 HA、长稳或生产 SLO 验收。
此前的负向诊断夹具只对应旧基线；v1 客户端被 v2 拒绝不能用来证明 poll 预算或 HMAC 的正确性，
因此本轮使用能完成合法 v2 认证的新回归测试验证这些行为。
