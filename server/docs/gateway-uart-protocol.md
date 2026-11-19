# GW32I33V2 板内 UART 协议 1.1

关联文档：[详细需求清单](../../docs/gateway-requirements.md) · [系统流程与对接指南](../../docs/gateway-integration.md) · [ESP ↔ 服务器协议](gateway-protocol.md)。本文是板内二进制字段和状态机的权威定义。

2026-11-19 三方统一采用 **[GW32-E2E-1.0](../../docs/gateway-end-to-end-standard.md)**；对应 [nRF 需求](../../docs/nrf54l15-requirements.md) / [开发文档](../../docs/nrf54l15-development.md) 与 [ESP 开发文档](../../docs/esp32s3-development.md)。本次文档对齐不改变帧版本、opcode、状态码或二进制字段。ESP `0.2.1` 已有持久化、采集、网络和升级执行源码及目标构建；尚缺的广播状态核对、硬件恢复、Peripheral 平台业务等见统一标准 C03～C05，不能把接口存在当成整机已经完成。

**状态：nRF 固件 0.2.0 的实现接口与 ESP 对接规范。** 本文按 [nRF54L15 源码](../../firmware/nrf54l15/src) 核对；主机测试不能替代 NCS 目标编译、烧录和实板验证，目前尚未完成后三项。适用硬件为 2026 年 10 月 29 日 GW32I33V2 优化版，U1 为 ESP32-S3-WROOM-1-N8R8，U9 为 nRF54L15-QFAA-R；其他原理图版本必须先核对网表。本协议覆盖 ESP 主机控制 nRF 的扫描、Beacon 过滤、GATT Central、对外广播、Peripheral 数据通道及 UART DFU。协议 1.1 保留线上帧版本 `01`，以 HELLO 能力位启用新增操作。ESP 的持久化、去重、联网和升级任务编排是对接方必须实现的责任，本文不表示已有生产级 ESP 固件。[固件构建与验证边界](../../firmware/nrf54l15/README.md) · [原理图 PDF（本地资料）](D:/chat/GW32_AT2401C_Optimized_20261029/GW32_Optimized_Final.pdf) · [最终网表（本地资料）](D:/chat/GW32_AT2401C_Optimized_20261029/evidence/final_v6.net) · [ESP 与服务器协议](gateway-protocol.md)

## 串口参数

| 参数 | 固定约定 |
| --- | --- |
| 电气接口 | 板内 3.3 V CMOS，全双工，信号不反相 |
| 速率与格式 | **1,000,000 baud，8N1**，数据低位先发，TX 空闲高 |
| 流控 | 两端开启硬件 RTS/CTS，CTS 低允许发送 |
| ESP 外设 | `UART_NUM_1`，TX GPIO17、RX GPIO18、RTS GPIO15、CTS GPIO16 |
| nRF 外设 | `UARTE20`，TX P1.04、RX P1.05、RTS P1.06、CTS P1.07 |
| 恢复引脚 | ESP GPIO8 经 R9 接 nRF `nRESET`。正常通信时 GPIO8 保持高阻，该线也接 H2-4 |

ESP 通过 GPIO matrix 分配串口引脚。nRF 链路启用期间使用 HFXO，按 UARTE20 的外设时钟配置 1 Mbaud；异步 EasyDMA 接收使用 3 个 256 字节缓冲轮换，后接 8192 字节软件环形缓冲。该链路独占两端所选串口，控制台和启动日志走独立接口。1 Mbaud 在 8N1 下的理论线速为每方向 100,000 B/s。驱动缓冲、流控阈值和实板误码在联调中验证。[Nordic UARTE](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/uarte.html) · [Nordic UARTE 电气规格](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/_tmp/nrf54l15/autodita/uarte/parameters.elec_spec.html) · [Nordic P1 引脚分配](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/chapters/pin.html-dedicated_pins?contentId=ckWYM65jMv4DBKTI__JinQ) · [ESP-IDF UART](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html)

## 帧格式

线上字节为 `00 | COBS(帧头 + payload + CRC32C) | 00`。所有 `u16/u32/u64/i32` 为小端，`i8/i32` 为二进制补码。字段连续编码，不包含 C 结构体对齐填充；布尔值严格为 0/1。MAC、UUID、AD 内容按各自定义的字节顺序传输。偏移从 COBS 解码后的第一个字节开始。

| 偏移 | 长度 | 字段 | 说明 |
| --- | ---: | --- | --- |
| 0 | 2 | `magic` | `47 57`，ASCII `GW` |
| 2 | 1 | `version` | `01` |
| 3 | 1 | `type` | 消息类型 |
| 4 | 2 | `payload_len` | `0..3072` |
| 6 | 8 | `link_id` | ESP 握手时生成的非零随机值 |
| 14 | 4 | `msg_id` | 握手为 0，控制请求 ID 或 BLE 批次序号从 1 开始 |
| 18 | N | `payload` | 长度等于 `payload_len` |
| 18+N | 4 | `crc32c` | 覆盖头部和 payload，结果按小端写入 |

CRC32C Castagnoli 使用反射多项式 `0x82F63B78`，初值和最终异或均为 `0xFFFFFFFF`。ASCII `123456789` 的结果为 `0xE3069283`。空分隔段直接跳过。连续非零段超过 3107 字节时丢弃到下一个 `00`。接收方验证 COBS、长度、magic、版本和 CRC 后才分派，坏帧计数并静默丢弃。未知消息类型计数并忽略。最大解码帧 3094 字节，含首尾分隔符的线上帧最多 3109 字节。

| `type` | 方向 | Payload |
| --- | --- | --- |
| `0x01 HELLO_REQ` | ESP → nRF | 空 |
| `0x81 HELLO_RSP` | nRF → ESP | 依次为 `nrf_boot_id[16]`、`capabilities:u32`、`fw_len:u8`、`fw_version[fw_len]`。版本字符串 UTF-8，1–63 字节。能力位见下表 |
| `0x10 CMD_REQ` | ESP → nRF | 依次为 `opcode:u8`、`args[]` |
| `0x90 CMD_RSP` | nRF → ESP | 依次为 `opcode:u8`、`status:u8`、`result[]`，回显请求 `msg_id` |
| `0x20 BLE_BATCH` | nRF → ESP | BLE 批次。`msg_id` 是同一 nRF 启动周期内的 `batch_seq` |
| `0x21 GATT_BATCH` | nRF → ESP | GATT 通知或指示批次。与 `BLE_BATCH` 共用 `batch_seq` |
| `0x22 PERIPHERAL_BATCH` | nRF → ESP | 远端写入本机 Peripheral RX 特征的数据，与前两种批次共用序号、FIFO 和 ACK |
| `0xA0 BLE_ACK` | ESP → nRF | 依次为 `status:u8`、`retry_after_ms:u16`，回显批次 `msg_id` |

| 能力位 | 含义 |
| --- | --- |
| 0 | 完整扩展广播接收 |
| 1 | Beacon 掩码过滤和扫描配置扩展 |
| 2 | GATT Central 连接、发现、读写 |
| 3 | GATT 通知、指示和订阅，依赖 bit2 |
| 4 | Legacy 对外广播及扫描响应 |
| 5 | 已验证的扫描 + 广播运行组合 |
| 6 | 已验证的扫描 + 已建立 Central 连接组合 |
| 7 | 已验证的广播 + 已建立 Central 连接组合 |
| 8 | 已验证的扫描 + 广播 + 已建立 Central 连接组合 |
| 9 | 按服务 UUID 查找 GATT 服务及读回连接信息，依赖 bit2 |
| 10 | 广播独立启停、专用状态查询和运行中载荷更新，依赖 bit4 |
| 11 | 可连接广播与本机 Peripheral 数据服务，依赖 bit4 |
| 12 | ESP 经 UART 传送 MCUboot 签名镜像，支持测试启动、确认和回滚；仅相应 DFU 构建声明 |

