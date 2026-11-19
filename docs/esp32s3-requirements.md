# GW32 ESP32-S3 嵌入式软件详细需求规格

| 项目 | 基线 |
| --- | --- |
| 文档版本 | 1.0 |
| 日期 | 2026-11-19 |
| 适用项目 | GW32I33V2，ESP32-S3 主处理器 + nRF54L15 蓝牙协处理器 |
| 对接基线 | nRF 固件 0.2.0、UART 协议 1.1（线上帧 version=1）、现有服务器消息契约 1.0/1.1 |

**状态：待实现的软件需求基线。** 当前仓库已有服务器和 nRF 固件，尚无 ESP32 固件执行器。本文用于开发拆分、接口对接和验收，不表示所列功能已经在 ESP32 上实现或验证。

本文包含 150 条带编号的软件需求、36 个验收用例，以及模块职责、持久化数据、恢复流程、开发阶段和待冻结事项。

实现路线见 [开发指南](esp32s3-development.md)，事务与恢复细节见 [持久化设计](esp32s3-persistence.md)，逐项测试步骤见 [验证计划](esp32s3-validation-plan.md)。这些开发文档不改变本文的需求编号和线上协议。

## 1. 范围、优先级和依据

ESP32-S3 必须完成两层协议之间的转换和可靠执行：服务器下发目标与任务，ESP 负责认证、持久化、资源协调和结果核验，再操作自身外设或 nRF；上行采集数据由 ESP 持久接管、跨射频合并并上传。

| 文档 | 本文使用方式 |
| --- | --- |
| [UART 协议](../server/docs/gateway-uart-protocol.md) | 帧、字节序、opcode、状态码、能力位、nRF 行为的权威定义 |
| [服务器协议](../server/docs/gateway-protocol.md) | MQTT/HTTP、任务、配置、上报、应用 ACK 和升级契约 |
| [消息 Schema](../server/internal/platform/assets/message-contracts.schema.json) / [OpenAPI](../server/internal/platform/assets/openapi.json) | JSON 字段、枚举及长度的可执行约束 |
| [系统需求](gateway-requirements.md) / [对接指南](gateway-integration.md) | 整机职责、时序及现有实现边界 |
| [nRF 工程](../firmware/nrf54l15/README.md) | 目标固件能力、构建及已知限制 |

P0：基础可靠性和集成前置条件；P1：本次交付范围内的业务功能，按配置启用；P2：后续扩展。P1 不等于可省略实现。尚未实现或未验收的能力不得登记为可用。

表中“必须”为验收要求；“建议”是实现起点，允许用等效方案满足验收；“待冻结”项必须在对应功能开发或发布前形成版本记录。内部模块名、状态名、日志字段只用于 ESP 实现，不新增服务器 JSON 字段或 UART opcode。

## 2. 系统职责与交付范围

```mermaid
flowchart LR
    S[服务器：规则、任务、持久化] <-->|MQTT / HTTPS| E[ESP32-S3]
    E --> J[任务日志、配置、事件 spool]
    E <-->|UART 1 Mbaud| N[nRF54L15]
    E <-->|本机 BLE| B[外部 BLE 设备]
    N <-->|BLE| B
    E <-->|STA / SPI Ethernet| U[上行网络]
```

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-SYS-001 | P0 | ESP 是服务器任务的唯一网关执行入口；nRF 不直接联网，服务器不直接打开板内 UART。 |
| ESP-SYS-002 | P0 | 支持以太网与 Wi-Fi STA；上行 MQTT/HTTPS 共用同一身份、任务 journal 和事件 spool，不因切换网络重新生成事件。 |
| ESP-SYS-003 | P0 | nRF 过滤并上传原始观测；ESP 负责跨芯片去重；服务器负责服务 UUID 解析、授权规则和自动连接决策。ESP 不能擅自建立另一套自动连接规则。 |
| ESP-SYS-004 | P1 | 实现 nRF 扫描、广播、Central/GATT 和 UART 升级的完整任务编排；实现 ESP 自身 BLE 扫描与广播。 |
| ESP-SYS-005 | P1 | 支持 ESP 主固件 OTA 与 nRF 组件 OTA，分开识别镜像、分区、版本、确认和回退；一次只运行一个升级任务。 |
| ESP-SYS-006 | P0 | 正确区分“命令受理”“驱动受理”“实际生效”“数据持久接管”“业务执行成功”；每项成功声明必须有相应证据。 |
| ESP-SYS-007 | P0 | 能力为签名设备清单、ESP 构建实现、运行时驱动、nRF HELLO 与已验收组合的交集。型号名不能替代能力协商。 |

本期不包括任意 Wi-Fi 帧注入、任意 Flash 写入、nRF bootloader 远程升级、通用 BLE 首次配对管理、多连接或未经验证的多角色并发。手机 Peripheral 云端业务与认证是接口缺口，见第 13 节。

## 3. 硬件抽象、SDK 与工程要求

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-BSP-001 | P0 | 继承已有 2026-10-29 板级基线：UART1 TX=GPIO17、RX=GPIO18、RTS=GPIO15、CTS=GPIO16；GPIO8 为 nRF 复位控制。这些是已有协议基线，不代表已复核后续 PCB。 |
| ESP-BSP-002 | P0 | GPIO8 正常保持释放/高阻，复位只按确认的板级电路拉低并释放；不能用推挽高电平驱动共享 nRESET。脉宽、上电状态和缓冲电路由板版本配置确定。 |
| ESP-BSP-003 | P0 | CH390H SPI 片选、时钟、数据、中断、复位及其他引脚必须从目标板网表冻结到独立 board 配置；本文不猜测未核对引脚。 |
| ESP-BSP-004 | P0 | 选定受支持的 ESP-IDF 稳定版本并锁定 tag/commit、toolchain、BLE Host、Ethernet 驱动及依赖校验值；CI 不跟随 latest 自动升级。本文不替工程指定未验证 SDK 版本。 |
| ESP-BSP-005 | P0 | 基线模块为 ESP32-S3-WROOM-1-N8R8；启动检测 Flash/PSRAM 与制造配置一致。PSRAM 仅作易失缓存，任何持久接管承诺必须由非易失存储完成。 |
| ESP-BSP-006 | P0 | 分区表必须给出双 ESP 应用槽、OTA 元数据、配置/凭据、journal、spool、隔离区和 nRF 镜像暂存的容量预算；不得挤占回滚镜像或已接管事件。 |
| ESP-BSP-007 | P0 | UART 驱动缓冲和 DMA 内存按所选 IDF/驱动要求分配，明确所有权和生命周期；不能把 nRF EasyDMA 实现直接等同为 ESP 驱动。 |
| ESP-BSP-008 | P1 | 交付源码、sdkconfig.defaults、分区表、依赖锁、构建脚本、烧录说明、制造配置格式、主机测试与硬件测试脚本；生产签名私钥不进入仓库。 |

ESP UART 的硬件流控和事件/缓冲接口以选定版本官方驱动为依据。[ESP-IDF UART](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html)

## 4. 软件模块与执行模型

以下为建议的模块边界；允许调整名称，但必须保留职责与单一所有者。

