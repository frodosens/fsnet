# 客户端 SDK

C++入口为 `fsnet/client.hpp`，CMake目标为 `fsnet::client`；Lua集成使用同一Client的script_host，
复用已有实体、差量、权限、确认版本和预测引擎。SDK不内置登录业务、商店或角色移动规则。配置的服务器连接可调用server-only客户端RPC；
角色来自已认证的固定连接，额外脚本连接不会自动取得该权限。

## 安装接入

```cmake
find_package(fsnet CONFIG REQUIRED COMPONENTS client)
target_link_libraries(game PRIVATE fsnet::client)
# 使用内置Lua适配器时：
# find_package(fsnet CONFIG REQUIRED COMPONENTS client lua)
# target_link_libraries(game PRIVATE fsnet::client fsnet::lua)
```

`cmake --install` / TGZ提供头文件、共享核心库、可选Lua静态库及可重定位CMake配置。
消费者需要兼容的C++20编译器/系统运行库；C++头文件和库必须来自同一次构建，不是跨编译器ABI。

## 连接和请求

```cpp
fsnet::ClientOptions options;
options.tls = {"ca.pem", "client.pem", "client.key", "gateway.example"};
fsnet::Client client(std::move(options));
fsnet::EntityId login_hub(configured_login_id); // 可信服务发现/启动配置提供
client.on_event([&](const fsnet::ClientEvent& event) {
    if (event.kind == fsnet::ClientEvent::Kind::connected) {
        client.call(login_hub, "login", "display name", [](fsnet::Result result) {
            // 服务器根据可信认证上下文选择账号；display name不决定账号或角色。
        });
    }
});
client.connect({"127.0.0.1", 7000, "gateway.example"});
// 在游戏主循环中调用client.poll()；没有SDK内部忙等或专用网络线程。
```

ClientState为disconnected/connecting/connected。connected表示Node传输认证/握手完成，
不等同于业务登录完成。可在connecting时提交请求，沿用Node有界队列与超时。
远端连接必须使用mTLS；明文仅允许loopback。Endpoint当前接收数字IP，server_name只控制
证书名验证，不执行DNS解析。token/TLS策略与Node相同，不把cluster token视为玩家身份。

`request(route, bytes, reply, timeout)` 是底层控制/兼容 RPC；业务调用使用实体。`call(EntityKey, method, bytes, reply, options)`
是共用FSE实体调用，options支持timeout/request_id。超时结果可能不确定，SDK不会自动重放。
安全重试须保留同一request_id和完全相同输入，并遵守服务端持久回执保留窗口。

## 自动实体与确认属性

```cpp
client.on_event([&](const fsnet::ClientEvent& event) {
    if (event.kind == fsnet::ClientEvent::Kind::enter && event.automatic) {
        auto entity = client.entity(event.handle);
        // 根据服务器owned元数据选择LocalPlayer/RemotePlayer表现。
        // entity->confirmed是不可变确认版本，可保留到后续帧。
    }
    if (event.kind == fsnet::ClientEvent::Kind::control) {
        auto target = client.controlled(); // 绑定/解除镜头与输入
    }
    if (event.kind == fsnet::ClientEvent::Kind::changed) {
        auto entity = client.entity(event.handle); // 当前确认值；不再次执行玩法
    }
});
```

enter/changed/control/leave事件与连接事件都在所属线程处理。SDK只持有有界实体元数据，
确认属性直接引用AttributeReplica共享版本；事件本身不复制整份属性。leave后entity(handle)
返回空，需要离场动画的应用可提前持有旧confirmed快照。

`ClientEntityHandle`包含连接generation和订阅ID；断线/重连后旧句柄不能写入新副本。
`entity(EntityKey)`优先返回自动副本；`entities()`返回当前视图；`controlled()`返回服务器控制目标。
手动watch不携带owner/controlled元数据，因此owned为nullopt；不要靠手动watch推断角色归属。

```cpp
client.watch({"Profile", saved_profile_id}, [&](fsnet::Status status, fsnet::ClientEntityHandle handle) {
    if (status != fsnet::Status::ok) return;
    auto image = client.entity(handle);
    client.write(handle, {{"position", fsnet::AttributeStruct{
        {"x", 1.0}, {"y", 0.0}, {"z", 0.0}}}}, [](fsnet::Result result) {});
});
```

write沿用公共客户端差量生成、服务器权限/类型/业务校验和确认后同步；不会直接修改确认副本。
unwatch释放本地订阅，不等同于销毁服务器实体。Native SDK不另造Vec3/容器/预测值模型。

## Lua和游戏引擎主循环

```cpp
client.connect(endpoint);
fsnet::LuaEngine scripts(client.script_host());
scripts.load(shared_source + client_source, "client");
// 每帧：
client.poll();
scripts.poll();
// 结束：
scripts.stop();
client.disconnect();
```

Lua使用 `fsnet.ref(login_hub_id):call('login',...)`、实体引用、`fsnet.entities`、字段/容器赋值、batch和
显式预测输入确认。Native与Lua读取同一同步投影，没有并行差量/回放实现。
一条连接只接一个Lua事件消费者；在poll之外取得script_host，可为已有自动实体补发初始表现事件。
断线后旧宿主停止并释放其路由与额外连接；旧宿主不能解绑新会话的同名路由。重新连接必须先停止旧LuaEngine，再取得新host并重载脚本。
SDK不自动重发未确认输入，也不在Native层重复实现Lua预测算法。

## 回调、断线和资源

请求/写入回调在拥有者线程执行，参数拒绝或断线可同步完成。on_event不应阻塞；回调异常会
记录last_error并请求以internal断开。事件引用只在回调期间有效，需要保留时复制元数据。
禁止递归poll。回调中disconnect/reconnect被推迟到当前网络poll/清理完成后，避免旧会话回调
访问新适配器。connect仍需先断开当前连接；回调内可disconnect后connect或在disconnected事件
安排重连。销毁Client抑制之后的应用回调；显式disconnect会完成挂起请求并发布移除/断线事件。

stats包含连接状态、generation、当前实体数和Node排队/连接计数。握手默认5秒、请求默认3秒，
都可配置；Node/AttributeClient原有帧、请求、订阅和内存预算继续生效。
示例见 [examples](../examples/README.md)。当前不含图形引擎、Unity/C#/浏览器绑定、DNS、
自动重连登录、持久场景或资源热更新；这些边界不能由客户端自行绕过服务器权限。
