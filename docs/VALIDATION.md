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

## 2026-10-24：Copy 优化后检查

最终源码 Debug 普通 CTest **30/30**、ASan/UBSan **19/19** 通过；Lua ON、PostgreSQL OFF。
本轮没有重跑可选 PostgreSQL 服务端或跨机部署。新增/扩充检查包括：Node借用输入在返回后
立即覆盖/销毁仍正确发送；借用/拥有封包的二进制内容和边界；FSA1/FSA2/FSA3转发只修改
订阅ID而保留属性字节；截断、尾随、非法撤销拒绝；确认版本在复制、移动、更新失败及reset后
保持旧版本不变。原有预测、权限、生命周期、持久回执、自动/手动迁移和独立进程检查继续通过。

性能数据、编译兼容边界与尚未消除的复制见 [Copy 优化记录](COPY_OPTIMIZATION.md)。
优化后的可重定位Release包另通过1000轮mTLS+文件持久化交付验收，包含崩溃接管、回执重放、
优雅交接和资源清理；[报告](validation/copy-optimized-delivery-2026-10-24.json)。

## 2026-10-24：开发环境迁入仓库

固定工具清单、激活/bootstrap脚本和五个CMake presets已进入源码管理。工具副本及本机性能
基线复制到被忽略的 `.dev/`；编译产物和第三方源码不入Git。
在 `.dev/build/debug` 重新下载/校验固定依赖，从头使用仓库内 CMake 4.4.4/Ninja 1.13.2
配置、编译，`ctest --preset debug` **30/30**通过。CMakeCache不包含旧fsnet-env路径。
另验证从仓库外运行bootstrap、重复准备跳过工具下载、全部preset列表、脚本语法、非法preset
拒绝和忽略规则。当前轮次没有新建ASan/Release/PG完整构建，不挪用历史结果充当本轮验证。

当前环境入口见 [开发环境](DEV_ENVIRONMENT.md)；历史记录中的旧绝对路径保持原样，仅代表
当时测量位置。旧目录未删除，后续开发可使用仓库内配置重建而不依赖它。

## 客户端SDK阶段

增加fsnet::Client、连接generation句柄、原生实体/控制事件、共用确认版本与脚本宿主接入。
当前Debug CTest **32/32**通过：新增无Lua依赖的SDK检查（握手、权限、二进制RPC、旧句柄、
断线清理、回调重连/销毁/异常、握手超时）及SDK安装后重定位的独立CMake消费者检查。
导出fsnet::client与可选fsnet::lua，验证静态Lua依赖随安装包完整交付。ASan和示例重写的
最终集成结果在后续阶段另记，本段不代替尚未执行的检查。

## SDK接入与示例重写最终验证

源码内Debug构建 **35/35**、ASan/UBSan **20/20**、Lua OFF核心构建 **12/12**通过；
核心检查后恢复Debug默认Lua ON配置。ASan使用仓库内独立preset重新配置构建，覆盖回调重连、
销毁、异常和额外连接/旧宿主路由清理；额外脚本连接不能冒充配置的服务器回调身份。

重写的scene/Lua、scene/Native、attributes三例均有独立进程检查。SDK安装测试将安装目录改名，
由独立CMake消费者链接fsnet::client及可选fsnet::lua，再直接运行安装包内三例。
原来的Lua单项脚本移至tests/fixtures，进程测试继续通过新SDK客户端宿主执行，回归覆盖保留。
性能夹具移至benchmarks，命令名不变；未把历史性能数据当成本轮SDK性能承诺。
Python语法、当前Markdown本地链接、git diff --check通过。未重跑可选PostgreSQL服务端、
真实图形平台、跨机HA或公网NAT；C#/Unity/浏览器绑定仍不在本轮范围。

## 指定目标节点交接阶段（2026-10-24）

沿用仓库 `.dev` 工具链和固定依赖，Lua ON、PostgreSQL OFF：

| 配置/检查 | 结果 |
| --- | --- |
| Debug | CTest 36/36 通过 |
| ASan/UBSan，leak 检测、遇错终止 | CTest 21/21 通过 |
| Lua OFF 核心（复用 Debug 依赖，随后恢复 Lua ON） | CTest 12/12 通过 |
| Python 交付夹具语法、本地 Markdown 链接、git diff --check | 通过 |

新增 `fsnet-transfer` 使用三个真实 TCP Node 和共享 CAS 存储，验证 Lua `Class:transfer`、
源 mailbox 空闲检查与自身等待拒绝、目标名称预检、源卸载失败回滚、目标激活失败后恢复、
交接确认超时与重试、同实体并发额度、租约到期后的目标约束、旧消息隔离、删除重建后的
交接编号不复用、停止取消。交接编号采用源实体记录的存储版本，不复用可从零重建的租约 fence。

