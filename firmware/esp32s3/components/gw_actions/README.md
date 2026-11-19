# 无线动作与双组件升级执行器

本组件对应 ESP32-S3 固件 **0.3.0**，面向 ESP-IDF **v5.5.6**。它包含实际 NimBLE、UART DFU 和 ESP 应用 OTA 调用，主机测试替换硬件边界；通过主机测试不等于已经完成目标板射频、断电、MCUboot 或整机升级验收。

## 对外接口和初始化

`gw_actions_init()` 在 `gw_core_init()`、`gw_capture_init()`、`gw_nrf_init()` 后、云端任务执行前调用一次；制造能力授权也须在动作执行前初始化。`gw_actions_configure(patch, complete, revision, deadline, &details)` 接收云端已合并的完整目标；`gw_actions_execute(command, deadline, &details)` 接收已认证、授权、去重并持久接受的完整任务。deadline 为本启动单调毫秒，不能直接保存后跨启动复用。

`gw_actions_recover()` 仅恢复已持久任务的证据或已知升级会话。返回 `ESP_ERR_NOT_FINISHED` 时云端继续保留执行状态和维护义务；`ESP_ERR_TIMEOUT` 表示无法证明副作用结果，应报告 UNKNOWN，不能新建动作重做。`ESP_OK` 才是成功证据，错误不可转换成成功。

### 已实现的路径

| 路径 | 实际行为 |
| --- | --- |
| nRF 扫描/过滤 | 旧版完整 ScanProfile 保持兼容；V2 JSON 编译为完整二进制对象，经 `0x19/0x1A/0x1B` 写入、完整读回并核验状态，不静默降级 |
| nRF 广播 | 完整模式、开关、间隔、AD/ScanRsp 写入和读回后核实际状态；PAUSED 必须有真实 Peripheral 连接证据 |
| 上报聚合配置 | 1.0 upload-only 或 scan/filters/upload 组合；上传参数先校验，capture 原子持久保存后切换，upload-only 不触发无线操作 |
| ESP 扫描 | NimBLE legacy 主动/被动扫描，禁用栈重复过滤；与 nRF 共享 V2 名称、UUID、RSSI、major/minor 和服务 UUID 掩码引擎，支持 REPORT/PAIR 及完整状态计数 |
| ESP 广播 | `NONCONNECTABLE`、`SCANNABLE_NONCONNECTABLE`、`CONNECTABLE_SCANNABLE`；完整 AD 边界检查，31 B 上限，100～10240 ms；异步事件启动规避 NimBLE 启停上下文限制 |
| ESP GATT / Peripheral | Central 与 Peripheral 共用单连接预算；Central 支持连接、发现、读写、读回、订阅；Peripheral 提供固定 INFO/RX/TX 服务，业务认证及持久接管由 `gw_peripheral` 负责 |
| nRF GATT | 独立 `gw_gatt_nrf.c`；持久 op_seq、异步结果查询、发现、连接身份、订阅和读回验证；按 capture 保存的原始证据复核自动连接 |
| ESP OTA | 独立制品验签、非运行应用槽下载、文件摘要和 Flash 读回、目标版本核验、持久重启意图、IDF 试运行确认/回退 |
| nRF OTA | 完整 Flash 缓存、UART BEGIN/CHUNK/FINISH、排空原始队列、REBOOT、新启动核验、运行镜像读回、CONFIRM 与 confirmed 状态回读 |
| 网络诊断 | `NETWORK` 的 OFF/PASSIVE/ACTIVE，复用 core 的网络租约和有界异步扫描，返回最多 32 个标准 `accessPoints` |

V2 分别受 `NRF54_BLE_FILTER_V2` / `ESP32_BLE_FILTER_V2` 门控，本机 Peripheral 受 `ESP32_BLE_PERIPHERAL` 门控。能力来自制造签名授权、实际编译/HELLO 和实板验证的交集，不能因源码已存在便自动登记可用。自动连接规则仍仅选择 nRF。

