# ESP32-S3 0.3.0 实现决策与交付范围

日期：2026-11-19。本文将[开发设计](../../docs/esp32s3-development.md)映射到本次源码；它不替代[验证记录](VALIDATION.md)或[50项整机验收](../../docs/esp32s3-validation-plan.md)。C01～C12 的三方实现、制造增量和兼容边界见[对齐实施记录](../../docs/gateway-alignment-implementation.md)。标为历史的段落保留旧版决策背景，其余内容描述 0.3.0。

## 两层协议与模块

服务器通过 HTTPS 或 MQTT 下发任务，ESP 负责身份、授权时限、持久受理、网络配置、结果重传及应用 ACK。两种传输共用任务 journal 与上行 spool，切换传输不重新执行已受理副作用。ESP 与 nRF 之间沿用 UART 1.1；nRF 负责其射频控制和采集，服务器不直接向 nRF 下发 JSON。UART 命令响应、CUSTODY、MQTT PUBACK、平台 RAW_PERSISTED / RESULT_PERSISTED / PERIPHERAL_PERSISTED 各有独立语义，不能互相替代。

| 逻辑职责 | 实际实现 |
| --- | --- |
| persistence、config、credential、network executor | `gw_core`；公开接口在`include/gw_core.h` |
| provisioning和本地网页 | `gw_provision`；Security2会话内调用核心接口 |
| HTTPS/MQTT、任务日志、结果重传 | `gw_transport` 统一选路与 MQTT 接入；`gw_http` 负责 HTTPS/TLS；`gw_cloud` 共用持久任务与应用 ACK |
| 签名时间与授权预算 | `gw_signed_time_verify` 校验独立时间签名；`gw_core/gw_time_bounds` 管理保守 UTC 区间及单调期限；TLS 核完整有效期区间 |
| 制造授权与验签 | `gw_authority` 核验原始制造清单、身份、摘要和板级能力；`gw_crypto` 提供标准 Ed25519 验证 |
| 启动盘点与配置快照 | `gw_cloud/gw_sync` 管理挑战/世代、独立组件镜像证据、完整候选快照和来源校验 |
| UART codec与nRF控制 | `gw_wire`直接编译既有纯C模块；`gw_nrf`提供驱动及协议适配 |
| observation、dedup、raw outbox、peripheral inbox | `gw_capture`及可主机编译的`gw_spool` |
| scan/advertising、GATT、OTA coordinator | `gw_actions`；分别调用ESP NimBLE、nRF UART及OTA实现 |
| 手机双向业务 | `gw_peripheral` 使用独立 Security2/ACL、PG 分片、持久业务 spool、平台 ACK 和手机 APPLIED；不进入网络配置/OTA 管理处理器 |

SDK固定ESP-IDF v5.5.6，BLE Host采用NimBLE，CH390组件固定0.4.1。默认8MiB Flash与Oct29 UART接线基线不代表后续修订板卡已验证。CH390和现场按钮GPIO需按板卡设置。

## 存储后端选择

原设计中的统一追加日志、检查点和通用GC没有按原字节格式实现。本版将低频配置/动作状态与高频观测分开，以减少扫描接管时复制配置树的开销；每项必须一起提交的不变量仍限定在同一后端内。

| 域 | 持久发布方式 | 原子边界 |
| --- | --- | --- |
| `gwstore`加密NVS | 先写另一份完整快照并commit/readback，再commit/readback世代绑定指针 | 同一`gw_store_transaction`中的配置、意图、结果和相关计数器 |
| `gwspool`加密Flash | 记录先完整写入并读回，再发布带世代和校验的轮转根 | nRF批内记录＋接管水位；观测配对＋事件身份；逐项交付状态 |
| `gwstore/secrets` | 独立加密NVS命名空间，安装持久成功后才发布可用凭据引用 | 秘密安装与后续引用按先后屏障衔接；未被引用的孤立秘密不等于配置提交 |
| 默认 `nvs` / `nimble_bond` | 使用既有制造 NVS 密钥显式 secure init；按锁定 SDK 格式预置 bond | BLE host 启动前检查原始记录，SDK 恢复后核安全材料和数量；异常停止 BLE 启动，不自动清库 |
| nRF镜像/ESP OTA槽 | 专用分区/官方OTA API，持久动作journal保存阶段与证据 | 镜像校验、试启动与确认依赖恢复状态机，不宣称跨分区单次原子写入 |

