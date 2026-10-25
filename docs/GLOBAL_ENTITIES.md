# 全局实体重构实施记录

分支：`refactor/global-entities`。2026-10-25 开始实施。
架构约束见[规划](ENTITY_ADDRESSING_PLAN.md)，重构前基线与成本见[差异分析](ENTITY_ADDRESSING_COMPARISON.md)。

## 已对齐的边界

- 客户端保持 Gateway 连接，由实体路由转发，不直连地图 Node。
- Manager、Player 使用持久状态；LoginEntity 可选择状态重建模式。
- 使用新协议与记录格式，不要求旧客户端并行或历史数据转换。
- 所有业务远程调用以实体为目标；登录认证、实例选择和地图策略由 Lua 实现。
- 按阶段测试、commit、push。此页只将已验证部分列为已实现。

## 阶段一：身份与创建目录

已实现公共 `EntityId`、`EntityIdAllocator`、`EntityCatalog` 和 FER4 元数据编解码。
第一阶段仅提供底层能力；后续调用路径接入见下节，既有演示仍待统一迁移。

`EntityId` 为非零 u64，Lua/文本表示固定为 16 位小写十六进制，例如 `0000000000000001`。
不使用 Lua 浮点数表示 ID。分配器使用同一权威 Storage 的单记录 CAS 预留号段，默认每段 4096 个；
本地递增不依赖节点编号或机器时间。进程重启取得新号段，未使用的号不回收，耗尽明确报错。
唯一性范围是共享同一权威 Storage 命名空间的集群，不能将两个独立恢复的数据库当作同一集群继续分配。

EntityId 是可序列化的公开身份值，不是进程指针或临时订阅句柄。外部系统可以保存 16 位文本；
C++ 通过 `str()` / `EntityId(text)` 和 `bytes()` / `EntityId::from_bytes(bytes)` 往返转换，
二进制固定为大端 8 字节。RPC、数据库关联、任务和其他系统引用同一身份，迁移/重启/恢复不改变它。
身份分配与序列化是独立合同；分配协议使用 CAS 号段，稳定身份可以由其他系统长期引用。

新记录按 `fsnet/entity/v2/<EntityId>` 定位，不以类型构成身份。FER4 保存类型、账号归属、
创建摘要、持久化模式和终止标记，同时保留现有状态、回执、owner、租约、fence 与交接信息。
初始目标节点可以在首次激活前登记；类型与账号归属来自可信服务端创建请求。

```cpp
// 同步公共机制：在既有 AsyncDatabase worker 内调用。
fsnet::EntityCatalog catalog(storage);
fsnet::EntityCreation creation{"Player", account_id, encoded_initial_state, "map-b"};
auto id = catalog.create(manager_id, stable_creation_key, creation);
auto description = catalog.find(id);
```

创建以可信创建者作用域和稳定 key 标识。先 CAS 保存永久操作描述，再登记对应 ID；
重试补全同一记录，参数变化拒绝，存储提交后 ACK 丢失也不会生成第二个逻辑实体。
这不是多记录事务：操作描述存在、实体尚未登记的中间状态通过原参数重试补全。
创建操作描述和已销毁身份记录不能按普通 RPC 有界回执清理；原创建 key 重试返回原 ID，不能复活旧实体。
碰撞或损坏元数据直接失败，不覆盖已有类型。

状态重建模式的初始状态已经进入 FER4 合同；执行、租约接管时的状态重建及可靠投递限制在后续宿主阶段接入。
普通旧格式编解码暂时保留用于现有回归夹具，不表示提供线上新旧数据兼容。

验收覆盖：并行分配、独立进程共享文件存储、重启后未用号段不重发、并发相同创建、
三个持久化边界的 ACK 丢失、参数冲突、注入身份碰撞、终止身份重试、u64 耗尽与 FER4 截断拒绝。

## 阶段二：公共调用与宿主

接入宿主的全局创建、元数据分发、独立账号归属、终止墓碑与重建状态缓存。
原生 SDK 新增 `call(EntityId,...)`；`EntityRouter`、Gateway 和 Lua 共用 FSE3 编码及 `@` 入口。
类型化引用可附带 expected_type 约束，服务端始终读取权威类型，类型不符拒绝。
未知 ID 不隐式创建，销毁后不再调用或复活。旧类型化底层夹具暂时保留，示例迁移仍在后续阶段。

```lua
-- 服务端异步任务；实体内的创建作用域自动使用 self.id。
local player=Player:spawn({name='Alice'},{
    creation_key='player-for-this-login',
    owner_account_id=account_id,
})
-- player.id 是可以保存在属性、数据库或外部系统中的稳定字符串。
local saved_id=player.id
local restored=fsnet.ref(saved_id)
restored:call('move',input_bytes)
```