ESP 原生 GATT 支持 `UNENCRYPTED` 及使用已有预置 bond 的 `BONDED_ENCRYPTED`。NimBLE 安全与 NVS 持久化已启用，首次配对仍关闭：只从受保护存储读取既有密钥、显式发起已有密钥加密并核实际安全级别；缺 bond 拒绝，不能降级明文。禁止新安全记录写入/删除、拒绝重复配对及首次配对请求，不能只依赖 `bonding=false`。没有实现通用 GATT `APPLICATION_REPLY`、长写、无应答写、多连接或完整扩展广播；独立手机业务的 APPLIED 应答属于 [Peripheral 业务合同](../gw_peripheral/README.md)，不能替代通用 GATT 写入证据。

### 预置 bond 与初始化失败

SDK 固定使用默认 `nvs` 分区的 `nimble_bond` 命名空间。`gw_store_init()` 从既有制造 NVS 密钥显式调用 secure init 初始化 `gwfactory`、`gwstore` 和 `nvs`；不调用会生成密钥或操作 eFuse 的默认初始化流程。预置记录须匹配锁定的 IDF/NimBLE 结构、地址类型、密钥长度和配置容量。

`gw_ble_bonds_check(false)` 在 BLE 初始化前检查加密命名空间及原始安全记录；`ble_store_config_init()` 返回后再用 `gw_ble_bonds_check(true)` 对照 SDK 恢复的安全材料与记录数量。不能仅依赖 SDK 的错误日志：读失败、长度不符、非法密钥或恢复不完整均阻止 BLE host 启动。空库可使用不依赖 bond 的功能，但请求 `BONDED_ENCRYPTED` 明确失败。旧明文或不兼容 NVS 必须由受授权制造流程迁移，设备不会自动擦除、重建密钥或清空 bond。

历史边界：0.2.x 曾仅提供非连接广播与 `UNENCRYPTED` GATT，并关闭 NimBLE security。上述限制记录的是旧版交付范围，不代表 0.3.0 当前实现。

## 数据路径与生命周期

NimBLE 扫描回调只复制数据到 32 项有界队列；独立工作任务按时间顺序调用共享 V2 引擎，REPORT 独立判定报告，PAIR 在有界窗口内组合 ADV/SCAN_RSP 判定后仍输出原始报告。队列与 PAIR 状态共同受固定内存预算约束，队列排空后才推进过期水位。随后调用 `gw_capture_esp()` / `gw_capture_esp_gatt()` 持久接管。队列或持久介质满时保留旧数据并计数丢弃新样本，回调不等待 Flash。扫描每条记录使用既有 UART 24 B 格式和 ESP 单调采集时间，后续双射频去重由 capture 实现。身份解析后的地址类型不会伪装为空中 PUBLIC/RANDOM 地址。

连接引用复用 `gw32-conn-v1` 派生规范，使用 capture 的本次 ESP 控制器启动 ID 和不复用的 32 位连接号。连接登记先持久再返回成功；重启时为旧连接提交 `ESP_RESTART` 关闭事件。断链先保留待持久关闭记录，关闭事件成功落盘前不开始新连接。旧操作回调使用单调 token 隔离，超时或断链不能把新操作变成成功。已经提交给 ATT 的写入若丢失应答且不是明确 ATT 错误，返回 UNKNOWN。

扫描与连接当前不并行；未验证扫描/广播同时运行时，完整目标同时启用两者会拒绝，不静默修改服务器字段。切换角色须在同一原子配置中停止旧角色并启动新角色；实际资源冲突先拒绝，配置更新失败尝试恢复旧目标，恢复失败进入 fault。Peripheral 建链后实际广播暂停，状态须保留连接证据。OTA 暂停 ESP 原生扫描/广播，已接管数据不清空。nRF 在 DFU 维护模式自行暂停无线，原队列持续接管；队列未排空时不重启 nRF。

## 持久化和幂等

