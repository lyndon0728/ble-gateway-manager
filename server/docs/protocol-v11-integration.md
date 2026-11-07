# GW32 协议 1.1 服务器联调

本实现以用户提供的 [ESP—服务器协议](gateway-protocol.md) 和 [板内 UART 协议](gateway-uart-protocol.md) 为输入，服务器软件版本 `0.2.0`、API 契约 `1.8.0`。本文记录软件对接方式，不替换两份协议，也不把其中标为设计稿的硬件能力写成实机验收结论。

## 边界

服务器负责 1.0/1.1 数据接管、capture 校验与持久化、UUID 解析、授权规则与连接任务、执行结果、客户投递。ESP 负责板内 UART 去重、持久接管、双路一对一合并和最终 eventId 分配。模拟器只产生**已经由 ESP 定稿的事件样本**；`observedBy=["NRF54","ESP32"]` 表示测试数据的预设来源，不表示服务器实现了该硬件去重算法。

UART COBS/CRC32C 的示例向量只用于检查文档编码一致性。服务器不直接打开 ESP/nRF 串口、不替 nRF 发送 BLE_ACK、不模拟 RTS/CTS、电气时序、Flash 掉电原子性或真实射频共存。实际固件仍须在板上完成协议规定的断电、流控、丢包、连接及回滚测试。

## 两种模拟 profile

默认 `reference-1` 保留原有 1.0 温湿度参考帧行为。新增 profile 需要显式注册模拟型号和启动选项：

```powershell
bin/gatewayctl.exe provision-gateway --data-dir data --gateway gw32_sim --token-file secrets/gw32-token --profile gw32-profile-1
bin/gateway-simulator.exe --profile gw32-profile-1 --gateway gw32_sim --token-file secrets/gw32-token --db sim-data/gw32.db --server http://127.0.0.1:8080 --count 1 --duration 60s
```

本地 profile 注册型号为 `GW32I33V2`，硬件版本明确标记 `SIMULATION_ONLY`，授予受控的 GW32 配置、采集上下文和 GATT 模拟能力。它是开发工具，不是量产设备信任入口；实际出货仍通过签名设备清单授权经过验证的能力。

在 GW32 profile 中，`--count 1` 生成一组两个事件：

| 事件 | 内容 |
| --- | --- |
| ADV | NRF54 为主来源，observedBy 为双射频，PUBLIC 地址 `AABBCCDDEEFF`，可连接，AD 为 Flags，不带目标 UUID |
| SCAN_RSP | ESP32 单路观测，同地址，采集时间相差 100 ms，含 128 位服务 UUID `12345678-1234-5678-9abc-def012345678` |

这组输入用于验证“UUID 只在扫描响应出现”的服务器关联路径。它不是 BLE 地址注册；自动连接规则仍必须显式授权这个已登记对端、网关范围和服务 UUID。固定时间向量位于 `cmd/internal/simkit/testdata/gw32-protocol-v11.json`，重放固定旧时间应触发新鲜度拦截；需要成功自动连接时由运行中的模拟器生成当前时间观测。

## 持久化与兼容

每条模拟事件在本地 spool 保存独立的 `schema_version`。切换 profile 只改变新生成事件；已有 1.0 或 1.1 事件保留 eventId、载荷、capture、采集时间和版本，发送时按版本分批。1.1 ACK 也必须匹配原批次版本。原有数据库自动增加此字段，旧事件默认为 1.0。

命令和结果信封仍为 1.0；仅数据 IngressBatch/IngressAck 使用 1.1 扩展。命令 journal 继续按 taskId/requestDigest 去重，结果以稳定 resultId/resultSequence 重试。`--ignore-acks N` 丢弃原始持久 ACK；`--ignore-result-acks N` 丢弃执行结果应用 ACK；`--offline 5s` 表示先持久生成事件再延后联网。恢复已有 spool 时使用 `--count 0`。

## GATT 模拟闭环

