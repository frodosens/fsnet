# fsnet

C++20 网络核心与 Lua 5.4 脚本宿主。当前版本提供有界 TCP RPC、可替换存储、服务路由与健康探测，以及客户端 ICE/STUN/TURN 互联。

目标边界：C++ 仅实现网络与分布式运行时、场景/实体载体及脚本宿主；游戏业务全部由脚本实现。Lua 承载游戏业务，已提供同实体串行调度和持久化属性载体。阶段计划见 [ROADMAP.md](docs/ROADMAP.md)。

**当前是可运行的重构基础，不是已经验收的生产级 MMO 框架。** 已实现能力、测试和未完成的高可用/安全工作见 [架构与验收](docs/ARCHITECTURE.md)；旧实现的严重缺陷见 [代码审查](docs/REVIEW.md)。旧源码和原始文档保存在 [原始版本](https://github.com/frodosens/fsnet/tree/69a4b62e52ff1874d25f7e64cdb176829f2024be)。

## 构建和测试

需要 C/C++20 编译器、CMake 3.24+、make 或 Ninja。已在 Linux/GCC 14 上验证。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

CMake 按固定版本与 SHA256 下载 Asio、SQLiteCpp（含 SQLite 源码）、libjuice 和 Lua，并从源码编译。应用不依赖 `/usr/local` 中的 libevent、jemalloc、SQLite 等本地应用库；仅保留编译器和操作系统运行库依赖。首次构建需要访问 `codeload.github.com`；后续可复用 build 中的下载缓存。离线构建需预先填充 FetchContent 源码，不自动使用未知系统库。

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFSNET_SANITIZERS=ON
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure
```

Sanitizer 构建检查 C++ 核心、Lua 宿主和原生 C ABI。

## 云环境

当前工作区无 sudo，系统目录只读。可重跑安装脚本会将经过 Debian 签名/包校验的构建工具解包到仓库外，安装固定版本的 CMake，并构建及测试新核心：

```sh
bash tools/cloud-install.sh
. /workspace/fsnet-env/activate-modern
```

复用当前 checkout；每个云任务已隔离，不需要新建 worktree。旧示例的本地诊断产物不属于新框架的运行依赖。

## 可选脚本适配器

默认构建 Lua 宿主；可以关闭脚本宿主，仅构建网络核心：

```sh
# 只构建网络/分布式核心，不下载或编译 Lua。
cmake -S . -B build-core -DFSNET_WITH_LUA=OFF
cmake --build build-core -j4
ctest --test-dir build-core --output-on-failure
```

通用 C ABI 用于原生调用与协议测试。`FSNET_WITH_LUA=OFF` 不生成 `fsnet-lua` / `fsnet-script` 目标。

## Lua 逻辑接口

```lua
local visits = {}
runtime.rpc('player.visit', function(player_id)
    visits[player_id] = (visits[player_id] or 0) + 1
    return tostring(visits[player_id])
end)
```

```sh
# 通过安全环境设置提供 FSNET_CLUSTER_TOKEN，宿主默认只监听 loopback。
./build/fsnet-script examples/lua/player.lua 7001
```

C++ 宿主负责启动、网络和 VM；示例中的计数规则全部在 Lua。Lua RPC 参数与返回值是二进制字符串，业务自己选择编码。`runtime.request(peer, route, payload, timeout_ms)` 等待时让出协程；`runtime.await` 允许显式处理状态码。Lua 原始字节接口通过 C ABI 客户端执行跨进程合同测试。详见 [脚本宿主合同](docs/SCRIPTING.md)。

## 数据库

`fsnet::Storage`（兼容别名 `Database`）定义读取、带版本的 compare_exchange 和条件删除；多记录事务只是可选扩展。Memory 与 SQLite 运行同一合同测试，可通过实现接口替换后端。SQLite 重开恢复已提交数据；版本不在删除后复用。业务必须使用读出的版本，不要假设版本固定递增 1。

存储最低要求是单记录原子 CAS，不要求数据库事务；可以使用 file:/目录 的 POSIX 文件后端。PostgreSQL 是可选后端，使用 FSNET_WITH_POSTGRES=ON 从固定源码构建。详见 [存储合同](docs/STORAGE.md)。

`EntityStore` 提供 owner 转移与 fenced save：将 owner、fencing 和实体状态保存在同一记录，通过单记录 CAS 提交，接管后旧 owner 的写入返回失败，不依赖数据库事务。接管时机由控制平面决定；此接口不把单机 SQLite 变成跨机器数据库集群。

## 服务容错与 P2P

C++ 的 `ClusterClient` 支持静态成员健康探测、指数退避重连、实体路由和幂等调用的一次故障重试。它不提供持久实体 owner 的一致性选举或跨机器数据复制。

`fsnet::IcePeer` 提供 ICE 描述、candidate 交换、UDP 收发、STUN 和 TURN 配置；`TurnRelay` 提供带过期凭据和配额的中继。集成测试只交换 relay candidates 并实际传输数据，另有直连测试。真实跨 NAT 网络仍需部署验收。

```sh
# 通过安全环境配置提供 FSNET_TURN_USER / FSNET_TURN_PASSWORD（至少 16 字节）。
./build/fsnet-relay 127.0.0.1
```

公网部署需设置实际 bind/external address、UDP 3478 和 UDP 49160–49200 的端口规则。TCP 已提供 TLS 1.3 双向证书验证、目标名称验证与路由授权，未加密连接仅允许 loopback。原始 ICE UDP 仍需安全会话层；认证信令与 UDP 禁用时的后备尚待完成。详见 [安全配置](docs/SECURITY.md)。

## 性能测量

```sh
./build/fsnet-benchmark 10000
```

输出一次真实 TCP loopback 的吞吐与 p50/p99。载荷 128 字节、并发 128，两个 Node 在一个线程内推进，不代表公网、脚本逻辑、数据库或真实游戏容量。不得从这个结果推断 MMO 在线人数。

属性同步另有 `fsnet-attribute-benchmark all 20000`（Release、开启 Lua），覆盖结构属性写入、副本增量应用及 1/8/32 个接收者的真实 TCP 同步。
前后对比、原始样本与复现方法见 [属性同步性能](docs/ATTRIBUTE_PERFORMANCE.md)。

## 兼容性

旧 C/libevent 实现、旧 C++ 客户端、Xcode 用户文件、旧构建产物、依赖压缩包和旧示例已从主线移除，通过上述原始版本保留历史。新协议有版本和边界检查，与旧的 `F` 包头协议不兼容。旧逻辑需要迁移到显式服务，旧客户端需要适配新协议或经过专门的隔离兼容网关；不能直接接入新节点。

依赖与许可证见 [THIRD_PARTY.md](docs/THIRD_PARTY.md)。

实体 RPC 权限、Lua 属性声明与原生客户端自动同步见 [ENTITY_SCHEMA.md](docs/ENTITY_SCHEMA.md)。当前同步连接到实体 owner 节点，Gateway 转发与迁移订阅仍在后续阶段。

脚本业务推荐使用 [面向对象的 Lua 实体 API](docs/OBJECT_API.md)，示例为 [attributes.lua](examples/lua/attributes.lua)。

服务端登录创建/恢复实体、自动 LocalPlayer/RemotePlayer 表现、控制目标与移除同步见
[实体存在性与表现](docs/ENTITY_PRESENCE.md)，可运行示例见 [场景客户端](examples/lua/client/player_scene.lua)。

类型化 struct、vec3/mat4x4、固定数组，以及整值/字段赋值自动提交和同步见
[结构属性](docs/STRUCTURED_ATTRIBUTES.md) 与 [变长容器](docs/CONTAINER_ATTRIBUTES.md)。