| 模块 | 职责 | 禁止行为 |
| --- | --- | --- |
| board / recovery | 外设初始化、版本引脚映射、受控 nRF reset | 初始化时无条件复位 nRF、抹除数据 |
| uart_transport | RX 流式解码、完整 TX 帧、RTS/CTS、坏帧恢复 | 多任务直接交叉写 UART；日志复用业务串口 |
| nrf_session | HELLO、能力、单控制请求、重试及 ID | 丢响应未解决就插入另一控制请求 |
| persistence | journal/spool 原子提交、恢复、回收、水位 | ACK 先于持久提交；因满盘擦除未交付数据 |
| capture / dedup | 观测归一化、时间映射、一对一配对、事件定稿 | 只按 MAC 去重；修改已定稿事件 |
| network / cloud | 网络候选、TLS、MQTT/HTTPS、应用 ACK | 把链路建立或 PUBACK 当持久成功 |
| command_executor | 任务校验、持久意图、资源协调、结果 | 从网络回调直接执行 GATT 写入或升级 |
| radio_manager | nRF 与本机 BLE 的目标、实际状态及恢复 | 自动抢占另一授权任务且不留记录 |
| ota_manager | 镜像验证、ESP/nRF 升级、试运行与恢复 | 同时升级两个控制器；下载完就报告成功 |
| diagnostics | 统计、故障、看门狗健康、脱敏日志 | 凭空填充未测量指标；异常时只重启不留证据 |

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-RT-001 | P0 | ISR、网络/BLE 回调只校验有限输入、复制/引用受控缓冲并入队，不等待云端或长时间 Flash 擦写。 |
| ESP-RT-002 | P0 | UART TX 由单一调度器串行输出完整帧；ACK/握手/控制优先于低优先级工作。DFU 等待响应期间 RX 与数据 ACK 必须继续运行。 |
| ESP-RT-003 | P0 | 每个队列有长度、字节上限、生产者、消费者、满队列行为和统计；不以无限堆分配吸收背压。 |
| ESP-RT-004 | P0 | Flash 事务、网络 I/O 和 UART 请求不形成循环锁等待；为存储回收保留控制/结果提交空间。 |
| ESP-RT-005 | P0 | 看门狗按任务真实进展喂狗；任务挂死进入可诊断恢复。nRF reset 不是处理普通网络断线的默认动作。 |

## 5. 启动、身份与恢复状态机

建议内部启动状态：`BOOT → STORAGE_RECOVERY → IDENTITY_READY → NRF_HANDSHAKE → RECONCILE → RUNNING`；网络可并行恢复。存储损坏进入 `STORAGE_FAULT`，不允许发送 CUSTODY；缺失身份进入受限配网状态，不接纳普通远程任务。

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-BOOT-001 | P0 | 每次 ESP 启动生成并持久记录新的 128 位 espBootId；恢复旧 spool 时保留每条事件原有 bootId/sequence/eventId。序号分配与事件提交同事务。 |
| ESP-BOOT-002 | P0 | 启动先恢复 journal、批次水位、待合并记录、结果队列和最后可用配置，再开始接管 nRF 数据。 |
| ESP-BOOT-003 | P0 | HELLO 后比较 nrf_boot_id；同启动仅重建会话，新启动才失效旧计数/连接并记录采集连续性中断。 |
| ESP-BOOT-004 | P0 | ESP 重启但 nRF 未重启时，先查询并对账旧 GATT/DFU 动作，避免盲目恢复新业务；关闭已失去 ESP 活连接登记的旧连接并生成相应链路事件。 |
| ESP-BOOT-005 | P0 | 若 nRF 为未确认试运行，优先完成镜像/健康检查及确认或回退，不让常规扫描恢复耗尽确认窗口。 |
| ESP-BOOT-006 | P0 | 身份、任务、事件状态不可读时不能恢复出厂后继续使用旧身份假装从未接管；应隔离错误、报告并执行受控维护。 |
| ESP-BOOT-007 | P0 | 未同步可信 UTC 时可采集并标 UNSYNCED；不能执行无法判断 expiresAt 的时间敏感任务。TLS 时间引导不得以关闭 CA/主机名校验替代。 |

## 6. UART 主机详细要求

### 6.1 固定接口与初始时间参数

| 参数 | 需求基线 |
| --- | --- |
| 串口 | UART1，1,000,000 baud，8N1，3.3 V，RTS/CTS |
| 帧 | `00 + COBS(header + payload + CRC32C) + 00`；整数字段小端，MAC/UUID 按协议 |
| 长度 | payload≤3072，解码帧≤3094，非零 COBS 段≤3107，含分隔符≤3109 字节 |
| 建链 | 非零随机 link_id；HELLO msg_id=0，业务控制从 1 开始 |
| 普通请求 | 发完整帧后等待 500 ms，最多重发两次；仍失败则重新握手并对账 |
| DFU 请求 | 30 s 等待；根据剩余任务/确认预算提前判定能否继续，不能无限阻塞 |
| HELLO 重试 | 初次 250 ms，失败翻倍，上限 2 s |
| 状态查询 | 无挂起控制请求时每 2 s GET_STATUS；升级长命令等待时不插入查询 |
| 明确 BUSY | 退避初始 250 ms，再以新 msg_id 重试，仍受任务期限和重试预算限制 |
| TIME_PROBE | 建链后及每 30 s 刷新；仅使用未重试、扣除处理后 RTT 0～50 ms 的样本 |

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-UART-001 | P0 | 流式接收半帧、多帧和空分隔；坏 COBS、CRC、长度、magic 或版本计数后丢弃，不执行命令，不因损坏传输帧直接发语义 FAULT。 |
| ESP-UART-002 | P0 | 硬件流控仅保护接收缓冲；Flash/spool 满使用 BLE_ACK BUSY。必须验证长 CTS 阻塞、FIFO overflow、driver buffer full 和恢复路径。 |
| ESP-UART-003 | P0 | 同一时刻最多一条挂起控制请求。超时重发相同 msg_id、相同 payload；nRF 仅缓存最近请求，未解决前禁止 GET_STATUS 等新请求覆盖缓存。 |
| ESP-UART-004 | P0 | 明确收到 BUSY 后使用新 msg_id；GATT 未受理动作保留原 op_seq。重新握手不能把已受理动作当作从未执行。 |
| ESP-UART-005 | P0 | 响应须匹配 link_id/msg_id/opcode，丢弃旧会话响应。完整 TX 完成后才启动响应计时；取消/排空旧 TX 后才能发送下一帧。 |
| ESP-UART-006 | P0 | 使用新 link_id 重新 HELLO 清除普通请求缓存，不清空 nRF FIFO、GATT op、Peripheral tx_seq、DFU session；分别查询对账。相同 link_id 的 HELLO 重试不清除普通缓存。 |
| ESP-UART-007 | P0 | UART ACK 使用批次消息号，与控制 msg_id 独立；不能混用两个序列。计数到上限前执行受控换会话/启动策略。 |
| ESP-UART-008 | P1 | 使用 HELLO 能力选择命令：基础扫描/增强过滤、广播管理、Peripheral、GATT service discovery、DFU；未支持时明确拒绝或使用语义等价的兼容路径。 |

### 6.2 命令路由

| ESP 工作 | UART opcode / type | 成功依据 |
| --- | --- | --- |
| 扫描/过滤 | 0x01/02 或 0x05/06 | 生效配置 + GET_STATUS |
| 广播 | 0x07/08，独立启停 0x0B，状态 0x0C | revision、目标内容与实际状态 |
| 状态/时钟/连接 | 0x03/04/09/0A | 有效会话中的实际快照 |
| Central/GATT | 0x10～0x18，0x17 查询结果 | op_seq 的终态与证据；受理 OK 不等于完成 |
| Peripheral | 0x0D/0E/0F，上行帧 0x22 | 连接/订阅状态；Notify 仅本机受理 |
| DFU | 0x30～0x37 | 当前镜像验证、明确确认及成功结果 |
| 数据批次 | type 0x20/0x21/0x22，ACK type 0xA0 | 持久提交完成后 CUSTODY |

