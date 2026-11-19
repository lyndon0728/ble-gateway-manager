# 平台—ESP32-S3—nRF54L15 人工贯通测试

版本：1.0；编写日期：2026-11-19；执行方式：由测试人员在真实部署和实板上执行。**本文件是操作计划，所有用例初始状态为“未执行”，不引用历史软件测试作为本轮通过证据。**

依据：[平台需求](../../server/docs/gateway-platform-requirements.md)、[平台开发文档](../../server/docs/gateway-platform-development.md)、[平台协议](../../server/docs/gateway-protocol.md)、[三方实现记录](../gateway-alignment-implementation.md)、[ESP 验收清单](../esp32s3-requirements.md)、[系统验收清单](../gateway-requirements.md)。精确请求以[OpenAPI](../../server/internal/platform/assets/openapi.json)和[消息 Schema](../../server/internal/platform/assets/message-contracts.schema.json)为准。

## 1. 范围、设备和记录规则

### 1.1 本轮范围

必测覆盖平台部署、双传输、采集与去重、任务与配置、签名时间、独立组件库存、受控快照恢复、权限审计、本地 Wi-Fi 配网、平台远程配置、双组件远程 OTA、ESP 本地 OTA 及容量稳定性。必须连接真实 ESP32-S3/nRF54L15，使用真实 AP、以太网、Broker、证书和 BLE 测试对端。

**手机蓝牙业务范围待用户确认，排除本轮必测。** 对应 `ACC-11`、`ESP-AT-25`、`C05`，相关平台需求是 `PLAT-GW-038/058`。此说明只划定独立业务认证和收发的执行范围，不删除或改写原编号。`ESP-AT-21` 的可连接广播 PAUSED、关闭时保持连接、断开后按目标恢复仍属必测，使用标准 BLE 测试客户端即可，不要求 C05 业务 Security2。手机通过 Wi-Fi 连接热点进行配网和 ESP OTA 仍属必测。

### 1.2 开始前填写的测试基线

| 项目 | 必填实际值 / 准入条件 |
| --- | --- |
| 运行编号 | RUN-ID、操作者、复核者、开始时间、证据目录；不同样机和批次独立编号 |
| 平台 | 实际 Git 提交、服务程序 SHA-256、静态资源版本、角色 all 或 manager/pipeline、实际配置；源码版本标识 0.4.0，OpenAPI 标识 1.11.0，不能仅凭这两个字符串判定代码相同 |
| 数据库 | management.db/pipeline.db 的备份位置；迁移应为 5；数据目录在同一主机本地磁盘，不使用网络共享存储 |
| 固件与硬件 | ESP/nRF 的板修订、SN、gatewayId、制造批次、实际固件版本及运行镜像摘要；当前交付固件基线 0.3.0 |
| SDK/目标产物 | ESP-IDF v5.5.6、NCS v3.2.1 的实际锁定版本、目标构建和签名产物证据；缺真实 nRF SDK/工具链或对应目标镜像时，相关实板测试标“阻塞” |
| 身份 | 专用测试 namespace、两台独立身份样机 A/B；ADMIN、只读账号、限范围 OPERATOR、另一范围账号；凭据不写入报告 |
| 授权 | 原始签名设备清单、设备身份绑定、board_caps、实际 HELLO 和可用能力；不得为测试绕过签名或虚报硬件能力 |
| 网络/时间 | 测试 TLS CA、服务域名、Broker ACL、两套 AP/网段、独立签名时间公钥/私钥配置、真实时间源确认及制造时钟漂移证据 |
| BLE/故障夹具 | 可编程 Beacon/GATT 外设、独立 BLE 抓包设备、UART 观察及受控故障夹具、可分别断 ESP/nRF 电源的夹具、可审计的 ACK 丢弃/延迟装置 |
| 业务接收端 | 可实际持久化并返回客户应用 ACK 的测试服务；记录其 inbox 身份及事务日志，不以模拟器代替硬件链路 |
| 容量预算 | 冻结采集速率、包长、缓存额度、保留期、持续时长、最大任务数、允许恢复时间和资源上下限；缺批准数值时容量/长稳判定阻塞 |

缺少硬件夹具、制造授权、独立时间钥、真实目标 SDK/镜像或可观察证据时，写“阻塞：缺少……”，不得改用模拟器填“通过”。仅 API 负例可由人工工具重放真实设备消息；人工构造内容不能充当当前镜像、连接、Flash 或射频证据。

### 1.3 任务 journal 容量门槛

设备 `gwstore` 最多保存 **64 个任务身份 / 96 KiB**，原防重证据没有自动归档 API。用例切换、每日重启、测试完成均不能假定该容量会复位。

1. 每批开始记录已占任务身份数、字节数和计划新任务数。**本测试批次保守预留**至少 16 个身份、24 KiB 供诊断和恢复；普通执行的“现有占用＋本批新增预算”控制在 48 个身份 / 72 KiB 内，任一先到即停止新增任务。这是测试安排，不是固件新增上限；固件边界仍为 64 个身份 / 96 KiB。
2. 同一幂等键的同请求重试应复用任务身份；新的诊断、配置、恢复、升级及失败准入后实际生成的任务都要计数。HTTP GET 查询不计为设备任务。
3. 长稳采用固定配置持续采集，不每隔几分钟创建诊断任务；每台样机只预算开始、结束等必要任务。以网页/API 只读查询及串口只读记录观察过程。
4. 任务容量极限使用专用样机单轮执行，不能与正常升级/恢复试验共用剩余容量。保留原 journal，不直接删记录、修改数据库或执行未经提供的“归档”命令。
5. 需要新批次时换用已有独立制造身份的测试样机并保留原样机证据；没有可用样机或批准恢复流程则标“阻塞”。不能复制旧身份/凭据到新板伪装同一网关。无法取得可信容量读数或保守预算时，相关容量试验标阻塞。

### 1.4 操作与证据约定

下文 `BASE` 为实际 HTTPS 平台地址，`GW-A/GW-B` 为真实登记身份，`T+10min` 为提交时当前可信 UTC 加 10 分钟的 RFC3339 值。不能照抄过去日期；任务期限最长不超过 7 天。

管理操作使用控制台 `/ui/` 登录会话，或经批准的同权限 API 客户端。Cookie 会话的写请求必须带现有 CSRF 头，沿用控制台实际请求；设备接口使用本设备 Bearer token 或已登记 mTLS。`Idempotency-Key`、ETag 和正文应保存到脱敏证据，Authorization、Cookie、密码、私钥及制造秘密必须删去。不得关闭 TLS 校验来“完成”用例。

