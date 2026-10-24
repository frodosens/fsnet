# 当前 API 使用指南

实现快照：2026-10-24。入口为内置 `fsnet.Entity`；共享 schema 和业务函数由两端加载，
运行时机制由公共 C++ 引擎提供。以下片段说明组合方式，完整可运行脚本见文末；
脚本沙箱尚无 require/模块加载，启动前需要拼接共享文件与各端入口。

## 1. 声明共享类与类型

```lua
local Item=fsnet.struct('Item',{id='string',count='int32'})
local Task=fsnet.struct('Task',{progress='int32',done='bool'})
Player=fsnet.Entity:extend('Player',{owner='id'})
Player:attr('name',{type='string',default='',sync='observers'})
Player:attr('position',{
    type=fsnet.vec3, authority='driver', client='rw', sync='observers',
})
Player:attr('items',{type=fsnet.map(Item,128),sync='owner'})
Player:attr('tasks',{type=fsnet.map(Task,128),sync='owner'})
Player:attr('tags',{type=fsnet.set('string',32),sync='owner'})
Player:attr('reward_claimed',{type='bool',default=false,client='none',sync='none'})

-- 示例业务入口；生产奖励条件需要由服务端业务规则决定。
Player:rpc('claim_demo_reward',{callers={'client'},access='owner'})
function Player:claim_demo_reward()
    if self.reward_claimed then return 'already claimed' end
    self.items['starter-item']={id='potion',count=1}
    self.tasks['welcome']={progress=0,done=false}
    self.tags:add('questing')
    self.reward_claimed=true
    return 'ok'
end

function Player:validate_write(proposal)
    -- OO hook 接收涉及属性的最终完整值，不接收内部 patch 数据结构。
    local p=proposal.position
    return not p or (math.abs(p.x)<=100 and math.abs(p.y)<=100 and math.abs(p.z)<=100)
end
Player:define()
```

本例允许 owner 提案修改 position，背包/任务仍默认服务端写、客户端读。
移动类玩法一般优先采用“输入 RPC + 服务端计算 + 本地预测”，不是直接信任客户端坐标。
`owner='id'` 指账号 subject 与实体 ID 一致，不是客户端自行认领。

| 类型 | 声明 | 默认值/边界 |
| --- | --- | --- |
| 标量 | int32/uint32/int64、number/double、bool/boolean、string | 显式提供 default；严格类型/整数范围，number 必须有限 |
| 结构 | `fsnet.struct(name,fields)` | 无 default 时递归零/false/空字符串；字段必须完整 |
| 数学简写 | `fsnet.vec3` / `'vec3'`，`fsnet.mat4x4` / `'mat4x4'` | 三分量、4×4 固定数组；矩阵默认零矩阵 |
| 固定数组 | `fsnet.array(Element,length)` | 长度 1..256，不允许增删或空洞 |
| list | `fsnet.list(Element,max_items)` | 默认空；最大容量默认 256，可配置 1..256 |
| map | `fsnet.map(Element,max_items)` | 默认空；字符串键 1..127 字节、不含 NUL |
| set | `fsnet.set(Scalar,max_items)` | 默认空；当前仅标量元素，成员唯一 |

属性选项：`server` 默认 rw，`client` 默认 r，`persist` 默认 true，`authority` 默认 server。
`client` 为 none/r/w/rw；`sync` 为 none/owner/observers，默认可读属性 owner、不可读属性 none。
`authority='driver'` 允许本地/预测模拟，不授予网络写权限。
`persist=false` 属性在当前驻留期缓存，卸载、重启或接管后恢复默认，不作为背包/任务的持久存储。
同一顶层属性内部字段继承其权限，没有字段级 ACL。总图像预算与类型细节见
[结构属性](STRUCTURED_ATTRIBUTES.md) 和 [容器](CONTAINER_ATTRIBUTES.md)。

## 2. 服务端注册与登录创建

另一个服务端入口文件接在共享声明后：

```lua
Player:register()
runtime.rpc('login',{callers={'client'},handler=function(ctx,name)
    assert(ctx.subject~='' and #name>0 and #name<=32,'invalid login')
    local player=Player:spawn_or_restore(ctx.subject,{name=name})
    player:show_to(ctx.peer)
    player:possess(ctx.peer)
    return player.id
end})
```

`ctx.subject/ctx.peer` 来自可信宿主认证。新实体保存初值，已有实体恢复状态、不重置初值。
返回的是逻辑引用，不是可跨 mailbox 直接写的服务端对象。
登录/会话重入、跨账号可见性、登出清理由业务实现；完整例子见 [player_scene.lua](../examples/lua/player_scene.lua)。

