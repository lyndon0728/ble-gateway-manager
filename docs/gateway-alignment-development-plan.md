# 三方差距闭合开发方案

| 项目 | 基线 |
| --- | --- |
| 文档 / 日期 | 1.1 / 2026-11-19；GW32-E2E-1.1 |
| 审查源码 | 原审查基线 `b8ba3ef`；当前源码 ESP/nRF 0.3.0；SDK不变 |
| 本轮状态 | **统一实现方案及三方源码/机器合同已落实；实板验收待完成**，软件验证见[交付记录](gateway-alignment-implementation.md) |
| 不变项 | 原有功能、系统93条/ESP182条/平台59条需求及既有验收编号保留；新增过滤使用独立BFLT编号 |
| 文档权责 | 本文冻结C02～C11的跨端实施选择；C12由[Beacon过滤规范](beacon-filter-spec.md)冻结。现行接口仍以既有协议/机器合同为准，接口按联合实现与能力门控生效 |

## 1. 闭合矩阵与发布顺序

| 差距 | 已确定的唯一实现方案 | 责任与源码落点 | 关闭条件 |
| --- | --- | --- | --- |
| C01 | 以实际组件版本/验证证据描述实现，不再称ESP缺失或SDK未选 | 三方文档/发布说明 | 已消除描述冲突，后续版本持续核对 |
| C02 | MQTT与HTTPS共用任务/事件持久对象、应用ACK校验和传输所有权 | ESP gw_cloud/gw_http，新增传输适配；平台现有MQTT入口 | 同一事件/任务跨传输重试不重做、不误回收 |
| C03 | 配置读回+实际广播状态+原因证据共同判成功 | ESP gw_nrf/gw_actions，平台结果验证 | ACTIVE/合法PAUSED/STOPPED/FAULT按目标准确区分 |
| C04 | 持久恢复意图、板级受控复位、重建boot关联与OTA优先恢复 | ESP gw_nrf/gw_actions/gw_core，板级配置 | 卡死/无响应恢复不误执行副作用；真实硬件回退取证 |
| C05 | 独立Peripheral业务封装、Security2业务身份、独立持久ACK与关联回发 | 三方Peripheral桥接及平台新入口 | 手机→ESP→平台及回发均有独立授权/接管/业务确认 |
| C06 | 基于新鲜挑战与当前镜像证据的组件库存 | 平台artifacts/设备库存，ESP gw_ota/gw_nrf | nRF源版本约束可证明才执行；ESP版本不替代 |
| C07 | 按radio实施Peripheral与已有bond安全支持，统一能力交集对账 | ESP gw_ble，平台能力校验 | 未实现/未验证不虚报；自动规则仍只选nRF |
| C08 | 完整目标快照+缺组拒绝+逐组受控任务恢复 | 平台配置存储/设备API，ESP持久配置 | complete=false永不用于全量覆盖；本地覆盖保护保留 |
| C09 | 制造公钥验证nonce绑定的签名时间锚；SNTP只作候选 | 平台签名时间服务，ESP gw_network/gw_core | 冷启动不关闭TLS校验；时间误差/回拨/重启行为受控 |
| C10 | 同一采集任务分别下发等价目标，独立radio配置不隐式覆盖 | 平台编辑与ESP配置组 | 文档语义已统一，跨端用例仍须执行 |
| C11 | 持久OTA阶段+当前镜像证据决定下载中断/试启动/回滚 | ESP gw_ota；nRF既有DFU状态 | 下载中boot变化不直接判ROLLED_BACK；正常试启动不重新擦写 |
| C12 | 有版本和能力门控的组合掩码过滤，显式REPORT/PAIR | 平台预览/Schema、共享C过滤器、ESP/nRF适配 | BFLT-AT-01～16三端同结果，再完成空口验收 |

执行顺序：先冻结本文及BFLT→增加规划合同的Schema/OpenAPI/黄金向量→完成持久对象与公共校验→实现C02/C03/C04/C07/C11→实现C05/C06/C08/C09与C12→目标构建→实板故障和全功能回归。服务端先部署兼容旧版本的接收/校验器，设备验证后按能力放行；不以文档版本号给设备自动增加能力。

## 2. 新合同登记与兼容规则

下表新增合同已在本轮源码落地，路径与对象统一，并已同步OpenAPI、Schema、业务验证器、设备适配与测试；发布启用须匹配制造能力及板级验证。现有消息信封1.0/1.1及UART frame1不改；新独立对象各带自己的schemaVersion=1.0。版本不支持或缺能力时显式拒绝，不忽略新字段。

