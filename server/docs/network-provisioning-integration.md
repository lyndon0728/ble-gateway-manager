# 本地配网状态、远程凭据与网络对账接口

日期：2026-11-19。本文描述服务器新增的实际实现；HTTP/SQLite与前端静态验证不代替ESP实板配网验收。产品流程见[配网设计](../../docs/esp32s3-network-provisioning.md)。新接口和配置前置条件均已加入[消息Schema](../internal/platform/assets/message-contracts.schema.json)及[OpenAPI](../internal/platform/assets/openapi.json)。

## 1. 部署及权限

启动时自动应用不可变数据库迁移v4，新增网络状态、凭据、probe与裁决记录，不修改v1～v3迁移。备份/恢复必须使用支持v4的程序；旧程序拒绝打开较新数据库。

远程安装密码需要环境变量`GW_NETWORK_CREDENTIAL_KEY`：标准Base64编码的32字节随机AES密钥，由部署秘密管理设施注入，不提交仓库、日志或普通配置。未配置或格式不正确时秘密创建/领取返回503，其他网络状态与配置功能仍可使用。服务器采用AES-256-GCM，AAD绑定gateway/credentialRef/deliveryId；不是设备独占解密的端到端密封包。平台秘密服务属于受信任处理者。

密钥须独立备份。当前没有在线换钥API；不能在仍有PENDING投递时直接换环境密钥。安装终态回执后删除服务器侧密文实体并保留不可变安装元数据，重发相同回执幂等；设备丢失已安装密码时需重新安全安装新凭据，不复用旧引用伪称仍可用。凭据元数据每设备最多128项，当前无自动删除接口；达到配额后拒绝新增，不能为腾空间删除正在使用/回退引用。

生产端点要求TLS，不信任任意X-Forwarded-Proto。只有明确`GW_ALLOW_INSECURE_DEV=true`的隔离开发运行可使用HTTP。所有响应设置`Cache-Control: no-store`；反向代理也不得记录秘密正文。

| 操作 | 权限 |
| --- | --- |
| 安装新密码 | `gateway:credentials`及目标网关范围；ADMIN/DEVICE_ADMIN有该权限，OPERATOR无 |
| 查询脱敏网络/凭据 | `gateway:read` |
| 创建网络裁决任务 | `gateway:configure` + `task:create`；取消未下发任务另需`task:cancel` |
| 设备接口 | 既有设备Bearer凭据或mTLS；从认证身份确定gateway/namespace，不能指定其他设备领取秘密 |

## 2. 设备接口

### POST /api/v1/gateway-network-probes

请求字段：`transactionId`（1～80字节标识）、`interface=ETHERNET/WIFI_STA`、`snapshotId`（64位小写十六进制SHA-256）、`nonce`（32～64位小写十六进制随机值）。响应回显这四项并增加`receiptId/gatewayId/namespaceId`，200仅在数据库提交后返回。

同设备同nonce同内容返回同receiptId，不同内容409；24小时保留最多2048个nonce/设备，达到上限429。ESP必须在候选接口的独立TLS连接上校验响应、身份、nonce、快照和时限。服务器无法仅凭interface字符串独立证明物理路由。

### PUT /api/v1/gateway-network-state

必填字段：

| 字段 | 约束 |
| --- | --- |
| networkGeneration / localRevision | 规范十进制字符串，范围0～uint64最大值，不使用JSON浮点计数 |
| desiredLocalNetwork / actualNetwork | 完整network对象，含uplinkOrder、ethernet、wifiSta；目标与实际独立 |
| activeInterface | ETHERNET / WIFI_STA / NONE |
| localOverride | boolean |
| verificationState | UNCONFIGURED / LOCAL_ONLY / PLATFORM_CONNECTED / PLATFORM_SYNCED / ROLLED_BACK |
| lastKnownPlatformRevision | 0～9007199254740991整数；是已知平台修订，不是本地版本 |

可选`credentials`：最多128条本地安装元数据，字段为`credentialRef/version/ssid/security/source=LOCAL/state=INSTALLED`，不包含密码或deliveryId。引用内容不可变且仅属于该设备；支持本地生成凭据的ADOPT_LOCAL，不制造云端投递回执。

服务端计算并返回`snapshotId/actualSnapshotId/stateVersion/syncToken`。摘要为network对象的RFC8785兼容子集：固定ASCII属性名、有限整数、正确保留Unicode字符串，不包括密码。相同generation不允许改变保存目标或localRevision；世代/本地计数倒退拒绝。实际接口故障回退可在同generation报告新的actualNetwork。