故障注入只能对专用测试链路执行。抓取 HTTPS 明文须使用已经批准且受测试设备信任的观测设施，不能把生产根 CA 或密钥替换为临时测试材料。无合法可控设施时，对应步骤阻塞。

每例记录：初始状态、实际步骤和时间、前后配置/版本、完整错误码、taskId/requestDigest/revision、eventId/batchId/resultId、bootId、ACK、设备日志和恢复结果。证据目录建议 `RUN-ID/用例ID/步骤号/`；至少保存脱敏请求响应、平台日志、UART/设备日志、必要的空口记录和图片。无法观察关键证据不能按猜测通过。

## 2. 已核实的人工操作入口

以下为实际入口，不代表每项都有专门按钮。组件库存、完整历史快照、签名时间等按 API 操作，不虚构控制台页面。

| 用途 | 真实入口 |
| --- | --- |
| 部署健康与契约 | `GET /health/live`、`GET /health/ready`、登录后读取 `/openapi.json`、`GET /api/v1/system/status` |
| 网关与配置 | 控制台“网关设备”的详情/配置；`GET /api/v1/gateways/{gatewayId}`；`GET/PUT /api/v1/gateways/{gatewayId}/configuration` |
| 任务证据 | 控制台任务列表/详情；`POST /api/v1/tasks`；`GET /api/v1/tasks/{taskId}`、`/request`、`/results`；取消入口为 `POST /api/v1/tasks/{taskId}/cancellation` |
| 原始事件与客户交付 | 控制台数据事件/详情；`GET /api/v1/events`、`GET /api/v1/gateways/{gatewayId}/events/{eventId}`、对应 `/deliveries` |
| 自动连接 | 控制台蓝牙对端、自动连接规则、连接列表；`/api/v1/ble-peers`、`/api/v1/auto-connect-rules`、`/api/v1/auto-connect-triggers`、`/api/v1/gatt-connections` |
| 设备业务通道 | `/api/v1/gateway-ingress/batches`、`/commands`、`/results`、`/status`，分别为 POST/GET/POST/POST；MQTT topic 见 MAN-E2E-01 |
| 网络 | 网关配置页的网络区域；管理端 `/api/v1/gateways/{gatewayId}/network-state`、`/network-credentials`、`/network-reconciliation`；设备端 `/api/v1/gateway-network-state` 等详见[配网接口](../../server/docs/network-provisioning-integration.md) |
| 组件库存 | 设备 `POST /api/v1/gateway-component-challenges`、`PUT /api/v1/gateway-components`；管理 `GET /api/v1/gateways/{gatewayId}/components` |
| 制品关联 | 设备 `GET /api/v1/gateway-component-artifact?component=GATEWAY或NRF54&sha256=实际摘要` |
| 运行能力 | 设备 `PUT /api/v1/gateway-runtime-capabilities`；管理查询网关的 capabilities/availableCapabilities |
| 快照 | 设备 `GET /api/v1/gateway-configuration-snapshot`；管理 `GET /api/v1/gateways/{gatewayId}/configuration-snapshot`；两者可带 `?snapshotId=历史ID` |
| 时间 | 无设备 token 的 `POST /api/v1/gateway-time-challenges`，响应 Ed25519 签名包 |
| 审计 | 控制台审计页；`GET /api/v1/audit-events` |
| 本地 Wi-Fi 页面 | 手机/笔记本连接真实 SoftAP 后，浏览器访问实际热点默认网关的 HTTP 地址；不是平台域名，也不需要手机 BLE 业务 App |

## 3. 平台专项用例

### MAN-PLAT-01：部署身份、Schema 与数据库迁移

**原映射：** PLAT-GW-AT-01/11；ESP-AT-34；ACC-17。**初始状态：未执行。**

**前置/数据：** 独立测试实例、真实发布程序与 UI、两库停机备份、原 v4 测试副本；有只读 SQLite 检查工具。

1. 记录程序/静态资源 SHA、Git 提交和部署角色，人工按[运维说明](../../server/docs/operations.md)启动本版服务；本例不重新执行自动化测试。
2. 请求 `/health/live` 和 `/health/ready`，登录控制台并读取 `/openapi.json`，记录实际版本及新增组件/快照/时间路径。
3. 用只读工具执行两库 `SELECT version,checksum,app_version FROM schema_migrations ORDER BY version;`，核对最大版本均为 5，v1～v4 checksum 与备份一致。management 中核对组件、runtime、快照历史和 Peripheral 扩展表存在；不因此执行待确认的 C05 测试。
4. 停启服务一次，重读迁移记录和已登记网关，确认无重复迁移和数据重置；对独立 v4 副本按同流程升级并保留升级日志。

**预期/证据：** readiness 通过；部署与契约匹配；v5 追加成功，旧 checksum 不变；独立副本升级后原数据仍可读。只检查文档版本不算通过。

**恢复：** 停止测试实例；保留迁移后库及原备份，不用旧二进制直接打开新库。需回退时只按批准的停机恢复流程恢复完整副本。

### MAN-PLAT-02：网关身份、范围和能力交集

**原映射：** PLAT-GW-AT-01；ESP-AT-16/34；ACC-01/17；C07。**初始状态：未执行。**

**前置/数据：** 独立身份 A/B、真实签名清单、已验证 board_caps，A 具 CONFIGURE/DIAGNOSTICS 与适用 radio 能力；B 位于另一权限范围。

1. 启动 A，记录实际 ESP/nRF boot 与 HELLO；观察设备 PUT runtime 报告和平台响应，再 GET A 的网关详情。
2. 将签名 capabilities、espImplemented、nrfHelloCapabilities、boardVerified 与 availableCapabilities 逐项对照；nRF V2 需要 HELLO bit13，ESP V2 独立授权。
3. 用无权限账号查询 B、尝试为 B 创建任务；用 A 设备凭据向状态接口提交含 B gatewayId 的真实报文副本；记录拒绝码及无新增任务/状态覆盖。
4. 在制造批准的专用样机减少一项 board_caps 后重启，确认相应操作不放行；不得靠人工数据库更新增加能力。
5. 经夹具使 nRF 不可读，观察真实 runtime 的 nrfBootId=null、HELLO 列表为空，确认未伪造 boot；恢复 nRF 后观察新的有效身份与能力。

**预期/证据：** 作用域不越权；能力只能求交集，不因平台升级增加；缺 nRF 身份时旧 nRF 库存不能用于源版本准入。保存三端身份与拒绝响应，不保存秘密。

**恢复：** 恢复原制造批准配置和接线，确认真实 runtime 更新；未提供合法制造修改流程时步骤 4 阻塞。

### MAN-PLAT-03：任务创建幂等、结果重投和过期

**原映射：** PLAT-GW-AT-06；ESP-AT-03/13/15；ACC-02。**初始状态：未执行。**