| 合同标识 | 实现入口/能力 | 内容 |
| --- | --- | --- |
| GW-CAPS-1 | `PUT /api/v1/gateway-runtime-capabilities`；签名清单增加相应功能授权 | ESP报告本次boot实际实现/板验证/nRF HELLO；平台计算可用交集 |
| GW-PERIPHERAL-1 | `POST /api/v1/gateway-peripheral/batches`；`PERIPHERAL_BRIDGE_1` | 业务事件与PERIPHERAL_PERSISTED确认，独立于RawEvent |
| GW-COMPONENT-1 | `POST /api/v1/gateway-component-challenges`、`PUT /api/v1/gateway-components`；`COMPONENT_INVENTORY_1` | 新鲜组件盘点挑战和镜像证据 |
| GW-SNAPSHOT-1 | `GET /api/v1/gateway-configuration-snapshot`；`CONFIG_SNAPSHOT_1` | 设备身份范围内的完整目标快照；本身不执行配置 |
| GW-TIME-1 | `POST /api/v1/gateway-time-challenges`；`SIGNED_TIME_1` | 不含秘密的签名时间响应，建立UTC信任后才开始普通TLS业务 |
| BFLT-2.0 | `beaconFilterV2`；平台 `NRF54_BLE_FILTER_V2` / `ESP32_BLE_FILTER_V2`、nRF bit13 | 详见过滤规范；UART 0x19/0x1A/0x1B |

除时间引导的公开挑战入口外，新设备API沿用每设备身份及TLS要求，不能使用平台管理员凭据。管理查询/操作沿用范围权限；设备只能读写自己的对象。能力增加须经制造清单授权、实际驱动可用和验证门槛，不接受正文自报即授权。

## 3. C02：MQTT / HTTPS统一执行

### 3.1 内部接口与对象所有权

新增内部传输适配层，接口职责为connect/subscribe/poll/send/cancel/receive；名字是实现设计，不是新云端命令。适配层只搬运既有消息，不保存第二份journal/spool、不自行解释应用ACK。

- MQTT沿用现有topic、QoS1、retain=false、设备用户名/ACL和TLS校验；`result-ack`保持单数。Broker地址/CA/设备凭据来自已授权制造配置，不能复用平台服务凭据。
- gw_cloud唯一分派器完成身份、任务有效期、requestDigest、持久受理和副作用意图；同taskId+同digest重复只恢复/重发结果，同ID不同内容冲突。
- 事件保持namespace/gateway/eventId/载荷，结果保持taskId/resultId/resultSequence；切换网络或协议不重建这些身份。
- 同时最多一个主动下行接收拥有者。切换流程：暂停新领取→取消旧传输请求→递增本地transportEpoch→建立新通道→恢复同一待发送队列。切换期间收到的重复命令仍走幂等分派。
- 迟到ACK不因来自旧epoch就直接丢弃：完整身份、对象及既定durability/逐项status匹配才应用；不能让旧ACK更新另一候选网络或回退reportedRevision。PUBACK只记传输统计。
- 故障退避沿用有界指数退避并加抖动，单次网络操作有deadline；不阻塞UART接管。重连后先订阅ACK/命令通道再发送积压。

### 3.2 网络候选与传输切换

候选网络仍按既有逐接口HTTPS challenge/probe验证，绑定候选接口，不能把MQTT连接成功当作替代证明。应用结果ACK绑定当前网络候选证据与任务；若走MQTT上报结果，也须证明该尝试使用已验证的候选接口，无法绑定时改用候选接口HTTPS完成事务。旧连接上的迟到结果ACK可清已确认结果，但不能单凭它提交未经验证的候选。

完成测试：同事件HTTP发送后丢响应再MQTT发送，只产生一条持久事件；命令双通道重复不重复GATT写入；PUBACK后断电不回收spool；旧epoch ACK不能提交新候选。关联ESP原云端/网络/恢复用例、平台AT-01/02/06/08。

## 4. C03：广播成功证据

事务固定为保存完整目标与原补丁→SET→GET_ADV_CONFIG→GET_ADV_STATUS→核验→持久结果。bit10缺失的旧设备使用0x08/0x09提供等价的现有状态证据；不足以证明时不标VERIFIED，不伪造GET_ADV_STATUS。

