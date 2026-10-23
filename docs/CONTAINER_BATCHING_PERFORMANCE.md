# 容器合批与增量性能样本

2026-10-23。源码基线为 `96d9e37`，实现与原始样本位于包含本报告的提交。
本报告比较**同一个新版本**的逐条 setter 与显式 batch 用法，隔离合批收益；
不把结果表述为与旧版二进制的整体速度对比。两种用法都启用了新下行容器补丁。

## 完整链路

- Debian 13 / Linux-6.18.55-x86_64-with-glibc2.41，INTEL(R) XEON(R) PLATINUM 8573C，3 个可用 vCPU，未绑核的共享云环境。
- GCC 14.2、CMake 4.4.4，Release `-O3 -DNDEBUG`，Lua 开启、PostgreSQL 关闭。
- 真实 TCP loopback，1 个服务端、1 个 owner 客户端。调用线程推进 Node/Lua/SDK，Memory 后端在独立存储线程完成 CAS；无 TLS、磁盘 fsync、客户端渲染或人工网络延迟。
- 一个实体包含 128 个 Item（id 字符串、int32 count）。每轮修改前 16 个元素的 count。
- 顺序方案每次赋值等待服务器确认；合批方案使用 `entity:batch()`，相同 16 次 setter 在草稿里完成后提交一次。
- 建连和订阅在计时前完成。每个进程运行 100 轮，首尾控制请求计入总计时；统计真实 write 调用和服务端提交计数，并断言最终值一致。
- 先各预热 10 轮，再交替顺序运行 sequential/batch、batch/sequential，共 7 对独立进程。测量期间不运行构建或测试。
- 耗时为各进程平均每轮时间的 7 轮中位数；不是单次操作的 p50，也不是吞吐容量上限。

| 每轮修改 16 个元素 | 逐条写入 | 显式合批 |
| --- | ---: | ---: |
| 耗时中位数 | 26.687 ms | 2.779 ms |
| 各轮平均耗时范围 | 23.814–31.145 ms | 2.559–4.506 ms |
| 实际 write 调用数 | 16 | 1 |
| 实际服务端提交数 | 16 | 1 |
| 上行 FSA 提案字节 | 66,368 | 4,148 |

本样本耗时约缩短 89.6%，约 **9.60 倍**；
提案数据减少 93.75%。字节计数来自写通道提交的 FSA1 提案，未包含请求 ID、RPC/TCP 信封、
下行通知、确认、首次快照或重传，不是网卡抓包结果。

## 下行补丁字节

独立构造同一前后状态，对比旧式 FSA1 整属性替换和新的自适应差量。
每种场景均先包含 128 个 Item，编码后实际应用到副本并断言等于完整目标状态。
这是确定性编码大小对比，不是另一组网络延迟测试。

| 场景 | 原整属性 FSA 字节 | 新差量 FSA 字节 | 减少 |
| --- | ---: | ---: | ---: |
| list_replace | 4146 | 70 | 98.3% |
| list_append16 | 4664 | 556 | 88.1% |
| map_add16 | 5200 | 805 | 84.5% |
| map_erase16 | 4008 | 191 | 95.2% |

list_replace 修改中间一个元素；list_append16 追加 16 个元素；map_add16 新增 16 个键；
map_erase16 删除 16 个现有键。数值仅针对该结构、键长度和属性名；不包含 FSN1/TCP 信封。
对密集变化/小容器会自动回退整属性，不承诺每次修改都有相同比例的节省。

## 复现与边界

```sh
cmake -S . -B /tmp/fsnet-container-perf -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build /tmp/fsnet-container-perf --target fsnet-container-benchmark -j3
python3 tools/compare_container_batching.py \
  /tmp/fsnet-container-perf/fsnet-container-benchmark \
  --runs 7 --rounds 100 --output /tmp/container-comparison.json
```

可用 `fsnet-container-benchmark wire` 单独校验编码大小，`sequential 100` 或 `batch 100` 单独运行链路。
工具保留全部原始计时、写入/提交计数、提案字节、二进制/共享库 SHA256 和平台信息。
[本轮原始样本](benchmarks/container-batching-2026-10-23.json)。

Debug CTest **27/27**、ASan/UBSan（含泄漏检测）**17/17** 通过。

服务端仍按请求持久化完整实体记录；客户端 batch 仍发送变化容器的最终全值。
草稿、预算检查、权限投影和副本应用仍有容器复制/遍历成本；本轮没有宣称这些成本归零，
也没有从单实体、单客户端、内存后端结果推算生产在线人数或公网延迟。
协议与使用合同见 [属性合批](ATTRIBUTE_BATCHING.md)。