**前置/数据：** A 在线且时间可信；预算 2～3 个新任务；准备 DIAGNOSTICS 参数 `{"profile":"HEALTH","durationSeconds":1}`，expiresAt=T+10min，固定键 `RUN-ID-diag-1`。

1. POST `/api/v1/tasks`，正文四字段 gatewayId/type/parameters/expiresAt，type=DIAGNOSTICS。记录返回 taskId。
2. 原样重投同一键和正文，核对 taskId/requestDigest 不变；同键修改 durationSeconds，确认冲突且未创建第二个任务。
3. 查询任务 `/request`、`/results`，记录设备产生的稳定 resultId/resultSequence。由夹具丢弃一次结果应用 ACK，确认设备重传相同结果而不重复执行。
4. 提交 expiresAt 早于当前时间的另一请求，确认拒绝且未产生实际动作。
5. 将设备短时断网，创建一个未过期的诊断任务；保持断网超过期限再恢复，确认未执行过期动作。记录服务器到期状态及设备日志。

**预期/证据：** 同意图只一个任务，冲突和过期明确拒绝；HTTP 202 只是受理；稳定结果只有 RESULT_PERSISTED 匹配后出队，不以 PUBACK 替代。

**恢复：** 恢复 ACK 和网络；确认未过期任务终态已接管。UNKNOWN 不删除，转 MAN-PLAT-05 的人工对账。

### MAN-PLAT-04：配置 ETag、修订及单组边界

**原映射：** PLAT-GW-AT-05/06；ESP-AT-15/20/43；ACC-02/09。**初始状态：未执行。**

**前置/数据：** A 无在途任务，已保存原上传组；预算 2 个配置任务及恢复任务。

1. GET `/api/v1/gateways/GW-A/configuration`，保存 ETag、desiredRevision、reportedRevision。
2. PUT 同路径，带原 If-Match、新 Idempotency-Key；正文为 expectedReportedRevision=读取值、configuration=`{"upload":{"batchMaxRecords":32,"flushIntervalMs":1000}}`、expiresAt=T+10min。
3. 查询新任务直到真实结果到达，确认 details.configuration 对应原请求且 SUCCEEDED/VERIFIED 后 reportedRevision 才推进。
4. 用旧 ETag、新键提交另一变更，确认冲突；去掉 If-Match 确认前置条件拒绝。再提交同时修改 network 与 radio 的非法补丁，确认整项拒绝。
5. 读取 `/configuration/effective`，确认它仍是 complete=false 的已确认补丁汇总，不当作完整运行读回；真实恢复另见 MAN-PLAT-08。

**预期/证据：** 原子单组修改、旧页面不能覆盖新目标；失败不推进 revision、不改变其它组。保存前后配置、任务和设备读回。

**恢复：** 重新 GET 新 ETag，用新修订恢复原上传组并验证；不直接回写历史 revision。

### MAN-PLAT-05：副作用不确定与 UNKNOWN 所有权

**原映射：** PLAT-GW-AT-06/07；ESP-AT-15/23/24；ACC-02/08。**初始状态：未执行。**

**前置/数据：** 专用 A 与真实 GATT 测试外设，外设写入计数可读；具备仅在写出后截断链路/结果的夹具；预留诊断身份。没有该夹具则阻塞。

1. 从控制台蓝牙连接页建立获准的 NRF54 连接，记录 connectionRef、boot 和外设写计数。
2. 创建一次带唯一测试值的 GATT_WRITE；在可证实物理写出后、设备不能确定完成之前注入链路故障。
3. 查询任务及结果，核对 UNKNOWN 或有实际证据的确定终态；若不确定，不允许凭超时推断“未执行”。
4. 重投相同命令/幂等请求，读取外设计数，不得产生第二次物理写入。尝试新的普通任务及取消，确认 UNKNOWN 持续占所有权；只读查询和获准 DIAGNOSTICS 可用于对账。

**预期/证据：** 写入可能发生时不盲重做；旧连接引用不污染新连接；取消不是清除 UNKNOWN 的旁路。保存外设计数、断链时点及所有任务历史。

**恢复：** 保留该样机和 UNKNOWN 证据，按批准的人工对账流程处理。没有现成恢复流程则本样机后续写操作阻塞，换独立制造样机；不得清 journal 或手工改任务状态。

### MAN-PLAT-06：签名时间、TLS 与冷启动门槛

**原映射：** PLAT-GW-AT-01/07/09；ESP-AT-11/16/45；C09。**初始状态：未执行。**

**前置/数据：** 独立时间密钥、制造 time_pub/time_key_id、已验证时钟漂移材料、测试 CA/域名；可录制和重放时间响应的批准夹具。缺时间钥或漂移依据则阻塞远程时效测试。

1. 核对服务端 `GW_TIME_SIGNING_KEY`、`GW_TIME_KEY_ID`、`GW_TIME_SOURCE_VERIFIED=true` 已安全配置，记录 keyId 和核时记录，不记录私钥。
2. 冷启动 A，抓取公开 POST `/gateway-time-challenges`：schemaVersion=1.0、32 字节随机 nonce 的规范 Base64、真实本次 bootId；确认不发送设备 token。
3. 记录返回 keyId/payloadBase64/signatureBase64；独立工具验证签名覆盖 ASCII `gw32-time-v1` 紧接 payload 原始 UTF-8，nonce/boot 一致。确认随后真实 TLS 和未过期诊断可进行。
4. 逐项注入错误签名、旧 nonce、旧 boot、超出有效窗口的响应；冷启动未建立可信锚时，不得执行远程 TTL 动作或把采集时间伪造为 SYNCED。
5. 在专用测试服务分别缺签名配置、错误证书主机名、过期证书；前者应拒绝签名，后两者设备 TLS 失败。禁止改客户端为忽略证书错误。
6. 恢复合法时间/证书；确认只有新鲜有效时间证据恢复远程业务。本地离线配网/OTA 不因没有 UTC 被禁用，交叉检查 MAN-E2E-07/09。

**预期/证据：** 错误/旧时间不能续期任务；SNTP 不独立建立信任；TLS 主机名、链和日期均检查。保存签名验证输出、设备拒绝日志及恢复时间线。

**恢复：** 恢复正式测试密钥/证书/时间源配置，移除响应替换规则，重新建立可信锚。

### MAN-PLAT-07：组件库存、挑战重试与真实运行证明

**原映射：** PLAT-GW-AT-09/10；ESP-AT-26/29/30/49/50；ACC-13/15/16；C06。**初始状态：未执行。**

**前置/数据：** 已验签的 GATEWAY/NRF54 制品、两颗芯片真实当前镜像摘要及确认状态；测试工具可记录设备真实盘点流量。

