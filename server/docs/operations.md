# 部署、维护与验收

## 部署形态与容量依据

`all` 是单进程本机验证入口。生产参考 Compose 使用 manager、pipeline、Mosquitto 三个长期进程：manager 执行任务调度、设备状态、管理 API 和网关 MQTT 接入；pipeline 执行解析、隔离、重处理、目标 outbox 和客户 ACK。网关 MQTT 的遥测、任务结果与状态使用独立有界队列，客户投递使用独立客户端。SQLite WAL 数据目录共享在同一 Linux 主机本地磁盘上，不能跨主机或 NFS/SMB 共享。

资源默认上限：manager 1 CPU/512 MiB，pipeline 2 CPU/1024 MiB，Broker 0.5 CPU/256 MiB；这些是便于隔离压力的初始配置，不是已测出的容量承诺。安装预检至少需要 5 GiB 可用磁盘；实际保留数据远大于此时应提前扩大空间。

设备数不能单独决定容量。至少测量 `网关数 × 每网关事件/秒 × 平均报文字节`、平均输出扩张倍数、必达目标数、客户最久停机时间、重试比例、解析耗时和磁盘同步写延迟。队列估算应覆盖 `峰值输入字节/秒 × 需容忍积压秒数 + 目标outbox字节 + 索引/WAL/备份空间`，并留足维护余量。在目标硬件上跑持续负载、客户端离线回灌、服务进程崩溃恢复和磁盘满测试后，才可给出网关数量上限。

## Linux Compose 安装

先准备 Docker Engine、Compose、OpenSSL，并将以下文件放入 `server/deploy/tls/`：

| 文件 | 要求 |
| --- | --- |
| ca.crt | 客户签发 CA 信任链；不含 CA 私钥 |
| server.crt / server.key | 服务 HTTPS 证书/私钥，SAN 包含客户访问域名/IP |
| broker.crt / broker.key | MQTT Broker 证书/私钥，SAN 至少包含 Docker 服务名 `broker`，以及网关实际访问的域名/IP |

`server.crt` 被 manager 和 pipeline 使用。私钥只有对应服务 UID 可读。默认 MQTT 采用 TLS + 每用户密码，HTTP 可以额外使用客户证书。安装器验证证书链和 Broker 内部域名，公网/客户访问域名仍需按现场地址验证。

```bash
cd server
cp deploy/.env.example deploy/.env
# 编辑 GW_ALLOWED_TARGET_HOSTS 为明确允许转发的客户主机。
sudo bash scripts/install-linux.sh --build
docker compose -f deploy/compose.yaml ps
```

首次安装生成随机管理员密码、Broker 服务密码；只保存在 `deploy/secrets/`。后续安装不会重置已有秘密。默认管理端口只绑定 `127.0.0.1:8443`，网关 HTTPS 入口为 `8444`，MQTT 为 `8883`；按客户防火墙限制来源。pipeline 的公开入口只承担原始 batches 和健康检查，管理/命令/结果/状态在 manager。单独测试 HTTP 固件应支持管理与数据两个地址，或使用统一的 MQTT 通道。

首次注册 MQTT 网关需要完成两处操作：平台登记设备 verifier，Broker 通过 `mosquitto_passwd` 建立同名用户。用户名必须等于 gatewayId；使用相同的设备 token，通过交互输入导入，不在 shell 命令行写密码。参考 ACL 已按 `%u` 限制到单个设备 topic，禁止给设备 platform 服务用户。每次修改 Broker 用户文件后重新加载配置，确认新凭据可连接、旧凭据已撤销。改变 namespace 时必须同步 ACL 的 `ns_local` 路径；不能仅修改某一进程的环境变量。