| 目标与实际 | 判定 |
| --- | --- |
| enabled=false，STOPPED，revision/模式/间隔/内容匹配 | 成功；已有Peripheral连接不因此被强制断开 |
| enabled=true，ACTIVE，完整配置匹配、无故障 | 成功；仅证明本机实际状态，空口覆盖另测 |
| enabled=true，PAUSED，完整配置匹配且有有效Peripheral连接/规定维护原因 | 可验证目标已提交，effective明确PAUSED及原因；不能显示正在发射 |
| enabled=true，STOPPED且没有合法暂停理由 | 不成功，继续有界对账或失败 |
| FAULT/读回错误/内容不一致 | 不成功；恢复旧配置，失败则保留FAULT/UNKNOWN及诊断 |

控制器异步状态可在读回期间变化；先取得配置/连接状态，再复读revision和实际状态，变化则按任务deadline有界重试。details.configuration原样回显请求，effective承载实际值，不因量化而修改原补丁。平台拒绝缺证据的成功结果。验收覆盖手机连接造成暂停、目标关闭、内容热更新部分失败、nRF复位、旧ACK和空口观察；沿用ACC-09/10。

## 5. C04：受控复位恢复

GPIO/极性/拉低时长/释放方式必须来自目标板已核对配置；本次不以历史GPIO8连接替代新PCB认证。通用代码只调用板级`assert_reset/release_reset`接口，默认禁用未经验证的主动复位。正常释放保持高阻/既定开漏语义，不能用推挽高驱动共享复位线。

持久recoveryIntent至少包含本地恢复ID、原因、旧boot、关联taskId或明确LOCAL来源、OTA阶段、将失效的连接/时间映射、恢复预算、板配置版本和数据接管水位。进入维护互斥后：

1. 尝试同boot重握手/状态查询，普通UART超时不立即复位。
2. 在预算内接管可读取的残余FIFO；若控制器卡死无法读取，记录其RAM数据可能丢失的边界，不伪造CUSTODY。
3. 未决物理副作用保留UNKNOWN，不能通过复位后重放来掩盖。先持久RESET_INTENT，再断言/释放复位。
4. HELLO必须证明新boot；重建link，失效旧连接引用、时间锚与PAIR缓存。未确认OTA优先核当前镜像及回退，再恢复其他已提交目标。
5. 成功/失败均持久原因与结果，限本次恢复预算内一次复位尝试；再次失败保持故障、退避并要求明确新恢复决策，避免无限复位。

无云任务时仍允许既有受控本地恢复，但必须有本地原因与相同约束。真实验收包括卡死新镜像回退、复位线共享调试器、掉电于RESET_INTENT前后、复位后旧通知/ACK不能复活连接；沿用OTA-011、ACC-16。

## 6. C05：Peripheral双向业务

### 6.1 身份、承载和授权

继续使用nRF固定INFO/RX/TX服务及0x22/0x0E/0x0F，不改变UUID或把手机RX当作管理命令。nRF仅转发；ESP完成重组、认证和业务路由。链路加密与业务身份独立，公开BLE可写不代表有配置/升级权限。

认证复用工程已锁定的protocomm Security2实现（SRP6a和认证加密），使用**独立的Peripheral业务客户端身份/凭据与权限**，不共享现场配网管理员会话或自动取得OTA/网络权限。凭据通过既有授权制造/维护流程写入加密存储；服务端登记clientId及业务route权限，不从未认证payload自报clientId授信。离线时仅已预置授权可使用，未知客户端拒绝。

协议组件支持业务扩展、可插入传输及Security2的SRP6a/AES-GCM会话，依据[ESP-IDF v5.5.6官方说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/provisioning/protocomm.html)。本项目的分片、客户端权限与平台业务桥接已由独立gw_peripheral和平台模块实现；SDK密码接口与业务持久确认分别测试。

BLE承载固定12字节片头：`magic[2]="PG",version:u8=1,type:u8,transportSeq:u32,totalLength:u16,offset:u16`，数值小端，后接非空fragment。type=0承载Security2会话protobuf，1承载认证后的加密业务信封，2承载认证后的加密应答；type不赋予身份。totalLength上限4096，offset连续。transportSeq每连接、每方向分别从1单调递增，u32耗尽须重建会话；应答使用发送方向自己的新序号，不回显对向序号。最近相同片可重试，同序号不同内容拒绝。每连接每方向最多一个在途重组，5s固定预算、不随分片续期；断链/boot变化清理并废除会话。

单次RX/TX包括片头的总长度≤min(512,MTU−3)，普通MTU23仍可携带8字节分片。TX须先订阅，按0x0E的tx_seq幂等受理；TX受理/ATT成功不代表业务送达。所有Security2包和重组边界做长度/状态校验，禁用自制加密算法；抗重放由会话密码状态与业务ID共同约束。

