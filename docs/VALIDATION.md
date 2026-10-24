# 当前实例验证记录

验证日期：2026-10-23。执行环境：Debian 13、GCC 14.2、CMake 4.4.4。构建目录位于仓库外 `/workspace/fsnet-env`。

## 共享实体阶段验证配置（历史）

依赖均使用 CMake 固定来源，构建输出在仓库之外。本轮结果：

| 配置/检查 | 结果 |
| --- | --- |
| Debug，Lua 与可选 PostgreSQL | CTest 19/19 通过 |
| ASan/UBSan，Lua 与可选 PostgreSQL | CTest 13/13 通过 |
| 纯核心，Lua 和 PostgreSQL 均关闭 | CTest 9/9 通过 |
| 清理阶段重跑 tools/cloud-install.sh | 安装、配置、构建及 CTest 17/17 通过 |
| 当前源码、文档和文件名清理检查 | 通过，无废弃适配器文件或引用 |
| Shell/Python 语法、本地 Markdown 链接、git diff --check | 通过 |

C++ 核心检查：

- Memory/SQLite 同一存储合同、二进制值、并发 CAS、事务失败无部分写入、删除重建版本不复用、SQLite 重开恢复。
- 两后端上的实体接管与 fencing：旧 owner 不能覆盖接管后的状态。
- TCP 二进制请求/回复、显式路由、应用异常、超时、挂起请求额度、错误 token、断线清理、超大帧声明导致关闭。
- C ABI 回调中请求销毁节点时延迟释放，防止 poll 访问已释放状态。
- 实体稳定路由、服务租约过期、节点停止后幂等调用转向另一个健康节点。
- ICE 直连传输；本机认证 TURN，限制交换 relay candidates，并从选中 TURN 路径的一侧传输游戏数据报。验证选中路径，不把单纯分配中继端口当成传输通过。

Python 进程检查：启动 C++ echo 进程，C ABI 发送含 NUL/非 UTF-8 的数据，SIGKILL 服务端，验证回调返回断线，再同端口重启并通过新连接调用成功。测试进程均在 finally 中清理。

## 性能样本

Release `fsnet-benchmark 10000`，两个 Node 共用一个调用线程，真实 TCP loopback，载荷 128 字节、并发 128：

- 10,000 请求约 0.0627 秒，约 159,510 请求/秒。
- p50 约 0.817 ms，p99 约 1.236 ms。

这是单次本机样本，不包含脚本逻辑、持久化、公网、加密、战斗逻辑或真实玩家负载；不据此推算在线人数，也没有生产延迟 SLO 结论。

## 未验证/未完成

CI 工作流已添加；本记录仅报告本地结果，不包含 GitHub Actions 的运行结论。未执行跨运营商 NAT、公网 TURN、UDP 被禁时的 TCP/TLS 后备、真实网络分区、跨机数据库 HA、自动实体接管或长期容量压测。TLS、RPC 身份/权限和可选 PostgreSQL 已有单独测试；不能据此推断公网 P2P 或跨机数据库 HA 已验收。

安装与启动配置已保存为环境草稿；没有发布环境，也未验证新任务从发布快照恢复。当前实例的通过结果不能代替这些步骤。

## 阶段 1：脚本宿主

Lua 5.4.7 由固定源码 SHA256 构建，普通与 sanitizer 配置分别验证脚本宿主。Lua 单元/集成检查包含二进制数据、立即/异步完成、重复/晚到回调、加载失败无半注册、OOM 后恢复、指令预算（含 pcall 循环）、队列数量/字节上限、悬挂任务总时限、停止时取消和回调内销毁引擎。

宿主合同启动 Lua 独立进程，通过原始字节客户端验证路由、NUL/非 UTF-8、后端请求、异常与超时恢复。模块资源加载和热更新尚未完成，详见 SCRIPTING.md。

## 旧文件清理验收

