# 上下行共用差量

2026-10-24。默认客户端 write、隐式字段赋值和 batch 已与服务端广播共用 `attribute_delta`：
相同的前后值使用相同的递归字段/容器差量生成器、FSA1/FSA2/FSA3 编解码和副本补丁应用器。
不是另写一套客户端操作记录，也不会重放 setter。

```text
客户端确认图像 + 本次最终修改
  → attribute_write_delta → attribute_delta
  → 上行属性替换或字段/容器补丁
  → 服务端授权、候选状态应用、类型及业务校验
  → CAS 提交成功
  → attribute_delta 按订阅者可见状态生成确认差量
  → 客户端直接应用到确认副本，不触发再次提案
```

## 客户端行为

业务代码不变：

```lua
runtime.start(function()
    local player=Player:watch('alice')
    player.data.position.x=10
    player:batch(function(draft)
        draft.items[64].count=2
        draft.tasks['daily.1'].progress=10
    end)
end)
```

前提仍是 owner、属性可写、预期版本正确且通过服务端业务校验。
原生 `AttributeClient::write` 收到最终属性值后，与本地确认副本比较，仅把请求涉及的属性交给共用生成器。
补丁不省字节时回退整属性；未拥有确认值（如 write-only 属性）或显式预期版本与本地版本不同时，保留全值提案。
不同版本不推测、不自动合并，服务器仍按权威版本接受或拒绝。

**显式 request_id 的兼容策略**：当前 SDK 保留固定请求 ID 写入的 FSA1 全值表示，暂不使用差量。
原因是调用方可能在确认副本已推进后，以同一 ID、同一值和固定预期版本重试；重新按新基准生成补丁会改变原始请求字节，
破坏现有去重语义。保持全值可延续旧重试合同。仅提供 ID 而不保持同一预期版本和输入，并不保证安全重试。
默认自动请求 ID 的赋值、write 和 batch 使用差量；本轮没有新增持久化提案缓存。

显式提交的未变化属性仍保留在提案中，继续检查该属性的写权限并交给 validator；
不会因差量把一个未授权的 no-op 丢掉而返回成功。Lua batch 本来就只收集最终变化，整批无变化时不发请求。

## 服务端应用与校验

共用 `stage_attribute_proposal` 用于原生实体入口、Lua `_attributes_proposal` 和 `_attributes_apply`：

1. 检查记录界限、非快照、`base == revision == 当前权威版本`。
2. 先对所有涉及的顶层属性检查调用身份、owner、写权限及属性存在性。
3. 在独立候选副本中使用 `AttributeReplica::apply` 应用路径/容器补丁，保留未修改的字段。
4. 用服务端 schema 检查最终属性类型、范围、容器容量/唯一性及合并图像限制。
5. `validate_write` 仍收到**涉及属性的最终完整值 table**，既有业务校验无需识别补丁。
6. 通过后按原有提交路径发布；失败、非法路径、越界、版本冲突等不部分生效。

服务端从不直接信任客户端补丁为确认状态，也不信任客户端传来的“新版本号”。
FSA1 全值提案继续可用。旧版服务端拒绝 FSA2/FSA3 上行提案，因此使用默认差量写入的客户端需要配套升级服务端。

## 大小与验证

`fsnet-container-benchmark wire 10` 使用同一前后状态比较两端生成器：

| 场景 | 整属性 FSA 字节 | 默认上行 | 下行 |
| --- | ---: | ---: | ---: |
| 1 KB 未变 payload，仅修改 data.position.x | 1111 | 54 | 54 |
| vec3，仅修改 x | 67 | 48 | 48 |
| 128 个 Item，仅修改一个 count | 4146 | 52 | 52 |
| list 追加 16 个 Item | 4664 | 556 | 556 |
| map 添加 16 个键 | 5200 | 805 | 805 |
| map 删除 16 个键 | 4008 | 191 | 191 |

[原始编码样本](benchmarks/shared-deltas-2026-10-24.json)。数字不含请求 ID、FSN1/RPC/TCP 信封和确认，不是网卡流量或耗时测试。
原生测试验证上下行生成的补丁除版本元数据外一致、候选状态不提前发布、只读/跨 owner/类型/路径/版本拒绝，
未变化属性仍校验；既有真实 TCP 两客户端、batch、validator 拒绝与固定 ID 重试测试也覆盖新流程。
Release 链路 smoke 额外核对 10 轮更新：逐条 160 次写/提交、batch 10 次写/提交，没有确认应用再次触发写入。

临时候选属性、Lua/SDK 接口、服务端图像仍有复制与遍历；本轮统一的是算法、编码和应用机制，
不宣称这些内存/CPU 成本已经消除。也未引入公共前缀树编码，字段路径仍使用 FSA3 扁平路径格式。

## 2026-10-25：接收时的原子预算检查

`AttributeReplica::apply(update, max_image_bytes)` 在修改之前检查合并图像的编码大小、原生内存费用、
路径和值约束；预算不足抛出 `AttributeReplicaBudget`，旧状态和版本保持不变。
`image_bytes()` 返回成功应用后的费用，供宿主维护多副本总预算。服务端提案复用相同的应用路径。

Native AttributeClient 直接使用这一路径，去掉为预算检查复制整个副本的步骤。
没有旧快照读者时只准备修改的属性；持有旧快照时仍执行 COW，保证读者不可变。
字段 patch 仍会物化被修改的顶层属性，完整图像的大小校验仍需遍历；不表示所有大对象修改已无复制成本。