认证后的明文业务信封固定：`{schemaVersion:"1.0",transportSeq,businessId,inReplyTo?,route,payloadBase64}`，业务payload≤2048B，route为登记的业务路由；transportSeq必须与片头相同，clientId由会话确定。businessId为随机128位ID的32位小写hex，由业务发送者创建并持久保留，跨重传/重连不变；inReplyTo仅引用被答复的businessId。业务去重键为(namespace,gateway,clientId,方向,businessId)，相同ID不同route/业务payload冲突，不能重连后重做副作用。无业务授权、过期会话、错误完整性标签或重放均不能进入平台业务队列。原始UART持久INBOX仅用于接管，不作为授权证据。

### 6.2 平台事件与独立持久确认

事件批次：`{schemaVersion,namespaceId,gatewayId,batchId,events:[...]}`；最多16项、每项业务payload≤2048B、完整JSON≤128KiB。事件字段固定为`eventId,radio,controllerBootId,connectionRef,clientId,transportSeq,businessId,inReplyTo?,route,payloadBase64,receivedAt?,timeQuality`，radio为NRF54/ESP32。timeQuality仅SYNCED/UNSYNCED；SYNCED要求RFC3339 UTC毫秒receivedAt及有效时间锚，UNSYNCED省略该字段。连接引用增加角色域，使用新Peripheral引用算法，不能碰撞Central引用：SHA256(`"gw32-peripheral-v1"`ASCII + radioByte + boot16 + conn_idLE4)前16字节小写hex；radioByte仍0nRF/1ESP。

平台按namespace/gateway/eventId与规范内容摘要幂等事务写入，再返回`{schemaVersion,namespaceId,gatewayId,batchId,durability:"PERIPHERAL_PERSISTED",items:[{eventId,status,code?}]}`；逐项status为ACCEPTED/DUPLICATE才可回收对应业务spool，冲突/暂不可接管保留或隔离。不能复用RAW_PERSISTED而混淆RawEvent。MQTT对应新topic为既有设备前缀下`peripheral-events`/`peripheral-ack`，QoS1、retain=false，内容与HTTP相同。

逐项status只允许ACCEPTED/DUPLICATE/RETRY/REJECTED。RETRY的code仅CAPACITY_TEMPORARY或INTERNAL_TEMPORARY，保持原对象退避重发；REJECTED的code仅NOT_AUTHORIZED、INVALID_EVENT、EVENT_ID_CONFLICT或BUSINESS_ID_CONFLICT，先将原文/原因持久移入隔离，不当作已持久业务成功回收。两个成功状态禁止code；身份、批次、逐项eventId及durability必须全匹配。401/403、缺项或无法解码的回复不释放对象；未知状态/code保留并告警。平台另按业务去重键处理不同eventId携带的同一businessId，不能重复调用业务处理器。

UART接管INBOX→认证/重组→业务spool的移交须同一可恢复事务标记，防止既丢原片又没事件；未认证数据有独立限额及过期清理计数，不能无限占Flash。平台只信认证设备的转发声明；这不是手机硬件证明，部署信任边界须如实标识。

Security2会话密钥不要求跨启动持久化；ESP重启/断链后，尚未完成认证并提交业务spool的原密文片段标记本地SESSION_LOST并保留原因计数，不能宣称业务已接收或已上传。手机仅在收到STORED后才可认定网关持久收件；没收到时重新认证，以相同businessId及业务内容重传。UART CUSTODY只证明原片已由ESP保管，不代替这个业务接收ACK。已解密且已持久的业务spool无需旧会话密钥即可继续上传。

业务去重摘要只包含clientId、方向、businessId、route、payloadBase64及inReplyTo，不包含transportSeq、controllerBootId、connectionRef、receivedAt等会变的传输证据。已有eventId的事件元数据保持不变；重连产生的新eventId可以关联同一businessId，由平台业务账本返回既有处理结果，不能因连接变化误判业务冲突或重复执行。

### 6.3 下行任务与业务应答

新任务`PERIPHERAL_NOTIFY`参数固定`radio,connectionRef,clientId,inReplyTo?,businessId,route,payloadBase64,timeoutMs`；businessId由平台在任务受理时生成并稳定保留，transportSeq由ESP在持久发送意图中按本方向分配，不由平台猜测。`PERIPHERAL_DISCONNECT`参数固定`radio,connectionRef,clientId`。timeoutMs为100～30000且不超过任务TTL。平台校验路由权限/当前客户端关联，ESP再次校验相同boot/连接/会话/client，禁止旧连接复用误投。

