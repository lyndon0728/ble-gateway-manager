# 网关管理平台对接需求清单

版本：1.0 · 日期：2026-11-19 · 统一标准：`GW32-E2E-1.0`

本文定义网关管理平台与 ESP32-S3、nRF54L15 贯通所需的业务、接口和验收要求，补充现有整个平台需求，不替换用户、项目、数据处理、客户投递、AI、部署和运维等既有功能。统一裁决见[端到端标准](../../docs/gateway-end-to-end-standard.md)；开发落点见[平台开发文档](gateway-platform-development.md)。

本轮交付是需求和开发文档对齐。下文“必须”表示产品与协议要求，不表示当前代码已实现或实板已验收。现有功能全部保留；协议字段适配、能力限制、实现缺口和硬件验证必须分别记录。当前源码审查基线为 `0897044`，ESP 固件实现基线为 0.2.1。

## 1. 三端职责与协议边界

| 主体 | 必须承担的职责 |
| --- | --- |
| 管理平台 | 身份与权限、目标配置及修订、持久任务、原始数据接管、UUID 解析、规则授权及自动连接决策、组件固件制品与部署、网络本地/远程冲突裁决、证据与审计 |
| ESP32-S3 | 网络与本地网页入口、平台协议执行、nRF UART 适配、双射频采集去重、可靠存储与重传、配置试用回退、双组件升级编排、自身 BLE 功能 |
| nRF54L15 | 按配置扫描与过滤、原始观测上报、BLE Central/Peripheral 与广播、UART 执行/读回、通过 ESP UART 升级 |

平台到 ESP 使用[平台协议](gateway-protocol.md)；ESP 到 nRF 使用[UART 协议](gateway-uart-protocol.md)。平台不直接下发任意 UART 二进制，也不把 UART ACK 当作云端任务完成。两个协议保留各自版本，`GW32-E2E-1.0` 是跨端约束标识，不是替换线缆帧或 JSON `schemaVersion` 的新字段。

## 2. 身份、能力与兼容性

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-001 | 网关身份由可信登记与凭据绑定 namespaceId/gatewayId；HTTP 身份、MQTT 身份及 topic 作用域必须一致，不能只相信消息正文自报身份。 |
| PLAT-GW-002 | 权限按操作者及实例/项目/组/网关范围检查；自动规则执行时再次检查规则所有者权限和网关授权，撤权后不得继续生成任务。 |
| PLAT-GW-003 | 设备能力以签名清单为平台授权上限，实际执行能力为清单、ESP 驱动、角色资源状态、nRF HELLO 的交集。一个目标的能力不得赋给另一目标。 |
| PLAT-GW-004 | 保留参考协议 1.0 与 GW32 控制扩展；GW32 原始采集消息使用 schemaVersion 1.1，任务、结果、状态等仍使用各自既有 1.0 信封。禁止全局替换版本字符串。 |
| PLAT-GW-005 | 配置键使用 `radios.nrf54` / `radios.esp32`；任务和 capture.radio 使用 `NRF54` / `ESP32`；缺省 radio 的既有兼容路径仍指向 NRF54。 |
| PLAT-GW-006 | 网关型号及硬件版本代表整机；组件标识区分 GATEWAY（ESP 主机）与 NRF54。不得用 ESP 版本证明 nRF 的源版本兼容性。 |