1. 观察设备 POST `/gateway-component-challenges` 和 PUT `/gateway-components`，分别记录组件、challengeId、nonce、inventoryGeneration、controllerBootId 和 evidenceLevel；GET 管理端 `/gateways/GW-A/components` 对照。
2. 对 CONFIRMED_ARTIFACT 逐项核对实际运行 SHA、签名制品 artifactId/版本、bootConfirmed 和当前 boot，不能只比较版本字符串。
3. 丢一次盘点响应，使真实设备重试；或暂停本设备盘点通道后重投已捕获的完全相同请求。收据应保持原 evidenceReceivedAt 和已分配世代，不能因重试刷新新鲜度。
4. 对同一已用挑战修改版本、nonce 或世代作负例，确认冲突；获取新挑战后提交旧未完成挑战，确认旧挑战失效。释放暂停后由设备重新盘点。
5. 阻断库存刷新超过 60 s，选择签名制品中声明非空 fromFirmwareVersions 的 NRF54 固件创建升级任务；应返回 FIRMWARE_COMPONENT_VERSION_UNKNOWN。fromFirmwareVersions 属于制品兼容信息，不另加到任务参数。恢复刷新且镜像被确认后再按真实源版本准入；错误源版本仍拒绝。
6. 完成本地 ESP OTA 后，查询设备只读 `/gateway-component-artifact?component=GATEWAY&sha256=实际摘要`。未导入制品时 404、库存保持 VERSION_ONLY；导入匹配签名制品后由真实盘点提升证据，不能由查询本身提升。
7. 通过受控 nRF 重启/回滚使 boot 改变，确认旧 boot 库存不再满足升级前置条件；新证明来自当前镜像。无 nRF 身份时不填伪造 boot。

**预期/证据：** 两组件不串版本；挑战 CAS、相同重试、过期和签名制品关联有效；受认证的设备证明不是独立硬件远程证明。保存真实镜像测量、收据及任务拒绝码。

**恢复：** 恢复盘点通道，等待真实新挑战/库存；不手工填 CONFIRMED_ARTIFACT。升级步骤使用 MAN-E2E-08/09 的恢复流程。

### MAN-PLAT-08：完整快照、不可变历史和受控单组恢复

**原映射：** PLAT-GW-AT-06/08；ESP-AT-15/18/43；ACC-12；C08。**初始状态：未执行。**

**前置/数据：** 专用样机 A；六组各有真实成功任务基线，安装网络凭据已确认；无 localOverride/UNKNOWN/在途配置。预算首次基线至多 6 个任务，恢复及反向恢复另计。

1. GET `/gateways/GW-A/configuration-snapshot`，核对 network、nrf54Scan、nrf54Advertising、esp32Scan、esp32Advertising、upload 六组及 revision/value/source/verifiedAt。缺组时应 complete=false，不用默认值补成完整。
2. 保存完整快照 S、generation、digest。独立按开发文档的规范 JSON 计算不含 digest 的摘要；重复 GET 未改变内容时 generation/digest 不变。
3. 从 S 选择 upload 完整组作为低影响恢复对象。先通过正常配置改变该组并确认，随后 GET 最新配置 ETag/reportedRevision。
4. PUT 原 configuration 接口，带新幂等键和 If-Match；body 加 sourceSnapshotId=S.snapshotId，configuration 仅包含 S 的 upload.value 全部字段，expiresAt=T+10min。记录新 taskId 和新 revision，确认下载 S 本身没有自动改变设备。
5. 恢复进行中 GET `/gateway-configuration-snapshot?snapshotId=S.snapshotId`，确认返回原完整历史；最新视图可能因在途任务不完整，但历史不被覆盖。成功必须经设备执行及 RESULT_PERSISTED。
6. 依次测试历史值只改一个字段、引用 B 的 snapshotId、缺一组基线、存在 localOverride；应分别拒绝越值/越范围/不完整/绕过本地覆盖。将两个组放入同一恢复请求也应拒绝。
7. 若具备已批准的隔离故障夹具，再测试错误 digest、乱序 generation 和中途断电：设备不得应用损坏候选或旧目标。无夹具标该步骤阻塞，不直接编辑设备存储造“通过”。

**预期/证据：** 候选与执行分离；仅一个历史完整组可用新 revision 恢复；历史来源不可变、当前本地覆盖不能清除。保存 S 原文、摘要校验、前后任务和设备目标。

**恢复：** 用新的正常任务恢复试验前 upload 组，网络覆盖按 MAN-E2E-07 裁决；不批量直接覆盖六组，不降低 revision。

### MAN-PLAT-09：权限撤销、审计与秘密不泄露

**原映射：** PLAT-GW-AT-01/07/08/09；ESP-AT-22/38/42/43；ACC-07/12。**初始状态：未执行。**

**前置/数据：** 两范围账号、专用 Wi-Fi 密码及 credentialRef、已授权自动连接规则、管理员和只读审计会话。

1. 只读账号进入配置/任务页面并直接发同样写请求；确认服务器拒绝，而不是仅隐藏按钮。范围 A 账号直接请求 B 的事件、库存、快照和任务，确认无数据泄露。
2. 管理员经网络配置页安装测试密码，观察 PENDING→INSTALLED；普通任务只含 credentialRef。只读查询凭据、任务原文、诊断、审计和浏览器日志，确认没有密码/设备 token/私钥。
3. 创建获准的自动连接规则后撤销其所有者的相关权限，发送新的真实符合广播，确认不再生成自动连接任务；恢复权限必须显式授权。
4. GET `/api/v1/audit-events` 或控制台审计页，关联本轮配置、任务、凭据和恢复操作的操作者、范围、对象及时间。失败请求保留 API 错误码及服务日志；没有审计项不能伪称已审计。

**预期/证据：** 写权限/范围在服务端生效；撤权后不会由旧规则继续动作；可追踪已提交变更且秘密不出现在普通证据。

**恢复：** 禁用本轮测试规则，恢复批准的用户权限；测试密码按现有凭据生命周期管理，不删除仍被当前/回退目标引用的凭据。

## 4. 真实链路贯通用例

### MAN-E2E-01：HTTPS、MQTT 与 AUTO 切换

**原映射：** PLAT-GW-AT-01/02/06；ESP-AT-13/14/15；C02。**初始状态：未执行。**

**前置/数据：** 真实 TLS Broker 和 ACL、制造批准的 cloud_link/mqtt_url/mqtt_pass 配置；若没有可用制造配置切换流程，相关模式阻塞。预算每模式 1 个诊断任务。