其余位发送为 0。ESP 仅调用能力位覆盖的功能。HELLO 请求和响应均使用 `msg_id=0`。为兼容扫描版主机，nRF 启动后默认关闭广播和 GATT 订阅，只有收到新命令才启用。

当前射频已启用的实现声明 bit1/2/3/4/9/10/11，即 `0x00000E1E`；bit0 和 bit5–8 始终为 0，完整扩展广播接收与角色并行属于后续扩展。GW32 自定义板默认 `CONFIG_GW_RADIO_VERIFIED=n`，不启动 BLE、射频能力为 0。bit12 独立取决于 DFU 编译配置及实际双槽检查结果，不因射频禁用而自动清除。能力表示该构建提供的接口，不能作为板级射频、吞吐或安全认证已验收的证据。扫描基础命令及只读诊断仍可用于管理；射频未就绪时不能启用扫描或广播。

## 建链与控制

ESP 恢复 Flash 队列和去重状态后发起 `HELLO_REQ`。nRF 每次启动生成 128 位随机 `nrf_boot_id`，扫描初始关闭。它收到 HELLO 后回显 `link_id`，报告本次 `nrf_boot_id`，排完 `HELLO_RSP` 才发送数据。相同 `link_id` 的 HELLO 可重试。新的 `link_id` 只清除普通控制请求缓存；同次启动内的扫描/广播配置、连接、GATT 动作缓存、Peripheral 通知缓存、DFU 会话、采集 FIFO 和待确认批次均保留。旧会话的非 HELLO 帧忽略；HELLO 是重新建链入口，不具备身份认证功能。ESP 收到匹配响应后读回配置及实际状态，与持久目标对账。nRF 芯片复位清除上述 RAM 状态，扫描与广播回到关闭的出厂配置；已配置的 Bluetooth bond 存储不属于这些 RAM 状态。

每次只挂起一个控制请求。超时用同一 `msg_id` 和相同 payload 重发，nRF 缓存最近一条已处理请求及其完整响应，错误响应也缓存。相同 ID 和内容只回放响应，相同 ID 但内容不同返回 `CONFLICT`，更旧的 ID 忽略。控制计数器耗尽前重新握手；命令 ID 与批次 ID 分属独立序列，可取相同数字。ESP 普通命令建议在帧发完后等待 500 ms，最多重发两次；三次失败后用新 `link_id` 重建链路，HELLO 初次间隔 250 ms，失败后翻倍到 2 s。空闲时 ESP 每 2 s 发一次 `GET_STATUS`。当前 nRF 向驱动传入 200 ms TX 超时，等待完成回调至多 300 ms，必要时再等 100 ms 取消回调；DMA 缓冲释放前不得启动下一帧，这不是端到端 200 ms 响应保证。两端均优先处理控制请求、响应及 ACK，数据帧之间允许控制流量通过。DFU 的长等待另见升级章节。

`CMD_RSP.status` 定义为 `0 OK`、`1 BAD_PARAM`、`2 UNSUPPORTED`、`3 BUSY`、`4 CONFLICT`、`5 APPLY_FAILED`、`6 STALE`、`7 NOT_FOUND`。仅 `OK` 带命令专用结果。收到 `BUSY` 后 ESP 可退避 250 ms，再用新请求 ID 尝试。缺失响应的重试仍使用原 ID。

以下命令表的请求参数长度不含 `opcode`，响应结果长度不含 `opcode/status`；所以 `CMD_REQ.payload_len=1+args_len`，`CMD_RSP.payload_len=2+result_len`。非 OK 响应固定为 2 字节 payload。除明确的变长字段外，缺少字节或多余尾字节均为 `BAD_PARAM`；未实现 opcode 为 `UNSUPPORTED`。`APPLY_FAILED` 后先读取配置、状态和诊断，不能假设控制器状态已恢复；GATT 动作的执行失败则通过 `GET_OP_RESULT` 表达。

| Opcode | 命令 | 请求参数 | `OK` 响应内容 |
| --- | --- | --- | --- |
| `0x01` | `SET_SCAN_CONFIG` | 完整 `ScanConfig` | 生效的完整 `ScanConfig` |
| `0x02` | `GET_SCAN_CONFIG` | 空 | 当前 `ScanConfig` |
| `0x03` | `GET_STATUS` | 空 | 下表所列状态快照 |
| `0x04` | `TIME_PROBE` | 空 | `nrf_rx_ms:u64`、`nrf_tx_ms:u64`，依次在接收有效请求和准备响应时采样 |
| `0x05` | `SET_SCAN_PROFILE` | 完整 `ScanProfile` | 生效的 `ScanProfile` |
| `0x06` | `GET_SCAN_PROFILE` | 空 | 当前 `ScanProfile` |
| `0x07` | `SET_ADV_CONFIG` | 完整 `AdvConfig` | 生效的 `AdvConfig` |
| `0x08` | `GET_ADV_CONFIG` | 空 | 当前 `AdvConfig` |
| `0x09` | `GET_RADIO_STATE` | 空 | `RadioState` |
| `0x0A` | `GET_CONNECTION_INFO` | `conn_id:u32`，要求能力位 9 | 依次为 `conn_id:u32`、实际 `addr_type:u8`、实际 `addr[6]`、`att_mtu:u16`、`security_level:u8` |
| `0x0B` | `SET_ADV_ENABLED` | `revision:u64`、`enabled:u8`，要求能力位 10 | 当前完整 `AdvConfig` |
| `0x0C` | `GET_ADV_STATUS` | 空，要求能力位 10 | 下述 35 字节 `AdvStatus` |
| `0x0D` | `GET_PERIPHERAL_STATE` | 空，要求能力位 11 | 下述 16 字节 `PeripheralState` |
| `0x0E` | `PERIPHERAL_NOTIFY` | `tx_seq:u64`、`conn_id:u32`、`len:u16`、`value[len]`，要求能力位 11 | `tx_seq:u64`，表示已交给本机通知发送队列 |
| `0x0F` | `PERIPHERAL_DISCONNECT` | `conn_id:u32`，要求能力位 11 | 空，表示已受理；用 `GET_PERIPHERAL_STATE` 确认断开 |
| `0x30` | `DFU_BEGIN` | `session:u64`、`image_size:u32`、`sha256[32]`，要求能力位 12 | 62 字节 `DfuStatus` |
| `0x31` | `DFU_CHUNK` | `session:u64`、`offset:u32`、`len:u16`、`data[len]`，1–1024 字节 | `DfuStatus` |
| `0x32` | `DFU_FINISH` | `session:u64` | `DfuStatus` |
| `0x33` | `DFU_STATUS` | 空 | `DfuStatus` |
| `0x34` | `DFU_ABORT` | `session:u64` | `DfuStatus` |
| `0x35` | `DFU_REBOOT` | `session:u64` | 空；受理后约 1000 ms 重启，响应不等于新镜像启动成功 |
| `0x36` | `DFU_CONFIRM` | 空 | 空；显式确认当前运行中的测试镜像，须先在本次启动成功执行 VERIFY_RUNNING |
| `0x37` | `DFU_VERIFY_RUNNING` | `image_size:u32`、`sha256[32]` | 空；读回当前主镜像并核对指定长度和摘要 |

常用固定长度如下；GATT 动作的长度在其专节列出。

