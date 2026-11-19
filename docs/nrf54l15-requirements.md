# nRF54L15 嵌入式软件需求与贯通验收

文档版本：1.1；更新日期：2026-11-19；统一标准：**GW32-E2E-1.1**；源码核对基线：nRF 应用 `0.2.0`、ESP 应用 `0.2.1`，源码提交 `9533a14`。本次完善开发文档与规划协议，保留既有功能和编号；Beacon V2 是待实现扩展，不改动当前固件或已上线字段。

## 1. 适用范围与文档优先关系

本文件是 nRF54L15 专项需求，继承 [总需求](gateway-requirements.md) 的原编号。`SYS-*`、`UART-*`、`SCAN-*` 等编号含义不变，不能以本文件发布为由删除尚未完成的需求。三方职责、冲突 C01～C12 的裁决及后续源码待办以 [端到端统一标准 GW32-E2E-1.1](gateway-end-to-end-standard.md) 为共同基准；字节编码以 [板内 UART 协议 1.2](../server/docs/gateway-uart-protocol.md) 为准，平台消息以 [服务器协议](../server/docs/gateway-protocol.md) 的 Schema `1.0/1.1` 为准。UART 文档修订 `1.2` 保留线上帧版本 `0x01`；其中既有接口对应当前固件，V2 的能力位及新命令必须实现并验收后才可使用。

实施步骤见 [nRF 开发文档](nrf54l15-development.md)，ESP 责任见 [ESP 需求](esp32s3-requirements.md) 和 [ESP 开发文档](esp32s3-development.md)。本文件不重新认证硬件原理图，不改动 UART 引脚、射频器件、SDK 或分区方案。

### 状态定义

| 标记 | 含义 |
| --- | --- |
| 已有源码 | 有对应源代码；并不等于目标构建、真实控制器或整机已验收 |
| 已有主机证据 | [VALIDATION](../firmware/nrf54l15/VALIDATION.md) 记录主机测试；替身不验证实际射频、签名启动或掉电 |
| 待贯通 | 功能仍属交付要求，但三方适配、真实状态确认或业务契约尚有缺口 |
| 待目标/实板 | 缺少匹配 NCS 的目标构建、烧录及真实链路证据 |
| 后续扩展 | 原总需求标为 P2 的功能，不纳入当前能力承诺 |

## 2. 三方边界

```mermaid
flowchart LR
    P[管理平台：目标、规则、授权、制品、任务结果] --> E[ESP32-S3：持久任务、网络、去重、UART 编排]
    E --> N[nRF54L15：扫描过滤、连接、广播、Peripheral、UART DFU]
    N -->|原始观测与动作状态| E
    E -->|事件与结果| P
```

| 项目 | nRF 必须负责 | ESP / 平台必须负责 |
| --- | --- | --- |
| 扫描 | 执行完整过滤配置；保留 ADV/SCAN_RSP 原字节、地址上下文及单调时间 | 平台下发规则；ESP 独立执行本机扫描、跨无线电一对一合并及持久上报 |
| 连接 | 对授权任务给出的地址执行 Central/GATT；返回实际对端及动作结果 | 平台解析 16/32/128 位服务 UUID、匹配规则；ESP 复核新鲜可连接证据并持久化操作 |
| 广播 | 按目标启停、模式、原始 AD 与间隔执行；提供实际状态 | 平台定义 Beacon 或自定义内容；ESP 合并配置、检查能力、读回并报告 |
| Peripheral | 固定 INFO/RX/TX 服务，不透明接收与回复 | ESP 业务认证、路由与持久收件箱；平台业务规划合同见 [差距实施方案](gateway-alignment-development-plan.md)，C05 源码仍待实现 |
| 升级 | UART 分块、候选槽校验、试启动、当前镜像核验与确认 | ESP 下载验签、任务恢复、FIFO 接管、启动健康判断及硬件恢复；平台分离两类固件 |
| 本地配网 / ESP OTA | 不解析 Wi-Fi 凭据，不把 Peripheral RX 当管理授权 | ESP 提供手机/笔记本 SoftAP 配网、本地 OTA、平台远程配置及 OTA；需求完整保留 |

## 3. 硬件、版本、能力要求

