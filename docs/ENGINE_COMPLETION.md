# 引擎补齐验收

本轮按以下顺序交付；每项以实现和测试为准，不把设计草案计为完成。

| 工作项 | 状态 | 证据/边界 |
| --- | --- | --- |
| 共享类、本地实例、确认副本、执行域 | 已实现 | SHARED_ENTITIES.md；fsnet-shared |
| Lua 客户端属性订阅/写提案、后台任务和定时等待 | 已实现 | ClientScriptHost；fsnet-shared 与 fsnet-shared-process |
| 预测、服务器校正、回滚与效果去重 | 已实现显式输入初版 | PREDICTION.md；独立预测载体、有界历史、显式累计确认与重放；不含自动输入协议或完整世界回滚 |
| 服务端 spawn/restore、自动副本、客户端表现与控制绑定 | 已实现连接内初版 | ENTITY_PRESENCE.md；fsnet-presence、fsnet-presence-process；可见性由服务端脚本显式管理 |
| 类型化结构/固定数组与嵌套赋值 | 已实现 | STRUCTURED_ATTRIBUTES.md；vec3/mat4x4、整值与字段提案/同步、预测与序列化 |
| 资源模块加载、服务端生命周期 hook | 待交付 | 客户端表现 hook 已实现；完整服务端创建/激活/卸载 hook 仍待补齐，保留 mailbox/fencing 合同 |
| 版本迁移与安全热更新 | 待交付 | 必须明确 drain、失败回滚及状态兼容规则 |
| Gateway 身份委托与属性流转发/迁移 | 待交付 | 客户端当前直接连接 owner |
| 加密 P2P、认证信令和 TLS 后备 | 待交付 | ICE/TURN 原语已有；不能以本机测试宣称公网验收 |
| 最终集成示例与故障验证 | 随阶段推进 | 最终汇总普通/sanitizer/独立进程验证结果 |

C++ 只提供机制与载体，玩法留在 Lua；不引入数据库事务依赖。真实公网跨 NAT、跨机存储
复制/仲裁及长期容量需要相应部署环境验证，代码和本机故障测试不能代替这些运行证据。
