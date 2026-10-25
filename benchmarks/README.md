# 引擎性能夹具

使用 Release 构建，运行 `python3 tools/review_performance.py .dev/build/release --runs 3 --output report.json`。

- `fsnet-benchmark`：原生协议吞吐。
- `fsnet-request-benchmark`：直连和 Gateway 转发；不是完整实体持久提交耗时。
- `fsnet-envelope-benchmark`：公共封包解析与持有成本。
- `fsnet-attribute-benchmark all 20000`、`fsnet-container-benchmark all 20000`：同一公共值模型夹具，测量 stage/publish、64KiB 结构字段差量、投影应用及容器追加。
- `fsnet-replication-benchmark`：复制转发。
- `fsnet-global-flow-tests <源码目录> --benchmark`：统一实体的 Lua/CAS/Gateway 链路，输出 global_id 场景。

旧 API 对照程序已删除，历史结果仅代表当时版本；不能与当前改写后的夹具直接比较。
持久投递崩溃恢复与迁移正确性由 CTest 单独验证，不混入上述微基准。
