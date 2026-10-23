# 传输与身份

Node 的 TLS 依赖由 CMake 从固定 OpenSSL LTS 源码构建并检查 SHA256，不链接机器上的私有 OpenSSL。构建需要 C/C++、CMake、Perl 和 make。系统 openssl 命令只用于测试生成临时证书。

```cpp
fsnet::TlsConfig security{"ca.pem", "service.pem", "service.key", "logic.example.internal"};
fsnet::Node node("", {}, security); // mTLS 身份，不向客户端分发 cluster token
node.authorize([](std::uint64_t peer, const std::string& fingerprint, const std::string& route) {
    return policy_allows(fingerprint, route); // 应用提供身份/权限策略
});
```

TLS 最低版本 1.3，双向验证证书链；出站校验目标证书名称，默认使用目标 IP，也可在 connect 时逐连接指定。CA、证书、私钥必须同时提供；配置错误直接拒绝启动，不降级到明文。私钥只从部署提供的文件读取，不写入仓库。0-RTT 未启用。

`peer_identity(peer)` 和授权回调返回已验证客户端证书的 SHA-256 指纹。授权在路由分发前执行；未通过或策略抛异常均拒绝调用。可信 CA 只证明证书来源，业务角色、账号绑定、吊销与轮换仍由应用/证书管理服务定义。共享 cluster token 可作为内部节点的额外校验；公开客户端使用独立证书与空 token，不能获得集群共享秘密。

无 TLS 时只允许 loopback 监听/连接。跨机器、容器网络或公开监听必须显式提供 TLS。C ABI 提供 fsnet_create_tls/fsnet_authorize。

示例可配置 FSNET_TLS_CA、FSNET_TLS_CERTIFICATE、FSNET_TLS_KEY、FSNET_TLS_PEER_NAME；FSNET_BIND_ADDRESS 默认 127.0.0.1，公开地址需要有效 TLS 配置。示例内部节点仍从 FSNET_CLUSTER_TOKEN 读取额外 token；客户端接入服务可在自身启动器中选择证书认证。

原始 IcePeer 是未加密 UDP 传输原语，不能直接把敏感业务字节发到公网。ICE 描述/候选本身也需要身份校验的信令。TURN 的凭据和配额只控制中继资源，不替代业务消息的加密与授权。