| 原编号 | nRF 专项验收要求 | 当前边界 |
| --- | --- | --- |
| SYS-001、SYS-003 | nRF54L15-QFAA-R；TX P1.04→ESP18，RX P1.05←ESP17，RTS P1.06→ESP16，CTS P1.07←ESP15 | 已有 Oct29 板定义；后续原理图须独立逐网核对 |
| SYS-002 | 1,000,000 baud、8N1、RTS/CTS、3.3 V，业务 UART 不混入日志 | 已有源码；持续 DMA/流控负载待实板 |
| SYS-004 | 区分 `nrf_boot_id`、`link_id`、`msg_id`、`capture_seq`、`batch_seq`、`revision`、`op_seq`、`conn_id`、DFU session | 已有协议和源码；平台 `taskId/eventId` 不能替代它们 |
| SYS-005、SYS-007 | 固定 NCS v3.2.1；发布同步 VERSION、CMake 与 HELLO 版本；归档源码、配置和分区 | nRF 目标构建仍未完成；ESP 构建通过不能替代 nRF 构建 |
| SYS-006 | 自定义板默认 `GW_RADIO_VERIFIED=n`；完成时钟、FEM、功率与引脚核验后才启用 RF | 已有门控；不得为演示直接登记完整 RF 能力 |
| SYS-008 | 生效能力取平台授权清单、ESP 已实现适配、nRF HELLO 能力及当前资源条件的交集 | 平台能力声明与真实执行须一起验收 |

射频开启的当前 nRF 源码声明 bit1/2/3/4/9/10/11（`0x00000E1E`）。bit0 完整扩展扫描及 bit5～8 并行角色组合均不声明。bit12 仅在 DFU 构建及运行时双槽检查成功后声明；射频禁用不自动禁用具备条件的 DFU。规划 bit13 用于 Beacon V2 完整过滤能力，当前固件为 0；不得因文档已定义而登记为支持。

## 4. UART 与数据接管

| 原编号 | 必须满足的行为 | 当前边界 |
| --- | --- | --- |
| UART-001 | `00 + COBS(header+payload+CRC32C) + 00`；完整校验后分派；控制 payload ≤3072 B | 已有源码及主机证据 |
| UART-002 | HELLO 重建 `link_id` 保留同次 nRF 启动的配置、FIFO、连接和业务操作状态；芯片重启生成新 `nrf_boot_id` | 已有源码；恢复不能按“串口重连=芯片重启”处理 |
| UART-003、UART-004 | 单个在途控制；普通命令 500 ms，DFU 擦写/哈希 30 s；丢响应重试同 ID 同内容；明确 BUSY 后重试使用新 ID | 两端已有源码；TIME_PROBE 采样不使用重试结果 |
| UART-005 | 同 revision 同内容幂等；同 revision 不同内容或更旧配置拒绝；完整验证及应用成功后提交 | 已有 nRF 源码；ESP 实际状态校验缺口见 C03 |
| UART-006～UART-008 | 只有持久 `CUSTODY` 才释放 FIFO；`BUSY` 保留并退避；`FAULT` 保留并停止自动发送；RAM 接纳和 MQTT PUBACK 均不是持久交接 | nRF FIFO 与 ESP spool 已有源码；整机掉电验收仍待执行 |
| DATA-001、DATA-005～DATA-008 | nRF 先过滤→ESP 持久接管→ESP 定稿并上传→平台 `RAW_PERSISTED`；每一步责任分开 | 两端已有实现入口；不能用“ESP 待开发”的旧表述否认已有源码，也不能宣称实板闭环已通过 |
| DATA-002～DATA-004 | nRF 同一无线电的重复观测全部保留；仅 ESP 做不同无线电的一对一匹配；时间、地址或上下文不足时分开保留 | ESP 已有去重源码；nRF 不做跨芯片去重，不伪造 UTC |

三种批次 `0x20 BLE_BATCH`、`0x21 GATT_BATCH`、`0x22 PERIPHERAL_BATCH` 共用 FIFO、采集序号和批次序号，每批只包含一种类型。容量保留 128 条 / 32768 B、单批 1～32 条 / payload≤2048 B；背压不能静默改成丢旧记录或假 ACK。

## 5. 扫描与过滤

