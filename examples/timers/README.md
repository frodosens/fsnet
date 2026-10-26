# 持久计时器示例

从仓库根目录运行（需要先构建 Lua ON 的 Debug 配置）：

```sh
. tools/dev/activate.sh
cmake --preset debug
cmake --build --preset debug
python3 examples/run.py timers
```

其他构建目录用 `--build /path/to/build`。也可直接执行
`python3 examples/timers/run.py --build .dev/build/debug`。
已加入 CTest，可执行 `ctest --preset debug -R '^fsnet-example-timers$' --output-on-failure`。

启动器自动使用临时文件存储、随机 token、loopback 端口和固定演示身份，不继承外部存储/TLS。
注册任务后读取初始状态，SIGKILL 服务端，等待 4 秒再用同一存储和 Node ID 重启。
客户端重新连接，通过原 EntityId 查询恢复结果；所有进程退出后删除临时存储。

演示使用 2 秒 owner 租约，宿主据此给实体请求 1 秒预算；4 秒停机窗口仍超过旧租约。
状态查询与计时器回调共享实体 mailbox，也要经过文件存储的 fenced 提交和 fsync。
客户端使用 3 秒网络期限，每 100 毫秒查询一次，保持约 10 秒的轮询等待；
客户端期限不能扩大服务端请求预算。这里验证持久恢复与执行次数，不把共享 CI 上的
250 毫秒存储响应时间作为正确性要求。

## 脚本

- [server.lua](server.lua)：持久实体、任务注册、命名回调、幂等启动和只读 status RPC。
- [client.lua](client.lua)：使用启动器提供的 EntityId 查询实体，等待任务完成。
- [run.py](run.py)：真实宿主启动、强制终止、保留存储、重启与进程清理。

服务器安排五类任务：

| 任务 | 写法 | 最终状态 |
| --- | --- | --- |
| 延迟任务 | `self:after(2500,'finish_job','job-42',{key='job'})` | 执行一次，参数恢复为 job-42 |
| 指定时间 | `self:at(deadline,'alarm','',{key='alarm'})` | 过期后执行一次 |
| 周期合并 | `self:every(500,'periodic_tick','',{count=3,missed='coalesce'})` | 错过的周期合并一次，再继续至成功 3 次 |
| 周期补跑 | `self:every(500,'catch_up_tick','',{count=3,missed='catch_up'})` | 按原到期点逐次执行 3 次 |
| 取消任务 | `self:cancel_timer(id)` | 始终为 0 |

这里的周期任务都用 `delay_ms=2500` 延后第一次触发，便于先展示停机前的状态。
指定时间取自 `self:timer('job').scheduled_at+250`，是 UTC 毫秒值，不是相对延迟。
`count` 表示实际成功次数，合并多个错过的周期仍只消耗一次。

关键输出（中间进度随调度时机变化）：

```text
Created durable timer entity <id>
state delayed:periodic:catch_up:absolute:cancelled:job = 0:0:0:0:0:pending
SIGKILL server; wait 4 seconds with its file storage preserved.
Restarted with the same entity <相同 id>
...
state delayed:periodic:catch_up:absolute:cancelled:job = 1:3:3:1:0:job-42
timers complete
PASS: delayed/absolute tasks recovered, periodic counts completed, cancelled task stayed cancelled.
```

`initialize` 的持久标志与任务注册同事务提交，重启时不会重新计时或重置次数。
回调使用普通实体方法，不导出为客户端 RPC；保存方法名和 bytes 参数，不保存闭包。
客户端没有排任务或改计数的权限，示例只公开读取状态。
传给 `runtime.start` 的闭包只是当前进程的执行入口，不属于持久任务。

这里只修改实体状态；若回调调用外部系统，要另外处理幂等或使用持久 outbox。
这是本机重启示例，不包含账号登录、重连授权恢复或跨节点迁移。
完整 API、替换规则与失败恢复见 [计时器文档](../../docs/ENTITY_TIMERS.md)。