原始版本保存在 Git 历史 `69a4b62e52ff1874d25f7e64cdb176829f2024be`。移除 119 个旧文件后，曾从新目录 `/workspace/fsnet-env/clean-build` 重新下载/配置依赖、编译并通过当时的 CTest。活动源码、测试、构建和安装脚本中没有对已删除旧目录的引用。旧审查报告的路径明确指向归档版本。

Lua 实体 mailbox 测试覆盖跨方法 FIFO、不同实体推进、await 期间互斥、异常释放、超时释放、忽略迟到回包、非法封包拒绝、Lua/原生编码一致和停止清理。此验证不包含跨节点独占所有权。

实体持久化验证包括：原生宿主的租约竞争、超期接管、未到期行政转移后的 fencing 拒写、句柄与显式交接、tick 背压；Lua VM 替换后的状态恢复；`fsnet-entity-process` 启动三个独立 Lua 服务进程，共享同一 SQLite 文件，验证双进程 owner 排他、SIGKILL 后租约接管、已确认状态恢复和第二次重启。没有把该本机共享文件测试描述为跨机数据库验证。

`fsnet-routing` 使用真实 TCP 节点测试目录路由、节点退出后的租约接管、保留窗口内去重、响应丢失后的安全重试和停止取消。Lua 集成另验证 gateway 协程通过 entity_call 找到 owner 并恢复响应。去重单元验证 ID 冲突、容量淘汰和新 owner 恢复。

TLS 测试临时生成 CA/服务/客户端与不可信证书，验证 TLS 1.3 双向认证、证书名称拒绝、未知 CA 客户端拒绝、按指纹路由授权、无共享 token 的证书认证与非 loopback 明文拒绝。ICE 的候选队列与结束标志改为原子批次获取，核心测试重复 20 次验证原先的结束信令竞态。

存储合同调整后，实体宿主测试使用不实现 transact 的后端，确认所有权、快照、接管、去重与卸载仅依赖单记录 CAS。文件后端验证二进制 key/value、多实例竞争、tombstone 防 ABA、低层 EntityStore fencing、重开持久化和损坏拒绝。独立进程测试覆盖多个文件 writer、SIGKILL 后已确认值保留与锁释放，以及完整 Lua 实体在文件后端的 owner 接管/恢复。该证据不等同于断电、远端文件系统或跨机存储一致性验证。

可选 PostgreSQL 验证使用固定源码编译的 libpq/服务端和临时 TLS 证书：共同 CAS 合同、二进制 key/value、4 个独立连接竞争、无事务的 EntityStore fencing、不可信证书拒绝、Lua 三进程接管、数据库 immediate stop/WAL 恢复以及存活客户端安全读取重连。已检查 libfsnet.so 动态依赖仅为系统 C/C++ 运行库，不依赖机器上的 libpq/OpenSSL 私有库。

共享实体阶段：fsnet-shared 覆盖本地 userdata、执行域、只读投影、客户端提案、定时等待、权限撤销和释放；fsnet-shared-process 启动两个真实程序，加载同一魔法阵类，验证服务端 tick 自动同步到 Lua 客户端。

## 客户端预测阶段：本次检出环境

2026-10-23，新环境重新配置固定来源依赖，使用 GCC 14.2、CMake 4.4.4。
仓库外构建目录为 `/workspace/fsnet-env/build` 与 `/workspace/fsnet-env/asan`。
本次开启 Lua、关闭可选 PostgreSQL；未重复旧环境的 PostgreSQL 或纯核心专用配置，
上文 19/19 与 13/13 的 PostgreSQL 配置结果属于共享实体阶段历史。

| 本次配置/检查 | 结果 |
| --- | --- |
| Debug，Lua ON、PostgreSQL OFF | CTest 20/20 通过 |
| ASan/UBSan，Lua ON、PostgreSQL OFF | CTest 13/13 通过；开启 leak 检测和遇错终止 |
| 修改文档的本地链接、Python 测试夹具语法、git diff --check | 通过 |

