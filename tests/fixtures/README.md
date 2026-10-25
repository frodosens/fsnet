# 当前测试夹具

`lua/timer_entity.lua` 使用全局实体和持久计时器 API。
`tests/support/runtime.hpp` 为测试提供真实 EntityScriptHost、LuaEngine、Catalog 创建与持久属性读取。
所有业务夹具使用 Catalog 分配的 EntityId；不提供任意字符串 ID 或账号路由适配器。
可运行应用见 [示例](../../examples/README.md)。
