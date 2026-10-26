# 文档导航

当前文档描述全局实体、逻辑连接授权和客户端 / 服务端共用的引擎机制。玩法与账号策略由 Lua / 应用定义。

## 阅读顺序

1. [项目首页](../README.md)：项目模型、构建、可运行示例和能力范围。
2. [API 使用指南](API_GUIDE.md)：从类定义、实体创建到同步、合批与预测。
3. [全局实体](GLOBAL_ENTITIES.md)：EntityId、Node、连接授权、创建幂等与路由合同。
4. [示例导航](../examples/README.md)：完整服务端 / 客户端流程及源码位置。
5. [当前架构](ARCHITECTURE.md)：模块边界、提交过程和公共引擎机制。

## API 与机制

| 主题 | 文档 |
| --- | --- |
| C++ SDK、实体句柄、确认快照、Lua 嵌入 | [客户端 SDK](CLIENT_SDK.md) |
| 类、继承、本地方法、RPC 与作用域 | [Lua 实体 API](OBJECT_API.md) |
| 本地实例、确认副本与执行域 | [共享实体](SHARED_ENTITIES.md) |
| RPC / 属性权限、schema 与提案校验 | [实体 schema](ENTITY_SCHEMA.md) |
| 逻辑 ConnectionRef 与 owner 授权 | [连接授权](CONNECTION_AUTHORITY_AND_TIMERS.md) |
| 服务端生命周期、销毁与 drain | [生命周期](ENTITY_LIFECYCLE.md) |
| 可见性、控制与 LocalPlayer / RemotePlayer | [客户端实体表现](ENTITY_PRESENCE.md) |
| struct、vec3、矩阵与固定数组 | [结构属性](STRUCTURED_ATTRIBUTES.md) |
| 有界 list / map / set | [容器属性](CONTAINER_ATTRIBUTES.md) |
| batch 草稿、提交与副作用限制 | [合批](ATTRIBUTE_BATCHING.md) |
| 字段路径 patch 与接收端应用 | [字段差量](FIELD_PATH_DELTAS.md) |
| 上下行共用生成器与重试语义 | [公共差量](SHARED_ATTRIBUTE_DELTAS.md) |
| 确认、校正、重放与效果处理 | [客户端预测](PREDICTION.md) |
| 指定目标 Node 交接 | [实体迁移](ENTITY_TRANSFER.md) |
| outbox / inbox、重投与去重 | [持久投递](ENTITY_DELIVERY.md) |
| 延迟、周期、取消与重启恢复 | [持久计时器](ENTITY_TIMERS.md) |
| Lua 协程、预算与宿主边界 | [脚本运行时](SCRIPTING.md) |

## 开发与部署

- [开发环境](DEV_ENVIRONMENT.md)：工具准备、CMake presets、依赖与 `.dev/` 布局。
- [存储合同](STORAGE.md)：CAS、后端、fencing 与当前存储格式。
- [Gateway](CLIENT_GATEWAY.md)：身份委托、目录路由、订阅转发与迁移恢复。
- [部署手册](PRODUCTION.md)：安装包、证书、配置、指标与排空。
- [安全](SECURITY.md)：TLS、可信身份、资源边界与 P2P 限制。
- [第三方依赖](THIRD_PARTY.md)：依赖版本与许可证。

## 验证与性能

- [master 生产加固复核](MASTER_HARDENING_2026-10-26.md)：最新 master 边界修复、Release/sanitizer 复测与交替性能对照。
- [生产审查修复与验证](PRODUCTION_REVIEW_FIXES_2026-10-26.md)：R01–R15 修复、投递源安全退休、接口变化与完整测试矩阵。
- [生产审查基线](PRODUCTION_REVIEW_2026-10-26.md)：原始缺陷证据、能力边界与上线前验证要求；含旧基线复现夹具。
- [当前接口与存储格式验证](REMOVE_ADAPTERS_2026-10-25.md)：各阶段源码版本、测试清单及证据，包含严格存储格式验证。
- [实体性能样本](BENCHMARK_CURRENT_ENTITIES_2026-10-26.md)：RPC / Gateway 与 Lua / CAS 分开测量，附三轮原始数据和边界。
- [基准程序](../benchmarks/README.md)：程序职责与复现命令。
- [公共复制优化](COPY_OPTIMIZATION.md)：借用解析、组帧、字节转发与确认版本共享。

本机通过不代表生产容量或跨机故障域已验收；具体限制见项目首页和部署手册。旧报告中的数字、测试名和命令仅对应其标注提交。

## 设计演进与历史记录

以下资料用于了解决策过程，当前可用 API 以本页前半部分的指南为准。

- [实体寻址规划](ENTITY_ADDRESSING_PLAN.md)、[新旧方案分析](ENTITY_ADDRESSING_COMPARISON.md)、[寻址对比样本](BENCHMARK_GLOBAL_ENTITIES_2026-10-25.md)。
- [结构与字符串复制审查](REVIEW_GLOBAL_STRUCTURE_AND_COPIES_2026-10-25.md)及[修复记录](GLOBAL_REVIEW_FIXES_2026-10-25.md)。
- [整体性能审查](PERFORMANCE_REVIEW.md)、[属性性能](ATTRIBUTE_PERFORMANCE.md)、[容器合批性能](CONTAINER_BATCHING_PERFORMANCE.md)。
- [第二轮代码审查](CODE_REVIEW_ROUND2_2026-10-24.md)、[历次验证](VALIDATION.md)。
- [阶段路线图](ROADMAP.md)、[阶段能力表](ENGINE_COMPLETION.md)：保留历史计划与当时的实施状态。
