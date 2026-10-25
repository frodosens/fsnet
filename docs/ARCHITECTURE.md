# 当前架构

实现快照：2026-10-24，`refactor/script-runtime`。阅读入口为 [文档导航](README.md) 与
[API 使用指南](API_GUIDE.md)；本文描述已实现机制，后续计划单列在 [路线图](ROADMAP.md)。

C++ 只负责网络、调度、实体/场景载体、存储机制和脚本宿主；业务状态结构、规则和跨实体流程由脚本定义。Lua 是游戏业务脚本语言。

2026-10-25 已确认下一阶段的[全局实体身份与业务 RPC 约束](ENTITY_ADDRESSING_PLAN.md)：
所有业务远程调用以实体为目标，每个逻辑实体独立分配全局 EntityId，多 LoginHub 可以分布部署并跨节点创建实体。
该重构尚未实现；下文仍描述当前类型＋ID 寻址和已有 API，不将规划视为已交付能力。

```mermaid
flowchart TB
    Client[客户端应用] --> SDK[Client SDK: 会话 / 句柄 / 实体事件 / Lua桥接]
    SDK --> Node[Node: TLS 1.3 / 身份授权 / RPC / 背压]
    Service[服务节点] --> Node
    Node --> Router[EntityRouter: owner 查询 / 连接复用 / 有界重试]
    Router --> Host[EntityScriptHost: 每实体 mailbox / tick / 生命周期]
    Host --> Lua[LuaEngine: 业务协程 / 指令与内存预算]
    Lua -->|新状态与响应| Host
    Host --> Worker[AsyncDatabase: 有界后台存储执行器]
    Router --> Worker
    Worker --> Storage[Storage: get / 单记录 CAS / erase]
    Storage --> Files[POSIX 文件存储]
    Storage --> SQLite[SQLite]
    Storage --> Memory[Memory: 开发与测试]
    Storage --> PG[PostgreSQL: 可选适配器]
```

上图的 Router 是可选服务间路由组合；客户端可直连 owner，或通过 ClientGateway 接入多个 owner。
Gateway 使用独立后端连接委托可信账号，转发属性流并恢复迁移订阅，见 [跨节点接入](CLIENT_GATEWAY.md)。
指定目标交接使用同记录 CAS 保存 placement 与交接编号，目标激活提交后 ACK，详见[交接协议](ENTITY_TRANSFER.md)。

目录查询使用公共 `decode_entity_record_view` 读取 owner、租约和 placement；它以
`string_view` 校验记录头与快照封装，不复制实体状态、RPC 收据或构造投递映射。
完整状态及投递内容的语义校验仍由实体执行前的 `decode_entity_record` 负责。
底层 Storage.get 仍读取完整原子记录，此优化减少解码与分配，不减少存储读取字节。

客户端SDK入口为 [CLIENT_SDK.md](CLIENT_SDK.md)。Client组合Node和ClientScriptHost，
持有会话generation、实体元数据和控制事件；属性投影仍只有一份AttributeClient，Lua和Native
共享确认版本/codec。重连不重放业务RPC，旧宿主释放其路由和额外连接，不复用旧句柄。

## 公共引擎与各端边界

客户端/服务端共用递归值模型、schema、FSA 编解码、`attribute_delta`、候选补丁应用和 Lua 载体机制。
客户端 SDK 用 `attribute_write_delta` 将确认基准与请求最终值交给共用生成器；服务端用
`stage_attribute_proposal` 授权并还原最终属性，再执行业务校验。广播仍由共用生成器产生每订阅者的差量。

| 公共机制 | 不能合并掉的各端职责 |
| --- | --- |
| 值、类型、路径与容器操作 | 服务端决定账号身份、属性写权限、可见范围和业务接受规则 |
| 差量生成、FSA1/2/3 编解码、候选图像应用 | 客户端持有确认副本；服务端持有权威状态并执行存储 CAS |
| 受管属性对象、Lua 运行时预算 | 客户端图形/输入适配；服务端调度、存储与生命周期适配 |
| 确定性 Lua 业务函数可共享 | 首次效果播放由客户端处理，服务端独立验证输入，回放禁止外部副作用 |

后续机制默认先检查现有公共实现，避免两端维护平行算法；业务类型与玩法继续留在 Lua。
这里 owner 授权绑定逻辑 ConnectionRef；持有运行租约/fence 的 Node 与该连接是不同概念。账号 subject 仅供业务认证使用。

## 存储合同不依赖数据库事务

