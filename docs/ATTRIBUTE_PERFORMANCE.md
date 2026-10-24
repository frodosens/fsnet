# 属性同步性能与优化

2026-10-23。本轮降低属性写入和同步路径的 CPU/复制成本，FSA1/FSN1 协议与 Lua API 保持兼容。
基线为 `475b6f8`，该轮优化版为 `81696f7`。后续容器合批与补丁测量见
[容器合批性能](CONTAINER_BATCHING_PERFORMANCE.md)。

## 测量方法

- Debian 13 / Linux 6.18.55，Intel Xeon Platinum 8573C，3 个可用 vCPU；共享云环境，未绑核。
- GCC 14.2，CMake 4.4.4，Release `-O3 -DNDEBUG`，开启 Lua、关闭可选 PostgreSQL。
- 旧版在修改核心前编译，保存独立可执行文件和 `libfsnet.so`；优化版使用相同编译配置和基准源码。
- 每个程序先跑一轮预热，再交替 A/B、B/A，共 7 对独立进程。测量期间不并行编译或运行测试。
- 实体有 32 个顶层属性：vec3、mat4x4、30 个标量；其中 10 个标量仅 owner 可见。
- 本地原生/Lua 场景每轮 20,000 次操作；编解码每轮 5,000 次；TCP 每种接收者数量每轮 1,000 次更新，另预热 20 次。
- 表内时间为每轮平均耗时的 7 轮中位数；不是单次采样，也不是最大吞吐。原始数据保留各轮平均值、范围、网络 p50/p99、编译标识和二进制 SHA256。

[全部原始样本](benchmarks/attributes-2026-10-23.json)。

## 对比结果

单位：微秒/操作；倍数为优化前耗时 ÷ 优化后耗时。

| 场景 | 优化前 | 优化后 | 倍数 |
| --- | ---: | ---: | ---: |
| 原生单标量 stage + publish | 1.589 | 0.191 | 8.34× |
| 原生 vec3 stage + publish | 2.018 | 0.626 | 3.23× |
| 原生副本应用 vec3 增量 | 3.395 | 0.738 | 4.60× |
| 32 属性全量编码 + 解码（对照） | 3.467 | 3.583 | 0.97× |
| Lua position.x 读取（对照） | 0.640 | 0.600 | 1.07× |
| Lua position 整值赋值 | 9.378 | 2.794 | 3.36× |
| Lua position.x 字段赋值 | 9.866 | 2.958 | 3.34× |
| TCP 同步，1 个接收者 | 207.174 | 167.578 | 1.24× |
| TCP 同步，8 个接收者 | 516.434 | 372.316 | 1.39× |
| TCP 同步，32 个接收者 | 1359.701 | 667.486 | 2.04× |

原生副本应用场景不含 SDK 的额外内存预算与回调开销；后者包含在 TCP 场景中。
Lua 场景为本地实例操作，不包含网络往返或服务端提交；真实同步由 TCP 场景单独覆盖。
编解码对照项中位数慢约 3.3%，两版样本范围有重叠；本轮没有获得编码压缩收益。
读取对照项没有专门优化，不把约 7% 的差异视为确定的读路径收益。

## 完整链路

真实 TCP loopback，1 个服务端、1 个写入客户端及其他观察者；所有 Node、Lua、SDK 在同一调用线程推进，存储由后台线程执行。
接收者总数包含写入客户端。每次通过 RPC 写入 `position.x`，等待 Memory 后端 CAS 提交、RPC 确认和所有接收者更新完成，再发下一次。
计时不包含连接/订阅建立，不包含客户端 Lua、渲染、TLS 或磁盘 fsync。

网络分位数同样取各轮分位数的中位数，单位微秒：

| 接收者 | p50，前 → 后 | p99，前 → 后 |
| --- | ---: | ---: |
| 1 | 182.7 → 146.6 | 501.1 → 434.2 |
| 8 | 455.9 → 350.1 | 865.0 → 657.6 |
| 32 | 1293.0 → 624.7 | 1828.5 → 1094.5 |

32 个接收者下平均耗时约减半，仍有每接收者处理成本；这些串行、单实体、内存后端结果不能推算生产在线人数。

