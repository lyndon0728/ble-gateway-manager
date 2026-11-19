# ESP32-S3 持久化与掉电恢复设计

版本 0.3，日期 2026-11-19。适用 GW32 ESP32-S3 主机，对接 nRF 固件 0.2.0、UART 1.1 和服务器消息契约 1.0/1.1；补充本地与远程网络配置及 ESP OTA 存储。

**状态：持久语义与原始后端设计。** 本文细化 [ESP32-S3 详细需求](esp32s3-requirements.md) 中的存储与恢复要求。0.1.0源码已采用“低频双快照＋高频独立spool”的后端；与本文追加日志、检查点和统一物理域的差异见[实现决策](../firmware/esp32s3/IMPLEMENTATION.md)，实际验证见[验证记录](../firmware/esp32s3/VALIDATION.md)。下文记录格式与拟议接口不代表已实现的二进制布局；提交、重放和防重要求仍是验收约束。内部设计不增加UART opcode或服务器JSON字段。

关联文档：[开发总览](esp32s3-development.md) · [UART 协议](../server/docs/gateway-uart-protocol.md) · [服务器协议](../server/docs/gateway-protocol.md) · [消息 Schema](../server/internal/platform/assets/message-contracts.schema.json)。字段和行为发生冲突时，以已冻结需求及两层协议为准。

## 1. 必须保持的存储不变量

1. 未完成可恢复提交，不发送 UART CUSTODY，不执行任务副作用，不把结果发往服务器。
2. UART 观测与批次接管水位同事务；事件与被消费观测的删除标记同事务；任务终态与对应结果同事务。
3. 事件身份、载荷、采集时间、来源和 schemaVersion 一经定稿不可修改。重传只改变交付调度记录。
4. `resultSequence` 属于任务，不能因 ESP 重启归零；GATT `op_seq`、Peripheral `tx_seq` 属于对应控制器启动，不能因 UART 重握手归零。
5. 已收到平台 ACK 也必须先提交本地回收资格，之后才由 GC 擦除。内容回收不等于删除任务防重记录或 UART 接管水位。
6. 恢复只重建持久状态，不自行重新执行 GATT 写入、重启、配置切换或 OTA。
7. nRF 的采集 FIFO、业务配置、GATT 动作缓存和 DFU 会话均为 RAM 状态。nRF bond 的持久化不改变这一事实；ESP PSRAM 也不是持久介质。
8. 本设计的掉电保证以已经完成并验证的底层持久写入为前提。CRC/SHA 检测损坏，不是认证，也不是对任意 Flash 损毁的自动修复保证。

## 2. 存储域与单写入者

业务对象采用一个逻辑事务域，由唯一 `persistence` 写入任务拥有。接管水位与观测不能分别放进两个无法协调提交的后端。配置、日志、事件可以使用独立配额或物理区域，但实现必须提供下面定义的跨对象原子提交语义。

| 区域 | 内容 | 约束 |
| --- | --- | --- |
| 制造身份/安全凭据区 | 安装身份、设备凭据、信任根、凭据版本 | 与业务擦除分离；普通 GC 不访问明文秘密；业务只保存受控引用 |
| 事务日志与检查点区 | 配置、接管水位、观测、事件、任务、结果、动作 journal | 一个提交域；提交之前的新对象对其他模块不可见 |
| 隔离配额 | 永久拒绝的原始数据、损坏证据和处理状态 | 可以是同一日志内的对象类别；有容量上限与审计回收策略 |
| nRF 制品暂存区 | 原始 manifest、签名、已下载签名镜像 | 不占用已接管数据和 ESP 回滚槽；完整校验后才发布可用引用 |
| ESP OTA 应用槽/元数据 | ESP 当前和候选应用、OTA 启动选择 | 由选定 IDF 的 OTA 实现管理；本日志只保存意图与证据，不自行改写启动元数据格式 |
| RAM/PSRAM 索引 | 键索引、暂存队列、解码缓冲和运行期锁 | 可从日志重建；掉电后不能作为成功证据 |

最终分区和容量需结合 N8R8 实测 Flash、双 ESP 镜像大小、nRF 暂存、索引/GC 开销冻结，不在本文指定地址或虚构可用容量。

所有调用者提交有界请求，写入任务按依赖串行处理。事务内只做数据验证和本地存储，不等待 UART、BLE、HTTP、MQTT 或应用 ACK。业务模块在事务完成后另行执行外部动作。大镜像下载和 GC 分片执行，不能长期阻塞 RX 接收；Flash 暂不可接管时，传输模块仍可返回 BUSY。

### 2.1 存储模块的拟议接口

以下名称是接口职责，不是现存函数或已经实现的同步原语。

| 操作 | 输入/输出语义 |
| --- | --- |
| `recover()` | 验证根、检查点和日志，重建索引；返回 READY、READ_ONLY 或 STORAGE_FAULT 及诊断 |
| `reserve(class, worst_case_bytes)` | 在提交前保留所需日志、提交记录和允许类别配额；不跨外部 I/O 长期占用 |
| `commit(read_set, mutations)` | 校验键版本后，完整提交 PUT/DELETE/计数器更新；返回持久提交号或明确失败 |
| `read_snapshot(keys)` | 读取一致的已提交视图及版本；不暴露准备中的对象 |
| `pin(object_ref)` / `release()` | 网络发送、校验或恢复读取期间固定对象版本，防止 GC 擦除引用 |
| `gc_step(budget)` | 在时间/字节预算内复制和发布检查点；不得绕过提交规则 |

`commit` 至少区分 `DURABLE`、`NO_SPACE`、`CONFLICT`、`IO_ERROR`、`INDETERMINATE`。后两者不能被调用者当作“肯定没发生”；先恢复并按事务 ID/对象键查询结果。确认 DURABLE 前须完成后端规定的写入完成、持久屏障及必要读回。`write()`、`fflush()`、普通文件关闭或多次 NVS key 写入的返回值不能自行承担该承诺；具体后端保证须由锁定 SDK、实现和掉电试验共同验证。

## 3. 追加事务格式

设计采用不可原位改写的记录：`BEGIN → PUT/DELETE... → COMMIT`。删除、交付、配额转移和计数器推进也写成新版本；不在旧记录上翻转“已 ACK”位。一个事务不跨日志段，段大小必须容纳实现允许的最大事务及段封口开销。放不下时先结束当前段并切换到预留的新段；最大事务和对象长度属于开发前冻结项。