最低要求是读取带版本的记录、按预期版本原子替换、按版本删除。版本在删除/重建后不能复用。Storage 不要求 SQL、多记录事务或数据库服务。所有引擎边界统一使用 Storage，不提供旧类型别名。

Memory/SQLite 可以提供可选的 transact 扩展，但实体宿主、目录路由、EntityCatalog 和所有权 不调用它。多实体变更不伪装成原子事务，业务用脚本定义补偿与状态推进；框架已提供[持久命令 outbox/inbox](ENTITY_DELIVERY.md)，状态与发送意图共同提交，确认后推进完成结果。

每个托管全局实体使用一个 FER5 记录：

```text
entity-key = 全局 EntityId
record = type + creation digest + owner connection/revision + placement
       + owner incarnation + lease expiry + fencing counter
       + opaque script state + bounded request receipts
       + optional durable outbox / inbox cursors / channel counters
```

owner、租约、状态和去重结果共同受一次单记录 CAS 保护。接管会替换同一个记录，导致旧 owner 的预期版本失效；提交无需同时修改两个 key。

文件后端用 SHA-256 派生文件名，记录中仍保留原 key 并校验；跨进程 flock 保护 CAS，临时文件写入后 fsync，再同目录 rename 并 fsync 目录。删除写 tombstone，保留版本历史；损坏的记录拒绝读取，不自动初始化为空。具体保证与限制见 STORAGE.md。

## 执行与响应顺序

1. 入站请求进入类型 + ID 对应的 FIFO mailbox。
2. 后台读取实体记录，必要时用 CAS 获取/续租；租约有效且足够长时复用。
3. Lua 收到状态字节、实体 ID 和参数，执行业务；可以 await 外部 RPC。
4. Lua 返回新状态和响应。宿主用此前的记录版本 CAS 替换完整实体记录。
5. 提交成功后才返回业务成功响应，并释放 mailbox。

同一实体的不同方法和 tick 共享 mailbox，await 期间不释放。其他实体可以继续推进。Lua/Node/宿主均在所属线程调度，数据库或文件操作在独立存储线程执行。CPU 扩容使用多个分片/节点/进程，不把一个 Lua VM 放到多个线程并发执行。

脚本失败不发布返回的新状态；下一次请求恢复已提交状态。已经发送的外部 RPC 副作用不受本地 CAS 回滚，必须另行幂等。状态聚合可表示玩家、房间或场景，C++ 不内置玩法规则。

## 服务端持久命令

服务端实体事务内 `ref:send(method,payload)` 暂存命令；父事务成功提交后才投递。
接收方把状态和 inbox 水位同一次 CAS 提交，源方再经 mailbox 保存完成结果。
超时、断线、过载和迁移保留 pending，业务 reject/明确失败作为结果持久保存。

```mermaid
sequenceDiagram
    participant A as 源实体
    participant D as 共享 CAS 存储
    participant W as 投递器
    participant B as 目标实体当前 owner
    A->>D: 状态 + outbox 同记录提交
    W->>D: 从持久源索引发现待发消息
    W->>B: 按逻辑实体路由，重复使用 ticket/序号
    B->>D: 业务状态 + inbox 水位同记录提交
    B-->>W: 成功或明确失败的持久确认
    W->>A: 经 mailbox 提交完成结果
```

每源 FIFO，不保证跨源全序。完成结果由业务查询后显式清理；接收水位不采用普通回执的
64 条淘汰窗口。源/目标 epoch 防止旧消息作用于删除重建的新实例。
这不改变同步 call、客户端输入或外部支付/邮件的语义，详见 [ENTITY_DELIVERY.md](ENTITY_DELIVERY.md)。

## 所有权、接管和重试

进程 owner 格式为稳定节点 ID/唯一启动 incarnation。路由器优先查找有效 owner；无 owner 或租约过期时按稳定节点 ID 做 rendezvous 选择，最终由存储 CAS 决定谁取得所有权。健康视图不授予独占写权。

过期/显式释放后，另一个宿主可恢复同一记录。旧 owner 即使仍在计算，也不能越过 CAS 发布旧状态。该保证是权威状态提交排他，不承诺网络分区时所有机器上都绝无残留计算；外部副作用仍必须携带业务幂等/隔离协议。租约接管需要同步时钟和保守的超时配置。

带请求 ID 的操作把请求与响应和新状态一起保存。重试回放已提交结果；同 ID 不同输入拒绝。默认保留最近 64 条、最多 4 MiB，淘汰后的重试可能再次执行，不能宣称无限期 exactly-once。