| Opcode | `args_len` | `OK result_len` |
| --- | ---: | ---: |
| `01 / 02` | 16–2320 / 0 | 16–2320 |
| `03 / 04` | 0 | 71 / 16 |
| `05 / 06` | 19–2755 / 0 | 19–2755 |
| `07 / 08` | 14–76 / 0 | 14–76 |
| `09 / 0A` | 0 / 4 | 15 / 14 |
| `0B / 0C` | 9 / 0 | 14–76 / 35 |
| `0D / 0E / 0F` | 0 / 14+len / 4 | 16 / 8 / 0 |
| `30 / 31` | 44 / 14+len | 62 |
| `32 / 33 / 34` | 8 / 0 / 8 | 62 |
| `35 / 36 / 37` | 8 / 0 / 36 | 0 |

`GET_STATUS` 结果固定为 71 字节。计数器均从 nRF 启动起累计，`queued_records` 和 `queued_bytes` 包含待确认批次；字节数只累计三类记录的编码长度，不包含批次头、帧头和 CRC。

| 偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0、8 | `uptime_ms`、`applied_revision` | 各 `u64` | 单调运行时间、已生效的扫描配置修订号 |
| 16 | `scan_state` | `u8` | `0` 停止，`1` 扫描中，`2` 控制器故障，`3` 暂停 |
| 17、19 | `queued_records`、`queued_bytes` | `u16`、`u32` | FIFO 当前占用 |
| 23、31 | `reports_seen`、`reports_filtered` | 各 `u64` | 扫描报告与按规则丢弃次数 |
| 39、47 | `dropped_full`、`dropped_incomplete` | 各 `u64` | 记录入队失败；扫描截断、超限或回调资源冲突丢弃 |
| 55、63 | `bad_rx_frames`、`batch_retries` | 各 `u64` | UART 坏帧/结构错误/驱动溢出事件；数据批次重发次数 |

### 扫描配置与平台命令

`ScanConfig` 按顺序编码。完整配置最多 2320 字节，控制请求最多 2321 字节，成功响应最多 2322 字节。

| 字段 | 类型 | 取值 |
| --- | --- | --- |
| `revision` | `u64` | 平台 `desiredRevision`，0 保留给出厂配置 |
| `scan_flags` | `u8` | bit0 `enabled`，bit1 `active`，其余为 0 |
| `interval_ticks`、`window_ticks` | 各 `u16` | `16..16384`，单位 0.625 ms，`window_ticks ≤ interval_ticks` |
| `min_rssi` | `i8` | `-127..20 dBm`，`-128` 表示关闭该过滤 |
| `uuid_count` | `u8` | `0..128`，紧跟 `uuid_count × 16` 字节 |
| `company_count` | `u8` | `0..128`，紧跟 `company_count × u16` 的 Bluetooth Company Identifier |