| 原编号 | 必须满足的行为 | 当前边界 |
| --- | --- | --- |
| SCAN-001 | 独立启停、主动/被动、10～10240 ms interval/window，window≤interval | nRF 与 ESP 对应源码已存在；按控制器 tick 量化 |
| SCAN-002 | RSSI、服务 UUID、Company ID 之间 AND；同列表内部 OR；最多各 128 个 UUID/公司 ID | 已有源码；关闭 RSSI 阈值用 -128，未知 RSSI 在启用阈值时不通过 |
| SCAN-003 | 最多 8 条有序 ALLOW/DENY；基础过滤后首条命中生效；含 ALLOW 而无命中则丢弃，否则放行 | 已有源码；规则不得跨 AD 结构拼接匹配 |
| SCAN-004、SCAN-005、SCAN-007 | 原始完整 AD 不改写；ADV 与 SCAN_RSP 独立记录、独立过滤；超限/不完整报告计数丢弃，完整但畸形 AD 仅使用可解析字段过滤 | 已有源码；真实空口验证待执行 |
| SCAN-006 | UUID 列表 AD 0x02～0x07、Service Data 0x16/0x20/0x21 正确识别 16/32/128 位；UART 统一规范顺序 16 B | nRF 匹配及 ESP 转换已有源码；平台保留 `wireBits`，不能从规范化文本反推原宽度 |
| SCAN-008 | 同一双路采集任务分别下发等价逻辑过滤目标并读回；两路独立配置保留，不隐式互相覆盖；量化分别检查 | 按 C10 统一语义；不因 nRF 能力而开放 ESP 未实现能力 |
| SCAN-009 | 完整扩展扫描仅在 bit0 声明后启用 | 后续扩展；规则可编码 EXT kind 不代表当前控制器接收该类报告 |
| SCAN-010 | 不把 bond identity 冒充空中 RPA；当前保守未知地址报告不得参与基于 MAC 的合并或地址自动连接 | 已有保守处理；原始 HCI 地址属于后续扩展 |

### 5.1 Beacon 字段掩码扩展要求（规划，待实现）

[Beacon 过滤规范](beacon-filter-spec.md) 是新增 `BFLT-*` 需求与验证向量的唯一编号入口；本节不重新分配编号。原 SCAN-002/003 的基础过滤和旧 `beaconRules` 保持不变，新功能显式选择 `beaconFilterV2`，以完整扫描组 revision 原子切换。

| 过滤项 | 统一要求 | 现状与开发责任 |
| --- | --- | --- |
| 字段选择 | `fieldMask` bit0名称、bit1 iBeacon UUID、bit2 RSSI、bit3 Major、bit4 Minor、bit5服务UUID；同规则所有选中字段 AND | 当前旧规则不能表达多 AD 字段 AND；nRF/ESP 共享 V2 引擎待实现 |
| 名称 | UTF-8 字节掩码，1～29 B；EXACT/PREFIX、COMPLETE/SHORTENED/ANY 显式选择；不能用字符数量当字节长度 | 旧原始 AD 掩码仅支持片段匹配；完整长度及名称来源策略是新增语义 |
| iBeacon UUID / Major / Minor | 只在同一完整 `0xFF` AD 内匹配；Data 必须恰25 B且固定头 `4C 00 02 15`；UUID16 B、Major/Minor为大端u16 | 旧单条24 B掩码已可匹配上述头与字段；V2类型化字段不能把多个AD里的值拼成一个Beacon |
| 服务 UUID | 与 iBeacon UUID 分开；按 AD 格式解析16/32/128位，规范为128位后逐字节掩码比较 | 不能把厂商UUID作为服务UUID自动连接证据；原精确列表过滤保留 |
| RSSI | 启用位与字段掩码独立；RANGE使用含端点的min/max；BITS对有符号dBm的补码字节按mask比较 | RSSI是接收元数据，不是AD内容；未知值不能匹配，BITS不等于强弱阈值 |
| 规则顺序 | 基础过滤先执行，再按V2列表首条完整匹配决定ALLOW/DENY；无命中沿用“有ALLOW拒绝，否则放行” | 不可把多条ALLOW当作同一规则的AND；不得同时启用非空旧规则与V2 |
| 报告范围 | REPORT每报告独立；ADV_SCAN_RSP_PAIR显式启用，仅legacy主动扫描、固定500 ms、唯一同radio/boot/revision/真实地址前序ADV | 未配对、超时、歧义、溢出均不放行；匹配后ADV与SCAN_RSP原字节分开上报，不合成一段AD |
| 能力与容量 | bit13及0x19～0x1B；完整body≤3070 B；最多8条规则，并预检实际编码总长 | 仅规划；原旧配置最大2755 B保持可用；V2超预算整体拒绝，不裁剪规则或扩大既有帧 |