## 安全和 P2P

TCP 已实现 TLS 1.3 双向证书验证、目标名称校验和证书指纹路由授权。公开客户端可以只使用独立证书与空 token；内部 cluster token 不应分发给客户端。明文连接仅允许 loopback。详见 SECURITY.md。

ICE/STUN/TURN 直连与中继原语已实现并做本机验证；原始 IcePeer 不加密业务数据。认证信令、会话权限、加密数据报、UDP 禁用时的 TLS 中继后备仍在后续阶段，不能把本机 TURN 测试当成真实跨 NAT 验收。

## 当前边界

- 文件后端支持本机多进程、持久化与崩溃恢复，不会把每台机器自己的目录自动复制成一致存储。跨机器 HA 需要具备相同 CAS/持久化语义的共享或复制后端，不能靠文件复制或健康检查假定一致性。
- PostgreSQL 适配器已实现，使用单记录条件 INSERT/UPDATE/DELETE；TLS、跨连接 CAS、实体进程接管与数据库崩溃恢复已验证。主备复制/仲裁与跨机 HA 仍需单独部署验收，数据库不是实体运行时的必需条件。
- 固定步长 tick、句柄代数、快照恢复和空闲卸载已实现。可部署启动器、指标、打包与本机多进程故障验收已交付；Native/Lua客户端SDK已交付；持久命令及重启补投已交付；图形/浏览器适配与跨机长稳容量仍待完成。
- 整体仍在分阶段交付，尚未标为任意规模通用游戏的生产验收版本。当前测试证据见 VALIDATION.md。

## 服务端生命周期

create/activate hook 与首个业务操作共用一次状态提交；创建未完成标记随租约持久化，宕机后可重试初始化。
deactivate hook 保存最终状态并原子释放租约；destroy 通过预期版本 erase 删除记录。空闲卸载共用同一路径，
drain 停止接单并卸载空闲实例，stop 保留快速取消。hook 不提供外部副作用 exactly-once 保证，详见 [生命周期](ENTITY_LIFECYCLE.md)。

## 客户端预测载体

共享类支持独立的只读确认副本和预测实例。应用显式提交带序号的输入，并在收到与
确认快照配套的累计处理序号后校正。运行时从确认快照构建候选预测状态、重放未确认
输入，成功后再替换旧状态；失败不移除历史或发布部分预测。预测执行禁止网络、等待
及跨对象修改，效果仅由首次成功 push 的返回值交给应用处理。业务输入协议和确定性
规则留在 Lua，服务器仍独立验证输入并通过原有 CAS 发布权威属性。具体边界见
[PREDICTION.md](PREDICTION.md)。

## 实体存在性与客户端表现

服务端脚本在可信登录上下文中 spawn/restore 权威实体，再显式 show 给场景内连接。
AttributeReplication 将连接/认证主体、实体代数与自动订阅绑定，先发送过滤后的初始
快照，再发送属性增量；hide、身份变化或卸载撤销订阅。客户端按服务器确认的 owner
选择 LocalPlayer/RemotePlayer 表现 prototype，控制目标另外同步，切换时先解除旧目标。
表现类不改变服务器实体类型，也不参与服务器授权。场景/AOI 策略留在 Lua；本阶段只
提供直连或 Gateway 转发的存在性同步与跨 owner 单控制目标，不提供持久化场景。详见
[ENTITY_PRESENCE.md](ENTITY_PRESENCE.md)。

## 属性、容器与状态载体

`AttributeValue` 是拥有独立存储的标量、结构、数组和字符串键 map。schema 声明叶类型、字段、
固定维度或动态容量；Lua 受管字段视图按路径解析当前值，不能用 rawset 绕过原生 setter。
list/set 使用序列值承载，set 的标量唯一性由 schema 检查，不内置 Item/Task 等游戏类型。

| 载体 | 修改与提交语义 |
| --- | --- |
| 服务端请求对象 | setter 立即改本次镜像，方法正常返回后 CAS，成功才发布/广播 |
| 客户端确认副本 | 仅反映服务器确认；允许写的属性发送提案并等待，不提前乐观修改 |
| batch 草稿 | 独立候选对象，回调立即修改、结束后收集最终变化；禁止等待/网络/跨对象写 |
| 本地实例 | 只写 driver 属性，不联网、不持久化 |
| 预测实例 | 确定性输入执行时写可见 driver 属性；校正时重放未确认输入，不联网 |
| 客户端表现对象 | 普通 Lua prototype 包装确认副本，适配模型/镜头/输入，不提供权威状态 |

