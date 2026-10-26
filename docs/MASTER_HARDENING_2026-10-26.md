# master 生产加固复核（2026-10-26）

后续已补 Lua OFF、TSan、PostgreSQL 和300秒资源稳定性验证，并修复新发现的空闲连接问题；
最新结果见 [补充验证](ADDITIONAL_VALIDATION_2026-10-26.md)。本文保留第一阶段的准确基线与实测记录。

## 基线与范围

基线为远程 master `8e4af0c9b73445d999a9e3593c1429c63bbec882`。
初始 `/workspace/fsnet` 位于干净的 `work@1af85fb`；先获取远程引用，确认 master 已合并
`refactor/global-entities@1fcc535`（文件树相同），`refactor/script-runtime@3f853ee` 为其祖先。
历史 `03d125f` 不是本轮基线。创建本地 `review/production-hardening` 分支；原 work 分支及
`/workspace/fsnet-env` 均保留，没有恢复旧未提交代码的声明。本轮不 push、PR、合并或部署。

先读取 AGENTS、README、构建/验证说明、架构、存储与生产文档及最近修复报告；无目录技能待加载。
审查覆盖 Node 的帧/认证/期限、异步存储队列、CAS/文件持久化、实体 ID/catalog、目录路由、
交接、投递、计时器发现与编解码、客户端回调和公共属性编解码。本轮是有边界的生产加固，
并非形式化验证或对所有代码路径的无缺陷证明。

fsnet 是 C++20/Lua 5.4 实体化游戏网络框架。C++ 承担传输、实体身份/调度、复制、存储及脚本宿主；
Lua 实现业务。Node/Client/Lua 在拥有者线程 poll，AsyncDatabase 在单独 worker 执行存储，完成回调回到
拥有者线程。同实体 mailbox 串行；状态、lease/fence、回执和 outbox/inbox 通过单记录 CAS 提交。
客户端与服务端共用属性模型、patch、编解码和预测机制，权限/持久化决定保留在服务端。
这与 AGENTS 的明确方向一致；本轮没有重写 API、改变协议或在 C++ 添加游戏规则。

## 已验证问题（严重度排序）

两个问题均为 P2：非默认配置或本地存储异常导致可用性故障，未发现它们可被普通远端客户端直接利用。

1. **P2：持久投递/交接/路由接收了底层不能执行的期限。**
   `src/entity_delivery.cpp:24`、`src/entity_transfer.cpp:37`、`src/entity_router.cpp:127,146`，
   对应底层 `src/node.cpp:484`。旧上层允许 5 分钟，而 Node 拒绝超过 30 秒的调用。
   有键 router 会不断重试本地 rejected，直至剩余期限小于 30 秒才真正发出；投递的固定 31 秒
   配置会不断失败，交接会被拒绝。旧库实测 31s keyed 请求经过 200ms：pending=1、业务执行=0、无完成。
   修复为共同的 `Node::max_call_timeout=30000ms`，入队/安装路由之前拒绝越界；没有扩大网络资源占用上限。
   这明确收紧了先前“接受但不能正常执行”的 30s–5min 配置，调用方应使用 1–30000ms。
   新回归在旧库上报 `invalid router call rejected before admission`，修复库通过。
   覆盖 0、负数、30001ms、300000ms、有/无键、owner 换绑、构造失败无路由残留，以及合法 30 秒调用。

2. **P2：POSIX 非普通记录可能在完整性校验前永久阻塞。**
   `src/file_storage.cpp:79–87`。旧实现 `openat(O_RDONLY)` 后才 fstat 检查类型；记录路径意外成为
   无 writer 的 FIFO 时，读取阻塞，CAS/erase 也经同一路径阻塞，worker stop/join 无法结束。
   基线诊断和新回归均被 timeout 终止（exit 124）；普通损坏字节测试不会发现此问题。
   修复添加 O_NONBLOCK，再对打开的同一 inode 做既有类型/大小校验，保留 O_NOFOLLOW 和原子 rename 读取语义。
   回归验证 get/CAS/erase 拒绝 FIFO、不替换坏记录、恢复原普通文件后能读写。
   这不保证普通磁盘、挂载故障或自定义 Storage 的 I/O 可取消。

## 验证

| 配置 | 基线 | 修复后 |
| --- | --- | --- |
| Release，Lua ON，PG OFF | 34/34，24.71s | 34/34，24.02s |
| Debug + ASan/UBSan，Lua ON，PG OFF | 本轮未跑旧版 sanitizer | 24/24，20.01s |
| git diff --check、性能脚本 Python 语法 | — | 通过 |

ASan 使用 detect_leaks=1:halt_on_error=1、UBSan halt_on_error=1；独立进程/安装消费者测试由
普通 Release 覆盖，包括 mTLS、SDK 安装重定位、计时器与持久投递进程恢复和交付示例。
第一阶段没有重跑 PostgreSQL、Lua OFF、TSan；随后已补测，详见补充报告。其他 OS/编译器仍未验证。
结构化逐项结果及产物哈希见 `validation/master-hardening-2026-10-26.json`。

## 性能实测