新增 `fsnet-prediction` 验证独立预测/确认状态、连续序号与历史限额、权威校正后
重放未确认输入、重复确认、无效确认、脚本失败和分配失败不发布部分状态、吞掉
联网/等待错误仍拒绝预测、跨实体写入限制、私有属性与 driver 权限、指令死循环
终止后继续正常调用、非零初始累计确认、权限撤销以及停止后的内存回收。

`fsnet-prediction-process` 启动两个独立交付程序，加载同一共享类：客户端预测两次
位移（5+5），服务端分别限幅为 4，首个确认后重放余下输入得到 9，最后收敛到 8，
效果只交付 2 次。它也覆盖未指定 peer 的实体引用经客户端配置连接直达 owner。
测试为本机回环，不代表公网、Gateway、重连重发或完整回滚世界验收。

## 自动实体存在性与客户端表现阶段

2026-10-23，沿用本次检出环境的 GCC 14.2 / CMake 4.4.4，Lua ON、PostgreSQL OFF。

| 配置/检查 | 结果 |
| --- | --- |
| Debug | CTest 22/22 通过 |
| ASan/UBSan，detect_leaks / halt_on_error 开启 | CTest 14/14 通过 |
| 本地 Markdown 链接、Python 夹具语法、git diff --check | 通过 |

`fsnet-presence` 覆盖两个不同认证账号的登录创建、已有初值不覆盖（含驻留 transient）、
自动 LocalPlayer/RemotePlayer 工厂、属性增量与私有字段过滤、服务器 owner/控制标志、
一个连接的控制切换与幂等、hide/show 不删除服务器实体、未创建实体拒绝、每连接限额、
隐藏后禁止手动观察、禁止控制他人、来自非服务器连接的伪造通知隔离、身份变化撤销、
普通工厂异常后的镜头/表现回收、内部 spawn 路由不公开，以及停止自动订阅后仍可保留
手动 watch、退订晚到自动创建。

`fsnet-presence-process` 使用两个独立交付程序，验证登录后不调用 watch 即创建 LocalPlayer，
初始属性先于 on_spawn，绑定镜头目标示意、预测输入与确认，注销时按顺序解除控制和移除。
图形模型/镜头/输入调用由引擎适配，本测试只检查无图形依赖的生命周期与目标字段。
本次没有运行 PostgreSQL、真实公网、自动空间 AOI、Gateway 迁移或完整服务端生命周期验收。

## 类型化结构、固定数组与嵌套自动赋值阶段

2026-10-23，GCC 14.2 / CMake 4.4.4；Lua ON、PostgreSQL OFF。

| 配置/检查 | 结果 |
| --- | --- |
| Debug | CTest 24/24 通过 |
| ASan/UBSan，detect_leaks / halt_on_error 开启 | CTest 15/15 通过 |
| 修改文档本地链接、Python 夹具语法、git diff --check | 通过 |

`fsnet-structured` 验证原生具名结构与固定数组的类型、深复制、FSA1 编解码/截断/重复字段
拒绝、嵌套深度与展开限额、非法候选不部分发布及属性恢复；Lua 验证 vec3、mat4x4、
自定义多层结构、整值/行/字段赋值、pairs/ipairs/长度、普通表不别名、受管路径别名、
rawset 拒绝、字段与下标/整数范围限制、关闭/跨请求作用域失效。

真实服务端和两个客户端验证服务端字段修改自动广播、客户端整值与嵌套赋值自动提交、
observer 收取值但不能写、私有属性隔离、validator 拒绝后确认值不变、显式 write 兼容、
脚本失败无广播、预测重放与禁止跨对象写，以及 1 MiB VM 预算下连续 5000 次字段读取
回收临时视图、停止后 VM 内存归零。

`fsnet-structured-process` 用两个独立交付程序加载共享结构，验证 position 整值/字段、
矩阵行/元素、自定义 pose 字段及服务器拒绝的完整链路。