镜像文件不作为单个日志事务写入。平台上传批次也不要求把最多 1 MiB 的网络 JSON 复制进一个事务，可保存不可变事件列表并流式组装。需要支持的最大事件、结果和配置必须在 admission 阶段校验，不能执行了物理动作后才发现结果装不下。

### 3.1 记录头提案

下面是磁盘格式 v1 的候选布局，和 UART 帧无关。落地前须固定编码测试向量、最大值和迁移规则；禁止直接保存编译器 C 结构体。整数小端，固定头 56 字节，校验全部边界后才能读取 body。

| 偏移 | 字段 | 类型/用途 |
| --- | --- | --- |
| 0 | `magic` | 4 字节 `GWST` |
| 4 | `format_version` | u16，提案值 1 |
| 6 | `record_kind` | u16，BEGIN、PUT、DELETE、COMMIT、ABORT 或段控制记录 |
| 8 | `header_len` | u16，v1 为 56 |
| 10 | `flags` | u16，保留位为 0 |
| 12 | `record_len` | u32，含头、body 和写入对齐填充 |
| 16 | `body_len` | u32，不含对齐填充 |
| 20 | `ordinal` | u32，事务内从 0 递增 |
| 24 | `tx_id` | 128 位随机事务 ID；与 eventId、taskId、UART msg_id 无关 |
| 40 | `segment_generation` | u64，与段头一致，防止把擦除后复用地址的旧引用当作新记录 |
| 48 | `header_crc32c` | u32，计算时自身字段置零，覆盖完整固定头 |
| 52 | `body_crc32c` | u32，覆盖 body |
| 56 | `body` 与 padding | body 按对象/记录版本解码；padding 为规范零字节 |

`record_len = align_up(56 + body_len, write_alignment)`。对齐由真实后端及加密模式确定，不假设任意字节可独立原子写。COMMIT 使用独立新记录，不重复编程此前记录的写入单元。检查 `body_len ≤ record_len−56`、对齐、剩余段容量、字段上限和填充值，拒绝整数溢出。

每段另有不可变段头，记录物理段 ID、generation、格式、前序提交锚和校验；封口记录保存有效范围与末提交摘要。新段的段头和根目录登记必须先持久发布，之后才可承载会被 ACK 的事务。根目录按序记录可重放的段及有效范围，不靠全盘搜索 magic 发现遗漏事务。尾部损坏后的换段也遵循这条规则：保留旧段已提交前缀，发布新尾段，禁止擦除仍含唯一已提交对象的旧段。

记录类型的 body 定义为确定性二进制结构，实施时冻结 TLV/其他编码及最大嵌套深度。对象的原始字节可以嵌入 body，例如原始服务器命令、结果 JSON 和 manifest；验签所用 manifest 字节绝不重新序列化。

### 3.2 事务内容和发布顺序

| 记录 | 必需内容 |
| --- | --- |
| BEGIN | 预期变更数、总长度上限、配额类别、调用目的；事务 ID 在头部 |
| PUT | 对象类别/编码版本、完整键、预期旧版本、新逻辑版本、值长度和值摘要、值 |
| DELETE | 完整键、预期旧版本、新逻辑版本、原因；这是 tombstone，不是立即擦除 |
| COMMIT | 本次 `commit_seq`、前一提交号/摘要、成员数量/总字节数、按顺序覆盖 BEGIN 和全部变更记录的 SHA-256 |
| ABORT | 已准备事务的终止原因；无 COMMIT 的记录永不进入业务视图 |

CRC32C 参数沿用 UART 协议定义。COMMIT 的事务 SHA-256 覆盖成员完整编码记录及规范 padding，按 ordinal 顺序连接，不包含 COMMIT 自身；成员数和总字节数必须同时吻合，不能只校验单个记录 CRC。

提交过程：

1. 检查所有 read-set 键版本、身份域、长度和配额。失败不写业务变更。
2. 保留整个事务、COMMIT、段控制记录所需空间；写 BEGIN 和全部变更，等待写入完成。
3. 写 COMMIT，完成底层持久屏障，读回并验证记录与事务摘要。
4. 以一个动作更新 RAM 中的已提交索引；之后才向调用者返回 DURABLE。
5. 调用者根据已提交对象发送 CUSTODY、结果或外部动作。步骤 3 成功而步骤 4/5 前掉电时，恢复必须重放该事务。

`commit_seq` 是存储域内的提交顺序，不是事件 sequence。准备中的事务不推进可见计数。只有单写入者能分配提交号；事务 ID 不依赖未持久计数器，重启后不得复用已知事务 ID。失败事务需要重新执行时，按业务键去重并使用新 tx_id。

## 4. 启动重放、尾部与损坏处理

根记录包含 `storage_format/identity_epoch/root_generation/checkpoint_ref/checkpoint_hash/checkpoint_commit_seq/tail_start/segment_generations`。根元数据放在两个独立可擦除区域，采用追加记录及校验，不覆盖唯一有效根。检查点包含所有存活对象的键、逻辑版本、物理引用和摘要，以及必须保留的序号、水位、防重和 tombstone 语义。

恢复顺序：

1. 检查制造身份与日志 `identity_epoch` 是否相容。身份不匹配时隔离旧域，不把旧事件换成新 gatewayId 上传。
2. 选择可验证的最新根，验证其检查点、全部引用和段 generation；不得为了“能启动”跳过不可读的已承诺对象。
3. 从检查点提交号后的尾日志顺序验证记录，逐事务收集边界和摘要。只有完整 COMMIT、成员一致、前序提交链一致的事务才更新索引。
4. 明确未提交的尾事务丢弃；当前尾部发生不完整编程后，不在损坏记录中间搜索 magic 继续猜测，也不覆盖该编程范围，换到新段继续。
5. 已封口段损坏、提交成员缺失、引用错代、键版本不连续或无法判断是否涉及已承诺数据时进入 STORAGE_FAULT；保留故障字节，不自动清空 NVS/格式化日志。
6. 恢复旧 spool、任务和水位之后，创建并持久化本次 ESP boot 对象，再允许新事件或外部副作用。接入 nRF 后单独执行协议对账。

上述“未提交尾事务可丢弃”针对断电撕裂当前写入的故障模型，不授权忽略任意介质损坏。后端若无法区别已承诺数据损坏与未提交尾部，应进入故障状态；是否增加独立提交账本/元数据副本，以及如何检测读扰和坏块，必须随后端方案冻结。存储恢复测试必须同时包含正常断电与人工损坏，两者预期不能混写。