Debian 13 x86_64，Linux 6.18.55，AMD EPYC 9V74，cgroup CPU 配额 `400000 100000`
（4 核时间），内存上限 16 GiB；GCC 14.2.0、CMake 4.4.4、Ninja 1.13.2。
Release 为 `-O3 -DNDEBUG`，Lua ON、PG OFF。没有线上流量或外部服务压测。

现有 `tools/review_performance.py` 对每项先 warmup，再串行 3 次，包含：原生 100000 次 TCP RPC
（128B/并发128）、直连与 Gateway（128B/20000次、4096B/10000次、65536B/1000次，并发32；
另测128B/5000次/并发1）、公共值模型/patch、parser、复制。另跑 Lua 全链路各 3 次：真实
loopback Client → Gateway → Lua Player → Memory CAS，2000 次 move、并发8。
这是完整应用调用路径，但不是磁盘持久化、多机或 TLS 吞吐；native echo 不含每请求 Lua/CAS，
parser/value 是微基准，不能用其速率估算在线人数。

微基准三轮中位数（µs/op；是独立进程的分时段测量，不作吞吐提升承诺）：

| 微基准 | 基线 | 修复后 |
| --- | ---: | ---: |
| 32 字段 stage/publish | 0.203 | 0.180 |
| 64KiB 稀疏字段编码 | 10.509 | 10.100 |
| 64KiB 稀疏字段副本应用 | 17.808 | 17.381 |
| 复制 validate/remap | 28.493 | 29.087 |

首次分时段三轮中位数：native 126015 → 129306 rps；Gateway 128B/并发32 为
57148 → 54068 rps；Lua 全链路为 8276 → 6670 rps（约 -19.4%）。未隐藏不利样本，也未据此
直接宣称优化或代码退化。为排除时段与夹具差异，追加相同可执行文件、交替加载 A/B 库的 5 对复测。
master 核心从 git archive 完整重编，SHA256 与首次基线库逐字节相同：
`b74a54954e03d8267d248b7d23a2eb4302e86870b49b20eed02e8d3609ed0415`。
Lua 测量使用同一旧版基准夹具，因为新版增加的负向回归本就应在旧库失败；业务测量循环未变。
所有编译及测试结束后才开始配对计时。配对结果与原始样本保留在性能 JSON，不混合各轮 p99 为全局分位数。

五对交替测量（各轮指标的中位数；延迟统一为 µs）：

| 场景 | 基线 rps | 修复 rps | 基线 p50/p99 | 修复 p50/p99 |
| --- | ---: | ---: | ---: | ---: |
| 原生 TCP，128B/并发128 | 126811 | 131183 | 977.7 / 1605.5 | 977.7 / 1461.4 |
| Gateway echo，128B/并发32 | 56942.8 | 56179.4 | 534.8 / 951.3 | 541.8 / 955.9 |
| Lua/Gateway/Memory-CAS，move/并发8 | 7251.1 | 7593.4 | 1077.6 / 1778.6 | 956.6 / 1591.2 |

Lua 吞吐样本区间基线 6579–7808、修复 6802–8798 rps，明显重叠；native 和 Gateway 也有波动。
首次 -19.4% 没有在交替复测中重现，不能将其归为已证实的代码回退；同样不能把配对 +4.7% 当作优化收益。
本轮未做性能机制改写，结论为未观察到稳定的正常负载退化，目标生产容量仍未验收。
原始 stdout、命令、重复序号和二进制哈希见 [完整性能证据](benchmarks/master-hardening-2026-10-26.json)。

复现（从仓库根目录，先激活工具）：

```sh
. tools/dev/activate.sh
cmake --preset release
cmake --build --preset release
ctest --preset release
python3 tools/review_performance.py .dev/build/release --runs 3 --output .dev/performance.json
python3 tools/review/master_hardening/paired.py .dev/build/release .dev/paired --build-only
# 等待其他编译/测试结束后：
python3 tools/review/master_hardening/paired.py .dev/build/release .dev/paired
cmake --preset asan
cmake --build --preset asan
ctest --preset asan
```

## 未解决的上线门槛

- 目标部署的多机共享存储线性一致性/HA、网络分区、时钟偏移、备份恢复与 fencing 隔离、长期容量及 SLO
  仍需实测；本机恢复测试不能替代这些条件。内存全表候选提交和整实体记录 CAS 的成本仍在，不能外推大型世界容量。
- 无统一覆盖全局实体+mTLS+双 owner 故障+负载的一键验收；readiness 仍依赖接入方阻止未就绪业务流量。
- 当前格式限定 FER5/FTI2，旧存档迁移、在线 schema/模块热更新未交付；升级和回滚必须有明确数据策略。
- P2P 的认证信令、加密及后备链路，公网跨 NAT 验收未完成；投递索引/inbox 水位有容量上限。
- 单个 native 回调不可抢占，文件或自定义存储故障不能由框架强制取消；部署需要系统级停止时限。
- 本轮性能不是生产容量承诺；共享容器样本存在波动，仍需目标硬件、持久后端、TLS 和实际业务场景验收。
