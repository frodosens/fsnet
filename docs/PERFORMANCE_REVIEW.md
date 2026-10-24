# 当前性能与字符串复制 review

后续实施进展、兼容边界及 A/B 证据见 [Copy 优化记录](COPY_OPTIMIZATION.md)。本文保留优化前基线。

2026-10-24，引擎基线 `53cee40`。本轮只增加测量工具及交付脚本的计时字段，没有修改引擎
参数、所有权或协议。结论：存在可避免的整块复制，但不能按 `std::string` 出现次数判断性能；
应优先处理重复编解码和整份快照，再处理只读参数。持久化链路的吞吐不能用纯 RPC 数字替代。

## 测量方式

GCC 14.2、Release -O3/NDEBUG、Lua ON / PostgreSQL OFF；Xeon Platinum 8573C，当前
cgroup CPU 配额为 2 核（200000/100000），未绑核。顺序运行三轮，表中取三轮指标的中位数，
p99 是每轮 p99 的中位数，不是汇总分布。共享云实例存在调度波动，没有与旧环境直接计算回归比例。
原始输出、全部重复样本、二进制/动态库哈希见 [JSON](benchmarks/engine-review-2026-10-24.json)。

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DFSNET_WITH_LUA=ON
cmake --build build-release -j3
python3 tools/review_performance.py build-release --runs 3 --delivery --output review.json
```

纯通信测量是单个 polling 线程、loopback 明文、Memory 目录；异步存储执行器另有工作线程。
新 request fixture 比较相同 FSE 封包、相同认证主体和原生 echo handler，Gateway 做真实委托、
目录查询/缓存和两跳转发；目录预置有效 owner，建立连接后再计时。不包含实体执行、Lua、
每请求 CAS、属性广播或 TLS，也不把测试中的固定身份信任用于生产。

## 结果

| 纯 RPC 场景 | 请求/秒 | p50 | p99 |
| --- | ---: | ---: | ---: |
| 原有 Node echo，128B，并发128 | 165,981 | 0.724 ms | 1.433 ms |
| 相同请求 fixture 直连，128B，并发32 | 148,433 | 0.194 ms | 0.434 ms |
| 相同请求 fixture Gateway，128B，并发32 | 67,406 | 0.452 ms | 0.685 ms |
| 直连，4 KiB，并发32 | 122,537 | 0.231 ms | 0.480 ms |
| Gateway，4 KiB，并发32 | 55,305 | 0.557 ms | 0.820 ms |
| 直连，64 KiB，并发32 | 10,214 | 3.002 ms | 6.470 ms |
| Gateway，64 KiB，并发32 | 7,145 | 4.310 ms | 7.732 ms |

Gateway 128B 吞吐约为直连的 45%，但它承担第二次网络往返、认证、目录/请求跟踪及额外
编解码。不能把这 55% 差额全部归因于字符串，也不能据此认定去掉复制就能恢复到直连速度。
64 KiB 的请求和回复都包含完整载荷；这些数值不是单向网卡带宽。

| 属性/Lua 场景 | 微秒/操作或完整一轮 |
| --- | ---: |
| 原生 scalar stage+publish，32属性 | 0.184 |
| 原生 vec3 stage+publish | 0.644 |
| 原生确认副本应用 vec3 delta | 0.817 |
| 32属性全量 encode+decode | 3.993 |
| Lua 本地 position.x 读取 | 0.682 |
| Lua 本地 position 整值赋值 | 2.881 |
| Lua 本地 position.x 赋值 | 2.968 |
| TCP + Lua 服务提交 + 更新到达，1接收者 | 253.595 |
| 同上，8接收者 | 317.957 |
| 同上，32接收者 | 879.289 |
| 128 Item 容器，逐次改16字段 | 28,575.690 / 轮 |
| 同上，batch16 | 2,907.136 / 轮 |

batch16 一轮约快 9.83 倍；100轮从1600次提案/提交降为100次，提案 FSA 字节从83,200降为
45,700。这些是 FSA 应用层大小，不包含 RPC、TLS 或 TCP 头。当前 1 KiB 结构单字段提案
54B、position.x 48B、128项 list 单元素替换52B。传输差量并不意味着整个运行时也只处理这几字节。

完整交付链路另跑三轮：独立客户端 → mTLS Gateway → mTLS owner → POSIX 文件 CAS/fsync →
确认同步，单账号串行1000次 vec3 双字段 batch，每次单独提交。中位数 **158.9轮/秒**，
父进程观察的1000轮时间为 **6.293秒**（范围5.888–6.528秒）。计时区间为观察到 recovered 至
load_done，不包含启动、证书生成和崩溃接管；不是逐请求延迟直方图或并发容量压测。
三轮还都通过 SIGKILL 接管、回执重放、优雅交接和资源回收。

启动器每轮固定 sleep 1 ms（service_host.cpp、client_host.cpp），异步存储结果也需下一轮
poll 才推进，串行请求会累计调度等待；还包含 TLS、Lua、持久化和复制。尚未用系统 profiler
分离它们各自占比，不能把该158.9数值或全部耗时解释为字符串瓶颈，更不能推算在线人数。

## Copy review：优先级和建议

以下是代码审阅结论；建议尚未应用到运行时，微基准原型的收益不作为端到端优化承诺。

### 1. Gateway 为读取 ID 复制完整 payload：可以避免

位置：[client_gateway.cpp](../src/client_gateway.cpp:175)、[entity_message.hpp](../include/fsnet/entity_message.hpp:22)。
`routed` 调用 owning 的 parse_entity_message，只取 id，解析器却分配并复制 payload，
随后马上销毁；原 input 接着移入 Request。应提供客户端/服务端共用的借用解析视图，
同步解析时不复制，只把后续需要的 ID 或完整接收缓冲拥有权交给异步对象。

Standalone envelope benchmark 对相同 FSE2 输入比较现有 owning parser 和仅用于 review 的
借用原型，校验 ID/payload/request ID 一致；每种载荷50,000次：

| payload | owning 时间 | 借用原型时间 | owning 分配次数 | 原型分配次数 |
| --- | ---: | ---: | ---: | ---: |
| 128B | 19.59 ns | 3.34 ns | 50,000 | 0 |
| 4 KiB | 59.84 ns | 3.33 ns | 50,000 | 0 |
| 64 KiB | 1,805.55 ns | 3.33 ns | 50,000 | 0 |

64KiB 场景累计请求分配量约3.28GB，是反复分配/释放的总和，不是驻留峰值。
借用原型不复制数据，只读取头和校验所用的少量字节；只适用于不需要独立持有整个内容的调用方。
不能拿它替代异步持有语义，也不能把约500倍局部比值宣称为RPC性能收益。

### 2. Gateway 属性转发完整解码再编码：值得优先处理

位置：[client_gateway.cpp](../src/client_gateway.cpp:222)、[转发](../src/client_gateway.cpp:253)、
[replication_wire.cpp](../src/replication_wire.cpp:29)。
为检查实体/版本并重映射 subscription，当前会 decode FSN1 → 构造完整 AttributeValue 树 →
重新 encode。每个订阅更新都重复属性编解码；转发节点本身不修改属性内容。
建议公共 codec 支持包头/版本检查和 ID 重映射，保留原属性字节。保留消息大小、身份、版本
和恶意封包检查，不能简单删除校验。本轮没有单独量化这段CPU占比。

### 3. 客户端每个 delta 后深拷贝完整确认快照：大实体尤其需要关注

位置：[client_script_host.hpp](../include/fsnet/client_script_host.hpp:24)。
image() 把 replica->values() 复制到新的 immutable AttributeUpdate，嵌套容器和字符串会递归
复制。网络收到48B字段补丁时，Lua适配仍可能复制整个大背包/结构，成本随总状态增长。
建议由原生副本持有可共享的不可变版本，或在共用值层引入结构共享；Lua/预测读取同一版本。
旧版本必须在仍被持有时保持不变，不能直接引用下一帧会被修改的 map。这需要独立的生命周期
与预算验证，不能仅添加 std::move，因为来源是 const 引用且原副本仍需保留。

### 4. Node::call 的按值参数并非必需

位置：[node.cpp](../src/node.cpp:354)、[Gateway调用](../src/client_gateway.cpp:181)、
[Router调用](../src/entity_router.cpp:92)。
call 内同步调用 frame(route,data)，随后只持有拥有数据的输出帧，Pending 不保存 route/data。
因此调用者传入 r->data / call->message 这种左值时，会先复制按值参数，再复制进帧。
可评估将这个同步读取边界改为 string_view；拥有数据的帧必须在返回前建好。

Router 会为重试保留 message，不能为减少这次复制直接 move 掉它；否则断线重试发送的数据
可能丢失。统一只读边界比在调用点盲目 std::move 更合适。改签名影响 C++ ABI，需全量重编；
不应在本轮 review 中静默替换公开API。

### 5. 实体内部状态封包有额外整块复制/扩容

位置：[entity_host.cpp](../src/entity_host.cpp:133)、[参数构造](../src/entity_host.cpp:158)、
[entity_message.hpp](../include/fsnet/entity_message.hpp:36)。
每次调用复制 job.state 到 chain.state，又把完整 state 打包成 FSS1、再包 FSE；解析时再次
构造拥有 state/payload 的字符串。`last ? job->request.payload : std::string{}` 还会产生一个
临时 payload 副本。entity_message/state_message 没有按总大小 reserve，可能再次扩容和复制。
可以先消除条件表达式临时值、预留精确容量，再评估共用拥有buffer+view的内部调用载体，
避免状态在C++宿主边界反复变成字符串。不能丢失原状态、回执或异常回滚边界。

单记录持久化目前仍保存整个实体快照；字段差量减少网络字节，不会自动减少存储的全值写入。
这个结构性成本与函数参数复制应分别评估。

### 6. 身份查询中的临时字符串可减少

位置：[node.cpp](../src/node.cpp:398)、[证书返回](../src/node.cpp:330)。
identity() 先将71B证书指纹复制到默认principal，再由自定义resolver覆盖，使用resolver时前一次
复制多余；receive() 有类似构造。peer_identity() 按值返回也会在Gateway委托轮询时反复分配。
优先改构造顺序，再设计同步查询边界，保留认证结果和异步上下文的拥有权；不返回可能在关闭
连接后悬空的长期引用。

## 哪些字符串应该保留拥有权

- `Node::Session::output` 的帧跨 async_write 存活，必须拥有或共享拥有buffer。
- handler 收到的数据可能跨 await/协程/下一次 read 存活，不能借用即将重用的 Session::body。
- Router 的重试请求、Gateway 的 pending 请求、持久化回执都需要稳定数据。
- Lua VM 的字符串不能在未固定其生命周期时直接借给异步C++任务。
- 注册路由、类名等进入容器长期保存，按值接收后 move 是合理用法；`const std::string&` 传参本身不复制。

建议后续顺序：共用借用解析与同步组帧边界 → Gateway保持属性字节转发 → 客户端确认快照结构
共享 → 内部状态载体与持久化成本。每步保留独立Release A/B样本和异步/重试/撤销测试。
本报告没有宣称已完成这些优化，也没有把微基准当成生产并发/时延验收。