支持 CONNECT、SUBSCRIBE/UNSUBSCRIBE、READ/WRITE、DISCONNECT 的模拟结果。CONNECT 使用协议规定的 radio、16 字节 controllerBootId、4 字节小端连接号派生 32 位小写十六进制 connectionRef。相同 CONNECT 任务重发不会创建第二条连接。

SUBSCRIBE 默认原子保存订阅、成功结果和一条合成通知。`--notifications 0..100` 调整每次新订阅生成的通知数；NOTIFY 对应 GATT_NOTIFY，INDICATE 对应 GATT_INDICATE。载荷仅为合成特征值，capture 保存接收时的 connectionRef 和句柄。同步读取通过任务结果返回，不混入通知事件。

DISCONNECT 将连接和订阅标记失效，并持久排入 `GATT_LINK_DOWN/DISCONNECTED`。可用 `--link-down-after 10s` 在本次运行 10 秒后注入一次异步断链。GW32 模拟器重启时保留已有通知和结果，关闭已登记旧连接并排入 `ESP_RESTART` 事件；旧引用后续调用返回 STALE_CONNECTION。执行意图已落盘但结果无法证明的动作继续返回 UNKNOWN，不因重启自动重复物理副作用。

自动连接任务的 `autoTrigger` 另外核对本地保留的 ADV 证据、对端身份、30 秒保留上限和任务 maxAgeMs。证据独立于原始上报 ACK 回收；无证据、过期、非 NRF54 或对端不符返回 STALE_OBSERVATION。模拟器只模拟所声明测试服务及特征的发现，不证明真实设备提供这些服务。

## 自动连接规则 API 示例

以下 PowerShell 示例使用已受客户 CA 信任的 HTTPS 服务器；本机验证可使用显式回环 HTTP。使用具备 `gatt:manage` 和相应网关 `gatt:execute` 范围的账号。先读实际 gateway.groupId，避免假定安装中的分组 ID。

```powershell
$base = 'https://gateway.example:8443'
$session = [Microsoft.PowerShell.Commands.WebRequestSession]::new()
$login = Invoke-RestMethod -Method Post -Uri "$base/api/v1/auth/sessions" -WebSession $session -ContentType 'application/json' -Body (@{
    name='admin'; password=(Get-Content 'secrets/admin-password' -Raw).Trim()
} | ConvertTo-Json -Compress)

function Gateway-API($method, $path, $body=$null, $etag=$null) {
    $headers=@{'X-CSRF-Token'=$login.csrfToken; 'Idempotency-Key'=[Guid]::NewGuid().ToString('N')}
    if($etag){$headers['If-Match']=$etag}
    $args=@{Method=$method; Uri="$base$path"; WebSession=$session; Headers=$headers}
    if($null -ne $body){$args.ContentType='application/json';$args.Body=ConvertTo-Json -InputObject $body -Depth 15 -Compress}
    Invoke-RestMethod @args
}

$gateway = Gateway-API GET '/api/v1/gateways/gw32_sim'
$peer = Gateway-API POST '/api/v1/ble-peers' @{
    scopeId=$gateway.groupId; displayName='registered test peer'
    deviceAddress='AABBCCDDEEFF'; addressType='PUBLIC'
}
$rule = Gateway-API POST '/api/v1/auto-connect-rules' @{
    name='custom service in scan response'; peerId=$peer.peerId
    gatewayIds=@('gw32_sim'); enabled=$true; priority=100
    serviceUuid='12345678-1234-5678-9abc-def012345678'; wireBits=128
    characteristicUuid='12345678-1234-5678-9abc-def012345679'
    security='UNENCRYPTED'; maxAgeMs=5000; cooldownMs=30000
}
$rules = Gateway-API GET '/api/v1/auto-connect-rules'
$connections = Gateway-API GET '/api/v1/gatt-connections?gatewayId=gw32_sim'

# 修改时使用刚读取的 revision，避免覆盖其他管理员的更新。
$rule = Gateway-API PATCH "/api/v1/auto-connect-rules/$($rule.ruleId)" @{
    enabled=$false
} ('"{0}"' -f $rule.revision)
```

