# 网络配置与双组件 OTA 人工测试手册

版本：1.0；编写日期：2026-11-19；源码基线：ESP32-S3 / nRF54L15 0.3.0，ESP-IDF v5.5.6、NCS v3.2.1。

**本文是供后续人工执行的操作清单，不是测试报告。本轮未操作设备，以下所有用例初始状态均为“未执行”。** 执行人须为每次运行另填结果及证据；既有主机测试或 ESP 目标构建成功不能代替实板通过。

手机在本文中仅通过网关 Wi-Fi 热点进行本地配网和 ESP 本地 OTA；不要求手机 BLE Peripheral 业务、BLE 配网或安装手机 App。nRF 升级仍为“平台 → ESP → UART → nRF”，没有本地网页上传 nRF 的入口。

## 1. 依据、范围和先决门槛

接口已核对当前 [本地 dispatcher](../../firmware/esp32s3/components/gw_core/gw_core.c)、[本地网页](../../firmware/esp32s3/components/gw_provision/web/src/app.js)、[OTA 执行器](../../firmware/esp32s3/components/gw_actions/gw_ota.c)、[平台网络页面](../../frontend/src/features/gateway/NetworkProvisioningPanel.vue)及 [OpenAPI](../../server/internal/platform/assets/openapi.json)。同时参考：

- [ESP 原验收计划](../esp32s3-validation-plan.md)、[系统验收编号](../gateway-requirements.md)。原编号不重分配；本手册编号表示具体人工执行项，映射不表示已完成原验收的全部层次。
- [配网设计](../esp32s3-network-provisioning.md)、[远程配网接口](../../server/docs/network-provisioning-integration.md)、[ESP OTA 协议](../esp32s3-ota.md)。其中旧实现基线/“待实现”文字不作为本轮是否已存在源码的判据。
- [ESP README](../../firmware/esp32s3/README.md)、[ESP 验证记录](../../firmware/esp32s3/VALIDATION.md)、[nRF 验证记录](../../firmware/nrf54l15/VALIDATION.md)、[UART 升级协议](../../server/docs/gateway-uart-protocol.md)、[三方实施记录](../gateway-alignment-implementation.md)。

| 门槛 | 执行前必须取得的材料 | 未满足时的处理 |
| --- | --- | --- |
| G1：可恢复实板 | 板修订、实际接线、可恢复的旧镜像/制造材料、受控供电和救援步骤；制造密钥由保管人管理 | 不执行切网、刷写、复位或断电用例 |
| G2：本地入口 | 实际镜像含网页及 OTA 处理器；已核实的现场授权入口和可重复开窗方式 | 默认 `GW_SETUP_BUTTON_GPIO=-1`、`GW_FACTORY_AUTHORIZED_SETUP=n` 无入口，相关用例阻塞 |
| G3：首次开窗测试配置 | `sdkconfig.local-setup-test` 仅可为首次无网络、无 `core/setup-used` 的设备尝试一次 600 s 窗口 | 不能通过反复重启无限开窗；不能删除已用标记冒充测试通过。需要重开窗的用例仍须 G2 |
| G4：安全/平台 | 独立测试设备、租户、权限、HTTPS、设备身份、制造公钥、加密 Flash/NVS；远程业务具备有效签名时间和已验证漂移材料 | 不关闭 TLS 日期/主机名验证；缺可信时间的远程测试阻塞，离线本地 OTA 可另行执行 |
| G5：nRF 目标产物 | 完整 NCS v3.2.1 与工具链、匹配板的目标构建/sysbuild、MCUboot、两套签名应用及容量报告 | **当前 nRF 目标构建尚未完成，不能宣称已有可验收 HEX 或实板 DFU 镜像；MAN-OTA-13～18 必须先补齐** |
| G6：硬件能力门控 | 制造签名授权与实板能力清单符合测试板；自定义板 RF/FEM、复位线及脉冲验证有独立记录 | nRF `GW_RADIO_VERIFIED` 和 ESP `GW_NRF_RESET_BOARD_VERIFIED` 默认关闭；不为获得“通过”盲目开启 |
| G7：故障注入 | 专用测试固件/注入器、可定位的提交/Flash/确认边界、经过审查的供电夹具与日志 | 页面不能制造的非法块、ACK 丢失、Flash 故障、挂死等场景标为阻塞，不虚构生产调试 API |

测试固件及注入器须先准备并记录源码提交、构建开关、触发方式、关闭方法。可参考既有 host 测试确定注入点，但 host 替身不等于设备故障注入器。UART 必须由一个主动主机控制；ESP 正在执行升级时不得把普通 USB-UART 工具并接成第二个命令发送者。需使用经过验证的串联代理或隔离测试固件。

本手册不要求烧写 eFuse、变更生产密钥、擦除制造分区或清空业务记录。已安装镜像不具备本地 OTA 时，先按既有受控制造/有线或远程流程部署可运行基线；本地网页不能升级自己尚不存在的入口。

## 2. 测试数据、操作入口与记录规则

### 2.1 每轮固定数据

以下是隔离实验网的建议值。执行前确认不会与现网、SoftAP 或其他测试网段重叠；若调整，整个运行记录统一替换。

| 变量/代号 | 具体用途或建议值 |
| --- | --- |
| `RUN_ID` | 唯一运行编号，例如 `20261119-lab01`；不是设备任务 ID |
| `PLATFORM_ORIGIN` / `GW_ID` / `NS_ID` | 实际隔离 HTTPS origin、已登记测试网关和租户；不得填生产目标 |
| `ADMIN_SESSION` / `DEVICE_IDENTITY` | 已有登录会话/设备认证材料的秘密引用；不得在手册、截图或终端历史中填明文 token |
| `AP_PASSWORD_REF` / `SETUP_PASSWORD_REF` | 设备独立热点密码和 setup 口令的秘密保管引用，两者不同 |
| 网络 A | SSID `GW32-LAB-A`，2.4 GHz 信道 1，WPA2_PSK，DHCP，`192.168.50.0/24`，作为已验证回退网络 |
| 网络 B | SSID `GW32-LAB-B`，2.4 GHz 信道 11，WPA2_PSK；DHCP 或静态 `192.168.60.20/24`、网关 `192.168.60.1`、DNS `192.168.60.1`；静态地址预先从 DHCP 池排除 |
| 网络 C | 隐藏 SSID `GW32-LAB-HIDDEN`；另准备支持 WPA3_SAE 的 2.4 GHz AP，均使用独立测试凭据 |
| Ethernet E | 已验证 CH390/板配置后使用 `192.168.70.0/24`，可单独阻断 DNS、TLS 或平台路由；无已验证硬件则以太网子场景阻塞 |
| `WIFI_A_SECRET_REF` / `WIFI_B_SECRET_REF` | 两个 8～63 UTF-8 字节的测试口令引用；不在证据中保存口令本身 |
| `CRED_A` / `CRED_B` | 实际安装后返回的不可变 `credentialRef`，不能手填一个字符串冒充已安装 |
| `NET0` | 执行前完整保存目标、实际网络、lastVerified、接口、generation/localRevision、平台修订及各引用的脱敏基线 |
| `ESP_A` / `ESP_B` | 可回滚旧/新 ESP 应用，真实 app_desc 版本、文件 SHA、size、签名 keyId、槽信息均登记；版本不要直接照抄旧文档示例 |
| `NRF_A` / `NRF_B` / `NRF_B2` | 可恢复旧镜像、新镜像、与 B 相同版本但不同 SHA 的测试镜像；均须先完成 G5；记录 MCUboot 和平台 manifest 两层签名 |
| `TTL_GOOD` / `TTL_SHORT` | 合法未来 UTC 截止时间：正常用例按实测下载、试启动和回执预算留余量；过期负例经受控延迟跨过原截止。不得复制示例固定日期 |

每次操作前记录 `networkGeneration`、`localRevision`（十进制字符串）、平台 `reportedRevision`、snapshotId、ESP/nRF boot ID、运行版本/SHA、待交付事件/结果 ID。秘密抓包需单独受控保存；可交付证据只保留脱敏头、字段、密钥指纹及密码引用。

### 2.2 已存在的入口

优先通过真实内置页面和平台页面执行。下列 JSON 是**解密后的数据示意**，不能以明文 curl 发送到本地控制入口。

本地连接热点后，使用 DHCP 实际默认网关访问页面，常见地址为 `http://192.168.4.1/`。`GET /prov-info` 为有限发现信息；`POST /prov-session` 执行 Security2；`POST /prov-network` 只接收当前认证会话的二进制密文，要求正确 SoftAP 接口、Host、Origin、cookie 和 `application/octet-stream`。

```json
{
  "apiVersion": "1.0",
  "requestId": "<本次随机唯一ID>",
  "operation": "CREATE_CANDIDATE",
  "expectedNetworkGeneration": "<GET_STATE当前十进制字符串>",
  "baseLocalRevision": "<GET_STATE当前十进制字符串>",
  "body": {
    "network": {
      "uplinkOrder": ["WIFI_STA"],
      "ethernet": {"enabled": false, "ipv4": {"mode": "DHCP"}},
      "wifiSta": {
        "enabled": true,
        "ssid": "GW32-LAB-B",
        "security": "WPA2_PSK",
        "credentialRef": "<CRED_B>",
        "ipv4": {"mode": "DHCP"}
      }
    }
  }
}
```