| 键 | 内容与用途 |
| --- | --- |
| `actions/current` | 当前非配置动作完整任务、请求摘要、INTENT/DONE/UNKNOWN、结果详情 |
| `actions/config/<radio>/<group>` | 各无线组当前配置意图、修订号和结果；未解决 UNKNOWN 阻止覆盖 |
| `actions/target/<radio>/<group>` | 最后成功目标；与成功结果在同一 store transaction 发布，失败尝试不会抹掉旧目标 |
| `gatt/esp-connection` | 本机连接原始启动/地址/连接号，供重启关闭事件使用 |
| `gatt/nrf-*` | nRF 操作序列、请求、结果、连接登记，详细规则见源代码与专用测试 |
| `ota/active` | 制品身份、组件、版本、摘要、Flash 槽、传输 session、原/确认启动和阶段 |

执行意图必须在第一次物理调用前成功落盘。相同任务和相同摘要回放原结果；内容冲突拒绝；未解决副作用不重放。`actions/current` 是执行日志，**不替代 cloud 的完整历史任务去重日志**；本 API 只能由 cloud 的历史任务闸门调用。不能绕过 cloud 直接重放更早已完成任务。

配置成功时，当前配置 journal、最后成功无线目标与完整 `cloud/configuration` 在**同一个** `gw_store_transaction` 发布。云层不再另写完整配置。`gw_actions_recover_config(patch, complete, revision, &details)` 只读匹配修订、原 patch、完整目标及已提交 cloud 快照的 DONE 证据；它不会再次下发物理配置。这样可以恢复设备已完成配置、云端结果尚未发布的掉电窗口。没有 DONE 证据的配置仍返回 UNKNOWN，需要对账。

正常启动恢复最后成功的无线目标；检测 nRF 新启动后重新应用其已提交扫描/广播目标。恢复使用原修订号，不生成虚假的新 revision。维护、旧动作 UNKNOWN 或配置 UNKNOWN 占用期间不恢复普通无线目标。未知结果需要对账，不能通过清理 journal 绕过。

## 制品信任、试运行与回退

制造 NVS 必须独立安装 `artifact_keys`（JSON 对象：发行 keyId → Base64 编码的 32 B Ed25519 公钥）、`model`、`hardware`。这些是设备本地信任锚，不从下载响应学习或覆盖。没有材料则拒绝升级。私钥不进入设备、配置协议或仓库。公钥 JSON 总长上限 8191 B；当前没有远程密钥轮换接口。

设备读取有效任务允许的 `/api/v1/artifacts/{artifactId}/verification` 和 `/content`。对 verification 中 **manifest 字符串的原始 UTF-8 字节**验签，不重新序列化 JSON。检查 kind、component、expectedFirmwareVersion、型号/硬件/源版本列表、SHA-256；nRF 强制 `MCUBOOT_SIGNED_BIN`。同样的 JSON 对象重新排版后签名不相同。网关组件名为 `GATEWAY`，不发明 `ESP32` 组件名。

下载过程计算全部文件 SHA-256，再从实际 Flash 读回核验；候选下载计数不等于已验证制品。nRF 加密分区写入按 16 B 缓冲，末块填充不进入文件摘要。ESP 槽还经过 IDF 镜像格式验证和 app_desc 版本核验；只升级应用槽，不能升级 bootloader、分区表或凭据。

nRF 相同启动、相同 session/size/SHA 的恢复以 DFU_STATUS 的实际 accepted_offset 为准；最近块应答丢失先查状态，不能换 session 重做未知动作。新 nRF 启动先校验实际运行主镜像，不沿用旧 RAM offset。成功必须经过匹配版本、运行镜像摘要、确认状态，并保存原始确认启动 ID 供结果重放。

