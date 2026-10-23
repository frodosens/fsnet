# 示例入口

| 文件 | 状态与用途 |
| --- | --- |
| [lua/attributes.lua](lua/attributes.lua) | 推荐实体 API：属性权限、同步范围、客户端写入校验、显式 RPC |
| [lua/shared/magic_circle.lua](lua/shared/magic_circle.lua) | 客户端与服务端共用的纯 Lua 魔法阵规则 |
| [lua/shared/circle_entity.lua](lua/shared/circle_entity.lua) | 客户端/服务端同一份实体类、属性、执行位置和 RPC 声明 |
| [lua/magic_circle.lua](lua/magic_circle.lua) | 服务端注册共享类 |
| [lua/client/replicated_circle.lua](lua/client/replicated_circle.lua) | 真正的 Lua 本地实例与自动同步副本 |
| [lua/client/magic_circle.lua](lua/client/magic_circle.lua) | 纯客户端本地执行与基于确认属性的查询，无引擎依赖 |
| [lua/player.lua](lua/player.lua) | 旧的无状态 RPC/内存示例，不是持久化实体设计 |
| [lua/stateful.lua](lua/stateful.lua) | 底层不透明状态兼容示例，也是恢复测试的夹具 |

## 共用魔法阵逻辑

从仓库根目录运行。脚本沙箱没有 `require`/文件加载能力，因此示例在启动前合并，
共享规则文件不依赖 `fsnet`，可以原样交给客户端 Lua VM：

```sh
cat examples/lua/shared/magic_circle.lua examples/lua/client/magic_circle.lua > /tmp/fsnet-circle-client.lua
lua /tmp/fsnet-circle-client.lua

cat examples/lua/shared/magic_circle.lua examples/lua/shared/circle_entity.lua examples/lua/magic_circle.lua > /tmp/fsnet-circle-server.lua
FSNET_CLUSTER_TOKEN=local-demo FSNET_DEMO_SUBJECT=alice /path/to/build/fsnet-script /tmp/fsnet-circle-server.lua 7001

# 另一终端：同一份实体类，由服务端 tick 驱动、客户端自动收取属性。
cat examples/lua/shared/magic_circle.lua examples/lua/shared/circle_entity.lua examples/lua/client/replicated_circle.lua > /tmp/fsnet-circle-client-entity.lua
FSNET_CLUSTER_TOKEN=local-demo /path/to/build/fsnet-client /tmp/fsnet-circle-client-entity.lua 127.0.0.1 7001
```

客户端示例包含断言，成功时无输出。`lua` 指客户端或开发机的 Lua 解释器，并非服务端
运行依赖。服务端示例使用已内置的 `fsnet.Entity`；收到实体请求后才激活并执行 tick，
单纯加载脚本不会生成魔法阵。示例的 `persist=false` 属性在重新激活时恢复默认值。

共享类通过 `:define()` 声明模型；服务端 `:register()` 导出 RPC，客户端 `:spawn_local()`
执行本地规则或 `:watch()` 获取只读确认副本。同步不会重复执行玩法。启动器已安装原生
属性服务和 Lua 客户端绑定。预测校正另有独立合同，不能直接对确认副本执行 advance。
详细 API 见 [共享实体](../docs/SHARED_ENTITIES.md)。

示例启动器仅用于本地演示。生产接入需要可信登录身份映射，不能把 cluster token 当玩家
身份；`owner='id'` 要求认证 subject 与实体 ID 一致。