## 3. 上行采集、解析与确认

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-007 | MQTT 与 HTTPS 提供相同的遥测、任务、结果、应用 ACK 和状态语义；保持 MQTT 需求。当前 ESP HTTPS 实现不等于取消 MQTT。MQTT QoS 1、retain=false，PUBACK 不等于应用持久接管。 |
| PLAT-GW-008 | 原始事件完整保留 payloadBase64、稳定 eventId、时间质量和 capture；不得仅上报解析结果而丢失原始 BLE AD/GATT 值。 |
| PLAT-GW-009 | 单批最多 200 条、单条原始载荷不超过 64 KiB、批次原始总量不超过 256 KiB、HTTP JSON 不超过 1 MiB；内部 upload.batchMaxRecords 可到 1000，ESP 必须拆为合法批次。 |
| PLAT-GW-010 | 平台在原文、收据和处理任务同事务持久化后才返回 RAW_PERSISTED；ACK 匹配 namespaceId/gatewayId/batchId，逐项 ACCEPTED/DUPLICATE 才允许 ESP 回收。身份相同内容不同必须显式冲突，不能视为重复成功。 |
| PLAT-GW-011 | UART CUSTODY 只证明 ESP 可恢复接管；平台 RAW_PERSISTED 只证明平台原始数据接管；客户最终交付另有应用 ACK。三个边界不得混用。 |
| PLAT-GW-012 | 跨 nRF/ESP 观测去重由 ESP 执行，一对一配对且保留 nRF 为主记录，capture.observedBy 表示双路共同观察。平台按稳定事件身份处理重传，不用 MAC/UUID/载荷全局去掉后续周期广播。 |
| PLAT-GW-013 | ADV 和 SCAN_RSP 分别上报；跨射频相同报告合并与平台 500 ms ADV/SCAN_RSP 证据配对是两项独立处理，不得混为一个窗口。时钟不可信时保留数据但不得借此触发需要可信新鲜度的自动连接。 |
| PLAT-GW-014 | 解析服务 UUID 支持 16/32/128 位以及合法服务数据字段，规范为小写完整 128 位 UUID，同时保留来源宽度及证据。UUID 类型由 AD type 和载荷结构决定，不依赖字符串长度猜测广播字节布局。 |
| PLAT-GW-015 | 保留现有解析、隔离、原文重解析、字段映射、HTTP/MQTT 客户投递和背压行为；不因新增网关对接标准变更既有客户数据功能。 |

## 4. 任务、配置与状态

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-016 | 管理端以 Idempotency-Key 创建任务，平台生成 taskId/requestDigest；同一键重试保持原请求与摘要。任务已受理、已发送、已执行、已验证必须分别展示。 |
| PLAT-GW-017 | 每网关当前只放行一个在途业务任务；UNKNOWN 持续占用且允许诊断对账，不以超时自动确认失败并重做可能已发生的 GATT 写入、重启或升级。 |
| PLAT-GW-018 | 只取消尚未投递任务；当前无下发 CANCEL 协议。任务到期停止尚未执行的后续动作，但不撤销已发生的物理副作用。 |
| PLAT-GW-019 | 结果 resultId/resultSequence 稳定，重投不得重新执行；结果在平台事务提交后返回 RESULT_PERSISTED，逐项 ACCEPTED/DUPLICATE 才完成 ESP 结果出队。 |
| PLAT-GW-020 | 配置更新使用 ETag/If-Match、expectedReportedRevision、Idempotency-Key；desiredRevision 由平台分配，reportedRevision 只依据合法执行证据前进且不得回退。 |
| PLAT-GW-021 | 一次扩展配置只修改 network、一个 radios 目标、或既有配置之一；一个 radio 只修改扫描组或广播组。ESP 合并该组持久快照后校验和执行，数组/字节段整体替换。 |
| PLAT-GW-022 | 配置查询中的 desired/reported 是最近配置补丁；平台汇总视图的 complete=false、valueSemantics=ACKNOWLEDGED_PATCH_NOT_RUNTIME_READBACK 明确表示不完整且不是运行读回，不得用它覆盖 ESP 完整持久目标；成功结果 details.configuration 回显原请求，details.effective 可独立报告量化及实际状态。 |
| PLAT-GW-023 | 只有设备结果通过内容与证据校验且为 SUCCEEDED/VERIFIED，才可显示任务成功。HTTP 2xx、MQTT PUBACK、UART 命令响应或 revision 分配都不能单独证明成功。 |
| PLAT-GW-024 | 诊断与状态保留网络、BLE、存储、固件版本、队列、丢弃和回退信息；目标配置与实际运行状态分别表达，敏感凭据不得出现在普通状态、审计和诊断包。 |