| 本地 operation | 当前实际 body |
| --- | --- |
| `GET_STATE` / `CLOSE_WINDOW` | `{}` |
| `SCAN_WIFI` | `{"mode":"PASSIVE"}` 或 `{"mode":"ACTIVE"}` |
| `PUT_CREDENTIAL` | `ssid/security/password`；密码仅通过认证密文提交 |
| `VALIDATE_NETWORK` / `CREATE_CANDIDATE` | `{"network":完整network对象}` |
| `APPLY_ONLINE` / `GET_TRANSACTION` / `CANCEL_TRANSACTION` | `{"transactionId":"<实际返回ID>"}` |
| `SAVE_LOCAL_ONLY` | `{"transactionId":"<实际返回ID>","acknowledgeUnverified":true}` |
| `RESTORE_PREVIOUS` | **`{}`**；当前代码使用设备保存的 previous 创建新候选，不发送任意前一快照引用 |
| `OTA_BEGIN` | `uploadId/manifest/signature`；manifest 是原始已签名文本字符串 |
| `OTA_WRITE` | `uploadId/offset/data`；data 是当前 1～4096 B 块的标准 Base64 |
| `OTA_STATUS` | `{}` 或 `{"uploadId":"<原ID>"}` |
| `OTA_COMMIT` / `OTA_ABORT` | `{"uploadId":"<原ID>"}` |

需要改变 requestId、JSON、块内容、时序或重放密文的负例使用 G7 的 Security2 测试客户端；现有网页没有“任意 JSON/故障注入”按钮。

平台已有网络配置页及“本地配置与远程网络管理”面板。页面缺席时可由获授权测试 API 客户端使用以下真实管理接口；设备接口不能用管理员身份代替设备提交以伪造实板证据。

| 管理操作 | 接口与必要字段 |
| --- | --- |
| 查询网络状态/凭据 | `GET /api/v1/gateways/{GW_ID}/network-state`（保存 ETag）；`GET .../network-credentials` |
| 安装新密码 | `POST .../network-credentials`，Idempotency-Key；body 为 `ssid/security/password`；需要 `gateway:credentials` 和目标范围权限 |
| 普通配置 | 先 `GET .../configuration` 保存 ETag/reportedRevision；再 `PUT .../configuration`，If-Match、Idempotency-Key；body 必含 `configuration`、`expectedReportedRevision`、未来 `expiresAt`，有设备状态时携带 `networkPrecondition.expectedNetworkGeneration/snapshotId` |
| 网络对账 | `POST .../network-reconciliation`，If-Match（network-state 的 ETag）、Idempotency-Key；`action=ADOPT_LOCAL/APPLY_PLATFORM`、`expectedNetworkGeneration`、`snapshotId`、未来 `expiresAt`、`cancelQueued`；APPLY_PLATFORM 另需完整 `network` |
| 上传制品 | `POST /api/v1/artifacts`，multipart `file/kind=FIRMWARE/sha256/manifest/signature`；保留 manifest 原文及 Idempotency-Key；权限 `artifact:write` |
| 创建升级任务 | `POST /api/v1/tasks`，Idempotency-Key；body 见下方；权限 `task:create`、`firmware:deploy` 及目标设备范围 |
| 观察任务 | `GET /api/v1/tasks/{taskId}` 或平台任务详情；保留实际 TaskResult 与持久 ACK，不仅记录创建 202 |

普通网络更新结构；`expiresAt` 使用提交时有效的 `TTL_GOOD` UTC值，必须晚于平台当前时间且不超过未来7天，不直接提交占位字符串。`expectedReportedRevision` 也须替换为刚读取的实际值：

```json
{
  "configuration": {
    "network": {
      "uplinkOrder": ["WIFI_STA"],
      "ethernet": {"enabled": false, "ipv4": {"mode": "DHCP"}},
      "wifiSta": {
        "enabled": true,
        "ssid": "GW32-LAB-B",
        "security": "WPA2_PSK",
        "credentialRef": "<CRED_B>",
        "ipv4": {"mode": "DHCP"}
      }
    }
  },
  "expectedReportedRevision": 0,
  "expiresAt": "<TTL_GOOD真实UTC>",
  "networkPrecondition": {
    "expectedNetworkGeneration": "<当前值>",
    "snapshotId": "<当前64位小写SHA256>"
  }
}
```

其中 `expectedReportedRevision=0` 仅示意数值类型，必须替换为 GET 返回值。升级任务结构：

```json
{
  "gatewayId": "<GW_ID>",
  "type": "FIRMWARE_UPGRADE",
  "parameters": {
    "component": "GATEWAY",
    "artifactId": "<平台上传返回的ID>",
    "expectedFirmwareVersion": "<对应真实镜像版本>"
  },
  "expiresAt": "<TTL_GOOD的真实UTC时间>"
}
```

nRF 用例仅把 component 改为 `NRF54` 并使用 nRF 制品和版本；`ESP32` 不是合法组件名。平台的创建/验签成功不替代设备的验签、Flash 读回和启动确认。

### 2.3 每项统一记录

每个子场景独立填写：状态（未执行/阻塞/通过/失败）、执行人/时间、源码提交、板卡/接线/电源拓扑、镜像与包 SHA、输入、操作时刻（注明单调钟或 UTC）、真实结果、证据目录、遗留问题。建议证据目录 `evidence/manual/<RUN_ID>/<用例ID>/<子场景>/`，包含脱敏 `baseline.json`、步骤记录、设备日志、平台结果、波形/抓包及 `result.md`。

清理时先恢复网络/时钟/代理故障，再按真实事务状态处理；不把 UNKNOWN 改成成功，不删除 journal/spool、不复用旧密文或篡改修订。取消只针对尚可取消的候选/接收阶段。已提交 OTA 必须先完成确认或实际回退；无法安全恢复时保留证据、隔离测试板并停止后续有副作用用例。

## 3. 网络配置用例

### MAN-NET-01：授权开窗、离线页面与关闭

- **原验收映射**：ESP-AT-37。
- **状态**：未执行。
- **前置/数据**：G1～G3；手机、笔记本各一台；AP/SETUP 秘密引用；首次无配置与已有 NET0 两种设备状态。
- **操作步骤**：
  1. 正常启动已有网络的设备，再断开平台；观察是否出现未经授权热点。
  2. 使用已验证现场入口打开 600 s 窗口，手机连接设备热点；关闭手机蜂窝与外部互联网，用实际 DHCP 网关地址打开完整浏览器。
  3. 笔记本另一次运行重复页面加载；记录本地资源请求，登录后查状态。
  4. 分别测试手动“关闭热点”、已认证空闲 5 min、600 s 硬期限；未认证页面刷新不得续期。重启后尝试原会话。
  5. 关闭后从 STA/Ethernet 和原热点连接尝试访问管理端点。
- **可观察预期**：普通失联不开永久热点；页面不请求 CDN；热点连接不等于 setup 登录；窗口关闭停止 AP/HTTP、原会话失效；上联接口不能访问管理控制。
- **证据**：开窗来源、实际 SSID/网关、客户端型号版本、资源来源、各网络侧访问记录和窗口单调时间。
- **清理恢复**：结束窗口、恢复 NET0 和手机正常连接；不清除 `core/setup-used`。
- **阻塞条件**：无可重复现场入口、缺制造认证材料、相关网页/OTA 代码未有效链接；仅首次开窗配置不足以覆盖全部子场景。

### MAN-NET-02：认证、密文、请求防重与凭据保护

- **原验收映射**：ESP-AT-38。
- **状态**：未执行。
- **前置/数据**：MAN-NET-01 可执行；正确/错误测试 setup 凭据；G7 客户端可保留 requestId 并修改密文/Origin。
- **操作步骤**：
  1. 未登录尝试读取详细状态、安装密码、创建候选；登录后安装 B 的测试密码，记 CRED_B，检查页面、URL、日志与浏览器存储。
  2. 重新开窗，连续 5 次错误认证或受保护请求失败；重连后再试正确口令，再经现场重新授权验证恢复。
  3. 逐项注入非法 Origin、STA 侧请求、错误标签、重复密文计数、旧会话密文、重复 JSON 键、深度超过 8、解密体超过 16 KiB。
  4. 使用新合法会话重复同 requestId/相同业务，再以同 ID 改业务内容；模拟凭据持久失败和配额不足。
