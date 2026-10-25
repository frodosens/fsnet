# 修复后整体 Benchmark

测量版本：`7f7bcd9ed9513c829cc7dc7079c4c4999484e95a`，2026-10-25。
GCC 14.2、Release `-O3 -DNDEBUG`、Lua ON、PostgreSQL OFF；Xeon Platinum 8573C，
cgroup 配额 2 核（200000/100000），未绑核。运行时没有源码修改，构建已完成后才开始测量。
各命令先进行一次不计时预热，再串行运行五轮；下面为各轮指标的中位数。
p99 是五个独立 p99 的中位数，不是合并分布；共享云机器的调度波动保留在原始样本中。

70 次测量命令全部成功；另外完成五次独立交付验证，每次 1000 轮持久提交，共 5000 轮。
[全部原始样本与二进制哈希](benchmarks/runtime-2026-10-25.json)、
[中位数及最小/最大值](benchmarks/runtime-summary-2026-10-25.json)。

## RPC 吞吐和延迟

以下为明文 loopback，客户端、Gateway 前后端和 owner 的 Node 在同一 polling 线程中推进。
Gateway 包含真实账号委托、目录查询/缓存和两跳转发，目标是原生 echo，
不包含每请求 Lua、实体提交、TLS 或跨机器 RTT。请求与回复携带相同载荷。

| 请求载荷 | 并发 | 路径 | 请求/秒 | p50 | p99 |
| --- | ---: | --- | ---: | ---: | ---: |
| 128 B | 1 | 直连 | 120,960 | 7.644 µs | 25.218 µs |
| 128 B | 1 | Gateway | 52,671 | 17.334 µs | 46.145 µs |
| 128 B | 32 | 直连 | 152,746 | 0.195 ms | 0.378 ms |
| 128 B | 32 | Gateway | 69,692 | 0.449 ms | 0.747 ms |
| 4 KiB | 32 | 直连 | 129,015 | 0.232 ms | 0.421 ms |
| 4 KiB | 32 | Gateway | 55,530 | 0.555 ms | 0.938 ms |
| 64 KiB | 32 | 直连 | 13,623 | 2.241 ms | 2.992 ms |
| 64 KiB | 32 | Gateway | 6,007 | 5.344 ms | 8.970 ms |

原有 Node echo（128 B、并发 128）为 169,499 req/s，p50 0.730 ms、p99 1.063 ms。
并发增加后排队进入请求时延，因此不能把并发 32 的 p50 当作空载单请求 RTT。
128 B 直连并发 32 的某一轮 p99 达 2.418 ms，其余轮约 0.300–0.432 ms，未删除该样本。

## Lua 与属性同步

| 场景 | 平均每操作耗时的五轮中位数 |
| --- | ---: |
| 原生 scalar stage+publish，32 属性 | 0.185 µs |
| 原生 vec3 stage+publish | 0.625 µs |
| 原生确认副本应用 vec3 delta | 0.992 µs |
| 32 属性全量 encode+decode | 3.983 µs |
| Lua 本地 position.x 读取 | 0.667 µs |
| Lua 本地 position 整值赋值 | 2.867 µs |
| Lua 本地 position.x 赋值 | 3.037 µs |

网络属性场景为真实 TCP＋Lua handler＋Memory 存储提交，计时到 ACK 和全部观察者收到更新；
每轮 1000 次更新，不经过 Gateway。它与上面的纯 echo 和下面的文件持久化场景分开测量。

| 观察者 | 平均每轮 | p50 | p99 |
| ---: | ---: | ---: | ---: |
| 1 | 0.190 ms | 0.162 ms | 0.489 ms |
| 8 | 0.301 ms | 0.272 ms | 0.649 ms |
| 32 | 0.709 ms | 0.665 ms | 1.232 ms |

## 真实客户端差量接收

对含 0/16/256 个不变字符串项的副本发送 1000 个 scalar 更新，每项字符串 512 B，
每个更新的 FSA 属性记录 37 B。下表分配量仅在 `client.poll()` 期间计数；耗时为累计
`client.poll()` 时间除以更新次数，包含网络事件与解码，不是端到端 RTT 或纯 codec 时间。

