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

## 第三阶段：默认入口收口

- R4：scene/attributes 改为全局 SceneHub/ProfileHub 实体入口，启动器取得入口 EntityId
  后配置客户端；原生客户端也使用该 ID 调用 login/logout，不再发独立 login RPC。
- 默认 Lua ref/watch、原生 Client::call 与 EntityRouter::call 要求全局 ID，不自动回退。
  原始注册、任意 ID 创建、owner-from-id、类型化 RPC 与账号路由改为显式 legacy 名称，
  兼容夹具、诊断和性能程序已迁移。具体入口见 [兼容适配器](LEGACY_ADAPTERS.md)。
- 普通实体类不再向传输注册 Type.method；只有显式 enable_legacy_routes/legacy 注册启用。
  Gateway 的 bind_entity 只配置 @ 白名单，旧路由使用 bind_legacy_entity/account。
- 当前 API 指南、全局架构、SDK、Gateway 和部署配置已同步，历史机制文档明确标注兼容夹具。
  原生内部字段 owner_binding/owner_key 不再把连接授权称为 account。
- 注册/查询等兼容包装器仍遵守预测外部副作用约束；新增默认入口无旧路由及 SDK 不接受
  旧 ID 的断言。C++ 接口改变须重编，不能混用旧头文件/库。

Debug 全量 49/49 再次通过；安装消费者、真实进程恢复和所有交付示例包含在其中。

## 第四阶段：订阅和内部入口补查

默认 Client::watch 也要求全局 ID，旧 ID 使用 legacy_watch。宿主统一入队点在默认模式
拒绝非全局 ID，防止属性订阅/快照查询绕回隐式旧实体创建；新增拒绝且不写记录的回归。
原生不透明状态适配器显式启用 legacy，现代上下文注册走共享私有注册函数。
所有 Type.$* 方法均视为内部入口，开启兼容模式也不向传输注册快照/创建/写入控制方法。
Debug 全量 49/49 通过。
