# 可替换存储

框架依赖 Storage，不依赖数据库事务。实现只需提供：

| 操作 | 最低合同 |
| --- | --- |
| get(key) | 返回同一时刻的一份完整值和版本，或不存在 |
| compare_exchange(key, expected, value) | 版本匹配才原子、持久化替换；expected=0 表示创建不存在的 key |
| erase(key, expected) | 按版本删除；删除后重建不能重复旧版本 |

事务是可选扩展，默认抛“不支持”；实体运行时不调用它。用户自定义后端无需实现 SQL 或多 key 事务。成功确认意味着后端已达到其承诺的持久化级别；I/O 错误可能发生在写入已生效之后，调用方需要请求 ID 与幂等重试。

## 文件后端

```cpp
auto storage = fsnet::open_storage("file:/srv/fsnet/state");
```

Lua 启动器配置 FSNET_STORAGE_URI=file:/srv/fsnet/state。memory: 用于开发；sqlite:/path/state.sqlite 仍可选。FSNET_DATABASE_PATH 保留为早期 SQLite 配置兼容项。

文件后端面向本地 POSIX 文件系统，多个线程/进程可打开同一目录。每个 key 映射到哈希分片下的稳定锁文件和记录文件；记录包含原 key、版本、tombstone 标志、值和 SHA-256 完整性校验。

提交过程：取得跨进程排他锁 → 校验预期版本 → 写独立临时文件 → fsync 文件 → 同目录原子 rename → fsync 目录 → 返回成功。进程退出自动释放锁；锁等待有 5 秒上限。读取使用共享锁，不会看到半个记录。损坏记录报错，不静默丢弃或覆盖。

delete 以 tombstone 替换记录，重建时沿用并增加版本；不能手工删除 tombstone/锁文件来回收空间，否则可能破坏 ABA 防护或跨进程锁。崩溃留下的 .tmp 文件不参与读取，可在全部实例停机后清理。根目录及父目录应在部署时预先创建并确保权限与持久化；数据文件权限 0600，目录挂载必须支持所需的 flock、rename、fsync 语义。

这不是文件复制协议，也不宣称任意 NFS、对象存储或网络分区下的共享盘都满足合同。跨机部署必须选用已验证的共享/复制存储实现；独立本地目录只能用于独立分片或单机多进程。恢复旧备份前必须停止旧 writer 并执行明确的恢复/版本隔离流程，不能在在线实例下直接替换文件。

## 实体记录与升级

托管实体把 owner、租约、fencing、状态与请求结果保存在单个 FER1/FER2/FER3 记录中。提交与接管都只进行一次单记录 CAS。底层 EntityStore 同样采用单记录格式；其辅助记录与托管实体格式不同，不应混读。

此前分支版本把 owner 和状态分开保存。新运行时检测到旧记录时拒绝初始化，避免静默重置。升级这些实验数据需要先停止全部旧实例，再离线转换；不能混跑旧的双 key 协议与新的单记录协议。已提供 fsnet-storage-migrate type id [--raw-state]：从 FSNET_STORAGE_URI 读取存储地址，拒绝仍有效的旧租约，保留旧 key，仅创建新记录；重复执行不覆盖目标。旧的纯状态恰好以 FSP1 开头时显式加 --raw-state。该命令不是在线迁移协议，执行前必须停止全部旧实例。

## 跨实体业务

两个实体的状态不要求同一事务提交。转账、交易、跨场景移动等流程应使用脚本定义的 Saga/幂等消息；需要可靠发送时把 outbox 内容放进本实体状态，和状态一起 CAS，再异步投递并确认。整个流程的业务补偿规则由脚本定义，不能由 C++ 网络框架猜测。

## 可选 PostgreSQL

`-DFSNET_WITH_POSTGRES=ON` 从固定 PostgreSQL 17.11 源码构建 libpq，并链接固定源码构建的 OpenSSL；不查找本机私有数据库客户端库。编译 Git 源码需要 Bison、Flex 和 m4。默认关闭，不影响文件/内存/SQLite 独立运行。

使用 open_storage('postgresql://...') 或 postgres_database(libpq_options)。远端连接必须 sslmode=verify-full，校验服务端证书与目标名称；库不会把公共连接降级为明文。驱动强制连接超时 5 秒、语句/锁超时 5 秒、target_session_attrs=read-write 和 synchronous_commit=on。凭据通过安全配置/环境提供，不写入日志或仓库。

单记录 CAS 使用带版本条件的 INSERT/UPDATE，删除同样带版本条件；版本使用不回退的 PostgreSQL sequence。该适配器不提供多记录事务扩展，实体运行时也不需要。所有权、状态与去重仍是同一行的一次条件替换。字节参数使用 libpq 二进制参数绑定，不拼接业务 key/value。

安全的读取在断线后可重连一次；写入不在驱动中自动重放，避免 COMMIT/网络状态不明时重复副作用。上层带请求 ID 的实体调用负责幂等重试。一个客户端连接由 mutex 串行使用，可配置多个 Storage/执行器实例进行分片与连接并发。

`-DFSNET_POSTGRES_TEST_SERVER=ON` 额外编译私有测试服务端。测试临时初始化数据库与证书，验证 TLS、同一存储合同、多个独立连接 CAS、进程 owner 接管，以及 immediate stop 后 WAL 恢复和读取重连。测试的 trust 身份配置仅用于本机临时实例；生产应设置 SCRAM/证书等数据库权限、备份和同步复制，不能把单台 PostgreSQL 当作 HA 集群。异步复制丢失已确认写入的部署不满足 Storage 的线性一致/持久化合同。

指定目标交接使用 FER3，把目标节点和交接编号与最终状态、租约释放一起 CAS 提交。
FER3 的 placement 在后续提交/重启中保留；旧程序不支持该格式，启用前必须升级全部目录读者和 owner。
详见[实体交接](ENTITY_TRANSFER.md)。
