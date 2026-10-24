# 变长容器属性

已实现 `fsnet.list(Element, max_items)`、`fsnet.map(Element, max_items)`、
`fsnet.set(Scalar, max_items)`。容量参数可省略，默认 256，允许范围 1..256；
容器初始为空，也可以提供完整 `default`。容量是上限，不预分配/填充元素。
固定维度数学数组仍使用 `fsnet.array(Element, length)`，原有 vec3/mat4x4 写法不变。

## 服务端添加 Item 和 Task

```lua
local Item=fsnet.struct('Item',{id='string',count='int32'})
local Task=fsnet.struct('Task',{progress='int32',done='bool'})
local Player=fsnet.Entity:extend('Player',{owner='id'})
Player:attr('items',{type=fsnet.list(Item,128),sync='owner'})
Player:attr('tasks',{type=fsnet.map(Task,128),sync='owner'})
Player:attr('tags',{type=fsnet.set('string',32),sync='owner'})

-- 在服务端实体方法中执行；权限/奖励规则由业务脚本决定。
function Player:grant()
    self.items:add({id='potion',count=1})
    self.tasks['daily.1']={progress=0,done=false}
    self.tags:add('questing')
end
function Player:finish()
    self.items[1].count=2
    self.tasks['daily.1'].progress=10
    self.tasks['daily.1'].done=true
end
function Player:remove_reward()
    self.items:remove(1)
    self.tasks['daily.1']=nil
    self.tags:remove('questing')
end
```

这些方法需要由服务端流程调用；普通方法定义不会自动成为可远程调用的 RPC。
顶层属性默认 `server='rw', client='r'`，背包与任务不必允许客户端直接修改。
一次服务端实体方法里的多次容器操作，随同本次实体状态成功提交后自动同步最终值；
失败不发布，默认也会持久化。可见范围仍由 `sync='owner'/'observers'/'none'` 决定。

## 容器操作

| 类型 | 写法 | 语义 |
| --- | --- | --- |
| list | `items:add(value)` | 追加一个元素 |
| list | `items:insert(index,value)` | 在 1..长度+1 的位置插入 |
| list | `items:remove(index)` | 删除元素，后续下标左移 |
| list | `items[index]=value` | 替换；下标为长度+1 时追加，不允许空洞 |
| list | `items:clear()` | 清空 |
| list / set | `values:contains(value)` | 按值查询，返回 boolean |
| map | `tasks[id]=value` | 添加或替换 |
| map | `tasks[id]=nil` | 删除，不存在时不变 |
| set | `tags:add(value)` | 增加成员，已有成员时不变 |
| set | `tags:remove(value)` | 删除成员，不存在时不变 |
| set | `tags:clear()` | 清空 |
| 全部 | `#values`、`pairs(values)` | 长度与遍历；list 另支持 `ipairs` |
| 全部 | `self.values={...}` | 完整替换，仍进行全部校验 |

修改方法不返回被删除的值，也不返回新增结果。map 不占用 `add`、`remove`、`clear` 等键名，
可以正常存放这些 ID。map 键为 1..127 字节、不含 NUL 的字符串，支持点号等字符；
数字键不会隐式转换。map 按键排序遍历，set 不承诺业务排序。
set 当前支持 int32/uint32/int64、number、bool、string 等标量；完整赋值时拒绝重复值，
不能通过下标修改成员。结构体元素使用 list/map。

list/map 元素可以是结构、固定数组或另一种容器，例如：

```lua
Player:attr('bags',{type=fsnet.map(fsnet.list(Item,32),8),sync='owner'})
-- 在服务端方法中：
self.bags['main']={}
self.bags['main']:add({id='potion',count=1})
self.bags['main'][1].count=3
```

list 元素的字段视图绑定下标，删除/插入会移动下标；不要把 `items[1]` 当作永久对象身份。
需要稳定实体/物品 ID 时推荐 `fsnet.map(Item,128)`，使用 `items[item_id]` 寻址。
普通 table 在赋值时复制，后续直接改 table 不会隐式修改属性；受管字段视图会自动提交修改。
不支持对字段视图使用 `table.insert`/`table.remove`，请使用以上方法。

## 客户端怎样知道变化

客户端沿用自动实体创建或 `Player:watch(id)`，无需额外拉取容器。订阅收到确认更新后，
`player.items`、`player.tasks` 就是新的状态。UI 可在帧更新里检查 `player.revision`，按稳定 ID
与上一帧状态比较，触发新增/删除/修改处理：