- **可观察预期**：越权/坏密文不进入业务修改；窗口锁定不能靠重连清除；同业务防重不产生第二次写事务，冲突拒绝；密码不回显，失败不发布可用引用、不降级明文。
- **证据**：脱敏拒绝码、请求 ID、提交次数、NET0 前后对比、浏览器存储/日志检查；原始秘密抓包单独保管。
- **清理恢复**：撤销注入、关闭窗口；新开窗确认原凭据及网络仍可用；新孤立凭据保留配额记录，不自行删除回退引用。
- **阻塞条件**：缺 G7 客户端/存储注入器的子场景阻塞；浏览器缺安全随机数或所需加密环境时应记录兼容失败，不能绕过。

### MAN-NET-03：扫描、隐藏 SSID 与参数边界

- **原验收映射**：ESP-AT-39。
- **状态**：未执行。
- **前置/数据**：A/B/C；静态 B 参数；32 个 ASCII `A` 为合法 SSID 边界，33 个为超限；10 个“网”加 `AB` 为 32 UTF-8 字节，11 个“网”为 33 字节。
- **操作步骤**：
  1. 登录，分别 PASSIVE/ACTIVE 扫描；检查列表最多 32 项，手填隐藏 C。
  2. 安装相应测试凭据，依次预览 DHCP、完整 STATIC、已验证的 WPA3_SAE 及 Ethernet/Wi-Fi 顺序。
  3. 逐项提交超长/空 SSID、7/64 字节口令、OPEN/企业模式、5 GHz 专用 AP、前缀 0/31、网关跨网段、网络/广播地址、缺 DNS、所有上联关闭、重复 uplinkOrder。
  4. 对合法/非法输入读取实际网络和修订；合法值继续由 MAN-NET-04 实际应用。
- **可观察预期**：语法非法在提交前拒绝且配置不变；合法参数通过预览不等于网络可用；无法接入的频段/认证不静默降级。STATIC 整对象有效，SSID 按 UTF-8 字节约束。
- **证据**：输入向量、扫描结果、字段错误、前后修订与实际配置、AP 真实频段/认证记录。
- **清理恢复**：恢复 AP 参数；取消未应用候选，保留 NET0。
- **阻塞条件**：WPA3 或 CH390 未实板验证则相应子项阻塞；未知枚举等 UI 不允许输入的负例需 G7 客户端。

### MAN-NET-04：本地在线配置完成逐接口证明

- **原验收映射**：ESP-AT-39；ACC-12 的本地候选部分。
- **状态**：未执行。
- **前置/数据**：G4；NET0=A；B 可访问真实测试平台；独立接口抓包；CRED_B。双接口子项另需 Ethernet E。
- **操作步骤**：
  1. 读取基线，选择 B、DHCP、已安装凭据，点击预览及在线应用，记录返回 transactionId。
  2. 持续查询原事务；热点因信道迁移断开时重连并重新登录，不新建候选。
  3. 记录该候选接口上的 DNS/TLS 和 `POST /api/v1/gateway-network-probes`，核对 transactionId/interface/snapshotId/nonce 与持久 receipt。
  4. 分别以 STATIC、双接口顺序重复，验证所需接口均有自己的新鲜证明；查询平台网络状态。
- **可观察预期**：链路/IP/DNS/TLS/认证和应用证明完成后才提交；世代与 localRevision 各加 1，localOverride 为真，显示平台已连接但待同步；不创建虚假 taskId/resultId，也不自行推进 desiredRevision。试用不超过 120 s 或原窗口剩余期限。
- **证据**：逐接口路由/抓包、receipt、事务阶段、提交前后 NET0、generation/localRevision 和平台状态。
- **清理恢复**：需要回 A 时通过新的恢复/候选事务验证并提交，不倒减修订；按 MAN-NET-12 对账。
- **阻塞条件**：签名时间、TLS、身份或平台 probe 扩展未就绪；只有旧接口可达不能算通过。

### MAN-NET-05：首次离线与明确 LOCAL_ONLY 保存

- **原验收映射**：ESP-AT-40；ACC-12 的本地离线边界。
- **状态**：未执行。
- **前置/数据**：首次无 lastVerified、已有 A 两组；平台不可达/无可信 UTC；本地窗口有效；B 合法但可暂时无法联网。
- **操作步骤**：
  1. 普通在线应用失败后查终态；确认没有自动保存为成功。
  2. 新建候选，先取消“仅本地保存”确认框，再重新选择并明确确认；记录 transactionId。
  3. 比较保存目标、实际网络、activeInterface、lastVerified、generation/localRevision；重启并重新授权登录查询。
  4. 恢复 B/平台，观察后台验证；另一次运行在旧验证完成前提交较新的本地目标，延迟旧响应后检查状态。
- **可观察预期**：只有明确确认才 LOCAL_ONLY；保存目标可与实际回退网络不同，旧 lastVerified/凭据保留；无虚假云任务或平台修订。后续证明不重复增加本地修订，旧 snapshot/generation 的迟到证明不能覆盖新配置。
- **证据**：用户确认/取消记录、目标/实际状态、修订、重启恢复、后台 CAS 和平台重新联网记录。
- **清理恢复**：用新事务恢复 A，再执行明确对账；保留首次无网和有回退两组结果。
- **阻塞条件**：缺可重开窗路径；后台响应延迟/CAS 子项缺 G7 时阻塞。

### MAN-NET-06：本地失败、取消与恢复前一配置

- **原验收映射**：ESP-AT-38/41；ACC-12。
- **状态**：未执行。
- **前置/数据**：已验证 A 及 CRED_A；B 错误口令、不可达静态网关、DNS/TLS 失败各一组；保留 CRED_A。
- **操作步骤**：
  1. 每次从 A 新建 B 候选，分别触发上述失败，等终态后再做下一组。
  2. 在 DRAFT 与试用中分别点击取消；另一次关闭窗口，记录回退完成时间。
  3. 成功提交 B 后用页面“恢复前一配置”；底层 `RESTORE_PREVIOUS` body 为 `{}`，观察新 transactionId，再按页面在线验证。
  4. B 的新密码安装后但未成功提交时，确认 A 仍能使用原引用恢复；已提交事务再次取消只查询既有结果。
- **可观察预期**：失败/取消恢复旧实际配置；不自动清除旧密码；恢复是新事务且修订前进，不倒减；已提交事务不因取消被隐式撤销。
- **证据**：失败原因、原/新凭据引用、事务阶段、实际接入 AP、恢复后的版本及配置。
- **清理恢复**：撤销 DNS/TLS/口令故障，确认 A 可达；不手动删除候选 journal 或仍被引用的凭据。
- **阻塞条件**：无已验证回退网络或救援路径；取消时刻无法定位则相应时序子项不能判通过。

### MAN-NET-07：本地提交边界掉电与响应丢失

- **原验收映射**：ESP-AT-41；ACC-12。
- **状态**：未执行。
- **前置/数据**：G7 电源/提交点夹具；A→B；带稳定 ID 的待交付事件及结果；只 ESP 断电与整板断电两种已确认拓扑。
- **操作步骤**：
  1. 分别在凭据持久前/后、候选持久前/后、验证中、网络提交前/后触发一次受控断电；每点从已恢复基线开始。
  2. 恢复后重新授权登录，查询原 transactionId，记录实际网络、目标、凭据及修订。
  3. 不断电，仅丢弃应用响应；新会话查询同一事务，重复相同业务请求，核对物理配置提交次数。
  4. 恢复平台，核对原事件/结果 ID 与交付集合。
- **可观察预期**：提交前回 A，提交后恢复已提交 B；LOCAL_ONLY 保持真实验证等级；浏览器断连不制造第二次提交。身份、回退引用与已接管记录保留，期限不会因重启无限延长。
- **证据**：电源波形与持久切点、恢复 journal/实际路由、前后修订、事务/事件核对账。
- **清理恢复**：完整恢复电源和正常网路，等待真实事务恢复结束，再以正常配置恢复 A。
- **阻塞条件**：没有准确提交边界、供电隔离或安全恢复工具；仅日志附近手动拔电不能声称覆盖全部边界。

### MAN-NET-08：平台密码投递、安装门槛与设备归属

- **原验收映射**：ESP-AT-42/38；ACC-12 的凭据前置。
- **状态**：未执行。
- **前置/数据**：有权限管理员与无 `gateway:credentials` 的账号、另一测试设备 GW_OTHER；G4；已配置秘密库主密钥但不输出其内容。
- **操作步骤**：
  1. 平台网络面板安装 B 密码，记录 PENDING 的 deliveryId/ref/version；断开设备或阻断领取，尝试把 PENDING 引用用于配置。
  2. 恢复设备，观察设备领取与加密持久安装后发送 INSTALLED；刷新面板后选择“用于配置”。
  3. 用相同 Idempotency-Key/相同内容重试安装；检查同一引用。测试错误权限、GW_OTHER 的引用、未知 ref、SSID/security 不匹配、错误密码。
  4. 注入设备持久失败、回执丢失；检查重复领取/回执后状态；核对普通任务、审计和查询均不含密码。
