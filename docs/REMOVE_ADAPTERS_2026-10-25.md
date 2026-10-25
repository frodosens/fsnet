# 统一实体接口：删除适配层

工作分支：refactor/global-entities。此次按用户要求直接删除旧 API 和实现分支，不提供别名或迁移包装。

## 删除范围

- Lua 无状态业务注册与任意字符串 ID 创建/调用、账号 owner 映射。
- ScriptHost、EntityScriptHost、Router、SDK、Gateway 的旧入口和启用开关。
- 独立 login/account 路由、Type.method 业务转发、Gateway 的旧清单类型。
- 闲置 EntityStore、双 key 离线迁移工具、适配层说明与依赖旧 API 的夹具/性能对照工具。
- 旧一键验收工具及安装入口；当前生产手册明确说明分项验证和缺少的整合验收。

业务 API 统一为 Catalog 分配的全局 EntityId、spawn/ref/watch、ConnectionRef 授权与 @ 分发。
Node 的控制路由、共享属性/patch 编解码、可靠投递和持久计时器仍属于当前机制。
本轮没有改写现有磁盘与线协议格式；接口删除不表示已经删除全部历史格式解码器。

## 测试迁移

| 当前测试 | 核心断言 |
| --- | --- |
| current-runtime | Catalog 创建重试、连接权限、角色、回执/冲突、回滚、挂起撤权、生命周期、失去 fence |
| current-lua-limits | 公开接口约束、沙箱、不可吞掉的指令上限、OOM 回滚和后续健康请求恢复 |
| current-prediction | 规范 ID 订阅、预测/确认分离、校正重放、失败原子性、网络/等待/打印限制 |
| current-delivery/process | outbox 回滚、角色、inbox 去重；接收提交后 ACK 前退出，重启不重复效果 |
| client-sdk/connections/global-flow | 连接代数、失效写入、真实 Gateway、远程创建、迁移、投递、计时器 |
| timer-process/postgres | 真实启动器 SIGKILL、稳定 ID、延迟/周期/取消恢复；PG 同时验证 TLS 与重连 |
| schema/patches/storage/files | 权限、共同值/差量、非法输入、容量、原子应用与持久 CAS |

测试数量变化源于删除旧模型的重复入口并重写关键断言，不把数量视为覆盖率。
历史审查/性能结果只对应原提交；旧程序可从 Git 历史查阅，不再作为当前依赖。

## 验证

Debug 全量 26/26 通过，包含安装消费者、四种交付示例、投递/计时器进程重启。
Release、ASan、core 与 PostgreSQL 验证在下一阶段补充。当前文档不宣称生产环境已验收。