1. 用 HTTPS 模式运行真实采集与诊断，记录 batches/commands/results/status 四通道、事件 ID 和结果 ID。
2. 用已制造 MQTT 模式样机重复同一业务。观察 topic 前缀 `gw/v1/{namespaceId}/{gatewayId}` 及 telemetry、telemetry-ack、commands、command-results、result-ack、status；检查 QoS1 和 retain=false。
3. 以 A 的 Broker 凭据向 B 的 topic 作负例，确认 Broker ACL 拒绝；错误 namespace/envelope 也不得进入平台。
4. 在 AUTO 模式先让 MQTT 可用，再只阻断 Broker，观察 HTTPS 接管；恢复 Broker。切换过程中保留待确认事件/结果，核对重投仍使用原 eventId/resultId/taskId。
5. 如同时测试网络候选，确认设备仍通过绑定候选接口的 HTTPS 试用，不能把旧 MQTT 连接当候选可达证明。

**预期/证据：** 两传输应用语义相同，无双重执行/假 ACK；模式不可用时有真实错误。保留 Broker 日志、ACK 和网关队列前后记录。

**恢复：** 恢复 Broker、ACL 和制造批准模式，排空已确认队列；不清未确认 spool。

### MAN-E2E-02：三层持久 ACK、丢 ACK 与重投去重

**原映射：** PLAT-GW-AT-02/06；ESP-AT-04～08/13/14/35；ACC-02/06。**初始状态：未执行。**

**前置/数据：** 编号可辨的真实 Beacon 序列、已启用持久客户接收端、可分别丢 CUSTODY/平台 ACK/客户 ACK 的夹具，明确 dedup 保留窗口。

1. 采集一组真实广播，记录 UART 接管、ESP eventId、平台原文和客户 deliveryId；用事件详情及 `/deliveries` 关联，不凭总行数推断同一事件。
2. 让平台已持久原文但丢 RAW_PERSISTED 响应；确认 ESP 保留记录并原样重投，平台返回 DUPLICATE，原事件没有第二份物理处理。
3. MQTT 仅放行 PUBACK、阻断 telemetry-ack，确认 ESP 不释放；放行正确身份和逐项 ACCEPTED/DUPLICATE 后才出队。错误 batchId/缺 item 不能释放对应记录。
4. 让客户已持久化但丢客户应用 ACK，确认仅客户投递重试，客户按稳定身份去重，不把网关队列再次视为客户责任。
5. 分别在 CUSTODY 前、已持久但 CUSTODY 前、CUSTODY 后平台 ACK 前及 ACK 后 GC 阶段断电；“仅 ESP 掉电且 nRF 存活”和“整板掉电”分别记录。

**预期/证据：** 未接管数据不提前 ACK；已接管数据按原身份恢复。整板掉电前尚未持久的数据缺口可解释，不能宣称 nRF RAM 能恢复。每层 ACK 的证据和掉电时点独立保存。

**恢复：** 恢复电源和各 ACK，逐项核对已接管集合、隔离集合、未接管缺口；不能把不一致数据删除后重新采集冒充恢复。

### MAN-E2E-03：双路采集、Beacon 掩码和原始事件一致性

**原映射：** PLAT-GW-AT-03/04；ESP-AT-09～11/19；ACC-04/05；BFLT-AT-01～16。**初始状态：未执行。**

**前置/数据：** 已具两目标扫描/V2 能力的实板；可控制名称、iBeacon UUID、RSSI、major/minor、服务 UUID 和 ADV/SCAN_RSP 的真实 BLE 夹具。采用[共同过滤向量](../beacon-filter-spec.md)，RSSI 由屏蔽/衰减条件实测，不能只把注入数字当空口证据。

1. 控制台网关配置分别选择 nRF/ESP 扫描组；设置相同基础过滤与 V2 规则，先使用“校验完整扫描配置”记录编译长度与样本结果，再分别提交并等待真实 VERIFIED。
2. 用实际发射器逐项发送名称、UUID、负 RSSI、major/minor 部分掩码正反例；对照空口、UART、平台原文，确认同规则 AND、首命中与基础 filters 交集，不把 iBeacon UUID 当服务 UUID。
3. 两路同开，同一报告只能一对一合并且以 nRF 为主，observedBy 含两路。重复周期广播继续形成独立新事件；同源重复、不同 AD 和迟到报告不误合并。
4. 改为主动 PAIR，真实名称放 SCAN_RSP；测试缺响应、超过 500 ms、多候选歧义，均拒绝并计数，不能按原 ADV 放行。50/150 ms 跨 radio 去重与 500 ms PAIR 分别观察。
5. 切回旧规则时同任务提交 beaconFilterV2=null 与旧规则；切到 V2 同任务清空旧规则。超 3070 字节完整 profile 应由平台整体拒绝、设备目标保持。

**预期/证据：** 预览与实际样本相符；原始 AD 字节不被过滤器改写；缺目标能力明确拒绝。没有时间/RF 控制夹具的边界子项标阻塞，普通随机广播不替代边界样本。

**恢复：** 分别用新修订恢复两路原扫描组，确认原始采集继续；任务数按批次预算拆分，不删除 journal 腾空间。

### MAN-E2E-04：服务 UUID、平台授权自动连接及 GATT

**原映射：** PLAT-GW-AT-04/07；ESP-AT-22～24；ACC-07/08。**初始状态：未执行。**

**前置/数据：** 真实固定身份 BLE 外设，含 16/32/128 位服务 UUID 样本与可读特征；平台对端和规则已批准，预算连接/断开/读取任务。

1. 在控制台蓝牙对端登记实际地址类型/地址；建立 NRF54 目标的自动规则，选择真实服务 UUID、RSSI、新鲜度及范围授权。
2. 外设发送服务在 ADV、服务在 SCAN_RSP 的两组样本，查原始事件、自动触发记录和 GATT_CONNECT 请求；核对标准化 UUID、源位宽、advEventId 和规则 revision。
3. 观察真实连接/服务发现、安全等级和 connectionRef，读取外设固定值；连接成功但服务不存在不得报业务连接成功。
4. 分别使用未登记对端、过期证据、ESP32 单路观测、撤权规则，确认没有 nRF 自动连接；不让 ESP 自建自动策略。
5. 断开外设后重连，旧 connectionRef 的读写必须拒绝；迟到旧成功/通知不能复活断链。模糊写入交由 MAN-PLAT-05。

**预期/证据：** 平台解析与授权后才连接，UUID 不靠长度猜测原始布局；实际外设值、服务和连接身份可对账。

**恢复：** 禁用测试规则、受控断开测试连接，保留迟到消息证据；未决任务不强行释放。

### MAN-E2E-05：平台广播控制与真实空口

