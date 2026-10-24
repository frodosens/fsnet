# Copy 优化实施与验证

基线为 `55de967`；[原始 review](PERFORMANCE_REVIEW.md) 描述优化前实现。
全部机制保持客户端/服务端共用，异步队列和重试请求继续拥有数据。

## 阶段一：同步借用边界

- 共用 FSE/FSS 借用解析器；原 owning API 在同一解析器上复制，保留独立生命周期。
- Gateway 路由只解析头，不复制业务 payload。
- Node::call 同步读取 string_view 并在返回前构造拥有数据的输出帧；发送队列继续持有帧。
- 内部实体状态移交给生命周期链，消除末次调用的条件表达式临时副本；封包按总大小 reserve。
- 使用自定义身份解析器时，不再先构造随后会被覆盖的证书指纹 subject。

普通 CTest 30/30 通过，新增输入销毁/覆盖后的异步发送、二进制借用解析和独立 owned 结果检查；
原 Router 回执重试、生命周期失败、预测与跨节点独立进程检查继续通过。
C++ Node::call 签名改变，嵌入方必须与新头文件一起重编，C ABI 和网络封包保持不变。

保存基线可执行文件及其 libfsnet.so，逐构建指定 LD_LIBRARY_PATH，预热后交替 A/B、B/A
三对，本机 Release / 明文 loopback / 并发32。不能混用新旧共享库。
[原始样本](benchmarks/copy-boundaries-2026-10-24.json)：64KiB direct 吞吐中位数
10,922 → 15,582 req/s；Gateway 6,727 → 7,132 req/s。128B/4KiB 没有稳定提升，
不将这些小幅波动描述为收益；大载荷少一次全值复制更容易被测到。
该实验未测TLS/持久化吞吐，也不把一次局部改善当作生产容量承诺。

```sh
python3 tools/compare_engine_benchmarks.py BEFORE_BUILD AFTER_BUILD --quick --runs 3 --output copy.json
```
