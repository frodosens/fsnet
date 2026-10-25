# 全局实体寻址对比

2026-10-25，`refactor/global-entities`。完整结果与二进制 SHA256 见
[原始五轮样本](benchmarks/global-entities-2026-10-25.json)。

GCC 14.2，Release `-O3 -DNDEBUG`，Lua ON，PostgreSQL OFF。
测量前完成构建；先一次不计入结果的预热，再串行运行五轮。
每轮在同一进程中先测旧入口，再测全局入口，下面为各轮指标的中位数。
未做绑核或顺序随机化，结果只表示这台共享云机器上的夹具，不能推广为生产容量或因果结论。

## 相同业务链路

客户端 SDK → Gateway → Lua Player → 内存 Storage 读取/单记录 CAS → 回复。
两种模式使用同一个 Player ID、同一状态、账号权限、连接、方法与参数 `move('0')`。
每种 2000 个请求、并发 8，单 polling 线程与既有存储工作线程，使用 pump/yield 驱动。
旧入口仅作为此比较夹具显式暴露；没有绕过 Lua、实体提交或账号检查。
不包含 mTLS、磁盘 fsync、数据库网络、跨机器 RTT 或公网客户端。

| 寻址 | 请求/秒 | p50 | p99 |
| --- | ---: | ---: | ---: |
| 旧 type/id + `Player.move` | 10,096 | 0.684 ms | 1.160 ms |
| 全局 EntityId + FSE3 | 9,738 | 0.698 ms | 1.291 ms |

这组样本中吞吐下降约 3.6%，p50 增加约 2.0%，p99 增加约 11.3%。
五轮吞吐范围：旧入口 10,020–10,224，新入口 9,444–10,213 请求/秒。
不能据此宣称统一寻址提升了吞吐；新入口包含 ID 解码、类型缓存和转入既有 mailbox 的工作。
没有对每项工作分别计时，不把全部差异归因于某个实现细节。

现有原生 echo benchmark 没有每请求 Lua 和实体 CAS，不能拿此前的约 70k Gateway echo 请求/秒与本表直接比较。
此次样本主要覆盖缓存命中；首次元数据加载、节点故障和服务重建成本没有单独做容量测量。

## 完整请求字节

相同的 16 位文本 ID，方法 `move`、1 B 参数、没有 request_id：

| 格式 | Node 固定头 | Node route | 实体 envelope（含参数） | 合计 |
| --- | ---: | ---: | ---: | ---: |
| 旧 `Player.move` + FSE1 | 18 B | 11 B | 23 B | 52 B |
| 新 `@` + FSE3 | 18 B | 1 B | 19 B | 38 B |
| 新 FSE3 附 expected_type=`Player` | 18 B | 1 B | 26 B | 45 B |

通用引用减少 14 B（约 26.9%）。这里核算 Node 请求帧，不包含 TCP/IP、TLS、应答或属性流。
FSE3 附带 request_id 时另加一字节长度和 token；旧 FSE2 使用两字节 token 长度。
属性差量 FSA 算法继续共用，这次没有改动压缩、预测或属性 patch 的算法。

## 复现

```bash
. tools/dev/activate.sh
cmake --preset release
cmake --build --preset release --target fsnet-global-flow-tests
.dev/build/release/fsnet-global-flow-tests "$PWD" --benchmark
```

程序先完成跨节点创建、登录、迁移和可靠投递断言，再输出两行 JSON 测量值。
无 `--benchmark` 时只执行功能测试，纳入普通 CTest 和 sanitizer 回归。
