# 指定目标节点的实体交接

`EntityTransferLimits::timeout` 是每个网络阶段的期限，必须大于零且不超过
`Node::max_call_timeout`（30 秒）；无效配置在构造时拒绝，不会先安装交接路由。

服务端可以把同一全局 EntityId 的实体交给指定的稳定节点 ID。客户端连接 Gateway，保持原订阅 ID、
实体对象和镜头引用；Gateway 恢复目标上的完整快照后继续差量同步。业务决定地图、出生点和
可见性，引擎负责所有权交接，不内置地图规则。

## Lua API

在源 owner 上经过业务权限校验的其他实体方法或后台任务中调用：

```lua
-- Player 已在所有目标 owner 注册相同的类、属性和生命周期逻辑。
-- player_id 和 target_node 由服务端业务决定；不要直接信任客户端给出的目标。
local status = Player:transfer(player_id, 'owner-b')
if status == runtime.status.ok then
    -- owner-b 已恢复、执行 on_activate，并完成持久化提交。
    -- Gateway 的订阅恢复异步完成，客户端不需要重新登录或 spawn。
end
```

返回状态，失败不自动抛错。未安装原生交接组件时返回 rejected。不能在 Player 自己的方法中
同步等待自身迁移，会拒绝而非死锁。源实体须已驻留、成功激活且 mailbox 空闲；有执行中或
排队请求时拒绝，可在外部任务稍后重试。接受交接后暂停源 mailbox 的新请求与 tick。

- not_found：目标未配置、不承载该类型或恢复重试时实体记录不存在。
- overloaded：协调器额度、目标容量/排空等临时限制。
- rejected：非法/本节点目标、忙碌/失效句柄、鉴权失败或与持久目标不符。
- timeout / disconnected / internal：阶段未成功确认，**不能推断源仍持有实体**。

超时后使用**相同 type/id 和相同目标**重试；源已卸载时，协调器读取持久交接编号并再次向目标
确认，不重新执行源卸载。目标已激活且仍持有有效实例时，不重复激活。若要继续转到第三个
节点，应由当前 owner 发起新交接，不能从旧源覆盖已提交的目的地。

## 原生组合

```cpp
#include <fsnet/entity_transfer.hpp>

// owner_node 已按证书建立可信 server 身份；Gateway 委托连接的角色仍为 client。
fsnet::EntityTransfer transfer(owner_node, entity_host, database, endpoints,
    trusted_owner_identity);
// 每轮 Node、entity_host、Lua、复制服务和 GatewayDelegation 之外：
transfer.poll();

// player_id 为可信业务保存的 fsnet::EntityId。
transfer.transfer("Player", player_id.str(), "owner-b", [](fsnet::Result result) {
    // ok 表示目标激活提交已确认；并不等待所有观察者收到快照。
});
```

`trusted_owner_identity` 必须核对已认证身份；生产启动器同时检查真实连接的证书指纹和主体。
组件在 host 安装 `transfer_entity` 回调，绑定两个仅 server 可调用且额外校验 Trust 的内部路由：
`system.entity_transfer.probe`、`system.entity_transfer.accept`。不会向 Gateway 的客户端路由
自动暴露。每个 host/Node 只安装一份组件；先停止组件，再停止 host 和 Node。
组件使用 Node、共享实体 key 编码、原有属性/租约/CAS 引擎，没有单独复制属性状态。
默认最多 64 个并行交接，同实体只允许一个在途交接，每个网络阶段 10 秒超时。

低层 `EntityScriptHost::handoff(handle,target,reply)` 返回 8 字节交接编号，供可信框架组合；
`accept_handoff(type,id,transfer_id,reply)` 校验持久目标和编号。低层 handoff 不执行网络预检，
通常应使用 EntityTransfer。原生 Node 须比组件、实体 host 和 VM 存活更久。

## 协议与持久状态

1. **预检**：源联系指定端点，验证目标稳定节点名称、类型是否注册、容量及是否排空。
   失败时不修改源实体；预检不预留容量，不是跨节点资源事务。
