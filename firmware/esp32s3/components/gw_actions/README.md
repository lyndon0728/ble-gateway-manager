# 无线动作与双组件升级执行器

本组件面向 ESP-IDF **v5.5.6**。它包含实际 NimBLE、UART DFU 和 ESP 应用 OTA 调用，主机测试替换硬件边界；通过主机测试不等于已经完成目标板射频、断电、MCUboot 或整机升级验收。

## 对外接口和初始化

`gw_actions_init()` 在 `gw_core_init()`、`gw_capture_init()`、`gw_nrf_init()` 后、云端任务执行前调用一次。`gw_actions_configure(patch, complete, revision, deadline, &details)` 接收云端已合并的完整目标；`gw_actions_execute(command, deadline, &details)` 接收已认证、授权、去重并持久接受的完整任务。deadline 为本启动单调毫秒，不能直接保存后跨启动复用。

`gw_actions_recover()` 仅恢复已持久任务的证据或已知升级会话。返回 `ESP_ERR_NOT_FINISHED` 时云端继续保留执行状态和维护义务；`ESP_ERR_TIMEOUT` 表示无法证明副作用结果，应报告 UNKNOWN，不能新建动作重做。`ESP_OK` 才是成功证据，错误不可转换成成功。

### 已实现的路径

| 路径 | 实际行为 |
| --- | --- |
| nRF 扫描/过滤 | 完整扫描配置经 `gw_nrf_scan()` 写入和读回；Beacon 规则在无线对象的 `beaconRules` 字段 |
| nRF 广播 | 完整模式、开关、间隔、AD/ScanRsp 经 `gw_nrf_advertise()` 写入和读回 |
| 上报聚合配置 | 1.0 upload-only 或 scan/filters/upload 组合；上传参数先校验，capture 原子持久保存后切换，upload-only 不触发无线操作 |
| ESP 扫描 | NimBLE legacy 主动/被动扫描，禁用栈重复过滤，复用同一 ISO C UUID/厂商/Beacon 规则；ADV 与 SCAN_RSP 独立处理 |
| ESP 广播 | `NONCONNECTABLE`、`SCANNABLE_NONCONNECTABLE`，完整 AD 边界检查，31 B 上限，100～10240 ms；异步事件启动规避 NimBLE 启停上下文限制 |
| ESP GATT | 单 Central 连接；CONNECT/DISCONNECT、服务和特征发现、READ、WITH_RESPONSE WRITE、READBACK、SUBSCRIBE/UNSUBSCRIBE；临时读写在任务内关闭连接 |
| nRF GATT | 独立 `gw_gatt_nrf.c`；持久 op_seq、异步结果查询、发现、连接身份、订阅和读回验证；按 capture 保存的原始证据复核自动连接 |
| ESP OTA | 独立制品验签、非运行应用槽下载、文件摘要和 Flash 读回、目标版本核验、持久重启意图、IDF 试运行确认/回退 |
| nRF OTA | 完整 Flash 缓存、UART BEGIN/CHUNK/FINISH、排空原始队列、REBOOT、新启动核验、运行镜像读回、CONFIRM 与 confirmed 状态回读 |
| 网络诊断 | `NETWORK` 的 OFF/PASSIVE/ACTIVE，复用 core 的网络租约和有界异步扫描，返回最多 32 个标准 `accessPoints` |

ESP 原生广播**不支持** `CONNECTABLE_SCANNABLE` / Peripheral 业务服务，返回不支持；这项能力不能登记为 ESP 已支持。nRF 可连接广播由其固件和能力位控制。

ESP 原生 GATT 当前只接受 `UNENCRYPTED`；没有实现 ESP bond 安装契约，因此关闭 `CONFIG_BT_NIMBLE_SECURITY_ENABLE`，防止远端安全请求触发默认首次配对。`BONDED_ENCRYPTED` 返回不支持，不能把“bonding=false”误当成“禁止配对”。nRF 使用已有 bond 的能力保持独立。没有实现应用协议级 APPLICATION_REPLY、长写、无应答写、多连接或完整扩展广播；这些能力均不得宣告。