名称可能在 SCAN_RSP，iBeacon字段在ADV。需要两者同时满足时必须显式选择配对范围；不能在REPORT模式偷偷复用历史名称。RSSI在配对范围只取主ADV。UART重握手不等于芯片重启，但候选缓存不得跨扫描revision、暂停/恢复或失效时间窗口匹配。

## 6. Central 与 GATT

| 原编号 | 必须满足的行为 | 当前边界 |
| --- | --- | --- |
| GATT-001、GATT-002 | 平台解析 UUID 后匹配授权规则；ESP 执行前复核当前 nRF 启动、地址类型、可连接 ADV 及证据时效 | 平台/ESP 已有实现；不能仅凭 UUID 即盲连，未知地址不能连接 |
| GATT-003 | 连接后读取实际地址、连接号、MTU、安全级别；连接引用绑定 radio、boot、role、conn_id | 已有源码；Central 与 Peripheral 的 conn_id 不能混用 |
| GATT-004 | 查主服务、特征发现、Read、Write Request、Notify/Indicate 订阅和取消；UUID 多匹配拒绝歧义 | 已有源码；一个在途动作、16 个发现句柄、4 条订阅 |
| GATT-005 | Notify/Indicate 进入 0x21 接管链；同步 Read 进入任务结果；断开后已入队数据仍可上报 | 已有 nRF / ESP 源码；不能把 Indication 的 ATT 确认等同 ESP CUSTODY |
| GATT-006、GATT-007 | 接受动作不等于完成；`op_seq`+查询终态；旧 boot/conn 引用失效；模糊副作用结果保留 UNKNOWN，不盲目重写 | 已有源码；断链、迟到回调和主机重启需整机验证 |
| GATT-008 | Central/Peripheral 共用一个连接名额；Central 期间扫描暂停；资源释放后按已提交目标恢复 | 已有源码；广播冲突返回 BUSY，由 ESP 显式编排 |
| GATT-009 | 初次配对、通用安全升级、多连接及更多并行组合 | 后续扩展；当前安全参数 1 仅使用预置 bond，不允许失败降级为明文 |

## 7. 对外广播和 Peripheral

| 原编号 | 必须满足的行为 | 当前边界 |
| --- | --- | --- |
| ADV-001、ADV-002 | 平台 `CONFIGURE.configuration.radios.nrf54.advertising` 控制启停；mode 保留三种现有枚举 | 已有三方入口；模式 2 额外要求 bit11 |
| ADV-003、ADV-004 | 支持 Beacon 与自定义 AD；AdvData/ScanRsp 各≤31 B；不可扫描模式禁止 ScanRsp | 以完整原始 AD 表达 iBeacon / 16、32、128 位 Service Data / 自定义内容，不新增 `beaconType` 线上字段 |
| ADV-005 | `intervalMs` 100～10240 ms，UART 单位 0.625 ms；报告量化后的真实间隔 | 频次约为 1000/intervalMs，不承诺精确空口周期 |
| ADV-006、ADV-007 | 只改内容在线更新，改模式/间隔重启；失败恢复旧完整目标，恢复失败 FAULT；成功才提交 revision | 已有 nRF 源码；两份 AD 的更新不保证空口原子性 |
| ADV-008 | 同时验证配置与 35 B `GET_ADV_STATUS`；目标 enabled 与实际 ACTIVE/STOPPED/PAUSED/FAULT 分开 | nRF 状态接口已实现；ESP 配置成功分支缺实际状态核对，列 C03 待办 |
| ADV-009～ADV-011 | 扫描目标与广播目标独立；已连接手机时广播 PAUSED；关闭广播不隐式断开手机；芯片重启默认停止、ESP 按最新目标恢复 | 已有 nRF 行为；恢复和冲突编排需联调 |
| ADV-012 | iBeacon measured power 是 1 m 校准 RSSI；接收元数据 `tx_power_dbm` 是 dBm；两者均不授予平台 RF 发射功率配置能力 | 当前没有服务器可调 nRF/FEM 发射功率字段，保留现有接口 |
| ADV-013 | 多广播集、扩展/周期广播、精确占空比 | 后续扩展 |
| PER-001、PER-002 | 固定 INFO/RX/TX UUID；RX Write Request 长度 1～min(512,MTU−3)，拒绝偏移、长写及 Write Command | 已有 nRF 源码 |
| PER-003、PER-004 | RX 进入 0x22，队列满返回 ATT 错；ESP 回复 0x0E 以 tx_seq 幂等；OK 表示本机受理 | 已有 UART 和 ESP 持久 INBOX；平台业务路径待贯通 C05 |
| PER-005、PER-006 | RX/TX 不自动授予配置/升级权限；解释前认证授权；DFU 拒绝新业务并等待连接对象回收 | 维护准入已有 nRF 源码；业务授权和平台路由待贯通，不能删除该功能 |