ESP持久发送意图，分片加密发送；对端type2明文为`{schemaVersion:"1.0",transportSeq,inReplyTo,status,responseBase64?}`，inReplyTo引用业务ID，status仅STORED/APPLIED/REJECTED，responseBase64解码≤2048B。STORED仅表示接收方已持久收件，APPLIED才表示业务处理成功；NOTIFY收到匹配APPLIED才可SUCCEEDED/VERIFIED，STORED或本机受理仍为EXECUTING。断链/超时且送达不明为UNKNOWN，不能重新投递副作用；REJECTED为明确业务失败。DISCONNECT以实际释放连接为完成证据。上行业务也只在本地持久后发送STORED，平台持久ACK不自动生成APPLIED；平台业务处理完成后经关联回复表达结果。

验收：旧手机未认证写入不能改网络；分片乱序/断链/重放、同ID不同内容、平台ACK丢失、手机ACK丢失、连接号复用、inbox/spool满及离线预授权均有明确结果。沿用PER-001～006、ACC-11，增加规划合同测试而非删减业务。

## 7. C06：可信组件库存

平台主字段firmwareVersion继续表示ESP。独立库存键为(namespace,gateway,component)，component仅GATEWAY/NRF54；保存当前boot、版本、imageSha256、artifactId、bootConfirmed、evidenceLevel、证据任务/挑战、服务器接收时刻。普通HELLO只给版本字符串，最多VERSION_ONLY，不能自动满足可信源版本约束。

设备向挑战入口请求`component`，平台返回一次性nonce（32随机字节Base64）、challengeId、inventoryGeneration（组件独立u64十进制字符串）和60s服务器有效期。ESP在收到挑战后读取当前身份和镜像证据，再PUT组件报告：`schemaVersion,namespaceId,gatewayId,challengeId,nonce,inventoryGeneration,component,controllerBootId?,evidenceLevel,firmwareVersion?,imageSha256?,artifactId?,bootConfirmed?,upgradeTaskId?`。平台以设备TLS身份/作用域、nonce/时效/一次性、制品签名元数据与任务证据核验；相同挑战同内容幂等，不同内容冲突。每组件最多一个未决挑战，新挑战使旧未决挑战失效；更新库存CAS匹配inventoryGeneration并递增，迟到旧报告不得覆盖。

NRF54的CONFIRMED_ARTIFACT等级要求：已知已签名制品或授权制造清单绑定的镜像、当前HELLO启动ID/版本、针对该制品size/SHA执行成功的VERIFY_RUNNING、DFU_STATUS确认状态。无法提供这些证据或无相应DFU能力则保持VERSION_ONLY/UNKNOWN，不能伪造SHA。GATEWAY用当前运行分区校验及boot确认；本地离线升级联网后同样重新盘点。

evidenceLevel只允许CONFIRMED_ARTIFACT/VERSION_ONLY/UNKNOWN。CONFIRMED_ARTIFACT必填真实firmwareVersion、imageSha256、artifactId及bootConfirmed=true；制造初始化镜像也须导入可核验的制品记录。VERSION_ONLY只必填当前读到的firmwareVersion，禁止填未知SHA/制品或true确认；UNKNOWN不填版本/镜像/确认字段，controllerBootId不可读取时也必须省略而不伪造。身份/VERIFY_RUNNING/确认状态前后复核同一boot，任一步重启立即废弃混合证据并重新盘点。新boot或低等级报告使旧boot的可信库存失效，旧证据仅留历史，不用于当前源版本约束。

相同挑战同内容重投返回原收据、原服务器evidenceReceivedAt及原盘点结果，不延长60s期限、不再次推进库存世代；只有新挑战与重新采证能刷新证据。服务器按持久挑战状态实现幂等，不能让重复PUT成为旧镜像证据永久保鲜的途径。

任务创建时检查库存；执行前再次挑战确认当前源组件，原版本约束仍生效，证据过期/boot变化则刷新或拒绝。固件执行器再检查当前组件，避免创建任务后组件已变化。若无法证明则继续使用既有`FIRMWARE_COMPONENT_VERSION_UNKNOWN`拒绝，不能用ESP版本或单独版本字符串绕过。

“可信”指受认证ESP在设备信任边界内的当前盘点加签名制品关联，不是nRF独立远程硬件证明。平台后收到旧boot报告不得覆盖更新证据；挑战绑定当前受理盘点世代并用服务器CAS更新。验收包含同版本不同SHA、回滚、boot变化、迟到旧挑战、本地ESP升级和库存损坏。

## 8. C07：实际能力交集和ESP本机BLE