- **可观察预期**：领取不等于安装；只有本设备正确绑定且 INSTALLED 的引用可配置。安装失败不发布可用引用；重复投递安全收敛，无明文密码进入 CONFIGURE；错误密码仍由实际接入验证拦截。
- **证据**：脱敏引用/投递/回执链、权限拒绝、状态时间线、持久安装证据和普通任务字段。
- **清理恢复**：保留有效 A 引用和回退材料；恢复投递通道，处理测试设备的未决投递，不直接换秘密库主密钥。
- **阻塞条件**：秘密服务返回 503、设备不在线、无注入器的持久失败子项；禁止管理员伪造设备 INSTALLED 回执。

### MAN-NET-09：平台远程网络配置成功闭环

- **原验收映射**：ESP-AT-17/42；ACC-12。
- **状态**：未执行。
- **前置/数据**：CRED_B 已 INSTALLED，A 可回退，无未对账 localOverride；合法管理员与实时 ETag/reportedRevision。
- **操作步骤**：
  1. 在配置页编辑完整 B network，选择已安装引用，创建 CONFIGURE；API 备用方式按 2.2 节携带版本及网络前置。
  2. 观察 taskId、服务端分配的 desiredRevision；在候选接口捕获 DNS/TLS 及 EXECUTING/NONE 的 networkProbe 结果。
  3. 核对每个必需 interface/candidateRevision 及四个 Ready 字段真实成立，逐项 RESULT_PERSISTED 在前一平台事务提交后，再观察本机网络提交与成功结果。
  4. 等匹配最终结果 ACK，查询 reportedRevision、network-state、实际接口；以 STATIC、已验证双接口顺序各重复一轮。
- **可观察预期**：任务创建 202 仅表示创建；候选取得全部独立应用证明后才提交，最终 SUCCEEDED/VERIFIED 与真实配置一致；只在匹配成功 ACK 后推进报告水位，无密码下发到 nRF。
- **证据**：任务与脱敏参数、逐接口数据包、probe/result ID、数据库持久 ACK 顺序、实际网络和修订。
- **清理恢复**：用新任务恢复 A，等最终 ACK；不要手动改数据库修订。
- **阻塞条件**：本地覆盖尚未裁决、缺凭据/可信时间/接口验证、已有未决任务；不能绕过占用直接创建第二任务。

### MAN-NET-10：候选证明缺失、错误与超时回退

- **原验收映射**：ESP-AT-17/42；ACC-12。
- **状态**：未执行。
- **前置/数据**：MAN-NET-09 基线；A 仍可达平台、B 可单独阻断；G7 代理能准确选择 probe/ACK。
- **操作步骤**：
  1. 只让 A 可达，B 分别 DNS 失败、TLS 主机名/证书失败、设备认证拒绝；提交 B 候选。
  2. 双接口候选丢弃其中一个应用 ACK；另测错误 gateway/task/result/interface/candidateRevision、仅 HTTP 空 2xx 或仅 MQTT PUBACK。
  3. 在隔离平台注入客户端尝试把首次新 probe 与 SUCCEEDED 放同一批次，记录平台拒绝；这一步是平台负例，不作为设备执行证据。
  4. 等至候选 120 s 或更早原任务截止，查询实际网络、结果和回退原因；恢复合法 ACK 后不得复活终态事务。
- **可观察预期**：旧接口可达不能证明 B；缺任一合法持久证明不提交。TLS 不降级；同批新 probe 绕过应被拒绝（`NETWORK_PROBES_NOT_PERSISTED`）；超时回 A，不伪报成功。
- **证据**：接口隔离抓包、错 ACK 样本、平台拒绝码、固定期限与恢复 NET0。
- **清理恢复**：解除代理/ACL 故障，完成已有任务的真实收尾；需要重试时新建合法任务。
- **阻塞条件**：无按接口隔离路由或逐项 ACK 注入能力；普通总断网不能替代“仅旧接口可达”。

### MAN-NET-11：远程提交掉电与主备链路切换

- **原验收映射**：ESP-AT-18；ACC-12。
- **状态**：未执行。
- **前置/数据**：两个已验证接口、A/NET0、待发 spool/任务结果；G7；CRED_A/B 均保留。
- **操作步骤**：
  1. 分别在候选试用中、全部 probe ACK 后提交前、提交后成功结果前切断 ESP 电源；每次恢复并查询任务和真实网络。
  2. 完成一轮双接口配置，按 uplinkOrder 断开主接口、恢复、短时反复抖动，再断开备用接口。
  3. 恢复网络，观察原 spool/结果重传，核对 gateway 身份和事件 ID。
- **可观察预期**：提交前恢复 last-good，提交后恢复 candidate；主备切换有界，无无限重配；身份/事件/结果不变；缺完整设备快照时不能把最近补丁当完整恢复。
- **证据**：供电/网络切点、持久阶段、接口路由、切换时间、前后配置和重传账。
- **清理恢复**：恢复双接口正常链路，再用正常任务回 NET0；解除注入。
- **阻塞条件**：CH390 或主备拓扑未验证、无提交边界仪器；不能用两个别名指向同一物理链路冒充双接口。

### MAN-NET-12：采用本地目标与应用平台草稿

- **原验收映射**：ESP-AT-43/42；ACC-12。
- **状态**：未执行。
- **前置/数据**：已通过本地操作产生 localOverride=true；平台收到真实 network-state，含 snapshotId/generation/ETag；本地安装引用已登记 LOCAL/INSTALLED。
- **操作步骤**：
  1. 在平台查看保存目标和实际网络差异，尝试普通覆盖；然后选择“采用本地目标”。
  2. 记录 ADOPT_LOCAL 的前置、服务器生成 resolutionToken 和正常任务；核对 uplinkOrder 全部接口证明、成功结果及 ACK 后再清除 localOverride。
  3. 再产生一次本地改动，选择“应用当前平台草稿”，确保发送完整 network，完成同样验证。
  4. 使用陈旧 ETag/generation/snapshot 发请求；令任务下发前本地再次变更；分别准备尚未下发、已发送、UNKNOWN 旧任务观察裁决门槛。
- **可观察预期**：明确裁决仍走正常配置事务；两类修订不混用；旧条件冲突、已发送/UNKNOWN 占用不被删除。只允许合法取消未下发任务；不能只暂停下发便绕过占用。先成功结果 ACK，后清除对应本地覆盖。
- **证据**：前后状态快照、ETag、任务/前置、全接口证明、ACK 与覆盖清除顺序、冲突结果。
- **清理恢复**：保留 UNKNOWN 供对账，不强删；合法完成剩余任务后恢复共同认可的 A 目标。
- **阻塞条件**：平台未收到真实本地状态、引用归属未登记或旧任务无法安全终结。

### MAN-NET-13：旧页面、并发入口与 OTA 排他

- **原验收映射**：ESP-AT-43；ACC-12 的事务互斥部分。
- **状态**：未执行。
- **前置/数据**：本地会话和平台用户；旧 generation/localRevision 的页面/客户端；可控网络候选、ESP 本地/远程 OTA、nRF OTA 阶段。
- **操作步骤**：
  1. 本地候选先占用，再发远程改网；换顺序重复，核对同一时刻唯一 owner。
  2. 保留旧页面版本，另一入口完成提交后再用旧值操作；同 requestId 改内容。
  3. 在 OTA RECEIVING、READY、已提交试启动阶段逐一尝试本地/远程改网；反向在网络候选中尝试 OTA。
  4. 冲突后重新读取状态；只在原事务安全终止后创建新的请求。
- **可观察预期**：明确 BUSY/版本冲突；不静默覆盖，localRevision 不占 desiredRevision；网络不能抢占试启动确认义务，OTA 不能覆盖网络回退证据。
- **证据**：请求顺序、owner/journal、版本、实际副作用次数、拒绝结果和试启动期限。
- **清理恢复**：完成或合法取消原 owner 的工作，恢复 A；不清理锁记录来强行继续。
- **阻塞条件**：nRF OTA 子项缺 G5；精确并发顺序缺 G7；未完成试启动恢复不能开始下一项。

### MAN-NET-14：真实浏览器、信道/子网变化与窗口收尾

- **原验收映射**：ESP-AT-44。
- **状态**：未执行。
- **前置/数据**：真实 Android/Chrome、iOS/Safari、笔记本 Chrome/Edge 或 Safari，记录实际版本；A 信道 1、B 信道 11；另设候选子网与当前 SoftAP 冲突；已验证的 BLE 采集负载。
- **操作步骤**：
  1. 每种客户端重复登录、扫描/手填、预览、在线/LOCAL_ONLY、查询、退出；手机蜂窝开/关各测一次。
  2. 应用跨信道 B，断连后获取新地址、重认证查询原事务；测试系统小窗口与完整浏览器。
  3. 应用与 SoftAP 冲突子网，观察地址迁移或明确拒绝；从客户端检查未新增 NAT/任意转发。
  4. 在采集/上传压力下重复，并等窗口关闭；检查上联、UART 接管和原待发数据继续。