业务对象由各后端互斥串行提交，不以通用单写入任务API暴露。持久写入结果模糊或已发布数据损坏时停止后续接纳，不自动清空日志、不自动选择旧快照重做动作。CRC/SHA用于损坏检测；介质的真实断电保证需板上验证。

`gw_store_init()` 显式初始化 `gwfactory`、`gwstore` 和默认 `nvs` 三个加密分区，均使用已有制造密钥；不调用自动生成密钥或写 eFuse 的默认路径。NimBLE 的恢复函数仅打印错误不足以确认 bond 就绪，因此额外执行 `gw_ble_bonds_check(false/true)` 前后检查。空 bond 库允许基础 BLE，但 `BONDED_ENCRYPTED` 请求因缺绑定而失败；旧明文或不兼容分区必须经过受授权制造迁移，设备不会自行擦除或重新生成密钥。

`gwstore`完整编码快照上限96KiB，最多256个键，值的Base64编码也占预算。单任务先预留结果空间；任务身份最多保留64项，还受总容量限制。回收ACK后的大载荷仍保留任务摘要与终态。达到预算时拒绝新副作用，不能无限接收任务；长期部署需要另行实现有明确平台保留窗口的归档策略，不能直接清空防重记录。

`gwspool`容量1MiB，固定记录槽与轮转根共同占用该容量。单条小广播也占一个记录槽，因此不能用1MiB除以广播字节数估算离线时长。具体槽数、根轮转和测试见[采集组件](components/gw_capture/README.md)。满盘时返回BUSY并保留已接管数据；永久拒绝进入隔离，不把它当ACK回收。

## 配网与平台闭环

本地入口与远程入口共用网络执行器及排他租约。本地版本使用`localRevision`，所有网络变更推进`networkGeneration`；服务器`desiredRevision`由服务器分配。LOCAL_ONLY允许现场保存但保留最后验证成功网络，不能伪报平台已确认。

远程凭据单独通过设备认证接口投递；CONFIGURE仅引用credentialRef。候选验证把DNS与TLS连接绑定被测接口，平台逐项确认EXECUTING探测结果后才提交。最终SUCCEEDED获得RESULT_PERSISTED后更新平台确认版本并解除租约。旧候选回退后，未确认的旧probe不会经同名旧接口重发成新证明。

历史决策（0.2.1，0.3.0 保持）：将已报告配置版本作为单调水位，只有匹配的成功结果 ACK 可推进，迟到旧结果、DUPLICATE、错误 ACK 或不安全数值不会使版本倒退。ACK 删除与水位推进仍由同一 journal 提交，不额外写入另一份可被旧任务覆盖的版本。

历史基础：网络配网功能随平台数据库 v4 迁移、OpenAPI/消息 Schema 和网络配置页面交付；[平台联调文档](../../server/docs/network-provisioning-integration.md)包含部署主密钥、权限、请求与结果契约。0.3.0 的对齐实现新增迁移 5，不修改已发布迁移 1～4；相应平台扩展必须与固件一起部署。

配置盘点按完整组保存不可变快照。下载快照只保存候选，不直接执行；恢复通过带 `sourceSnapshotId` 的普通 CONFIGURE 任务，检查完整值、来源、revision、凭据和本地覆盖后执行。一次恢复一个完整组，不能用快照绕过 UNKNOWN 或未决副作用。镜像库存分别记录 GATEWAY/NRF54，缺少当前镜像、boot 或确认状态时保留 VERSION_ONLY/UNKNOWN。

## 无线过滤、广播与手机业务

两芯片共享 BFLT-2.0 引擎：名称、UUID、RSSI、major/minor 和服务 UUID 使用统一组合掩码，支持 REPORT/PAIR、有界输入队列、固定 PAIR 窗口和 96 字节状态。旧 ScanProfile 保持兼容，V2 只在 `NRF54_BLE_FILTER_V2` / `ESP32_BLE_FILTER_V2` 授权与实际能力均满足时执行，完整配置原子切换并读回，不静默降级。

广播完整配置读回后还须验证实际 ACTIVE/STOPPED 状态；PAUSED 只有真实 Peripheral 连接才能作为成功证据。当前单连接与已验证角色预算保持：没有扫描/广播并发证据时，完整目标同时启用两者明确拒绝，不修改平台目标字段。切换角色在同一配置事务中表达停止旧角色和开启新角色；失败恢复旧目标，恢复不确定则保持 UNKNOWN。