能力对账输入是签名清单的授权上限、ESP编译实现/实板启用项、当前nRF HELLO、角色资源及固件版本。GW-CAPS-1对象为`schemaVersion,namespaceId,gatewayId,espBootId,nrfBootId,manifestDigest,espImplemented,nrfHelloCapabilities,boardVerified,unavailableReasons`；列表为确定能力字符串，完整值在Schema枚举中登记，不能任意文本当授权。

平台验证已保存的签名清单，返回交集及原因。ESP保存并验签同一制造清单原文/签名，用本机实现和HELLO再次求交集；没有清单或验签失败不能开放新增能力。网络报告只是可见性，不代替执行前再次检查。角色暂时忙与功能未实现分别表达，不能把暂停永久取消能力。

ESP本机完善可连接广播与Peripheral服务，按既有服务/业务合同实施；Central/Peripheral连接预算、扫描/广播并发以实测组合门控。BONDED_ENCRYPTED只使用安全预置的已有bond并验证实际链路安全级别，未找到bond明确拒绝；首次配对/通用配对管理仍属原P2范围，不静默新增自动配对。平台自动连接规则保持NRF54目标，不因补ESP支持就改投ESP。

用例：把nRF能力误填到ESP、无bit13下发V2、缺Peripheral下发可连接广播、无bond请求加密、当前资源忙、nRF重启能力减少、制造签名错误都须与平台/设备一致。保留原GATT/广播需求，不以UNSUPPORTED作为功能完成证据。

## 9. C08：完整目标快照与受控恢复

平台另存各组已确认完整目标，和当前desired/reported补丁查询并存；不把已有`complete=false`视图改名冒充完整快照。快照包含`schemaVersion,namespaceId,gatewayId,snapshotId,generation,complete,groups,missingGroups,blockedReasons,digest`，generation为u64十进制字符串，digest为除digest外对象的既定规范JSON SHA256。missingGroups只列下述固定组名；blockedReasons为UNKNOWN_TASK、LOCAL_OVERRIDE、MISSING_BASELINE、PENDING_CONFIGURATION的非重复子集；complete=true时两数组必须为空，所有组必须齐全。

固定组为`network,nrf54Scan,nrf54Advertising,esp32Scan,esp32Advertising,upload`；每组包含`revision,value,source,verifiedAt`，source为`{kind,id}`，kind仅VERIFIED_TASK/FACTORY_MANIFEST/LOCAL_ADOPTED，id分别引用成功任务、已验签制造清单或采用本地目标的成功对账任务。verifiedAt是平台完成来源核验的UTC时间，不是当前设备读回时间。revision沿平台安全整数约束，制造初值允许0；value是完整目标且无明文凭据。旧顶层scan/filters归一到nrf54Scan，所有组用其Schema默认值补齐仅限有可证明初始化基线；不能凭空把缺失历史组填默认后称完整。

只有每个组都有受确认的完整基线与按顺序应用的成功补丁，且不存在UNKNOWN任务/冲突时complete=true；否则complete=false并列明缺组/阻塞原因。最后一次补丁不等于最后一次完整值。完整目标快照不是实时硬件读回，verifiedAt说明其证据时间，不伪装当前无线状态。

ESP接收快照后校验身份、digest、generation、各组revision及完整字段，仅写恢复候选。执行规则：

1. 本机目标仍完整且有localOverride时不覆盖，先按既有ADOPT_LOCAL/APPLY_PLATFORM对账；网络凭据只引用已安装credentialRef，缺凭据拒绝自动切网。
2. 本机目标丢失则只自动恢复旧身份、journal/spool等可证明数据，不因下载快照而立即执行物理配置。
3. 平台使用既有配置任务语义逐组生成受控恢复任务，标明来源snapshotId（已增加该受审计引用），分配新的desiredRevision；每网关仍一个在途任务，组间不声称原子事务。
4. 每组写候选→实际执行/完整读回→稳定结果/应用ACK→提交；失败停止后续组并保留已完成组/剩余组清单。新运行revision可大于快照历史值，禁止拿旧revision覆盖设备更新目标。
5. 网络组先保当前可达接口，候选探测后提交；本地覆盖、在途OTA和UNKNOWN副作用阻止恢复，不通过cancelQueued绕过。

验收覆盖缺一组、历史有UNKNOWN、digest错误、重复/乱序generation、旧快照、凭据缺失、离线本地覆盖与中途断电。完整恢复功能由可证明基线+显式恢复任务闭合，不添加一个无授权的“直接恢复全部”旁路。

## 10. C09：签名时间引导

### 10.1 信任和线格式