结构、vec3/mat4x4、固定数组以及有容量上限的 list/map/set 均已实现；深复制、类型、范围、
容量、唯一性、深度/节点/字节和原生内存预算继续检查。属性权限/可见性作用于顶层属性，尚无字段级 ACL。
`persist=false` 属性只保留在有界驻留缓存中，卸载/重启/接管后恢复默认；存储的属性 FSA1 图像只包含持久属性和版本，
网络快照按权限包含当前瞬态值。详见 [结构属性](STRUCTURED_ATTRIBUTES.md) 与 [容器](CONTAINER_ATTRIBUTES.md)。

## 默认上行提案与下行确认

```mermaid
sequenceDiagram
    participant C as 客户端 SDK / 公共引擎
    participant S as 服务端宿主 / 公共引擎
    participant L as Lua 业务校验
    participant D as Storage CAS
    C->>C: 确认基准与最终值 → 共用差量生成
    C->>S: 提案 base=revision=预期权威版本
    S->>S: 认证/owner/顶层权限；候选补丁应用与 schema 校验
    S->>L: validate_write(涉及属性的最终完整值)
    L-->>S: 接受或拒绝
    S->>D: 方法返回后 CAS 完整实体记录
    D-->>S: 提交成功
    S->>S: 按每订阅者可见图像生成确认差量
    S-->>C: 一向确认通知，新权威版本
    S-->>C: 写入成功响应
    C->>C: 应用确认副本，不调用 setter、不再次上发
```

成功且属性有变化时才广播；无最终变化的提案仍可返回成功，但不产生属性变化通知。
拒绝、异常、版本冲突或 CAS 失败不发布部分状态。
服务端普通实体 RPC 修改也从方法返回后的 CAS 接入同一个广播路径。

提交/合并边界是一次实体方法或显式客户端 batch，不是帧末或每隔几毫秒。
服务端方法中的多次 setter 合并为一次实体提交；await 不提交、不释放 mailbox。
普通客户端赋值每条都等待服务器确认，batch 只在回调结束后发一份最终提案。
接收同步不会重执行业务函数；业务回调主动再次写属性，仍可能形成业务反馈循环。

默认 write/字段赋值/batch 上下行共用差量；没有确认基准或指定版本与本地不同则回退全值，
服务器仍校验权威版本。固定 request_id 的 SDK 写入保留稳定 FSA1 全值表示，维持原有幂等重试合同；
未变化但显式提交的属性也不跳过授权/validator。细节见 [共用差量](SHARED_ATTRIBUTE_DELTAS.md)。

## 差量选择、协议与接收应用

- FSA1：完整快照、持久化图像、整属性替换与回退提案。
- FSA2：顶层 map 键编辑和序列连续区间 splice。
- FSA3：有类型的扁平字段路径，可替换叶/子树或编辑嵌套容器。
- FSN1：属性同步信封，携带订阅/实体身份、状态与租期；`system.attributes.update` 使用一向通知。
- FER1/FER2/FER3：服务端存储的实体记录（FER2 标记未初始化，FER3 保存指定目标），包含租约/fencing、完整状态和有界回执，不用网络 patch 代替存储快照。

`data.position.x`、`tasks['daily.1'].progress`、`items[64].count` 可只发送变化字段。
路径中的字符串键和数字下标有类型，map 键中的点不拆层；线上数组下标从 0 开始，Lua 仍从 1 开始。
每个子树比较叶编辑、区间替换、整值替换，整条记录再比较 FSA1/FSA2/FSA3 大小，严格更省时才选新表示。
没有公共前缀树编码，没有字符串内部字节差分；新增元素发送完整新值，首次订阅/重新 watch 发送可见全量快照。

接收端核对基准版本，在候选图像上应用所有编辑、校验合并后图像，再一次发布。
非法路径/类型、缺键、越界或预算失败不推进副本版本；版本缺口使订阅失效，应用需要重新取得快照。
FSA1/FSA2 保持可读；旧端不认识 FSA3，且旧服务端会拒绝上行补丁，当前没有能力协商，需要配套升级。

上限为深度 8、4096 值节点、256 顶层属性、1 MiB 编码、256 个编辑；容器容量与订阅预算另行限制。
候选对象仍有复制与遍历、权威实体仍整体持久化，不把网络字节节省当成同等 CPU/磁盘节省。
完整路径与大小样本见 [字段差量](FIELD_PATH_DELTAS.md)，合批语义见 [合批](ATTRIBUTE_BATCHING.md)。
