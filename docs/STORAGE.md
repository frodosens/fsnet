# 可替换存储

框架依赖 Storage，不依赖数据库事务。实现只需提供：

| 操作 | 最低合同 |
| --- | --- |
| get(key) | 返回同一时刻的一份完整值和版本，或不存在 |
| compare_exchange(key, expected, value) | 版本匹配才原子、持久化替换；expected=0 表示创建不存在的 key |
| erase(key, expected) | 按版本删除；删除后重建不能重复旧版本 |

事务是可选扩展，默认抛“不支持”；实体运行时不调用它。用户自定义后端无需实现 SQL 或多 key 事务。成功确认意味着后端已达到其承诺的持久化级别；I/O 错误可能发生在写入已生效之后，调用方需要请求 ID 与幂等重试。

`Storage::persistent()` 表示该实例的提交能否跨进程重启保留，未知后端默认 false。
Memory、SQLite `:memory:` 和临时空路径数据库返回 false；File、文件 SQLite 和 PostgreSQL 返回 true。
SQLite 按实际连接的数据库文件判断，不以 URI 前缀猜测。生产 `fsnet-service` 要求显式 URI 且该能力为 true；开发宿主仍可使用易失后端。
此能力声明不等于多机 HA、备份或设备故障持久性验收。

## 文件后端

```cpp
auto storage = fsnet::open_storage("file:/srv/fsnet/state");
```

启动器统一读取 `FSNET_STORAGE_URI`：支持 `memory:`、`file:<目录>`、`sqlite:<文件>`，
以及启用 PostgreSQL 后的 `postgresql://...` / `postgres://...`；未设置时使用内存存储。

文件后端面向本地 POSIX 文件系统，多个线程/进程可打开同一目录。每个 key 映射到哈希分片下的稳定锁文件和记录文件；记录包含原 key、版本、tombstone 标志、值和 SHA-256 完整性校验。

提交过程：取得跨进程排他锁 → 校验预期版本 → 写独立临时文件 → fsync 文件 → 同目录原子 rename → fsync 目录 → 返回成功。进程退出自动释放锁；锁等待有 5 秒上限。读取直接打开不可变的原子记录，不会看到半个记录。损坏记录报错，不静默丢弃或覆盖。

记录以 `O_NONBLOCK` 打开后检查 inode 类型；错误的 FIFO 记录不会在 `open` 等待 writer，
get/CAS/erase 均拒绝非普通文件。该标志不使普通磁盘 I/O 可取消，挂载或设备故障仍需系统级停止期限。

delete 以 tombstone 替换记录，重建时沿用并增加版本；不能手工删除 tombstone/锁文件来回收空间，否则可能破坏 ABA 防护或跨进程锁。只读 get 直接读取原子替换的不可变记录，不为 miss 创建目录或锁；写入仍使用稳定的逐 key 锁。崩溃留下的 .tmp 文件不参与读取，可在全部实例停机后清理。根目录及父目录应在部署时预先创建并确保权限与持久化；数据文件权限 0600，目录挂载必须支持所需的 flock、rename、fsync 语义。

这不是文件复制协议，也不宣称任意 NFS、对象存储或网络分区下的共享盘都满足合同。跨机部署必须选用已验证的共享/复制存储实现；独立本地目录只能用于独立分片或单机多进程。恢复旧备份前必须停止旧 writer 并执行明确的恢复/版本隔离流程，不能在在线实例下直接替换文件。

## 实体记录与格式边界

全局实体使用 FER5 保存 EntityId 对应的类型/创建摘要、逻辑连接归属、placement、运行节点租约/fence、快照和请求结果。提交与接管都使用单记录 CAS，创建 claim 由 EntityCatalog 持久保存；销毁保留身份墓碑。

实体记录只读写 FER5，缺失/非法全局元数据、FER1/2/3/4 和未知版本均拒绝。实体 key 仅使用
规范 EntityId，不查找旧类型/字符串 ID key。计时器发现索引只读写 FTI2，不再读取 FTI1。
不支持旧存档恢复、离线迁移或自动转换；遇到不支持的格式报错，原记录不会被静默重置/覆盖。


## 跨实体业务

两个实体的状态不要求同一事务提交。转账、交易、跨场景移动等流程应使用脚本定义的 Saga/幂等消息；需要可靠发送时使用框架 `ref:send`，把 outbox 与源状态一起 CAS，再异步投递并确认；接收方状态与 inbox 水位共同提交。整个流程的业务补偿规则由脚本定义，不能由 C++ 网络框架猜测。

