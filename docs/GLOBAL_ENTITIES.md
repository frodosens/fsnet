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
这一步尚未切换现有宿主、SDK、Gateway 和示例的业务调用路径。

`EntityId` 为非零 u64，Lua/文本表示固定为 16 位小写十六进制，例如 `0000000000000001`。
不使用 Lua 浮点数表示 ID。分配器使用同一权威 Storage 的单记录 CAS 预留号段，默认每段 4096 个；
本地递增不依赖节点编号或机器时间。进程重启取得新号段，未使用的号不回收，耗尽明确报错。
唯一性范围是共享同一权威 Storage 命名空间的集群，不能将两个独立恢复的数据库当作同一集群继续分配。

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

## 后续实施

1. 宿主按权威元数据解析类型与账号归属，统一实体调用、明确未知 ID 和墓碑行为。
2. 共用客户端/服务端调用编码、Gateway 转发、Lua EntityRef 与自动分配创建 API。
3. 指定 Node 幂等创建、Manager/LoginEntity 示例、服务实体重建和入口发现。
4. 迁移、订阅/预测、持久消息回归及 Release 性能对比。