## 5. 扫描、过滤与对外广播

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-025 | nRF 和 ESP 都保留独立扫描配置 enabled/active/intervalMs/windowMs；10–10240 ms 且 window≤interval。平台使用毫秒整数，UART 使用 0.625 ms tick；实际量化值通过有效状态体现。 |
| PLAT-GW-026 | 基础过滤 minRssi 范围 -127–20 dBm；扩展对象允许 null 清除阈值，旧顶层 filters 保持旧约束。UUID/厂商列表每项上限 128，空数组表示清除。UART -128 哨兵只用于适配层，不作为新的平台 RSSI 值。 |
| PLAT-GW-027 | 基础过滤中不同类别 AND、同一列表 OR；Beacon 规则最多 8 条，按顺序首个命中生效。存在 ALLOW 规则且全未命中则拒绝，仅 DENY 或空规则则默认放行。ADV/SCAN_RSP 各自过滤。 |
| PLAT-GW-028 | Beacon 规则采用 action/reportKinds/adType/offset/valueBase64/maskBase64；匹配字节 1–24、掩码等长且非全零，offset+长度≤254。Schema 能表达 EXT 规则不代表已验证目标扩展扫描能力。 |
| PLAT-GW-029 | 平台可分别开启、关闭两路广播，定义广播模式、内容和频次，使用具有 revision 的 CONFIGURE；启停保留其他参数，更新内容不清除扫描规则。 |
| PLAT-GW-030 | 广播 mode 为 NONCONNECTABLE、SCANNABLE_NONCONNECTABLE、CONNECTABLE_SCANNABLE；间隔 100–10240 ms。可连接广播同时要求目标 BLE_ADV 与 BLE_PERIPHERAL，nRF 还检查 HELLO 能力位。 |
| PLAT-GW-031 | Beacon 与自定义广播均以 advDataBase64/scanResponseBase64 表达完整原始 AD，每段解码 0–31 字节；NONCONNECTABLE 的扫描响应必须为空。平台可提供内容生成界面，但不得自动注入改变已提交的 AD 字节。 |
| PLAT-GW-032 | 广播查询区分已保存配置与真实启停状态；BLE 诊断须能沿 ESP 读取 nRF GET_ADV_CONFIG/GET_ADV_STATUS。nRF 重启默认关闭，由 ESP 恢复已提交目标；同启动周期 UART 重握手不丢目标。 |

## 6. GATT 与自动连接

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-033 | 保留 GATT_CONNECT/DISCONNECT/READ/WRITE/SUBSCRIBE/UNSUBSCRIBE；按 radio、地址类型、规范 UUID、connectionRef、超时及能力校验。报告实际安全级别、MTU、服务发现及订阅 CCC 证据。 |
| PLAT-GW-034 | 自动连接由服务器解析原始 AD 并按显式授权规则生成 NRF54 的 GATT_CONNECT；不能让 ESP/nRF 自建未授权自动连接策略，也不能把 ESP32 单独观测当成 nRF 已见的可连接证据。 |
| PLAT-GW-035 | 自动连接前验证稳定对端登记、规则启用及 revision、范围权限、广播新鲜度/RSSI、可连接标志、所需 UUID 与目标能力；对端跨网关独占，限速及冷却保持既有规则。 |
| PLAT-GW-036 | autoTrigger 由服务器生成并关联 advEventId、规则 revision、规范 UUID 和来源；ESP 校验本地证据，nRF 执行连接及服务发现。服务不存在不能报业务连接成功。 |
| PLAT-GW-037 | connectionRef 绑定 radio、控制器 boot ID 和连接 ID，不能只用易复用的控制器 handle；断链事件建立墓碑，迟到成功结果不得复活已断连接。 |
| PLAT-GW-038 | nRF Peripheral 可连接广播、服务发现、订阅及写入路径继续保留；Peripheral 收件 0x22 进入 ESP 本地持久 inbox，与云端 Central GATT 值上报区分。手机 ATT 成功不代表平台业务已执行。 |
| PLAT-GW-039 | UNENCRYPTED/BONDED_ENCRYPTED 等安全要求不得在执行时静默降级；当前 ESP 原生 BLE 的非 UNENCRYPTED、可连接广播和自动连接限制必须明示为支持集差异/待实现，不删掉既有相应需求。 |

