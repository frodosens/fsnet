# 面向对象的 Lua 实体 API

本文保留机制说明与显式 legacy 夹具；当前业务接入以 [API 指南](API_GUIDE.md) 和
[全局实体](GLOBAL_ENTITIES.md) 为准，兼容入口见 [legacy 适配器](LEGACY_ADAPTERS.md)。

端到端组合与当前能力总览见 [API 使用指南](API_GUIDE.md)；本文保留类/RPC/作用域的详细合同。

这是业务脚本的推荐入口。`fsnet.Entity` 随 Lua 适配器内置，不依赖本地 Lua
库、`require` 或运行时文件系统。C++ 负责调度、网络、权限和持久化，游戏规则写在 Lua。
旧的 `runtime.entity` / `runtime.legacy_rpc` 注册接口继续作为底层及兼容接口。

## 定义类和实体方法

```lua
local Entity = fsnet.Entity
local Combatant = Entity:extend('Combatant')
Combatant:attr('hp', {type='int32', default=100, client='r', sync='observers'})

function Combatant:is_alive()
    return self.hp > 0
end

Combatant:rpc('damage', {callers={'server'}})
function Combatant:damage(payload)
    local damage = tonumber(payload)
    if not damage or damage < 0 then self:reject() end
    self.hp = math.max(0, self.hp - damage)
    return tostring(self.hp)
end

local Player = Combatant:extend('Player', {owner='legacy_id'})
Player:attr('name', {type='string', default='Player', client='rw', sync='owner'})

Player:rpc('respawn', {callers={'client'}, access='owner'})
function Player:respawn()
    if self:is_alive() then self:reject() end
    self.hp = 100
end

function Player:validate_write(proposal)
    return not proposal.name or (#proposal.name > 0 and #proposal.name <= 32)
end

Player:register()
```

属性读写直接使用 `self.hp`，调用本地方法使用 `self:is_alive()`。本地方法不会成为网络
入口；只有 `:rpc` 显式导出的方法才注册网络路由。RPC 处理器接收一个二进制字符串，
返回字符串或不返回值（对应空响应）。不会隐式 JSON 编解码。当前执行上下文通过
`self.context` 访问，包括 `caller`、`subject`、`peer`、`entity_id` 和 `entity_type`。
`self.id` / `self.type` 给出当前实体身份，不能直接赋值。

`self:reject()` 以 rejected 结束操作，即使被 pcall 捕获也不会提交状态。错误、超时或
失去 fencing 同样不会提交本次属性镜像。外部 RPC 的副作用不随本地失败自动撤销。

## 类、继承和权限

| 接口 | 含义 |
| --- | --- |
| `Entity:extend(name, options)` | 定义实体类；类名唯一，1–127 字节，无点/NUL/保留前缀 |
| `Class:extend(name, options)` | 单继承，继承属性、本地方法、RPC 声明和 owner 选项 |
| `Class:attr(name, definition)` | 声明类型、默认值、读写权限、同步范围和持久化 |
| `Class:rpc(name, {callers=..., access=...})` | 声明远程入口；业务函数用普通 `function Class:method` 定义 |
| `Class:private(name)` | 显式隐藏继承的远程入口，保留本地方法 |
| `Class:register()` | 编译并注册类，同时封存此类和祖先的定义 |
| `Class:legacy_ref(id, options)` | 创建有本地 RPC 声明的逻辑实体引用 |
| `Entity:legacy_reference(type, id, options)` | 创建没有本地类定义的跨节点实体引用 |

属性合同与 [ENTITY_SCHEMA.md](ENTITY_SCHEMA.md) 一致。属性与方法不能重名，框架成员和
生命周期名称保留；权限字段拼写错误、缺少实现、未知配置、重复声明在加载时拒绝。
类的注册批次失败时会撤销本批收集的路由和 schema；即使脚本用 pcall 捕获错误，也不会
留下半个类或占用路由名额。这是加载声明的回滚，不使用数据库事务。
未知实例字段不允许随意增加，需要保留跨操作的临时值时声明 `persist=false` 属性。

覆写普通方法可以直接重新定义；覆写继承的 RPC 时必须在子类重新声明 `:rpc` 权限，
或者用 `:private` 隐藏，避免不经意继承客户端入口。例如：