`fsnet-gateway` 扩展为指定目标往返及当前控制实体迁移：保留前端订阅 ID，不发送离开，
恢复状态和控制权，其他控制目标不受无关实体交接影响。迁移缓存竞态修复后，交接/Gateway
专项曾各连续运行 20 次通过；最终全量回归也通过。

`fsnet-delivery` 保留原 SIGKILL 接管、回执重放、100 轮属性合批与优雅排空检查，另外启动
启用交接的独立 owner 进程，通过独立 owner/Gateway 证书白名单、mTLS 和共享 POSIX 文件
存储验证指定目标往返。客户端确认同一 Lua 实例、单次 spawn、已提交状态和后续移动可用。
安装重定位/SDK 独立消费者随 Debug 全量检查通过。

验证范围为本机进程。未验收跨机 HA、真实网络分区、长稳容量或迁移输入自动补发；
也不表示脚本模块/在线 schema 迁移已实现。升级与失败恢复合同见 [ENTITY_TRANSFER.md](ENTITY_TRANSFER.md)。


## 持久化实体命令收尾（2026-10-24）

`811b03a` 已提交 outbox/inbox 初版；本阶段完成生命周期、内存预算和交付检查。
使用仓库 `.dev` 固定工具链、Lua ON、PostgreSQL OFF：

| 配置/检查 | 结果 |
| --- | --- |
| Debug | CTest 39/39 通过 |
| ASan/UBSan，detect_leaks/halt_on_error | CTest 23/23 通过 |
| Lua OFF 核心（复用 Debug 依赖，随后恢复 Lua ON） | CTest 13/13 通过 |
| Python 夹具语法、本地 Markdown 链接、git diff --check | 通过 |

`fsnet-delivery-codec` 原生检查 FSP1/FSP2 兼容、二进制消息/确认、全部截断位置、超大载荷、
重复 ticket 和索引拒绝，不依赖 Lua。`fsnet-durable` 通过共享文件 CAS 与三个真实 Node 验证：

- 源状态/outbox 同提交、失败回滚、源与目标最终 CAS 故障后不发布半份结果。
- 容量耗尽明确拒绝、pending 不允许清理、成功及明确失败结果查询和显式清理。
- 超过普通 64 条回执窗口后的重放隔离、内容篡改及伪造下一序号拒绝。
- 业务 reject 时回滚其暂存的后续命令，保留失败确认并推进水位。
- 接收方迁移、发送方携带 outbox 迁移、持久索引重新发现、删除重建 epoch 隔离。
- 非 server 及不可信 server 拒绝；无 pending 时销毁 hook 新建命令也不得随销毁被抹掉。
- 拒绝删除 pending 源实体不破坏激活标记；后台确认触发续租时保留 persist=false 属性与激活次数。

`fsnet-durable-process` 在独立发送方/接收方进程上验证两种边界：源返回已提交 ticket 后
SIGKILL，重启后自动补投；接收方奖励和 inbox 提交后故意丢弃确认并 SIGKILL，重启重放
原确认且奖励不重复。临时共享 POSIX 存储和进程均在 finally 中清理。

`fsnet-delivery` 保留原 mTLS/Gateway、故障接管、属性合批和指定目标往返检查，并新增
服务端源实体创建标志与 outbox 同提交，经真实 owner 证书白名单投递到目标实体，客户端
确认同步属性；重复发布不产生第二笔奖励。SDK 安装重定位随 Debug 全量测试通过。

修正 ACK/新 inbox 生成后的驻留预算统计，并提前检查 Lua 参数和确认帧大小。
上述结果不代表跨机 HA、数据库副本丢失恢复、无限消息容量或外部副作用 exactly-once。
协议/API/升级约束见 [ENTITY_DELIVERY.md](ENTITY_DELIVERY.md)。

## 逻辑连接授权与持久计时器（2026-10-25）

`refactor/global-entities` 上的 `4a94d55`、`14be2e3`、`d9c958c` 接入逻辑连接身份、
全局实体 owner 换绑，以及持久实体计时器。使用仓库 `.dev` 固定工具链，PostgreSQL OFF。

| 配置/检查 | 结果 |
| --- | --- |
| Debug，Lua ON | CTest 48/48 通过 |
| Release，Lua ON | CTest 48/48 通过 |
| ASan/UBSan，detect_leaks/halt_on_error | CTest 31/31 通过 |
| Debug，Lua OFF 核心 | CTest 17/17 通过 |
| 持久计时器专项 `fsnet-timers` | 连续 10 次通过 |
| SIGKILL/重启专项 `fsnet-timer-process` | 连续 3 次通过 |

