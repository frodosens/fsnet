# 客户端SDK示例

先看 [客户端SDK](../docs/CLIENT_SDK.md)。此目录保留应用接入示例；性能程序在
[benchmarks](../benchmarks/README.md)，兼容脚本断言在 [tests/fixtures](../tests/fixtures/README.md)。

## 一键运行

从仓库根目录：

```sh
bash tools/dev/bootstrap.sh
. tools/dev/activate.sh
cmake --preset debug
cmake --build --preset debug
python3 examples/run.py scene
python3 examples/run.py scene --native
python3 examples/run.py attributes
```

其他构建位置使用 `--build /path/to/build`。启动器自动组合共享脚本，启动临时loopback服务端
和客户端，退出后清理进程/脚本；不再需要手工cat多个文件。
演示固定认证账号alice，使用临时token和内存存储，不继承生产TLS/存储设置。
它是单账号本机演示；真实账号由证书/可信登录系统映射，不能用展示名称替代认证。

## 目录与职责

| 位置 | 内容 |
| --- | --- |
| [client/main.cpp](client/main.cpp) | Lua客户端宿主：Client连接 → script_host → LuaEngine → 每帧poll |
| [client/native.cpp](client/native.cpp) | 无Lua客户端：连接/登录、Local/Remote表现、镜头控制、确认快照、实体RPC、登出 |
| [server/main.cpp](server/main.cpp) | fsnet-script开发宿主；通用机制配置，玩法全部来自Lua |
| [server/service.cpp](server/service.cpp) | fsnet-service生产接入启动器：mTLS、账号委托、目录、指标和drain |
| [scene/shared.lua](scene/shared.lua) | 共享Player类型、vec3位置、创建/激活、输入确认和可重放移动规则 |
| [scene/server.lua](scene/server.lua) | 登录时spawn/restore、show/possess、同owner内可见性和登出 |
| [scene/client.lua](scene/client.lua) | 自动LocalPlayer/RemotePlayer、镜头、预测/校正、效果去重、三次移动 |
| [attributes/shared.lua](attributes/shared.lua) | vec3、mat4x4、嵌套结构、list/map/set声明与服务器校验 |
| [attributes/client.lua](attributes/client.lua) | 整值/字段赋值、batch、拒写回滚、容器新增/修改/删除 |

scene的Lua客户端打印预测/确认位置0→2→5→4，每个输入只播放一次footstep。
服务端一直使用同一个Player类型；LocalPlayer/RemotePlayer只决定客户端表现。
原生客户端不重复实现Lua移动/预测规则，演示通用SDK和渲染适配入口。
镜头/效果用字段和打印代替图形引擎调用，可以替换成自己的平台适配器。

attributes示例只允许客户端修改position/transform/pose；背包、任务、标签由服务器提交，
客户端只读。sample RPC是用于展示同步的演示动作，不是可直接用于生产的奖励接口。

## 嵌入自己的客户端

```cmake
find_package(fsnet CONFIG REQUIRED COMPONENTS client lua)
target_link_libraries(game PRIVATE fsnet::client fsnet::lua)
```

CMake链接 `fsnet::client`，需要Lua时再链接 `fsnet::lua`。应用创建Client、配置mTLS endpoint，
将 `client.script_host()` 交给LuaEngine，每帧推进Client和LuaEngine即可。不要自行重复创建
第二个AttributeClient，也不要把未确认的预测位置写回确认副本。
原生句柄包含连接generation，断线后需要重新登录/建立投影；SDK不自动重发不确定结果的RPC。

生产启动方式和证书/路由清单见 [部署手册](../docs/PRODUCTION.md)。SDK可连接owner或Gateway，
同一连接上的owner迁移由Gateway恢复订阅。scene中的online表只演示单owner内可见性，不是
跨节点持久场景或自动AOI；断线会话回收和多设备登录还需要应用策略，示例不宣称已经实现这些业务能力。
## 全局实体示例

`entities/shared.lua`、`entities/server.lua` 和 `entities/client.lua` 展示
Manager → LoginEntity → Player → 地图 Node 的新模型。入口配置保存 Manager 的 EntityId；
客户端通过 `fsnet.ref(id)` 调用，服务端通过 `Class:spawn` 自动分配全局 ID。
创建、迁移、账号归属与稳定 ID 的合同及两节点运行命令见 [实施文档](../docs/GLOBAL_ENTITIES.md)。