## 7. 持久存储与数据接管

### 7.1 必须保存的数据

以下为内部语义字段，不要求采用相同文件名或数据库结构。

| 数据集 | 最少内容 | 回收条件 |
| --- | --- | --- |
| 身份与凭据 | namespaceId、gatewayId、型号/硬件版本、能力、凭据引用、信任根标识 | 授权制造/迁移流程，普通任务不能删除 |
| 配置快照 | 各组目标/生效 revision、完整快照、网络 last-good/candidate、事务阶段 | 新版本已验证提交且恢复不再依赖旧版 |
| UART 接管水位 | nrfBootId、batch_seq、first_capture_seq、count、type、payload SHA | 与当前 nRF 启动关联独立保留，不能随云端事件 ACK 删除 |
| 待合并观测 | 来源、原始载荷、控制器启动/采集身份、采集时刻、时间锚与误差、入队次序 | 定稿事件及消费标记原子提交后 |
| 事件 spool | schemaVersion、eventId、sequence、时间质量、capture、payload、重试/隔离状态 | 对应 RAW_PERSISTED 逐项接管或受控隔离维护 |
| 任务 journal | taskId、requestDigest、原命令、状态、意图、对应 UART op/session、证据 | 已交付终态仍需保留防重记录，保留期与服务器协商 |
| 结果 outbox | resultId、resultSequence、原始结果内容、交付状态 | RESULT_PERSISTED 匹配后回收内容，保留任务防重 |
| 升级 journal | component、制品 ID/版本/长度/SHA、验证状态、阶段、启动 ID、offset、session、恢复证据 | 成功/失败证据已持久交付且不再需要恢复 |

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-STO-001 | P0 | 提供可断电恢复的提交单元，支持事务 ID、长度/版本/校验和、完整提交标记与损坏隔离；不能假定多个 NVS key 的更新天然组成一个业务原子事务。 |
| ESP-STO-002 | P0 | UART 批次观测与接管水位必须同事务提交；校验完整批次后全量接管或全量拒绝，不能 ACK 半批。 |
| ESP-STO-003 | P0 | 配对/单条定稿时，把稳定 eventId、事件内容与消费待定观测的标记同事务提交；重启后不得一条变两条或两条都消失。 |
| ESP-STO-004 | P0 | 重放最近相同批次且 type/摘要一致时返回 CUSTODY，不再次生成观测；批号内容变化、前跳或结构非法返回 FAULT 并留证据。 |
| ESP-STO-005 | P0 | 新 nRF 启动首批要求 batch_seq=1、capture_seq=1。ESP 接管水位丢失而 nRF 仍从中途发批次时，不能任意接受为“新起点”，须进入数据连续性故障处理。 |
| ESP-STO-006 | P0 | 存储暂不可写/满盘返回 BUSY，retry_after_ms=250～5000。无法安全恢复的接管故障与正常暂时背压必须区分。 |
| ESP-STO-007 | P0 | nRF FIFO FAULT 无远程清除命令；HELLO 不解除。复位前必须核查已接管数据并记录可能丢失的 RAM 观测，不用无限握手假装恢复。 |
| ESP-STO-008 | P0 | GC/压缩过程中，先提交新的有效副本和索引再回收旧空间；保留结果、控制日志和故障记录的预留空间，避免满盘后无法报告满盘。 |
| ESP-STO-009 | P0 | 对永久拒绝数据提供有界隔离区及明确满区策略；不得静默删除尚未交付的原始数据，任何授权丢弃须审计原因和计数。 |
| ESP-STO-010 | P1 | 记录写入量、GC 次数、容量水位和最大提交延迟；用预期包率计算 Flash 寿命与离线时长，不把 RAM/PSRAM 容量算作离线持久容量。 |

建议小配置/凭据使用受保护的 NVS，连续事件采用经过掉电测试的追加日志或等效存储；选型不免除上述事务要求。官方说明 NVS 更适合较多小值，批量大载荷应单独评估。[ESP-IDF NVS](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html)

### 7.2 CUSTODY 提交流程

```text
收到有效批次
→ 校验启动身份、序号、结构与长度
→ 早于最近持久水位的旧批次忽略；最近批次相同则回放 ACK，内容冲突则 FAULT
→ 为观测和水位预留足够持久空间
→ 同事务写入观测 + 批次身份/摘要 + 提交标记
→ 存储层确认可恢复
→ 发送 CUSTODY
```

BLE 写入应答、串口收齐、RAM 入队、文件写函数返回、MQTT PUBACK 都不能独立替代“可断电恢复”的证明。

## 8. 双射频合并、时间和平台事件

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-DATA-001 | P0 | 将 nRF 与本机 BLE 观测映射到同一内部格式，保留原始 AD、报告类别、实际地址类型、PHY/SID、RSSI、连接属性和来源启动身份；未知字段用未知语义，不造值。 |
| ESP-DATA-002 | P0 | 只有不同无线电的观测可配对。匹配键包含地址及类型、ADV/SCAN_RSP、legacy/extended、SID、ADV 可连接属性和完整 AD；Hash 命中后仍比较原字节。 |
| ESP-DATA-003 | P0 | TIME_PROBE 建立 nRF→ESP 单调时间映射；时间差绝对值加误差估计≤50 ms 才可配对。误差考虑 RTT、量化和锚龄内漂移；任一芯片重启或观测 capture_ms 距有效锚时刻超过 60 s 后映射失效，不能用刚取得的新锚强配旧积压记录。 |
| ESP-DATA-004 | P0 | 每条观测从进入暂存队列起最多等待 150 ms；到期选时间差最小的未配对异源候选，同差选先入队者；每条最多配对一次。 |
| ESP-DATA-005 | P0 | 合并后以 nRF 为主记录，observedBy 固定为 [NRF54,ESP32]；同无线电重复、不同 AD/报告类型、未知地址、不可比元数据和迟到观测独立保留。 |
| ESP-DATA-006 | P0 | 待定区初始上限 256 条或 128 KiB；满时若 spool 有空间则单条定稿，否则向 nRF BUSY。本机 BLE 无 UART 重试保障，应计数并按既定策略暂停/丢弃新观测。 |
| ESP-DATA-007 | P0 | ESP 重启后失去旧单调映射的待定观测分别定稿，不沿用陈旧时间锚继续匹配；定稿必须防重。 |
| ESP-DATA-008 | P0 | 有可信 UTC 锚才填 SYNCED/capturedAt；否则 UNSYNCED 并省略 capturedAt。UTC 校时跳变后重建 UTC 锚；重传不改变已定稿事件的时间质量、采集时间、来源或载荷。 |
| ESP-DATA-009 | P0 | 定稿时分配 eventId=`espBootId-sequence`，持久保存完整事件；不同 schemaVersion 分批发送，协议升级不重写旧 spool。 |
| ESP-DATA-010 | P1 | nRF bond identity 被标为未知地址时保留原始数据，不用于按 MAC 合并或自动连接；不能自行补回看似合理的地址。 |

| 来源 | 平台映射 |
| --- | --- |
| ADV / SCAN_RSP | protocolHint=ble-ad-v1，payloadBase64 为完整 AD；1.1 capture 保存采集上下文 |
| GATT Notify / Indicate | ble-gatt-value-v1，只编码特征值，带捕获时 connectionRef 和句柄 |
| 链路关闭 | ble-gatt-link-v1，空 payloadBase64，linkReason 使用协议枚举 |
| 同步 GATT Read | 任务结果通道，不伪造通知事件 |
| Peripheral 0x22 | 独立本地应用 inbox；现有 RawEvent 无此类型，见第 13 节 |