根、索引或记录校验失败不允许回退到一个会重新执行旧动作的历史快照。可读对象只用于受限诊断和经确认的导出；是否继续某个独立存储域的业务由故障策略决定，不能继续为不确定的批次发送 CUSTODY。

## 5. 持久对象、键和字段

所有业务键隐含前缀 `identity_epoch/namespaceId/gatewayId`。以下斜线格式为逻辑键，不要求实现为文件路径。对象必须包含 `object_schema`、逻辑版本和内容摘要；未知未来版本不得按旧布局猜测。

| 对象键 | 最少持久字段 | 约束/回收条件 |
| --- | --- | --- |
| `identity/current` | 型号、硬件版本、身份 epoch、凭据/信任根版本引用 | 秘密实体在受控区；换身份必须显式处理旧 journal/spool |
| `boot/<espBootId>` | 128 位启动 ID、创建提交号、复位原因、事件/local capture 下一序号 | 当前 boot 创建失败则不进入业务；真实 ESP 重启生成新 ID |
| `nrf/<nrfBootId>` | 首次/最近观测信息、固件版本、HELLO 能力、状态对账结果 | 这是观测证据，不代替实时 HELLO；与旧数据引用一起保留 |
| `custody/<nrfBootId>` | 最近 batch_seq、first_capture_seq、count、type、payload SHA-256、最后 capture_seq、提交号 | 与该启动关联独立保留，云端 ACK 不能删除 |
| `observation/<radio>/<controllerBootId>/<captureSeq>` | 原始记录/AD、类别、地址和属性、捕获时刻、入队 boot/单调时刻、入队序号、时间证据、来源批次 | nRF 序号来自批次；ESP 本机由自身启动域分配；消费与事件创建同事务 |
| `event/<eventId>` | 原 espBootId/sequence、schemaVersion、不可变完整事件字节/摘要、来源 observation 键列表 | 定稿后不可修改，平台合法逐项接管后才可回收内容 |
| `event_delivery/<eventId>` | PENDING/DELIVERED/QUARANTINED、ACK 证据、重试调度、隔离原因 | 交付状态与事件内容分开，避免重试改变去重摘要 |
| `upload/<batchId>` | schemaVersion、有序 eventId/内容摘要列表、发送身份、成员结果 | 固定成员后才发送；批次可重新分组，事件不能改 ID |
| `config/<group>` | 目标/已提交 revision、完整快照、原 patch、taskId、量化后的实际快照、阶段 | 扫描、广播、网络等组独立；不把最近服务器 patch 当完整配置 |
| `network/candidate` | 来源、本地transactionId或云taskId、基线generation/localRevision、旧实际/候选快照、凭据引用、阶段/期限、验证证据 | 云任务记录probe resultId/ACK，本地记录独立挑战；按来源的提交门槛处理，不伪造任务 |
| `network/current` | networkGeneration、localRevision、实际来源、activeLocalSnapshot（本地保存目标）、actualNetwork/activeInterface、lastVerified/前一回退引用、localOverride、同步状态 | 目标与实际分开；本地提交不修改config/network的平台revision；所有入口共用世代CAS |
| `network/transaction/<id>` | 来源/操作者摘要、请求ID与受保护防重摘要、阶段/结果、窗口/boot、版本与配置引用 | 重连/重启查询同一事务；无可信UTC记录单调时刻和时间质量 |
| `credential/<ref>/<version>` | 设备/SSID/安全模式绑定、加密实体引用、INSTALLED状态、deliveryId和回执交付状态 | 不存明文秘密；当前/候选/回退仍引用时不可回收 |
| `network/reconciliation` | 本地完整脱敏快照/snapshotId、平台基线、同步token、裁决/关联任务与前置条件 | 只有匹配世代的正常平台任务验证提交后清除对应override |
| `task/<taskId>` | requestDigest、完整原命令、内部阶段、执行意图、有效期、证据、下一 resultSequence | 同 ID/digest 防重；终态防重记录独立于 outbox 内容回收 |
| `result/<taskId>/<resultId>` | resultSequence、不可变线上结果字节、内容摘要、交付状态 | 按该任务顺序发送；匹配 RESULT_PERSISTED 后可回收内容 |
| `fact/<taskId>/<factId>` | 尚不能构造线上信封的事实、控制器/ESP boot、单调时刻、动作结果和来源 | 无可信 UTC 时保留，不能生成伪造 observedAt 的结果 |
| `gatt_seq/<radio>/<controllerBootId>` | 下一个 op_seq、最近持久动作键 | 全启动共享动作序列，不能按 taskId 各自从 1 开始 |
| `gatt_op/<radio>/<controllerBootId>/<opSeq>` | taskId、opcode、完整 args/摘要、超时预算、意图/受理/终态证据、conn_id | 最新 nRF 结果缓存被下一动作替换前，ESP 必须已持久保存终态 |
| `connection/<radio>/<controllerBootId>/central/<connId>` | connectionRef、目标/实际对端、MTU、安全、任务、登记状态、关闭原因/事件键 | 控制只接受当前活登记；历史通知仍能解析原引用 |
| `auto_evidence/<eventId>` | nRF boot、原地址/类型、可连接性、采集单调时间、时间依据、关联 UUID 事件 | 至少保留协议 30 s 上限；独立于事件 ACK；重启后不能靠新单调时钟延长有效期 |
| `peripheral_inbox/<nrfBootId>/<captureSeq>` | peripheral_conn_id、channel/kind、原始应用字节、业务验证/处理状态 | 0x22 的持久接管对象；未定义云端类型时不得伪装为 GATT_NOTIFY |
| `peripheral_tx/<nrfBootId>/<txSeq>` | conn_id、业务事务 ID、完整载荷、发送意图、受理/应用 ACK 状态 | tx_seq 在 nRF 启动内递增；本地受理不等于业务送达 |
| `artifact/<artifactId>/<sha256>` | 原始 manifest/签名、组件/版本/兼容性、镜像长度、存储引用、完整验证结果 | 文件与其摘要必须验证完成后才发布 VERIFIED 引用 |
| `ota/current` | 唯一组件升级所有者、taskId、artifact 引用、阶段、恢复目标、确认/结果证据 | ESP/nRF 升级互斥；试运行及待对账期间不能释放互斥后开始另一升级 |
| `ota/nrf/<taskId>` | nrfBootId、非零 session、size/SHA、最后查询状态/offset、块信息、FINISH/REBOOT/VERIFY/CONFIRM 意图与证据 | 进度不是 nRF 掉电持久检查点；按第 10 节恢复 |
| `ota/esp/<taskId>` | 旧/候选版本和 SHA、目标槽、存储格式兼容版本、启动/健康/有效确认阶段 | 不取代 IDF OTA 元数据；回退应用须能读共享存储 |
| `fault/<faultId>` | 故障类别、对象/任务/批次、原始证据摘要、boot、时间质量、维护决定 | 有独立配额；无线损失计数与受控丢弃原因不得只存 RAM |

