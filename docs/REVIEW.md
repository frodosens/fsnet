# 代码审查与设计结论

审查基线：`69a4b62e52ff1874d25f7e64cdb176829f2024be`。以下问题指原有实现，不能将新核心的测试结果当成旧代码已修复的证据。

## 仓库结构与执行路径

- `FSNet/`：C/libevent 网络线程、流和 Ruby 原生绑定。每个 server 自建 I/O 线程，回调进入全局队列，再由 Ruby 主循环在 Fiber 中派发。
- `scripts/dist_server/`：2016 年版本的 gate/logic/center。逻辑节点声明服务，gate 随机绑定实例，center 连接并探测逻辑节点；RPC 通过方法名与动态类型数组编码。
- `scripts/rubylib/` 和 `demo_servers/`：HTTP/TCP 包装和早期示例，部分接口早于当前分布式实现。
- `client/.../client_lib/`：C++ 客户端、通道和参数容器；部分通道操作没有实现，协议与最新 Ruby 脚本不能视为已经兼容。
- `fsnet/Makefile`、`fsnet.bundle`：开发者 macOS/RVM 生成产物，硬编码本机目录；不是 Linux 构建入口。
- `libs/`：Ruby 2.1.2、libevent alpha 源码归档。缺少依赖锁定、自动化测试、数据库、迁移、发布与故障恢复流程。

## 按风险排序的问题

| 等级 | 位置 | 触发与影响 | 新路径的处理 |
| --- | --- | --- | --- |
| P0 | `scripts/dist_server/server/base_server.rb:25`、`remote_server.rb:69` | 网络方法名只经过 `respond_to?`，随后 `send`；包括 `instance_eval` 等继承方法，存在远程代码执行路径 | 仅执行显式 `bind` / `rpc` 注册的路由；JSON 不承载对象类型 |
| P0 | `scripts/dist_server/server/serial_object.rb:13`、`util/array.rb:45` | 不可信网络内容进入 `Marshal.load`，存在对象注入及反序列化执行风险 | 新协议不使用 Marshal，旧协议不自动接入新节点 |
| P1 | `FSNet/rb_define.c:862` | 连接失败后 `fs_free(node)`，下一行将已经释放的 C 指针当 Ruby 对象执行 `RDATA(node)->data = NULL` | 新连接由 RAII 管理，失败回调清理所有挂起调用 |
| P1 | `FSNet/rb_define.c:190`、`:215` | 输入流长度使用收到的数据总长而非分配的 payload 长度；备用 mb parser 未验证声明长度已有完整数据就 memcpy | 新协议先验证版本、固定头、路由长度和最大帧长，再分配/解析 |
| P1 | `FSNet/rb_define.c:441`、`:78`、`:828` | Fiber 保存栈上参数地址，队列中的 Ruby VALUE 没有 GC 根/mark；跨原生线程创建 Ruby 值，生命周期与 GVL 不受统一约束 | 新核心不依赖 Ruby 内部布局；Ruby/Fiddle 回调、Fiber 显式保根，回调仅在 Ruby 的 poll 线程执行 |
| P1 | `FSNet/fs_node.c:169`、`fs_server.c:400` | 关闭/停止直接销毁 mutex、event_base 等，缺少线程 join 与状态所有权；可能与回调并发使用 | 新连接状态在所属事件循环串行修改，关闭幂等，取消后的回调检查 closed |
| P1 | `FSNet/fs_stream.c:188`、客户端 `fs_stream.c:197` | `memcpy` 压缩相互重叠的缓冲区属于未定义行为；弱引用缓冲还可能被 realloc | 新实现使用拥有明确生命周期的 string/队列，不复用这套流实现 |
| P1 | `FSNet/fs_server.c:780`、`rb_define.c:848` | 固定 64 字节数组上的 `strcpy` 无长度检查 | 新核心使用有上限的字符串与校验 |
| P1 | `FSNet/fs_node.c:225`、`rb_main.c:70` | 网络缓存无限扩张；全局回调队列满后阻塞 I/O，无按客户端配额 | 帧长、连接数、发送队列、挂起调用、入站请求、空闲时间均有上限 |
| P1 | `scripts/dist_server/logic_server/logic_server.rb:36` | `1 / 1000` 为整数 0，生成零间隔心跳；关闭 GC 的诊断不能解决问题且放大内存增长 | 新健康探测使用 steady_clock 和非零毫秒间隔；不禁用 GC |
| P1 | `scripts/dist_server/server/remote_server.rb:88` | Fiber 等待 RPC 没有 deadline；失联后仅 resume，不能区分超时、执行失败或部分完成 | 明确 status、deadline、断线清理，写请求默认不重放 |
| P2 | `scripts/dist_server/util/array.rb:69` | `Bignum` 分支在 `Integer` 之后，旧 Ruby 的大整数也会先走 int32；新版 Ruby 又移除了常量 | JSON 逻辑接口独立于原来的类型编码；整数范围由业务 schema 校验 |
| P2 | `client/.../FSClientBase.cpp:16`、`:90`、`:124` | hostname 固定 memcpy16 不保证 NUL，close 不重置 fd；connect/select 不检查 SO_ERROR，可能把失败当成功；析构缺资源释放 | 新 C++ Node 可用作客户端，连接异步并检查 error_code；旧 SDK 尚未迁移 |
| P2 | `scripts/demo_servers/simple_http/src/hello_server.rb:57` | 直接拼接网络 URI 为文件路径，缺少目录边界校验 | 旧 HTTP 示例仅作为历史参考，不是新框架的公网入口 |

## 运行证据

实际构建过仓库随附 Ruby 2.1.2 和扩展。HTTP 首页返回 200，缺失文件返回 404。分布式示例在 Ruby 3.3 上发生 Bignum NameError；在 Ruby 2.1.2 下仍出现活跃 C 节点对应 Ruby 对象被释放的告警。关闭 GC 仅用于定位，虽然观察到节点注册，但运行随后达到容器内存上限；该模式未写入启动步骤。

这些结果支持重建运行时边界，而不是继续把原始 C 指针、Ruby VALUE 和网络线程互相传递。不能据此断言旧框架所有问题均已找到；上表包含静态确认的缺陷，未为每一个旧缺陷构造攻击请求。

## 设计判断

gate/logic 分工、显式 service 和 Fiber 顺序式 RPC 值得保留。单中心、随机实体路由、没有持久化语义的内存状态和任意方法派发不适合高可用。

新的默认构建与旧实现隔离。先交付可验证的网络/存储/逻辑/P2P 基础，再按 `ARCHITECTURE.md` 的验收清单实现状态接管、加密和部署。旧 Ruby 应用、C++ SDK、客户端协议仍需显式迁移；它们没有因新目录出现而自动升级。