## 8. nRF UART OTA

| 原编号 | 必须满足的行为 | 当前边界 |
| --- | --- | --- |
| OTA-001、OTA-002 | `FIRMWARE_UPGRADE(component=NRF54)`；独立校验 manifest、型号/硬件、`MCUBOOT_SIGNED_BIN`、大小及完整文件 SHA | ESP 下载、验签、journal 与 nRF 执行器已有源码；旧“尚无 ESP 执行器”已失效 C01 |
| OTA-003、OTA-004 | 首次 SWD 安装 MCUboot+初始应用，项目专用签名密钥、双槽 swap-using-offset；只写候选槽 | 已有配置；未执行真实签名启动/交换测试 |
| OTA-005～OTA-007 | 维护准入、角色停止与连接回收；CHUNK 1～1024 B 连续 offset，首块≥32 B；同 boot 可重握手续传，下载中 nRF 复位后核对阶段，有效重传授权下从 0 BEGIN | 基础传输已有源码；按升级阶段区分 boot 变化的恢复缺口见 C11；接纳 offset 含 RAM 缓冲，不是断电检查点 |
| OTA-008～OTA-010 | FINISH 读回 SHA、TEST；REBOOT 后新 HELLO 与健康检查；VERIFY_RUNNING 当前槽 SHA→CONFIRM→STATUS 确认 | 两端已有代码；62 B DfuStatus 不能改布局或以版本字符串代替当前镜像核验 |
| OTA-011 | 试启动默认 120 s；到期不能补确认；挂死时由 ESP 硬件恢复触发 MCUboot 回退 | nRF 主循环超时路径存在；ESP GPIO8 主动恢复未实现，列 C04 |
| OTA-012 | 结果保留 component、版本、nrfBootId、bootConfirmed、imageSha256；不得覆盖 ESP 主固件版本 | 结果校验已有；独立可信 nRF 版本登记缺口见 C06 |
| OTA-013、OTA-014 | 活跃 DFU 排斥业务变更；ABORT 后按目标恢复；私钥离线、拒绝公开 SDK 示例私钥 | 已有入口；目标构建与密钥部署须单独验收 |
| OTA-015 | 强制防降级、密钥轮换、断电跨启动续传、bootloader 升级 | 后续扩展，不因 MCUboot 回退而宣称已支持 |

制品大小以实时 `DfuStatus.max_image_size` 为上限，再与 ESP 缓存及平台制品约束取最小值。当前布局的 600 KiB 仅是该构建结果，不能写成所有 nRF 硬件的固定容量。ESP 本地/远程 OTA 的 `component=GATEWAY` 保留独立状态机，不走 nRF DFU。

## 9. 安全、容量与可观测性

| 原编号 | 要求与归属 |
| --- | --- |
| SEC-001～SEC-003 | 平台身份、TLS、授权在 ESP/平台执行；nRF 按板内受信主机指令工作。预置 bond、公开 INFO 和 CRC 均不替代业务授权 |
| SEC-004、SEC-005 | 业务 UART 不含日志，日志不含 Wi-Fi 凭据、token、私钥；CRC32C 仅检测传输错误，不是认证或加密 |
| OPS-001、OPS-002 | 诊断包含版本、boot、状态、队列和丢弃/错误/重试；保留有界容量和实际状态，不只返回期望配置 |
| OPS-003、OPS-004 | 包速率、距离、功率、共存、丢包率用实板报告证明；发布归档 SDK、构建配置、分区、SHA、签名标识和验证记录 |

## 10. 已确认的贯通问题与关闭条件

