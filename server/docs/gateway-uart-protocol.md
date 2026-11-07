# GW32I33V2 板内 UART 协议 1.1

**状态：设计稿。** 适用硬件为 2026 年 10 月 29 日 GW32I33V2 优化版，U1 为 ESP32-S3-WROOM-1-N8R8，U9 为 nRF54L15-QFAA-R。本协议覆盖 ESP 主机控制 nRF 的扫描、Beacon 过滤、GATT Central 和对外广播，以及 nRF 采集结果交接。协议 1.1 保留线上帧版本 `01`，以 HELLO 能力位启用新增操作。ESP 的网络、原生 BLE 和服务器任务见独立的服务器协议。[原理图 PDF](D:/chat/GW32_AT2401C_Optimized_20261029/GW32_Optimized_Final.pdf) · [最终网表](D:/chat/GW32_AT2401C_Optimized_20261029/evidence/final_v6.net:10277) · [ESP 与服务器协议](gateway-protocol.md)

## 串口参数

| 参数 | v1.0 约定 |
| --- | --- |
| 电气接口 | 板内 3.3 V CMOS，全双工，信号不反相 |
| 速率与格式 | **1,000,000 baud，8N1**，数据低位先发，TX 空闲高 |
| 流控 | 两端开启硬件 RTS/CTS，CTS 低允许发送 |
| ESP 外设 | `UART_NUM_1`，TX GPIO17、RX GPIO18、RTS GPIO15、CTS GPIO16 |
| nRF 外设 | `UARTE20`，TX P1.04、RX P1.05、RTS P1.06、CTS P1.07 |
| 恢复引脚 | ESP GPIO8 经 R9 接 nRF `nRESET`。正常通信时 GPIO8 保持高阻，该线也接 H2-4 |