```lua
Player:rpc('damage', {callers={'server'}})
function Player:damage(payload)
    return Combatant.damage(self, payload) -- 显式调用基类实现
end
```

类注册后不可通过普通赋值修改定义，但仍可创建子类。无需注册只作为基类的抽象类。
`owner='legacy_id'` 表示账号 subject 与实体 ID 对应；不声明时使用宿主的外部 owner 映射。
子类可用 `owner='external'` 显式取消继承的 id 映射。外部映射仍需按实体生命周期串行变更。
类定义及权限不是热更新协议，已有持久化属性的 schema 修改需要明确迁移。

## 实体引用和异步 RPC

```lua
-- 在另一个实体或无状态 RPC 中：
local player = Player:legacy_ref('42')
local result = player:damage('10', {timeout_ms=1000, request_id='operation-123'})

-- 不在本进程加载 Player 实现时：
local remote = Entity:legacy_reference('Player', '42')
local result = remote:call('damage', '10', {timeout_ms=1000, request_id='operation-123'})

-- 连接已由 runtime.connect 建立，显式选择节点：
local player = Player:legacy_ref('42', {peer=peer, timeout_ms=1000})
```

未设置 peer 时沿用 `RoutedScriptHost` 的实体 owner 路由；设置 peer 时使用明确的连接。
引用不会创建本地实体对象，也不缓存权威属性；实体激活仍由宿主接收请求时完成。
有类定义的引用只允许调用声明过的 RPC，无法远程调用私有方法；无本地定义的引用用
`:call`，服务端依然检查实际路由权限。引用上的方法调用会让出当前请求协程，并保留
当前实体 mailbox；返回后继续使用同一 `self`。同一个实体严格串行，不同实体可推进。

等待调用当前实体的 RPC 会直接报错，应该使用 `self:local_method()`。跨实体的循环
等待仍需要由业务避免并受 deadline 约束。request_id 的重试去重沿用有界实体回执合同。
引用不会额外授予调用权限，也没有补齐跨 Gateway 的客户端身份委托功能。

## 对象作用域和现有生命周期

`self` 是原生 userdata，不是可以随意 rawset 的普通表。它绑定当前请求/实体执行作用域：

* 同一请求 await 前后可继续访问同一个对象。
* 在其他实体或其他请求中访问缓存的 self，会被拒绝，即使原请求仍在等待。
* 方法返回、异常、超时或取消后，保存到全局的 self 不能再读写属性或调用实例方法。

跨请求请保存实体 ID 或逻辑引用，不要保存 self。`self.context` 是本次请求的上下文表；
修改它不会改变原生认证身份。实例对象不共享可变全局业务状态，原生属性仍在存储 CAS
成功后才按权限同步到客户端，无需业务代码手动发送同步 RPC。

当前可覆写的框架 hook：

```lua
function Player:on_tick(step_ms)
    -- 固定步长；与普通方法共用 mailbox。可直接修改声明的属性，不必返回响应。
end

function Player:validate_write(proposal)
    -- 客户端属性提案；返回 true 接受，false/nil 拒绝，具体合同见 ENTITY_SCHEMA.md。
    return true
end
```

服务端已提供事务化 create/activate/deactivate/destroy hook，以及显式卸载/删除和 drain；
签名、提交/重试与宕机边界见 [生命周期](ENTITY_LIFECYCLE.md)。`new`/`init` 仍不作为持久实体构造 hook。

客户端/服务端共享类、本地实例与确认副本见 [共享实体](SHARED_ENTITIES.md)。

完整例子见 [attributes.lua](../tests/fixtures/lua/attributes.lua)。

服务端 `legacy_spawn_or_restore`、连接可见性与客户端表现生命周期见
[实体存在性与表现](ENTITY_PRESENCE.md)。客户端表现 hook 不代替服务端实体生命周期。

结构属性、固定数组、整值与嵌套赋值自动同步见 [STRUCTURED_ATTRIBUTES.md](STRUCTURED_ATTRIBUTES.md)。

客户端 `:batch()`、动态容器与默认双向字段差量的当前合同见 [合批](ATTRIBUTE_BATCHING.md)、
[容器](CONTAINER_ATTRIBUTES.md)、[上下行共用差量](SHARED_ATTRIBUTE_DELTAS.md)。
