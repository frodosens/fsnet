# 本地生产门槛补充验证（2026-10-26）

以 `bd0ad59` 继续验证，扩展全局链路夹具的可选持续负载模式并补资源采样。
五分钟测试发现并复现了第三个 P2，随后只修复 Router 的已断开连接缓存；
最初两个缺陷的回归见 [master 加固报告](MASTER_HARDENING_2026-10-26.md)。

## 新发现并修复：空闲断线后的第一次无键调用失败（P2）

`src/entity_router.cpp:97` 原先只在缓存 peer ID 为零时连接。没有在途请求的连接因空闲关闭后，
缓存 ID 仍非零；下一笔新调用被送往已不存在的 session，返回 disconnected。有键调用可随后重试，
无键调用则直接失败，正常重新登录也受到影响。

首个300秒测试成功完成 2,200,751 次 move，但随后的重新登录断言失败，整个测试 exit 1。
确定性短回归在成功调用后关闭已知 peer、等待客户端确认断线，再发新无键调用，旧实现同样失败。
修复在新尝试前核验 connected/connecting；已明确断开的缓存建立新连接。这不是重放不确定请求：
既有中途断线的无键调用仍返回 disconnected，原有“不得重放”回归继续通过。
同类连接复用检查已用于 Transfer/Delivery，本轮复用这一合同，没有引入新架构。

## Lua OFF

独立 `core` preset，Debug、Lua OFF、PostgreSQL OFF，从源码构建：**20/20 通过，13.86s**。
覆盖核心、连接、属性、文件与 SQLite、存储 worker、授权、路由和传输回归，以及安装 SDK 与进程检查。

```sh
. tools/dev/activate.sh
cmake --preset core
cmake --build --preset core
ctest --preset core
```

## ThreadSanitizer

先编译/执行含 atomic 和 thread 的最小程序，成功。再构建 Debug/Lua OFF 的整个核心与测试，
CXX/可执行/共享库链接分别添加 `-fsanitize=thread`，CXX 还加 `-fno-omit-frame-pointer`，
设置 `TSAN_OPTIONS=halt_on_error=1` 后执行 CTest。

原始结果：**17/20 通过，18.24s**，没有在通过的原生测试中观察到 TSan race 报告。
三项失败保留原始输出，不算通过：

- `fsnet-file-process`、`fsnet-process`：未插桩 Python 的 ctypes 动态载入 libtsan 失败，
  `cannot allocate memory in static TLS block`，没有进入业务断言。
- `fsnet-sdk-package`：自动创建的未插桩 SDK consumer abort，`event: Invalid argument`。
  SDK consumer 构建未继承此手动 TSan 配置；不能直接把该失败定性为 fsnet 并发缺陷。

独立安装同一 TSan SDK，并给消费项目明确增加 -fsanitize=thread 后，consumer 退出0。
这支持“自动安装夹具混合了插桩/未插桩进程”的解释；原始17/20失败记录仍保留，不改写为全量通过。

Asio 编译期间有 `atomic_thread_fence is not supported with -fsanitize=thread` 警告；SQLite、
OpenSSL、libjuice 等 C/外部依赖也没有完整插桩，因此上述结果不是所有跨库并发安全的证明。
普通 Lua OFF/Release 已通过相应 Python 和 SDK 夹具；未尝试修改系统 ASLR、Python 或绕过权限限制。

## PostgreSQL

无需下载或安装新依赖。只读复用了旧环境的 PostgreSQL **17.11** 私有服务端及静态 libpq /
libpgcommon_shlib / libpgport_shlib。当前 `src/postgres_database.cpp` 用 FSNET_WITH_POSTGRES 重编，
与本轮 Release 的其他对象和固定 OpenSSL 链接到独立库；原有旧库没有当作当前代码验证。
`tools/review/master_hardening/postgres_existing.py` 保存重编步骤。