## 改动

1. `EntityAttributes::publish` 仅复制变更属性；先完成全部校验与分配，再执行不会抛异常的交换，保留整次发布原子性与无变化不递增版本语义。
2. 编码与精确大小校验共用值遍历逻辑。预算检查无需生成临时报文，也无需把完整 map 复制进临时 `AttributeUpdate`。
3. `AttributeReplica` 增量应用通过有序合并校验累积图像，只准备增量值，再交换已有值或转移新节点。快照仍整体替换；缺版本、过期、非法值和容量超限均不部分生效。
4. Lua 对象缓存不可变 schema 的内存记账，更新时仍计算动态值大小并执行原有预算检查。
5. 广播从宿主已校验的完整图像直接生成权限投影，避免为每个订阅者重建默认实体和恢复状态；连接身份、owner/AOI、租约、代数、预算和背压仍逐个检查。

Debug CTest **24/24**，ASan/UBSan（含泄漏检测）**15/15** 通过。
新增覆盖合并后字段数、字节数、节点数超限时状态/版本保持不变，以及结构值精确大小与编码大小一致。
本轮未开启 PostgreSQL，不把历史 PostgreSQL 验证当成本轮结果。

## 带宽和剩余开销

- 此基准全量 FSA1 为 **613 字节**；单个 vec3 顶层属性增量为 **67 字节**，两版完全相同。
- `position.x` 修改仍发送整个 `position`；不是仅传一个 double。加上该实体 FSN1 信封后，每接收者每次通知应用载荷为 **96 字节**。
- 32 个接收者每次通知合计 3,072 字节；若每秒 20 次，则约 60 KiB/s。这里只计算出站属性通知，不含 TCP/TLS 帧、RPC 请求/确认、重传或首次快照。
- 尚无字段路径增量、数值量化、属性编号压缩或按帧合并。需要降低带宽时，应单独设计协议和兼容策略。
- Lua 写入仍复制候选值图像做预算/整体校验；SDK 为聚合预算保留候选副本；服务端仍按请求恢复、校验并持久化完整实体记录。
- 每个订阅者仍保存自己的投影和计算差量。大量观察者时可继续研究共享投影/编码缓存，但权限与版本差异必须纳入缓存键。

## 复现

构建当前版本：

```sh
cmake -S . -B /tmp/fsnet-perf-after -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build /tmp/fsnet-perf-after --target fsnet-attribute-benchmark -j3
/tmp/fsnet-perf-after/fsnet-attribute-benchmark all 20000
```

准备独立基线源码目录（不改变当前分支），复制相同基准并注册目标：

```sh
mkdir -p /tmp/fsnet-perf-baseline-src
git archive 475b6f8 | tar -x -C /tmp/fsnet-perf-baseline-src
cp benchmarks/attribute_benchmark.cpp /tmp/fsnet-perf-baseline-src/examples/
cat >> /tmp/fsnet-perf-baseline-src/CMakeLists.txt <<'CMAKE'
if(FSNET_WITH_LUA)
  add_executable(fsnet-attribute-benchmark examples/attribute_benchmark.cpp)
  target_link_libraries(fsnet-attribute-benchmark PRIVATE fsnet-lua)
endif()
CMAKE
cmake -S /tmp/fsnet-perf-baseline-src -B /tmp/fsnet-perf-before -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build /tmp/fsnet-perf-before --target fsnet-attribute-benchmark -j3
python3 tools/compare_attribute_benchmarks.py \
  /tmp/fsnet-perf-before/fsnet-attribute-benchmark \
  /tmp/fsnet-perf-after/fsnet-attribute-benchmark \
  --runs 7 --iterations 20000 --before-revision 475b6f8 \
  --after-revision "$(git rev-parse HEAD)" --output /tmp/attribute-comparison.json
```

工具交替启动两个构建，分别使用同目录共享库并保留全部样本；不要让两个路径指向同一份已被覆盖的库。
可用 `native`、`lua`、`network` 代替 `all` 单独定位，但不要使用 Debug/sanitizer 数据做 Release 对比。
