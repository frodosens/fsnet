# 有类型的结构属性与嵌套赋值

属性支持具名结构、固定长度数组，以及它们的嵌套组合。变长 list/map/set 见 [容器属性](CONTAINER_ATTRIBUTES.md)。
整值赋值和字段/数组元素赋值
都经过相同的类型、权限、版本和提交机制。C++ 不内置玩法结构，数学类型只是通用描述的简写。

```lua
local Vec3=fsnet.struct('Vec3',{x='number',y='number',z='number'})
local Pose=fsnet.struct('Pose',{position=Vec3,rotation=Vec3})
Player:attr('position',{
    type=Vec3, default={x=0,y=0,z=0},
    authority='driver', client='rw', sync='observers',
})
Player:attr('pose',{type=Pose,client='rw',sync='observers'})
Player:attr('matrix',{type='mat4x4',client='rw',sync='observers'})

-- 服务端实体方法中，或者有写权限的客户端异步任务中：
self.position={x=10,y=0,z=20}
self.position.x=11
self.pose.position.y=5
self.matrix[1][4]=20
```

`fsnet.vec3` 与字符串类型 `'vec3'` 均代表 x/y/z 三个有限 number 字段。
`fsnet.mat4x4` 与 `'mat4x4'` 代表 4 行、每行 4 个 number 的固定数组，使用 `[行][列]`。
如需 16 元素平面布局，可声明 `fsnet.array('number',16)`。
结构/数组没有显式 default 时，递归使用零、false、空字符串；矩阵默认为零矩阵，
需要单位矩阵时显式传入完整 default。原有标量属性的 default 要求保持不变。
结构名用于描述，不作为网络上的名义类型标识；相同字段形状按相同结构校验。

## 修改如何同步

| 载体 | 整值与嵌套赋值的效果 |
| --- | --- |
| 服务端请求对象 | 修改本次请求镜像，成功提交单记录 CAS 后自动向获准客户端广播 |
| 客户端确认副本，`client='rw'` | 自动提交属性提案并等待服务器验证；成功后由服务器同步确认值及其他观察者 |
| 客户端确认副本，无写权限 | 拒绝；嵌套字段不能绕过属性 ACL |
| 纯本地对象 | 仅允许 driver 属性，本地修改、不联网 |
| 预测状态 | 仅在框架执行输入时允许修改可见 driver 属性；首次预测与回放都不联网 |

客户端例子：

```lua
runtime.start(function()
    local player=Player:watch('alice')
    player.position={x=1,y=2,z=3}
    player.position.x=10
    -- 每条赋值都已等待服务器写结果，读取仍是服务器确认值。
    print(player.position.x)
end)
```

自动提案沿用原生 write 通道、owner 校验、`validate_write`、预期版本及提交后同步。
整个属性继承顶层权限和可见范围，不提供字段级 ACL。非 owner 即使看到 `client='rw'`
属性也不能提交成功。write-only 属性可以整值提交，但不能读取并修改嵌套字段。

客户端赋值需要能让出的运行时任务；不能在加载阶段或预测回放中发送写操作。
服务器拒绝、冲突或超时会抛出 Lua 错误，可用 pcall 处理。确认状态不做提前乐观修改，
超时并不证明服务器没有提交。隐式赋值由 SDK 生成请求 ID、使用当前确认版本和 3 秒
deadline；需要控制重试 ID、版本或 deadline 时，继续使用显式 `entity:write(values,options)`。

每次嵌套赋值基于最新确认值构造**整个顶层属性**，并携带预期版本。两条并发提案不会
静默覆盖：版本不符时拒绝，不自动合并或重试。希望一个向量整体原子修改时使用一次
整值赋值；客户端连续写 x/y/z 会分别等待并提交三次。服务端同一次方法中的多次修改
则在同一次实体提交后合并广播最终值。客户端可用 `:batch()` 显式合并提案；
下行结构/序列/map 支持递归字段路径与容器补丁，详见 [字段路径增量](FIELD_PATH_DELTAS.md)
和 [合批与增量](ATTRIBUTE_BATCHING.md)。

服务器可以独立修改 `client='rw'` 属性，客户端提案仍由服务器决定是否接受。
位置通常推荐采用移动输入 + 服务器计算 + 本地预测；显式声明可写位置意味着应用
选择接受客户端位置提案，并应在 `validate_write` 中校验业务约束。

## 受管字段视图与复制

`self.position`、`self.matrix[1]` 等返回原生 userdata 字段视图，不是可绕过 setter 的
普通 Lua table。`rawset` 和修改 metatable 都不能绕过校验。`#`、`pairs`、`ipairs` 可用于
读取结构/数组。嵌套赋值会复制当前顶层属性、修改并验证候选值，失败不发布部分值。

```lua
local position=self.position
self.position={x=1,y=2,z=3}
position.x=4 -- live path：修改当前 position.x，最终为 {4,2,3}

local value={x=1,y=2,z=3}
self.position=value
value.x=99 -- 普通表已被复制，不再修改实体

other.position=self.position -- 整值复制，不共享可变状态
```

字段视图绑定原对象及访问路径。实体作用域结束、确认订阅撤销或对象 close 后，缓存的
字段视图也会拒绝访问。普通 Lua 表只能通过再次赋值提交，不能将其后续修改自动追踪到
实体。引擎已有的其他 userdata 数学类型需要适配为完整表或本运行时的字段视图。

结构要求字段完整，拒绝额外/缺失字段和不匹配类型；固定数组从 1 开始，不能增删元素、
留空洞或写越界下标。支持嵌套 int32/uint32/int64、number、boolean、string 的相同范围校验。
整数不会隐式转换为字符串，NaN/Infinity 不允许进入属性。普通输入表不能带 metatable。

## 编码、持久化与预算

原有 FSA1 标量 tag 0..3 保持不变，新增 tag 4（具名结构）和 5（序列，固定数组/list/set 共用）。结构字段按
名称稳定排序，数组按索引编码；递归值拥有独立存储，不共享可变子树。原有标量记录可
直接读取；旧二进制不识别新 tag，结构类型上线需要同步更新读写端。已有持久化结构
改变字段或数组维度需要明确迁移，不能用默认值掩盖不兼容的记录。

结构最多 64 字段、数组每维 1..256 元素、值最多嵌套 8 层，每个编码属性镜像最多 4096
值节点、256 顶层属性、1 MiB；类型声明也受深度/节点限制，并在构造默认数组前限制
展开大小。字符串原有限额为 64 KiB。Lua 原生载体/字段视图、复制候选值与同步投影采用
保守的分配计费，嵌套值的节点开销也计入既有内存预算，不只按二进制长度计费。

## 示例与验证

共享类：[structured_entity.lua](../tests/fixtures/lua/shared/structured_entity.lua)。
客户端：[structured_entity.lua](../tests/fixtures/lua/client/structured_entity.lua)。
服务端：[structured_entity.lua](../tests/fixtures/lua/structured_entity.lua)。

`fsnet-structured` 包含原生 schema/codec/复制边界、两个真实客户端与 Lua 服务端，验证
整值/嵌套自动提案、观察者接收与拒写、validator 拒绝、原生作用域与类型/矩阵越界、
脚本失败回滚、预测重放和跨对象副作用限制。`fsnet-structured-process` 使用两个独立
交付程序执行相同语法及服务器校验。