| 服务端引用 API | 效果 |
| --- | --- |
| `Class:spawn_or_restore(id,values)` | 在异步任务中创建/恢复；已注册服务端类可用 |
| `ref:show_to(peer)` | 授予当前认证连接观察权限，推送初始快照 |
| `ref:hide_from(peer)` | 移除自动副本，不删除持久化实体 |
| `ref:possess(peer)` | 控制此可见且归该连接所有的实体；先解除旧目标 |
| `ref:unpossess(peer)` | 解除控制，保留可见性 |

`sync='observers'` 不等于全服广播，必须有可信观察授权。账号归属与当前控制目标分开。
可见性/控制是连接内状态，重连需要重建。通过 [ClientGateway](CLIENT_GATEWAY.md) 接入时，
owner 迁移会保留前端订阅和表现对象；Gateway 断线后的重连仍由应用处理。

## 3. 客户端自动创建与 LocalPlayer/RemotePlayer

```lua
local LocalPlayer,RemotePlayer={},{}
function LocalPlayer:on_spawn()
    self.camera_target=self.entity -- 示意字段，实际镜头由图形引擎适配。
end
function LocalPlayer:on_possess() self.accept_input=true end
function LocalPlayer:on_unpossess() self.accept_input=false end
function LocalPlayer:on_despawn() self.camera_target=nil end
function RemotePlayer:on_spawn() self.position_source=self.entity.position end
function RemotePlayer:on_despawn() self.position_source=nil end

fsnet.entities:register(Player,{owned=LocalPlayer,observed=RemotePlayer})
fsnet.entities:start()
runtime.start(function()
    local id=runtime.server_request('login','Alice')
    -- 服务端返回 RPC 不代替客户端表现事件分发，等待工厂消费创建消息。
    while not fsnet.entities:get('Player',id) do runtime.sleep(1) end
    local view=fsnet.entities:get('Player',id)
    print(view.entity.name)
end)
```

服务端统一使用 Player 类型；客户端 owned/observed prototype 决定本地/远端表现。
hook 顺序为初始快照 → on_spawn → 按控制消息 on_possess；移除时先解除控制，再 on_despawn。
`view.entity` 是确认副本，`owned/controlled` 是服务器元数据，普通表现表不参与授权。
`fsnet.entities:stop()` 停止并清理自动表现；当前管理器不能再次 start。
移除时副本可能已撤销，清理图形资源要用先前保存的资源引用或 ID。

不用工厂时也可以手动订阅：

```lua
runtime.start(function()
    local player=Player:watch('alice')
    print(player.position.x,player.revision)
    player:close()
end)
```

自动副本已经存在时通常直接使用 `view.entity`，无需再建立一份手动 watch。
观察其他账号仍需服务端授权。原生回调与 Lua 读取都会在订阅撤销后失效。

## 4. 属性、容器与提交时机

在服务端实体方法中：

```lua
self.position.x=10
self.items['starter-item'].count=2
self.tasks['welcome'].progress=10
self.tasks['welcome'].done=true
self.tasks['welcome']=nil
self.tags:remove('questing')
```

赋值立即改变本次请求对象；方法正常结束后才执行完整实体记录 CAS，成功后广播并返回成功。
await 不提前提交，仍占有该实体 mailbox；异常/拒绝/超时/失去 fencing 不发布本次属性镜像。
外部 RPC 已产生的副作用不能随属性回滚。相同 self 不能保存到另一个请求使用。

| 容器 | 写入与读取 |
| --- | --- |
| list | `:add(v)`、`:insert(i,v)`、`:remove(i)`、`:clear()`、`[i]=v`；长度+1 可追加，不能留洞 |
| map | `[key]=v` 添加/替换，`[key]=nil` 删除；缺键读取 nil；整值 `{}` 清空 |
| set | `:add(v)`、`:remove(v)`、`:clear()`；不可通过下标修改成员 |
| 全部 | `#container`、`pairs`；list 另有 `ipairs`，list/set 有 `:contains(v)` |

动态元素可以嵌套结构和容器。字段视图沿当前路径解析，普通 table 赋值会复制而非共享。
list 插入/删除会移动下标；需要稳定物品/Task ID 时推荐 map。
客户端 UI 可检查确认版本，并按稳定 ID 对比前后状态；没有内置逐元素业务事件日志。

