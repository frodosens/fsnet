# 全局实体方案与当前方案：差异、API 和复杂度

2026-10-25。运行时代码基线 `2c3cd01`；新方向见
[实体寻址规划](ENTITY_ADDRESSING_PLAN.md)，目前尚未实现。本文中的新 API 均为设计示意。

结论：新方案让业务统一按实体引用编排，框架承担全局身份、类型解析和可靠创建。
实现属于中高复杂度的核心寻址重构；现有 mailbox、属性差量、预测、租约/fencing 和交接算法大部分可复用。
如果还要兼容旧客户端、历史实体及未完成持久消息，工程复杂度会上升到高。

## 能力与结构差异

| 维度 | 当前方案 | 新方案 |
| --- | --- | --- |
| 实体身份 | `(type,id)`；Player/alice 和 Pet/alice 可以共存 | 一个逻辑实体一个全局 EntityId；不同实体不复用身份 |
| 业务 RPC 入口 | `runtime.rpc('login',...)` 与实体方法并存 | LoginHub、Player、Room 等业务 RPC 都以实体为目标 |
| 类型与分发 | 路由 `Player.input` 直接选择已注册类型/方法 | EntityId 解析类型元数据，再在方法表中分发 |
| 账号归属 | 支持 `owner='id'`，也已支持可信外部映射 | 规范为独立归属关系；Account 实体的 ID 可直接作为会话账号身份 |
| 创建 | `Class:spawn_or_restore(id,values)` 在当前宿主创建/恢复 | 引擎分配/登记 ID，可选择目标节点，创建操作有独立幂等合同 |
| 引用未知 ID | 某些类型化请求可按注册类的默认值初始化不存在的实体 | 仅凭未知 ID 不知道类型，应返回 not_found；创建必须显式给出类 |
| 路由 | 根据类型＋ID 查 owner，使用缓存与 placement | 根据 EntityId 查类型与 owner；允许同一权威记录提供两者 |
| 迁移 | 保持 type/id，切换 owner，客户端恢复原订阅 | 保持 EntityId，切换 owner；保留既有交接和恢复流程 |
| 客户端表现 | type/id 查找，已有 LocalPlayer/RemotePlayer 与预测 | EntityId 查找；初始元数据提供类型，确认/预测/表现仍共享逻辑 ID |
| 存储 | 记录键包含类型＋ID | 按 EntityId 定位，类型/归属/版本进入元数据 |
| 持久消息 | 引用包含类型、ID、epoch、ticket | 统一逻辑 EntityId；保留重放、序号与旧 owner 隔离语义 |
| 系统控制 | Node 原生握手、健康检查、运维路由 | 继续属于基础设施，不强制变成业务实体 |

旧方案已经具有多节点路由、实体交接和服务端实例隔离，多个 LoginHub 也可以用不同的复合键表示。
全局唯一 ID 提供统一身份规则；将 Player 创建请求投递到另一台机器，还需要跨节点创建协议。
仅替换 ID 并不会自动获得远端创建、自动分片或更高容量。

实体的“独立”指身份、状态和执行作用域，不要求每个实体独占线程、Node、进程或数据库连接。
它们继续共享宿主、调度器和存储执行器。

## 当前与建议 API

以下当前片段来自已交付接口；新片段中的 `fsnet.service/ref/transfer`、`spawn` 及选项名尚待定稿。
业务参数仍为 bytes；此重构不自动加入 JSON 或多参数序列化。
EntityId 的 Lua 表示及 RPC 返回值编解码仍需定稿，下列新片段省略该转换。

### 登录入口与实体调用

当前客户端/实体协程：

```lua
local player_id=runtime.server_request('login',login_bytes)
local player=Player:ref(player_id)
player:input(input_bytes,{request_id='input-1'})

-- 没有本地 Player 类时，也要显式给出类型：
local remote=fsnet.Entity:reference('Player',player_id)
remote:call('input',input_bytes,{request_id='input-1'})
```

新方案的调用模型：

```lua
-- 可信发现选择某个 LoginHub 实例，返回逻辑引用。
local hub=fsnet.service('login')
local player_id=hub:call('login',login_bytes)

-- 通用引用只需要全局 ID。
local player=fsnet.ref(player_id)
player:call('input',input_bytes,{request_id='input-1'})
```

有共享类定义的场景仍可保留 `Player:ref(player_id):input(...)` 这样的类型化便利接口，
检查解析出的实体类型，并复用同一公共 EntityRef 路径；不需要业务把所有已有方法调用改成字符串 call。
RPC 声明、`self.position.x`、容器 batch、生命周期和预测函数的业务写法原则上保持。
未知实体、类型不符、版本不符和未导出方法需要返回明确错误。