创建接口使用 Idempotency-Key：因网络超时重试同一请求时复用原键和原请求内容，示例 helper 的新键仅用于新的操作。PATCH 使用 If-Match，发生 412 时重新读取再决定修改。规则停用使用 `enabled=false`，保留版本和审计，不通过删除历史记录消除正在执行的任务。对端也可通过 `PATCH /api/v1/ble-peers/{peerId}` 修改显示名或状态，使用对端自己的 revision。管理页面的自动连接导航提供对端、规则和连接状态的读取入口；规则写入以这些 API 为准。

自动任务只有在注册对端、网关授权、GW32 能力、可连接的新鲜 NRF ADV 和有效 UUID 证据全部匹配时创建。上例只指定目标规则，不通过手动创建 GATT_CONNECT 冒充自动发现。`scripts/e2e-v11.ps1` 会从无 UUID 的 ADV 加同址 ESP SCAN_RSP 触发自动任务，断言重发不创建第二个 CONNECT，再完成订阅、通知、关闭和网络配置。

## 网络候选配置模拟

为保持 1.0 API 兼容，`GET /api/v1/gateways/{gatewayId}/configuration` 的 `desired` 与 `reported` 仍表示**最近一次任务的原始配置 patch**，不是所有网络、无线组和上传参数的完整运行快照。服务器验证部分修改时内部合并已验证历史与出厂默认；真实设备的完整实际状态通过 DIAGNOSTICS 或执行结果 `details.effective` 获取。不能把最后一条 patch 中未出现的接口理解为已关闭，也不能用缺失字段覆盖其他组。模拟器的 DIAGNOSTICS 返回其持久配置快照并始终标记 simulated。

network CONFIGURE 先持久候选配置与试用意图，为各列入 uplinkOrder 的接口生成稳定 EXECUTING 结果；`details.networkProbe` 含 interface、candidateRevision 和 link/ip/dns/tlsReady。只有这些结果收到身份匹配、逐项 ACCEPTED/DUPLICATE 的 RESULT_PERSISTED ACK 后，模拟器才在同一事务中提交候选和 SUCCEEDED/VERIFIED 结果。

缺少任一 probe ACK 时，最后可用配置保持不变。超时或提交前重启将候选回滚并保留失败结果；提交后重启保留新配置。任务仅含 Wi-Fi credentialRef，不携带密码。模拟器没有真实以太网或 Wi-Fi 驱动，也没有将 TLS 连接绑定到物理候选接口；ready 字段均为模拟证据。真实固件需要完成两份协议要求的接口绑定、应用 ACK 探测与断电恢复。

## 验证入口与限制

```powershell
scripts/build.ps1 -Test
scripts/e2e.ps1
# 新扩展的实际进程联调：
scripts/e2e-v11.ps1
```

单元测试覆盖混合版本 spool、ACK 版本校验、连接引用向量、CONNECT 幂等、通知持久化、重启失效、网络全部 probe ACK 门槛和候选回滚。端到端报告以脚本本次实际执行结果为准；不能仅凭脚本存在宣称通过。现有 1.0 流程必须与新增 1.1 流程共同验收。

服务器仍使用客户服务器上的本地数据库与独立 outbox；新增 BLE metadata、连接和自动规则不会扩大已验证设备容量。生产 MQTT/TLS、容器部署、嵌入式固件和真实无线链路分别执行其验收，不纳入纯软件模拟结论。

量产签名能力中，UART HELLO 的 service-find bit 9 对应 `NRF54_BLE_GATT_SERVICE_DISCOVERY`。服务器自动连接还要求 `GW32_CONTROL_PROFILE_1`、`GW32_CAPTURE_CONTEXT_1`、`NRF54_BLE_GATT_CENTRAL` 和 `GATT_CONNECT`，不得仅根据型号推断这些能力已经实现。
