# 跨节点客户端接入

客户端连接 Gateway。Gateway 使用共享 CAS 目录中的权威实体类型、placement、owner/租约
选择 Node，转发实体 RPC、属性提案、订阅与存在性消息。账号映射属于业务，
实体授权来自原始逻辑连接 ConnectionRef，与每跳物理 peer 分开。

## 绑定与信任

```cpp
// front/back 必须为不同 Node，TLS 与身份解析器由宿主配置。
fsnet::ClientGateway gateway(front, back, storage, endpoints);
gateway.bind_entity("Manager", {"discover"});
gateway.bind_entity("LoginEntity", {"login"});
gateway.bind_entity("Player", {"move"});
```

bind_entity 配置 @ 的方法白名单，按目录的权威类型检查；不开放 Type.method。
未配置的方法默认拒绝，payload 的 expected_type 不能替代权威类型。
白名单只决定可转发入口，owner 再验证 schema 的 caller、owner、属性权限和业务规则。
server-only 方法即使被加入转发表，客户端仍不能调用。

owner 通过 GatewayDelegation 接受可信 Gateway 的 FSCN1 身份委托，
保留原 ConnectionRef，固定为 client 角色。Node 的信任关系来自 mTLS、证书 pins 和可信解析器，
不能根据客户端 payload 自称 server 或声明连接归属。Gateway 为客户端会话到各 owner 维护
独立后端链路；原始 peer 只在本 Node 有效，不跨节点当作身份。

服务端可在远端创建 Player 后调用 show_to(ctx.connection) / possess(ctx.connection)，
通过已验证的 Gateway 链路把呈现操作转交实际承载节点；这些控制入口不暴露给前端客户端。
一个逻辑连接可以拥有多个实体；断线会使旧调用上下文失效，重连产生新连接，业务验证后显式 bind_owner。

## 路由与迁移

客户端从可信启动/服务发现取得 ManagerId，实体 discover 返回 LoginEntityId，
login 返回 PlayerId。后续 RPC 只含实体 ID 和方法，不要求客户端保存 Node 地址。
Gateway 根据权威目录路由，指定目标迁移后恢复订阅和控制状态，保留原客户端句柄。
重复恢复用最终快照/控制确认结束，不依赖客户端一定收到某个中间通知。

属性和状态校验使用同一公共 replication/patch 引擎；Gateway 不另造一份可变业务副本。
身份换绑后旧投影会被撤销；外部观察者/AOI 的恢复须提供明确的 Restore 策略。
普通输入 RPC 不承诺在不确定结果下自动重发；持久实体命令使用 outbox/inbox。

## 资源与交付

客户端、链接、订阅、请求、排队字节、迁移时限都有显式上限。可观察 pending、lookup/link 等待者、
迁移和转发失败计数。排空后拒绝新请求，已进入底层存储的提交仍遵守原持久合同。
多机存储必须提供相同的 CAS/fence 语义，不能把本地 SQLite 或文件复制当成共享目录。

启动器的 entity 清单使用同一白名单；legacy-account、legacy-entity 和 bind_legacy_* 仅用于
显式兼容夹具，不与新业务入口混称。见 [兼容适配器](LEGACY_ADAPTERS.md)、
[部署](PRODUCTION.md)、[全局实体](GLOBAL_ENTITIES.md) 与 [修复记录](GLOBAL_REVIEW_FIXES_2026-10-25.md)。