当前示例登录后由服务端 show/possess 自动同步客户端实体的流程继续保留；
上面返回 player_id 用于解释引用取得方式，不要求应用再手动创建客户端 Player。

### 创建、账号归属和目标节点

当前服务端：

```lua
local Player=fsnet.Entity:extend('Player',{owner='id'})
-- 声明属性和 RPC 后注册；账号 subject 由可信宿主提供。
Player:register()
local player=Player:spawn_or_restore(ctx.subject,{name='Alice'})
```

当前也可以采用 `owner='external'` 和宿主的 `ownership(resolve)`，使玩家 ID 与账号分开。
新方案应复用现有授权策略，改变身份解析与归属保存方式，而不是重新实现一套鉴权算法。

当前示例的 login 已经取得可信 `ctx.subject`，主要演示创建/显示玩家。
若 LoginHub 承担实际账号认证，还需定义未登录会话可调用范围、认证结果的可信传递与会话身份绑定，
以及身份变化后旧委托连接/订阅的处理；不能把客户端参数中的账号 ID 直接当成已认证身份。

新方案示意：

```lua
local player=Player:spawn({name='Alice'},{
    owner_account_id=account.id,
    target_node_id=selected_node,
    creation_key=stable_creation_key,
})
-- player.id 由全局身份机制确定；超时重试复用本次创建的身份。
```

Account、Player、Pet 各有全局 ID，Player/Pet 记录其归属的 Account ID。
账号实体 ID 本身就可作为会话认证主体，不额外要求第三套账号编号。
创建选项中的账号归属和目标节点必须来自可信服务端上下文与策略。

`creation_key` 的作用域、保存期限、参数一致性与查询合同必须明确；普通 RPC 的有界回执
不能直接当作无限期创建去重。跨机器创建丢失确认时，重试不得重新分配另一批 Player。
多个 LoginHub 同时登录同一账号时，Lua 可由 Account 实体协调复用关系；唯一 ID 本身不保证一账号只创建一名角色。

### 迁移、订阅与持久命令

```lua
-- 当前：类型由 Player 类提供。
local status=Player:transfer(player_id,'owner-b')
local ticket=Player:ref(player_id):send('grant_reward','100')

-- 新方案示意：通用操作以全局 ID 定位。
local status=fsnet.transfer(player_id,'owner-b')
local ticket=fsnet.ref(player_id):send('grant_reward','100')
```

类型化便利接口可以继续存在，只需在公共层使用 EntityId。
迁移调用仍遵守空闲 mailbox、自迁移等待拒绝、固定目标重试和权限检查；
`send` 仍表示可靠投递，普通 call 超时仍是结果未知，不因为身份统一而自动获得持久消息语义。

Native SDK 的对应变化：

```cpp
// 当前
client.call({"Player", "alice"}, "input", bytes, reply);
client.watch({"Player", "alice"}, ready);

// 新方案示意；EntityId 的编码和构造接口尚未确定
client.call(player_id, "input", bytes, reply);
client.watch(player_id, ready);
```

`ClientEntityHandle` 的会话代数/订阅 ID 继续承担本地有效性校验；
它与稳定 EntityId 分工不同，不能因省掉类型字符串就删掉这层保护。

## 迁移流程的复用和改动

已实现的核心流程继续使用：

```text
检查目标能力与容量
→ 源实体排空并执行 deactivate
→ 状态、回执、目标与交接号同记录 CAS 提交
→ 目标获取租约/fence、恢复并激活提交
→ 返回确认
→ Gateway 恢复原订阅、控制状态和属性流
```

需要调整的是地址与元数据：所有阶段传同一 EntityId，记录给出类型和版本，
接口按该身份读取 owner；订阅、出入站持久消息和回执引用均同步迁移。
新方案不会把 coroutine、外部连接、等待中的普通 RPC 或未持久属性自动搬到目标机器，
也不会新增零停顿保证。客户端确认状态、预测状态和展示对象仍沿用同一逻辑实体身份。

## 性能和协议成本

不能仅凭取消 `Player.` 就断定吞吐会提高。

- 当前实体 RPC 的类型通常只出现在 `Player.input` 路由里，FSE 只放实体 ID，并非每次发送两份类型。
- 新方案需知道 ID 对应的类型；可将类型与 owner 放同一记录并缓存，不必为每次 RPC 再访问一个中心服务。
- 类型化引用可以从共享 schema 得到方法名/编号；通用引用首次绑定可能需要元数据。稳态调用避免重复解析。
- 只改成固定路由 `system.entity.call` 再在 payload 加目标字段，未必省字节。压缩需要按完整 Node 帧核算。
- 数字方法 ID 会增加稳定编号、接口版本与映射校验工作。先保留方法名 `input`，也能完成全局实体模型。
- 同一 ID 的类型和方法表必须校验，避免客户端/服务端编号不一致时分发到另一个合法方法。