服务器仍负责 AD 服务 UUID 的 16/32/128 位解析与自动规则；ESP 做传输校验和必要的目标转换，不改变这一职责。

## 9. 服务器连接、消息和 ACK

| 业务 | MQTT topic 后缀 | HTTP 等价入口 |
| --- | --- | --- |
| 原始事件 | telemetry / telemetry-ack | POST /api/v1/gateway-ingress/batches |
| 下行任务 | commands | GET /api/v1/gateway-ingress/commands |
| 执行结果 | command-results / result-ack | POST /api/v1/gateway-ingress/results |
| 状态 | status | POST /api/v1/gateway-ingress/status |

MQTT 前缀为 `gw/v1/{namespaceId}/{gatewayId}/`，QoS 1，retain=false。`result-ack` 是单数。控制命令、TaskResultBatch、GatewayStatus 仍使用 schemaVersion=1.0；采集 IngressBatch/IngressAck 按事件契约使用 1.0 或 1.1。

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-CLOUD-001 | P0 | HTTPS 验证 CA/主机名，TLS 至少 1.2；设备使用独立 token 或登记证书。MQTT 用户名为 gatewayId，订阅仅本设备命令/ACK topic。 |
| ESP-CLOUD-002 | P0 | MQTT/HTTP 只是传输适配器，同一任务和数据走同一防重路径；链路切换/重连不清空 journal、不重置在途事件身份。 |
| ESP-CLOUD-003 | P0 | 默认上报 64 条或 250 ms 刷新，实际服从 upload 配置；每平台批次≤200 条、原始总字节≤256 KiB、单条≤64 KiB、HTTP JSON≤1 MiB。batchMaxRecords 可到 1000 的内部缓冲必须拆批。 |
| ESP-CLOUD-004 | P0 | RAW_PERSISTED ACK 必须匹配 namespaceId/gatewayId/batchId，逐项 ACCEPTED/DUPLICATE 才回收；缺失 item、错误身份或无响应保留。 |
| ESP-CLOUD-005 | P0 | REJECTED_RETRYABLE 保留并遵守 retryAfterMs、退避和抖动；REJECTED_PERMANENT 移到可诊断隔离路径，不能紧密重试或伪装成功。 |
| ESP-CLOUD-006 | P0 | RESULT_PERSISTED ACK 按 namespaceId/gatewayId/taskId/resultId 匹配，不能要求不存在的结果 batchId；仅对应 item 为 ACCEPTED/DUPLICATE 才标已交付，拒绝/缺项/身份不符不回收。接管结果后仍保留任务终态防重记录。 |
| ESP-CLOUD-007 | P0 | GatewayStatus 只能发送 Schema 已定义字段；firmwareVersion 表示 ESP 主机版本，不能填 nRF 版本。nRF 版本和状态放到已允许的诊断/任务详情中。 |
| ESP-CLOUD-008 | P0 | GatewayStatus.observedAt 与 TaskResult.observedAt 必须是合法 RFC3339。UTC 未建立时暂存内部事实/单调时刻，延后构造此类信封；恢复时间后记录实际观察/对账时刻，不伪造掉电前精确 UTC，也不省略必填字段。 |
| ESP-CLOUD-009 | P0 | 对 1.1 capture 严格校验：未知 RSSI/功率省略；Schema 范围 -127～126；NRF54 ADV 带 connectable，SCAN_RSP 不带；legacy 不带 SID/secondaryPhy，primaryPhy 不用 LE_2M。 |
| ESP-CLOUD-010 | P1 | 断线重连必须有上限退避和抖动、认证失败分类及诊断；永久身份/证书错误进入受限恢复，不能无休止快速登录。 |

## 10. 任务 journal 与配置事务

### 10.1 任务执行

内部建议状态为 `RECEIVED → VALIDATED → ACCEPTED_DURABLE → EXECUTING_DURABLE → VERIFYING → TERMINAL_DURABLE → RESULT_DELIVERED`；无法证明的物理动作进入待对账状态。线上结果使用 Schema 中的 state/verification 枚举，不直接发送这些内部名字。

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-CMD-001 | P0 | 校验消息版本、身份、type、参数、能力、createdAt/expiresAt 和 requestDigest 格式后，原子保存 taskId/digest/命令，再报告受理。不得在持久意图之前执行副作用。 |
| ESP-CMD-002 | P0 | 相同 taskId/digest 返回既有进度或结果；相同 taskId 不同 digest 拒绝并告警。requestDigest 对应原始 TaskRequest，不是带服务器追加 desiredRevision 的最终命令重新计算值。 |
| ESP-CMD-003 | P0 | 执行前和可中断阶段边界检查 TTL；总 timeoutMs 是整个任务预算，不能为连接、发现、写入和读回各重新获得一份预算。 |
| ESP-CMD-004 | P0 | resultId/resultSequence/内容持久后才发送；ACK 丢失保持原结果重发。不能因为结果未送达再次执行物理动作。 |
| ESP-CMD-005 | P0 | 已发出但结果不可证实的 REBOOT/GATT_WRITE/OTA 等任务按证据对账，不能通用地“重试整个任务”；GATT 模糊写入报告 UNKNOWN。 |
| ESP-CMD-006 | P0 | 当前没有下发 CANCEL 命令，服务器仅可取消未投递任务；到期也不等于物理副作用被撤回。ESP 应停止尚未执行的后续动作，对不可逆或已发动作保留真实结果/UNKNOWN。 |
| ESP-CMD-007 | P1 | REBOOT 必须先保存意图并完成必要接管，真实 ESP 重启后以新 bootId 报告；不能只修改 bootId 字符串冒充重启。 |
| ESP-CMD-008 | P1 | 将资源冲突、能力不足、参数错误、超时、已过期和结果未知映射到既有服务端错误契约；内部新增错误不得随意作为线上枚举。 |
| ESP-CMD-009 | P0 | 与服务器每网关仅一个在途任务的约束一致；存在待对账 UNKNOWN 时保留证据并允许 DIAGNOSTICS，不在本地绕过占用继续执行新业务任务。 |

### 10.2 配置分组

| 配置组 | ESP 保存/执行方式 |
| --- | --- |
| network | last-good、candidate、试用阶段和各接口 probe 结果；应用 ACK 门槛见第 11 节 |
| radios.nrf54 扫描组 | scan/filters/beaconRules 合并完整快照，转 ScanProfile；独立 revision |
| radios.nrf54 广播组 | advertising 合并完整快照，转 AdvConfig；独立 revision |
| radios.esp32 扫描/广播组 | 相同 JSON 语义，调用本机 BLE；独立状态及能力 |
| 既有 scan/filters/upload | 兼容映射：scan/filters 到 nRF，upload 由 ESP 执行 |

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-CFG-001 | P0 | 一次扩展任务只改 network、一个 radios 目标或既有配置组；一个 radio 只改扫描组或广播组。数组/原始字节整体替换，未出现字段继承本组持久快照。 |
| ESP-CFG-002 | P0 | 合并后的完整配置再次校验能力和互斥；不能通过 patch 省略 mode 等字段绕过可连接广播能力。radios.<target>.filters 扩展支持 minRssi:null 清除阈值，旧版顶层 filters 不接受该 null；空数组用于清除相应过滤列表。 |
| ESP-CFG-003 | P0 | 持久保存目标及执行意图，应用后读回配置/实际状态，再提交本组生效快照。失败保留错误和真实实际状态，不能只改 reportedRevision。 |
| ESP-CFG-004 | P0 | 成功结果 details.configuration 回显原 patch，reportedConfigurationRevision 等于 desiredRevision；量化值和实际状态用允许的 details.effective 表达。 |
| ESP-CFG-005 | P0 | 服务器 configuration 的 desired/reported 是最近补丁，不是全量快照；设备 token 不能当管理员调用管理查询接口。Flash 清空后当前没有全量设备配置拉取接口，须重新配置/管理重放，不做虚构恢复。 |
| ESP-CFG-006 | P1 | 毫秒按既有公式转换到 0.625 ms ticks，统一整数舍入，读回报告实际值；revision 使用无损整数编码，避免 JSON 数值实现丢精度。 |

