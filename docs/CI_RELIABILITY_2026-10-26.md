# CI 失败排查与持久化进程测试期限

排查基线：master `fd87e40ab593ea18c2185b2f61214e810b59cffa`。

## 当前失败

[master CMake run 40288042895](https://github.com/frodosens/fsnet/actions/runs/40288042895)
只有 `postgres` job 失败。该 job 运行完整测试集，并非只运行数据库测试：

- `fsnet-postgres` 通过，耗时约 21 秒。
- 35 项测试中 34 项通过；失败的是 `fsnet-example-timers`。
- 服务端重启后已恢复相同 EntityId，并执行到 `1:2:3:1:0:job-42`，随后状态 RPC 返回 timeout（3）。
- 其余 Debug/Lua ON、Debug/Lua OFF、Release/Lua ON、sanitize 四个 job 通过。

示例设置 500ms 租约，宿主由此将请求预算设成 250ms。状态方法虽然只读取业务属性，
仍经过实体 mailbox、租约检查和持久化 CAS；它与计时器回调共享邮箱，文件提交还需要
root/data/directory fsync。共享 runner 的磁盘延迟会消耗这段预算，客户端延长期限无法改变服务端预算。

## 历史红记录

排查时最近 50 次 run 中，37 次成功、11 次失败、2 次正常跳过。失败集中在：

- 初次接入 Windows SDK：SQLiteCpp 缺整数类型声明、缺 mswsock 链接、MSYS2 缺 Git；当前均已修复。
- 初期 PostgreSQL 外部构建配置；恢复普通构建的 OpenSSL shared 默认后，后续构建通过。
- SDK 发布草稿按 tag 查询返回 404；当前发布器已改用 numeric release ID。
- 旧 master [run 40264337088](https://github.com/frodosens/fsnet/actions/runs/40264337088)
  的 `postgres` job 实际失败于 `fsnet-current-delivery-process`：实体创建失败，未到预定的 exit 42 崩溃点。
  这个文件存储进程用例也只给请求 200ms，慢 fsync 可以复现同样失败。

历史失败不会因为后续修复而自动变绿；应检查当前提交的 job 和具体失败测试。

## 修复范围

| 持久化进程用例 | 原租约 / 请求预算 | 修复后 |
| --- | --- | --- |
| 持久计时器示例 | 500ms / 250ms | 2000ms / 1000ms；4 秒停机仍超过旧租约 |
| 计时器进程恢复测试 | 500ms / 250ms | 2000ms / 1000ms；停机改为 2.5 秒，继续覆盖租约过期恢复 |
| 持久投递崩溃恢复测试 | 500ms / 200ms | 4000ms / 2000ms，仅用于持久存储；内存用例保留原期限 |

计时器查询显式采用 3000ms 网络期限，示例轮询从 20ms 改成 100ms。
引擎的 deadline、租约 fencing、持久化 fsync、精确计数和不重复执行断言均保留；没有重试业务 RPC 来掩盖失败。
投递实体创建失败会输出实际 Status，便于区分超时与其他错误。

## 验证与复现

本地 Release 已重新构建相关可执行文件，计时器、计时器进程、计时器示例、
投递与投递进程五项定向测试均通过。另做真实 fsync 延迟注入：

- 每次 fsync 延迟 50ms，进程先正常运行 800ms：旧计时器示例复现 `RPC failed: 3`，修复版完成同 ID 恢复、精确计数和不重放检查。
- 每次 fsync 延迟 75ms：旧持久投递用例复现 `delivery entity creation`；修复版按预定位置 exit 42，重启后 reward 仍为 3，重复投递不重复执行。
- 修复后的计时器进程用例在相同 50ms fsync 注入下通过两次重启、不重放和稳定 ID 检查。

Linux 开发环境可使用独立探针复现慢盘；它不链接进 fsnet，仍调用真正的 fsync：

```sh
mkdir -p .dev/ci-repro
cc -shared -fPIC tools/review/ci_reliability/slow_fsync.c \
  -o .dev/ci-repro/slow_fsync.so -ldl -pthread
LD_PRELOAD="$PWD/.dev/ci-repro/slow_fsync.so" \
  FSNET_TEST_FSYNC_DELAY_MS=50 FSNET_TEST_FSYNC_SLOW_AFTER_MS=800 \
  python3 examples/timers/run.py --build .dev/build/release
LD_PRELOAD="$PWD/.dev/ci-repro/slow_fsync.so" \
  FSNET_TEST_FSYNC_DELAY_MS=75 FSNET_TEST_FSYNC_SLOW_AFTER_MS=0 \
  python3 tests/current_delivery_process.py .dev/build/release/fsnet-current-delivery-tests
```