**原映射：** PLAT-GW-AT-05；ESP-AT-20/21；ACC-09/10；C03。**初始状态：未执行。**

**前置/数据：** 独立 BLE 嗅探器、标准 BLE 测试客户端、Beacon 与自定义合法 AD、原广播目标及已验证模式；每 radio 预算启用/更新/关闭任务。客户端只需真实连接/断开，不需 C05 业务认证和消息。

1. 在网关配置页分别选择 nRF 广播、ESP 广播，先仅对一路提交完整 enabled/mode/intervalMs/advDataBase64/scanResponseBase64。
2. 用真实抓包核对原始 Beacon/自定义字节和实际周期，保存量化读回；以任务 `/results` 和 BLE 诊断确认配置与实际 ACTIVE/STOPPED 一致。
3. 分别关闭/开启、修改内容和间隔，核对另一 radio 与扫描目标不变；NONCONNECTABLE 的 scanResponse 必须为空。
4. 对真实能力支持的三种 mode 分别观察；缺 BLE_PERIPHERAL 时 CONNECTABLE_SCANNABLE 应拒绝。启用已授权的可连接广播，用标准 BLE 测试客户端连接，核对实际连接与广播暂停的 PAUSED 证据，不能只因保存 enabled=true 判为 ACTIVE。
5. 保持客户端连接，由平台关闭广播，核对广播停止但已有连接未被强制断开；断开客户端后目标仍为关闭时不得自行开播。再按批准角色配置启用广播、连接后断开，核对按已保存 enabled 目标恢复广播。只验证连接和广播生命周期，不进行 C05 业务收发。
6. 有批准控制器故障夹具时，在热更新中注入部分失败，核对恢复旧载荷；恢复失败必须停止并报 FAULT，不以保存的 enabled=true 宣称仍广播。无夹具则该子项阻塞。

**预期/证据：** 平台要求、设备读回、实际状态和空口一致；广播频次由 intervalMs 表达，不能混用 Hz。PAUSED 必须关联真实连接，关闭广播和断开连接是独立动作，恢复遵循持久广播目标。

**恢复：** 以新任务恢复原完整目标；现场嗅探确认无遗留测试广播。

### MAN-E2E-06：远程网络凭据、候选接口与回退

**原映射：** PLAT-GW-AT-08；ESP-AT-17/18/42～44；ACC-12。**初始状态：未执行。**

**前置/数据：** 原可用网络、第二 AP/网段、受测以太网，平台 `GW_NETWORK_CREDENTIAL_KEY` 已安全注入；可分别控制新旧路由的平台可达性。

1. 配置页网络区域安装新 Wi-Fi 密码；经 GET `/gateways/GW-A/network-credentials` 确认 INSTALLED 后再选择 credentialRef。尝试未安装引用应拒绝。
2. GET 网络状态、配置 ETag 和 reportedRevision；提交单 network 组候选，包含真实 uplinkOrder、ethernet、wifiSta 及 DHCP/静态参数。
3. 让旧接口可访问平台、新接口仅能获取 IP 或访问其它网站，候选应失败/回退；不能因旧连接在线认定成功。
4. 放通候选全部变更接口及首选路由，观察逐接口应用探测与已持久结果 ACK，再确认目标提交和真实 activeInterface。
5. 分别在提交前/后断电，再次查询网络状态，核对恢复 last-good 或新目标、身份/spool 不变。丢 probe/结果 ACK 时不提前提交。

**预期/证据：** 凭据引用与 SSID/security 匹配；真实接口绑定和平台持久 ACK 成立才提交；失败有可解释回退及原事务证据。

**恢复：** 经已确认可用路径恢复原网络；远程不可达时使用 MAN-E2E-07 的已授权本地入口，不重置制造身份或数据。

### MAN-E2E-07：手机/笔记本 Wi-Fi 配网及本地覆盖裁决

**原映射：** PLAT-GW-AT-08；ESP-AT-37～44；ACC-12。**初始状态：未执行。**

**前置/数据：** 至少一台实际手机和一台笔记本；真实 SoftAP/setup 制造凭据；已确认的物理授权开窗方式。没有实际按键/受控开窗接线说明则阻塞，不能假设有自动热点按钮。

1. 现场按批准方式开启配网窗口，连接设备热点，浏览器打开实际默认网关 HTTP 地址。断开互联网，确认内置页面仍可打开，不依赖 BLE App/CDN。
2. 用错误账号验证拒绝，再用授权账号登录；输入隐藏 SSID/选择扫描结果，分别检查 DHCP 和已批准静态 IPv4、优先级及非法参数拒绝。
3. 在平台不可达时显式选择 LOCAL_ONLY 保存；查询本地事务，平台恢复后 GET `/gateways/GW-A/network-state`，核对 localOverride=true、localRevision/networkGeneration 独立增长，不伪造平台 revision/云任务。
4. 平台配置页读取最新网络 ETag/快照，选择 ADOPT_LOCAL 或 APPLY_PLATFORM；API 为 POST `/gateways/GW-A/network-reconciliation`，带 If-Match/Idempotency-Key、expectedNetworkGeneration、snapshotId、expiresAt；APPLY_PLATFORM 另带完整 network。
5. 先用旧 ETag/旧世代作负例，再用正确值裁决；确认 resolutionToken 由服务器生成，成功结果已持久并 ACK 后才清除 localOverride。
6. 浏览器断开后重连查询原 transactionId；关闭窗口或等超时后再次访问，确认受保护操作失效。另测平台网络/OTA 与本地候选竞争，只能一个写事务。

**预期/证据：** 手机仅通过 Wi-Fi 完成配网；离线保存不是 PLATFORM_SYNCED；旧页面不能覆盖新本地目标；密码不回显到普通状态或审计。

**恢复：** 用已授权本地恢复前一网络或平台新任务恢复基线，关闭配网窗口；清除浏览器测试会话，不清设备 journal。

### MAN-E2E-08：平台双组件 OTA 与库存更新

**原映射：** PLAT-GW-AT-09；ESP-AT-26～32/36/49/50；ACC-13～16；C04/C06/C11。**初始状态：未执行。**

**前置/数据：** GATEWAY/NRF54 两种真实目标签名包、可回退旧镜像、真实 MCUboot 及独立镜像证明；nRF reset 功能只有板级接线/电平/脉冲已验证才启用。缺目标 SDK/bootloader/夹具即阻塞相应项。