## 11. 以太网、Wi-Fi 与网络切换

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-NET-001 | P0 | 实现 CH390H SPI Ethernet 和 Wi-Fi STA 驱动适配，分别报告链路/IP/DNS/TLS/应用可达性；一个接口失败不重置整个事件系统。 |
| ESP-NET-002 | P0 | network.uplinkOrder 含 1～2 个不重复 ETHERNET/WIFI_STA，且都对应启用接口；默认以太网 DHCP 优先。 |
| ESP-NET-003 | P0 | 支持 DHCP 与静态 IPv4；静态地址、prefixLength 1～30、同网段网关、1～2 个 DNS 校验；ipv4 作为整个对象替换。 |
| ESP-NET-004 | P0 | STA 启用时 SSID 为 1～32 UTF-8 字节，WPA2_PSK/WPA3_SAE 按实际驱动支持；credentialRef 必须在本机安全凭据存储可解析，任务不携带明文密码。 |
| ESP-NET-005 | P0 | 网络变更先持久 last-good/candidate/意图，试用最长 120 s 且不超过任务有效期；所有需要验证的变更接口分别试用。 |
| ESP-NET-006 | P0 | probe 的 DNS、TLS、管理连接及结果 ACK 必须实际绑定被测候选接口；不能用旧接口收到 ACK 来证明新网络成功。 |
| ESP-NET-007 | P0 | 每个受测接口的 EXECUTING/NONE probe 结果使用稳定 resultId；全部收到匹配 RESULT_PERSISTED 的 ACCEPTED/DUPLICATE 后才原子提交 candidate。 |
| ESP-NET-008 | P0 | 提交前失联/超时/重启恢复 last-good；提交后重启恢复新配置；两种情况均保留数据、身份和结果。 |
| ESP-NET-009 | P1 | 正常接口故障按 uplinkOrder 切换已验证接口，保留在途上传并重试；设置防抖与恢复策略，具体时间作为实现参数实测冻结。 |
| ESP-NET-010 | P1 | DIAGNOSTICS NETWORK 的 wifiScan=OFF/PASSIVE/ACTIVE，最多返回 32 个 AP；按任务时间与地区限制执行。SoftAP 只用于授权现场配网窗口，不是长期开放管理入口。 |

Wi-Fi 与 ESP 本机 BLE 共享射频资源，必须启用所选 SDK 的适用共存机制并测量实际扫描空档、网络吞吐和延迟；芯片支持某组合不等于整机负载验收通过。[ESP-IDF 共存](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html)

## 12. 扫描、广播与无线资源协调

### 12.1 扫描与过滤

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-RAD-001 | P1 | nRF/ESP 扫描分别支持 enabled、active、intervalMs/windowMs，范围 10～10240 ms 且 window≤interval；本机驱动无法支持的精度/模式须明确拒绝或报告协议允许的量化。 |
| ESP-RAD-002 | P1 | 两路过滤语义一致：RSSI、UUID/厂商列表、最多 8 条 Beacon 掩码规则；完整解析字段参与匹配，保留原始 AD，不只上传解析值。 |
| ESP-RAD-003 | P1 | ESP 本机 ADV 与 SCAN_RSP 独立采集和过滤，使用捕获时元数据；不以强 RSSI、相同 MAC 或相同 UUID 代替跨源配对规则。 |
| ESP-RAD-004 | P0 | 当前 nRF 不声明并行扫描/广播/Central 组合，Central+Peripheral 共用一连接；冲突返回 BUSY 或按已有授权任务协调，不默默停止另一任务。 |
| ESP-RAD-005 | P0 | 保存停止/暂停原因和恢复目标。Central/DFU 优先使用 nRF 保留配置目标的暂停机制；显式停扫/启播/恢复的 revision 必须来自后续服务器配置任务，ESP 不得自行加一制造修订号。显式关闭后的旧目标不会自动复活。 |

### 12.2 广播

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-ADV-001 | P1 | 服务器可独立控制 nRF/ESP 的广播启停、模式、内容与间隔；沿用 CONFIGURE，不新增无 revision 的启停通道。 |
| ESP-ADV-002 | P1 | 支持 NONCONNECTABLE、SCANNABLE_NONCONNECTABLE、CONNECTABLE_SCANNABLE；最后一种在对应目标有 Peripheral 实现和能力时才接受。 |
| ESP-ADV-003 | P1 | iBeacon、16/32/128 位 Service Data、自定义 AD 均通过完整字节配置。mode 是连接属性，不是 Beacon 类型；不得自行增加未定义 beaconType JSON 字段。 |
| ESP-ADV-004 | P1 | advDataBase64/scanResponseBase64 解码各≤31 字节，AD 结构完整；不可扫描模式要求空扫描响应。严格校验 Base64/长度/UTF-8 名称字节数，不静默截断。 |
| ESP-ADV-005 | P1 | intervalMs 为 100～10240 ms；显示频率可由 1000/intervalMs 估算，但不能承诺精确空口每秒包数。iBeacon 校准 RSSI 与实际 RF 发射功率分开。 |
| ESP-ADV-006 | P1 | 对 nRF 使用 0x07/08；仅改 enabled 且支持 bit10 时可用 0x0B，同样有 bit10 才读 0x0C。旧广播固件用 0x08/0x09 读回配置及实际状态。配置相同也要处理更高 revision 的提交。 |
| ESP-ADV-007 | P1 | 内容在线更新失败按 nRF 状态处理；AdvData/ScanRsp 非空口原子更新，恢复失败为 FAULT。不得在两包切换间隙误报完整一致的新载荷。 |
| ESP-ADV-008 | P1 | enabled=true/PAUSED 且存在有效手机连接是正常状态；enabled=false 不等于断开手机。显式断连需单独授权和本地路由。 |
| ESP-ADV-009 | P1 | nRF 重启后按持久广播目标恢复，UART 重握手则先读回；ESP 本机广播同样记录目标、驱动实际状态和失败原因。 |

ESP 本机可连接广播需要自己的 GATT 服务与认证实现；nRF Peripheral 已存在不等于 ESP 同时具备该服务。该能力未实现时不登记 ESP32_BLE_PERIPHERAL。

## 13. GATT 与手机 Peripheral