平台整数必须无损序列化。存储层可用 u64 保存序号，但发送前仍须满足当前 Schema、JSON 实现和服务器数值处理的共同上限；端到端上限未验证前不得承诺完整 u64 JSON 精度。任何序号接近已冻结上限都要停收/排空并按协议换代，禁止回绕。

### 5.1 跨启动 ID 和时间的规则

- 每个真实 ESP 启动先提交新的 boot 对象，新事件的 sequence 从 1 分配；分配计数和事件创建必须同事务。事务失败不会产生可发送的新 ID。
- ESP 本机 captureSeq 在观测成功持久接纳时与本机捕获计数同事务推进；不能把尚未提交的 RAM 队列下标当作可恢复采集身份。resultId 和 batchId 使用满足现有 Schema 的独立随机唯一值，首次提交后固定；它们不能只由会在重启时归零的计数生成。
- 旧事件继续使用旧 `espBootId-sequence`。ESP 重启后才定稿的旧观测使用当前新 boot 的事件序列，同时保留原控制器捕获身份；事件序列表示定稿，不表示重新采集。
- UART `capture_seq/batch_seq` 由 nRF 启动限定；`link_id` 只限定一次控制会话。ESP 重启后以新 link_id 对账，不把旧 msg_id 当跨重启的动作防重键。
- 保存时间锚的 `espBootId/nrfBootId/anchor monotonic values/RTT/error bound`，仅供同一有效时间域判断。不得用两次 ESP 启动的 `uptime_ms` 相减，也不得把持久锚恢复后直接视为新启动有效锚。
- 已在采集时可靠确定的 UTC 及其证据可随观测保留；没有这项证据时，恢复后不能用当前 UTC 倒推出掉电前精确采集时间。失去单调映射的待定观测分别定稿，有有效采集 UTC 才使用 SYNCED，否则 UNSYNCED。
- 期限与重试等待保存来源 boot 和原始策略，不直接恢复旧 uptime 截止值。可信 UTC 未建立时不执行无法判断 expiresAt 的任务；无需 TTL 的采集仍可进行。计时器恢复策略不得把任务总预算或去重等待重新开始一整轮。

## 6. UART CUSTODY 事务

接管对象根据帧类型分流：0x20 进入扫描观测/事件，0x21 进入 GATT 事件处理，0x22 进入独立应用 inbox；三类共用同一个 `custody/<nrfBootId>` 水位。整个批次校验后全量接管或拒绝，不能对其中部分记录 CUSTODY。

```text
有效会话 + 完整批次结构验证
→ 读取该 nrfBootId 的已提交接管水位
→ 最近相同批次：比对 type/first/count/SHA，直接回放 CUSTODY
→ 早于已提交水位：忽略；前跳/同号冲突：持久诊断并 FAULT
→ 新批次：验证 batch_seq 与 capture_seq 连续并计算全部观测键
→ reserve(全部记录 + 水位 + COMMIT + 最终定稿所需预算)
→ 同事务 PUT 观测/事件/inbox + PUT 新水位 + 必要计数
→ DURABLE 后发送 CUSTODY；其他结果不发送 CUSTODY
```

新 nRF 启动首次接管要求 batch_seq=1、first_capture_seq=1。已有启动的水位损坏、缺失或来源不明时，不能接受从中间开始的批次来“修复”连续性；进入维护流程。SHA 覆盖完整批次 payload，不能只对各 AD 的拼接计算。重建链路不改变上述键，也不能删除旧水位。

单次提交的最低变更集合为 `所有新记录 + custody 新水位`；若记录可直接定稿，还需在同事务创建 event、delivery、对应 event sequence 推进。允许先全部持久为待定对象再分别定稿，但已 CUSTODY 的对象必须始终有可恢复的拥有者。

只有存储暂不可写或配额不足才用 BUSY，retry_after_ms 为 250–5000。FAULT 用于语义完整性/连续性错误，会冻结 nRF 数据队列，且 HELLO 不能解除。FAULT 原因应先写入预留故障区；若存储完全损坏无法留存，不得因此伪发 CUSTODY，应进入可诊断故障并保留可读证据。

CUSTODY 的发送事实不需要额外成为“必须持久”的 ACK 位：提交水位已经足够在重放时重发 ACK。ACK 发送失败不撤销事务，也不允许重新生成观测。

## 7. 双射频定稿和事件上传

### 7.1 匹配与定稿事务

只有不同无线电的可比较扫描观测才配对。沿用需求规定的地址/类型、ADV 或 SCAN_RSP、legacy/extended、SID、可连接属性和完整 AD 匹配，Hash 命中后仍比较原字节；时间差绝对值加误差估计不超过 50 ms。观测采集时刻距有效锚超过 60 s、未知地址或元数据不可比时单独定稿。

每条从进入暂存队列起最多等待 150 ms；期限不能从 Flash 完成或重启恢复时重新起算。单无线电采集直接定稿。双路到期时选择时间差最小的未配对异源候选，同差选入队次序更早者，命中后以 nRF 为主。运行期配对决策与持久提交由同一序列化状态机协调；不能一边异步提交配对，一边让另一线程消费同一观测。

```text
read_set = 待定观测 A/B 版本 + 当前 boot 事件计数版本
mutations = 新事件及 delivery=PENDING
          + A/B 的消费 DELETE
          + 当前 boot 的下一 sequence
          + 需要保留的自动连接证据
commit(read_set, mutations)
```

单条定稿只消费 A。没有 COMMIT 时 A/B 仍是待定；COMMIT 完整时只恢复一个新事件。配对事务因版本冲突失败时重新读取状态，不重用已经对外发送的 eventId；新事件只有提交成功后才能入网络发送队列。

150 ms 是配对决策截止，不是对任意慢 Flash 的完成保证。达到截止时固定选择，后续来得更晚的记录不得补配该观测；IO 失败恢复后只能从已提交状态重新决定。ESP 重启使旧运行期时间域失效，未提交的旧候选分别定稿，不能把两个独立定稿事件事后合并。