命令元数据在没有计时器状态时以 FSP2 保存，同时含计时器时使用 FSP3。持久源索引先于源最终 CAS 登记，崩溃最多留下空登记项，不需要
多记录事务或存储扫描 API。索引与水位均有明确容量限制，不自动淘汰。升级与完整合同见
[ENTITY_DELIVERY.md](ENTITY_DELIVERY.md)。

## 可选 PostgreSQL

`-DFSNET_WITH_POSTGRES=ON` 从固定 PostgreSQL 17.11 源码构建 libpq，并链接固定源码构建的 OpenSSL；不查找本机私有数据库客户端库。编译 Git 源码需要 Bison、Flex 和 m4。默认关闭，不影响文件/内存/SQLite 独立运行。

使用 open_storage('postgresql://...') 或 postgres_database(libpq_options)。远端连接必须 sslmode=verify-full，校验服务端证书与目标名称；库不会把公共连接降级为明文。驱动强制连接超时 5 秒、语句/锁超时 5 秒、target_session_attrs=read-write 和 synchronous_commit=on。凭据通过安全配置/环境提供，不写入日志或仓库。

目标检查在连接与认证之前执行，覆盖显式 host/hostaddr 和 libpq 环境默认值。service 配置不能在预检查时证明为本地，因此必须使用 verify-full；建立连接后的 TLS 检查作为额外保护。

单记录 CAS 使用带版本条件的 INSERT/UPDATE，删除同样带版本条件；版本使用不回退的 PostgreSQL sequence。该适配器不提供多记录事务扩展，实体运行时也不需要。所有权、状态与去重仍是同一行的一次条件替换。字节参数使用 libpq 二进制参数绑定，不拼接业务 key/value。

客户端采用非阻塞 libpq 与 socket poll：连接/重连、连接锁等待、发送及接收共享一次公开操作的 5 秒总期限，连接成功后的网络黑洞同样受约束。
libpq 的异步连接仍可能同步调用系统 DNS，因此驱动在连接前要求地址可直接使用：数字 IPv4/IPv6 自动转为 hostaddr，localhost 固定选择 IPv4 loopback 127.0.0.1，Unix socket 路径及空 host 默认 Unix socket 不需要 DNS。
其他 DNS 名称必须同时提供对应的数字 hostaddr，host 保留用于 verify-full 证书名称验证；例如 `host=db.internal hostaddr=10.0.0.11 sslmode=verify-full`，不能以地址代替预期的证书名称。
host/hostaddr 列表最多 8 项，数量必须匹配；hostaddr 只能是数字地址，Unix socket 对应项可留空。使用 service 时还必须显式或通过环境同时提供 host 和数字 hostaddr，避免 service 隐藏的主机名进入阻塞解析。
这些检查在连接/认证之前执行，不依赖 DNS 故障返回。服务配置、证书/凭据等本地文件系统调用仍不能由 socket 期限强行抢占，部署需保留系统级停止期限。
安全的读取在断线后可在剩余期限内重连一次；写入不在驱动中自动重放，避免 COMMIT/网络状态不明时重复副作用。上层带请求 ID 的实体调用负责幂等重试。一个客户端连接由 timed_mutex 串行使用，可配置多个 Storage/执行器实例进行分片与连接并发。

AsyncDatabase 在 admission 时分配队列节点，worker 到完成队列的交接不再分配内存。
`stop()` 默认排空；`stop(StopMode::cancel_queued)` 取消未开始的操作并保留可 poll 的失败完成，当前已开始操作等待后端返回。
框架组件快速 stop 使用 cancel_queued，不能用它撤销已提交写入，也不能强制取消任意自定义 Storage 中的阻塞代码。

`-DFSNET_POSTGRES_TEST_SERVER=ON` 额外编译私有测试服务端。测试临时初始化数据库与证书，验证 TLS、同一存储合同、多个独立连接 CAS、进程 owner 接管，以及 immediate stop 后 WAL 恢复和读取重连。测试的 trust 身份配置仅用于本机临时实例；生产应设置 SCRAM/证书等数据库权限、备份和同步复制，不能把单台 PostgreSQL 当作 HA 集群。异步复制丢失已确认写入的部署不满足 Storage 的线性一致/持久化合同。

指定目标交接仍使用 FER5，把目标节点和交接编号与最终状态、租约释放一起 CAS 提交。
placement 在后续提交/重启中保留；所有目录读者和 owner 必须使用同一当前格式合同。
详见[实体交接](ENTITY_TRANSFER.md)。