- **可观察预期**：无 CDN/App 依赖；手机不误把无互联网当作配置失败；原事务可查，旧响应不覆盖新状态；子网冲突不产生假证明；内存/缓存有界，丢失有计数，窗口最终关闭。
- **证据**：真实设备兼容矩阵、录像、实际信道/子网、事务 ID、资源/延迟/丢失曲线与关闭后访问结果。
- **清理恢复**：恢复 AP/手机网络与基线负载，退出所有窗口。
- **阻塞条件**：缺真实手机不能用桌面视口替代；未冻结负载指标/未验证双射频能力时压力子项阻塞，不能强开共存能力。

## 4. OTA 用例

### MAN-OTA-01：发行包、构建和救援预检

- **原验收映射**：ESP-AT-34/50；ACC-17 仅作为 nRF 目标构建前置核查，不表示完成其全部板型构建。
- **状态**：未执行。
- **前置/数据**：ESP_A/B、仓库外测试发行私钥及匹配设备公钥、未存在的输出目录；nRF 部分另需 G5。
- **操作步骤**：
  1. 核对提交、锁定 SDK/toolchain、最终 sdkconfig、链接 map、分区表；确认双 ESP 槽各 `0x280000`，镜像不超过实际槽容量且旧槽可救援。
  2. 在发行环境运行已有 `firmware/esp32s3/scripts/package_ota.py`，参数使用真实 `--image/--private-key/--key-id/--model/--hardware/--from-version/--output-dir`；不执行烧录。
  3. 独立核验生成的 firmware.bin、manifest.json、signature.txt、verification.json，比较实际 app_desc、sizeBytes、SHA 和原始 manifest Ed25519 签名。
  4. 以错误芯片、截断/超大镜像、非 Ed25519 私钥、已存在输出目录做工具负例；核对包/日志不含私钥。
  5. nRF 独立记录目标/sysbuild、MCUboot/签名配置、真实 `zephyr.signed.bin` 和容量，不使用 ESP 成功构建替代。
- **可观察预期**：合法包字段与实际二进制一致，非法输入拒绝且不覆盖旧包；没有自动生成生产密钥或烧写 eFuse。尚无 nRF 目标产物时其检查保持阻塞。
- **证据**：构建日志/map、产物 SHA、独立验签、工具正反例、救援步骤版本；私钥仅记 keyId/指纹。
- **清理恢复**：关闭发行环境秘密访问，隔离负例包并标明不得生产部署；不删除基线镜像。
- **阻塞条件**：缺真实旧/新目标镜像、公钥/签名授权、可执行救援路径；nRF G5 当前未满足。

### MAN-OTA-02：手机/笔记本离线本地 ESP OTA 正常流程

- **原验收映射**：ESP-AT-45/50/31。
- **状态**：未执行。
- **前置/数据**：G1～G3；ESP_A 正在运行，合法 B 包；nRF 健康且无未确认镜像；禁用上联/UTC 同步，仍保留本地窗口。
- **操作步骤**：
  1. 记录运行槽、ESP boot、网络修订和待交付数据；手机连热点登录，选择 verification.json 与 firmware.bin。
  2. 上传，记录 uploadId、每次设备 offset；全部校验后查 READY，同时确认旧槽/版本仍运行。
  3. 明确点击安装确认；记录 ACCEPTED/REBOOT_PENDING 和真实重启，不把页面进度条作为成功。
  4. 观察至少 10 s 健康观察与 90 s 窗口内实际 mark-valid；通过现场入口重开窗口、重认证并查询原 uploadId。
  5. 笔记本及另一真实手机浏览器分别从已恢复 A 的独立运行重复；恢复联网后查看正常版本上报。
- **可观察预期**：无互联网/UTC 仍可完成本地认证和安装；仅完整签名/大小/SHA/SDK/版本通过才 READY；真实新槽确认后才 CONFIRMED。网络版本不推进，不创建虚假远程任务。
- **证据**：设备/浏览器版本、包 SHA、offset、槽/boot/健康时间线、LOCAL journal、修订与数据集合、恢复联网后的真实版本。
- **清理恢复**：保留确认结果；后续需要 A 时使用已签名且兼容的正常升级/既有救援流程，不直接擦空新槽。
- **阻塞条件**：升级后无法重新开窗、nRF 未健康/未确认、基线不含本地 OTA 或包不匹配；这些条件不能以关闭健康检查绕过。

### MAN-OTA-03：本地认证与制品准入负例

- **原验收映射**：ESP-AT-46。
- **状态**：未执行。
- **前置/数据**：合法 B 的隔离副本；未知 keyId、篡改签名、错 model/hardware/fromVersion、错 component/芯片、manifest 超 8192 B、缺/非法 sizeBytes、bin 内容或 app_desc 不符等独立样本；G7。
- **操作步骤**：
  1. 未登录、过期会话、错误 Origin、STA/Ethernet 侧分别尝试 OTA_BEGIN/WRITE/COMMIT。
  2. 用合法会话逐份提交准入样本；另测裸 nRF、merged 镜像和 `component=ESP32`。
  3. 对通过 BEGIN 但文件 SHA/SDK/app_desc 不符的样本完成传输，尝试 COMMIT。
  4. 每次记录首次擦写是否发生、所写槽、offset、状态和当前旧镜像可启动性。
- **可观察预期**：认证或前置验签/兼容性失败不启动擦写；下载后才可发现的错误允许仅影响非运行候选槽，但不能 READY/切槽/确认。错误脱敏，不因浏览器检查被绕过而误接受。
- **证据**：每个样本唯一编号、包/manifest SHA、拒绝阶段/代码、Flash 写入范围和旧槽启动记录。
- **清理恢复**：终止尚可取消的失败上传，换新 uploadId；隔离全部负例包。
- **阻塞条件**：没有设备侧写槽观测和 G7 客户端时，只能记录页面拒绝子项，不能判设备准入全项通过。

### MAN-OTA-04：本地块边界、重复块和身份冲突

- **原验收映射**：ESP-AT-46。
- **状态**：未执行。
- **前置/数据**：合法 B；G7 Security2 客户端和 Flash 写次数观测；固定 uploadId，原始字节一致的块集。
- **操作步骤**：
  1. BEGIN 后分别发送解码长度 0、1、4096、4097 B 的块，合法值放在有效连续偏移且不越 sizeBytes，记录每次状态。
  2. 测试负数/非整数/跳跃/越界 offset；发送合法最后接纳块后原字节重试，再改 1 B 重试；重试更早的块。
  3. 同 uploadId 更换 manifest/signature；另在未完整上传时 COMMIT。
  4. 完成正常上传，丢最终块响应，新会话 STATUS；依据实际 READY 判断，不重复创建上传。
- **可观察预期**：只有连续合法块推进 offset；同一最近块相同字节幂等、不第二次写入；冲突/旧块拒绝；非法块不推进进度，未 READY 不能安装；同 ID 不可换包。
- **证据**：块 offset/长度/摘要、响应、Flash 写次数、最终 offset/size 和状态。
- **清理恢复**：合法 ABORT 未提交候选；重新测试使用新 ID，不擦除 journal。
- **阻塞条件**：缺可构造已认证业务请求的客户端或实际写次数观测；网页按 4096 B 正常上传不能覆盖所有负例。

### MAN-OTA-05：浏览器断连与上传状态恢复

- **原验收映射**：ESP-AT-47/50。
- **状态**：未执行。
- **前置/数据**：B 包；原 uploadId 可记录；保持 ESP 不重启，原窗口仍有效；可断客户端链路/选择性丢响应。
- **操作步骤**：
  1. 在普通块响应前断浏览器连接；重新连热点、登录、查询原 uploadId，重新选择原文件后按设备 offset 续传。
  2. 在最后块 READY 响应前断连；重连 STATUS 后确认页面不盲重传、不沿用未经核实旧 READY。
  3. 在 COMMIT 响应前断连；等待设备实际重启/确认或回退，再重开窗口查询，禁止新 BEGIN 覆盖。
  4. 在 Android、iOS 和笔记本完整浏览器分别检查文件选择、进度和重新认证行为。
- **可观察预期**：不重放旧密文；同 boot RECEIVING 才按原 offset 继续；READY 待明确安装；提交响应丢失不等于安装失败，最终按真实槽与持久状态显示。
- **证据**：断连时刻、原 uploadId/boot、会话更替、offset、页面录像、最终实际镜像及结果。
- **清理恢复**：完成确认/实际回退；未提交测试可取消，恢复正常客户端连接。
- **阻塞条件**：无重新开窗方式、不能区分 ESP 重启与仅浏览器断连、缺精确丢响应工具的对应子项。

### MAN-OTA-06：取消、空闲和固定窗口截止

- **原验收映射**：ESP-AT-47。
- **状态**：未执行。
- **前置/数据**：B 包；分别准备 RECEIVING、READY、REBOOT_PENDING；可记录单调窗口开始/截止和上传活动。
- **操作步骤**：
  1. RECEIVING 及 READY 分别点击取消，查询 ABORTED 与维护释放；测试在一个 WRITE 在途时点击取消。
  2. 上传暂停超过 5 min；另保持合法活动跨过本次窗口固定截止；分别观察终止与 AP 收尾。
  3. 原上传开始后关闭窗口再重新授权，尝试原 ID 继续；确认新窗口不延长旧 BEGIN 授权。
  4. 已明确 COMMIT 后尝试取消/关窗，观察实际试启动义务继续。