2. **释放**：源检查本地句柄和空闲 mailbox，执行 `on_deactivate('transfer')`。最终持久状态、
   原有去重回执、目标稳定节点和交接编号（源记录存储版本，删除重建不复用），在**同一个带 fencing 的实体记录 CAS** 中提交；
   同时清空源 owner/租约。源提交失败则保留原状态和实例，不发布半份交接。
3. **接管**：目标检查记录确实指向自己且编号匹配，用已有租约 CAS 获得新 fence，恢复持久状态，
   执行 `on_activate`，最终状态提交后才回复 ACK。没有获得租约的节点无法执行/提交业务。
4. **恢复复制**：Gateway 按同一目录路由规则找到目标，为原订阅取快照并恢复差量。
   目录缓存短暂指向旧 owner 时，内部快照请求返回可重试的 overloaded，不把它当权限拒绝删副本。

源释放后宕机，持久目标仍有效。目标激活失败可在目标重试；已获得目标租约但尚未激活的状态
不会被报告成功。目标确认丢失时，重试确认检查同一交接编号；旧编号不能接管后续交接。
原始业务 RPC、预测输入不随交接自动重发，原有持久回执保留窗口仍适用。

目标 `placement` 在接管、续租、普通提交、卸载和重启后保留。**租约过期也不退回随机节点**，
其他节点或配置缺少该目标时返回不可用，而不是把地图实体放错节点。目标故障恢复需重启同一
稳定节点的新 incarnation，等待旧租约到期；当前没有绕过目标的行政强迁/取消 API。

## 交付启动器配置

在所有参与 owner 的环境中同时配置：

```sh
FSNET_ENTITY_ENDPOINTS=/etc/fsnet/endpoints.tsv
FSNET_OWNER_PINS=/etc/fsnet/owner-pins.tsv
```

端点文件与 Gateway 的五列格式一致，例如 `owner-b 10.0.0.12 7001 Player owner-b.example`。
owner-pins 为 `sha256:<64位小写十六进制> owner-b`，只列可信 owner 证书；与原有 Gateway pins
分开，禁止两类清单使用同一个证书。owner 现在也发起 mTLS 连接，其证书需要 serverAuth/clientAuth。
证书 SAN 必须匹配端点 TLS 名称。未配置这两个变量时保留原接入行为，不启用交接组件。

## 升级和边界

有无指定目标均使用 **FER5**，目标、交接编号、初始化标志与全局元数据在同一记录中保存。
实体记录仅支持 FER5，计时器发现索引仅支持 FTI2；历史记录、旧 key、FTI1 与未知版本均拒绝，
不会自动转换或在读取失败后覆盖原记录。全部 owner、Gateway 和目录读者必须采用同一当前合同。
没有旧存档迁移/兼容恢复工具，不能通过手工删除 placement 改写已提交的目标。

所有参与节点应部署兼容 schema/脚本。预检只检查类型注册，不比较脚本版本；不兼容状态会在
目标恢复/校验时失败，目标仍被保留，需修复部署后重试。没有在线 schema/模块状态迁移。

仅转移持久属性与去重回执；persist=false 属性、Lua 协程、等待中的调用和进程资源不随迁移搬运。
生命周期 hook 可能在失败/重启后重新执行，外部副作用仍需业务幂等保障。

客户端迁移期间保留最后确认状态，属性写入可能返回 overloaded。控制元数据可产生
unpossess/possess 回调，表现 spawn 不重复。尚无迁移期间输入自动缓存补发、地图预加载、AOI
成员持久化、跨实体/地图事务或零停顿承诺。观察者恢复仍由现有 Restore 策略重新验证权限。

验证：`fsnet-global-flow` 覆盖两节点定向交接、稳定 ID、客户端订阅/控制、持久命令和随实体迁移的计时器。

服务端持久命令已新增 outbox/inbox 协议，使用 `ref:send(...)`，随实体迁移保留，
详见 [ENTITY_DELIVERY.md](ENTITY_DELIVERY.md)。普通同步 call 和客户端预测输入仍保留原语义。
