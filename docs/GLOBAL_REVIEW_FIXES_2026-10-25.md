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

## 第二阶段：持有边界与复制

- R5：Router 增加拥有封包的 call_encoded，RoutedScriptHost 不再拆出 payload 再编码。
  Router 的同步编码入口用 string_view；异步 Call 继续拥有原帧。宿主全局分发不再转成
  FSE1 再 owning parse，而是把原输入缓冲移交结构化消息（移去头部仍有一次 memmove）。
  队列按保留的 capacity 计费，不把 view 保存到异步任务。
- R6：ConnectionRef 共用 matches_key，保持 canonical 格式检查而不分配格式化字符串。
  owner-only 策略单次调用只复查一次 live；属性写权限和批量快照的 owned 判断分别只计算一次。
  排队、挂起恢复、提交前的授权边界不合并。
- R7 部分：公共 entity_timers_wire_size 校验并计长，预算检查不再完整编码任务状态；
  实际编码一次 reserve。提交记录移动持有任务状态，只为 arm 保留少量索引元数据。
  获取/续租阶段仍保留必要的状态副本，强异常保证的注册草稿也仍保留。

Debug 全量 CTest 49/49 通过，包含二进制协议、队列持有、迁移/重启/ACK 丢失和客户端预测。

## 第三阶段：统一实体入口，删除旧实现

依用户要求，不保留接口适配层：

- 删除任意字符串 ID 创建、账号 owner 映射、独立 login/account 路由、Type.method 业务转发及客户端旧 call/watch 入口。
- Lua 仅保留全局实体类；公开 runtime.rpc/entity/entity_call/entity_request 不存在。内部注册要求显式策略与属性模型。
- SDK、Gateway、Router、宿主与订阅均要求规范 EntityId；Gateway 清单只接受 entity。
- 删除未使用的 EntityStore、离线双 key 迁移工具、过时夹具/对照程序；测试直接通过 Catalog 创建实体，不增加兼容 shim。
- scene/attributes/native 示例全部使用全局 Hub 入口；连接授权只使用 ConnectionRef，移除 AttributeView 中闲置的 subject 副本。
- 所有 Type.$* 控制入口保持内部使用；无效订阅的用户回调异常被隔离。

公共值/patch、可靠投递、计时器与存储 CAS 机制继续共用。异步持有边界保留必要的字符串所有权。
本次没有改写持久化与线协议格式；格式演进不与 API 删除混为一次迁移。

## 测试迁移与发现

新测试使用当前模型保留关键语义：current-runtime 覆盖生命周期、权限、回执/冲突、回滚、挂起撤权和 fence；
current-delivery 及独立进程覆盖 outbox 回滚、角色检查、inbox 去重和 ACK 丢失恢复；
current-prediction 覆盖校正重放与副作用约束；current-lua-limits 覆盖沙箱、指令和内存失败后恢复。

资源测试发现 OOM 结束后的协程栈仍保留临时对象，后续健康请求可能无法分配。
结束任务时先 lua_resetthread 再释放 registry 引用，释放失败栈，并验证持久状态没有提交。
完整验证结果见 [删除适配层记录](REMOVE_ADAPTERS_2026-10-25.md)。
