# 2026-10-26 生产审查复现夹具

这些程序归档了基线 `1af85fb2303976d2f7619ab07bc6270b13fb15de` 的负向观察。
它们用于复现审查发现，不属于回归测试，也不把进程退出或被外部超时终止计作测试通过。
修复后观察变化应交由人工解释，并另写针对正确行为的回归测试。

所有网络夹具只连接自身创建的 `127.0.0.1` 临时服务。程序不读取生产数据库，
不修改引擎源码，不下载依赖。证书仅在 `launcher` 的临时目录现场生成并清理，
仓库没有归档证书或私钥。

先按仓库开发文档完成 Lua Release 构建，再运行：

```sh
source tools/dev/activate.sh
python3 tools/review/production_2026_10_26/run.py --list
python3 tools/review/production_2026_10_26/run.py --only entity_runtime client_activation
python3 tools/review/production_2026_10_26/run.py
```

`--build-dir` 默认 `.dev/build/release`，相对路径以仓库根目录为基准。
编译产物、原始 stdout/stderr 和 `observations.json` 写入忽略的
`.dev/production-review/repro/<build-tag>/`；每个编译和探针进程有外部期限。
`CXX` 可选择编译器。Lua 夹具链接该构建目录的两个 Lua 静态库和 `libfsnet`。
runner 的退出状态表示编译/记录流程是否完成；**不能解释为审查问题已经修复**。

| 探针 | 基线观察与前提 |
| --- | --- |
| `entity_runtime` | 空 schema 的 rebuildable 实体，同 request_id 连续执行两次；durable 对照执行一次。原生异步 handler 不回复时，20ms 期限后 mailbox 仍 busy。 |
| `entity_exception` | 原生 handler 回调中注入一次 `bad_alloc`，分配器恢复后仍不完成。仅故障注入，不说明远程可触发。 |
| `client_activation` | 非 owner/observer 的订阅最终被拒绝，但先激活实体并提交生命周期修改；调用者已知有效 ID/type。 |
| `client_lookup` | 同一实体自动出现且被手动 watch 时，按 ID 和按 EntityKey 得到不同投影/归属元数据。 |
| `gateway_connection_change` | **可信自定义 identity adapter** 在同一物理连接、同 subject 下更换 ConnectionRef 后，旧 owner 私有更新仍被转发；默认 launcher 不进行这种重绑定。 |
| `router_deadline` | 三秒调用期限、1.5 秒正常处理：直连成功，路由约一秒超时；不带 request_id。 |
| `lua_gc_shutdown` | 终结器停机循环，输出 `LOAD_OK/GC_ENTER`，外部两秒终止。预期出现 `external_deadline` 这一缺陷观察，绝非通过测试。 |
| `delivery_registry` | 夹具将发送者上限调为 2；两个发送者完成并销毁后，注册表仍满，第三个不能入队。属于已有文档说明的回收/容量限制。 |
| `storage_completion_oom` | 合成 worker 完成队列分配失败，terminate handler 以 73 退出并打印堆栈；不是实际系统耗尽内存，也不证明网络触发条件。 |
| `token_echo` | 本地明文 fixture 的客户端回显服务器首帧即可通过 token 相等检查；不绕过 mTLS、角色或业务路由鉴权。 |
| `launcher`（显式选用） | 临时 mTLS 配置下，Lua bootstrap 报错后 ready 仍为 true；持久存储检查接受 `sqlite::memory:`。 |
| `lua_native_budget`（显式选用） | Lua 2MiB 配额不约束 schema 转换的全部原生分配；示例宽容器使 RSS 明显超过 VM 配额，最终请求失败。会产生约数百 MiB 短时分配，不是容量基准。 |
| `transport_poll_budget`（显式选用） | 两秒合法通知流下观察单次 `Node::poll` 长时间占用。只表示缺少每次 poll 公平预算，不预测公网/mTLS 延迟或吞吐。 |
| `storage_memory_copy`（显式选用） | 记录一个 CAS 的分配随内存数据库总记录数增长。memory 后端本来定位开发使用。 |
| `pg_blackhole`（显式选用） | 成功连接本地 PG 后，代理丢弃双向流量；七秒后操作和 stop 仍未返回。server statement_timeout 不能约束该网络故障。 |

负载、内存和 PostgreSQL 诊断不在默认集合中：

```sh
python3 tools/review/production_2026_10_26/run.py --only lua_native_budget
python3 tools/review/production_2026_10_26/run.py --only transport_poll_budget
python3 tools/review/production_2026_10_26/run.py --only storage_memory_copy launcher
python3 tools/review/production_2026_10_26/run.py \
  --build-dir .dev/build/postgres --only pg_blackhole
```

PG 探针需要启用 PostgreSQL 的构建、非 root 开发账户及本地 PostgreSQL 安装；
安装前缀默认 `<build-dir>/postgres-install`，可用 `--pg-install` 指定已有安装。
它创建临时数据目录和回环端口，流量丢弃仅作用于该探针，不配置系统防火墙。
其连接明确关闭 TLS 来隔离本地故障，与生产连接配置无关。
若构建目录使用外部动态依赖，请像正常运行该构建一样设置 `LD_LIBRARY_PATH`。
