# Peripheral 业务桥接

本组件实现 [贯通开发合同 C05](../../../../docs/gateway-alignment-development-plan.md#6-c05peripheral双向业务)。nRF 的 INFO/RX/TX 与 UART `0x22/0x0D/0x0E/0x0F` 保持原格式；ESP 本机 BLE 使用 `gw_ble_peripheral.h`。手机业务不会进入网络配置或 OTA 管理处理器。

## 接入

- 初始化：`gw_peripheral_init()`，在 core、capture、UART、actions 与制造授权初始化后调用。
- 平台上行：`gw_peripheral_service(namespaceId,gatewayId,interface,deadline)`。调用统一 `gw_transport_json`；指定接口仍由统一传输绑定 HTTPS。该调用不占用认证 worker 的锁等待 HTTP。
- 任务：`gw_peripheral_execute()` 处理 `PERIPHERAL_NOTIFY` / `PERIPHERAL_DISCONNECT`；`gw_peripheral_recover()` 只读持久结果，不重复发送不确定的业务。
- 诊断：`gw_peripheral_diagnostics()` 返回业务 spool、业务账本、任务证据与 SESSION_LOST/拒绝计数，不返回凭据或会话密钥。

制造清单必须授权 `PERIPHERAL_BRIDGE_1`，相应 radio 必须通过 `NRF54_BLE_PERIPHERAL` / `ESP32_BLE_PERIPHERAL` 的实板授权门控。缺少业务能力或业务 ACL 时初始化返回成功，业务服务保持关闭，以允许原网关功能正常启动。任务明确返回不支持；不能把 capability 当作客户端身份。

## 独立业务凭据

在既有加密 factory NVS 中写入字符串 `periph_acl`，格式如下。最多 8 个客户端；`username` 是 Security2 的 SRP 用户名，`clientId` 为平台登记的业务客户端 ID，`routes` 为离线预授权白名单。

```json
{"clients":[{"clientId":"phone-001","username":"business-phone-001","slot":0,"enabled":true,"routes":["telemetry"]}]}
```

每个 slot 使用独立 blob：`periph0_salt` 为 16 字节，`periph0_ver` 为 384 字节；其他 slot 按 `periph1_salt` / `periph1_ver` 等命名。salt/verifier 必须按 ESP-IDF v5.5.6 Security2 生成。此处不会读取 `setup_user`、`sec2_salt`、`sec2_verifier`，也不会把手机业务授予现场管理员权限。

平台须另外登记同一 clientId 及 route 权限；factory ACL 不是平台权限的替代。平台通过已验证的上行业务事件建立当前客户端关联。未配置、未知或未授权的客户端无法建立业务会话。

## PG 分片及加密

固定 12 字节头：`"PG", version=1, type, transportSeq:u32LE, totalLength:u16LE, offset:u16LE`，后接非空片段。type 0 为 Security2 握手 protobuf，type 1 为受认证业务信封，type 2 为受认证业务应答。4096 字节总长度；单片包括头不超过 `min(512, MTU-3)`，MTU23 可装 8 字节内容。

每连接两个方向各自从 sequence 1 递增，最近完全相同片段可重试，同序号不同内容拒绝。使用捕获/持久接管时刻开始的 5 秒固定预算，分片不续期。两路射频分别持有会话；会话最长 10 分钟，失效后关闭连接并重新认证。worker 处理有界队列，BLE 回调仅复制数据。不会在回调中等待认证或 Flash。

加密直接调用锁定版本 `protocomm_security2` 的公开安全接口，包括 SRP6a 握手、加密与解密；不实现另一套密码算法。该接口允许主动发送下行业务。重发缓存的输出必须保留原密文及原传输序号，不再次解密或重复使用新生成的会话状态。

## 持久接管与平台 ACK

nRF 原片先进入 capture 的独立未认证 INBOX。读取时携带 controller boot、ESP ingress boot、真实连接号、到达单调时刻和可信映射时间。旧 ESP boot 的密文或已释放连接的数据标记 `SESSION_LOST`，不能重建已失效的密码会话。

每个处理结果与最后 INBOX receipt token 在 `peripheral/state` 的同一持久快照提交；业务事件和去重摘要同事务写入后，才释放原片并向手机发 `STORED`。重启或 ACK 丢失可根据 receipt token 完成 INBOX 回收。手机没有收到 `STORED` 时须重新认证，以原 businessId 和内容重传。

JSON 对象的全部必填字段与可选字段必须先完整构建，再加入持久快照。分配失败时不产生残缺事件、去重记录或成功回执；receipt 元数据构建失败会回退到已保存快照并保留原片。任务意图、原始参数和 UART 序号预留同样必须完整保存后才能执行物理发送。

去重摘要包含 clientId、方向、businessId、route、payloadBase64、inReplyTo，排除传输序号、boot、连接及接收时间。同 ID 同业务内容不会新增上行事件；异内容拒绝。已持久事件的 eventId、batchId 和元数据保持稳定。可信接收时间有有效映射时为 SYNCED，否则为 UNSYNCED 并省略 receivedAt。

上行使用 `/api/v1/gateway-peripheral/batches`，每次一个稳定事件批次，仍满足最多16条的合同。完整核验身份、batchId、eventId、durability 与逐项状态：仅 ACCEPTED/DUPLICATE 且不带 code 时回收；合法 RETRY 保留；合法 REJECTED 将原事件和原因在同一快照移至隔离。401/403、错误或缺失 ACK、未知状态和错误 code 均不回收。

## 下行与故障边界

NOTIFY 在任何发送之前持久化完整发送意图和本方向 transportSeq。UART TX 受理、ATT 通知成功和手机 STORED 均不是业务成功；仅同一身份、连接与 businessId 的加密 APPLIED 应答产生 VERIFIED。REJECTED 为明确失败；断链、密码会话丢失、超时或重启时的送达不确定性保留 UNKNOWN。恢复入口不会重新投递副作用。

DISCONNECT 以旧连接实际释放为完成证据，`CLOSING` 不算完成。新 controller boot 不能拿来证明旧会话正常关闭。

当前有界资源：每 radio 一个实际 Peripheral 会话（驱动基线）、ESP 回调队列16项、nRF未认证 INBOX32项、业务 spool8项、持久业务去重账本64项、任务证据16项。总 Flash 快照也受现有 store 上限限制。满时拒绝新工作并保留已有证据；不静默驱逐去重账本或待确认业务。隔离数据同样占配额，需经受授权维护流程处理。

## 验证

`tests/run_host.py --cc <compiler> --idf <ESP-IDF v5.5.6 path>` 编译真实 `gw_peripheral.c` 和 `gw_pg.c`，覆盖多会话、MTU23、5秒截止、乱序/重放/同片重试、持久 STORED、相同业务重连去重、冲突、Flash失败、spool/输入队列满、平台ACK丢失/错误ACK、隔离以及 APPLIED/CLOSED 完成证据。

另对 cJSON 的 861 个分配位置逐点注入失败：业务构建69处、接收/持久/手机STORED187处、NOTIFY248处、DISCONNECT195处、上行构建49处、手机APPLIED记录62处、任务结果22处、UART序号预留29处。验证缺少完整持久意图时不执行任务，未完整保存业务时不发送STORED，回执证据不完整时不返回成功，未预留持久序号时不发送。存储替身同样先完整复制再发布，避免测试把失败写入误当作已有快照丢失。

host 测试使用显式的 SDK 密码、Flash、传输及设备替身，不证明密码算法或空口时序。目标 SDK 编译、手机真实 Security2 握手、双射频连接、掉电、Flash寿命和最大负载仍需按整机验证报告记录；不能仅凭 host 结果开启实板授权。