结构/数组扩展 FSA1 新 tag，旧标量记录可读取；没有验证旧二进制读取新结构（旧实现会
拒绝新 tag），也未执行 PostgreSQL 或结构 schema 在线迁移。客户端写提案仍是服务器
验证后提交，不代表信任客户端位置或提供自动冲突合并。


## 属性同步性能优化

2026-10-23，Lua 开启、PostgreSQL 关闭：Debug CTest 24/24，ASan/UBSan（含泄漏检测）15/15 通过。
增量合并后的字段数、字节数、节点数超限均验证无部分更新；既有权限、撤销、预测、生命周期和跨进程检查通过。
Release 与 `475b6f8` 基线交替测量 7 对进程，详见 [性能报告与原始数据](ATTRIBUTE_PERFORMANCE.md)。
本轮没有公网、磁盘数据库、多实体并发或生产容量验收结论。

## 变长容器属性

2026-10-23，Lua 开启、PostgreSQL 关闭：Debug CTest 26/26，ASan/UBSan（含泄漏检测）16/16 通过。
新增 list/map/set 原生编码与持久化恢复、容量/类型/唯一性/总节点预算失败不发布、嵌套修改与深复制、
两个客户端收取服务端增删、owner 自动提案与观察者拒写、脚本回滚、预测重放与内存释放检查。
独立进程测试运行交付的背包/任务示例，验证客户端识别新增、修改、删除。
容器当前同步整个变化属性，没有逐元素操作日志或增量压缩；合同见 [容器属性](CONTAINER_ATTRIBUTES.md)。


## 属性合批与容器补丁

2026-10-23，Lua 开启、PostgreSQL 关闭：Debug CTest 27/27，ASan/UBSan（含泄漏检测）17/17 通过。
新增 FSA2 编解码、混合替换/补丁、截断和边界、非法补丁无部分更新、600 轮固定随机重放；
客户端 batch 覆盖单次提交、取消、禁止副作用、版本/validator/观察者拒绝、无变化不写、草稿回收。
独立交付程序验证多个结构/数组字段一次提交。Release 顺序/合批交替测量 7 对进程，
写请求与存储提交数均核对为 16:1，结果见 [容器合批性能](CONTAINER_BATCHING_PERFORMANCE.md)。
FSA2 用于下行，FSA1 用于快照、存储及上行最终值提案；旧读端需同步升级。


## 递归字段路径增量

2026-10-24，Lua 开启、PostgreSQL 关闭：Debug CTest 27/27，ASan/UBSan（含泄漏检测）17/17 通过。
新增 FSA3 字段/字符串键/数字下标路径编解码、1 KB 结构只发送变化叶字段、密集回退、深度 8 边界、
非法/缺失路径、截断与多个编辑中途失败无部分更新；既有 FSA1/FSA2 和随机容器重放继续通过。
两个客户端、预测与独立结构属性进程测试继续覆盖更新后的广播路径。
Release 编码大小检查：1024 字节未变 payload 的结构只改 position.x，整属性 1111 字节，字段更新 54 字节；
vec3 单 x 更新 67→48 字节，128 个 Item 中改单个 count 为 4146→52 字节。
这里是编码字节对比，不是端到端速度或容量结论；详见 [字段路径增量与原始样本](FIELD_PATH_DELTAS.md)。

## 上下行共用差量生成

2026-10-24，Lua 开启、PostgreSQL 关闭：Debug CTest 27/27，ASan/UBSan（含泄漏检测）17/17 通过。
默认客户端 write/字段赋值/batch 与广播共用 attribute_delta，服务端原生入口和 Lua validator/apply 共用提案 staging。
新增相同前后状态的双向补丁一致性、候选不提前发布、跨 owner/只读/类型/路径/版本拒绝、未变化只读属性仍拒写；
既有固定 ID 重试、真实 TCP 两客户端和独立交付示例通过。固定 ID SDK 写入保留 FSA1 表示，保持原有去重语义。
Release 字节验证：1 KB 结构单叶更新，上下行均为 54 字节；vec3 单 x 为 48 字节，Item count 为 52 字节。
Release 链路 smoke 核对 10 轮：逐条 160 次写/提交、batch 10 次写/提交，没有确认应用再次写入。
这是字节与功能验证，未把并行构建期间的 smoke 计时用于速度结论。见 [共用差量与原始样本](SHARED_ATTRIBUTE_DELTAS.md)。