### 13.1 Central/GATT

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-GATT-001 | P1 | 接收服务器 GATT_CONNECT/DISCONNECT/READ/WRITE/SUBSCRIBE/UNSUBSCRIBE，检查 radio、地址/类型、服务/特征、权限、连接引用和总时限。当前服务器自动连接目标固定 nRF。 |
| ESP-GATT-002 | P1 | 自动任务执行前再核验本地 nRF 可连接 ADV、控制器启动 ID 和 maxAgeMs；观测证据独立于 spool ACK 保留，按既有 30 s 保留窗口管理，任务不得超出其时效。 |
| ESP-GATT-003 | P1 | 建链后 0x18 查规则服务，按需发现特征，0x0A 核验实际对端/MTU/安全；成功连接无线链路本身不足以完成带服务要求的任务。 |
| ESP-GATT-004 | P0 | 持久 taskId/digest→nrfBootId/op_seq/请求/阶段映射；UART OK 后查询 0x17 到终态并落盘，再发下一个 GATT 动作。 |
| ESP-GATT-005 | P1 | 按协议算法派生 connectionRef：ASCII gw32-conn-v1 + radio 字节 + 控制器 16 字节 boot ID + 小端 conn_id 的 SHA-256 前 16 字节小写 hex；Central/Peripheral 本地 ID 分属不同角色。 |
| ESP-GATT-006 | P1 | 无 connectionRef 的 READ/WRITE 在同一任务预算内连接、发现、操作、断开；有引用则须匹配当前已登记活连接。断开后旧引用不能控制新连接，已入队通知仍保留原身份。 |
| ESP-GATT-007 | P1 | 写入只用 WITH_RESPONSE，长度为 1～min(512,MTU−3)，句柄来自当前发现；READBACK 按任务要求执行。当前服务器拒绝 APPLICATION_REPLY，不能因 Schema 有该枚举就宣布可用。 |
| ESP-GATT-008 | P0 | 写入已送出但断链/复位/超时无法证实，返回 UNKNOWN，不换 op_seq 重新写；同一 task 重投只恢复证据或重发结果。 |
| ESP-GATT-009 | P1 | BONDED_ENCRYPTED 只使用已有绑定，不能静默降级明文或触发首次配对；绑定预置、轮换/删除另列待冻结流程。 |
| ESP-GATT-010 | P1 | 有已登记连接时，在无待决控制请求的轮询机会及动作完成后查询 0x09 Central 状态，必要时读 0x0A；正常轮询初始周期 2 s。检测断链后持久生成 GATT_LINK_DOWN 并失效连接/订阅。0x03 不含连接状态；Peripheral 另用 0x0D，不能混用。 |

### 13.2 nRF Peripheral 不透明数据

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-PER-001 | P1 | 解析 0x22 的 peripheral_conn_id/channel/kind/长度并与 Central 数据隔离；channel=1 不是 Central characteristic handle。 |
| ESP-PER-002 | P0 | 0x22 与其他批次共用接管序列，先写可恢复本地应用 inbox 再 CUSTODY；手机 ATT Write Response 只证明写入 nRF RAM，不能表示 ESP/云端已执行。 |
| ESP-PER-003 | P0 | 公开 RX 字节不直接执行配网、GATT 写入或升级；必须经过应用认证、授权、抗重放和长度/速率限制。认证未实现时只受限存储/拒绝业务，不开放敏感操作。 |
| ESP-PER-004 | P1 | 回复检查当前连接和 CCC，持久业务事务 ID 与 tx_seq 后调用 0x0E；OK 只表示栈受理。需要业务送达时采用应用 ACK，不因 UART 丢响应重复业务。 |
| ESP-PER-005 | P0 | 当前 RawEvent 没有 Peripheral RX 类型，也没有服务器公开的 Peripheral 回复/断连任务。正式云端转发前必须版本化补齐业务 Schema、认证、ACK、路由和权限，不伪装为 GATT_NOTIFY。 |

因此“支持手机连接和 UART 数据转发”与“手机配置网关/云端交互已完成”必须独立验收。

## 14. nRF54L15 OTA 执行器

升级链路是服务器→ESP 网络下载→UART→nRF；不是 BLE 直连 DFU。采用项目 DFU 构建的 `zephyr.signed.bin`，不发送裸应用、merged.hex 或其他分区镜像。

### 14.1 阶段与成功证据

```text
任务验证 → 制品下载 → manifest验签/文件SHA/兼容性验证 → 持久升级意图
→ 准备无线维护、处理冲突连接 → BEGIN（保留目标并暂停无线）
→ 接管剩余FIFO、连续CHUNK → FINISH/READY → 确认FIFO已接管 → REBOOT
→ 新HELLO → 当前版本与健康检查 → VERIFY_RUNNING → CONFIRM
→ DFU_STATUS.running_confirmed=1 → 持久成功结果 → RESULT_PERSISTED
```

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-NOTA-001 | P1 | component=NRF54，检查 NRF54_UART_DFU_1 与 HELLO bit12、当前镜像已确认、槽容量及匹配 bootloader 前提；不能拿 ESP 固件版本判断 nRF 源版本。 |
| ESP-NOTA-002 | P0 | 验证服务器原始 manifest 字节的 Ed25519 签名，校验 keyId/组件/型号/硬件/版本及文件 SHA；不得解析再重新序列化后验签。MCUboot 的镜像签名是另一层验证。 |
| ESP-NOTA-003 | P1 | 将完整且校验通过的 nRF 镜像保存到受控暂存区，之后才 BEGIN；下载断线不影响旧 nRF。禁止下载未完成时以不可恢复流直接驱动擦写。 |
| ESP-NOTA-004 | P0 | 升级前持久保存业务恢复目标、任务和制品，按授权处理冲突连接。可由前置服务器配置停扫，也可由 BEGIN 进入保留目标的维护暂停；后者在传输期间持续接管原 FIFO，REBOOT 前必须排空。不能为临时停扫自造 revision；无法排空则失败/安全终止或记录显式允许的损失，不静默复位。 |
| ESP-NOTA-005 | P1 | BEGIN 使用非零 session，绑定 taskId/digest/artifact/长度/SHA/nrfBootId；运行时读 max_image_size，不固定按 600 KiB 假定所有构建容量。 |
| ESP-NOTA-006 | P1 | 每块 1～1024 字节、首块至少 32 字节，连续 offset；同请求重试字节完全相同。重握手先 STATUS，仅 RECEIVING 且 boot/session/size/SHA 匹配、偏移合法时续传；READY 进入启动核验流程，FAULT 用原 session 成功 ABORT 后才释放维护，ABORTED 重新开始必须换 session。 |
| ESP-NOTA-007 | P0 | accepted_offset 可含 nRF RAM 缓冲，不能当断电检查点；nRF 启动 ID 改变后重新判定实际镜像/阶段，未完成下载从 0 重新 BEGIN。 |
| ESP-NOTA-008 | P0 | FINISH/READY 已设置待试启动标记，意外复位也可能启动候选；若尚允许清理须成功 ABORT，不能仅停止传输。REBOOT_PENDING 无法 ABORT。 |
| ESP-NOTA-009 | P1 | 新启动后核对期望版本、UART 稳定性、存储/状态和既定健康项目，执行 0x37 对当前运行镜像完整文件长度/SHA 校验，再 0x36 确认，最后读 running_confirmed=1。 |
| ESP-NOTA-010 | P0 | 确认预算使用 confirm_remaining_ms，默认 nRF 窗口 120 s；30 s 长命令不能无限重试直到窗口外。任何健康/镜像失败都不确认，超时/挂死按可控 reset 回退。 |
| ESP-NOTA-011 | P1 | 成功 details 含 component=NRF54、firmwareVersion、32 位小写 hex nrfBootId、bootConfirmed=true、匹配制品的 64 位小写 hex imageSha256；结果先持久后发送。 |
| ESP-NOTA-012 | P0 | nRF 尚无独立已验证源版本登记，服务器对非空 fromFirmwareVersions 限制会拒绝；不能删除发布约束或改填 ESP 版本绕过。 |