- **可观察预期**：取消/超时终止未提交候选且不切槽；在途取消不被界面吞掉；已提交不能回到接收阶段或抹除恢复义务；租约只在安全结束后释放。
- **证据**：BEGIN 固定 deadline、最后活动时间、取消请求/响应、状态与 owner、AP 关闭时刻。
- **清理恢复**：未提交上传使用新 ID 重开；已提交完成确认或回退后再执行其他任务。
- **阻塞条件**：无法测量窗口/空闲时间或无法定位在途请求；不得修改生产超时常量代替实际时间测试。

### MAN-OTA-07：两入口 OTA、改网与重启互斥

- **原验收映射**：ESP-AT-47/43。
- **状态**：未执行。
- **前置/数据**：合法本地 B、平台 ESP/nRF 制品和任务权限；可观察维护 owner；nRF 子项须 G5。
- **操作步骤**：
  1. 本地 ESP RECEIVING/READY 时分别创建远程 ESP OTA、nRF OTA、网络候选及网关重启请求。
  2. 分别以远程 ESP/nRF OTA、网络候选先取得 owner，再尝试本地 BEGIN。
  3. 在试启动确认义务存在时尝试同组冲突请求；保留原任务授权期限。
  4. 原工作安全结束后重新查询状态并重新提交新的合法请求。
- **可观察预期**：冲突明确拒绝/忙碌，无抢占、无第二次擦写或网络切换；不能把 BUSY 当排队成功；原确认期限不延长，既有 spool/动作证据保留。
- **证据**：到达顺序、任务/uploadId、owner、拒绝码、Flash/网络动作计数及时间线。
- **清理恢复**：只取消尚可取消的原工作；完成所有已提交恢复义务，保留拒绝记录。
- **阻塞条件**：nRF 目标/能力未就绪、任务占用无法安全终结；不创建未定义的强制解锁接口。

### MAN-OTA-08：本地 OTA 各持久边界断电

- **原验收映射**：ESP-AT-48/31。
- **状态**：未执行。
- **前置/数据**：G1/G7；A/B、已接管数据；仅 ESP 与整板供电拓扑；可观察 otadata、journal、真实槽。
- **操作步骤**：
  1. 每个独立子场景从 A 开始：BEGIN 意图、擦写中、最终校验中、READY、COMMIT 意图、设置启动槽前后、REBOOT_PENDING、CONFIRMING、mark-valid 前后、CONFIRMED 持久前后。
  2. 在各点触发受控断电，恢复后读取真实启动槽、镜像、SDK 状态及原 uploadId；不先发新 BEGIN。
  3. RECEIVING/READY 未提交恢复后，以新 uploadId、offset=0 重新测试；已提交按实际启动结果完成恢复。
  4. 核对网络/凭据、身份、待交付事件与动作证据保留。
- **可观察预期**：未提交不自动安装，旧 RAM offset 不跨 boot；提交歧义按实际槽/状态判定，不能只看错误响应认定未安装。只有真实回旧槽才 ROLLED_BACK，只有真实新槽确认才 CONFIRMED。
- **证据**：每个切点的电源波形、otadata/槽、镜像 SHA、journal、前后数据集合及恢复时限。
- **清理恢复**：先完成确认/实际回退，再恢复已确认基线；禁止擦除 otadata 来掩盖恢复失败。
- **阻塞条件**：缺准确提交点/隔离供电/救援能力；未覆盖的边界逐项保留未执行，不能合并成“掉电测试通过”。

### MAN-OTA-09：离线健康失败、真实回滚与存储兼容

- **原验收映射**：ESP-AT-48/31/32。
- **状态**：未执行。
- **前置/数据**：专用已签名 B 故障镜像，可在 core/存储/身份/capture/nRF/provision 初始化、镜像核验或主循环触发故障；A 能读取当前全部持久域；兼容矩阵和救援方案已审核。
- **操作步骤**：
  1. 保存配置、凭据引用、spool、UART 水位、任务及 OTA journal 基线；断外网/UTC，以本地入口安装各故障镜像。
  2. 记录至少 10 s 观察门槛和 90 s 试启动上限；检查不健康镜像未 mark-valid，实际 rollback/reboot 回 A。
  3. A 启动后逐域读取并恢复正常上报，核对事件/任务 ID 与未决写入未被重放。
  4. 仅在发行确有持久格式迁移且已有实现/测试构建时，另覆盖版本标记、复制、索引发布、首次读取前后断电及回退读取；当前无通用迁移入口时将此子项记阻塞。
- **可观察预期**：无 UTC 不直接否决合法 LOCAL 授权，但健康故障必须真实回退；不只是写 FAILED 后继续跑未确认镜像。新版本不得破坏旧版安全读取和接管责任。
- **证据**：故障构建/触发记录、90 s 时间线、真实回旧槽、原数据逐项核对、迁移版本矩阵（适用时）。
- **清理恢复**：恢复正式测试基线 A，撤除故障固件；核对设备再次可正常升级。
- **阻塞条件**：缺安全故障镜像、实际看门狗/回滚证据或存储兼容矩阵；不凭当前无格式变化宣称所有未来迁移通过。

### MAN-OTA-10：平台 ESP OTA 与同包双入口

- **原验收映射**：ESP-AT-49/31/50。
- **状态**：未执行。
- **前置/数据**：G4；A/B；MAN-OTA-01 同一四文件包；FIRMWARE_UPGRADE 能力，TTL_GOOD；无其他 owner。
- **操作步骤**：
  1. 上传原 bin、manifest 原文和 signature 创建 FIRMWARE 制品，保存 artifactId；核对服务端实际验签与归属。
  2. 按 2.2 创建 component=GATEWAY 任务；另一个独立运行验证兼容省略 component，而非填写 ESP32。
  3. 观察设备只通过当前有效任务授权的 verification/content 下载，独立验签并写非运行槽；记录文件读回 SHA/版本。
  4. 真实重启，在原授权内完成在线恢复、健康检查和 mark-valid；等持久 SUCCEEDED/VERIFIED 与匹配 RESULT_PERSISTED。
  5. 比较本地入口执行 MAN-OTA-02 时同包的实际镜像 SHA，确认两入口签名原文一致、GATEWAY/nRF 库存独立。
- **可观察预期**：完整实际新镜像确认后才成功；任务与结果稳定，源/目标版本匹配；不改分区表、bootloader、凭据和 spool。
- **证据**：制品 ID/原文摘要、任务/权限、下载、Flash/槽/boot/健康/确认、结果与 ACK、同包双入口 SHA。
- **清理恢复**：完成结果交付；后续恢复 A 使用合法兼容制品和新任务，不复用已完成 taskId。
- **阻塞条件**：签名时间/TLS/授权缺失、源版本未知或不兼容、网络不能覆盖完整升级预算。

### MAN-OTA-11：远程 ESP 准入、过期与下载故障

- **原验收映射**：ESP-AT-49/31；ESP-AT-16 的远程无可信时间部分。
- **状态**：未执行。
- **前置/数据**：G7 可控延迟/TLS/内容故障；短 TTL；错误组件、型号、硬件、源版本、expectedVersion、签名、长度及 nRF 制品各一份；均为隔离测试发行物。
- **操作步骤**：
  1. 分别测试无权限/跨设备/缺升级能力、NRF54 制品配 GATEWAY 任务及各错误 manifest；记录平台拒绝与设备独立拒绝的层次。
  2. 中断下载、改变内容、尝试跳到其他 origin；检查旧运行槽和设备 token 未外送。
  3. 在首次写入前、擦除后、块边界、远程试启动确认前分别使原任务过期或可信时间失效；恢复连接后重投原任务。
  4. 保持原授权不变观察恢复；不得转为 LOCAL 来续权。
- **可观察预期**：不合格制品/身份拒绝；TLS 验证不关闭，故障不伤运行槽；重试/轮询不延长原 TTL/冻结单调期限。未确认的新镜像不能在过期后强行确认。
- **证据**：负例编号、拒绝层、原截止/单调预算、写入边界、TLS/重定向记录、实际启动/回退结果。
- **清理恢复**：恢复时间源/TLS/内容服务，完成实际回退或安全终态；重新升级须合法新任务。
- **阻塞条件**：无真实时间签名环境、边界暂停点或授权过期测试工具；仅改浏览器时间不能代表设备时间失效。

### MAN-OTA-12：远程结果重传与已确认恢复

