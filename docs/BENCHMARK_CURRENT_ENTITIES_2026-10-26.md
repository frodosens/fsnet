# 当前全局实体性能样本

2026-10-26。Release，预热一次后串行三轮；不与构建、CTest 或其他基准并发。
平台、配额、命令、源码版本和二进制哈希在 [原始样本](benchmarks/global-api-removal-2026-10-26.json)。
以下是每轮指标的中位数；p99 是三轮 p99 的中位数，不是合并样本分位数。

## 原生 echo 与 Gateway

该夹具已使用 Catalog 分配的全局 ID、@/FSE3 及真实 Gateway 委托和权威类型白名单。
响应回送原封包，测量包括网络事件循环和转发；每请求不执行 Lua、实体激活或持久 CAS。
loopback、明文、单 polling 线程、memory Storage。

| 链路 | 负载 B | 并发 | 请求/秒 | p50 μs | p99 μs |
| --- | ---: | ---: | ---: | ---: | ---: |
| direct | 128 | 32 | 156,307 | 196.61 | 314.78 |
| gateway | 128 | 32 | 66,689 | 472.40 | 1002.25 |
| direct | 128 | 1 | 125,113 | 7.61 | 22.32 |
| gateway | 128 | 1 | 52,123 | 18.33 | 41.10 |

4096/65536 B 和其他公共算法的完整数据也在原始文件，不将本机延迟外推为公网 FPS 时延。
删除旧 Type.method 基准后封包和目录模型已改变，不能直接与历史旧基准比较吞吐。

## 实体 Lua/CAS/Gateway

[原始三轮](benchmarks/global-flow-current-2026-10-26.json)：每轮先完成 Manager/LoginEntity/Player 创建与交接，
再计时 2000 次 RPC，并发 8；包含 Lua 调度和 memory Storage 的带 fence 提交，不含 TLS/磁盘 fsync。
请求/秒中位数 11,621，p50 598.12 μs，p99 1076.99 μs。
这与上表 echo 的测量范围不同，不能相互替代。

## 公共机制诊断

[分配与调度样本](validation/global-api-removal-2026-10-26.json) 显示：连接 owner 比较不分配，
单次 owner 策略检查只复查一次 live；计时器计长不分配；tracked=1/2 的外节点先出现场景都执行了本地到期任务。
未列方法和 server-only 方法均拒绝，Type.method 不存在；两次新连接登录找到相同 PlayerId。

公共值基准包含 stage/publish、64KiB 未变结构中的单字段 patch、投影应用和 list 追加。
该 64KiB 示例的字段编码为 44B；生成/投影仍涉及值校验与复制，较小网络补丁不等于零 CPU 成本。
索引分片 CAS、实体完整快照保存、Lua 与属性候选复制等成本仍存在。

未进行跨机器、TLS+持久实体负载合测、数据库 HA、长时间容量或生产 SLO 验收。