```lua
local known={}
local function refresh_tasks(player)
    local next_tasks={}
    for id,task in pairs(player.tasks) do
        if not known[id] then
            print('task_added',id)
        elseif known[id].progress~=task.progress or known[id].done~=task.done then
            print('task_changed',id)
        end
        next_tasks[id]={progress=task.progress,done=task.done}
    end
    for id in pairs(known) do
        if not next_tasks[id] then print('task_removed',id) end
    end
    known=next_tasks
end
```

这是状态同步，不是可靠操作日志，也没有内置逐元素事件队列。同一提交内先添加后删除的
中间状态不会到达客户端；帧间多个确认更新也可能被 UI 合并。必须逐笔消费的奖励/任务事件，
应由业务维护带 ID 的持久化记录并显式确认，不能依赖两份快照的 diff。

`client='rw'` 的容器支持客户端异步任务中的相同增删和嵌套赋值，自动提案、等待服务端
`validate_write`、预期版本校验和提交后广播。确认副本不提前乐观修改，观察者不能越过 owner
写权限。并发提案版本冲突会拒绝，不自动合并，行为与 [结构属性](STRUCTURED_ATTRIBUTES.md) 一致。
纯本地/预测状态只能修改 driver 属性；预测首次执行和回放不会发送网络请求。

## 编码与限制

list/set 复用 FSA1 tag 5 序列编码，长度允许 0..256；固定数组的非零固定长度仍由 schema 校验。
map 使用新 tag 6，独立于 tag 4 固定字段结构，编码条目数、字符串键和递归值。
原有标量、结构、固定数组的有效编码不变；使用新容器时需要同时升级服务端、客户端和离线读取工具，
旧版不认识 map tag，也会拒绝空序列。不自动迁移已有不兼容的属性类型。

下行广播会自动选择递归字段路径、map 键编辑或序列区间 splice，补丁更小时不再发送整个容器/元素；
例如 `items[64].count` 可以只发送该叶字段，见 [字段路径增量](FIELD_PATH_DELTAS.md)。
客户端连续写入可用 `:batch()` 合成一次提案，见 [合批与增量](ATTRIBUTE_BATCHING.md)。
客户端默认提案也共用差量生成器，固定请求 ID 等回退规则见 [上下行共用差量](SHARED_ATTRIBUTE_DELTAS.md)。
每个容器容量上限不替代
总图像上限：4096 个值节点、深度 8、256 个顶层属性、1 MiB 编码、单字符串 64 KiB。
Lua 候选值解析也有递归节点预算，map 的嵌套分配会计入载体/副本内存预算。
元素类型、容量、唯一性或图像预算校验失败，不发布部分更改。

## 可运行示例与验证

共享声明：[container_entity.lua](../tests/fixtures/lua/shared/container_entity.lua)。
服务端：[container_entity.lua](../tests/fixtures/lua/container_entity.lua)。
客户端：[container_entity.lua](../tests/fixtures/lua/client/container_entity.lua)，包含任务新增/修改/删除的通知 diff。
示例 `demo` RPC 仅用于触发确定的服务端流程，不能直接照搬成生产奖励入口。

```sh
cat tests/fixtures/lua/shared/container_entity.lua tests/fixtures/lua/container_entity.lua > /tmp/fsnet-container-server.lua
FSNET_CLUSTER_TOKEN=local-demo FSNET_DEMO_SUBJECT=alice /path/to/build/fsnet-script /tmp/fsnet-container-server.lua 7001

# 另一终端，连接本轮新启动的默认内存服务端：
cat tests/fixtures/lua/shared/container_entity.lua tests/fixtures/lua/client/container_entity.lua > /tmp/fsnet-container-client.lua
FSNET_CLUSTER_TOKEN=local-demo /path/to/build/fsnet-client /tmp/fsnet-container-client.lua 127.0.0.1 7001
```

`fsnet-containers` 覆盖原生 codec/持久化恢复、容量/类型/唯一性/递归预算、两个客户端同步、
owner 自动提案与观察者拒写、脚本失败回滚、嵌套容器、深复制、预测重放和停止内存释放。
`fsnet-container-process` 使用独立的服务端与客户端程序运行上述交付示例。
