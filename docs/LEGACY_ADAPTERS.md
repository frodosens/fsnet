# 显式兼容适配器

默认业务 API 为全局 EntityId + @ + FSE3，见 [当前指南](API_GUIDE.md)。
此页列出原生回归夹具/迁移诊断仍使用的旧入口，不提供历史客户端与线上旧数据并行保证。

| 边界 | 显式兼容入口 |
| --- | --- |
| Lua 任意字符串身份 | Class:legacy_spawn_or_restore、legacy_ref、legacy_reference、legacy_watch |
| Lua 旧账号归属 | owner='legacy_id'；新实体应 bind_owner(ctx.connection) |
| Lua 原始服务/实体注册 | runtime.legacy_rpc、legacy_entity、legacy_entity_rpc、legacy_server_request |
| 原生旧归属 | EntitySchema::legacy_owner_from_id、EntityScriptHost::legacy_ownership |
| 原生旧实体创建 | ScriptHost::legacy_spawn_entity |
| 原生不透明状态夹具 | ScriptHost::bind_legacy_stateful |
| 原生客户端旧寻址 | Client::legacy_call；普通 call 需要全局 ID |
| 原生路由旧寻址 | EntityRouter::legacy_call；普通 call 需要全局 ID |
| owner 的 Type.method | EntityScriptHost::enable_legacy_routes，默认关闭 |
| Gateway 旧转发 | bind_legacy_account、bind_legacy_entity |
| 服务启动器兼容清单 | legacy-account、legacy-entity；普通 entity 行只配置 @ 白名单 |

兼容注册包装器显式开启旧 owner 路由；新 fsnet.Entity 类的内部注册不开启它。
旧 subject-only 委托及 FER1/2/3、FSE1/2 codec 保留在原生适配边界；新业务不使用它们生成身份。
FSS1 是内部 Lua 状态边界，不是业务的实体寻址协议，仍由公共宿主使用。
system.health、属性控制、交接和可靠投递控制协议也不属于待实体化的业务 login 路由。

这些接口仍受角色/owner/容量/版本校验约束，不能用于绕过新实体的权限。
兼容夹具位于 tests/fixtures 和 tests 的 native fixtures；不再作为 examples 的默认接入方式。
C++ 成员/签名及 Lua 名称改变要求嵌入方重编并迁移，不能混用旧头文件和新共享库。
