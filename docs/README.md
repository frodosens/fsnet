# 文档导航

当前实现快照：2026-10-24，`refactor/script-runtime`。
C++ 提供客户端/服务端共用的引擎机制，业务逻辑写在 Lua；文档中的“已实现”不代表完成生产容量或公网验收。

## 开始使用

1. [当前架构](ARCHITECTURE.md)：模块边界、状态载体、提交时间、双向差量、协议与未完成范围。
2. [API 使用指南](API_GUIDE.md)：共享类 → 服务端注册/登录 → 客户端实体 → 字段/容器修改 → 合批 → 预测，以及原生入口。
3. [示例启动说明](../examples/README.md)：仓库内交付的可运行程序与脚本组合。
4. [开发环境与迁移](DEV_ENVIRONMENT.md)：固定工具版本、构建 presets、仓库内缓存与环境重建。
5. [构建与启动](../README.md)：依赖、CMake、存储与本地启动入口。

## API 合同

| 主题 | 文档 |
| --- | --- |
| C++客户端连接、实体句柄、Lua桥接与安装 | [客户端 SDK](CLIENT_SDK.md) |
| 类、继承、本地方法、RPC、作用域 | [面向对象 API](OBJECT_API.md) |
| 本地/确认/预测实例、执行域 | [共享实体](SHARED_ENTITIES.md) |
| 身份、RPC/属性权限、原生同步与提案 | [实体 schema](ENTITY_SCHEMA.md) |
| 服务端初始化、激活、卸载、删除与 drain | [生命周期](ENTITY_LIFECYCLE.md) |
| 登录、spawn/restore、可见性、控制与表现 | [实体存在性](ENTITY_PRESENCE.md) |
| struct、vec3、矩阵、固定数组与字段视图 | [结构属性](STRUCTURED_ATTRIBUTES.md) |
| 变长 list/map/set、Item/Task 示例 | [容器属性](CONTAINER_ATTRIBUTES.md) |
| batch 边界、草稿、失败与副作用限制 | [合批](ATTRIBUTE_BATCHING.md) |
| 字段路径、FSA3、大小选择与接收应用 | [字段差量](FIELD_PATH_DELTAS.md) |
| 默认上下行共用差量、固定 ID 回退 | [共用差量](SHARED_ATTRIBUTE_DELTAS.md) |
| 输入确认、校正、重放和效果处理 | [预测](PREDICTION.md) |
| Lua 宿主、协程、预算、底层兼容接口 | [脚本宿主](SCRIPTING.md) |
| CAS、存储后端、fencing 与恢复 | [存储](STORAGE.md) |
| Gateway 委托、转发、迁移与资源边界 | [跨节点接入](CLIENT_GATEWAY.md) |
| 部署模板、发行包、指标、故障验收 | [交付运行手册](PRODUCTION.md) |
| TLS、可信身份、网络与 P2P 边界 | [安全](SECURITY.md) |

## 实现状态与证据

- [完成状态](ENGINE_COMPLETION.md)：当前能力与尚未交付项。
- [路线图](ROADMAP.md)：后续阶段，不将计划视为已实现 API。
- [全局实体与业务 RPC 规划](ENTITY_ADDRESSING_PLAN.md)：已确认的全局独立 EntityId、业务统一实体调用、多 LoginHub 与跨节点创建约束；尚未实现。
- [验证记录](VALIDATION.md)：各阶段配置、测试数量与未验证场景；最新配置为 Lua ON、PostgreSQL OFF；当前结果见验证记录末节。
- [Copy 优化实施](COPY_OPTIMIZATION.md)：借用解析、同步组帧、Gateway属性字节转发和确认版本共享。
- [当前性能与字符串复制 review](PERFORMANCE_REVIEW.md)：RPC/Gateway、Lua/属性、mTLS持久化链路和 copy 审阅；本轮没有修改运行时。
- [第二轮架构与实现审查](CODE_REVIEW_ROUND2_2026-10-24.md)：保留基线 9de3218 的复现；13 项确认问题及回调边界已修复，修复机制与验证见文档首节。
- [属性复制开销优化](ATTRIBUTE_PERFORMANCE.md)、[合批性能](CONTAINER_BATCHING_PERFORMANCE.md)：历史提交、测量方法与原始数据。
- [字段字节样本](benchmarks/field-paths-2026-10-24.json)、[双向字节样本](benchmarks/shared-deltas-2026-10-24.json)：编码大小，不是网络延迟/在线人数结论。

生命周期、Gateway、交付测试及发行包结果见验证记录；部署范围与未验证项在运行手册中单列。