连接检查包括同账号不同连接隔离、可信 Gateway 保留原始连接、断线后旧上下文失效、
跨节点换绑与旧投影撤销、远端创建后向原连接 show/possess。
计时器检查包括三种周期推进、有限次数、取消/替换、失败回滚与重试、容量拒绝、卸载唤醒、
目标节点迁移后触发，以及索引/实体两个提交边界的 ACK 丢失。
真实进程测试在注册任务后 SIGKILL，过期后重启继续执行；第二次重启验证已完成任务不再推进状态。

ASan 配置不运行独立进程夹具；这些由普通构建覆盖。以上为本机共享存储和故障注入检查，
不表示已完成跨机分区、时钟异常、生产容量或外部副作用 exactly-once 验收。
API 与恢复合同见 [CONNECTION_AUTHORITY_AND_TIMERS.md](CONNECTION_AUTHORITY_AND_TIMERS.md)
和 [ENTITY_TIMERS.md](ENTITY_TIMERS.md)。

复现时先 `. tools/dev/activate.sh`，分别对 `debug`、`release`、`asan`、`core` 执行
`cmake --preset <配置>`、`cmake --build --preset <配置>`、`ctest --preset <配置>`。
重复专项使用 `ctest --preset debug -R '^fsnet-timers$' --repeat until-fail:10` 和
`ctest --preset debug -R '^fsnet-timer-process$' --repeat until-fail:3`。

补充计时器应用示例后，`ctest --preset debug -R '^fsnet-example-'` 的四个示例全部通过。
`python3 examples/run.py timers` 完成真实 SIGKILL/文件存储恢复、稳定 ID、延迟/指定时间、
合并/补跑周期、取消和完成后不再触发检查。安装到独立 prefix 后，从
`share/fsnet/examples/run.py timers` 使用默认安装路径同样通过，无需源码目录或手工拼接脚本。
示例代码与预期输出见 [examples/timers](../examples/timers/README.md)。

## Runtime 审查修复（2026-10-24）

针对 [代码审查报告](CODE_REVIEW_2026-10-24.md) 的七项缺陷补齐修复：Node 回调生命周期与接收预算、实体运行时权限更新与重放激活、不确定提交后的订阅失效恢复、Lua 模式匹配工作预算与输入容量释放。同时优化持久投递扫描、目录解码，并修复 Gateway 慢轮询时的候选重试和恢复后的旧缓存。

最终验证：Debug 39/39，ASAN/UBSAN 23/23，Lua OFF 13/13；Gateway 与 mTLS 投递恢复各连续三次通过。正式回归覆盖权限撤销后的排队/挂起/持久化调用、commit-then-throw、订阅重建、回调重入销毁、接收额度回收、Lua pcall/模式匹配超限，以及租约到期前禁止接管和慢轮询后的恢复。原诊断工具保留观测输出，输入容量释放通过 Debug RSS 探针复核；构建输出均留在忽略目录 `.dev/review/`。

复现命令与架构限制见审查报告的“修复后验证”。这些结果不等于历史去重索引自动回收、可抢占的敌对脚本沙箱或公网生产验收。

## Actions 迁移恢复与进程清理修复（2026-10-24）

Actions 运行 `40089723284` 的 Debug Lua 作业在 SIGKILL 恢复断言全部通过后失败：
TemporaryDirectory 先退出，仍运行的子进程继续写共享目录，触发 `Directory not empty`。
夹具现在先终止、回收所有子进程并关闭客户端，再清理目录，不忽略删除错误；专项连续五次通过。

运行 `40091262353` 的 Release 与 PostgreSQL 作业均失败在 `fsnet-gateway` 的受控 Pet 交接；
PostgreSQL 专项测试本身通过。本地 Release 也复现同一恢复超时。根因是 Gateway 将 show
产生的中间 enter 当成恢复完成；重试已可见的副本又不会产生新通知，控制状态和完成信号可能丢失。
恢复 RPC 现在返回最终快照及控制状态，重复调用也明确确认；Gateway 以确认结束迁移。
新增重复恢复无新通知的确定性回归，同时验证显式取消控制。owner 与 Gateway 须一起升级。

本地修复后 Debug 39/39、Release 39/39、ASAN/UBSAN 23/23 通过；受控迁移专项在 Release 连续十次通过。
Release 归档解包后，运行 `validate_delivery.py --rounds 1000` 通过 mTLS、故障接管与交接验收。
GitHub 矩阵关闭 fail-fast，失败也保留 CTest 日志，避免一个配置失败取消其他配置的检查。
