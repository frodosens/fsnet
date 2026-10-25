# 全局实体审查修复

对应 [审查报告](REVIEW_GLOBAL_STRUCTURE_AND_COPIES_2026-10-25.md)，分阶段记录实际落地项。

## 第一阶段：调度活性、入口策略、重连

- R1：FTI2 索引保存 placement 提示，pending 必须读取实体记录判定；节点过滤和 schema
  过滤先于跟踪容量准入。迁移也按写前顺序更新索引；每 16 轮重新核对提示，保留持久发现兜底。
  缓存满时优先准入比已有等待项更早的任务，不驱逐在途任务。
- R2：`@` 取得权威实体类型后使用 bind_entity 的同一方法白名单，校验 expected_type；
  未配置方法默认拒绝。相同绑定可重试；服务端 RPC 仍由实体 schema 拒绝客户端。
- R3：LoginEntity 的 Player 创建参数固定且不带连接，随后显式绑定当前连接和配置显示名。
  同一 LoginEntity 的账号创建 key 可找回原实体；跨多个 LoginEntity 的账号唯一策略仍属于业务。
- R7 部分：无本地任务时索引轮询退避至 refresh 的 16 倍；本机计时器提交/不确定提交
  更新进程内提示立即唤醒发现。迁移、失败恢复继续依赖持久索引，进程内提示不是正确性条件。

新增外节点/未知类型先出现且 tracked=1 的调度回归、未列 client RPC 拒绝、允许列表中的
server-only 拒绝，以及新连接换绑/改显示名后保持 PlayerId 的两节点回归。
Debug timers、global-flow、gateway、gateway-boundaries、timer-process、example-timers：6/6 通过。
这些结果不是所有审查项均已完成的声明；后续阶段在下方继续记录。