ESP 试运行窗口为 **90 秒**，在启动时建立独立定时器。确认检查持久状态恢复、当前运行槽/文件摘要/版本、可信时间、可用网络接口、nRF UART 健康及不存在未确认 nRF 镜像；云端通过认证通道进入恢复流程后才能确认。到期调用 IDF 回退并重启。原任务 expiresAt 和同一启动冻结的 deadline 同时约束恢复，重新轮询不能延长授权。无可信时间或过期任务不能确认新镜像；nRF 已知可终止会话按原 session ABORT，试启动等待回退。

本实现使用 IDF OTA rollback 与 nRF MCUboot 机制，但没有完成实板切电、看门狗/卡死、FEM/RF 共存和已签名发行镜像验收。nRF 恢复先做有限 UART 重握手；只有持久恢复意图、维护租约及已验证板级配置均满足时，才允许一次受控硬件复位，随后必须核对新 boot。`CONFIG_GW_NRF_RESET_VERIFIED` 默认关闭；GPIO、低脉冲及释放等待的默认值不代表实板证据，未验证时不会拉复位。安全启动制造、eFuse 和防降级仍需独立制造/实板验证。

## 本轮验证

- `test_actions.c` 直接编译生产调度器，验证意图写入失败无副作用、重复回放、同身份不同内容冲突、修订倒退、配置目标原子保存、UNKNOWN 不重发和升级恢复。
- `gw_nrf/tests/test_scan_config.c` 覆盖实际 V2 JSON 编译器的完整配置、掩码/范围/PAIR 校验及广播实际状态证据；共享过滤引擎向量由 nRF 主机套件验证。
- `test_bonds.c` 直接编译生产 `gw_ble_bonds.c`，覆盖空库、NVS 打开/读取失败、损坏长度、非法密钥、SDK 恢复缺失/材料不符及恢复一致；不以替身证明空口加密或实际 NVS 断电行为。
- `test_ota.c` 直接编译生产升级执行器，注入错误签名、存储失败、下载内容改变、ESP 试启动/授权过期、nRF 分块/确认应答丢失。Flash/UART/HTTP/时钟和 SHA API 为主机替身；它证明控制流屏障，不证明物理 Flash 或 mbedTLS 的算法实现。
- `test_ed25519.py` 使用独立 Python cryptography/OpenSSL 和 RFC 8032 向量验证实际 Monocypher C verifier：145 项通过，含错误 key/signature、原文重排、追加字节与非规范 scalar。
- `test_gatt_nrf.c` 由 nRF GATT 子模块维护，覆盖真实执行状态机的操作序列、失去应答、未知写入和连接结果原子发布。
- 上述 actions/OTA/verifier 已以 C11 `-Wall -Wextra -Werror` 编译通过。目标 ESP-IDF 编译及其准确结果以仓库统一验证记录为准；没有用这些主机结果替代目标板通过。

运行原动作/OTA 主机测试：`python tests/run_host.py --cjson-dir <cJSON.c和cJSON.h目录> --compiler <cc路径>`。使用 Zig 时增加 `--compiler-arg cc`。完整 ESP 主机入口 `firmware/esp32s3/scripts/test_host.py --cc <cc路径> --idf <ESP-IDF v5.5.6目录>` 另包含扫描编译器、bond 初始化及其他组件检查。脚本需要 Python cryptography；不下载依赖、不安装生产密钥、不修改设备。

## 依赖来源

NimBLE 的 API 对照 ESP-IDF v5.5.6 固定子模块 `espressif/esp-nimble@71a1fd7bfd0654c6d174cc3f9cb007864f767cbc`。编译实际使用 IDF 自带组件。

Ed25519 使用 [Monocypher 4.0.3 官方发布包](https://monocypher.org/download/)，源码保存在 `vendor/monocypher`，没有修改。`SOURCE.json` 记录官方下载地址、已与官方 checksum 核对的 SHA-512；`LICENCE.md` 保留上游双许可证。生产构建由公共 `gw_crypto` 组件编译并供制品、制造清单和签名时间验证复用；使用可选标准 Ed25519/SHA-512 实现，不使用 Monocypher 默认的其他签名方案替代标准 Ed25519。