| 不变字符串项 | 接收耗时/更新 | C++ 分配次数/更新 | 累计分配字节/更新 |
| ---: | ---: | ---: | ---: |
| 0 | 2.825 µs | 6 | 455 B |
| 16 | 2.754 µs | 6 | 455 B |
| 256 | 3.880 µs | 6 | 455 B |

分配量不随未变数组增长，确认正式接收路径没有因预算检查而深复制整个状态。
同一构建中的显式候选复制对照，在 256 项时仍为 262 次、146,256 B/操作；
这只是局部对照，其计时范围不含 Node 接收，不能直接用两者耗时计算网络加速倍数。
完整图像校验仍遍历未变值；持有历史 snapshot 时仍会发生 COW。

同一构建的其他局部对照：完整解码＋重新编码转发 72.020 µs，校验＋原字节重映射 27.331 µs；
更新后额外 clone 快照 18.139 µs，共享 snapshot 4.309 µs。它们不是不同版本服务端的吞吐对比。

## 容器合批和网络字节

128 个 Item 中修改 16 个字段，每轮测试 100 次：

| 写入方式 | 平均每轮 | 提案/提交次数（100 轮合计） | 提案 FSA 字节合计 |
| --- | ---: | ---: | ---: |
| 逐次写 16 字段 | 25.461 ms | 1600 / 1600 | 83,200 B |
| batch16 | 2.951 ms | 100 / 100 | 45,700 B |

这个夹具的 batch 每轮耗时约低至逐次写入的 1/8.63，提案/提交减少 93.75%，提案属性字节减少约 45.1%。
下面仅计算 FSA 记录，不含 RPC、FSN1、TCP/TLS、ACK 或重传；上行与下行使用同一差量算法。

| 修改 | 整属性记录 | 差量记录 |
| --- | ---: | ---: |
| 1 KiB 结构，仅修改 position.x | 1111 B | 54 B |
| vec3，仅修改 x | 67 B | 48 B |
| 128 个 Item，修改一个字段 | 4146 B | 52 B |
| list 追加 16 个 Item | 4664 B | 556 B |
| map 添加 16 个键 | 5200 B | 805 B |
| map 删除 16 个键 | 4008 B | 191 B |

## mTLS＋文件持久化交付链路

独立客户端进程 → mTLS Gateway → mTLS owner → POSIX 文件 CAS/fsync → 确认同步。
单账号串行执行 1000 次 vec3 双字段 batch，每次单独提交；五次测试使用独立目录与临时 PKI。

- 中位吞吐：167.4 batch/s；五轮范围 135.4–171.8 batch/s。
- 1000 次 batch 的计时中位数：5.975 s；范围 5.821–7.385 s，折合平均 5.975 ms/batch。
- SIGKILL 后接管确认中位数：1.966 s；范围 1.943–1.970 s。
- 五轮均通过 mTLS/身份拒绝、委托、自动订阅、崩溃接管、回执重放、持久命令、优雅和定向交接、资源清理等 16 项检查。

batch 时间从父进程观察到 recovered 到 load_done，不包含启动、证书生成或崩溃恢复。
启动器固定 1 ms 轮询，链路还包含 TLS、Lua、执行器等待、复制和 fsync；没有 profiler 分离各项占比。
这是单账号串行持久提交结果，不是引擎总并发容量或 FPS 输入处理上限；跨机器网络和生产磁盘需要另测。

## 复现

```sh
. tools/dev/activate.sh
cmake --build --preset release
mkdir -p .dev/benchmarks-2026-10-25
c++ -std=c++20 -O3 -DNDEBUG -Iinclude tools/review/round2/replica_cow.cpp \
  -L.dev/build/release -Wl,-rpath,"$PWD/.dev/build/release" \
  -lfsnet -pthread -ldl -lm \
  -o .dev/benchmarks-2026-10-25/fsnet-receiver-benchmark
python3 tools/review_performance.py .dev/build/release --runs 5 --delivery \
  --receiver .dev/benchmarks-2026-10-25/fsnet-receiver-benchmark \
  --output .dev/benchmarks-2026-10-25/full.json
```

本轮只更新测量入口和报告，没有修改引擎。未对旧源码交替 A/B，历史轮次差异不作因果归因。
