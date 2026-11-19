# 三方协议对齐实现与交付记录

日期：2026-11-19。固件版本 ESP32-S3 **0.3.0**、nRF54L15 **0.3.0**；SDK 保持 ESP-IDF **v5.5.6**、NCS **v3.2.1**。本文记录此前 GW32-E2E-1.1 / BFLT-2.0 开发方案的源码实现，原需求和验收编号保留。

## 1. 实现矩阵

| 项目 | 本轮代码行为 | 主要入口 |
| --- | --- | --- |
| C01 | 按真实组件版本、源码测试、目标构建和实板状态分别记录 | 本文、各固件 VALIDATION/README |
| C02 | HTTPS / MQTT 共用任务 journal、Flash spool、稳定事件与结果 ID、应用 ACK；PUBACK 不释放业务记录 | [gw_transport](../firmware/esp32s3/components/gw_transport/gw_transport.c)、[云任务](../firmware/esp32s3/components/gw_cloud/gw_cloud.c) |
| C03 | 广播配置完整读回后再查实际状态；PAUSED 必须有真实 Peripheral 连接，缺失证据保持不确定 | [无线证据](../firmware/esp32s3/components/gw_nrf/gw_radio_evidence.h)、[nRF适配](../firmware/esp32s3/components/gw_nrf/gw_nrf.c) |
| C04 | OTA 恢复先有限重握手，再按已持久恢复意图最多尝试一次板级复位；默认不开启硬件复位 | [OTA恢复](../firmware/esp32s3/components/gw_actions/gw_ota.c)、[板级配置](../firmware/esp32s3/components/gw_nrf/Kconfig) |
| C05 | 独立手机业务 Security2、PG 分片、持久 STORED、平台 PERIPHERAL_PERSISTED、NOTIFY 的手机 APPLIED | [业务桥接说明](../firmware/esp32s3/components/gw_peripheral/README.md)、[平台](../server/internal/platform/peripheral.go) |
| C06 | 一次性挑战/CAS 世代、当前镜像与 boot 核验、独立 GATEWAY/NRF54 库存；证据不足降为 VERSION_ONLY/UNKNOWN | [设备盘点](../firmware/esp32s3/components/gw_cloud/gw_sync.c)、[平台库存](../server/internal/platform/gateway_integration.go) |
| C07 | 设备验签制造清单、身份和摘要；授权/实际编译/HELLO/板验证求交集；ESP Peripheral 与安全预置 bond 支持 | [制造授权](../firmware/esp32s3/components/gw_authority/gw_authority.c)、[ESP BLE](../firmware/esp32s3/components/gw_actions/gw_ble.c) |
| C08 | 完整快照及不可变历史；下载仅存候选；带 sourceSnapshotId 的单组配置任务经完整值、revision 和本地覆盖检查 | [快照存储](../server/internal/platform/gateway_snapshot.go)、[设备校验](../firmware/esp32s3/components/gw_cloud/gw_sync.c) |
| C09 | 独立公钥验证 nonce/boot 绑定时间包；UTC 保守区间、完整 TLS 日期区间、任务单调预算 | [签名验证](../firmware/esp32s3/components/gw_cloud/gw_signed_time_verify.c)、[时间预算](../firmware/esp32s3/components/gw_core/gw_time_bounds.c) |
| C10 | 两路扫描保持独立目标，双路上传继续在 ESP 定稿/去重；角色忙不改持久目标 | [采集](../firmware/esp32s3/components/gw_capture/gw_capture.c) |
| C11 | 镜像传输阶段重启报传输中断；FINISH 不确定状态不虚构回滚；新镜像实际校验后确认 | [OTA阶段恢复](../firmware/esp32s3/components/gw_actions/gw_ota.c) |
| C12 | 名称、UUID、RSSI、major/minor 和服务 UUID 的统一组合掩码，REPORT/PAIR、完整原子切换和96字节状态 | [共享过滤器](../firmware/nrf54l15/src/gw_filter_v2.c)、[平台编译器](../server/internal/platform/beacon_v2.go) |

代码闭合与实板验收分别管理：本轮提供实现和软件验证；不能据此开启未经实测的射频组合、复位 GPIO 或时钟漂移授权。

## 2. 唯一接口与兼容

