# 桌面客户端二进制 SDK

桌面 SDK 面向不持有 fsnet 实现源码的引擎插件开发者。SDK 包包含公共头文件、预编译核心库、
可重定位 CMake 导出目标、构建 manifest 与依赖许可证。Godot 插件通过 `fsnet::client` 链接，
不需要 Lua 或 PostgreSQL，也不会在插件构建时下载/编译 fsnet 实现。

## 目标与构建

发行目标为 Linux x86_64、Windows x86_64（MSYS2 MinGW-w64）、macOS x86_64 和 arm64。
`.github/workflows/desktop-sdk.yml` 在各目标原生 runner 上构建并实际运行外部 SDK 消费者。
macOS Intel 使用 `macos-15-intel`，arm64 使用 `macos-15`；二者分别验证，不将交叉编译当作运行验证。
Windows 当前使用 MinGW-w64，不承诺和 MSVC 的 C++ ABI 互换。

```sh
cmake -S . -B .dev/sdk-build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=OFF -DFSNET_WITH_LUA=OFF -DFSNET_WITH_POSTGRES=OFF \
  -DFSNET_OPENSSL_STATIC_ONLY=ON
cmake --build .dev/sdk-build --target fsnet --parallel 4
cmake --install .dev/sdk-build --component sdk --prefix .dev/sdk-prefix
python3 tools/sdk/verify.py --prefix .dev/sdk-prefix --build-dir .dev/sdk-consumer
python3 tools/sdk/package.py --prefix .dev/sdk-prefix-relocated \
  --build-dir .dev/sdk-build --platform linux-x86_64 --version 0.3.1 --output .dev/sdk-dist
```

verify 会将 prefix 移至新目录，然后只使用已安装头文件和库编译、运行独立 C++20 消费者。
Windows 在 MSYS2 MINGW64 shell 中运行，使用 `python`。macOS 可显式指定
`-DCMAKE_OSX_ARCHITECTURES=x86_64` 或 `arm64`；OpenSSL Configure 随目标架构选择，
不在同一构建目录混合多种架构。

## 安装与发行合同

SDK tag 为 `sdk-v0.3.1`，资产命名 `fsnet-sdk-v0.3.1-<platform>.zip`，zip 根目录直接为安装前缀。
`manifest.json` 记录 fsnet 源码提交、编译器与 C++ ABI、协议版本、平台、运行库清单及 SHA256。
`SHA256SUMS` 校验整个 ZIP。插件发布工具应先验证 ZIP，再验证 manifest/运行库，而不是下载浮动 master。

Linux 运行库位于 `lib/libfsnet.so`；macOS 为 `lib/libfsnet.dylib`，install_name 使用 `@rpath`；
Windows 为 `bin/fsnet.dll`，同时携带必要的 MinGW libstdc++/libgcc/libwinpthread 运行 DLL。
消费者仍需要与 SDK 兼容的 C++ ABI；Godot 扩展也须按平台/架构分别编译。

OpenSSL 被静态链接到核心共享库，不把机器上的私有 OpenSSL 作为 SDK 依赖。
libjuice/SQLite/SQLiteCpp/Asio 的原始许可证和精确源码 URL 随 SDK 提供，第三方许可证不被插件许可证替代。
Windows GNU 运行库保留 MSYS2/GCC 的运行库例外与许可证通知。

只编译 fsnet 核心和外部 SDK 消费者，是跨平台客户端 ABI 验证，不等于 Windows/macOS 的生产服务端持久化验收。
这些平台的客户端包支持范围，以已通过的平台流水线及最终发行 manifest 为准。

## 发布已验证的构建

手动运行 `publish-sdk.yml`，传入成功的桌面 SDK run ID、完整源码 SHA 和版本。
发布器会验证四个平台的 ZIP、运行库、架构与来源，拒绝覆盖已存在的 tag，
然后创建精确指向该 SHA 的 tag 和草稿。上传和读取草稿始终使用创建 API 返回的 release ID；
GitHub 按 tag 的查询端点不会公开草稿。

四个 ZIP、总 `SHA256SUMS` 和发行 manifest 的服务器摘要与本地结果全部匹配后，
发布器才公开同一个草稿。摘要尚未计算时进行有限重试，任何实际摘要不匹配立即停止。
若上传或验证中断，会保留草稿和 tag 供检查，不自动删除、替换或重建已有发行。