出厂配置为 revision 0、停止扫描、被动模式、100 ms 间隔、50 ms 窗口、无过滤。ESP 将平台提供的整数毫秒按 `ticks = (ms × 8 + 2) / 5` 向下取整，最大量化误差 0.25 ms。UUID 可接受 4 或 8 位十六进制简写及标准 36 字符 UUID。简写展开至 Bluetooth Base UUID，再按标准 UUID 文本从左到右的 16 字节编码。列表去重后按 UUID 字节序或公司 ID 数值升序编码。[Zephyr 扫描参数](https://docs.zephyrproject.org/latest/doxygen/html/structbt__le__scan__param.html)

启用的过滤条件之间取 AND，同一列表内取 OR。UUID 匹配 16/32/128 位 Service UUID List（AD type `0x02..0x07`）和 Service Data（`0x16/0x20/0x21`）中的服务 UUID。厂商匹配 `0xFF` 的前两个数据字节，按小端解释。每个广播或扫描响应独立过滤。RSSI 未知且启用阈值时视为不通过。解析 AD 时先校验每段边界，只用可完整解析的字段匹配。

- `CONFIGURE` 由 ESP 校验任务身份、有效期并写入命令 journal，然后把部分字段合并至上次生效的完整配置。`scan` 和 `filters` 通过 `SET_SCAN_CONFIG` 下发，`upload` 由 ESP 本地应用。nRF 同 revision、同内容且扫描状态匹配时幂等成功，同 revision 内容不同或更旧则返回 `CONFLICT`。新 revision 先完整校验，再切换扫描。切换失败时恢复原配置并返回 `APPLY_FAILED`，恢复失败时还要进入 `scan_state=2`。`enabled=0` 只停止新增采集，既有队列继续发送。
- ESP 下发前验证本地上传参数，持久化待应用配置和任务阶段。nRF 回读匹配后，ESP 提交新的生效配置和上传设置。收到明确拒绝时结束该次配置意图，保留上次已提交配置。链路恢复后先用 `GET_SCAN_CONFIG`、`GET_STATUS` 对账，恢复已提交配置或继续仍有效的在途任务。实际生效状态无法证实时按平台规则报告 `UNKNOWN`。平台 `SUCCEEDED/VERIFIED` 以 nRF 配置回读、扫描状态和 ESP 上传设置均匹配为条件，`details.configuration` 原样保留该任务请求的配置对象，`reportedConfigurationRevision` 等于 `desiredRevision`。
- GPIO8 用于板内 nRF 恢复。复位后 ESP 重新握手、读回配置并恢复已提交的目标配置。平台任务与 GPIO 复位的对应规则见服务器协议。[平台配置与命令约束](gateway-protocol.md#命令执行日志与结果)

### Beacon 扫描过滤

`ScanProfile` 为 `scan_len:u16`、`ScanConfig[scan_len]`、`rule_count:u8` 和依序排列的规则，最多 8 条。每条按下表编码。配置体最大 2755 字节，控制请求最多 2756 字节，成功响应最多 2757 字节。

| 字段 | 类型 | 取值 |
| --- | --- | --- |
| `action` | `u8` | `0 DENY`，`1 ALLOW` |
| `kind_mask` | `u8` | bit0–3 分别对应四种报告 `kind`，至少一位置位 |
| `ad_type` | `u8` | 1–255，如 `0xFF` 厂商数据、`0x16` 16 位 Service Data |
| `offset` | `u16` | 从该 AD 结构的 Data 首字节起算 |
| `n` | `u8` | 匹配长度 1–24，要求 `offset+n ≤ 254` |
| `value[n]`、`mask[n]` | 字节串 | 逐字节比较 `(data & mask) == (value & mask)`，mask 至少一位非零 |

先执行基础 RSSI、服务 UUID 和厂商过滤，再按配置顺序查找第一条匹配规则。任一完整且同类型的 AD 结构匹配即命中该条，匹配结果不得跨两个 AD 结构拼接。无规则命中时，有 ALLOW 规则则丢弃，否则放行。这样可按 iBeacon 的厂商 ID、子类型、UUID、Major 和 Minor 过滤，也可按 Service Data 匹配 Eddystone 或自定义 Beacon。扫描配置和规则按同一个 revision 原子生效。规则生效时，旧 `SET_SCAN_CONFIG` 返回 `CONFLICT`，完整更新统一走 `SET_SCAN_PROFILE`。通过规则后的原始 AD 保持原样。

### 对外广播

`AdvConfig` 依次为 `revision:u64`、`enabled:u8`、`mode:u8`、`interval_ticks:u16`、`adv_len:u8`、`adv_data[adv_len]`、`rsp_len:u8`、`rsp_data[rsp_len]`。长度为 `14+adv_len+rsp_len`，原有二进制布局不变。模式 0 为不可连接且不可扫描的 legacy 广播，要求 `rsp_len=0`。模式 1 为不可连接、可扫描的 legacy 广播。新增模式 2 为可连接、可扫描的 legacy 广播，须有能力位 11，用于下面定义的本机 Peripheral 服务。间隔为 160–16384 个 0.625 ms tick，即 100–10240 ms；配置的是广播事件间隔，实际空中频次还受 BLE 随机延迟和调度影响，不是精确周期定时器。AdvData 和 ScanRspData 各最多 31 字节，发送前校验完整 AD 结构。

广播有独立 revision，出厂 revision 0 且关闭，默认模式 0、160 ticks、两段数据均为空。服务器决定启停、模式、内容和间隔；ESP 持久化目标后调用 nRF，nRF 不自行改变业务广播内容。配置 revision 的同值重试、冲突、失败恢复与扫描相同。`SET_ADV_ENABLED` 在当前完整配置上只替换 revision 和 enabled，其余字段保持；成功仍返回完整 `AdvConfig`。新 revision 必须递增，同值同内容幂等、同值不同内容或更旧 revision 返回 `CONFLICT`。不支持能力位 10 的旧固件继续用完整 `SET_ADV_CONFIG` 启停。

当前广播正在运行且仅原始 AD/ScanRsp 数据发生变化时，使用运行中数据更新；模式或间隔变化时停播后重新启动。完整参数校验通过后才接触控制器，应用成功后才提交 revision。SDK 分别设置 AdvData 与 ScanRsp，空口可能短暂跨越两个版本，不能将该更新描述为两包原子切换。失败时恢复旧的完整配置；恢复失败则进入 `FAULT` 并尝试停播，在无法确认已停止时拒绝新的扫描或连接。相同内容的新 revision 只更新配置版本，无需制造广播空档。已停止广播的内容更新只保存配置，后续启用时发出。

当前实现禁止扫描、Central 连接和广播同时运行。Peripheral 已连接或连接资源尚未回收时，新 revision 的 `enabled=1` 广播配置返回 `BUSY`，包括仅更新内容、间隔或 revision 的情况；可以提交 `enabled=0` 的配置，或等待断开后再更新。模式 2 因本机 Peripheral 连接暂停时，同 revision、同内容的重试可以成功，不要求此时在空口发包。已经提交的 revision 在 `FAULT` 下重试返回 `APPLY_FAILED`，恢复时使用新的 revision 并回读实际状态。

`GET_ADV_CONFIG` 回读目标配置，`GET_ADV_STATUS` 回读实际状态；`enabled=1` 不等于正在空口发送。`AdvStatus` 固定为下列 35 字节。计数器仅表示本机调用成功，不是接收方收到的广播包数。

| 偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0 | `revision` | `u64` | 最后成功提交的广播配置修订号 |
| 8 | `enabled` | `u8` | 目标启停值 |
| 9 | `state` | `u8` | `0 STOPPED`、`1 ACTIVE`、`2 FAULT`、`3 PAUSED` |
| 10 | `mode` | `u8` | 0、1、2 |
| 11 | `interval_ticks` | `u16` | 已提交间隔 |
| 13、14 | `adv_len`、`rsp_len` | 各 `u8` | 已提交两段载荷长度 |
| 15 | `last_driver_error` | `i32` | 最近广播驱动错误；成功应用后清零。负 errno 仅用于诊断，命令状态仍使用通用状态码 |
| 19 | `last_apply_ms` | `u64` | 最后成功提交新 revision 的 nRF 单调时间，初始为 0 |
| 27 | `live_updates` | `u32` | 成功提交的运行中载荷更新次数，不计回滚尝试 |
| 31 | `start_count` | `u32` | 控制器成功启动广播的次数，包含恢复/回滚启动 |

Beacon、厂商数据、Service Data 和网关身份公告均通过完整 AD 字节表达。iBeacon 的 measured power 字节表示该 Beacon 的校准 RSSI，不能作为 nRF/FEM 发射功率设置。当前命令不提供未经实板标定的天线口功率承诺；广播信道和射频功率按板级驱动配置执行。

`RadioState` 固定为 15 字节，依次为 `scan_state:u8`（偏移 0）、`adv_state:u8`（1）、`adv_revision:u64`（2）、`conn_id:u32`（10）、`conn_state:u8`（14）。扫描和广播状态为 `0 STOPPED`、`1 ACTIVE`、`2 FAULT`、`3 PAUSED`。连接状态为 `0 NONE`、`1 INITIATING`、`2 CONNECTED`、`3 CLOSING`。`NONE` 时 `conn_id=0`，发起连接时分配 ID，连接成功后该 ID 可用于 GATT。当前上限为一个广播集、一个在途 GATT 动作、16 个已发现特征句柄和四条订阅。Central 发起连接时暂停扫描，连接成功后仍保持暂停；只有连接失败或随后断开、资源允许时才按目标配置恢复扫描。广播未停止时发起 Central 连接返回 `BUSY`。角色并行须未来实现并声明相应能力位后再使用；当前冲突由 ESP 显式停止冲突角色后重试。[Nordic 控制器角色调度](https://github.com/nrfconnect/sdk-nrfxlib/blob/main/softdevice_controller/doc/scheduling.rst)

`GET_CONNECTION_INFO` 只读取 `CONNECTED` 的指定连接，否则返回 `NOT_FOUND`。实际地址类型使用 0 public、1 random，`security_level` 使用 `0 UNENCRYPTED`、`1 ENCRYPTED`、`2 AUTHENTICATED_ENCRYPTED`。

### 可连接广播与本机 Peripheral 服务

能力位 11 启用模式 2；旧固件或没有该能力的目标必须拒绝该模式。Central 与 Peripheral 合计最多一条连接，两种连接的命令和身份不可混用。`GET_RADIO_STATE` 中原有 `conn_id/conn_state` 保持 Central 语义，Peripheral 连接另由 `GET_PERIPHERAL_STATE` 查询。接入 Peripheral 后，广播实际状态为 `PAUSED`，目标配置仍可为 `enabled=1`；断开后只有目标仍启用且资源允许时才恢复。服务端关闭广播会取消自动恢复，但不隐式中断已经存在的连接；显式断连使用 `PERIPHERAL_DISCONNECT`。

服务 UUID 为 `7be00001-4b5a-4c31-9d91-6c2f9a0b5400`，固定特征如下。该服务是应用数据通道，服务器的广播配置不会动态改写 GATT 数据库。

| 特征 UUID | 属性 | 内容 |
| --- | --- | --- |
| `7be00002-4b5a-4c31-9d91-6c2f9a0b5400` | Read | `version:u8=1`、`capabilities:u32`、`uptime_ms:u64`，共 13 字节 |
| `7be00003-4b5a-4c31-9d91-6c2f9a0b5400` | Write | 远端写入的不透明应用数据，进入 `PERIPHERAL_BATCH` 交给 ESP |
| `7be00004-4b5a-4c31-9d91-6c2f9a0b5400` | Notify + CCC | ESP 通过 `PERIPHERAL_NOTIFY` 请求发送给当前已订阅的远端 |

INFO 的能力位是独立命名空间，与 UART HELLO 无关：bit0 表示 RX Write，bit1 表示 TX Notify，bit2 表示 UART 通知请求去重，当前值为 `0x00000007`。RX 只支持带应答的 ATT Write Request，数据长度为 1 至 `min(512, ATT_MTU−3)`；拒绝 Write Command、Prepare/Execute Write 和非零 offset。FIFO 接收成功才返回 Write Response，满队列返回 ATT `Insufficient Resources`。该应答只证明数据进入 nRF RAM，不能证明 ESP 持久接管或服务器收到。

`PeripheralState` 依次为 `conn_id:u32`（偏移 0）、`state:u8`（4，0 无连接、1 已连接、2 关闭中）、`addr_type:u8`（5）、`addr[6]`（6）、`att_mtu:u16`（12）、`subscribed:u8`（14）、`security:u8`（15），总计 16 字节。地址按显示顺序；安全等级沿用 0 明文、1 加密、2 认证加密。无连接时 16 字节均为 0，地址字段不是有效 public 地址。连接号在当前 nRF 启动周期内不复用，Central 与 Peripheral 各自分配连接号，ESP 必须同时绑定 `nrf_boot_id` 和角色。

`PERIPHERAL_NOTIFY` 数据为 1 至 `min(512, ATT_MTU−3)` 字节。非当前连接或未订阅时拒绝发送；非零 `tx_seq` 在同一启动周期内严格递增，最近受理序号及内容跨 UART 重握手保留。相同序号和内容只回放受理结果，同序号不同内容返回 `CONFLICT`，更旧序号返回 `STALE`。`OK` 只证明本机接受了通知，不证明对端应用接收或执行；需要业务确认时，应用数据应携带事务 ID，由对端经 RX 特征返回结果，ESP 不因 UART 响应丢失而分配新序号盲目重发。

新通知请求连接不存在时返回 `NOT_FOUND`，未订阅时返回 `CONFLICT`，前一通知仍在栈中或连接正在关闭/回收时返回 `BUSY`；最新已受理请求的相同重试即使连接已断开仍回放 `OK`。同一时间最多一个待完成通知。栈发送回调 10 s 内未完成时固件关闭连接并等待资源回收，不自动再次发送该通知；此过程不生成对端已收到的业务结果。`PERIPHERAL_DISCONNECT` 受理后异步断开，资源回收前广播可能仍为 `PAUSED`。

远端写入仅进入有界队列，不直接执行 Wi-Fi 配置、服务器凭据变更、GATT 控制或 OTA。链路可能为明文；ESP 必须在解释载荷前校验应用身份、权限和重放信息。未完成应用认证的连接仍可用于读取公开 INFO，但不具备网关管理权限。启停/模式/内容/间隔的权威来源仍是服务器经 ESP 下发的配置。

当前服务的 INFO、RX 和 CCC 不强制链路加密；“不具备管理权限”须由 ESP 应用协议落实，不是 nRF 自动认证。固件拒绝首次配对，也不提供配对、绑定导入/删除命令；已有 bond 的重加密与制造阶段的安全预置是不同流程。

### GATT Central 连接与收发

每个动作请求在表中专用参数之前携带 `op_seq:u64`、`timeout_ms:u16`。超时为 100–30000 ms，从 nRF 受理时计算。ESP 在发送前将 `nrf_boot_id`、`op_seq`、请求及执行意图持久化，重连后先核对启动 ID。动作收到 `OK` 和回显的 `op_seq:u64` 仅表示已受理，随后以 `GET_OP_RESULT` 查询终态。固定地址和 UUID 沿用本协议字节顺序。

含上述 10 字节前缀的 `args_len` 分别为：`10=18`、`11=14`、`12=46`、`13=16`、`14=18+len`、`15=19`、`16=18`、`18=30`（opcode 均为十六进制）。这些动作的立即受理结果均为 8 字节；下表“成功结果数据”只出现在后续 `GET_OP_RESULT` 的 `SUCCEEDED` 结果中。

| Opcode | 动作 | 专用参数，按顺序 | 成功结果数据 |
| --- | --- | --- | --- |
| `0x10 GATT_CONNECT` | 建立 Central 连接 | `addr_type:u8`（0 public、1 random）、`addr[6]`、`security:u8`（0 允许明文、1 使用已预置的加密绑定） | `conn_id:u32`、`att_mtu:u16` |
| `0x11 GATT_DISCONNECT` | 断开指定连接 | `conn_id:u32` | 空 |
| `0x12 GATT_DISCOVER` | 查找指定服务内的特征 | `conn_id:u32`、`service_uuid[16]`、`char_uuid[16]` | `value_handle:u16`、`ccc_handle:u16`（无则 0）、`properties:u8`（Bluetooth Characteristic Properties 位图） |
| `0x13 GATT_READ` | 读取特征 | `conn_id:u32`、`value_handle:u16` | `len:u16`、`value[len]`，最多 512 字节 |
| `0x14 GATT_WRITE` | 带 ATT 应答写入 | `conn_id:u32`、`value_handle:u16`、`len:u16`、`value[len]` | 空，成功表示收到 ATT Write Response |
| `0x15 GATT_SUBSCRIBE` | 订阅通知或指示 | `conn_id:u32`、`value_handle:u16`、`ccc_handle:u16`、`mode:u8`（1 Notify、2 Indicate） | 空 |
| `0x16 GATT_UNSUBSCRIBE` | 取消订阅 | `conn_id:u32`、`value_handle:u16`、`ccc_handle:u16` | 空 |
| `0x18 GATT_FIND_SERVICE` | 按 UUID 查找主服务，要求能力位 9 | `conn_id:u32`、`service_uuid[16]` | `start_handle:u16`、`end_handle:u16`，均非零且 start ≤ end |

`0x17 GET_OP_RESULT` 的请求只有 `op_seq:u64`，`OK` 响应依次为 `op_seq:u64`、`opcode:u8`、`state:u8`、`error:u8`、`att_error:u8`、成功结果数据。状态为 `0 RUNNING`、`1 SUCCEEDED`、`2 FAILED`、`3 TIMEOUT`、`4 UNKNOWN`。错误为 `0 NONE`、`1 LINK_LOST`、`2 PEER_REJECTED`、`3 NOT_DISCOVERED`、`4 AMBIGUOUS`、`5 SECURITY`、`6 LENGTH`、`7 DEADLINE`、`8 STACK`。仅在收到 ATT 错误时填对应 ATT 错误码，其余情况为 0。查询最新动作返回其当前状态，查询更旧序号返回 `STALE`，尚未受理的更大序号返回 `NOT_FOUND`。ESP 初始每 100 ms 查询一次，终态持久化后才发下一动作。

- nRF 在同一启动周期内保留最近受理动作的序号、请求和结果，UART 重握手保留该记录。相同序号和内容只返回已有受理信息，更旧序号拒绝，相同序号不同内容返回 `CONFLICT`。首个动作序号非零，此后严格递增。正在执行时收到新动作返回 `BUSY`。
- `conn_id` 非零，启动周期内不复用。连接断开时句柄和订阅全部失效，重连后重新发现。主服务或目标特征有多个匹配时，动作结果的 error 为 `AMBIGUOUS`；未发现时为 `NOT_DISCOVERED`。ESP 对设备、特征和允许的动作做策略检查。读取、写入及订阅句柄必须来自当前连接的发现结果，属性必须允许相应操作；未经发现的句柄在受理阶段返回 `NOT_FOUND`。写入长度为 1 至 `min(512, ATT_MTU−3)`，不支持长写入或无应答写入。读取可累计 ATT 分段结果，最大 512 字节。
- nRF 重启时，ESP 以新启动 ID 清除连接映射。达到超时时间后取消底层操作或等栈回调收尾，收尾前后续动作返回 `BUSY`。迟到的连接成功必须主动关闭。已经交给 ATT 的写入如果结果因断链、复位或超时无法确定，则报告 `UNKNOWN`，ESP 不重发该物理写入。平台要求 READBACK 时，ESP 在同一连接内另做读取并验证内容。收到 Write Response 只表示 ATT 操作成功。[Zephyr GATT Client](https://docs.zephyrproject.org/latest/doxygen/html/group__bt__gatt__client.html)

服务器触发的自动连接仍使用 `GATT_CONNECT` 的地址和地址类型。ESP 在执行前核对上报事件所对应的 nRF 启动 ID、实际观测地址、可连接属性和单调采集时间。通过校验后，ESP 依次执行连接、`GATT_FIND_SERVICE`，以及规则需要时的 `GATT_DISCOVER`，最后用 `GET_CONNECTION_INFO` 核实实际对端和安全状态。UART 服务 UUID 固定按规范字符串顺序编码为 16 字节。广播中的 16 位和 32 位 UUID 先由服务器展开，nRF 按规范 UUID 执行 GATT 发现。[服务器自动连接规则](gateway-protocol.md#服务器解析与自动连接)

`security=1` 只允许对预置 bond 发起加密，不能把加密失败降级为首次配对或明文连接。当前 UART 没有绑定管理或用户配对确认命令。UUID 解析与是否自动连接由服务器/ESP 决策，nRF 不独立运行服务器规则引擎。

## ESP 经 UART 升级 nRF

该通道接收已经经过服务器和 ESP 验证、本项目 DFU 构建生成的 MCUboot 签名 `zephyr.signed.bin`。默认非 DFU 构建不声明能力位 12；启用须具有 MCUboot、可用的双镜像槽和正确的板级验签公钥。服务器制品 manifest 的签名和 MCUboot 镜像签名是两层独立校验：ESP 校验 manifest、SHA-256、目标组件及型号，nRF 的 MCUboot 在启动时验证镜像签名。串口 CRC 与整包 SHA-256 不能替代签名。

当前升级范围仅为 nRF 应用，不含 MCUboot 本身、ESP 固件、任意分区或裸 bin。首次安装需通过调试器部署与签名密钥匹配的 MCUboot 和初始签名应用，之后才能走 UART 升级。构建采用已审查的 MCUboot swap-using-offset 双槽模式，上传只写 secondary 槽；必须显式提供部署方签名密钥，不能把 SDK 开发默认密钥作为生产信任根。镜像大小为 `32..max_image_size`，以上限查询值为准；当前官方 664 KiB 槽布局按固件保留空间后最多接收 600 KiB，改变分区后不能沿用该常数。具体构建入口见 [固件 README](../../firmware/nrf54l15/README.md)。

所有 DFU 命令沿用普通命令缓存和 status 定义。BEGIN、FINISH、ABORT 和 VERIFY_RUNNING 涉及 Flash 擦写或整镜像读回，ESP 对这些命令使用 30 s 响应等待，不套用普通命令的 500 ms 等待；主机参考工具对全部 DFU 命令统一使用 30 s。重试仍保留相同请求 ID 和内容。非零 `session` 由 ESP 为本次升级生成并记入任务 journal，绑定当前 `nrf_boot_id`、制品 ID、大小与摘要。`DFU_BEGIN` 在任何擦写前检查参数和镜像槽容量，再使扫描/广播静止；已有 Central 或 Peripheral 连接、其他升级会话，或当前运行镜像尚未确认时返回 `BUSY`，由 ESP 明确停止冲突业务。升级期间不启动新的无线业务。`DFU_ABORT` 仅清理候选镜像槽并撤销待升级标记，不改写当前正在运行的镜像；同一 ABORT 可幂等重试，重新升级须换用新的 session。

同一 session、大小和摘要的 BEGIN 重试保留进度，不重复擦除；该 session 换大小或摘要返回 `CONFLICT`，已经 ABORT 的 session 返回 `STALE`，处于 FAULT 时返回 `APPLY_FAILED`。关闭可连接广播时若恰有连接到达，BEGIN 可返回 `BUSY`，等待连接关闭和资源回收后才能擦写。DFU 活跃状态下，普通设置与动作命令返回 `BUSY`；允许的只读命令为 `02/03/04/06/08/09/0A/0C/0D/17`，原队列交接仍可继续。

`DFU_CHUNK` 必须连续发送，offset 等于已接纳偏移；首块至少 32 字节用于验证 MCUboot 头部，当前仅接受 flags 为 0 的未压缩、未加密签名应用镜像。整文件包含 MCUboot 头部和签名 TLV，SHA-256 覆盖传输文件的全部字节。只有最近一块的相同 offset、长度和字节允许重复确认且不重复写 Flash，更旧块返回 `STALE`，跳号或内容冲突返回 `CONFLICT`。ESP 在超时后先重试原 UART 请求 ID；重握手后先读 `DFU_STATUS`，根据 session、摘要和偏移继续。传输会话状态保存在 nRF RAM，已接纳偏移可能包含 Flash 写入缓冲中的字节；UART 重握手保留，nRF 复位后不保证续传，ESP 必须重新 BEGIN 并从 offset 0 开始。

`DFU_FINISH` 要求接收长度完整，并从候选 Flash 槽读回计算 SHA-256。校验通过后请求 MCUboot TEST 升级，状态进入 `READY`；同一会话 FINISH 可幂等重试。`READY` 只表示镜像已完整接收且测试启动已准备，MCUboot 的镜像签名验证仍在重启时执行。签名无效或不兼容的镜像不得被视为升级成功。

进入 READY 时待升级标记已经写入：即使还没收到 REBOOT，外部复位或掉电恢复也可能触发候选测试启动。需要取消时必须在重启前成功 ABORT 并读回状态。REBOOT 首次受理后约 1000 ms 复位，同请求或同 session 重试不延长倒计时；`REBOOT_PENDING` 拒绝 ABORT。写入、读回摘要或请求测试启动失败会进入 FAULT，保留诊断并继续占用无线维护状态；用原 session 成功 ABORT 后才能开始新会话并恢复目标无线配置。维护期间暂停的扫描/广播在 ABORT 后按目标配置尝试恢复，恢复是否成功仍须读回实际状态。

| 当前状态 | 可继续的动作 | 关键拒绝行为 |
| --- | --- | --- |
| IDLE / ABORTED | 新 session BEGIN；VERIFY_RUNNING 后 CONFIRM；STATUS | ABORTED 的旧 session 不可重新 BEGIN |
| RECEIVING | 连续 CHUNK；收齐后 FINISH；ABORT；STATUS | FINISH 未收齐为 CONFLICT；跨 session 的块为 NOT_FOUND |
| READY | 幂等 FINISH；REBOOT；ABORT；STATUS | CHUNK 为 BUSY；VERIFY_RUNNING / CONFIRM 为 BUSY |
| FAULT | ABORT；STATUS | 同 session BEGIN 为 APPLY_FAILED；新 session BEGIN 为 BUSY |
| REBOOT_PENDING | 幂等 FINISH / REBOOT；STATUS | ABORT、CHUNK、VERIFY_RUNNING / CONFIRM 为 BUSY |

表中动作仍要求参数正确且 session 匹配；所有状态允许 STATUS。BEGIN 新会话还要求当前运行镜像已确认。传输阶段掉电不会覆盖当前主槽；尚未 FINISH 的候选不应被当作已准备升级。nRF 重启后 RAM 状态回到 IDLE，ESP 需根据新启动 ID 和实际运行镜像重新判定任务进度。

`DfuStatus` 固定为 62 字节：

| 偏移 | 字段 | 类型 | 含义 |
| --- | --- | --- | --- |
| 0 | `session` | `u64` | 当前传输会话，空闲为 0 |
| 8 | `state` | `u8` | `0 IDLE`、`1 RECEIVING`、`2 READY`、`3 FAULT`、`4 ABORTED`、`5 REBOOT_PENDING` |
| 9 | `running_confirmed` | `u8` | 当前运行镜像是否已由 MCUboot 确认，0/1 |
| 10 | `accepted_offset` | `u32` | 下一块应使用的偏移 |
| 14 | `image_size` | `u32` | 本次镜像总长度 |
| 18 | `max_image_size` | `u32` | 当前构建的候选槽可接收上限 |
| 22 | `last_error` | `i32` | 最近底层错误，诊断用途 |
| 26 | `sha256` | 32 字节 | 当前会话目标摘要 |
| 58 | `confirm_remaining_ms` | `u32` | 当前未确认测试镜像剩余确认时间；不处于测试确认窗口时为 0 |

ESP 发 `DFU_REBOOT` 后主动重新发起 HELLO，验证新的 nRF 启动 ID、版本、UART 通信及业务自检，再用签名 manifest 对应文件的准确大小和 SHA-256 执行 `DFU_VERIFY_RUNNING`。该命令大小同样限定为 `32..max_image_size`，读回当前主镜像的相应字节，不能只比较固件版本字符串或信任先前下载的摘要。存在活跃 DFU 会话时 VERIFY_RUNNING 和 CONFIRM 返回 `BUSY`；VERIFY_RUNNING 的参数或读回校验失败会清除本次启动的验证标记并更新 last_error，但不把 DFU 状态改为 FAULT。VERIFY_RUNNING 成功后才发 `DFU_CONFIRM`，随后回读 `running_confirmed=1` 才报告升级成功；即使当前镜像已经确认，CONFIRM 也要求本次启动已有成功的 VERIFY_RUNNING，否则返回 `CONFLICT`。nRF 重启使这项确认前提失效，UART 重握手不使其失效。这样可以区分版本字符串相同但实际镜像不同、或回滚到旧镜像的情况。

测试镜像不自动确认，默认确认窗口 120 s；未确认镜像超过窗口后收到 CONFIRM 返回 `STALE`，不能用迟到确认取消回滚。窗口超时由 nRF 升级状态机触发复位并允许 MCUboot 回滚，若新镜像挂死而无法运行状态机，ESP 应通过板内复位线恢复。服务器任务过期、链路中断或证据不足时不能仅凭 FINISH/REBOOT 已返回 OK 报告成功。旧镜像回滚后需重新握手、读回配置并由 ESP 恢复已提交业务目标。

## BLE 数据批次

`BLE_BATCH` payload 为 `count:u16 | first_capture_seq:u64 | record[count]`。每批 1–32 条，payload 最多 2048 字节。每个 nRF 启动周期的 `capture_seq` 和 `batch_seq` 均从 1 开始。通过过滤并成功入队后才分配 `capture_seq`，同一批中的第 `i` 条序号为 `first_capture_seq+i`，`i` 从 0 开始。批次按 FIFO 生成，首次发送后批次号及 payload 冻结。序号到达上限前停止新增采集，排空后受控重启并生成新的 `nrf_boot_id`。

| 记录内偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| 0 | `capture_ms` | `u64` | nRF 扫描回调取得完整报告时的单调运行时间，单位 ms |
| 8 | `kind` | `u8` | `0` legacy ADV、`1` legacy SCAN_RSP、`2` extended ADV、`3` extended SCAN_RSP |
| 9 | `props` | `u8` | bit0 可连接、bit1 可扫描、bit2 定向，其余为 0。ADV 的 bit0 直接编码控制器报告的可连接属性，0 为否、1 为是。SCAN_RSP 的 bit0 为 0 |
| 10 | `addr_type` | `u8` | `0` public、`1` random、`0xFF` 匿名或未知 |
| 11 | `addr` | 6 字节 | MAC 显示顺序，`AA:BB:CC:DD:EE:FF` 编成 `AA BB CC DD EE FF`。匿名或未知时全 0 |
| 17 | `rssi_dbm` | `i8` | dBm，`-128` 未知 |
| 18 | `tx_power_dbm` | `i8` | dBm，`-128` 未知 |
| 19 | `primary_phy` | `u8` | `0` 未知、`1` LE 1M、`3` LE Coded |
| 20 | `secondary_phy` | `u8` | `0` 无或未知、`1` LE 1M、`2` LE 2M、`3` LE Coded |
| 21 | `sid` | `u8` | `0..15`，`0xFF` 未提供 |
| 22 | `ad_len` | `u16` | legacy `0..31`，extended `0..1650` |
| 24 | `ad_data` | `ad_len` 字节 | BLE 回调交付的完整 AdvData 或 ScanRspData，原字节顺序 |

ADV 与 SCAN_RSP 分别成记录。扩展广播只在 `capabilities` bit0 置位后发送完整重组报告，截断、超限或重组失败计入 `dropped_incomplete`。nRF 按下发规则过滤后，把地址、`props` 和原始 AD 交给 ESP。同一路收到的重复广播分别入队，双路观测去重由 ESP 完成。串口重发去重按采集身份处理。[Bluetooth SIG 广播与扩展数据](https://www.bluetooth.com/bluetooth-le-primer/)

当前固件不声明 bit0，仅提供 legacy 报告。Zephyr 可能将已绑定设备的空中 RPA 替换为 bond identity；为避免把解析后的 identity 冒充空中 MAC，当前实现对匹配已保存 bond identity 的扫描报告编码 `addr_type=0xFF`、全零地址，保留原始 AD 和属性。真实 identity 广播也可能被保守标为未知。服务器不能按此地址自动连接或参与基于 MAC 的双路配对；原始 HCI 地址或显式 identity 字段属于后续扩展。

### GATT 通知和指示批次

`GATT_BATCH` 沿用 `count:u16`、`first_capture_seq:u64`、变长记录的批次头和 32 条、2048 字节上限。每条依次为 `capture_ms:u64`、`conn_id:u32`、`addr_type:u8`、`addr[6]`、`value_handle:u16`、`kind:u8`（0 Notify、1 Indicate）、`len:u16`、`value[len]`，长度为 0 至 `min(512, ATT_MTU−3)`。入队时保存地址和句柄，连接断开后仍可交付已入队的数据。无线 ATT Indication 的确认由 BLE 栈处理，UART ACK 独立确认 ESP 接管。

`PERIPHERAL_BATCH` 使用同一批次头；每条记录的 24 字节头依次为 `capture_ms:u64`、`peripheral_conn_id:u32`、`addr_type:u8`、`addr[6]`、`channel:u16=1`、`kind:u8=0`、`len:u16`，随后为远端写入的原始应用字节。`channel=1` 固定表示上述 RX 特征，不能当作 Central GATT value handle。BLE、GATT、Peripheral 三种记录共用一条 FIFO、同一对批次与采集序号及 `BLE_ACK`，每批只装同类型的连续记录。ESP 须按 type 区分，不得把 Peripheral 写入伪装为 GATT Notify；已接管的 Peripheral 字节由 ESP 的应用认证/路由层处理。

### 接管确认与背压

1. nRF 使用有界 RAM FIFO，初始上限 128 条且编码后共 32 KiB，包含待确认批次。达到条数、下一条装不下、遇到记录类型边界或最早记录等待 50 ms 后发送，每次最多一个在途数据批次。队列满时保留旧记录，丢弃新报告并增加 `dropped_full`。串口 RX 和控制响应继续运行。
2. ESP 校验整个批次。新 `nrf_boot_id` 的首批要求 `batch_seq=1` 且 `first_capture_seq=1`。后续批次号与采集序号必须衔接。ESP 在同一个可断电恢复的 Flash 提交中写入该批次的待合并观测或已定稿事件，以及去重状态 `(nrf_boot_id, batch_seq, first_capture_seq, count, type, SHA-256(payload))`，然后返回 `BLE_ACK`。待合并观测已持久接管，可在重启后生成事件。旧版去重记录缺省为 `type=0x20`。Flash 恢复只认完整提交。该去重状态独立于平台 spool 回收，在同一 nRF 启动周期内始终保留最近已提交批次。
3. `BLE_ACK.status=0 CUSTODY` 表示已持久接管，或最近批次重放且类型与摘要相同，`retry_after_ms=0`。nRF 仅在会话与批次号均匹配的 `CUSTODY` 后回收记录。同一批号内容变化、批次向前跳号或有效帧携带结构非法的批次返回 `2 FAULT`，间隔字段为 0。nRF 保留队首，停止全部数据批次发送并继续响应诊断。当前没有清除队列 FAULT 的命令；新 HELLO 也不清除此状态，后续 ACK 不再回收队首。须由 ESP 记录交接故障、核查已持久数据，再决定是否复位 nRF；复位会丢失尚未接管的 RAM 记录。已经落后于最近持久批次号的旧帧忽略。
4. ESP spool 满或 Flash 暂时不可写时返回 `1 BUSY`，`retry_after_ms` 为 `250..5000`。nRF 按返回间隔重发同一批次。帧发完 500 ms 未收到有效 ACK 时按 250 ms 起始指数退避，最多退到 5 s，各次加入 0–100 ms 抖动。ESP 可重建会话，nRF 随新 `link_id` 重发原批次号及 payload。CTS 只保护接收缓冲，spool 背压通过 `BUSY` 表达。

同一 nRF 启动周期内，ESP 掉电前已经提交的批次可以在恢复后去重。nRF 掉电或复位会清除尚未接管的 RAM 记录，ESP 通过变化的 `nrf_boot_id` 识别采集连续性中断。已写入 ESP 的事件继续由平台 ACK 驱动回收。

## ESP 接管后的平台映射

ESP 仅在单条观测或双路匹配对定稿时分配平台 `eventId=<espBootId>-<sequence>`，其 `sequence` 属于网关事件流。待合并记录以采集身份交接，匹配对只生成一条平台事件。平台重发沿用已提交的 ID、时间和原始载荷。双射频来源标记和 GATT 通知使用服务器协议的 1.1 扩展。[ESP 双路去重](gateway-protocol.md#esp-双路采集去重) · [平台采集数据映射](gateway-protocol.md#双射频数据上报-11)

| 来源 | `protocolHint` | `payloadBase64` |
| --- | --- | --- |
| 通用 ADV 或 SCAN_RSP | `ble-ad-v1` | 原始完整 `ad_data` 的标准 Base64。平台按该 hint 和 `advertisement` 配置 `ble_ad@1.0.0` 或 PASSTHROUGH 路由 |
| 已登记的参考传感器 | `reference-sensor-v1` | 按设备配置匹配合法 Company ID 与厂商 AD 布局，提取内部完整 18 字节 BG 帧再编码。服务端解析器负责校验其内容和 CRC16 |

已登记设备用配置中的 `sourceDeviceId`，否则按报告地址生成 `ble:public:AABBCCDDEEFF` 或 `ble:random:AABBCCDDEEFF`。随机地址只标识本次观测来源。匿名报告省略该字段。完整报告中的畸形 AD 或超出平台 `ble_ad` 解析上限的记录仍可原样上报，由平台规则进入解析隔离或透传。平台的 `RawEvent` 不接收额外 JSON 属性。

ESP 建链后用 `TIME_PROBE` 对齐当前 `nrf_boot_id` 与 ESP 的单调时钟，取两端收发中点映射 `capture_ms`。仅使用未重试、扣除 nRF 处理时间后往返延迟在 `0..50 ms` 的样本。每 30 s 刷新。任一芯片重启或观测时刻距有效锚超过 60 s 时，该单调时间映射失效。有可信 ESP UTC 锚时才输出 `SYNCED` 和 UTC `capturedAt`。ESP 校时跳变后重新建立 UTC 锚。无可信 UTC 时输出 `UNSYNCED` 并省略 `capturedAt`，仍可用有效单调时间映射做板内去重。持久化后固定上行时间字段。

ESP 独立聚合平台批次，默认 64 条或 250 ms 刷新。受平台配置约束，实际每批最多 200 条、单条解码后最多 64 KiB、总原始字节最多 256 KiB，HTTP JSON 最多 1 MiB。ESP 通过现有 MQTT `gw/v1/{namespaceId}/{gatewayId}/telemetry` 或 HTTP `POST /api/v1/gateway-ingress/batches` 上报。只有匹配身份及 `batchId` 的 `RAW_PERSISTED` ACK 中逐条 `ACCEPTED` 或 `DUPLICATE` 才释放对应事件。[平台上报与 ACK](gateway-protocol.md#事件标识与应用-ack)

## 联调向量

以下帧使用同一 `link_id=0x0102030405060708`。广播批次和首条采集序号均为 1，报告时间为 nRF 启动后 1000 ms，类型为 legacy ADV，地址 `AA:BB:CC:DD:EE:FF`，RSSI `-60 dBm`，AD 为 `02 01 06`。随后 GATT 通知使用批次号和采集序号 2，连接号 1，句柄 `0x002A`，值 `10 20 00`。`SET_ADV_CONFIG` 样例使用控制请求 ID 1，启用 revision 1、间隔 250 ms 的不可扫描广播。线上十六进制包含首尾 `00`。

```text
HELLO_REQ
00 05 47 57 01 01 01 09 08 07 06 05 04 03 02 01 01 01 01 05 4F 42 A1 E8 00

BLE_BATCH
00 06 47 57 01 20 25 0A 08 07 06 05 04 03 02 01 01 01 01 02 01 02 01 01 01
01 01 01 01 03 E8 03 01 01 01 01 01 01 02 03 0A AA BB CC DD EE FF C4 80 01
03 FF 03 08 02 01 06 7D EC FD E0 00

BLE_ACK（CUSTODY）
00 06 47 57 01 A0 03 0A 08 07 06 05 04 03 02 01 01 01 01 01 01 01 05 6C 95 5D A1 00

GATT_BATCH
00 06 47 57 01 21 25 0A 08 07 06 05 04 03 02 01 02 01 01 02 01 02 02 01 01 01
01 01 01 03 F2 03 01 01 01 01 01 02 01 01 01 01 08 AA BB CC DD EE FF 2A 01 02
03 03 10 20 05 5B 7F EF C8 00

CMD_REQ（SET_ADV_CONFIG）
00 06 47 57 01 10 12 0A 08 07 06 05 04 03 02 01 01 01 01 03 07 01 01 01 01 01
01 01 02 01 07 90 01 03 02 01 06 05 9D 86 9C 40 00
```

没有 UTC 锚时，上述 BLE 记录可生成以下平台事件。

```json
{
  "eventId": "d89b2ff7b18d43f79272d157a148588c-1",
  "timeQuality": "UNSYNCED",
  "sourceDeviceId": "ble:public:AABBCCDDEEFF",
  "sequence": 1,
  "protocolHint": "ble-ad-v1",
  "payloadBase64": "AgEG"
}
```

待实板联调须覆盖 1 Mbaud 双向传输和 CTS 停顿、CRC 及超长帧恢复、最大配置与 Beacon 规则优先级、广播启停和 ScanRsp、连接和断线后的句柄失效、写入执行后丢响应、混合数据批次重放、Flash 提交前后 ESP 断电和 nRF 独立复位。还须验证 Peripheral 写入背压/通知超时/断线恢复、DFU 掉电/错误签名/相同版本不同制品/未确认回滚。分别记录 RX 溢出、Flash 提交延迟、FIFO 丢弃数和当前允许角色的有效采集速率；未声明的并行角色组合不属于已交付能力。
