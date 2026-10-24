# 分阶段交付

状态快照：2026-10-24。当前架构见 [ARCHITECTURE.md](ARCHITECTURE.md)，使用入口见 [API_GUIDE.md](API_GUIDE.md)。

分支：`refactor/script-runtime`。每个阶段形成可检查的改动，完成相关验证后 commit + push；不将未实现计划标为已交付。

| 阶段 | 范围 | 验收边界 |
| --- | --- | --- |
| 0：基础重构（已提交） | 审查、CMake、网络 RPC、可替换存储、fencing、健康路由、ICE/TURN、测试 | 当前本机验证记录见 VALIDATION.md；不代表完整生产级框架 |
| 1：脚本宿主（已实现初版） | 已实现通用宿主接口、Lua 适配器、消息/错误/协程边界 | 不加载脚本时核心独立运行；示例业务全部在脚本；Lua 适配器执行宿主合同 |
| 1.1：旧文件清理 | 原始版本已推送归档标签；旧 C/旧 SDK、脚本、压缩依赖和构建产物退出主线 | 删除后从干净构建目录重建并执行现有测试 |
| 2：场景与实体载体（初版已实现） | 单写者 mailbox、固定步长 tick、含宿主身份/代数的句柄、不透明状态快照与恢复、执行/内存预算 | C++ 不含玩法规则；并发、销毁、超时和脚本异常有测试 |
| 2.1：客户端实体（初版已实现） | 共享类、本地/确认/预测载体、服务端 spawn/restore、连接可见性、LocalPlayer/RemotePlayer 表现与控制目标同步 | PREDICTION.md 与 ENTITY_PRESENCE.md；场景/AOI 策略由 Lua 显式管理，服务端生命周期已另行补齐，仍无自动空间发现 |
| 2.2：结构属性（已实现） | 通用 struct、固定数组、vec3/mat4x4、整值/嵌套赋值、自动客户端提案与服务器提交后同步 | STRUCTURED_ATTRIBUTES.md；路径 delta 由 2.4 补齐，仍不含字段级 ACL、自动冲突合并或在线结构迁移 |
| 2.3：变长容器（已实现） | list、字符串键 map、标量 set，结构/容器嵌套、增删改同步 | CONTAINER_ATTRIBUTES.md；有类型/容量/总预算，不是逐笔业务操作日志 |
| 2.4：共用差量与合批（已实现） | 客户端 batch、递归字段/容器差量、默认上下行共用生成器、候选补丁应用 | ATTRIBUTE_BATCHING.md、FIELD_PATH_DELTAS.md、SHARED_ATTRIBUTE_DELTAS.md；固定 ID 全值回退，无公共前缀树、定时帧合包或跨实体事务 |
| 3：持久化与接管 | 异步存储执行器（已实现）、生产数据库适配器、单记录 CAS/幂等、fencing 与恢复编排 | 独立进程接管、旧 owner 写入拒绝、存储失败与重试边界 |
| 3.1：生命周期与跨节点客户端接入（已实现） | create/activate/deactivate/destroy、drain；Gateway 可信账号委托、订阅转发与 owner 迁移 | ENTITY_LIFECYCLE.md、CLIENT_GATEWAY.md；默认只自动恢复账号所属实体，AOI 需可信策略 |
| 4：安全与 P2P 集成 | 加密、客户端身份与授权、认证信令、NAT/中继接入及配额 | 恶意请求与重放检查；真实跨 NAT 测试需要相应网络环境；明确 UDP 禁用时的能力边界 |
| 5：迁移与发布（部分交付） | 可重定位包、部署模板、readiness/指标、独立进程故障验证已实现；Native/Lua客户端SDK已交付；图形适配、跨机HA与长稳容量待完成 | PRODUCTION.md；本机 mTLS 故障/负载与安装包验收，不以回环吞吐推算在线人数 |

阶段可根据实际依赖拆分为更小的提交。新的范围决定先写入架构文档，避免实现逐渐偏离“C++ 只提供载体，脚本实现业务”的目标。
