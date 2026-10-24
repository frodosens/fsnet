# 引擎性能夹具

这些程序测量公共运行时，不是游戏客户端/服务端示例。由CMake构建，程序名保持不变：
`fsnet-benchmark`、`fsnet-request-benchmark`、`fsnet-envelope-benchmark`、
`fsnet-attribute-benchmark`、`fsnet-container-benchmark`、`fsnet-replication-benchmark`。

请使用Release构建；统一入口是 `tools/review_performance.py`，新旧构建交替对照使用
`tools/compare_engine_benchmarks.py`。测量方法和边界见
[性能review](../docs/PERFORMANCE_REVIEW.md)、[优化记录](../docs/COPY_OPTIMIZATION.md)。