待定上限为 256 条或 128 KiB。接管新观测时还需为它最终进入事件/应用 inbox 的空间和必要提交开销保留预算，避免 CUSTODY 后出现“观测有空间、定稿永远无空间”。满时有 spool 预算则单条定稿，否则 nRF 返回 BUSY；ESP 本机 BLE 没有 UART 重放保障，应按已冻结策略暂停或丢弃新增观测并计数，不能删除已经持久接管的对象。

### 7.2 平台批次、逐项 ACK 和隔离

发送前提交 `upload/<batchId>` 的 schemaVersion、身份和有序事件键/摘要。网络传输从固定版本对象读取，不能把新旧 schemaVersion 混在一批。批次可改变，事件的 ID/内容不变；重试同一批次时必须仍能证明它的成员关系。

ACK 必须匹配 namespace/gateway/batchId 且 durability=RAW_PERSISTED；只对该 batch 的对应 eventId 更新交付状态。ACCEPTED/DUPLICATE 写 DELIVERED，RETRYABLE 保留内容与退避信息，PERMANENT 同事务移入隔离状态并保留原数据，缺失项不变。部分成功后可给剩余事件建立新批次；对迟到旧 ACK 仍按已保存成员关系核验，幂等更新。

先提交交付状态，再释放 RAM 索引引用，最后由 GC 回收 payload。若 ACK 已到但本地提交前掉电，重发旧事件由平台按 eventId 去重；不重复执行采集业务。消息 PUBACK 或 HTTP 2xx 不能触发该事务。达到服务器去重保留期的旧 spool 转运维对账，不无限承诺历史 ID 仍能去重。

## 8. 任务、配置和结果事务

### 8.1 任务受理与结果

受理事务创建 `task/<taskId>`，保存服务器原始 requestDigest、完整命令及校验结果。相同 ID/digest 查询既有状态，不重新执行；相同 ID/different digest 留冲突证据。CONFIGURE 的 digest 对应原 TaskRequest，不能用追加了 desiredRevision 的命令重新计算后覆盖。

准备执行时先提交 `EXECUTING_DURABLE`、副作用意图、当前 boot、总预算与设备目标；之后才发 UART 或执行本机动作。持久“准备发送”不等于“确定已经发送”，恢复时须把这段窗口作为可能已执行处理，使用动作自身可查询证据收敛。

结果构造事务至少同时更新：

- task 的阶段/终态与证据；
- 该 task 的 nextResultSequence；
- 不可变 resultId、resultSequence、observedAt、state/verification/details 和结果摘要；
- 结果 outbox=PENDING；连接成功时还包含 connection 登记。

resultSequence 在任务内递增，跨 ESP 启动延续，不使用 event sequence。无可信 UTC 时只保存内部 fact；取得可信时间并完成必要对账后，以实际观察/对账时刻构造合法 observedAt，不伪造旧事实的精确 UTC。不得因暂时不能发送结果而丢弃已经取得的 GATT/升级证据。

结果重传保持完整字节和身份不变。RESULT_PERSISTED 按 namespace/gateway/taskId/resultId 匹配，不增加不存在的结果 batchId。只在逐项 ACCEPTED/DUPLICATE 后提交 DELIVERED；仍保留 taskId/digest/终态/已执行标记以防重复任务。UNKNOWN 保留待对账占用，按服务器约束只放行诊断，不自行把 UNKNOWN 转成可重跑。

### 8.2 配置与网络候选

一个配置组保存目标、已提交完整快照及在途候选；数组/原始字节整体替换。应用前提交候选及任务意图，应用后获取实际回读，再提交本组生效快照与可验证结果；失败保留旧提交版本和真实控制器状态，不能用回滚本地记录假装硬件已回滚。

远程网络 candidate 额外保存 last-good、每个受测接口和 probe resultId。来自对应候选接口的合法 RESULT_PERSISTED 证据持久后，才能在事务中把 candidate 变成 last-good。任一必要 ACK 尚未提交时重启，恢复旧 last-good；配置提交后重启恢复新配置。probe 结果 ACK 与最终成功结果是不同事实。

### 8.3 本地配网、凭据与双入口版本

按[配网设计](esp32s3-network-provisioning.md)，本地transactionId/localRevision与云taskId/desiredRevision独立；共同持久networkGeneration并在接纳和提交时CAS。租约覆盖网络试用全过程，ESP重启先恢复/回退在途候选，再接纳新操作。

本地在线提交把activeLocalSnapshot、lastVerified、generation+1、localRevision+1、localOverride、审计及事务终态一次发布；不改平台config/network的已确认revision。LOCAL_ONLY提交保存目标和未验证状态，同样推进本地/公共世代，但保留旧lastVerified和凭据；后续网络失败回退只改变实际选用/诊断，不丢弃用户保存目标。提交前掉电回到原实际快照，提交后恢复对应保存状态；浏览器是否收到响应不改变该判断。

本地挑战的nonce/响应绑定boot、transaction、接口、snapshotId和期限；重启使未完成验证失效。后台LOCAL_ONLY验证发布前重新获得网络租约并检查snapshotId/networkGeneration，过期证据不得改变较新lastVerified。窗口、SRP会话及会话密钥不恢复为活跃；已提交事务结果和凭据引用仍须可查询。配置对账或证明状态更新不再推进一次配置修订，远程采用/覆盖的正式新任务提交则推进公共世代。

凭据实体在独立加密后端写入、完成持久屏障并读回验证后，才在统一事务域发布可引用元数据及INSTALLED。跨后端不假定原子写：先秘密实体后引用，失败可留下不可见孤儿但不能留下指向未完成秘密的有效引用。回收先确认无当前/候选/回退/未结束投递引用，再提交元数据删除，最后擦除实体。普通日志不保存秘密正文；密码防重摘要用受控HMAC，不公开明文密码散列。远程安装回执在发布后才排队，断电重发同一deliveryId而非重复创建版本。

新增至少覆盖AT-38/40/41/42/43：秘密实体和引用发布之间掉电、LOCAL_ONLY与lastVerified恢复、网络提交与响应之间掉电、本地状态上传和旧云命令竞争、裁决提交前再次本地修改。不能让恢复出厂或GC绕过这些引用及版本约束。

## 9. GATT 与 Peripheral 动作 journal

### 9.1 Central 动作