制造配置写入独立用途的Ed25519时间服务公钥及keyId，密钥管理与固件签名用途隔离。公开挑战请求不含设备token、密码或可复用授权：`{schemaVersion:"1.0",nonce,bootId}`；nonce为32字节强随机Base64，bootId为当前ESP16字节ID的32位小写hex。服务器对请求限流，返回`{keyId,payloadBase64,signatureBase64}`。

签名内容为ASCII域前缀`gw32-time-v1`后接解码的payload原始UTF-8字节，不先重排JSON再验签。payload字段固定`schemaVersion,nonce,bootId,issuedAtUnixMs,uncertaintyMs,validForMs`；issuedAtUnixMs为无前导零的u64十进制字符串，uncertaintyMs为0～1000的JSON整数，validForMs为固定JSON整数60000。签名长度64B；payload≤1024B、整个响应≤4096B，拒绝重复键、未知字段、非规范数值及不受信keyId。

时间包可经普通不可信传输搬运，但该通道只取无秘密签名时间包，不能承载设备凭据或其他API；正常业务仍强制TLS CA/主机名/日期校验。签名/nonce/boot绑定提供真实性与新鲜挑战关联，网络阻断仍可导致不可用，不能为可用性退化为不验TLS。

### 10.2 UTC区间与动作期限

ESP独立时间worker记录发起/接收单调时间，要求往返≤2000ms，nonce为本次尚未消费值、boot相同、签名正确。100ppm是待板级验证的单调时钟漂移上限，未经验证不能提升时间为可信；若实测上界更大则先更新受审查的预算与向量。定义q=2ms+ceil(RTT×100ppm)，覆盖时间戳量化及测量漂移；接收时UTC保守区间为`[issuedAt−uncertainty−q, issuedAt+uncertainty+RTT+q]`，以后按单调时长推进，下界减、上界加ceil(age×100ppm)。所有加减、转换及deadline计算必须检查溢出/下溢。

validForMs从签名的issuedAtUnixMs起算，可信截止UTC为issuedAt+60000；本机上界到达该值即失效，不是收到旧包后重新获得完整60s。接收时可用单调预算最多60000−uncertainty−RTT−q，随误差增长可进一步缩短；到期前必须取得新nonce的签名锚。服务器签名端必须有经运维验证的时间来源，否则停止签发，不签一个自称准确的普通错误时钟。

平台任务尚未到期的判断使用UTC上界：上界≥expiresAt则不能开始新副作用；任务创建时间/证书notBefore用下界检查。TLS验证必须在整个可信区间内满足日期约束。已受理动作固定单调deadline，后续校时只可缩短，不能延长或复活已过期动作。

旧源码`clock_synced && year>=2025`已替换为有效签名锚/误差区间状态；SNTP仅提供候选或诊断，不能单独把状态升为可信。锚过期停止新远程授权动作，保留持久数据和安全对账；不会删除离线采集。

本地配网及本地OTA的现场认证、单调窗口、签名镜像、离线试启动/恢复均不增加互联网或UTC前提。重启后旧单调锚失效；远程恢复须重新建立时间证据，本地恢复按LOCAL授权journal继续既有流程。

测试：伪签名、nonce复用/错boot、2s以上延迟、服务器误差超限、系统时钟回拨、锚过期、重启、任务截止边界、TLS日期边界；以及断网条件下本地OTA仍可按授权完成。时间公钥轮换/制造更新按受授权配置流程实施，不允许网络自行替换根公钥。

## 11. C11：nRF升级分阶段恢复

恢复先读取持久阶段、旧boot、目标SHA/大小/版本、session与授权预算，再读取新HELLO/DFU_STATUS。boot变化只是证据之一；不得无条件进入nrf_trial或直接标ROLLED_BACK。

| 持久阶段/当前状态 | 规定动作 |
| --- | --- |
| 尚未BEGIN，旧/新boot | 核能力、授权与已确认运行镜像后，可建立新的下载意图；不能借旧offset续传 |
| BEGIN_INTENT/TRANSFERRING，boot未变 | 查session/大小/SHA/accepted_offset；一致才按原序列续传，冲突停止对账 |
| TRANSFERRING，boot变化 | 记下载中断，旧session/offset失效；旧镜像仍运行不是候选回滚。允许失败并重新授权；若有效恢复策略允许重传，重新BEGIN从0，不要求同任务自动重试 |
| FINISH_INTENT，是否设置TEST不明 | 先核DFU状态、当前镜像与确认状态；禁止先ABORT擦槽，保留UNKNOWN直至可证实 |
| READY/REBOOT_INTENT，boot未变 | 同session核READY，可在授权内执行受控重启；响应丢失先查boot不盲重启 |
| READY/重启后新boot | 核实际运行目标SHA；目标运行且健康→VERIFY_RUNNING→CONFIRM→STATUS确认；旧镜像且有交换/失败证据才判回退 |
| CONFIRMING/CONFIRMED，结果或ACK丢失 | 验证当前运行与确认状态，重发稳定结果；不重新下载、不重复刷写 |
| 任一阶段无法通信/镜像不明 | 按C04受控恢复，未决副作用/确认不明保留UNKNOWN；不能假定成功或自动清除升级租约 |

