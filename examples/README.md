# 示例入口

当前设计见 [架构](../docs/ARCHITECTURE.md)，端到端接口组合见 [API 使用指南](../docs/API_GUIDE.md)。

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

## 客户端预测与服务器校正

同一份实体类由两个宿主加载；客户端先预测两个输入，服务器施加输入上限并同步累计
确认序号，客户端重放剩余输入。每个首次输入交付一次效果，校正不重复播放。

```sh
cat examples/lua/shared/predicted_actor.lua examples/lua/predicted_actor.lua > /tmp/fsnet-prediction-server.lua
FSNET_CLUSTER_TOKEN=local-demo FSNET_DEMO_SUBJECT=alice /path/to/build/fsnet-script /tmp/fsnet-prediction-server.lua 7001

# 另一终端：
cat examples/lua/shared/predicted_actor.lua examples/lua/client/predicted_actor.lua > /tmp/fsnet-prediction-client.lua
FSNET_CLUSTER_TOKEN=local-demo /path/to/build/fsnet-client /tmp/fsnet-prediction-client.lua 127.0.0.1 7001
```

完整 API、输入确认与确定性限制见 [客户端预测](../docs/PREDICTION.md)。

## 登录后自动创建 LocalPlayer / RemotePlayer

服务端统一注册 Player，客户端按归属选择表现 prototype。服务端场景脚本负责把在线玩家
show 给彼此；进入/离开同步会自动调用表现生命周期，不需要客户端逐个 watch。

```sh
cat examples/lua/shared/player_entity.lua examples/lua/player_scene.lua > /tmp/fsnet-scene-server.lua
FSNET_CLUSTER_TOKEN=local-demo FSNET_DEMO_SUBJECT=alice /path/to/build/fsnet-script /tmp/fsnet-scene-server.lua 7001

# 另一终端：
cat examples/lua/shared/player_entity.lua examples/lua/client/player_scene.lua > /tmp/fsnet-scene-client.lua
FSNET_CLUSTER_TOKEN=local-demo /path/to/build/fsnet-client /tmp/fsnet-scene-client.lua 127.0.0.1 7001
```

单客户端示例打印 LocalPlayer、镜头目标，依次模拟三次移动（+2、+3、-1），
展示每次发送前的预测位置与确认位置、服务器确认后的位置（2、5、4），再注销并移除。
客户端 `LocalPlayer:move(delta)` 是普通业务方法，demo 在异步任务中依次调用并等待每次确认；
实时输入循环和不确定 RPC 结果的重试策略由业务另外实现。
双账号互相看到 RemotePlayer 的真实 Node 验证见 `fsnet-presence`；上面的 loopback
演示固定 subject=alice，不能用它模拟两个不同登录账号。生产宿主需提供可信账号映射。
镜头/模型绑定在示例中仅用字段和打印展示，实际图形调用由引擎适配层提供。
完整接口与撤销/预算边界见 [ENTITY_PRESENCE.md](../docs/ENTITY_PRESENCE.md)。

## vec3 / mat4x4 与嵌套结构自动同步

```sh
cat examples/lua/shared/structured_entity.lua examples/lua/structured_entity.lua > /tmp/fsnet-struct-server.lua
FSNET_CLUSTER_TOKEN=local-demo FSNET_DEMO_SUBJECT=alice /path/to/build/fsnet-script /tmp/fsnet-struct-server.lua 7001

# 另一终端：
cat examples/lua/shared/structured_entity.lua examples/lua/client/structured_entity.lua > /tmp/fsnet-struct-client.lua
FSNET_CLUSTER_TOKEN=local-demo /path/to/build/fsnet-client /tmp/fsnet-struct-client.lua 127.0.0.1 7001
```

示例依次整值写 position、修改 position.x、矩阵行/元素及嵌套 pose 字段，隐式提交服务器
校验后读取确认值；随后展示被服务器拒绝的写入，以及四次字段赋值一次 batch 提交。见 [结构属性](../docs/STRUCTURED_ATTRIBUTES.md)。


## 变长背包与任务

共享声明、服务端增删与客户端新增/修改/删除通知见
[容器属性示例](../docs/CONTAINER_ATTRIBUTES.md#可运行示例与验证)。支持 list、字符串键 map 和标量 set，
服务端成功提交后自动更新获准客户端副本。
