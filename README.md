# fsnet

C++20 网络核心、Lua 5.4 脚本宿主与可选 Ruby 逻辑接口。当前版本提供有界 TCP RPC、可替换存储、服务路由与健康探测，以及客户端 ICE/STUN/TURN 互联。

目标边界：C++ 仅实现网络与分布式运行时、场景/实体载体及脚本宿主；游戏业务全部由脚本实现。Lua 是首选嵌入式适配器，Ruby 是可选外部适配器。场景/实体的调度载体尚待后续阶段实现。阶段计划见 [ROADMAP.md](docs/ROADMAP.md)。

**当前是可运行的重构基础，不是已经验收的生产级 MMO 框架。** 已实现能力、测试和未完成的高可用/安全工作见 [架构与验收](docs/ARCHITECTURE.md)；旧实现的严重缺陷见 [代码审查](docs/REVIEW.md)。旧源码和原始文档保存在 [归档标签 archive/legacy-c-ruby21](https://github.com/frodosens/fsnet/tree/archive/legacy-c-ruby21)。

## 构建和测试

需要 C/C++20 编译器、CMake 3.24+、make 或 Ninja；Ruby 逻辑与测试需要 Ruby 3.2+（验证版本 3.3.8，需 JSON/Fiddle 标准库）。已在 Linux/GCC 14 上验证。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
ctest --test-dir build --output-on-failure
```

CMake 按固定版本与 SHA256 下载 Asio、SQLiteCpp（含 SQLite 源码）、libjuice 和 Lua，并从源码编译。应用不依赖 `/usr/local` 中的 libevent、jemalloc、SQLite 或 Ruby 2.1；仅保留编译器和操作系统运行库依赖。首次构建需要访问 `codeload.github.com`；后续可复用 build 中的下载缓存。离线构建需预先填充 FetchContent 源码，不自动使用未知系统库。

如果 CMake 找到 Ruby 3.2+，CTest 会包含 Ruby 测试；否则只运行 C++ 测试。可用 `-DFSNET_RUBY_EXECUTABLE=/path/to/ruby` 指定解释器。

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DFSNET_SANITIZERS=ON
cmake --build build-asan -j4
ctest --test-dir build-asan --output-on-failure
```

Sanitizer 构建检查 C++/C 核心；Ruby/Fiddle 在普通构建中单独验证，避免将 ASan 库直接加载进未插桩的 Ruby 进程。

## 云环境

当前工作区无 sudo，系统目录只读。可重跑安装脚本会将经过 Debian 签名/包校验的 Ruby 依赖解包到仓库外，安装固定版本的 CMake，并构建及测试新核心：

```sh
bash tools/cloud-install.sh
. /workspace/fsnet-env/activate-modern
```

复用当前 checkout；每个云任务已隔离，不需要新建 worktree。旧 Ruby 2.1 和旧示例的本地诊断产物不属于新框架的运行依赖。

## 可选脚本适配器

默认构建 Lua 宿主；Ruby 适配器不嵌入 Ruby VM，不给核心引入 Ruby 链接依赖。可以分别关闭它们：

```sh
# 只构建网络/分布式核心，不下载或编译 Lua，也不发现 Ruby。
cmake -S . -B build-core -DFSNET_WITH_LUA=OFF -DFSNET_WITH_RUBY=OFF
cmake --build build-core -j4
ctest --test-dir build-core --output-on-failure
```

`FSNET_WITH_RUBY=OFF` 关闭 Ruby 自动发现与适配器测试；通用 C ABI 仍保留，供其他语言使用。`FSNET_WITH_LUA=OFF` 不生成 `fsnet-lua` / `fsnet-script` 目标。

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

C++ 宿主负责启动、网络和 VM；示例中的计数规则全部在 Lua。Lua RPC 参数与返回值是二进制字符串，业务自己选择编码。`runtime.request(peer, route, payload, timeout_ms)` 等待时让出协程；`runtime.await` 允许显式处理状态码。Lua 与 Ruby 的原始字节接口运行同一组跨进程合同测试。详见 [脚本宿主合同](docs/SCRIPTING.md)。

## Ruby 逻辑接口

```ruby
require_relative 'ruby/lib/fsnet'

class PlayerService < FSNet::Service
  def initialize(database)
    @database = database
  end

  rpc :get do |request|
    record = @database.get("player:#{request.fetch('id')}")
    record && {player: record.value, version: record.version}
  end
end

node = FSNet::Node.new(token: ENV.fetch('FSNET_CLUSTER_TOKEN'))
node.mount('player', PlayerService.new(FSNet::Database.new(path: 'players.db')))
node.listen(host: '127.0.0.1', port: 7001)
node.run
```

```sh
# 在环境设置中配置 FSNET_CLUSTER_TOKEN；不将真实凭据写入仓库。
FSNET_LIBRARY="$PWD/build/libfsnet.so" ruby examples/player.rb
```

只有显式注册的方法可通过网络调用。Fiber 内可用 `node.request(peer, 'profile.get', params, timeout: 1000)` 等待另一个服务；Fiber 让出期间，poll 线程继续处理其他请求。普通调用使用 `node.call(...) { |value, error| ... }`。Node 的方法在所属线程调用；数据库句柄支持线程间串行访问，但同步数据库操作会阻塞该线程，较重业务需单独的执行器。

## 数据库

`fsnet::Database` 定义读取、带版本的 compare_exchange、条件删除、多记录原子事务。Memory 与 SQLite 运行同一合同测试，可通过实现接口替换后端。SQLite 重开恢复已提交数据；版本不在删除后复用。业务必须使用读出的版本，不要假设版本固定递增 1。

Ruby 的 `database.save(key, value, version: observed_version)` 在版本冲突时返回 false。默认 version=0 仅表示创建。示例覆盖玩家读取和写入。存储最低要求是单记录原子 CAS，不要求数据库事务；可以使用 file:/目录 的 POSIX 文件后端。PostgreSQL 驱动仍在接入中。详见 [存储合同](docs/STORAGE.md)。

`EntityStore` 在 C++ 和 Ruby 中提供 owner 转移与 fenced save：将 owner、fencing 和实体状态保存在同一记录，通过单记录 CAS 提交，接管后旧 owner 的写入返回失败，不依赖数据库事务。接管时机由控制平面决定；此接口不把单机 SQLite 变成跨机器数据库集群。

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

输出一次真实 TCP loopback 的吞吐与 p50/p99。载荷 128 字节、并发 128，两个 Node 在一个线程内推进，不代表公网、Ruby、数据库或真实游戏容量。不得从这个结果推断 MMO 在线人数。

## 兼容性

旧 C/libevent/Ruby 2.1 实现、旧 C++ 客户端、Xcode 用户文件、旧构建产物、依赖压缩包和旧示例已从主线移除，通过上述归档标签保留历史。新协议有版本和边界检查，与旧的 `F` 包头协议不兼容。旧逻辑需要迁移到显式服务，旧客户端需要适配新协议或经过专门的隔离兼容网关；不能直接接入新节点。

依赖与许可证见 [THIRD_PARTY.md](docs/THIRD_PARTY.md)。