## 5. 客户端写提案与 batch

普通客户端赋值需要可写属性和异步任务：

```lua
runtime.start(function()
    local player=Player:watch('alice')
    player.position.x=10 -- 发送提案并等待确认，不直接修改确认值。
    player:batch(function(draft)
        draft.position.x=11
        draft.position.y=3
        draft.position.x=12 -- 草稿最终 x=12、y=3，不重复提交中间的 x=11。
    end)
    assert(player.position.x==12 and player.position.y==3)
    player:close()
end)
```

普通赋值每条都等待确认；batch 在独立草稿里立即修改，回调结束后一次 write、validator、CAS。
回调禁止网络、sleep/yield、嵌套 batch、修改其他对象；结束后草稿及其视图失效。
确认状态始终与草稿分开，失败不部分更新；无最终变化的 batch 不发提案。
当前合并边界是一次服务端实体方法或显式 batch，没有按帧/定时发送窗口。

需要指定超时/版本/固定请求 ID 时可显式写：

```lua
local revision=player.revision
local values={position={x=12,y=3,z=0}}
player:write(values,{
    expected_revision=revision,
    request_id='position-operation-42',
    timeout_ms=3000,
})
```

这段需放在已有有效 player 的异步任务中。需要重试时保留相同 ID、版本和输入，不重算一批不同值。
默认 write/赋值/batch 使用共用字段差量；固定 request_id 的 SDK 写入为保持旧重试字节语义，保留 FSA1 全值。
缺失确认基准或显式版本与本地基准不匹配也回退全值，服务器独立决定是否接受。
未变化但显式提交的属性仍需授权、仍交给 validator，不用丢弃 no-op 绕过检查。

默认超时为 3000 ms；显式 write 接受 1..30000 ms。版本冲突、拒绝、超时会以 Lua 错误返回，可 pcall。
超时不是撤销远端提交；去重回执保留有界，不承诺无限期 exactly-once。

## 6. 双向差量与防反馈循环

上下行共用 `attribute_delta`、编码器及候选补丁应用器；服务端额外执行授权、schema、业务校验与提交。
`validate_write(proposal)` 看到涉及属性的最终完整 table，业务不必解析内部 patch。

| 格式 | 当前用途 |
| --- | --- |
| FSA1 | 初始/恢复快照、持久化、整属性替换及回退提案 |
| FSA2 | 顶层容器键编辑和区间 splice |
| FSA3 | 带有类型路径的叶字段替换或嵌套容器编辑 |

`position.x`、`tasks['daily.1'].progress`、`items[64].count` 可以只发送路径和值；
新增完整元素需要发送新元素。多个字段当前是扁平路径列表，不是公共前缀树编码。
每个子树和整个记录都会比较成本，密集修改时可能发送整个子树或属性。
服务器按每个订阅者最后成功发送的可见图像生成广播，首次订阅仍完整快照。

接收确认时原生副本应用补丁，不调用 Lua setter、不重执行业务函数，不自动再次上发。
业务变化回调中主动再写仍可能形成业务反馈循环，需要由业务设计避免。

图像上限：深度 8、4096 值节点、256 顶层属性、1 MiB；patch 最多 256 个编辑、路径最多 8 段。
FSA1/FSA2 继续可读；未知版本旧读端会拒绝，暂无能力协商，新上行补丁还需要支持其提案校验的新服务端。
详见 [字段增量](FIELD_PATH_DELTAS.md) 与 [共用差量](SHARED_ATTRIBUTE_DELTAS.md)。

## 7. 预测独立于确认副本

移动推荐参考已交付的 [player_entity.lua](../examples/lua/shared/player_entity.lua) 和
[客户端场景](../examples/lua/client/player_scene.lua)，不要把上面的可写坐标示例当作移动输入协议。
该示例的共享方法为 `advance`，确认属性为 x/processed_input：

```lua
-- 异步任务中，PlayerEntity 已 define，当前尚未处理任何新输入。
local confirmed=PlayerEntity:watch('alice')
local prediction=confirmed:prediction({acknowledged=confirmed.processed_input})
local sequence=confirmed.processed_input+1
local effect=prediction:push(sequence,'advance','2')
-- effect 可在预测成功后交给音效/动画；重放不会再交付它。
PlayerEntity:ref('alice'):input(tostring(sequence)..':2')
while confirmed.processed_input<sequence do runtime.sleep(1) end
prediction:reconcile(confirmed.processed_input,confirmed.revision)
print(prediction.state.x,confirmed.x)
prediction:close()
confirmed:close()
```