## 数据路径与生命周期

NimBLE 回调只复制数据到 32 项有界队列；独立工作任务调用 `gw_capture_esp()` / `gw_capture_esp_gatt()` 持久接管。队列或持久介质满时保留旧数据并计数丢弃新样本，回调不等待 Flash。扫描每条记录使用既有 UART 24 B 格式和 ESP 单调采集时间，后续双射频去重由 capture 实现。身份解析后的地址类型不会伪装为空中 PUBLIC/RANDOM 地址。

连接引用复用 `gw32-conn-v1` 派生规范，使用 capture 的本次 ESP 控制器启动 ID 和不复用的 32 位连接号。连接登记先持久再返回成功；重启时为旧连接提交 `ESP_RESTART` 关闭事件。断链先保留待持久关闭记录，关闭事件成功落盘前不开始新连接。旧操作回调使用单调 token 隔离，超时或断链不能把新操作变成成功。已经提交给 ATT 的写入若丢失应答且不是明确 ATT 错误，返回 UNKNOWN。

扫描与连接当前不并行。可连接/广播资源冲突先拒绝；配置更新失败尝试恢复旧目标，恢复失败进入 fault。OTA 暂停 ESP 原生扫描/广播，已接管数据不清空。nRF 在 DFU 维护模式自行暂停无线，原队列持续接管；队列未排空时不重启 nRF。

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

本实现使用 IDF OTA rollback 与 nRF MCUboot 机制，但没有完成实板切电、看门狗/卡死、FEM/RF 共存和已签名发行镜像验收。nRF 自身超时复位以其固件为准；当前执行器不会猜测板级复位引脚并强拉复位。硬件挂死恢复策略、安全启动制造、eFuse 和防降级仍需独立制造/实板验证。

## 本轮验证

- `test_actions.c` 直接编译生产调度器，验证意图写入失败无副作用、重复回放、同身份不同内容冲突、修订倒退、配置目标原子保存、UNKNOWN 不重发和升级恢复。
- `test_ota.c` 直接编译生产升级执行器，注入错误签名、存储失败、下载内容改变、ESP 试启动/授权过期、nRF 分块/确认应答丢失。Flash/UART/HTTP/时钟和 SHA API 为主机替身；它证明控制流屏障，不证明物理 Flash 或 mbedTLS 的算法实现。
- `test_ed25519.py` 使用独立 Python cryptography/OpenSSL 和 RFC 8032 向量验证实际 Monocypher C verifier：145 项通过，含错误 key/signature、原文重排、追加字节与非规范 scalar。
- `test_gatt_nrf.c` 由 nRF GATT 子模块维护，覆盖真实执行状态机的操作序列、失去应答、未知写入和连接结果原子发布。
- 上述 actions/OTA/verifier 已以 C11 `-Wall -Wextra -Werror` 编译通过。目标 ESP-IDF 编译及其准确结果以仓库统一验证记录为准；没有用这些主机结果替代目标板通过。

运行本组件主机测试：`python tests/run_host.py --cjson-dir <cJSON.c和cJSON.h目录> --compiler <cc路径>`。使用 Zig 时增加 `--compiler-arg cc`。脚本需要 Python cryptography；不下载依赖、不安装生产密钥、不修改设备。

## 依赖来源

NimBLE 的 API 对照 ESP-IDF v5.5.6 固定子模块 `espressif/esp-nimble@71a1fd7bfd0654c6d174cc3f9cb007864f767cbc`。编译实际使用 IDF 自带组件。

Ed25519 使用 [Monocypher 4.0.3 官方发布包](https://monocypher.org/download/)，源码保存在 `vendor/monocypher`，没有修改。`SOURCE.json` 记录官方下载地址、已与官方 checksum 核对的 SHA-512；`LICENCE.md` 保留上游双许可证。只使用可选 Ed25519/SHA-512 实现做制品签名验证，不使用 Monocypher 默认的其他签名方案替代标准 Ed25519。