清除已知localOverride时额外提供`appliedTaskId`，要求该设备对应CONFIGURE已经SUCCEEDED且前置条件绑定旧快照与generation，新generation恰好为其+1。**先交付真实成功结果并收到ACK，再上报清除override的状态。** 迟到的旧任务不能清掉较新本地改动。

### GET /api/v1/gateway-network-credentials/pending

返回`{"items":[...]}`，最多8项，每项`deliveryId/credentialRef/version/ssid/security/password`。这是唯一向对应设备返回密码的端点，严格TLS设备认证，不进入普通任务或结果消息。领取不等于安装，未收到终态回执可重复领取。

### POST /api/v1/gateway-network-credentials/receipts

请求`deliveryId/credentialRef/version/state`，state为INSTALLED或FAILED。INSTALLED须在设备加密存储完成持久提交后发送；响应`status=ACCEPTED/deliveryId/state`。重复相同终态接受，冲突终态409，其他设备的投递404。

## 3. 管理接口和配置任务

`POST /api/v1/gateways/{gatewayId}/network-credentials`：提交`ssid/security/password`和Idempotency-Key；201返回`credentialRef/version/deliveryId/state=PENDING`。SSID最多32UTF-8字节，口令8～63字节；WPA2_PSK/WPA3_SAE。普通任务、审计和幂等响应不保存密码；秘密请求指纹采用HMAC。幂等重试相同内容返回同一引用。

`GET /api/v1/gateways/{gatewayId}/network-credentials`返回脱敏items及安装状态，不返回密码。新接口创建的引用在安装确认前不能用于远程网络任务；上报过网络状态的设备还要求已登记且SSID/security匹配的INSTALLED凭据。既有尚未上报状态的设备仍兼容原来的受控预置credentialRef流程。

`GET /api/v1/gateways/{gatewayId}/network-state`返回设备最新状态和ETag；没有报告时404。平台配置页展示保存目标、实际配置、当前接口和本地覆盖状态，并可安全安装密码、选择已安装凭据及发起裁决。

`POST /api/v1/gateways/{gatewayId}/network-reconciliation`要求If-Match及Idempotency-Key，字段：

```json
{
  "action": "ADOPT_LOCAL",
  "expectedNetworkGeneration": "12",
  "snapshotId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
  "expiresAt": "2026-11-19T12:10:00Z",
  "cancelQueued": true
}
```

上例是字段示意，执行时使用实时未来expiresAt及真实摘要/ETag。APPLY_PLATFORM另需完整`network`；ADOPT_LOCAL采用desiredLocalNetwork。cancelQueued只取消同网关尚未尝试下发的网络任务；已发送、执行中或UNKNOWN继续占用，返回TASK_IN_PROGRESS，不删除不确定证据。

成功202返回正常Task；实际Command.parameters增加：

```json
{
  "networkPrecondition": {
    "expectedNetworkGeneration": "12",
    "snapshotId": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    "resolutionToken": "netresolve_server_generated"
  }
}
```

snapshotId是裁决时本地保存目标的比较基线。resolutionToken由服务器生成并绑定目标设备、世代、基线摘要、候选网络摘要及唯一任务，不可复用于其他任务。普通网络配置可携带前两个字段；服务端在已有设备状态时为没有显式条件的旧管理请求补入当前基线。ESP仍须在执行前以本地状态再次校验，不能只依赖创建时校验。

HTTP轮询和MQTT分发都拦截与已报告本地版本冲突的旧网络任务。裁决任务验证候选uplinkOrder全部接口；每个probe必须在先前服务器事务持久化，随后才能接纳SUCCEEDED/VERIFIED。原TaskRequest摘要规则保持：服务端补入的执行参数与desiredRevision不被误当作客户端原请求重新计算摘要。

## 4. 验证与限制

新增Go测试覆盖：静态密文及身份AAD篡改、秘密API权限/TLS/跨设备隔离、凭据安装回执幂等及门槛、探测nonce防冲突、超过JS安全整数的计数、状态倒退、HTTP/MQTT旧任务拦截、仅取消未下发任务、所有接口证明及匹配任务后清除覆盖。前端测试验证秘密请求不会进入普通写请求的待重试指纹缓存。

本次运行完整`go test ./...`及Vue TypeScript检查通过；新增接口主机测试不证明设备Flash提交、真实接口绑定、浏览器热点通信或RF性能。生产TLS、现场身份注入、ESP实板及真实网络断连/掉电仍须按[验收计划](../../docs/esp32s3-validation-plan.md)执行。