作为现有格式的尺度参考：无 request_id、空业务参数时，调用 Player/alice 的 move 为
18 B Node 固定头＋11 B 路由＋11 B FSE1，共 40 B。新方案的 EntityId 位宽和实际帧布局尚未固定，
因此目前不能给出真实节省比例或新旧吞吐对比；现有 [Benchmark](BENCHMARK_2026-10-25.md) 只测了当前实现。

### 服务实体的持久化和并发

目前 `EntityScriptHost` 的托管操作读取状态并走最终 CAS 提交，而 `runtime.rpc` 的无状态处理器
可以不走实体存储。若将后者机械地迁入当前托管路径，LoginHub 会增加存储及串行等待成本。
`persist=false` 属性也不代表整次调用跳过持久化提交。

建议统一实体身份和 RPC，同时明确服务实体是否需要持久状态，以及对应的生命周期、回执、失败恢复合同。
如果提供内存服务实体模式，它属于尚待实现的通用引擎能力；不能悄悄跳过现有持久实体的提交和 fencing。
需要跨重启可靠投递的实体仍使用持久合同，暂态服务的可接受状态丢失由应用选择。

同一实体的 mailbox 仍串行。多 LoginHub 分担入口工作；若它们全转发到一个全局管理实体，
该实体仍会成为串行集中点。按账号/房间分散协调，比只增加 LoginHub 数量更能分散实际执行压力。

## 实现复杂度

下表是定性工程评估，不是工期或已完成量。

| 子项 | 复杂度 | 主要工作/风险 |
| --- | --- | --- |
| 外观 API 统一，暂保留旧复合身份 | 中 | 可做过渡层，但尚未满足全局身份目标，不能标为完整新方案 |
| 全局 ID 分配与占用 | 中高 | 多节点、重启、误配置、注入碰撞、销毁不复用和历史占用 |
| 单记录元数据与寻址/类型解析 | 中高 | 现有处理器在读取实体前已按 route 选类，新路径要调整解析、权限和 mailbox 准入次序 |
| SDK/Lua 与账号归属 | 中 | 外部归属解析可复用，但 self.id、实体查询及旧业务关联需检查 |
| 登录前准入与登录后会话绑定 | 中高 | 当前示例依赖预先认证；真实 LoginHub 还要协调 Gateway 身份与既有委托连接 |
| 跨节点创建与多入口重试 | 高 | 身份分配、目标登记、激活、丢 ACK、参数冲突及账号绑定不是一个跨记录事务 |
| 交接状态机及客户端恢复 | 中 | 复用已测流程，统一引用并补元数据、权限和旧句柄回归 |
| FSA 值、差量、预测算法 | 低至中 | 算法复用；调整引用/事件并验证确认与预测身份保持 |
| 数字方法 ID 与版本协商 | 中高，可拆分 | 稳定方法表、类型/版本不匹配和旧客户端处理 |
| 暂态服务实体模式 | 中高，可拆分 | 内存状态、租约失效、错误恢复及持久投递能力边界 |
| 新旧客户端同时在线 | 高 | 双协议、别名解析、权限一致性和兼容退出条件 |
| 历史记录与未完成投递迁移 | 高 | 旧复合键映射、业务嵌入引用、回执摘要/渠道、备份与可恢复转换 |

已有数据迁移还需区分：Storage 删除 tombstone 保护版本不复用，不自动等价于新的 EntityId
永不重新分配；新分配/占用合同必须明确覆盖已销毁身份。

没有需保留的部署时，可以统一切换协议/记录版本并重建演示数据，降低兼容工程量。
有存量数据时，应先做停止写入、备份、稳定旧键→新 ID 映射、记录/引用转换与校验，
再按恢复点推进切换；不透明业务 payload 内的引用需要业务迁移钩子。

## 建议落地顺序

1. 固定全局 EntityId、EntityRef、类型元数据和归属合同，明确创建前后与销毁后的状态。
2. 打通一个公共调用路径，先保留方法名；类型化 API 作为便利包装，系统控制路由继续保留。
3. 接入多 LoginHub 发现与跨节点幂等创建，明确服务实体是否持久化。
4. 统一 SDK、订阅、预测、交接和持久消息引用，完成旧状态迁移或明确新版本切换边界。
5. 执行普通/内存/独立进程故障回归，再测协议字节、冷/热路由、服务实体吞吐、尾延迟与持久提交。
6. 依据测量结果，独立决定是否引入数字方法 ID 和连接内引用压缩。

推荐采用全局实体作为长期业务模型，同时将方法编号压缩、新旧版本并行等工作单独评估，
保持每一阶段可验证，继续复用客户端/服务端公共引擎。