- **原验收映射**：ESP-AT-49；ESP-AT-15/13 的升级任务与结果 ACK 子集。
- **状态**：未执行。
- **前置/数据**：一次合法 GATEWAY 升级；G7 可丢 ACK/重复命令；Flash 写次数、resultId/resultSequence 可观察。
- **操作步骤**：
  1. 已受理任务在 HTTP/MQTT 下重复下发相同 taskId/内容，再注入同身份不同内容。
  2. 新镜像确认后丢成功结果或平台 ACK；确认后仅重启 ESP，观察原结果恢复。
  3. 分别只给 PUBACK/空 HTTP 2xx、错误 task/result 身份、迟到/重复 ACK，最后给合法 RESULT_PERSISTED。
  4. 核对整个过程真实安装/切槽次数及平台任务记录。
- **可观察预期**：相同任务不第二次刷写；冲突拒绝；已确认事实只补齐结果，不重新升级。resultId/resultSequence/内容稳定，只有匹配应用 ACK 才完成交付，迟到 ACK 不倒退修订。
- **证据**：原任务/结果摘要、ACK 样本、outbox 前后、真实写入/重启次数、平台接纳记录。
- **清理恢复**：撤销 ACK 注入并完成原结果交付；不删除 journal 来消除重复提示。
- **阻塞条件**：无可辨识传输 ACK 与应用 ACK 的代理/日志；MQTT 未部署时保留该传输子项未执行。

### MAN-OTA-13：nRF 经 ESP UART 升级正常链路

- **原验收映射**：ESP-AT-26/29；ACC-13/15。
- **状态**：未执行；当前受 G5 阻塞。
- **前置/数据**：匹配 MCUboot 与已确认 NRF_A，合法 NRF_B `MCUBOOT_SIGNED_BIN`；平台/制造能力 FIRMWARE_UPGRADE、NRF54_UART_DFU_1，真实 HELLO bit12；G4～G6；无 Central/Peripheral 冲突连接。
- **操作步骤**：
  1. 记录实际 nRF boot/版本/确认状态和已提交扫描/广播目标；上传 NRF_B 的平台签名制品，创建 component=NRF54 任务。
  2. 观察 ESP 完整暂存并核 SHA 后才发 BEGIN；记录 session、size、SHA、max_image_size、每块 accepted_offset。
  3. 完成 FINISH，排空旧采集 FIFO 后发送 REBOOT；捕获新 HELLO 与真实新 boot。
  4. 依次核健康、`0x37 VERIFY_RUNNING(size,SHA)`、`0x36 CONFIRM`、`0x33 STATUS running_confirmed=1`，再检查持久成功结果/平台 ACK。
  5. 检查按原已提交 revision 恢复业务目标，ESP 版本保持其自身值。
- **可观察预期**：BEGIN/CHUNK/FINISH/REBOOT 任一受理都不是最终成功；新镜像真实读回及确认完成才 SUCCEEDED/VERIFIED；UART 与采集责任不中断为虚假 ACK。
- **证据**：两层签名、制品/任务、完整 UART 命令及 boot/session、运行 SHA/确认、FIFO 排空、结果和恢复目标。
- **清理恢复**：等结果交付，确认 nRF/ESP 均稳定；必要时用合法 NRF_A 制品和新任务恢复。
- **阻塞条件**：当前无完整 nRF 目标构建/实板 MCUboot 镜像；缺能力/板验证不能强行宣告，不能用 uart_probe 的自测代替整链路。

### MAN-OTA-14：nRF 两层签名与制品准入拒绝

- **原验收映射**：ESP-AT-26；ACC-13。
- **状态**：未执行；当前受 G5 阻塞。
- **前置/数据**：MAN-OTA-13 的可恢复基线；独立负例：错组件/型号/硬件/源版本、manifest 签名/文件 SHA、超 max_image_size、裸 bin/merged.hex、缺 capability；另准备**平台 manifest 合法但 MCUboot 签名错误**的隔离镜像。
- **操作步骤**：
  1. 每次从已确认 A 开始，逐份创建任务并记录平台、ESP 或 UART 拒绝阶段和 BEGIN 是否发生。
  2. 当前 nRF 源版本缺可信镜像证据时提交带非空 fromFirmwareVersions 制品，验证拒绝；补真实库存证据后重测，不删除限制绕过。
  3. 测未确认运行镜像、存在连接/其他 DFU 会话；观察 BUSY 且无覆盖。
  4. 对 MCUboot 错签名样本执行到可达到的 READY/试启动，观察 bootloader 拒绝和旧镜像可用性。
- **可观察预期**：前置错误在各自正确边界拒绝；平台验签不替代 MCUboot。错 MCUboot 签名可能到 READY，但不能确认/报告成功；旧应用保持可救援。
- **证据**：负例与原始签名摘要、拒绝阶段、BEGIN/写范围、bootloader 记录、实际旧镜像和平台终态。
- **清理恢复**：只终止身份匹配且允许 ABORT 的会话；未知会话不发控制；恢复已确认 A。
- **阻塞条件**：缺受控负例发行/MCUboot 镜像或救援；不得在普通业务设备尝试坏 bootloader/分区表。

### MAN-OTA-15：nRF 分块、重握手及传输阶段复位

- **原验收映射**：ESP-AT-27/36；ACC-13/14 的块与传输恢复部分。
- **状态**：未执行；当前受 G5 阻塞。
- **前置/数据**：ESP 已完整验证的 NRF_B 暂存、RECEIVING、原采集 FIFO；G7 串联代理/测试构建，30 s DFU 等待政策；可独立复位 nRF。
- **操作步骤**：
  1. 丢最新 CHUNK 响应，观察相同 UART 请求 ID/原字节重试；重握手后先 STATUS 比较 boot/session/state/size/SHA/offset。
  2. 注入首块不足 32 B、块长 0/1025、跳 offset、最新块相同/改 1 B、较旧块；合法上限 1024 B 单独测试。
  3. RECEIVING 时仅 nRF 复位，再比较新 boot、旧 RAM offset 与恢复行为；不得继续旧偏移，原任务按真实阶段报告中断后，重新升级使用新的受授权会话从 0 开始。
  4. 对 RECEIVING/FAULT/ABORTED 状态重握手，分别核身份匹配后续传、原 session 安全终止、新 session 重新开始；身份不匹配时不发 ABORT/REBOOT。
  5. 同时检查原 FIFO 数据接管，控制等待期间 RX/ACK 未被全部阻断。
- **可观察预期**：最近同块幂等、不第二次 Flash 写；冲突/旧块拒绝。只有同 boot 的完整绑定 RECEIVING 能续传，accepted_offset 不是断电检查点；传输中断不误报镜像回滚或成功。
- **证据**：逐块摘要/偏移/写次数、STATUS、boot/session、任务阶段、UART/数据 ACK 时序。
- **清理恢复**：解除串联故障，按状态安全终止或完成；新升级不重用 ABORTED session，不清空旧已接管数据。
- **阻塞条件**：G5/G7 未准备、无法区分 UART link 重握手与芯片 boot 变化；并接第二命令主机不允许作为注入方式。

### MAN-OTA-16：nRF FINISH、READY 与启动歧义

- **原验收映射**：ESP-AT-28/36；ACC-14。
- **状态**：未执行；当前受 G5 阻塞。
- **前置/数据**：合法 NRF_B；能定位 FINISH 写 TEST 标记、READY、REBOOT_PENDING；ESP 持久升级 journal；G7 电源/响应夹具。
- **操作步骤**：
  1. FINISH 请求发出后丢响应，分别在 TEST 标记前/后、READY 但未 REBOOT 时执行仅 nRF 复位及整板断电。
  2. 恢复主动 HELLO/STATUS，先核实际运行版本、boot、主镜像及确认状态，再判定继续确认、回退或仍可安全终止。
  3. READY 注入迟到 CHUNK；REBOOT_PENDING 尝试 ABORT；测试同会话 FINISH/REBOOT 合法重试及 ABORT 失败。
  4. 比较未接管 FIFO 缺口与已接管数据，保留每种断电拓扑证据。
- **可观察预期**：FINISH 不确定不盲目重新 BEGIN/擦写；可能已启动候选，不能仅按旧 offset 判断。REBOOT_PENDING 不假称已 ABORT；终止失败保留维护义务；实际镜像证据不足保持 UNKNOWN。
- **证据**：TEST/FINISH/READY 波形与 UART、bootloader/镜像、journal 和终态、数据责任核对账。
- **清理恢复**：只按已确认状态继续/安全终止；无法识别实际启动时隔离板卡，按救援流程处理。
- **阻塞条件**：没有真实 MCUboot TEST 标记/交换观察、缺精确切点或供电拓扑证据。

### MAN-OTA-17：nRF 同版本不同镜像与确认期限

- **原验收映射**：ESP-AT-29；ACC-15/16。
- **状态**：未执行；当前受 G5 阻塞。
- **前置/数据**：NRF_B/B2 版本字符串相同但 SHA 不同；新测试镜像/已确认镜像两组；G7 可控制 VERIFY/CONFIRM；确认窗口按实际配置记录，默认 120 s。
- **操作步骤**：
  1. 运行 B 却请求验证 B2 SHA；再发送错误长度；确认失败后尝试 CONFIRM。
  2. 未 VERIFY 直接 CONFIRM；正确 VERIFY 后故意再次验证失败；分别 UART 重握手、芯片重启后尝试复用先前验证。
  3. 正常执行 VERIFY→健康核对→CONFIRM→STATUS；另延迟超过实际确认窗口再 CONFIRM，观察状态机超时触发真实复位及 MCUboot 回旧镜像。
  4. 在隔离平台注入客户端测试成功结果缺失/错误/大写 imageSha256，区分平台验证与设备实测。