1. 经控制台制品页导入并验签两种制品；记录 artifactId、原 manifest/signature、SHA、大小及实际兼容版本，不保存私钥。
2. 分别创建 FIRMWARE_UPGRADE：parameters 含 component=GATEWAY 或 NRF54、artifactId、expectedFirmwareVersion；有效期覆盖真实下载/擦写/确认。两组件依次执行，不能并行抢占。
3. 观察 GATEWAY 的真实新启动与健康确认，NRF54 的 UART 写入、重启后 HELLO、VERIFY_RUNNING/CONFIRM 证据，核对结果与新组件库存一致。
4. 在专用轮次提交错误签名/型号/组件/SHA/尺寸及不匹配源版本，确认拒绝；错 MCUboot 签名按实际启动验证行为记录，不能把 READY 当升级成功。
5. 用夹具分别在传输中、FINISH 结果不确定、试启动未确认点中断；传输中重启不得复用旧 RAM offset，不能误判已回滚；重新 BEGIN 必须有有效授权并从 0 开始。
6. 丢成功结果 ACK，确认只重投稳定结果、不重新升级。未确认/挂死镜像的真实回退须分别取证；硬件 reset 未验证保持禁用，不假报其通过。

**预期/证据：** 两组件版本/SHA/boot 独立；只有真实运行并确认才成功，ESP 90 s/nRF 120 s 试运行边界有时间线；本地数据和配置在批准兼容范围内保留。

**恢复：** 按已验证 bootloader/发行回滚流程恢复已签名可用镜像，重新取得真实库存；结果不确定时保留升级所有权，不重刷掩盖故障。

### MAN-E2E-09：手机/笔记本 Wi-Fi 本地 ESP OTA

**原映射：** PLAT-GW-AT-10；ESP-AT-31/32/45～50。**初始状态：未执行。**

**前置/数据：** MAN-E2E-07 的本地入口；实际 ESP 的 verification.json 与 firmware.bin，同一签名原文也已用于平台入口；至少一台手机和笔记本分别执行。

1. 断开外网，在授权配网窗口连接 SoftAP 并登录本地网页，选择 verification.json 和 firmware.bin，开始上传并记录 uploadId/offset。
2. 上传完成只到 READY；尚未点击明确安装时不得切槽。检查无云 taskId/requestDigest/远程成功结果被伪造。
3. 点击明确安装，记录 REBOOT_PENDING、实际重启及 90 s 内健康确认；无需互联网、可信 UTC 或浏览器持续连接。重新进入本地页确认 CONFIRMED 或实际失败/回退。
4. 分轮验证坏签名/非法长度、浏览器断连重登录、窗口关闭、取消和远程 OTA 竞争；同启动 RECEIVING 可按真实偏移恢复，接收中设备重启后新 uploadId 从 0 开始。
5. 有夹具时在提交前后断电及制造已批准的健康失败条件下验证回退；联网后查询组件库存/制品关联，未导入 artifactId 时允许 VERSION_ONLY，不冒称确认制品。

**预期/证据：** 本地与平台同包校验一致；手机通过 Wi-Fi 操作；READY/提交/运行确认分离，窗口和互斥不绕过。保存本地状态、实际镜像 SHA 和重启日志。

**恢复：** 经已验证的 ESP 回滚/本地升级路径恢复基线版本，关闭窗口并确认网络配置、未接管/已接管队列责任未丢失。

### MAN-E2E-10：离线、满队列和任务容量单轮

**原映射：** PLAT-GW-AT-02/06/11；ESP-AT-08/12/15/33/35；ACC-03/06/18。**初始状态：未执行。**

**前置/数据：** 三类专用轮次：设备数据 spool、平台接管额度、任务 journal；真实受控负载与容量只读观测，冻结 GW_MAX_QUEUE_BYTES 和设备空间预算。任务极限轮次使用独立样机，常规测试不能顺便耗满。

1. 数据轮次先记录基线，阻断平台并维持真实 Beacon 输入；观察待定区/spool/隔离区、水位、背压和可解释丢弃，不得发出尚未持久的 CUSTODY。
2. 在专用平台实例设置批准的较小接管额度，阻断客户消费并产生真实数据直到平台拒绝接管；设备应保留待确认原文并退避，不能因 HTTP 请求完成清队列。
3. 恢复平台/客户，核对稳定 eventId、原文、业务去重及队列逐步排空；超出 dedup 保留期的离线记录单独隔离评估，不能无限承诺去重。
4. 任务容量专用轮次逐个创建低影响、可真实完成的诊断，记录每个身份/字节增量，直到任一实际容量边界拒绝；不得超过设备 64 身份/96 KiB 限制后假称继续正常受理，也不得用重复旧幂等键冒充新身份负载。
5. 达到容量后检查已有结果重传、防重证据与只读查询仍可解释；记录实际错误及未受理请求，停止该样机新增任务。此轮不要求凭空提供不存在的归档 API。

**预期/证据：** 有界资源和明确背压，无假 ACK/无声丢未确认数据；任务容量不会通过覆盖旧防重记录维持表面成功。保存容量曲线、事件集合对账和拒绝响应。

**恢复：** 数据轮次仅恢复消费/链路按正常 ACK 回收；任务极限样机保持 journal 和证据并退出常规测试。没有批准恢复流程则标阻塞，换独立制造样机，不擦除旧身份历史。

### MAN-E2E-11：长期运行与重启后的全链路对账

**原映射：** PLAT-GW-AT-03/05/08/11；ESP-AT-09～14/18/19/33/44；ACC-03～06/09/12/18。**初始状态：未执行。**

**前置/数据：** 冻结持续时长（建议先 24 h，正式时长由验收负责人填写）、实际包速率/长度、客户吞吐、允许延迟/恢复时间、Flash/堆/队列预算；经板级验证的双扫描、网络及广播组合。参数缺失则阻塞判定，不自行承诺容量。

1. 开始前保存配置、库存、任务身份/字节占用和只读诊断基线；本轮最多预算初始配置及开始/结束必要诊断，不周期创建新任务。
2. 固定真实 BLE 输入并持续上传；人工按冻结间隔用 UI/API GET 和只读设备日志记录队列、最老记录年龄、错误、内存、存储和交付延迟，不添加伪造监控字段。
3. 按预定时间表执行真实 AP 切换、Broker 短断、客户短离线、Flash GC 压力及一次受控重启。能力不支持的角色组合记录明确拒绝，不强开组合。
4. 结束后恢复链路，核对原始事件、重复 ACK、客户业务身份、双路 observedBy 和各组持久目标；存在的 UNKNOWN/隔离/未接管缺口逐项说明。
5. 对照冻结指标判定持续泄漏、无进展队列、死锁、假成功或不可恢复故障；超过任务预算立即停止新增任务，不能清 journal 延长试验。

**预期/证据：** 在批准负载下有持续进展、资源有界、恢复符合承诺；所有异常可追溯。缺传感器计数器、容量指标或完整日志时不能只凭“网页在线”通过。