以下编号与统一标准一致。逐项开发约定、依赖与关闭证据统一见 [差距实施方案](gateway-alignment-development-plan.md)；文档统一关闭的是语义歧义，源码或实测不能随之标为完成。

| 标准项 | 当前证据 | 关闭条件 |
| --- | --- | --- |
| C01 旧实现状态 | 核对前 README 称无 ESP 执行器，但 `gw_actions/gw_ota.c` 已实现 nRF 编排 | 本轮已修正文档；继续分别标识源码、主机证据、目标构建及实板状态 |
| C03 广播实际状态 | `gw_nrf_advertise` 仅 0x07/0x08 配置读回，`gw_actions_configure` 未核 0x0C | 后续补读取/核对 AdvStatus 与实际 PAUSED 例外；故障注入后不误报成功 |
| C04 挂死恢复 | `gw_nrf_init` 仅将 GPIO8 设为输入释放，未有主动 nRF 复位恢复编排 | 后续按匹配硬件验证复位控制、时序、boot 变化及回退；保留 FIFO 丢失审计 |
| C05 Peripheral 业务 | nRF 0x22 和 ESP INBOX 已有，平台业务扩展已在差距实施方案中确定；对应Schema/源码尚缺 | 完成三方业务契约及授权；不得把 RX 冒充广播 RawEvent 或 Central Notify |
| C06 nRF 版本登记 | NRF54 制品指定 `fromFirmwareVersions` 时平台缺独立可信当前版本 | 增加明确可验证的登记/证明路径；完成前拒绝无法证明的版本约束，不拿 ESP 版本代替 |
| C11 升级阶段与重启 | ESP `nrf_transfer` 对 boot 变化无条件调用 `nrf_trial`，下载中复位也可能被记为 ROLLED_BACK | 根据 journal 区分下载中断、READY 后试启动及实际回滚；有效授权下重传须从 0 BEGIN，不承诺同一任务必自动重传；试启动优先核验/确认，不能重新擦写 |
| C12 Beacon V2 | 旧规则没有名称完整长度、多字段AND和显式ADV/SCAN_RSP配对；RSSI只有原minRssi | 按Beacon规范及UART规划0x19～0x1B实现共享引擎、能力门控、容量与失败路径，完成BFLT向量及实板验收 |

## 11. 验收矩阵

沿用总需求 ACC 编号，增加下列 nRF 验收细化；新增字段掩码、短名/全名、iBeacon字节序、RSSI及配对场景采用 Beacon 规范的 BFLT 编号。本轮文档检查不改变历史测试结论。

| 原验收编号 | 必验场景 | 判定证据 |
| --- | --- | --- |
| ACC-01～ACC-03 | 最大帧、坏 CRC/COBS、错会话、同请求重放、RTS/CTS 阻塞与控制优先 | 协议向量、主机日志、实板串口抓取分别记录 |
| ACC-04、ACC-07 | 16/32/128 UUID、AND/OR、Beacon 首命中、ADV/SCAN_RSP、未知地址；授权/过期证据拒连 | 三方相同输入结果；手机/嗅探器实际报告 |
| ACC-05、ACC-06 | 双路相同帧一对一合并；CUSTODY 前后、RAW_PERSISTED 前后分别掉电 | 不重复生成事件、不越责任边界释放数据 |
| ACC-08 | Notify/Indicate、同步 Read、写入丢回复、旧 boot/conn、迟到回调 | 已受理与已完成分离；无法判定写入为 UNKNOWN |
| ACC-09、ACC-10 | 三模式启停、Beacon/自定义内容与间隔、热更新回滚、FAULT、手机连接 PAUSED | 配置、0x0C 实际状态及空口结果一致；C03 关闭后执行 |
| ACC-11 | 手机 RX/TX、INBOX 满、授权失败、业务回发、关闭广播保留连接、DFU 迟到连接 | 不假接管、不越权；C05 端到端业务待补 |
| ACC-13～ACC-16 | 错签名/型号/SHA/容量、跳块、下载中重启与试启动区分、VERIFY_RUNNING、超时/挂死回退 | 真实 MCUboot 和电源故障注入；C04、C06、C11 对应条件关闭 |
| ACC-17、ACC-18 | DK、自定义板、DFU 构建；RF/FEM、时钟、供电、真实负载 | 独立 nRF 构建日志与对应硬件测试报告，当前待完成 |