- **可观察预期**：版本字符串不能代替主槽读回；本次 boot 正确 VERIFY 才可确认，失败清除该前提；芯片重启使验证失效，单纯 link 重握手不同。超窗确认拒绝，未确认镜像由实际超时/启动机制回退；只有真实 running_confirmed=1 与匹配摘要才成功。
- **证据**：两镜像 SHA、每次 UART 输入/状态、confirm_remaining_ms、boot、主槽读回和平台拒绝记录。
- **清理恢复**：让未确认镜像实际回退或在有效预算内正确确认；恢复已确认基线。
- **阻塞条件**：无同版本异镜像测试包、缺真实主槽/确认时刻可见性或 G7；不能直接修改成功结果伪造实板证据。

### MAN-OTA-18：nRF 挂死、受控复位与确认后重传

- **原验收映射**：ESP-AT-30；ACC-14/15/16。
- **状态**：未执行；当前受 G5/G6/G7 阻塞。
- **前置/数据**：已审核的挂死/重启循环候选、可回滚 NRF_A、真实复位线及开漏/释放时序证明；默认关闭与显式板级验证启用两套已记录配置，观察 ESP 持久 reset intent/owner。
- **操作步骤**：
  1. 默认 `GW_NRF_RESET_BOARD_VERIFIED=n` 下制造 UART 不可达；核对 ESP 有限重握手且不擅自拉复位。
  2. 仅在复位板级验证和制造授权齐全的专用测试配置上，安装挂死候选；核对持久恢复意图、维护 owner、一次受控复位脉冲及释放后的新 boot。
  3. 在 reset intent 前/后、CONFIRM 前、已确认但响应丢失、确认后 ESP 掉电分别测试；记录实际回退或已确认镜像。
  4. 丢成功结果 ACK，恢复网络后观察只补交原结果、不重刷；核对业务目标按原 revision 恢复。
- **可观察预期**：不健康镜像不确认；默认门控关闭不猜测 GPIO。允许复位时开漏拉低后释放，不推挽驱高共享复位线、不反复复位；真正回旧镜像才报告回退，确认后只恢复证据/结果。
- **证据**：构建/授权清单、实际 GPIO/电压/脉冲、持久 intent 和 owner、新旧 boot/镜像、确认与 ACK、擦写次数和业务恢复。
- **清理恢复**：恢复已确认基线和经批准的板配置，撤除挂死注入；任何复位失败保持隔离诊断，不清 journal 强行解锁。
- **阻塞条件**：当前 nRF 目标构建未完成；复位线/脉冲/共享调试器未实测或缺签名授权/板清单；不得仅因默认 GPIO8、10 ms/100 ms 数值存在就启用。

## 5. 原验收编号覆盖索引

以下是本手册用例与原验收的精确映射。只覆盖注明范围；执行人须逐子场景判定，不能因一行映射而把原编号整体标为通过。

| 原 ESP 验收 | 本手册用例 | 范围说明 |
| --- | --- | --- |
| ESP-AT-13 | MAN-OTA-12 | 仅升级结果的传输/应用 ACK 分离，不覆盖全部采集批次 |
| ESP-AT-15 | MAN-OTA-12 | 仅升级任务重复、冲突及结果重放 |
| ESP-AT-16 | MAN-OTA-11 | 仅无可信时间时远程 OTA 准入/确认，不覆盖采集时间字段 |
| ESP-AT-17 | MAN-NET-09、MAN-NET-10 | 远程逐接口证明及失败回退 |
| ESP-AT-18 | MAN-NET-11 | 远程提交边界与真实主备切换 |
| ESP-AT-26 | MAN-OTA-13、MAN-OTA-14 | nRF 制品准入、双层签名及正常链路 |
| ESP-AT-27 | MAN-OTA-15 | nRF 块、重握手、传输阶段复位 |
| ESP-AT-28 | MAN-OTA-16 | FINISH/READY 与启动歧义 |
| ESP-AT-29 | MAN-OTA-13、MAN-OTA-17 | 运行镜像与确认依据 |
| ESP-AT-30 | MAN-OTA-18 | 挂死/复位、确认恢复与结果重传 |
| ESP-AT-31 | MAN-OTA-02、MAN-OTA-08、MAN-OTA-09、MAN-OTA-10、MAN-OTA-11 | 两入口应用槽、试启动/回滚及远程故障 |
| ESP-AT-32 | MAN-OTA-09 | 当前持久格式兼容；实际迁移子项须实现/夹具另行就绪 |
| ESP-AT-34 | MAN-OTA-01 | 构建/包/分区/安全材料与救援预检；不执行不可逆制造 |
| ESP-AT-36 | MAN-OTA-15、MAN-OTA-16 | DFU 各持久阶段和状态身份恢复 |
| ESP-AT-37 | MAN-NET-01 | 开窗/离线页面/关闭与访问范围 |
| ESP-AT-38 | MAN-NET-02、MAN-NET-06、MAN-NET-08 | 认证/防重、凭据保护和回退引用 |
| ESP-AT-39 | MAN-NET-03、MAN-NET-04 | 参数/扫描与真实本地在线配置 |
| ESP-AT-40 | MAN-NET-05 | LOCAL_ONLY、目标/实际分离及后续验证 |
| ESP-AT-41 | MAN-NET-06、MAN-NET-07 | 本地失败/取消/恢复与断电 |
| ESP-AT-42 | MAN-NET-08、MAN-NET-09、MAN-NET-10、MAN-NET-12 | 远程秘密安装/网络验证及本地引用纳管 |
| ESP-AT-43 | MAN-NET-12、MAN-NET-13、MAN-OTA-07 | 裁决/版本/旧任务/维护互斥 |
| ESP-AT-44 | MAN-NET-14 | 真实浏览器、信道/子网及有条件负载 |
| ESP-AT-45 | MAN-OTA-02 | 离线本地正常安装与确认 |
| ESP-AT-46 | MAN-OTA-03、MAN-OTA-04 | 认证/制品拒绝和分块幂等 |
| ESP-AT-47 | MAN-OTA-05、MAN-OTA-06、MAN-OTA-07 | 断连、期限、取消与并发 |
| ESP-AT-48 | MAN-OTA-08、MAN-OTA-09 | 本地掉电、健康回退与旧数据 |
| ESP-AT-49 | MAN-OTA-10、MAN-OTA-11、MAN-OTA-12 | 远程 ESP 正常/失败/结果交付 |
| ESP-AT-50 | MAN-OTA-01、MAN-OTA-02、MAN-OTA-05、MAN-OTA-10 | 签名包、真实浏览器与同包双入口 |

| 原系统验收 | 本手册用例 | 范围说明 |
| --- | --- | --- |
| ACC-12 | MAN-NET-04、MAN-NET-05、MAN-NET-06、MAN-NET-07、MAN-NET-08、MAN-NET-09、MAN-NET-10、MAN-NET-11、MAN-NET-12、MAN-NET-13 | 本地/远程候选、回退、凭据与互斥 |
| ACC-13 | MAN-OTA-13、MAN-OTA-14、MAN-OTA-15 | nRF 准入、两层签名和块顺序 |
| ACC-14 | MAN-OTA-15、MAN-OTA-16、MAN-OTA-18 | nRF 传输/FINISH/确认/复位边界；真实交换需 G5/G7 |
| ACC-15 | MAN-OTA-13、MAN-OTA-17、MAN-OTA-18 | nRF 运行镜像、确认与结果恢复 |
| ACC-16 | MAN-OTA-17、MAN-OTA-18 | 正常未确认超时回退及挂死受控复位/回退；板级复位门控默认关闭 |
| ACC-17 | MAN-OTA-01 | 仅前置证据检查；未完成 nRF DK/自定义板/DFU 构建不得标通过 |

本文不覆盖手机 BLE 业务 ACC-11，不替代 RF/FEM 专项 ACC-18、完整 72 小时负载或所有原始采集持久性测试。相关未执行项目继续保留在原验收计划。

## 6. 逐次执行结果表模板

| 用例/子场景 | 状态 | 执行人/UTC时间 | 板/源码/镜像SHA | 实际结果与偏差 | 证据路径 | 清理/恢复结果 | 阻塞或问题编号 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `<MAN-NET或MAN-OTA编号/子场景>` | 未执行 | 待填写 | 待填写 | 不预填“通过” | 待填写 | 待填写 | 待填写 |

通过需同时具备相应设备、平台和物理证据；无法产生规定故障或观察关键边界时填“阻塞”，并写明缺少的测试固件/注入器/目标构建/板级验证。发现失败时保留原始状态、异常包与恢复过程，不为了继续测试把失败记录改成通过。