ESP 通过 GPIO matrix 分配串口引脚。nRF 链路启用期间使用 HFXO，按 UARTE20 的外设时钟配置 1 Mbaud，RX 使用连续双缓冲 EasyDMA。该链路独占两端所选串口，控制台和启动日志走独立接口。1 Mbaud 在 8N1 下的理论线速为每方向 100,000 B/s。驱动缓冲、流控阈值和实板误码在联调中验证。[Nordic UARTE](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/uarte.html) · [Nordic UARTE 电气规格](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/_tmp/nrf54l15/autodita/uarte/parameters.elec_spec.html) · [Nordic P1 引脚分配](https://docs.nordicsemi.com/r/bundle/ps_nrf54l15/page/chapters/pin.html-dedicated_pins?contentId=ckWYM65jMv4DBKTI__JinQ) · [ESP-IDF UART](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html)

## 帧格式

线上字节为 `00 | COBS(帧头 + payload + CRC32C) | 00`。所有 `u16/u32/u64` 为小端，`i8` 为二进制补码。MAC、UUID、AD 内容按各自定义的字节顺序传输。偏移从 COBS 解码后的第一个字节开始。

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

其余位发送为 0。ESP 仅调用能力位覆盖的功能。HELLO 请求和响应均使用 `msg_id=0`。为兼容扫描版主机，nRF 启动后默认关闭广播和 GATT 订阅，只有收到新命令才启用。

## 建链与控制

ESP 恢复 Flash 队列和去重状态后发起 `HELLO_REQ`。nRF 每次启动生成 128 位随机 `nrf_boot_id`，扫描初始关闭。它收到 HELLO 后回显 `link_id`，报告本次 `nrf_boot_id`，排完 `HELLO_RSP` 才发送数据。相同 `link_id` 的 HELLO 可重试。新的 `link_id` 清除控制请求缓存，nRF 保留同一次启动内的扫描配置、采集 FIFO 和待确认批次。旧会话的帧一律忽略。ESP 收到匹配的 HELLO 响应后读回扫描配置，与持久目标配置对账。

每次只挂起一个控制请求。超时用同一 `msg_id` 和相同 payload 重发，nRF 缓存最近一条已处理请求及其完整响应。相同 ID 和内容只回放响应，相同 ID 但内容不同返回 `CONFLICT`，更旧的 ID 忽略。控制计数器耗尽前重新握手。初始定时为 TX 最多阻塞 200 ms，帧发完后等控制响应 500 ms，最多重发两次。三次失败后用新 `link_id` 重建链路，HELLO 初次间隔 250 ms，失败后翻倍到 2 s。空闲时 ESP 每 2 s 发一次 `GET_STATUS`。TX 超时时停止或排空当次驱动发送，再调度下一完整帧。两端均优先发送控制请求、响应及 ACK，数据帧之间允许控制流量通过。

`CMD_RSP.status` 定义为 `0 OK`、`1 BAD_PARAM`、`2 UNSUPPORTED`、`3 BUSY`、`4 CONFLICT`、`5 APPLY_FAILED`、`6 STALE`、`7 NOT_FOUND`。仅 `OK` 带命令专用结果。收到 `BUSY` 后 ESP 可退避 250 ms，再用新请求 ID 尝试。缺失响应的重试仍使用原 ID。

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

`GET_STATUS` 按顺序编码下表字段。计数器均从 nRF 启动起累计，`queued_records` 和 `queued_bytes` 包含待确认批次，字节数按记录在 BLE 批次中的大小计算。

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `uptime_ms`、`applied_revision` | 各 `u64` | 单调运行时间、已生效的扫描配置修订号 |
| `scan_state` | `u8` | `0` 停止，`1` 扫描中，`2` 控制器故障，`3` 暂停 |
| `queued_records`、`queued_bytes` | `u16`、`u32` | FIFO 当前占用 |
| `reports_seen`、`reports_filtered`、`dropped_full`、`dropped_incomplete`、`bad_rx_frames`、`batch_retries` | 各 `u64` | 分别统计扫描回调、过滤、队列满丢弃、截断或超限报告、串口坏帧和批次重发 |

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

`AdvConfig` 依次为 `revision:u64`、`enabled:u8`、`mode:u8`、`interval_ticks:u16`、`adv_len:u8`、`adv_data[adv_len]`、`rsp_len:u8`、`rsp_data[rsp_len]`。布尔值使用 0/1。模式 0 为不可连接且不可扫描的 legacy 广播，要求 `rsp_len=0`。模式 1 为不可连接、可扫描的 legacy 广播。间隔为 160–16384 个 0.625 ms tick。AdvData 和 ScanRspData 各最多 31 字节，发送前校验完整 AD 结构。

广播有独立 revision，出厂 revision 0 且关闭，默认模式 0、160 ticks、两段数据均为空。配置 revision 的同值重试、冲突、失败恢复与扫描相同。ESP 用 `enabled` 控制启停，用 `GET_ADV_CONFIG` 和实际状态确认生效。空口发射功率和 FEM 时序按实板固件能力验证。当前命令定义用于 Beacon 和扫描响应，BLE Peripheral 服务须由单独能力和预置 GATT 服务定义。

`RadioState` 依次为 `scan_state:u8`、`adv_state:u8`、`adv_revision:u64`、`conn_id:u32`、`conn_state:u8`。扫描和广播状态为 `0 STOPPED`、`1 ACTIVE`、`2 FAULT`、`3 PAUSED`。连接状态为 `0 NONE`、`1 INITIATING`、`2 CONNECTED`、`3 CLOSING`。`NONE` 时 `conn_id=0`，发起连接时分配 ID，连接成功后该 ID 可用于 GATT。固件初始设计上限为一个 Central 连接、一个广播集、一个在途 GATT 操作和四条订阅。连接发起期间暂停扫描，完成或失败后恢复。连接发起期间仅在验证过相应组合时保留广播，否则返回 `BUSY`。连接保持期间的角色并行仅在相应能力位已验证时启用。新请求与当前角色冲突时返回 `BUSY`，由 ESP 明确停止冲突角色后重试。[Nordic 控制器角色调度](https://github.com/nrfconnect/sdk-nrfxlib/blob/main/softdevice_controller/doc/scheduling.rst)

`GET_CONNECTION_INFO` 只读取 `CONNECTED` 的指定连接，否则返回 `NOT_FOUND`。实际地址类型使用 0 public、1 random，`security_level` 使用 `0 UNENCRYPTED`、`1 ENCRYPTED`、`2 AUTHENTICATED_ENCRYPTED`。

### GATT Central 连接与收发

每个动作请求在表中专用参数之前携带 `op_seq:u64`、`timeout_ms:u16`。超时为 100–30000 ms，从 nRF 受理时计算。ESP 在发送前将 `nrf_boot_id`、`op_seq`、请求及执行意图持久化，重连后先核对启动 ID。动作收到 `OK` 和回显的 `op_seq:u64` 仅表示已受理，随后以 `GET_OP_RESULT` 查询终态。固定地址和 UUID 沿用本协议字节顺序。

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
- `conn_id` 非零，启动周期内不复用。连接断开时句柄和订阅全部失效，重连后重新发现。主服务或目标特征有多个匹配时返回 `AMBIGUOUS`，未发现时返回 `NOT_DISCOVERED`。ESP 对设备、特征和允许的动作做策略检查。写入及订阅句柄必须来自当前连接的发现结果。写入长度为 1 至 `min(512, ATT_MTU−3)`，超过范围直接失败。
- nRF 重启时，ESP 以新启动 ID 清除连接映射。达到超时时间后取消底层操作或等栈回调收尾，收尾前后续动作返回 `BUSY`。迟到的连接成功必须主动关闭。已经交给 ATT 的写入如果结果因断链、复位或超时无法确定，则报告 `UNKNOWN`，ESP 不重发该物理写入。平台要求 READBACK 时，ESP 在同一连接内另做读取并验证内容。收到 Write Response 只表示 ATT 操作成功。[Zephyr GATT Client](https://docs.zephyrproject.org/latest/doxygen/html/group__bt__gatt__client.html)

服务器触发的自动连接仍使用 `GATT_CONNECT` 的地址和地址类型。ESP 在执行前核对上报事件所对应的 nRF 启动 ID、实际观测地址、可连接属性和单调采集时间。通过校验后，ESP 依次执行连接、`GATT_FIND_SERVICE`，以及规则需要时的 `GATT_DISCOVER`，最后用 `GET_CONNECTION_INFO` 核实实际对端和安全状态。UART 服务 UUID 固定按规范字符串顺序编码为 16 字节。广播中的 16 位和 32 位 UUID 先由服务器展开，nRF 按规范 UUID 执行 GATT 发现。[服务器自动连接规则](gateway-protocol.md#服务器解析与自动连接)

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

### GATT 通知和指示批次

`GATT_BATCH` 沿用 `count:u16`、`first_capture_seq:u64`、变长记录的批次头和 32 条、2048 字节上限。每条依次为 `capture_ms:u64`、`conn_id:u32`、`addr_type:u8`、`addr[6]`、`value_handle:u16`、`kind:u8`（0 Notify、1 Indicate）、`len:u16`、`value[len]`，长度为 0 至 `min(512, ATT_MTU−3)`。入队时保存地址和句柄，连接断开后仍可交付已入队的数据。BLE 广播和 GATT 共用一条 FIFO、同一对批次与采集序号及 `BLE_ACK`，每批只装同类型的连续记录。无线 ATT Indication 的确认由 BLE 栈处理，UART ACK 独立确认 ESP 接管。

### 接管确认与背压

1. nRF 使用有界 RAM FIFO，初始上限 128 条且编码后共 32 KiB，包含待确认批次。达到条数、下一条装不下、遇到记录类型边界或最早记录等待 50 ms 后发送，每次最多一个在途数据批次。队列满时保留旧记录，丢弃新报告并增加 `dropped_full`。串口 RX 和控制响应继续运行。
2. ESP 校验整个批次。新 `nrf_boot_id` 的首批要求 `batch_seq=1` 且 `first_capture_seq=1`。后续批次号与采集序号必须衔接。ESP 在同一个可断电恢复的 Flash 提交中写入该批次的待合并观测或已定稿事件，以及去重状态 `(nrf_boot_id, batch_seq, first_capture_seq, count, type, SHA-256(payload))`，然后返回 `BLE_ACK`。待合并观测已持久接管，可在重启后生成事件。旧版去重记录缺省为 `type=0x20`。Flash 恢复只认完整提交。该去重状态独立于平台 spool 回收，在同一 nRF 启动周期内始终保留最近已提交批次。
3. `BLE_ACK.status=0 CUSTODY` 表示已持久接管，或最近批次重放且类型与摘要相同，`retry_after_ms=0`。nRF 仅在会话与批次号均匹配的 `CUSTODY` 后回收记录。同一批号内容变化、批次向前跳号或有效帧携带结构非法的批次返回 `2 FAULT`，间隔字段为 0。nRF 保留队首，停止该批次的自动重发并继续响应诊断。已经落后于最近持久批次号的旧帧忽略。
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

固件联调覆盖 1 Mbaud 双向传输和 CTS 停顿、CRC 及超长帧恢复、最大配置与 Beacon 规则优先级、广播启停和 ScanRsp、连接和断线后的句柄失效、写入执行后丢响应、混合数据批次重放、Flash 提交前后 ESP 断电和 nRF 独立复位。分别记录 RX 溢出、Flash 提交延迟、FIFO 丢弃数、各角色组合的有效扫描和可持续采集速率。