健康检查必须在实现阶段冻结为可执行项目，例如：HELLO/能力/版本一致、关键状态请求成功、无重启循环、所需无线初始化成功、必要扫描/广播短测与恢复不报错。UART 没有“一键健康成功”命令；不得把建议清单当成已经由 nRF 自动执行。

## 15. ESP32-S3 自身 OTA

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-EOTA-001 | P1 | 自身升级 component=GATEWAY 或沿用省略组件的旧契约；不能自造 component=ESP32。与 NRF54 制品严格分流。 |
| ESP-EOTA-002 | P0 | 使用双应用槽和 OTA 元数据，将完整已验证的 ESP 应用写入非运行槽；本期不远程更新 bootloader、分区表、身份或任意数据分区。 |
| ESP-EOTA-003 | P0 | 固件与 manifest 分别按部署信任策略验证，兼容型号/硬件/源版本及大小先检查；nRF MCUboot 镜像不能作为 ESP 应用，反之亦然。 |
| ESP-EOTA-004 | P0 | 持久记录升级意图、候选版本/SHA、旧版本、任务和待发结果。重启前处理 nRF 当前动作与未接管数据，不能让主机重启重复 GATT 写入。 |
| ESP-EOTA-005 | P1 | 启用并验证所选 IDF 的应用回滚机制；试运行完成 Flash 恢复、身份、UART、配置和必要网络功能自检后才标有效，失败回退。确认策略/时限须冻结并测试，不能套用 nRF 的 120 s 为 ESP 默认值。 |
| ESP-EOTA-006 | P0 | 新旧应用需兼容共享存储格式；破坏性 journal/spool 迁移不得在试运行时使旧固件无法恢复。必须测试迁移中断与回退读取。 |
| ESP-EOTA-007 | P1 | 自身启动成功后更新 GatewayStatus.firmwareVersion 并回传符合契约的升级结果；服务器目前未像 NRF54 强制校验运行镜像 SHA，ESP 仍须自行完成镜像/启动证据，不能降低验收要求。 |
| ESP-EOTA-008 | P1 | 发布冻结 Secure Boot、Flash/NVS 加密、密钥备份、救援与安全版本策略；eFuse/防降级等不可逆制造动作单独执行，本需求编写不触发烧写。 |

官方 ESP OTA 文档说明双 OTA 槽、OTA 数据及应用试运行/回滚机制；具体 Kconfig/API 按项目锁定版本实现，不直接照搬浮动 stable 页的符号。[ESP-IDF OTA](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/ota.html)

## 16. 异常恢复与安全要求

| 故障 | ESP 必须采取的动作 |
| --- | --- |
| Wi-Fi/以太网断开 | 继续安全接管至持久容量上限，重连/切换后重发原事件；满盘后 BUSY |
| UART 控制响应丢失 | 原 ID/字节重试，仍失败换会话后按动作类型对账 |
| ESP 重启、nRF 未重启 | 恢复持久水位；先对账旧动作，再处理旧连接和业务恢复 |
| nRF 重启 | 失效旧连接/时间映射，记录连续性中断；重建配置和升级阶段 |
| Flash 满或写慢 | 保留已接管记录，暂停/背压新采集，控制与诊断仍可运行 |
| 批次摘要冲突 | 保存证据、FAULT；禁止用 HELLO 或清水位掩盖 |
| 配置应用失败 | 读回实际状态；保留原有效快照/明确恢复结果，不假报 revision |
| GATT 写入结果不明 | UNKNOWN，保留 journal，禁止重做物理写入 |
| OTA READY 后异常重启 | 优先查询当前启动/镜像，不能只按旧 download offset 继续 |
| 认证/证书失败 | 有界退避和受限恢复，不关闭 TLS 验证、不暴露凭据 |

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-SEC-001 | P0 | 凭据、签名私钥、完整认证会话和 Wi-Fi 密码不得进入日志、普通 diagnostics 或公共 BLE 特征；凭据引用与证书轮换需可审计。 |
| ESP-SEC-002 | P0 | UART CRC 是检错而非认证；网络及手机命令在到达 UART 路由之前完成授权，物理串口和调试接口纳入设备部署边界。 |
| ESP-SEC-003 | P0 | Base64/JSON/COBS/AD/GATT 长度在分配和复制前检查；深度、数量、速率和累计内存有上限，拒绝未知外部字段。 |
| ESP-SEC-004 | P0 | 恢复出厂必须区分业务配置重置与设备身份销毁，明确 journal/spool/凭据去留；不能把生产身份重新开放成无认证设备。 |
| ESP-SEC-005 | P1 | 每次 nRF reset、配置回滚、数据隔离/丢弃、升级确认/回退记录原因、任务关联和证据；没有任务时也保留本地恢复原因。 |

## 17. 性能、容量与诊断

| 编号 | 优先级 | 需求 |
| --- | --- | --- |
| ESP-OPS-001 | P0 | 输出实际 ESP/nRF 版本与启动身份、UART 能力、扫描/广播目标及实际状态、连接、队列水位、时钟质量、网络状态、升级阶段和最近故障。 |
| ESP-OPS-002 | P0 | 统计 UART 坏帧/重试/溢出、Flash 延迟/BUSY、各源观测/过滤/配对/独立定稿、spool/隔离数、网络应用 ACK 延迟、重启原因。queue FAULT 由 ESP 保存原因，不能假设 nRF GET_STATUS 有独立字段。 |
| ESP-OPS-003 | P0 | 建立资源预算：内部 RAM、PSRAM、任务栈、UART 缓冲、TLS/BLE 峰值、Flash 可用空间、nRF 暂存与写放大；目标构建后用 map 和压力测试验证。 |
| ESP-OPS-004 | P0 | 离线容量按 `可用持久字节 / 实际平均记录占用` 和峰值输入速率计算；必须包含索引、校验、提交、GC 及预留开销，不按原始 AD 字节简单估算。 |
| ESP-OPS-005 | P1 | 建议进行至少 72 小时混合压力测试，覆盖双扫描、Wi-Fi 重连、UART 背压与存储回收；该时长是新增验收建议，不是已通过结果。 |
| ESP-OPS-006 | P1 | 产品包率、最大离线时长、允许采集空档、网络切换时间、Flash 寿命及重启恢复时间须用实际存储/镜像/板卡测量后冻结，当前不写虚构性能承诺。 |

## 18. 验收测试清单

每个用例记录 ESP/nRF/服务器提交号、SDK、板版本、配置、输入、断电/丢包点、日志和结果。单元测试、模拟测试、目标编译和实板测试分别标记，不相互替代。