同一 `radio/controllerBootId` 分配非零递增 op_seq。事务中保存动作完整 args、opcode、taskId、实际剩余 timeout_ms（100–30000）及序号推进后才能发送。nRF 只缓存最新已受理动作，故 ESP 必须提交其终态后才受理下一动作，不能依赖 nRF 查询任意历史序号。

| 恢复观察 | 决定 |
| --- | --- |
| 相同 nRF boot，原动作仍为最新且 RUNNING | 查询 GET_OP_RESULT，不发第二个物理动作 |
| 相同 nRF boot，查询得到终态 | 持久结果和证据，再推进任务 |
| 相同 nRF boot，原请求明确未受理且任务仍有效 | 按错误语义允许重试；BUSY 后新 msg_id，保留未受理 op_seq |
| GET_OP_RESULT 对非零原 op_seq 返回 NOT_FOUND，且同 nRF boot/完整意图/单主机序列均已证实 | 按协议表示该序号尚未受理；任务仍有效且预算允许时才重试，不与其他 opcode 的 NOT_FOUND 混淆 |
| 查询为 STALE、日志缺失或启动/序号关系无法解释 | 不凭空判定未执行；保留证据并进入待对账 |
| nRF boot 改变，写入是否执行不可证实 | UNKNOWN，不能换 op_seq 重写 |
| UART 丢响应但未换会话 | 同 msg_id/相同字节重试；不得插入新控制查询覆盖最近缓存 |

UART 握手变化只允许重新获得控制会话，不允许重建副作用身份。ESP 重启但 nRF 连接仍在时，先对账旧操作再关闭旧连接，避免断链破坏剩余证据。关闭连接登记与一次 GATT_LINK_DOWN 事件创建同事务，以该连接原身份和关闭事件键防重；旧排队通知继续携带原 connectionRef。

connectionRef 算法只按现有 Central 云端契约使用。Peripheral 的本地键必须额外包含 role；不能因为其 conn_id 数字与 Central 相同而复用同一平台引用或动作权限。

### 9.2 Peripheral 应用数据

0x22 全批持久进入 inbox 后才 CUSTODY，手机的 ATT Write Response 已发生于 nRF RAM 入队，不代表本事务完成。inbox 记录应用认证、防重放、路由及处理结果；业务协议未定义前只允许受限存储/拒绝敏感业务。

发送通知前，持久 `tx_seq/conn_id/完整载荷/业务事务 ID` 及意图，再调用 0x0E。UART OK 仅更新“本机栈已受理”；对端应用 ACK 另行提交，不把通知回调视为业务送达。相同 nRF boot 的最新相同 tx_seq 可回放受理结果，旧序号 STALE 或重启后无法证明送达时不能盲目重发业务。tx_seq 的分配状态与最近请求引用跨 ESP 重启保留。

## 10. OTA journal 与镜像暂存

### 10.1 制品暂存的发布

先创建下载记录并预留空间，保存原始 manifest/签名、artifactId、组件、期望大小/SHA 和下载状态。内容可分块写入不可见暂存实体；仅“下载 offset”不能代表完整可用。下载完验证全部文件 SHA、manifest 原始字节签名、型号/硬件/组件/版本约束并完成持久写入后，提交 VERIFIED 制品引用。

掉电后孤立暂存块可以作为未完成下载清理；被有效 artifact/ota 对象引用的实体禁止 GC。恢复后使用前重新检查实体长度、摘要及信任策略适用性，不能只信一个旧 `verified=true` 标志。部署私钥不存于普通 journal；manifest 信任根与 MCUboot 镜像验签公钥是独立关系。

### 10.2 nRF 升级

升级 journal 绑定 task/digest、组件、制品、准确文件长度/SHA、业务恢复目标、nrfBootId、session 和执行阶段。BEGIN 前必须已持久保存完整镜像与意图，并按授权关闭连接、停止新增采集和接管剩余 FIFO。镜像传输阶段不回收暂存实体。

每个 CHUNK 从固定制品偏移读取，最大 1024 字节，首块至少 32 字节。v1 设计在每次有效响应后持久记录最新状态、accepted_offset 和最近块 offset/len/SHA，再发送下一块；写放大优化若改为批量保存，必须另外证明恢复仅依赖 STATUS 和不可变制品，不把滞后本地进度用来重发过旧块。

| nRF 对账状态 | 存储/恢复动作 |
| --- | --- |
| RECEIVING，boot/session/size/SHA 一致 | 校验 0≤accepted_offset≤size，从返回位置继续；本地 offset 仅作检查，不覆盖 nRF 真实状态 |
| nRF 启动变化且原下载未完成 | 旧 RAM offset 作废；确认实际运行镜像和任务仍允许后，新会话从 0 开始 |
| READY | 记录已武装 TEST；掉电也可能试启动，不再按下载中发送 CHUNK |
| REBOOT_PENDING | 保存重启意图并等待主动 HELLO 对账；不可 ABORT，重试不延长 nRF 倒计时 |
| FAULT | 保存 last_error；原 session ABORT 成功后读回再恢复业务，新升级使用新 session |
| ABORTED | 不复用旧 session；没有新的合法任务意图不能自动重新开始 |
| 新启动、镜像尚未确认 | 优先健康和运行镜像校验；读取确认预算，不能因处理普通业务错过窗口 |

FINISH、REBOOT、VERIFY_RUNNING、CONFIRM 各自在发送前有持久意图，响应与相关证据随后提交。FINISH/READY 不是升级成功；MCUboot 签名检查在启动阶段。运行镜像与期望制品准确长度/SHA 一致、版本符合任务且健康检查通过后，才允许 CONFIRM；再次读回 running_confirmed=1，再提交终态和成功结果。

CONFIRM 已执行但 ESP 未保存响应就掉电时，恢复后重新读当前 boot/版本/镜像并核验确认状态，不能因为缺少本地响应就重新刷写。只比较版本字符串或本地下载 SHA 不能证明当前镜像。验证标记属于本次 nRF 启动，芯片重启后须重新 VERIFY_RUNNING；UART 重握手不重置此标记。

`confirm_remaining_ms` 是当前查询时的运行期预算，不能把旧值保存后在 ESP 重启时重新获得该窗口。默认 120 s 不是 ESP 自身 OTA 的确认期限；长命令等待受剩余预算约束。nRF 挂死需 ESP 已验证的硬件复位路径才能促成未确认回退。

### 10.3 ESP 自身升级与存储格式