**恢复：** 停止测试发射器，恢复批准配置和网络，正常排空已确认队列；保留长期日志、未决任务和样机身份，不做无证据的批量清理。

## 5. 可选附录：MAN-E2E-90 手机 BLE 业务（排除必测）

**原映射：** ACC-11、ESP-AT-25、C05；关联 PLAT-GW-038/058。**初始状态：未执行；范围待确认，不计入本轮必测完成率。**

这里保留已有编号追溯，不要求用户为 Wi-Fi 配网/OTA 安装蓝牙业务 App。只有范围确认、实际手机业务客户端、独立 Security2 制造凭据、route 授权及相应能力均齐备后才可执行。普通 BLE 连接对广播暂停/恢复的影响已经列入 MAN-E2E-05，不属于本附录的待确认范围。

**可选执行步骤：**

1. 按[业务桥接说明](../../firmware/esp32s3/components/gw_peripheral/README.md)登记专用 clientId/route，并通过真实 `PUT /api/v1/gateways/{gatewayId}/peripheral-clients/{clientId}` 授权；该接口不安装手机密码。
2. 真实手机通过独立业务 Security2 会话发送 PG 分片，记录本地 STORED、平台 `/gateway-peripheral/batches` 的 PERIPHERAL_PERSISTED 和稳定 businessId。
3. 经真实任务接口测试 PERIPHERAL_NOTIFY/DISCONNECT；NOTIFY 只有匹配会话/业务身份的手机 APPLIED 才成功，STORED/ATT/发送成功均不能替代。
4. 分别测试未认证、乱序、冲突重放、旧 boot 离线消息、缺 runtime、满队列和手机 ACK 丢失。旧消息可持久接管，但不得重新创建旧 boot 的下行会话；不影响普通管理任务授权。

**证据与恢复：** 保存独立身份和每层 ACK，确认 C05 范围后另行批准验收指标；结束时撤销测试 route、关闭业务会话，未确认业务保留/隔离，不与采集 RawEvent 队列混清。

## 6. 验收编号追溯与人工结果表

平台原编号使用 **PLAT-GW-AT**，下表不另造 PLAT-AT 别名。映射表示本文件包含相应平台/贯通步骤，不声称覆盖芯片专项的每个电气、UART 畸形帧、FEM 或全部 OTA 掉电点；这些仍按原专项计划逐点执行。

| 原验收 ID | 本文件用例 |
| --- | --- |
| PLAT-GW-AT-01 | MAN-PLAT-01/02/06/09，MAN-E2E-01 |
| PLAT-GW-AT-02 | MAN-E2E-01/02/10 |
| PLAT-GW-AT-03 | MAN-E2E-03/11 |
| PLAT-GW-AT-04 | MAN-E2E-03/04 |
| PLAT-GW-AT-05 | MAN-PLAT-04，MAN-E2E-05/11 |
| PLAT-GW-AT-06 | MAN-PLAT-03/04/05/08，MAN-E2E-01/02/10 |
| PLAT-GW-AT-07 | MAN-PLAT-05/06/09，MAN-E2E-04 |
| PLAT-GW-AT-08 | MAN-PLAT-08/09，MAN-E2E-06/07/11 |
| PLAT-GW-AT-09 | MAN-PLAT-06/07/09，MAN-E2E-08 |
| PLAT-GW-AT-10 | MAN-PLAT-07，MAN-E2E-09 |
| PLAT-GW-AT-11 | MAN-PLAT-01，MAN-E2E-10/11 |
| ESP-AT-01/02/03 | MAN-PLAT-02/03、MAN-E2E-10/11 只覆盖身份/重试/负载关联；完整 UART 向量、CTS/DMA 仍见原专项 |
| ESP-AT-04～08/35 | MAN-E2E-02/10 |
| ESP-AT-09～11/19 | MAN-E2E-03/11；ESP-AT-11 另见 MAN-PLAT-06 |
| ESP-AT-12～14 | MAN-E2E-01/02/10/11 |
| ESP-AT-15 | MAN-PLAT-03/04/05/08，MAN-E2E-01/10 |
| ESP-AT-16 | MAN-PLAT-02/06 |
| ESP-AT-17/18 | MAN-E2E-06；ESP-AT-18 另见 MAN-PLAT-08、MAN-E2E-11 |
| ESP-AT-20/21 | MAN-PLAT-04、MAN-E2E-05，含标准 BLE 客户端连接下的 PAUSED、关闭及恢复 |
| ESP-AT-22～24 | MAN-PLAT-05/09、MAN-E2E-04 |
| ESP-AT-25 | MAN-E2E-90，范围待确认/排除必测 |
| ESP-AT-26～30/36 | MAN-PLAT-07、MAN-E2E-08，仍需原专项每个真实掉电点 |
| ESP-AT-31/32 | MAN-E2E-08/09 |
| ESP-AT-33/34 | MAN-E2E-10/11；构建/制造证据另见 MAN-PLAT-01/02 |
| ESP-AT-37～44 | MAN-E2E-06/07；权限/恢复关联 MAN-PLAT-08/09 |
| ESP-AT-45～50 | MAN-E2E-08/09；时间/库存关联 MAN-PLAT-06/07 |
| ACC-01～03 | MAN-PLAT-02/03/05、MAN-E2E-02/10/11 的贯通部分；UART 专项不被替代 |
| ACC-04～06 | MAN-E2E-02/03/10/11 |
| ACC-07/08 | MAN-PLAT-05/09、MAN-E2E-04 |
| ACC-09/10 | MAN-E2E-05，含可连接广播生命周期与热更新回退 |
| ACC-11 | MAN-E2E-90，范围待确认/排除必测 |
| ACC-12 | MAN-PLAT-08/09、MAN-E2E-06/07/11 |
| ACC-13～16 | MAN-PLAT-07、MAN-E2E-08；真实 MCUboot/电源/复位专项仍需原计划 |
| ACC-17/18 | MAN-PLAT-01/02、MAN-E2E-10/11 的基线及负载部分；目标 SDK、射频/FEM 专项不能由平台测试代替 |

| 用例 | 初始结果 | 执行人/时间 | 证据位置 | 失败或阻塞原因/恢复状态 |
| --- | --- | --- | --- | --- |
| MAN-PLAT-01 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-02 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-03 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-04 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-05 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-06 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-07 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-08 | 未执行 | 待填 | 待填 | 待填 |
| MAN-PLAT-09 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-01 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-02 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-03 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-04 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-05 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-06 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-07 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-08 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-09 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-10 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-11 | 未执行 | 待填 | 待填 | 待填 |
| MAN-E2E-90 | 未执行；范围待确认，排除必测 | 待填 | 待填 | 不计入本轮必测完成率 |