| 编号 | 用例 | 通过条件 |
| --- | --- | --- |
| ESP-AT-01 | UART 黄金向量、半帧、粘帧、最大帧和损坏帧 | 解码与 nRF 一致，无越界和错误执行 |
| ESP-AT-02 | CTS 长阻塞、RX overflow、TX 中止 | 无缓冲复用/帧交叉，恢复后可握手与控制 |
| ESP-AT-03 | 丢控制响应、重复/冲突命令、明确 BUSY | 同请求无重复副作用，BUSY 后新 ID，不覆盖待决缓存 |
| ESP-AT-04 | Flash 提交前仅 ESP 断电，nRF 保持运行 | 未发 CUSTODY，nRF 保留批次，重放可重新接管 |
| ESP-AT-05 | 提交后、CUSTODY 前仅 ESP 断电，nRF 保持运行 | 重放只确认，不生成重复观测/事件 |
| ESP-AT-06 | CUSTODY 后、云端 ACK 前断电 | 已接管数据恢复并按原身份上传 |
| ESP-AT-07 | RAW_PERSISTED 后 GC 中断 | 已接管数据不重复物理处理，其他未交付记录不丢失 |
| ESP-AT-08 | 水位丢失、批次跳号/摘要冲突 | 进入可诊断故障，不任意接受新起点 |
| ESP-AT-09 | 50 ms 边界、多候选同差、150 ms 到期 | 严格按时间误差、最小差和入队次序一对一匹配 |
| ESP-AT-10 | 同源重复、不同 AD/类型、未知地址、迟到 | 保留独立事件，不错误合并 |
| ESP-AT-11 | 任一芯片重启、时间锚过期、UTC 跳变 | 失效正确锚，旧事件不改时间/来源 |
| ESP-AT-12 | 待定区/spool/隔离区满，Flash 慢写 | 有界内存和背压，ACK 不提前，诊断可用 |
| ESP-AT-13 | MQTT PUBACK 与应用 ACK 分别丢失/伪配对 | 仅合法应用逐项 ACK 才回收 |
| ESP-AT-14 | 1.0/1.1 混合旧 spool 重放 | 原 schema/ID/内容保持，分批正确 |
| ESP-AT-15 | task 重发/摘要冲突/过期/结果 ACK 丢失 | 不重执行，稳定结果重传，模糊副作用 UNKNOWN |
| ESP-AT-16 | 无可信 UTC 冷启动 | 不伪造 observedAt，不执行 TTL 无法判断的任务，采集可 UNSYNCED |
| ESP-AT-17 | network 候选仅旧接口可达、缺少一项 probe ACK | 不提交候选；超时恢复 last-good |
| ESP-AT-18 | 候选提交前后分别断电、主备接口切换 | 恢复正确版本，身份和 spool 保留 |
| ESP-AT-19 | 扫描过滤两路对照空口 | 规则/字节/ADV与SCAN_RSP一致，差异有诊断 |
| ESP-AT-20 | 服务器广播开关、三模式、载荷、间隔 | UART/本机读回与空口一致，失败不假报成功 |
| ESP-AT-21 | 热更新部分失败、手机连接时关闭广播 | 恢复/FAULT 清楚；关闭广告不误断手机，PAUSED 正确 |
| ESP-AT-22 | 未授权、过期、未知地址自动连接 | 不发起连接；合法任务完成服务/特征和实际对端核验 |
| ESP-AT-23 | GATT 写出后断链/复位，查询结果丢失 | 不重新物理写入，保存 UNKNOWN/终态证据 |
| ESP-AT-24 | ESP 重启但 nRF 连接仍存，队列中有旧通知 | 先对账后清连接，旧通知身份正确，旧引用拒绝新控制 |
| ESP-AT-25 | 手机恶意 RX、满队列、Notify 响应丢失 | 未认证不执行敏感动作，接管/受理/业务 ACK 不混淆 |
| ESP-AT-26 | nRF 制品错组件/型号/manifest签名/SHA/尺寸，另测错误MCUboot签名 | 前类在传输前或协议边界拒绝；错误MCUboot签名可到READY但须启动时被拒绝，不能误报成功，旧应用可用 |
| ESP-AT-27 | nRF 下载中 UART 重握手与芯片复位 | 前者核对后续传；后者不复用 RAM offset |
| ESP-AT-28 | FINISH/READY 后未发 REBOOT 即断电 | 正确识别试启动，不误认为只是旧下载未完成 |
| ESP-AT-29 | 同版本不同镜像、校验失败、窗口到期 | VERIFY_RUNNING 拦截，未确认镜像不报成功 |
| ESP-AT-30 | nRF 新镜像挂死、CONFIRM 前后复位 | 硬件恢复/回退有效；确认后的结果重传不再升级 |
| ESP-AT-31 | ESP OTA 写入、切槽、试运行、自检失败 | 旧固件可恢复，真实新版本成功才确认 |
| ESP-AT-32 | ESP 存储格式迁移后回滚 | 旧固件仍能安全读取已接管事件和任务 |
| ESP-AT-33 | 双扫描+Wi-Fi+FlashGC+云端重试长稳 | 无死锁、持续泄漏和假 ACK；资源及损失满足冻结指标 |
| ESP-AT-34 | SDK 构建、分区/镜像容量、生产安全配置 | 可复现构建和完整证据，密钥不入仓库，恢复路径可执行 |
| ESP-AT-35 | CUSTODY 前后整板断电，nRF RAM 丢失 | 仅承诺已持久接管记录恢复；未接管数据缺口可解释，不伪称可重放 |
| ESP-AT-36 | DFU RECEIVING/READY/FAULT/ABORTED 时重握手 | 按状态续传/启动/原session终止/新session重开，不因摘要相同误续传 |

## 19. 开发阶段与待冻结事项

| 阶段 | 必须交付 | 完成门槛 |
| --- | --- | --- |
| M0 工程与存储 | 固定 IDF/依赖、BSP、分区、持久事务/恢复 | 构建成功；关键提交断电测试通过 |
| M1 UART 与 nRF | 会话/命令/ACK/配置对账、单源采集 | ESP-AT-01～08 及对应硬件测试 |
| M2 网络与服务器 | 身份/TLS、spool、任务journal、网络候选 | 应用 ACK、掉电恢复和真实接口探测闭环 |
| M3 双无线业务 | ESP BLE、双路合并、广播、Central/GATT | 去重/广播/GATT 空口及服务器任务闭环 |
| M4 双组件升级 | nRF UART OTA、ESP 自身 OTA、恢复与结果 | 正常/异常/掉电/错误签名/回退矩阵通过 |
| M5 发布 | 压力、安全、制造配置和故障诊断交付 | 所有 P0/P1 验收完成，能力清单据实签发 |

以下事项不能被默认猜测；实现时先形成记录，受影响功能才可进入发布：

1. 实际 PCB/原理图版本、CH390H/复位/调试引脚和有效复位时序。
2. ESP-IDF、BLE Host、CH390H 驱动版本及授权来源；最终 Flash 分区与离线容量。
3. 可信 UTC 的冷启动引导、证书更新/吊销和初始设备凭据注入流程。
4. journal/去重保留期、隔离区满策略、损坏水位和恢复出厂后的管理恢复流程。
5. 手机 Peripheral 的认证、载荷、业务 ACK 与云端版本化接口；首次 BLE 绑定的受控预置流程。
6. ESP/nRF 各自的健康项目与确认预算；安全版本/防降级和密钥生命周期。
7. 精确峰值包率、离线时长、恢复时间、Flash 寿命和 Wi-Fi/BLE 共存验收指标。

上述未冻结事项不妨碍先开发可独立验证的传输、存储及解析模块，但不能用它们掩盖端到端功能缺失。

## 20. 文档核验与官方参考

本文件按当前两份协议、nRF 0.2.0 源码和服务器 Schema/校验实现核对。现有服务器/模拟器和 nRF 主机测试不是 ESP 实现证据；本文所有 ESP 实板验收项均待执行。

ESP-IDF 官方参考于 2026-11-19 查阅；当时 stable 页面显示 v6.1.1，仅用于核对能力和约束，**不表示本工程已选定或验证该版本**：

- [UART 驱动与硬件流控](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/peripherals/uart.html)。
- [NVS 存储](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/storage/nvs_flash.html)。
- [Wi-Fi/BLE 射频共存](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/coexist.html)。
- [ESP 应用 OTA 与回滚](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/ota.html)。

实现时优先使用锁定版本对应的官方文档/API，版本变化需要重新验证，不因本文件引用 stable 页面而自动升级依赖。
