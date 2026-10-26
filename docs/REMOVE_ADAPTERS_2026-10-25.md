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
2026-10-26 继续取消历史存档兼容：实体记录仅支持 FER5，计时器发现索引仅支持 FTI2。

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

## 补查后的收口

- Gateway raw watch 在准入前拒绝非规范 ID；目录查询必须找到带全局元数据的记录，不再回退旧 key。
- Router 始终按目录中的权威类型选 Node；删除 typed-route 条件分支。
- 删除 `Database = Storage` 类型别名，存储与调度边界统一使用 Storage；现有数据库工厂仍是当前后端 API。
- 删除启动器的旧 SQLite 环境变量回退，配置统一为 FSNET_STORAGE_URI。
- 请求基准也改为 Catalog 分配的 ID 和 @/FSE3。它测量 native echo/转发，不执行每请求 Lua 或持久提交。

补查还确认了取消清理风险：挂起超时后若直接 resetthread，用户 __close 的 pcall 循环可吞掉
普通预算 hook 的错误。现在清理前安装 CALL/COUNT(1) 的专用失败 hook，在脚本 close 第一条指令前终止。
正常返回的 close 按 Lua 语义执行；超时/取消不执行用户清理。回归同时验证回滚、OOM 恢复和后续请求。

## 接口删除阶段验证（历史存档收口前）

引擎源码 f5fad66 的全配置结果：

| 配置 | CTest 结果 |
| --- | --- |
| Debug / Lua ON | 26/26 |
| Release / Lua ON | 26/26 |
| ASan + UBSan | 17/17 |
| Lua OFF core | 14/14 |
| PostgreSQL 专项 | 1/1 |

随后 181ec93 只清理启动器配置及相应脚本环境过滤；受影响的安装包/示例/计时器进程在
Debug、Release 各 6/6 通过，PostgreSQL 专项 1/1 再通过。
[测试清单与二进制哈希](validation/remove-adapters-tests-2026-10-26.json) 保存了每项名称和耗时。
这不是把初次删除接口的结果当作最终补查结果；两轮均实际运行。

2026-10-26 另做 Release 一次预热、三轮串行测量，构建/测试期间不混跑：

- [完整引擎基准原始样本](benchmarks/global-api-removal-2026-10-26.json)：f5fad66，含平台、CPU 配额、二进制哈希。
- [所有权/路由/计时器诊断](validation/global-api-removal-2026-10-26.json)：f5fad66，包含连接重登保持相同 PlayerId。
- [全局实体 Lua/CAS/Gateway 样本](benchmarks/global-flow-current-2026-10-26.json)：181ec93，三轮独立进程。
- [测量摘要与范围](BENCHMARK_CURRENT_ENTITIES_2026-10-26.md)。

旧对照程序已删除，改写前后的不同夹具不能直接用于推算优化百分比。
本机检查不代表生产环境、跨机故障域、公网延迟或容量已验收。

## 当前格式边界

用户已明确不兼容旧数据。实体记录只读写 FER5，必须携带有效的全局元数据；
FER1/2/3/4 和未知版本均拒绝。实体 key 只由规范 EntityId 生成，不再回退到类型加字符串 ID 的旧 key。
计时器发现索引只读写 FTI2，FTI1 与未知版本拒绝；读取失败不会把旧记录当作空值重置或覆盖。
没有旧存档迁移、自动转换或降级读端。旧实验存储不再受支持，部署使用当前格式数据或空存储。

FSE1/2/FSS1 仍用于引擎内部边界；FSP1/2/3 按快照是否携带投递/计时器状态选择，均是当前编码，
不是历史存档适配器。本次保留这些有效编码的现有布局，客户端/服务端继续共用相同编解码实现。
上方测试和基准对应各自标注的既有提交；历史存档收口后的新验证由本阶段结果单独记录。

## 历史存档读端删除验证（2026-10-26）

生效源码 `b00a3b3`。这轮结果与上面的接口删除阶段分别记录；没有再次运行性能基准，
上方性能数据仍只对应其标注提交。

| 配置 | 结果 |
| --- | --- |
| Debug / Lua ON | 26/26 |
| Release / Lua ON | 26/26 |
| ASan + UBSan | 17/17 |
| Lua OFF core | 14/14 |
| PostgreSQL 专项 | 1/1 |

[测试清单、耗时和二进制哈希](validation/storage-format-tests-2026-10-26.json)。
新增断言使用完整历史 FER1/2/3 布局，验证完整/借用读端都拒绝；同时拒绝 FER4 和未知版本，
缺少全局元数据不能编码。FTI1/未知索引在读取、生产与发现入口均报错，原字节和存储版本不变；
真实实体宿主遇旧记录也不会重置或覆盖它。

当前 FSP1/2/3 的合法往返、截断/尾字节拒绝仍有覆盖；全量测试继续验证全局创建、生命周期、
fencing、客户端订阅/预测、定向迁移、投递 ACK 丢失恢复及持久计时器重启恢复。
源代码 `include/`、`src/` 已没有 FER1/2/3/4、FTI1 解码或旧实体 key 回退分支。