VERIFY_RUNNING参数仍为目标size/SHA，版本字符串相同不足以证明镜像。nRF确认窗口仍默认120s，普通DFU等待30s不得越过剩余预算。远程任务到期不能新增擦写/确认授权，恢复先保护已运行镜像和证据；本次不增加bootloader自升级、跨掉电offset续传或新安全配对功能。

表内TRANSFERRING是现有ESP的nRF传输journal阶段；nRF DFU状态RECEIVING和ESP本地上传RECEIVING属于不同状态机，不能用后两者代替journal分支。

验证在BEGIN、每块、FINISH前后、设置TEST、发送REBOOT、HELLO、VERIFY_RUNNING、CONFIRM、平台结果ACK各点注入重启/断电，证据分别落在正确阶段；沿用ACC-13～16和ESP OTA验收。

## 12. 联合开发门槛与文档维护

| 门槛 | 必需产物 | 不能替代的证据 |
| --- | --- | --- |
| D0 设计对齐 | 本文、BFLT、两层协议、三方需求/开发文档中的名称、单位、状态、版本一致 | 不表示规划API已上线 |
| D1 机器合同 | 新对象Schema/OpenAPI、能力枚举、长度/身份/错误校验、往返向量 | 不能只写UI或一侧解析器 |
| D2 实现与软件验证 | 原功能回归、各缺口故障测试、BFLT跨端向量、真实生产函数测试 | 不把替身通过当空口/Flash通过 |
| D3 目标构建 | 固定SDK、两个芯片各变体、依赖锁、产物SHA与容量记录 | ESP构建不替代nRF构建 |
| D4 实板贯通 | 网络切换、UART背压、真实Beacon/手机/GATT、双OTA和掉电回退 | 文档/模拟器不等于整机通过 |
| D5 发布 | 先兼容服务端，再经验证设备，最后按能力启用新功能；回滚到旧固件前恢复旧过滤目标 | 不让旧固件静默忽略V2配置或未知持久版本 |

各差距只有D1～D4对应证据齐备才标“实现闭合”；0.3.0已完成D1/D2的软件实现和验证，ESP有D3目标构建，nRF目标构建及D4实板证据尚缺。新增持久对象/配置引擎须版本化保存，旧固件不认识时拒绝危险恢复并进入明确维护状态；降级前必须备份和迁移目标，不能把未知字段当空配置。所有后续记录继续使用Markdown，源码/Schema保留原生格式。

## 13. 前版设计校核记录

2026-11-19前版设计交付完成17份Markdown一致性检查：294个相对文件链接、14个章节锚点、15个JSON示例及代码围栏通过；系统93条需求正文及18项ACC、ESP182条需求/50项验收、平台59条需求/11项验收的既有编号与顺序保持，新BFLT为18条需求/16项验收。系统PER-005仅更新实施状态说明，需求正文不变。

独立计算核对了名称Base64、iBeacon字段偏移/端序、RSSI范围/位掩码、87B完整配置向量、3070B预算及96B状态布局。交叉审阅修正了未知RSSI默认放行歧义、PAIR边界/计数、双向业务序号/掉电重传、组件证据条件字段、快照来源、时间有效期及OTA阶段名称。上述记录属于当时仅文档的交付，不代表0.3.0源码测试或目标构建结果。

## 14. 本轮实现补充

实际状态、制造键、部署和验证见[实现记录](gateway-alignment-implementation.md)。GW-CAPS-1 的 nrfBootId 在当前nRF身份不可读时为null，HELLO列表为空；不构造假boot。GW-COMPONENT-1 增加设备只读 `GET /api/v1/gateway-component-artifact?component=...&sha256=...`，只把本机已验签/实际重读的镜像关联到平台已验签制品，不提高证据等级。GW-SNAPSHOT-1 支持按 `?snapshotId=...` 取不可变历史；已有配置API/任务接收 sourceSnapshotId，逐组匹配完整目标并分配新revision。

制造阶段100ppm漂移/射频组合/复位脉冲的实测门槛不因本轮编译通过而免除。当前ESP默认硬件复位关闭，nRF默认GW_RADIO_VERIFIED=n。