ESP OTA journal 与选定 IDF OTA 元数据交叉核验，保存候选版本/镜像身份、旧版本、启动槽、健康阶段和结果。主机重启前处理 nRF 动作与 FIFO；不能假设 nRF 会随 ESP 自动复位。

0.2.0 的本地 OTA 与远程 OTA 在同一执行器中区分来源。本地 `ota/local` 记录 source=LOCAL、uploadId、签名制品身份、候选槽/大小/版本/SHA 和阶段；远程 `ota/active` 继续绑定 taskId/digest/artifactId/原始授权期限。不能以本地记录覆盖未决远程升级，也不为本地记录生成 TaskResult 或推进网络/平台修订。

本地每块接收偏移及最后块重试证据只在本次启动的 RAM 中保留，不是断电检查点，也不为每个 4096 字节块复制整个配置快照。接收中或 READY 时掉电，恢复为 ABORTED，旧运行槽继续；新上传须使用新 ID 从零开始。READY 表示镜像已验证，只有明确 COMMIT 才记录启动意图并设置启动分区。窗口结束也只取消未提交候选，不能取消已经建立的试启动恢复义务。

COMMIT 的 journal 与 otadata 不能宣称跨分区原子提交。恢复须对照实际运行槽、SDK 镜像状态、文件长度/SHA、版本及健康事实；REBOOT_PENDING/CONFIRMING 本身不表示成功。新应用经过本地 90 s 限时自检或远程有效任务的恢复流程后才标有效并保存 CONFIRMED；回退需核实旧槽实际运行。若标有效与记录成功之间断电，依据实际状态恢复结果，不重刷镜像。完整状态与验收见[OTA 设计](esp32s3-ota.md)。

试运行期间共享 journal/spool 的写入格式必须仍能被可能回退的旧应用读取。优先保持向后可读编码，或并存新旧版本并保留旧根；未确认前禁止破坏性迁移和回收旧读路径。仅修改格式版本号不构成兼容性。确认新应用有效及迁移可恢复后，才能按发布策略回收旧格式副本。

## 11. 检查点、GC 和擦除顺序

GC 的目标是回收不可达物理字节，不改变业务是否已接管、是否已执行或时间身份。正常日志写入和 GC 由同一写入者调度；网络持有的 pin、升级制品引用和恢复中的对象都属于存活引用。

1. 在已提交序号 C 获取一致快照，封口当前段并确定 C 之后的尾日志入口。选择非活动、非固定引用的候选段；预留复制段、检查点及根发布空间。
2. 分片复制候选段内仍存活的对象，保留其逻辑键、版本、内容和摘要。物理地址改变不等于新的业务事件或新动作。C 后的新事务只进入新的尾日志，不原地修改快照对象。
3. 生成完整检查点索引：指向保留段或验证完成的新副本，包含 C 时的所有存活状态、计数和防重信息。DELETE 的语义必须保留，或者证明旧版本已完全从可达集合排除，不能让删除对象在重放时复活。
4. 校验所有新引用并完成持久屏障；安装根时由单写入者合并当前已发布的尾段目录，向两个根元数据区域分别追加并验证新 generation 的根。根指向检查点 C 和 C+1 起的全部尾日志，不能用 GC 开始时的过时段列表覆盖期间新建的尾段；尾部新提交由恢复重放，不要求为了 GC 丢弃后续事务。
5. 只有新根已按策略冗余发布，且旧候选段不再被新根、尾部对象或 pin 引用时，才擦除旧段。擦除和重新使用需推进 segment_generation；旧物理引用必须失效。

根发布前掉电使用旧根，旧段尚未擦除；发布后使用新根，旧段可能保留或已部分擦除但不再是唯一数据源。两份根不同 generation 是更新中的合法状态，选择可完整验证的新根；无法验证最新根/引用时不能无条件退到旧根并重做已执行任务。元数据区域自身轮换也必须保留另一份有效根，禁止同时擦除两份。

每次 GC 记录复制字节、回收字节、耗时、擦除次数和写放大。GC 不能靠删除未交付 payload、UNKNOWN 任务或当前 nRF 水位解决空间不足。发布后的检查点已保存当前防重状态时，可淘汰被其完全覆盖的历史事务；当前业务对象的回收条件仍由各对象定义决定。

## 12. 配额、损坏与受控维护

至少区分观测/事件、结果/任务、隔离、GC 工作空间、故障记录和镜像暂存配额。预留空间应覆盖：最大单事务、完成一次安全 GC 的工作集、已受理任务的最小终态结果、必要故障记录。具体字节数和高低水位需按真实对象尺寸/峰值负载测量冻结，不能写一个没有容量证明的百分比就开始接管。

| 情况 | 处理 |
| --- | --- |
| 事件区达到接管阈值 | 暂停/背压新增观测；nRF BUSY，本机 BLE 按策略暂停或计数丢弃新增输入 |
| 待定区满但可定稿 | 使用已预留预算单条定稿，不无限等待跨源配对 |
| 隔离区满 | 保留已有原始数据，拒绝继续消耗不可保证的空间；按冻结策略告警/人工导出，不能静默覆盖 |
| 结果或任务预留不足 | 在新物理动作开始前拒绝受理/推进；已有结果不因事件压力删除 |
| 镜像暂存区与 spool 竞争 | 升级先做容量准入；不删除已接管事件或 ESP 回滚镜像为下载腾位置 |
| 明确未提交尾部 | 按第 4 节恢复，保留旧已提交视图，换新段，不整盘格式化 |
| 已提交数据/索引损坏 | STORAGE_FAULT，停止不可靠接管和副作用，保存证据并走受控恢复 |
| 水位丢失但 nRF 仍运行 | 不接纳中途批次为新起点；核查已接管数据，明确是否复位及其 RAM 损失 |
| 任务防重保留期即将到期 | 与服务器/维护策略对账；不能简单 TTL 删除后把迟到 task 当新动作 |
| 恢复出厂/重新绑定 | 明确身份、凭据、spool、task、watermark 去留；旧身份数据不得重标成新身份 |

损坏隔离保留可读证据不代表数据已交付。受控删除需要明确授权、对象范围、原因和损失计数；审计记录也要获得持久空间。存储完全不可写时不能承诺本次故障一定已经落盘，应在可用诊断通道报告实际限制。

离线时长按 `可用事件持久字节 / 实际平均记录占用 / 输入记录率` 估算，包含 envelope、索引、事务、交付元数据和 GC 预留；长期寿命另计复制写放大、热点元数据和擦除分布。不得用原始 AD 总字节或 PSRAM 容量当作离线持久容量。