## 架构与 API 文档同步

2026-10-24，文档整理，运行时代码未变。新增统一导航与端到端 API 指南，同步公共引擎边界、
提交/合批时机、动态容器、FSA1/2/3、默认双向差量、固定 ID 回退、预测及尚未实现范围。
从 API_GUIDE.md 提取共享声明、服务端注册/登录和客户端表现代码，使用实际 fsnet-script/fsnet-client
独立进程完成一次 smoke：登录创建、自动表现、隐式字段提案、batch、服务端奖励 RPC、背包任务读取、
重复领取不重发奖励和清理通过。此检查不新增 CTest 项，也未重跑普通/sanitizer 全套；最近运行时结果仍为 27/27、17/17。
本次另检查更新文档的本地链接/章节锚点、代码围栏和 git diff --check。


## 服务端生命周期阶段

2026-10-24，Lua ON / PostgreSQL OFF：普通 CTest 28/28、ASan/UBSan（含泄漏检查）18/18 通过。
新增事务化 create/activate/deactivate/destroy、创建未完成 FER2 标记跨宿主重试、业务失败不重复激活、
卸载失败无属性发布、idle 卸载、删除重建/旧句柄拒绝及 drain 检查；FER1 已初始化记录保持兼容。
旧二进制不认识 FER2，升级需排空旧 owner，且 hook 外部副作用仍需幂等。详见 ENTITY_LIFECYCLE.md。

## 2026-10-24：生命周期、跨节点客户端与交付验收

当前源码配置：Lua ON、PostgreSQL OFF。Debug 普通 CTest **30/30** 通过，
ASan/UBSan（detect_leaks=1、halt_on_error=1）原生 CTest **19/19** 通过。
本轮未重跑可选 PostgreSQL 服务端及跨机环境，不将历史 PG 测试当作本轮执行结果。

- `fsnet-lifecycle`：创建/激活顺序与提交、失败初始化重试、恢复、显式/空闲卸载、删除、drain 和权限/句柄边界。
- `fsnet-gateway`：两 owner、Alice/Bob，认证账号委托及角色隔离、私有属性过滤、写权限/校验、
  订阅 ID 重映射、跨 owner 单控制目标、自动与手动订阅迁移、删除不复活、身份变更撤销、readiness 与排空。
- `fsnet-delivery`：真实独立进程和临时 mTLS PKI，拒绝不安全启动配置、不可信 CA、未映射证书；
  SIGKILL owner 后保留确认值与唯一前端视图、持久回执跨 owner 去重、共享字段合批、重启节点、
  SIGTERM 优雅交接、资源回收与最终 ready=false。普通测试使用 100 轮串行合批。

Release 构建后执行 cmake install 和 CPack TGZ，解压到不同目录，使用包内启动器、客户端和
验收脚本通过 **1000 轮**同一故障验收。`ldd` 确认从包内相对路径加载 libfsnet.so，其他依赖
仅为系统 C/C++ 运行库；未依赖构建目录的私有 OpenSSL/libpq。原始采样和二进制哈希见
[交付验收报告](validation/delivery-release-2026-10-24.json)。本机 2 秒测试租约的恢复耗时约
2 秒；该数据是一次局部样本，不构成默认 60 秒租约、公网、并发玩家规模或生产 SLO 承诺。
报告资源值为轮询采样峰值，不是严格的最大分配统计。

Python 语法、文档本地链接和 git diff --check 通过。GitHub Actions 增加 Release 打包、
可重定位验收及产物上传；这里只报告本机执行，未声称远端工作流已通过。
部署、指标、停止时限、存储/协议升级限制和目标环境的剩余验收见 [运行手册](PRODUCTION.md)。