本机支持三种 legacy 广播模式、固定 INFO/RX/TX Peripheral 服务和已有预置 bond 加密；首次配对不开放，缺 bond 或实际加密不足不会降级明文。`ESP32_BLE_PERIPHERAL` 与 `PERIPHERAL_BRIDGE_1` 分别控制无线服务与业务桥接，并受制造授权及实板验证门控。自动连接规则仍仅针对 NRF54。

手机业务采用独立 Security2 凭据及 route ACL，不能复用配网管理员。BLE 回调仅有界复制，ATT 写成功不是持久接管；业务记录与去重摘要落盘后才应答 STORED，平台 PERIPHERAL_PERSISTED 才释放上行记录。下行通知仅在同连接/身份/业务 ID 的加密 APPLIED 后成为 VERIFIED；丢失会话或结果不确定保持 UNKNOWN。详细边界见 [Peripheral 业务组件](components/gw_peripheral/README.md)。

## 本地与远程 ESP OTA

`gw_actions/gw_ota.c` 复用制造发行公钥、原始 manifest 验签、非运行槽写入和运行镜像核验。远程任务继续使用 taskId/digest/TTL 与云端持久结果；本地使用独立 source=LOCAL/uploadId 和 `ota/local`，与远程 `ota/active` 分开，不能把本地结果合成为不存在的云任务或配置 revision。

`gw_provision` 的已认证 Security2 业务通道转交五个 OTA 操作；每块最多 4096 字节，BEGIN 固定本次窗口期限，空闲 5 min 中止。仅最后一块完全相同字节可重试，接收 offset 保留 RAM，避免每块重写整个 NVS 快照。RECEIVING/READY 经掉电或窗口收尾后不自动安装；COMMIT 持久意图和设置启动槽后安排重启，恢复以实际启动槽和镜像为准。

本地试运行确认不等待服务器或 UTC，使用持久签名安装授权及 90 s 限时健康流程；核心、接管、nRF、动作及配网初始化、至少 10 s 启动观察、实际镜像与 nRF 健康条件满足才标有效。远程继续受其授权及网络恢复约束。本地/云端动作共用维护租约，网络候选和另一组件 OTA 不能抢占。完整协议、打包和状态见[OTA 设计](../../docs/esp32s3-ota.md)；真实硬件回滚必须另有证据。

nRF UART 恢复先做有限重握手，再按已持久恢复意图、维护租约及已验证板参数至多执行一次受控复位，复位后核验新的 boot。`CONFIG_GW_NRF_RESET_VERIFIED` 默认关闭；默认 GPIO 和脉冲参数不等于制造或实板验证，不允许据此开启复位能力。传输阶段重启按真实阶段报告中断；FINISH 结果不确定不能虚构回滚或成功。

## 明确边界

- 当前上行支持 HTTPS、MQTT 与 AUTO 选路。MQTT 固定 TLS、QoS1、retain=false；基础订阅就绪后才可用，应用 ACK 之前不回收持久数据。网络候选验证仍通过绑定被测接口的 HTTPS。
- 当前远程时间信任来自独立制造时间公钥验证的 nonce/boot 绑定时间包，并按已验证时钟漂移预算扩展保守 UTC 区间。SNTP 不单独建立信任；TLS 日期必须覆盖完整区间，任务受固定单调预算约束。缺少新时间信任材料时拒绝普通远程 TLS 业务，本地认证配网和本地 OTA 保持可用。
- 出厂安全材料由工具生成，实际Flash加密、NVS密钥烧录、eFuse设置属于制造步骤；固件不自动执行不可逆制造操作。
- 未认证 Peripheral 原始片段仅进入独立有界接收路径；只有独立业务认证和授权通过的信封才进入业务 spool。已实现的手机双向业务合同不授予网络配置、OTA 或任意写入权限。
- 无物理网关测试证据时，不声明CH390、射频共存、Flash寿命、手机兼容、真实双组件升级或72小时稳定性已达标。

历史边界（0.2.x）：当时设备仅使用 HTTPS，时间依赖 SNTP/合理 UTC，手机双向业务合同尚未实现；这些是旧版限制，已由以上 0.3.0 实现替代。

以上边界均保留在验收计划中。本版交付可审查源码、测试入口、平台扩展及构建流程；生产放行以对应实测记录为准。