- 仍只有两类链路：ESP ↔ nRF 的 UART 二进制协议，以及 ESP ↔ 平台的 HTTPS/MQTT 协议。服务器不直接向 nRF 下发 JSON。
- UART frame version 仍为 1，payload 上限3072字节、COBS/CRC32C、串口参数不变。V2 是完整配置的新版本对象，命令为 `0x19/0x1A/0x1B`，能力为 HELLO bit13。
- 平台按目标使用 `NRF54_BLE_FILTER_V2` / `ESP32_BLE_FILTER_V2`。旧设备走原完整 ScanProfile；不会把 V2 静默降级成部分匹配。
- 现有 JSON 消息 1.0/1.1 保持；新库存、快照、签名时间和业务桥接使用各自 schemaVersion 1.0。新增平台数据库迁移为5，不改已发布迁移1～4。
- nRF 能力映射采用实际 UART 定义：扫描bit1、Central bit2、Notify bit3、广播bit4、发现bit9、广播管理bit10、Peripheral bit11、UART DFU bit12、V2 bit13。无当前 nRF 身份时 runtime 的 `nrfBootId=null`、HELLO能力列表为空，不能伪造全零 boot。
- 两路 BLE 使用现有单连接及已验证角色预算。没有并发组合证据时，扫描/广播冲突明确拒绝并回滚；停止旧角色和启用新角色应在同一完整配置事务表达。

机器合同见 [Schema](../server/internal/platform/assets/message-contracts.schema.json)、[OpenAPI](../server/internal/platform/assets/openapi.json) 和 [MQTT合同](../server/internal/platform/assets/mqtt-contracts.json)。

## 3. MQTT 接入与重投

制造键 `cloud_link` 为 `HTTPS`、`MQTT` 或 `AUTO`，缺省使用 HTTPS；MQTT URI 必须 `mqtts://`，用户名为 gatewayId，密码独立配置。验证制造根 CA、主机名和签名时间区间，QoS1、retain=false。

固定前缀 `gw/v1/{namespaceId}/{gatewayId}`。基础订阅 `commands/telemetry-ack/result-ack` 就绪后可用；Peripheral 另需 `peripheral-ack`，失败不阻断基础通道。AUTO 在相应通道不可用时选 HTTPS。候选网络试用即使 MQTT 已连接，仍通过绑定受测接口的 HTTPS 证明可达。

同一设备只经公共 journal 消费下行任务；临时 MQTT 接收队列不是持久受理。平台 command_outbox 在设备应用受理前持续以原 taskId 重发，设备只在 journal 落盘后发送业务受理结果。重复任务、结果 ACK 丢失、传输切换不重复执行副作用。

## 4. 制造配置增量

沿用加密 `gwfactory` 分区和原制造流程。以下值没有共享默认口令，不能把示例密钥或测试配置部署成生产授权。

| 键 | 类型 | 用途 |
| --- | --- | --- |
| `dev_manifest` | UTF-8字符串或原始UTF-8 blob，无末尾NUL | 平台 DeviceManifest 原文件；保持原始字节以校验 SHA256 |
| `dev_artifact` | UTF-8字符串或blob | 外层 kind=DEVICE_MANIFEST 的签名清单原文 |
| `dev_signature` | 64字节blob | Ed25519签名，覆盖 dev_artifact 原始字节 |
| `manifest_pub` / `manifest_keyid` | 32字节blob / 字符串 | 制造清单验证公钥及 keyId；匹配 gateway/sn/model/hardware 条目 |
| `board_caps` | JSON字符串数组 | 已实际验证的能力集合，只能收窄签名清单授权 |
| `time_pub` / `time_key_id` | 32字节blob / 字符串 | 独立用途的时间服务 Ed25519 公钥与 keyId |
| `time_url` | 字符串 | 只提供签名时间的HTTP源，例如部署方指定的独立时间入口；该入口不发送token |
| `time_ppm` | 字符串 `100` | 制造流程确认单调时钟漂移不超过100ppm后才写入；未验证不写 |
| `cloud_link` / `mqtt_url` / `mqtt_pass` | 字符串 | 传输选择、TLS broker地址、每设备密码；普通API token仍独立保留 |
| `gw_fwproof` / `nrf_fwproof` | JSON字符串或blob | 可选制造镜像候选：`{manifest,signature,artifactId?}`；用既有 artifact_keys 验签，运行时仍重新读镜像/boot确认 |
| `periph_acl` | JSON字符串 | 独立客户端/route离线授权，格式见业务桥接说明 |
| `periph0_salt` / `periph0_ver` 等 | 16字节/384字节blob | slot 0～7 的独立业务 Security2 凭据，与 setup 管理员分开 |