## 13. 掉电与损坏验收矩阵

每个用例记录故障注入位置、两颗芯片是否同时掉电、最后持久提交号、发送过的 ACK/动作、恢复对象及对端观测。以下是待执行验收，不是测试通过记录。

| 注入点 | 故障范围 | 必须观察的结果 | 关联验收 |
| --- | --- | --- | --- |
| UART 记录写一半、COMMIT 前 | 仅 ESP 复位/掉电 | 未 CUSTODY；nRF 同 boot 重放后只接管一次 | AT-04 |
| COMMIT 完整、CUSTODY 前 | 仅 ESP，nRF 保持运行 | 水位与观测一起恢复；重放只发 ACK | AT-05 |
| CUSTODY 后、平台 ACK 前 | ESP 或整板 | 已接管记录及旧 eventId 恢复，原内容上传 | AT-06、35 |
| CUSTODY 前 | 整板，nRF RAM 丢失 | 只能恢复已提交部分，记录连续性中断；不声称未提交记录可补回 | AT-35 |
| 配对 PUT event 后、DELETE/COMMIT 未完整 | ESP | 无半配对事件；两条待定仍可恢复，重启后按失效时间域单条定稿 | AT-09、11 |
| 配对事务已提交、RAM 队列未清理 | ESP | 只恢复一个合并事件，不重复消费/生成 | AT-05、09 |
| RAW_PERSISTED 到达、本地回收事务前 | ESP | 允许原事件重传，依赖相同 eventId，未交付对象不丢 | AT-07、13 |
| 交付已提交、GC 复制/根发布/擦除各阶段 | ESP | 有效根与唯一存活副本始终可恢复，无对象复活 | AT-07、12 |
| GATT 意图已提交、UART 实际发出边界 | ESP/nRF 分别复位 | 按 op_seq/boot 对账；不因日志没有受理响应就重复写 | AT-23、24 |
| GATT 终态已获、结果事务中断 | ESP | 恢复可查询证据或 UNKNOWN；不执行第二次物理动作 | AT-23 |
| 结果已发、RESULT_PERSISTED 丢失 | ESP/网络 | 同 resultId/sequence/内容重发，task 防重不丢 | AT-15 |
| 网络各 probe ACK/候选提交前后 | ESP | 提交前 last-good，提交后 candidate；不伪造被测接口证据 | AT-17、18 |
| nRF 镜像下载/验证引用发布边界 | ESP | 未验证实体不 BEGIN，已验证实体及任务绑定可恢复 | AT-26、27 |
| CHUNK 的 nRF ACK 前后 | UART 重握手/两芯片分别复位 | 相同 nRF boot 按 STATUS，nRF 新 boot 不复用 RAM offset | AT-27 |
| FINISH 已武装 TEST、REBOOT 尚未发 | 整板 | 识别可能已经试启动，按运行镜像对账 | AT-28 |
| VERIFY/CONFIRM 意图、确认前后、结果提交边界 | ESP/nRF 分别复位 | 未确认不假成功；已确认按期望镜像收敛，不重新刷写 | AT-29、30 |
| DFU FAULT/ABORTED 后重握手 | UART/ESP | 原 session 终止或新 session 开始，摘要相同也不误续传 | AT-36 |
| ESP 试运行后写入共享存储再回退 | ESP | 旧应用能读已接管数据、任务与当前根；无破坏性迁移 | AT-31、32 |
| 已封口段/检查点/水位人为翻转字节 | 介质损坏注入 | 显式故障/隔离，不魔数重同步并跳过已承诺数据 | AT-08、12 |

AT 编号对应 [需求的验收清单](esp32s3-requirements.md#18-验收测试清单)。除模拟每个记录/写入边界外，实板须覆盖 Flash 写入/擦除、电压下降、实际驱动缓存和加密配置；软件模拟无法证明物理掉电窗口。

## 14. 需求追踪与实施前冻结项

| 设计章节 | 主要需求 |
| --- | --- |
| 1–4 提交与恢复 | ESP-RT-001～005、ESP-BOOT-001～007、ESP-STO-001/008 |
| 5 对象、序列与时间 | ESP-BOOT-001/003、ESP-DATA-003/007～009、ESP-CLOUD-008 |
| 6 CUSTODY | ESP-STO-002/004～007、ESP-UART-003～007、ESP-PER-002 |
| 7 定稿与上传 | ESP-STO-003/009、ESP-DATA-001～010、ESP-CLOUD-002～005 |
| 8 任务/配置/结果 | ESP-CMD-001～009、ESP-CFG-001～006、ESP-NET-005～008、ESP-CLOUD-006 |
| 8.3 本地配网与秘密引用 | ESP-PROV-008～018；ESP-AT-38/40～43 |
| 9 GATT/Peripheral | ESP-GATT-004～010、ESP-PER-001～005 |
| 10 升级 | ESP-NOTA-001～012、ESP-EOTA-001～008 |
| 11–13 GC、配额与测试 | ESP-STO-008～010、ESP-OPS-002～006、ESP-SEC-004/005 |

本文只细化 182 条需求中有关持久化的部分，不替代其余网络、BSP、BLE、安全和发布验收。

开发前须冻结并记录：

1. Flash 几何、加密写入对齐、底层持久屏障、段大小、最大事务/对象、坏块与撕裂写模型；确认记录格式提案能在该后端实现。
2. 分区/配额和一次完整 GC 的空间证明；nRF 制品暂存与双 ESP 镜像同时存在时的最坏容量。
3. 检查点/根副本发布与损坏检测方案；是否需要独立提交账本及恢复扫描的时间上限。
4. task、结果、UART 历史启动、上传批次成员表、故障及隔离对象的保留期；与平台去重期和受控维护流程一致。
5. 永久拒绝、隔离满、身份迁移、恢复出厂及水位不可读时的人工流程，不默认删除旧数据。
6. 镜像暂存校验、确认窗口内健康证据、跨版本存储兼容矩阵及迁移回退方式。
7. 目标包率、Flash 提交延迟、可容忍采集损失、寿命与恢复时间；以测量结果决定 checkpoint/GC 频率。

本轮核验只检查设计与本地需求、协议、Schema 及 nRF 行为的一致性；未执行 ESP 编译、Flash 写入或掉电实验。实现时应先完成事务故障注入与恢复测试，再接入任何会产生外部副作用的执行器。
