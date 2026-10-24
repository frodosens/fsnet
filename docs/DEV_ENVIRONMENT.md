# 开发环境与 fsnet-env 迁移

后续开发使用仓库自身的配置重建环境，不再依赖固定的 `/workspace/fsnet-env`。
需要长期保留的配置、脚本、测量数据和验收报告全部进入 Git；工具安装、依赖源码、二进制和
编译缓存放在仓库内 `.dev/`，通过 `.gitignore` 排除，不作为源码提交。

## 初次使用

基础环境需要 Bash、Python3/pip、支持 C++20 的编译器、GNU make、Perl；完整 TLS/交付测试
还需要 openssl 命令。验证过的主机为 Debian 13 x86_64、GCC 14.2。
应用库版本及 SHA256 由 CMake 固定，开发工具版本在 `tools/dev/requirements.txt` 固定：
CMake 4.4.4、Ninja 1.13.2。OS/编译器本身仍由宿主提供，不宣称不同机器能生成逐字节相同的二进制。

```sh
bash tools/dev/bootstrap.sh
. tools/dev/activate.sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

bootstrap 不需要 sudo，只在 `.dev/python-tools` 安装固定工具；匹配版本已存在时跳过下载。
从任意工作目录运行该脚本都可找到其 checkout。激活脚本必须在 Bash 中 source；它使用当前
checkout 的绝对路径设置 PATH，不修改系统配置，不保存账号、token 或私钥。
首次准备需访问 Python 包源，首次 CMake 配置/构建需下载 CMake 已固定并校验的源码归档。

| preset | 用途 | 输出 |
| --- | --- | --- |
| debug | Lua ON、PostgreSQL OFF、普通测试 | `.dev/build/debug` |
| release | Release、Lua ON、普通测试与性能程序 | `.dev/build/release` |
| asan | ASan/UBSan、Lua ON、原生测试 | `.dev/build/asan` |
| core | Lua OFF、PostgreSQL OFF、核心测试 | `.dev/build/core` |
| postgres | Lua与PostgreSQL服务端集成测试 | `.dev/build/postgres` |

构建 preset 默认2个并行任务，可临时 `cmake --build --preset debug -j4`；OpenSSL/PostgreSQL
外部构建脚本另有自身并行设置。asan 的 test preset 自动设置 detect_leaks/halt_on_error。
直接运行工具或程序时，sanitizer 环境仍需由调用方设置。
`CMakeUserPresets.json` 已忽略，可放本机覆盖项，不要提交凭据或个人路径。

兼容原来的自动化入口：

```sh
bash tools/cloud-install.sh             # bootstrap + debug configure/build/test
bash tools/cloud-install.sh release
bash tools/cloud-install.sh asan
```

## 可选 PostgreSQL 工具

需要 bison、flex、m4。Debian/Ubuntu 无 sudo 时：

```sh
bash tools/dev/postgres-tools.sh
. tools/dev/activate.sh
cmake --preset postgres
cmake --build --preset postgres
ctest --preset postgres
```

脚本复用主机已配置的 APT 来源与签名校验，只更新 `.dev/apt` 的私有缓存，将下载包解包到
`.dev/root`，不向系统安装。`cloud-install.sh postgres` 缺少上述命令时会调用它。
其他系统请用对应包管理器准备这些基础工具；不能把一个发行版的二进制工具复制到另一系统。
本次环境迁移不等于重新执行 PostgreSQL 服务端验收，历史 PG 结果仍在 VALIDATION.md 中单列。

## 日常使用与性能对照

```sh
. tools/dev/activate.sh
cmake --build --preset debug
ctest --preset debug
cmake --preset release
cmake --build --preset release
python3 tools/review_performance.py .dev/build/release --delivery --output .dev/review.json
```

激活默认令 FSNET_LIBRARY 指向 debug 的 libfsnet.so；选择其他配置时，在 source 前设置
`FSNET_DEV_PRESET=release`。不要在同一进程混用不同构建的头文件、可执行文件和共享库。
C++ API/类布局变化后必须重编消费者。

当前工作区的固定工具已从旧目录复制到 `.dev/python-tools`，并保留了两个便于继续 A/B 的本机
基线副本：`.dev/baselines/copy-before-55de967` 和 `.dev/baselines/attributes-before`。
这些是可丢弃的本机缓存，不在 Git 中；迁移到新机器时从相应 Git 基线重建即可。
例如本机现存的 copy 基线可用于：

```sh
python3 tools/compare_engine_benchmarks.py \
  .dev/baselines/copy-before-55de967 .dev/build/release \
  --runs 5 --output .dev/comparison.json
```

有现成的同平台 pip target 工具时，可首次运行
`bash tools/dev/bootstrap.sh --reuse-tools /path/to/python-tools`，避免重新下载；bootstrap 仍检查
版本。不会移动旧 CMake 构建目录，因为 CMakeCache/生成文件嵌入原来的绝对路径，直接移动
不能视为有效重建。本次在 `.dev/build/debug` 从头配置构建以验证新入口。

## Git 中保留什么

- `CMakePresets.json`、`tools/dev/`、`tools/cloud-install.sh`：环境与构建入口。
- `CMakeLists.txt`、`cmake/`：应用依赖来源、版本和SHA256。
- `docs/benchmarks/`、`docs/validation/` 与相关说明：已完成测量的原始数据、二进制哈希和证据。

历史报告内的旧工作区路径是当时的测量信息，不是当前构建依赖，保持原样。
编译/安装日志与临时实验程序不作为开发记录纳入 Git。
`.dev/` 可以删除重建；新 checkout 运行 bootstrap 和 preset 即可继续开发。
旧 `/workspace/fsnet-env` 本次没有删除，可在确认不再需要旧构建缓存后单独清理。