0.2.x设备升级前须先按制造流程安装时间公钥、服务地址及经过验证的漂移授权；缺少这些信任材料时0.3.0不会恢复普通远程TLS业务，本地认证配网/OTA仍可用。现有网络凭据安装接口不能改写制造信任根。

较大的签名文件使用 blob，整套制造数据必须能装入原64KiB工厂分区；本轮未改分区地址或 OTA 槽。不同用途密钥独立管理；公开时间通道不能复用普通业务凭据。

生成独立业务 verifier 可用已有工具的新模式：

```powershell
python firmware/esp32s3/scripts/Generate-Setup.py --purpose peripheral --slot 0 --username business-phone-001 --output-dir <仓库外制造目录>
```

工具交互输入密码，不打印或写入明文密码；生成 `periph0_salt.bin` / `periph0_ver.bin`，由制造流程导入同名NVS blob。原 setup 模式输出不变。工具不执行烧录或 eFuse 操作。

## 5. 平台部署与恢复

时间签名端需配置 `GW_TIME_SIGNING_KEY`（Base64的32字节seed或64字节私钥）、`GW_TIME_KEY_ID`、`GW_TIME_SOURCE_VERIFIED=true`。只有运维核实时间源后才设置最后一项；缺配置则拒绝签名。设备只有在新鲜签名锚建立后才开始普通 TLS 业务，SNTP 不能单独建立信任。本地配网和本地 OTA 仍保持离线可用。

配置恢复用原 `PUT /api/v1/gateways/{id}/configuration` 或 CONFIGURE 任务，新增 `sourceSnapshotId`。一次仅恢复一个完整组；平台校验其值与不可变快照一致、分配新revision，上一组完成再执行下一组。`GET /api/v1/gateway-configuration-snapshot?snapshotId=...` 可按设备身份读取历史来源。当前缺组、UNKNOWN、副作用未决、localOverride 或凭据缺失不会被快照下载绕过。

本地 ESP 升级确认后保存真实镜像证据，重新联网盘点。没有平台 artifactId 时，设备用只读 `/api/v1/gateway-component-artifact?component=...&sha256=...` 关联已导入并验签的制品；该查询不代替本机签名与实际镜像校验。未导入制品、缺少确认状态或 boot 变化均保持低证据等级，不冒用 ESP 版本满足 nRF 来源约束。

## 6. 软件验证与实板边界

可复现的验证入口：

```text
python firmware/esp32s3/scripts/test_host.py --cc <C编译器> --idf <ESP-IDF v5.5.6>
powershell -File firmware/nrf54l15/scripts/Test-Host.ps1 -Compiler <C编译器>
cd server && go test ./... -count=1
cd frontend && npm test && npm run build
```

本轮运行记录及最终ESP镜像摘要在 [ESP验证记录](../firmware/esp32s3/VALIDATION.md)。nRF主机测试验证生产协议/过滤/无线状态机；没有完整NCS目标构建或实板数据时不标成已通过。仍需实板完成：两芯片串口电气/流控、FEM与RF、实际手机Security2和bond、掉电/双组件回退、复位脉冲、100ppm时间预算、联网切换与最大持续负载。

最终文档校核：24份Markdown、377个本地链接、11个章节锚点及15个JSON示例通过；系统93条需求/18项ACC、ESP182条需求/50项验收、平台59条需求/11项验收、BFLT18条需求/16项验收的编号集合保持不变。源码差异格式检查通过。Windows最终ESP完整编译/链接及228项构建输入一致性检查通过，Peripheral额外861个逐分配点内存故障场景通过；这些软件证据不替代上述实板项。

## 7. GitHub交付

源码与Markdown已推送至 `codex/gateway-nrf54-v0.2.0` 分支（保留既有分支名，固件版本为0.3.0）。主要实现提交为 `9be7b62`，测试排版兼容修复为 `7efe497`；后者未改变生产代码。

[固件CI 44546976480](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44546976480)全部成功，覆盖Linux主机、网页协议、ESP目标构建和实际链接符号；[平台CI 44545398972](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44545398972)全部成功，覆盖Windows/Linux、race、协议进程、浏览器/真实API和容器构建。验证通过后仅补充本节及固件验证记录。分支上传不等于已合并main或量产放行。