## 7. 本地 Wi-Fi 配网与平台远程配置

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-040 | 保留手机/笔记本连接 ESP SoftAP 的本地网页配网，支持 Wi-Fi STA、Ethernet、DHCP/静态 IPv4 和主备顺序。平台远程配置复用同一候选试用/回退语义；现场入口无需伪造云端任务。 |
| PLAT-GW-041 | 普通 network 配置只携带 credentialRef；远程密码经独立受权接口安装，设备加密存储成功才回 INSTALLED。未安装或 SSID/security 不匹配的凭据不得用于远程网络切换。 |
| PLAT-GW-042 | networkGeneration/localRevision 为规范十进制 uint64 字符串；平台配置 revision 为 JSON 安全整数。必须分别管理，不能以本地成功推进平台 desiredRevision。 |
| PLAT-GW-043 | 设备上报保存目标、实际网络、activeInterface、localOverride、verificationState、lastKnownPlatformRevision。LOCAL_ONLY 与 PLATFORM_CONNECTED/PLATFORM_SYNCED 不等价；同一 generation 下实际故障切换可变，不得改写保存目标。 |
| PLAT-GW-044 | 远程切换逐个验证变更接口及候选首选路由，应用连接绑定候选接口，并获得先持久后确认的网络试用结果；失败/到期/提交前重启回退。仅获得 IP 或连通公网不等于平台可达。 |
| PLAT-GW-045 | 本地修改与远程目标冲突使用 ADOPT_LOCAL/APPLY_PLATFORM、If-Match、expectedNetworkGeneration、snapshotId 及服务器生成 resolutionToken 裁决；不得用旧页面或手工 token 覆盖本地状态。 |
| PLAT-GW-046 | 清除 localOverride 必须引用已成功的配置任务和新世代，ESP 先取得匹配成功结果应用 ACK，再清除覆盖。已发送或 UNKNOWN 任务不得通过 cancelQueued 释放所有权。 |
| PLAT-GW-047 | 本地入口要求物理配网窗口和本地认证，密码不进入普通任务、审计和诊断。平台远程秘密当前为服务器可信解密再加密存储的模式，不能宣称设备独占端到端密封。 |

## 8. 双组件 OTA 与本地 ESP OTA