Compose 的 init 使用 `service_completed_successfully` 等待初始化完成，两个业务进程随后启动；启动顺序不代替健康检查。参见 [Docker Compose 启动顺序](https://docs.docker.com/compose/how-tos/startup-order/)。基础构建镜像版本在 Dockerfile 中固定，离线包记录实际镜像 ID/digest；[Go 官方镜像标签](https://hub.docker.com/_/golang) 是更新镜像的来源。

## 环境变量

| 变量 | 默认/用途 |
| --- | --- |
| GW_DATA_DIR | `data`，必须持久化且仅授权运行用户可写 |
| GW_LISTEN | `127.0.0.1:8080` |
| GW_ROLE | `all`；生产拆为 manager/pipeline |
| GW_NAMESPACE | `ns_local`；初始化后固定 |
| GW_NODE_ID | `data_local`；单机 manager/pipeline 指向同一数据节点 |
| GW_TLS_CERT / GW_TLS_KEY / GW_CLIENT_CA | HTTPS 证书、私钥、可选设备 CA |
| GW_ALLOW_INSECURE_DEV | 仅显式 `true` 允许开发；主程序 HTTP 仍限制回环 IP |
| GW_BROKER_URL / GW_BROKER_USER | 网关 Broker 地址及平台服务账号 |
| GW_BROKER_PASSWORD_FILE | 主程序从受保护文件读取 Broker 密码；避免放入进程命令行 |
| GW_BROKER_CA | Broker CA PEM；不跳过证书验证 |
| GW_ALLOWED_TARGET_HOSTS | 逗号分隔客户转发主机白名单，禁止配置任意 URL 当代理 |
| GW_MAX_QUEUE_BYTES | 默认 1 GiB，原始数据/outbox 接管额度；实际磁盘还需额外空间 |
| GW_ARTIFACT_PUBLIC_KEYS | 可选 JSON `{keyId:base64Ed25519PublicKey}`；也可使用 `data/trust/keys.json` |

## 密钥与制品签名

```bash
bin/gatewayctl keygen --key-id vendor-1 --out signing/private.key --public-out signing/trust.json
bin/gatewayctl sign --manifest release/manifest.json --file release/firmware.bin --private-key signing/private.key --out release/signature.txt
```

签名工具验证 manifest 的 sha256 与文件一致后，对 manifest 原始字节签名。私钥只保留在发行机，客户服务器仅安装审查过的公钥。`master.key` 是本实例加密凭据所需的密钥，丢失后不能重建现有加密秘密；它随加密备份保存，不能从备份中单独遗漏。

## 加密停机备份与恢复

停止 manager 和 pipeline，Broker 可以暂时保留设备会话。每个服务进程持有操作系统共享锁，维护工具取得排他锁；有进程仍使用数据目录时备份/恢复会明确失败。进程崩溃会由操作系统释放锁，不需要删除 lock 文件。

```bash
docker compose -f deploy/compose.yaml stop manager pipeline
# 原生安装示例；Compose 可使用同版本gatewayctl容器，挂载相同data及独立backup目录。
bin/gatewayctl backup --data-dir data --out /secure-backup/backup-001.bgb --key-file /separate-key-store/backup.key
bin/gatewayctl restore --data-dir restored --archive /secure-backup/backup-001.bgb --key-file /separate-key-store/backup.key
```

归档包含两个数据库、WAL、实例密钥、凭据加密文件、制品与运行配置；使用 tar/gzip 后分块 AES-256-GCM 加密。独立的 32 字节备份密钥若不存在会自动生成，必须与备份分开保管。恢复验证每一帧、完整结束标记和归档路径，拒绝路径穿越、符号链接、特殊文件、损坏或截断归档。只能恢复到空目录，不能覆盖现有运行目录。

恢复会先持久化 `restore-reconciliation.required`，然后安装已验证的数据库文件。在有客户下游的系统中，恢复旧备份后直接递增本地 revision 可能小于客户已经处理的 revision；因此重新解析保持锁定。由**所有客户接收端**导出各源事件最高 processingRevision，取最大值合并：

```json
{
  "namespaceId":"ns_local",
  "complete":true,
  "revisions":[
    {"gatewayId":"gw_demo","eventId":"boot_01-1","processingRevision":7}
  ]
}
```

```bash
bin/gatewayctl reconcile-revisions --data-dir restored --namespace ns_local --file merged-customer-revisions.json --confirm-complete
```

该命令必须停机执行。它校验身份/正整数/唯一项并同步落盘 `revision-highwater.json`，之后才解除恢复标记。`complete:true` 是操作员对所有客户接收端已经参加对账的明确确认；工具无法替用户证明没有遗漏。不要猜数字、用空数组代替缺失客户资料或手工删除标记。历史 outbox 仍按稳定 deliveryId 恢复投递，客户继续幂等处理。

## 升级、观测与恢复验收

当前数据库通过 schema 版本与校验和防止不匹配程序静默修改结构。升级必须停止服务、先加密备份，验证发布签名和镜像身份，在测试恢复副本验证兼容性后再替换程序/镜像。管理 API 的在线维护请求不能伪装已经完成跨库备份、恢复或程序替换；需要停机的动作返回明确状态。首版没有自动跨版本迁移、集群滚动升级或自动回滚编排。

健康探针用于进程/数据库就绪；系统状态与指标用于队列字节、最旧待投递年龄、磁盘余量、任务结果与解析隔离。客户不可用时应观察 outbox 积压增长和重试，恢复后观察清空速度；不要仅以网关在线数量判定业务链路正常。

验收至少包含：错误设备令牌拒绝、namespace 越权拒绝、重复事件、内容冲突、丢原始 ACK、丢结果 ACK、客户提交后丢 ACK、网关/服务进程重启、命令过期/未知结果、规则版本切换、解析异常隔离、客户长时间断开、磁盘额度耗尽、备份损坏拒绝、恢复 revision 对账。真实硬件验收另外覆盖 BLE 丢包边界、Flash 断电恢复、实际配置/重启、固件签名与回滚。仓库测试和模拟器不会代替这些硬件证据。