原 `tests/postgres_fixture.py` **完整退出 0**：TLS 前置限制、数字 hostaddr/逻辑 TLS 主机名、DNS/service
拒绝、存储合同/并发 CAS、Lua 计时器恢复、连接及已连接黑洞期限、锁等待、不确定写入、
immediate stop 后 WAL 恢复与安全读取重连。服务数据和证书只存在临时目录，服务端已停止。
不是多机 PostgreSQL HA/同步复制验收，也不是从空环境重新构建 PostgreSQL 的验证。

```sh
python3 tools/review/master_hardening/postgres_existing.py \
  .dev/build/release /path/to/postgres-17.11-install .dev/pg-check
LD_LIBRARY_PATH="$PWD/.dev/pg-check" python3 tests/postgres_fixture.py \
  /path/to/postgres-17.11-install /usr/bin/openssl \
  "$PWD/.dev/pg-check/fsnet-postgres-tests" \
  "$PWD/.dev/build/release/fsnet-script" "$PWD/.dev/build/release/fsnet-client"
```

## 300 秒本地持续负载

扩展现有全局实体夹具，保留初始化、远程创建、迁移、权限及收尾断言。
工作负载：一个进程内 Client → Gateway → 两个 owner 中当前 Player → Lua move → POSIX 文件 CAS，
同一实体并发 8，payload `0`，不加 TLS。持续 300 秒后排空已发请求；每个回复必须成功。
延迟仅保存最近 4096 个样本，避免测试器自身随请求数无限扩容；分位数明确是尾部窗口，不能代表整段总体分布。
外部 Python 每秒读取 /proc 的 RSS、高水位、线程、FD、CPU 时间。

本环境临时目录位于 **tmpfs**。虽走完整文件后端与 fsync 调用，吞吐不代表物理磁盘持久化能力，
断电/设备故障也未覆盖。本测试用于资源稳定性和持续调用正确性；只有一个热点实体，不是大型世界容量测试。
全部其他编译/测试完成后开始计时。

首次运行：2,200,751 次 move 成功后重新登录失败，整个夹具退出1，不能算验收通过。
修复后：**2,028,857 次请求，300.001秒测量，6762.83 rps**；后续重新登录、换绑、远端创建及收尾断言均通过，整个进程退出0（总耗时301.47秒）。
尾部4096样本 p50=851.336µs，p99=1981.86µs；不是全程分位数，也不与 Memory 基准直接作吞吐比较。

30秒预热后 RSS **11836–11836 KiB**，线程 **12**，FD **29–45**（包含文件打开期间的暂态），未观察到持续增长。
进程累计 CPU 约 380.22 秒，约 1.27 核当量；最终存储26文件、3057字节。
这只是一次五分钟、单热点实体的本地稳定性验收，不是小时/天级长稳证明。

```sh
python3 tools/review/master_hardening/soak.py .dev/build/release \
  --seconds 300 --output .dev/soak.json
```

新持续模式的 2 秒冒烟与未带选项的原 `fsnet-global-flow` 回归均通过。
最终修复的全配置复测：Release34/34（25.44s）、ASan/UBSan24/24（18.19s）、
Lua OFF20/20（11.44s）、TSan原生17/17（15.83s），PostgreSQL原夹具再测退出0。
TSan三项混合消费者夹具未改写为通过。

最终修复再做5对交替性能：原生113523→112035 rps、Gateway49353.6→48511.5、
Lua/Memory-CAS6495.01→6497.79。吞吐中位数差异均小于2%；原生p99从1800→2100µs，
Lua尾部p99从1717→1902µs，保留这些不利波动，未做统计显著性或生产SLO结论。
原始样本见 `benchmarks/paired-final-2026-10-26.json`。

原始逐秒数据见 `benchmarks/soak-300s-2026-10-26.json`；新增逐项验证、失败输出、PostgreSQL日志及哈希
见 `validation/additional-2026-10-26.json`。

跨机故障域、公网网络、长时间（小时/天级）稳定性、实际业务实体规模、物理存储与目标SLO仍是上线门槛。