| 编号 | 需求 |
| --- | --- |
| PLAT-GW-048 | 平台 FIRMWARE_UPGRADE 保留 component=GATEWAY/NRF54，省略时兼容 GATEWAY。GATEWAY 为 ESP 自身 OTA，NRF54 由 ESP 下载验证后经 UART 升级。两组件版本及升级证据分别记录。 |
| PLAT-GW-049 | 固件制品使用签名 manifest 原始 UTF-8、签名、公钥标识、imageFormat、精确大小、SHA-256、型号/硬件/源版本兼容信息；验签不得先解析重排再序列化替代签名原文。 |
| PLAT-GW-050 | 远程制品下载绑定所属网关、在途未过期任务及制品；有效期和授权必须覆盖下载、擦写和提交窗口。本地上传使用本地授权和单调期限，不要求云端任务；签名不替代任一入口的执行授权。 |
| PLAT-GW-051 | NRF54 要求 FIRMWARE_UPGRADE、NRF54_UART_DFU_1 与 UART DFU 能力；镜像为 MCUBOOT_SIGNED_BIN，不能把裸 bin/merged.hex 作为同类制品。成功须含组件、新版本、新 nrfBootId、bootConfirmed=true 和匹配摘要。 |
| PLAT-GW-052 | GATEWAY 新固件完成自检和启动确认后才成功；保留 ESP 90 s 与 nRF 120 s 各自试运行约束，失败回滚并可对账。nRF 启动变化须按持久阶段区分下载中断与 READY 后试启动，不能一律判回滚；重新传输须有效授权并从 0 BEGIN，不强制同任务自动重传。下载完成、串口写入完成、设置启动分区均不等于升级成功。 |
| PLAT-GW-053 | ESP 本地网页 OTA 与远程 OTA 使用相同签名包、大小/摘要/型号/版本校验与全局升级互斥；本地入口由物理窗口及本地会话授权，以 LOCAL/uploadId 记录，不能编造 taskId/requestDigest/远程结果。 |
| PLAT-GW-054 | 本地 OTA 可离线完成，试启动和恢复不以互联网或可信 UTC 为必要条件，联网后报告真实当前状态；必须保持启动确认、回滚和断电恢复要求。升级窗口与会话关闭后停止尚未提交的操作，提交结果不明时保留证据及互斥进行对账。 |
| PLAT-GW-055 | nRF 源版本兼容约束必须来自独立可信组件库存。现平台尚无此库存时，非空 fromFirmwareVersions 明确拒绝并列入后续开发，不得用 ESP firmwareVersion 代替或绕过约束。 |
| PLAT-GW-056 | 自动连接新鲜度、远程任务有效期、TLS 和远程升级授权依赖可信 UTC；时间来源的可认证性、误差与启动后可信门槛必须完成验证，不能把制造指定 SNTP 服务器等同密码学认证。本地离线 OTA 使用本地授权和单调期限，不因此增加 UTC 前提。 |
| PLAT-GW-057 | nRF 升级失败或试运行未确认时保留通过板级复位恢复的要求；只有软件 DFU 命令、等待和重新 HELLO 不等于硬件复位恢复已完成。 |
| PLAT-GW-058 | Peripheral 业务透传保留平台封装、身份绑定、会话授权、关联回复及下行回发需求；目前本地持久 inbox 不等于这些接口已贯通。后续须正式扩展 Schema/命令合同后实现，不临时借用 Central GATT 字段。 |
| PLAT-GW-059 | ESP 保存各组完整目标并在重启后恢复；本地持久目标丢失时禁止把 complete=false 的平台视图当全量恢复快照，须走受控重放/重配置并留下可验证恢复证据。 |

## 9. 验收清单

下列用例是贯通验收门槛；代码单元测试、主机模拟器或文档检查通过均不替代实板用例。

