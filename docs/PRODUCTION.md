# 交付与部署验收

交付程序 `fsnet-service` 提供 owner / gateway 两种模式，配套配置位于 `deploy/`。
这是可重复部署与故障验收入口。上线仍需目标环境的数据库 HA、网络分区、容量及长稳验收；
本仓库的本机多进程结果不代替这些证据。

## 构建、安装、打包

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DFSNET_WITH_LUA=ON
cmake --build build -j3
ctest --test-dir build --output-on-failure
cmake --install build --prefix /opt/fsnet
cpack --config build/CPackConfig.cmake -B packages
```

TGZ 包包含 `bin/fsnet-service`、`bin/fsnet-client`、`bin/fsnet-native-client`、`bin/fsnet-script`、
`lib/libfsnet.so`、公开头文件、CMake SDK导出目标与可选Lua静态库、
Lua示例、示例启动器、部署模板及文档。
安装后的SDK示例可直接 `python3 /opt/fsnet/share/fsnet/examples/run.py scene --native`。可解压到其他前缀；程序通过 `$ORIGIN/../lib` 寻找库。
Linux 构建仍依赖兼容 glibc/libstdc++ 等系统运行库，不是任意系统通用的静态发行包。
`tools/validate_delivery.py` 同时安装到 `share/fsnet/tools/`，需要 Python3 与 openssl。
可选 PostgreSQL 需构建时启用相应选项，见 STORAGE.md；默认交付验证不要求它。

## 配置与启动

创建专用 `fsnet` 用户，准备 `/etc/fsnet` 与 `/var/lib/fsnet`，私钥和环境配置仅允许服务用户读取。
参考 `deploy/*.env.example` 配置系统服务；这些文件是模板，不包含有效证书或账号。
`deploy/fsnet-owner@.service` 默认每台机器 owner 端口 7001；同机运行多个 owner 时应覆盖
ExecStart 使用不同端口。节点 ID 必须稳定且不同，重启自动生成新的 incarnation 用于 fencing。

各进程必须显式提供持久存储 URI 和 CA/证书/私钥，缺失即启动失败：

```sh
# 两个 owner 和 gateway 指向同一逻辑存储。
export FSNET_STORAGE_URI=file:/var/lib/fsnet/state
export FSNET_TLS_CA=/etc/fsnet/pki/ca.pem
export FSNET_TLS_CERTIFICATE=/etc/fsnet/pki/owner-a.pem
export FSNET_TLS_KEY=/etc/fsnet/pki/owner-a.key
export FSNET_METRICS_FILE=/var/lib/fsnet/owner-a.json
/opt/fsnet/bin/fsnet-service owner /etc/fsnet/server.lua 7001 owner-a /etc/fsnet/gateway-pins.tsv
# 使用 gateway 自身证书及环境配置运行：
/opt/fsnet/bin/fsnet-service gateway 7000 /etc/fsnet/endpoints.tsv /etc/fsnet/clients.tsv /etc/fsnet/routes.tsv
```

默认只绑定 loopback。配置 `FSNET_BIND_ADDRESS` 后再开放所需端口；owner 仅接受内网 Gateway
证书白名单，玩家只访问 Gateway。owner 证书需 serverAuth，Gateway 因前后端共用证书需要
serverAuth/clientAuth，玩家需 clientAuth。端点 TLS 名称必须匹配证书 SAN，不能关闭名称验证。
无 cluster token 的 mTLS 可用；证书认证和账号授权仍是两层不同检查。

清单均为空白分列，`#` 开头为注释，最多 4096 行，不支持带空格字段：

| 文件 | 列与示例 |
| --- | --- |
| gateway-pins.tsv | `sha256:<64位小写十六进制> gateway-a`，owner 允许的 Gateway 证书及身份 |
| clients.tsv | `sha256:<64位小写十六进制> alice`，Gateway 认证账号映射 |
| endpoints.tsv | `owner-a 10.0.0.11 7001 Player owner-a.example`，节点/地址/端口/类型列表/TLS名称 |
| routes.tsv | `account login Player`、`entity Player move,read`，仅暴露列出的业务入口 |

可用 `openssl x509 -in CERT.pem -noout -fingerprint -sha256` 获取指纹，删除冒号并转为小写，
添加 `sha256:` 前缀。清单更新需滚动重启；没有在线证书吊销和配置热加载。
启动器默认只恢复账号自己所属的实体；观察者/AOI 恢复需在宿主中提供 Restore 策略。

`file:` 仅用于同机共享 POSIX 文件目录，本机测试不支持把本地目录复制到不同机器冒充共享存储。
跨机必须使用所有节点可见、提供相同 CAS 合同的存储，例如可选 PostgreSQL；数据库的复制、
仲裁、备份恢复和 TLS 验证由部署方另行验收。SQLite/网络文件系统不可直接据此推断跨机安全。

指定目标交接可选配置 `FSNET_ENTITY_ENDPOINTS` 与 `FSNET_OWNER_PINS`，使 owner 能按独立的
owner 证书白名单互连；与 Gateway pins 分开，owner 证书同时需要 clientAuth/serverAuth。
启用前须升级全部 owner/Gateway 以读取 FER3，见[交接部署与升级合同](ENTITY_TRANSFER.md)。

## 健康、排空与升级

`system.health` 正常返回 ok / ready，排空后返回 overloaded / draining。可选
`FSNET_METRICS_FILE` 每 100 ms 原子替换 JSON；监控需同时检查进程、文件更新时间和 ready，
异常退出留下的旧 ready=true 文件不能算健康。文件包含连接/排队资源；owner 增加实体、提交、
失败和 Lua 内存，gateway 增加订阅、迁移和转发计数。无业务载荷、账号或私钥。

SIGTERM/SIGINT 将 readiness 置 false。owner 停止接单、等待现有请求、运行卸载 hook、
提交并释放租约，最多 35 秒，超时退出非零；Gateway 停止新请求并等待最多 4 秒后关闭。
systemd 停止时限分别配置 45/10 秒，避免正常 drain 被过早 SIGKILL。先从负载均衡摘除再停止。
稳定账号、实体类型和 ID 必须跨版本一致。

FER2 初始化标记不能被旧可执行程序读取。首次从旧版本升级时，先备份并排空旧 writer，再启动
新 writer；回滚前必须确认存储兼容，不能把旧二进制直接连接到含 FER2 的存储。
模块热更新/状态迁移尚未实现，不支持在运行中替换 schema。hook 的外部副作用需业务幂等，
详见 ENTITY_LIFECYCLE.md。Gateway 故障后客户端需由应用重新连接、登录和重建订阅。

## 可重复故障验收

```sh
python3 tools/validate_delivery.py \
  --service build/fsnet-service --client build/fsnet-client \
  --rounds 100 --output delivery-report.json
# 对解压后的交付包重复同一检查：
python3 /opt/fsnet/share/fsnet/tools/validate_delivery.py \
  --service /opt/fsnet/bin/fsnet-service --client /opt/fsnet/bin/fsnet-client \
  --rounds 1000 --output delivery-release.json
```

脚本临时生成 PKI，启动两个 owner、Gateway 和 Lua 客户端，通过真实 loopback mTLS 检查：
未知 CA 拒绝、合法但未映射证书拒绝、可信账号委托、自动实体、SIGKILL owner 后租约接管、
确认状态与已创建标记保留、相同 RPC ID 跨 owner 去重、字段合批负载、重启原节点、优雅交接、
前端视图不重复创建、资源清理和最终 readiness。超时/断言令验证非零退出，finally 清理进程。
临时证书/私钥不进入报告；报告包含二进制哈希、步骤、耗时和采样资源峰值。

普通 CTest 包含 100 轮验收，sanitizer 单独运行原生生命周期与 Gateway 测试。
`--rounds` 是单账号串行合批次数，不是并发容量压测；资源采样峰值也不是严格分配峰值。
需另外在目标机器验证真实并发、慢客户端、磁盘满/断电、跨机网络分区/时钟漂移、数据库 HA、
证书轮换及长稳容量，定义可接受的恢复/延迟 SLO。不得用本机耗时推算玩家在线规模。