```cpp
std::string stored_id = id.str();
fsnet::EntityId restored(stored_id);
client.call(restored, "move", input_bytes, reply);
```

FSE3 布局为 `magic[4] + id[u64] + flags[u8] + method[u8 length,bytes]`，
之后可选 request_id 和 expected_type（各 u8 长度），最后为不透明业务 bytes。
request_id 最长 128 B，类型与方法最长 127 B；内部 `$` 方法不可通过此入口调用。

重建实体在本进程内保留业务属性和 revision；记录中保存初始状态与身份/租约，接管或卸载后从初始状态恢复。
仍进行存储 CAS 和 fencing，此阶段不宣称已经消除每次调用的存储成本。
持久 outbox/inbox 投递及有状态交接拒绝重建实体，避免对可丢失业务状态承诺跨重启可靠性。
持久 Player 的状态和回执继续按原合同提交。订阅和 Gateway 恢复在读取权威账号归属后再校验。

## 阶段三：指定节点创建和完整链路

`EntityTransfer` 的公共放置适配器接入 `on_create_on_node`：创建者先在共享 Storage 登记身份、类型、
初始状态和目标，再通过仅可信 owner 可调用的 `system.entity_create.activate` 通知目标激活。
目标重新读取同一 FER4，校验类型、placement、租约和 fencing；网络消息不能指定或覆盖初始状态。
确认丢失后按原 creation_key 重试复用原 ID。未激活的登记记录由相同创建操作重试补全。
这不是跨记录事务，也不承诺业务层账号绑定与角色创建为一个事务。

服务启动器在配置 owner endpoints 后安装公共 EntityRouter，Lua 的通用引用可以跨 Node 调用。
无路由器的单机宿主走同一 FSE3 分发路径。Gateway 保持一条客户端连接，backend 委托与订阅恢复继续复用原机制。

完整 Lua 示例在 [`examples/entities`](../examples/entities/)，分别给出共享 Player、Manager/LoginEntity 和客户端表现。
Manager 的持久 list 保存 LoginEntity IDs，登录实体可重建；LoginEntity 创建 Player、show/possess，
然后交接到地图节点。先建立投影使 Gateway 可以保持原客户端对象和订阅句柄。
账号认证由可信接入提供，示例没有实现密码数据库；跨登录实体复用同一账号的角色可以由 Account 实体协调，
不由全局 ID 唯一性暗中决定游戏账号策略。

Bootstrap 使用可信服务配置：在已加载类的宿主调用
`create_entity("Manager", {}, {"game-bootstrap", "login-manager", "", "node-a"}, reply)`，
将返回的 ID 保存为客户端入口配置。之后业务请求都通过实体引用执行。
它可以被另一服务、配置系统或账号资料引用，不保存 owner 节点作永久地址。

可运行的两节点验收（包含示例脚本加载、Gateway 和原生客户端）：

```bash
. tools/dev/activate.sh
cmake --build --preset debug --target fsnet-global-flow-tests
ctest --preset debug -R '^fsnet-global-flow$'
```

该夹具使用明确的 loopback 测试身份，并不替代已有 mTLS 交付验收。
覆盖 Manager 两个子实体在不同 Node 激活、重复 bootstrap 保留 ID、发现和登录、Player A→B→A 交接、
存储 ID 往返后再次 RPC、自动创建 LocalPlayer 投影和迁移时客户端句柄不变。
通用 `fsnet.ref(id):send(...)` 在存储执行器中解析权威目标类型，保留原 outbox/inbox 协议；
夹具还覆盖了跨节点持久奖励命令、客户端调用 server-only 方法拒绝和迁移后再次调用。
现有 scene/attributes 示例和底层旧类型化夹具仍保留，不能据此宣称已删除所有历史接口。

## 验证与交付边界

Debug 全量 45 项通过；新增身份、宿主和两节点链路 3 项在 Release、ASan/UBSan 下通过。
重建实体丢失本地状态缓存时也清除旧业务回执，避免在初始状态上重放旧响应；测试注入存储已提交但 ACK 丢失。
性能测量见[全局寻址对比](BENCHMARK_GLOBAL_ENTITIES_2026-10-25.md)。

Manager 的入口 ID 由可信配置供应；当前示例依赖已认证的账号 subject，没有实现密码认证或 Gateway 登录后身份切换。
旧协议/历史数据不提供转换或并行保证；旧底层接口仍供原生夹具与既有示例使用，后续可单独清理。
数字方法 ID、连接内短句柄和模块 schema 版本协商不属于本次已交付能力。