此例 PlayerEntity 是另一份交付模型，不是第 1 节的 Player；不要将两份同名 Player schema 拼在同一 VM。
预测不自动发包或重试输入；服务器必须将累计输入确认与权威状态放在同次提交中。
预测/重放禁止外部副作用，状态与未确认历史在成功后一起替换；初版不是完整世界回滚或渲染平滑器。
本地模拟可用 `Class:spawn_local()`，仅写 driver 属性且不联网；具体限制见 [预测](PREDICTION.md)。

## 8. 原生宿主与工程接入

| 机制 | 入口/职责 |
| --- | --- |
| schema 和值 | `EntitySchema`、`EntityAttributes`；stage 不提交；权威发布以存储成功为前提，候选模型修改不代表持久化 |
| 公共差量 | `attribute_delta`、`attribute_write_delta`、`stage_attribute_proposal` |
| 编解码/副本 | `encode_attributes`、`decode_attributes`、`AttributeReplica::apply` |
| 服务端宿主 | `NodeScriptHost` → `EntityScriptHost`，可选 RoutedScriptHost 路由适配 |
| 服务端同步 | `AttributeReplication(node,entity_host,observer_hook)`；拥有宿主的单个发布监听器 |
| 客户端原生 | `AttributeClient::watch/write/get/unwatch` |
| 客户端 Lua | `ClientScriptHost` 提供确认快照/提案/存在性事件，`LuaEngine` 执行业务 |

Native 值/codec 定义见 [attribute_wire.hpp](../include/fsnet/attribute_wire.hpp)，宿主见
[entity_host.hpp](../include/fsnet/entity_host.hpp)，同步见 [attribute_replication.hpp](../include/fsnet/attribute_replication.hpp)。
Node、宿主、引擎和 manager 在所属线程推进 poll；Node 必须比适配器更晚销毁。
启动器会配置这些机制，嵌入自己的图形引擎时参考 [client_host.cpp](../examples/client_host.cpp)
和 [script_host.cpp](../examples/script_host.cpp)。

逻辑实体引用通过已配置的路由宿主或客户端默认服务器连接发出；直接 NodeScriptHost 没有自动 owner 发现能力。
不要把一个普通服务器连接当作 Gateway 原客户端身份委托。不同端各自的认证、权威状态、存储提交和平台适配保持边界。

## 9. 可运行入口与尚未实现

| 场景 | 共享/服务端/客户端入口 |
| --- | --- |
| 登录、自动 LocalPlayer/RemotePlayer、镜头与移动预测 | [运行说明](../examples/README.md#登录后自动创建-localplayer--remoteplayer) |
| vec3/矩阵/嵌套赋值/batch | [运行说明](../examples/README.md#vec3--mat4x4-与嵌套结构自动同步) |
| 背包与任务增删、客户端状态 diff | [容器运行说明](CONTAINER_ATTRIBUTES.md#可运行示例与验证) |
| 输入确认与重放 | [运行说明](../examples/README.md#客户端预测与服务器校正) |

服务端 create/activate/deactivate/destroy hook、卸载/删除与 drain 已实现，见 [生命周期](ENTITY_LIFECYCLE.md)。
模块资源加载/热更新与状态迁移仍未实现；
Gateway 身份委托/订阅迁移和可部署启动器见 [跨节点接入](CLIENT_GATEWAY.md)、[交付运行手册](PRODUCTION.md)。
仍没有自动几何 AOI、跨实体事务、公共前缀树编码或自动输入重发。
ICE/STUN/TURN 原语不等于完整认证/加密 P2P 链路。完整已实现/计划边界见
[完成状态](ENGINE_COMPLETION.md)、[路线图](ROADMAP.md)、[验证记录](VALIDATION.md)。


## 原生借用输入与确认版本

`Node::call/notify` 同步读取 `string_view`，在返回前生成拥有数据的发送帧；调用方随后可销毁输入。
`parse_entity_message_view` / `parse_state_message_view` 返回借用视图，使用期间必须保留原输入；
要跨 await 或持有独立字符串时，使用原 owning parser 或自行持有原缓冲。
`AttributeReplica::snapshot()` 返回可跨后续 apply/reset 存活的只读版本，旧版本保持不变；
`values()` 的借用引用只保证到下一次成功更新。C++ 嵌入方需按新头文件全量重编，Lua API 和
线上封包不变。实现与测量边界见 [Copy 优化记录](COPY_OPTIMIZATION.md)。