| 用例 | 覆盖范围 | 通过条件 |
| --- | --- | --- |
| PLAT-GW-AT-01 | 身份、能力、两种传输 | 错身份/错 topic 拒绝；HTTP/MQTT 产生相同事件与任务语义；未实现能力不虚报。 |
| PLAT-GW-AT-02 | 上行与应用 ACK | CUSTODY、RAW_PERSISTED、客户 ACK 分别故障注入；断电、丢 ACK 后稳定身份重传，只有正确逐项 ACK 触发回收。 |
| PLAT-GW-AT-03 | 双路扫描去重 | 同一报告一对一合并、nRF 主记录、observedBy 正确；周期广播不被无限去掉；不可映射时间不强合并。 |
| PLAT-GW-AT-04 | 过滤和 UUID | RSSI null/空数组/首条规则/16、32、128 位 UUID 边界一致；ADV 与 SCAN_RSP 不混作同一原始报告。 |
| PLAT-GW-AT-05 | 广播 | 两目标独立启停、Beacon/自定义内容、间隔量化与读回；不支持角色显式拒绝；重握手、重启、更新失败回退后目标可解释。 |
| PLAT-GW-AT-06 | 任务与修订 | 重复请求、重投任务、迟到 ACK、摘要冲突、过期、UNKNOWN 不造成重复副作用或 reportedRevision 回退。 |
| PLAT-GW-AT-07 | 自动连接与 GATT | 授权规则触发、撤权及时阻止、UUID 服务发现、不新鲜证据拒绝、跨网关独占、模糊写入 UNKNOWN、迟到结果不复活断链。 |
| PLAT-GW-AT-08 | 本地与远程配网 | 手机/笔记本完成本地设置；每候选接口应用探测失败回退；本地覆盖裁决及并发旧 ETag 不覆盖新配置；密码无明文泄漏。 |
| PLAT-GW-AT-09 | 平台双组件升级 | 两组件签名/摘要/版本独立，下载权限及过期有效，擦写和重启断电可恢复，真实试运行和确认后才能成功。 |
| PLAT-GW-AT-10 | 本地 ESP OTA | 同包本地/远程一致，离线本地升级不制造远程任务，窗口关闭/慢上传/提交状态不明行为受控，回滚可验证。 |
| PLAT-GW-AT-11 | 长时间运行 | 满盘背压、断网缓存、Wi-Fi/BLE 共存、多连接/广播调度和恢复均有真实计数及容量边界，不静默丢未确认数据。 |

## 10. 当前差距与保留需求

| 项目 | 当前审查结论 | 后续要求 |
| --- | --- | --- |
| ESP MQTT 通道 | 平台入口与需求存在；ESP gw_cloud 当前使用 HTTPS | 补齐同等应用 ACK、身份、重连和网络试用语义，不能只做 PUBACK 适配。 |
| ESP 原生 Peripheral / 加密连接 | Schema/产品需求覆盖更宽；当前驱动仅非连接广播和 UNENCRYPTED 等子集 | 保留需求，补真实实现、能力声明与测试；不支持时明确拒绝。 |
| NRF54 源版本约束 | 平台缺独立可信组件库存，对非空约束拒绝 | 定义并实现组件库存的可信上报、重启/升级更新与兼容校验，然后开放约束任务。 |
| 广播实际状态 | nRF 提供状态查询；ESP 当前配置读回/诊断还不能完整证明 GET_ADV_STATUS 实际状态一致 | 按统一标准 C03 补端到端读回，不把已保存 enabled 当实际广播已开启。 |
| nRF OTA 硬件复位恢复 | 软件 DFU 已有执行路径；硬件复位恢复链尚须补齐验证 | 按 C04 保留板级复位与重连/回滚证据要求。 |
| Peripheral 平台业务 | UART 收件与 ESP 本地 inbox 已有；平台业务合同尚缺 | 按 C05 补消息封装、认证和回发任务，不删除该需求。 |
| 平台配置恢复视图 | 汇总响应明确 complete=false | 按 C08 保留完整恢复需求，不能把缺字段解释成清空或默认值。 |
| 可信 UTC | 当前制造指定 SNTP 无密码学认证 | 按 C09 补时间可信性与误差证明，避免过期任务、自动连接与远程 OTA 判断基于虚假时间；本地离线 OTA 不增加 UTC 前提。 |
| nRF 传输中重启恢复 | 当前 ESP 在 nRF boot 变化后可能把下载中断当作试启动回滚 | 按[统一标准 C11](../../docs/gateway-end-to-end-standard.md#2-冲突缺口与唯一处理标准)依据 journal 分阶段恢复；有效授权下重新传输从 0 BEGIN，不强加同任务自动重传。 |
| 真实贯通 | 存在服务器、ESP、nRF 的各自实现和测试，不能等同三方联合验收 | 按本节验收清单和各固件验证计划完成实板证据。 |
