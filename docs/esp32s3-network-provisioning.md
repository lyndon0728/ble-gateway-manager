# ESP32-S3 本地 Wi-Fi 配网与平台远程网络配置

| 项目 | 内容 |
| --- | --- |
| 文档版本 / 日期 | 1.1 / 2026-11-19 |
| 需求依据 | [ESP 专项需求](esp32s3-requirements.md) ESP-NET、ESP-PROV-001～020；[验收计划](esp32s3-validation-plan.md) ESP-AT-17/18、37～44 |
| 范围 | 手机或笔记本连接网关热点，用浏览器配置上联网；平台远程修改同一组网络参数 |
| 当前状态 | 本文保留设计契约；对应固件及平台扩展已有源码/部分主机证据，完整实板验收尚未完成 |
| 协议边界 | 遵循 [GW32-E2E-1.1 三方贯通标准](gateway-end-to-end-standard.md)与[服务器协议](../server/docs/gateway-protocol.md)；本地接口与平台配网扩展已有源码/Schema，准确字段见[平台联调](../server/docs/network-provisioning-integration.md)，仍须共同部署及实板验收 |

## 1. 用户操作与职责

同一授权热点和 Security2 网页通道还提供 ESP32-S3 本地固件上传、状态查询、取消和明确安装。该功能的独立 uploadId、OTA 操作、签名格式及离线回滚见[本地与远程 OTA](esp32s3-ota.md)。OTA 和网络候选共用维护互斥；本地升级不创建网络 transactionId、不推进网络修订，窗口结束取消未提交上传但不打断已提交的试启动义务。

本地流程：**授权打开配网 → 手机/笔记本连接 GW32 热点 → 浏览器登录 → 选择上联网并填写参数 → 验证并应用 → 查看结果 → 关闭热点**。网页和所需脚本由 ESP 提供，不依赖互联网、云端 CDN 或安装手机 App。

平台流程：**有权限的管理员选择网关 → 编辑网络配置 → 必要时先安装新的 Wi-Fi 凭据 → 提交变更 → 网关验证候选网络 → 平台显示实际结果**。设备完全离线时只能等待上线或现场恢复；远程配置不能凭空建立已经断开的管理链路。

两条入口最终进入同一个 `network_owner`，使用同一校验器、候选/回退存储及接口探测器。只改变网络接入，不把密码、网页请求或网络管理命令转发给 nRF。BLE 手机 Peripheral 配网是另一项尚未冻结的业务，不作为本功能的前置条件。

| 页面状态 | 含义 | 不能据此推断 |
| --- | --- | --- |
| 参数已保存 | 本地事务和凭据已持久提交 | 已连接路由器、已有互联网 |
| 网络已连接 | 接口链路和 IP 配置有效 | 平台可达、设备认证成功 |
| 平台已连接 | 被测接口完成可信 TLS、设备认证和新鲜挑战应答 | 平台目标配置已采用本地改动 |
| 平台已同步 | 正常平台配置任务完成必要 probe、持久 ACK 和修订对账 | 其他未测试功能已通过 |
| 已回退 / 待同步 / 仅本地保存 | 如实显示具体阶段、当前使用网络和原因 | 不统一显示为“配置成功” |

## 2. 可配置参数及边界

| 参数组 | 本地网页 | 平台远程 | 约束 |
| --- | --- | --- | --- |
| 上行优先级 | Ethernet / Wi-Fi 主备顺序 | 相同 | 1～2 个不同且启用的接口；默认 Ethernet 优先 |
| Wi-Fi STA | 启停、扫描选择/手动 SSID、安全类型、密码更新 | 启停、SSID、安全类型、凭据选择/安全更新 | 2.4 GHz；SSID 1～32 UTF-8 字节；隐藏网络可手动填写；不支持模式明确拒绝 |
| Wi-Fi / Ethernet IPv4 | DHCP 或静态 | 相同 | 静态地址、prefixLength 1～30、同网段网关、1～2 个 DNS；拒绝冲突和不可用主机地址 |
| 密码 | 安全会话中录入新密码；不显示原密码 | 独立秘密接口安装后引用 credentialRef | 留空表示“保持”须显式选择；不能把空字符串默认为删除或正确密码 |
| 接入诊断 | 链路、IP、DNS、TLS、认证、失败原因 | 任务状态及逐接口结果 | 密码/会话密钥不回显；诊断不等同提交 |

普通配置对象继续使用 `network.uplinkOrder/ethernet/wifiSta` 和 `credentialRef`；`ipv4` 整对象替换。Wi-Fi 只开放当前平台认可的 `WPA2_PSK/WPA3_SAE`，不因芯片还支持其他模式就直接允许 OPEN、企业认证、WPS 或 5 GHz。密码格式按安全模式及锁定驱动验证；首版人类输入采用 8～63 字节 UTF-8 口令的产品限制，原始十六进制 PSK 暂不开放。

SoftAP 是配置入口，STA 是连接现场路由器的上行接口，页面必须分开说明；修改 STA 的 SSID/密码不修改热点密码。网关身份、租户、服务器地址、信任根和设备证书不放入普通网络表单，本轮也不新增 LTE、NAT 路由或 Wi-Fi 原始帧功能。

## 3. 热点与网页产品规则

| 项目 | 首版设计值 / 行为 |
| --- | --- |
| SSID | `GW32-SETUP-<设备标识后6位>`；标识用于辨认，不用作密码 |
| 热点保护 | 每台设备独立随机 WPA2 密码，至少 16 个随机字母数字字符；通过设备标签/制造交付提供，禁止全批通用默认密码 |
| 本地操作认证 | 独立 setup 账号/口令，与热点密码和云端设备 token 分离；设备保存 SRP salt/verifier，首次交付和重置方式在制造方案中锁定 |
| 地址 | 默认 `192.168.4.1/24`，DHCP 给客户端分配地址；与现有/候选上行子网冲突时从预设私网段中选择不重叠网段 |
| 浏览器入口 | 默认 `http://192.168.4.1/`；入口页说明当前地址；captive portal 为辅助发现，不能依赖自动弹窗或截获 HTTPS |
| 窗口 | 默认总计 10 min、已认证操作空闲 5 min 关闭，单次授权硬上限 30 min；未认证访问不续期 |
| 连接与资源 | 默认最多 1 个客户端、1 个已认证操作会话、1 个网络候选事务；静态内容压缩存储，不从外网加载 |
| 成功关闭 | 本地已提交后显示结果；在线成功默认保留至多 60 s 查看时间，再关闭 AP；不得超过窗口硬期限 |
| 失败退出 | 试用失败回退，剩余窗口内可继续修改；用户退出或硬期限到达后关 AP，未提交事务回退 |

开窗须来自明确现场授权，例如已验证的实体操作或维护流程；按钮、引脚和长按动作必须依照实际板卡冻结，本文件不虚构硬件。可在首次出厂安装状态使用预先授权的有限窗口；普通断网/云端失联不得自动变成永久开放热点。平台远程配置 STA 不需要打开 SoftAP；远程开窗没有现成命令，本版不以网络配置字段偷偷实现。

当前固件已有按钮与制造授权开窗入口，但默认 `GW_SETUP_BUTTON_GPIO=-1`、`GW_FACTORY_AUTHORIZED_SETUP=n`，因此默认配置不会打开热点，相关网页/OTA 路由也可能被链接器裁除。交付本地功能前必须按实际板卡启用并验证授权入口；不能把默认目标构建通过等同本地配网已经可用。

网页至少提供：设备型号/标识、连接说明、登录、当前网络脱敏摘要、Wi-Fi 列表与刷新/隐藏网络、密码“保持/替换”、DHCP/静态输入、Ethernet/Wi-Fi 主备、变更预览、验证并应用、明确的仅本地保存、进度/错误、恢复旧配置、退出。

移动端避免把“此网络无互联网”当作失败；指导用户保持连接并打开系统浏览器。AP+STA 同时工作时，STA 所连路由器的信道优先，热点可能迁移信道；浏览器断开不代表配置失败，重连后查询同一个 transactionId。ESP-IDF 对该行为有明确说明。[Wi-Fi 模式与信道](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-guides/wifi-driver/overview.html)

已知静态子网冲突在试用前处理；DHCP 后才发现冲突时，先迁移 SoftAP 网段并提示/允许客户端重新获取地址，再继续上行验证，不能用重叠路由得到假成功。若预设网段均冲突则拒绝候选并保留原配置。SoftAP 不向 Ethernet/STA 转发客户端流量，也不把管理服务绑定到这些上行接口。

## 4. 本地安全会话与接口契约

### 4.1 传输选择

首版使用 **SoftAP + HTTP transport + protocomm Security 2**，以 SRP6a 建立会话并用 AES-GCM 保护业务内容；仅提供 Security 2，不提供 security0 或认证失败后的明文兼容路径。该安全组件和 HTTP transport 来自 ESP-IDF；本项目浏览器适配器及网页已有源码和协议测试，真实移动浏览器/实板兼容性仍须验证，不能声称官方已提供本项目完整网页客户端。[固定版本 protocomm](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/provisioning/protocomm.html)

HTTP 可返回本地静态页面和最小发现信息；密码、配置、状态细节和控制全部置于已认证加密消息中，禁止普通明文 JSON 配置接口。使用锁定且经过审查的客户端加密实现和协议测试向量，不自创密码算法。检查普通 HTTP 源下浏览器的安全随机数/WASM/所需 API 能力；缺失时停止提交并提示兼容浏览器，不能降级为明文或使用 Math.random。

控制入口仅接受 SoftAP 接口的客户端，同源策略、Host/Origin 检查、严格内容类型、帧长/并发/速率限制均生效。每个加密会话独立计数和重放保护，重启/关窗失效；持久业务请求防重不依赖会话 RAM。默认最多 5 次认证失败后锁定当前窗口，重新现场授权才能重试，不能靠重连客户端清零。

ESP 固件0.2.1对POST正文设置15 s固定接收预算，收到新字节不续期，每次接收前后复核预算和窗口。SDK单次阻塞接收仍有最多5 s超时尾差，但迟到正文不会进入业务分派。窗口结束由监督任务主动中断已接受的socket（包括尚未完成HTTP头的连接），随后停AP并结束HTTP服务；不能等待慢客户端自行完成传输才关闭热点。

本方案的浏览器脚本由现场隔离热点分发；应用层加密并不单独证明明文静态页面的来源完整性。制造交付和安全评审必须明确这一信任边界；要求抵御恶意替代网页的部署须增加受信任的本地 HTTPS 证书交付或可信离线客户端方案，不引导用户忽略证书告警。该评审属于上线条件，不能以“使用 AES”代替。

### 4.2 HTTP 与加密业务封装

以下是本地接口契约，与云端 `/api/v1/...` 和 UART 分开。入口已由 [gw_provision](../firmware/esp32s3/components/gw_provision/README.md)实现；protobuf/session 映射以已锁定 ESP-IDF v5.5.6 及既有 Security2 协议向量核验，接口存在不替代每项产品要求的实际验收。

| 端点 | 内容 | 权限 |
| --- | --- | --- |
| `GET /`、静态资源 | 本机网页，严格 CSP、无外部脚本/遥测 | 开窗客户端 |
| `GET /prov-info` | 接口版本、设备显示标识、安全版本、当前入口地址；不含网络/凭据详情 | 开窗客户端，有速率限制 |
| `POST /prov-session` | 官方 Security 2 握手二进制报文 | 开窗、未锁定；账号认证 |
| `POST /prov-network` | Security 2 加密后的业务请求/响应 | 当前已认证会话；请求体 `application/octet-stream` |

业务解密后是 UTF-8 JSON，顶层含 `apiVersion="1.0"`、`requestId`（随机 UUID）、`operation` 和 `body`。修改操作还要求 `expectedNetworkGeneration`、`baseLocalRevision`，两者均为无符号整数的十进制字符串，避免浏览器数值截断。响应含同一 requestId、`code`、`body`；成功受理不等于提交，异步操作返回 transactionId 及阶段。

单条解密业务请求上限 16 KiB，响应上限 32 KiB，JSON 嵌套最多 8 层；扫描最多 32 AP。拒绝未知字段及不符合版本的枚举。相同 requestId 和规范业务内容返回既有结果；相同 ID 内容不同返回 `REQUEST_CONFLICT`。秘密请求的防重摘要采用设备密钥 HMAC，不写入可离线猜测的明文密码 SHA；幂等记录不保存明文请求。

| operation | 输入 / 返回 | 行为 |
| --- | --- | --- |
| `GET_STATE` | 返回当前 generation/localRevision、脱敏配置、连接/同步状态、窗口剩余时长 | 不返回旧密码、SRP verifier 或 token |
| `SCAN_WIFI` | active/passive；返回 SSID/BSSID/channel/RSSI/security | 默认 PASSIVE，可显式 ACTIVE；不启动 BLE 扫描 |
| `PUT_CREDENTIAL` | 新密码、安全模式、SSID 绑定；返回不可变 credentialRef | 原子写安全存储后才成功；未引用凭据按受控配额清理 |
| `VALIDATE_NETWORK` | 完整 network、预期版本；返回校验错误/脱敏差异 | 只校验，不改网络或安装密码 |
| `CREATE_CANDIDATE` | 完整 network、预期版本；返回 transactionId | 持久草稿并获取唯一网络租约；最多存活至窗口期限 |
| `APPLY_ONLINE` | transactionId | 最长 120 s 试用，按接口完成平台挑战验证后原子提交 |
| `SAVE_LOCAL_ONLY` | transactionId、`acknowledgeUnverified=true` | 明确选择保存未获平台证明的网络，保持原 lastVerified |
| `GET_TRANSACTION` | transactionId | 返回当前阶段、各层检测、实际网络、错误和回退状态 |
| `CANCEL_TRANSACTION` | transactionId | 未提交时取消并回退；已提交返回已提交，不隐式撤销 |
| `RESTORE_PREVIOUS` | 预期版本、前一配置引用 | 建立新的本地候选，完整执行验证或显式 LOCAL_ONLY；不倒减修订 |
| `CLOSE_WINDOW` | 无 | 未提交事务先安全回退，记录结果后关闭 AP；不删除配置 |

PUT_CREDENTIAL、候选创建和应用等修改均检查版本/所有者；已在运行的同一事务查询不因 generation 后来变化而丢失历史。跨会话查询先重新认证，事务 ID 本身不授予权限。HTTP 重试不得再次写入配置或重复推进 localRevision。

解密后的候选创建示例（实际 HTTP 正文须加密，引用是占位示例）：

```json
{
  "apiVersion": "1.0",
  "requestId": "4ce9dd33-2b3f-4d01-ae77-48a6801be082",
  "operation": "CREATE_CANDIDATE",
  "expectedNetworkGeneration": "12",
  "baseLocalRevision": "3",
  "body": {
    "network": {
      "uplinkOrder": ["WIFI_STA"],
      "ethernet": {"enabled": false, "ipv4": {"mode": "DHCP"}},
      "wifiSta": {
        "enabled": true,
        "ssid": "site-gateway",
        "security": "WPA2_PSK",
        "credentialRef": "local-site-credential-v2",
        "ipv4": {"mode": "DHCP"}
      }
    }
  }
}
```

统一错误至少包括 `AUTH_REQUIRED/AUTH_LOCKED/SESSION_EXPIRED/INVALID_ARGUMENT/UNSUPPORTED/REQUEST_CONFLICT/REVISION_CONFLICT/NETWORK_BUSY/OTA_BUSY/CREDENTIAL_NOT_INSTALLED/SUBNET_CONFLICT/TIME_UNTRUSTED/PROBE_FAILED/ROLLED_BACK/STORAGE_FAULT`；它们是本地接口代码，不自动成为现有云端 errorCode 枚举。

## 5. 本地候选、离线保存与回退

持久状态包括 `networkGeneration`（所有来源的提交世代）、`localRevision`（本地提交次数）、`lastVerified`、`activeLocalSnapshot`、候选/原配置、来源/事务 ID、凭据引用、验证证据、localOverride 和平台同步状态。候选提交同时写状态和审计；只改 RAM 或调用驱动 API 不算成功。

### 5.1 本地在线应用

`DRAFT → CANDIDATE_DURABLE → LINK/IP_CHECK → PLATFORM_PROBE → LOCAL_COMMITTED_PENDING_SYNC`。

1. 校验权限、候选完整性、引用凭据、版本 CAS 和资源租约，持久保存回退快照。
2. 试用时间为 120 s 与窗口剩余期限的较小值，以单调时钟计时；浏览器重连不能重新延长。
3. 按远程配置相同规则验证候选首选接口和 uplinkOrder 中发生变化的接口；DNS、TLS和应用请求实际绑定受测接口。
4. 使用第 7 节设备 probe 接口进行随机挑战应答；校验 TLS 主机/证书、设备身份、transactionId、接口、快照标识、nonce 和时限。不伪造云端 taskId/resultId，不借用旧连接应答。
5. 全部必要接口通过后原子提交本地快照、lastVerified、generation+1、localRevision+1 和 localOverride；显示“平台已连接，待配置同步”。云端 reportedConfigurationRevision 保持不变。
6. 超时、任一验证失败、窗口关闭或提交前重启，恢复试用前的实际配置，保留失败记录。整板重启不自动续跑旧 AP 会话或旧挑战。

可信 UTC/设备身份缺失时，不绕过 TLS 日期/主机校验来获得 ONLINE。仍可操作本地认证和保存网络；页面显示 `TIME_UNTRUSTED` 或身份未配置，并走明确的 LOCAL_ONLY。现有普通心跳、MQTT 连接成功或第三方 ping 都不能代替上述带事务绑定的应用证明。probe 在 gw_cloud 和平台已有实现；对未部署该扩展的旧平台，仍不得展示本地 ONLINE 验证通过。当前制造指定 SNTP 不提供密码学认证，时间信任验证按统一标准 C09 保留，不因接口存在视为通过。

### 5.2 LOCAL_ONLY 是明确选择

首次安装无互联网或平台暂不可达时，用户可在预览页选择“仅本地保存，平台未验证”。该操作仍要求本地认证、参数校验、凭据持久和明确确认；不是在线试用失败后自动提交。

- 原子保存 `activeLocalSnapshot`，推进 generation/localRevision，设置 localOverride 和 `LOCAL_ONLY`；**不覆盖原 lastVerified**。
- 提交后可尝试连接所存网络并显示真实链路状态。首次无旧配置时保留待连接目标；存在旧网络时按冻结的有界尝试策略恢复可用旧链路，同时保留本地已保存目标，不无限往返切网。
- 页面分别显示“已保存目标”和“当前实际使用网络”。后续在新链路完成挑战才更新验证状态和 lastVerified；发布前重新取得网络租约并核对 snapshotId/networkGeneration，陈旧验证不得覆盖较新配置。该证据更新不制造第二次配置提交，不再次推进修订。
- 保存后重启仍保留目标、旧 lastVerified 和待同步标记；不恢复过期会话，不清空 spool/身份。重新进入热点需要新的有限授权窗口。
- 若在线事务已失败并回退，必须新建候选后才能选择 LOCAL_ONLY；不复活终态事务。

回退指针至少保留当前已验证与前一可用配置及其凭据。只有新配置验证/对账结束且不再被回退或活动事务引用后才可清理旧凭据；保存新密码不立即使旧密码失效。

## 6. 平台远程配置与密码更新

### 6.1 复用的现有实现

平台管理 API `PUT /api/v1/gateways/{id}/configuration` 使用既有权限、If-Match、Idempotency-Key、expectedReportedRevision，生成 CONFIGURE 并由服务器分配 desiredRevision。[配置服务](../server/internal/platform/control.go)

现有 [平台配置页面](../frontend/src/features/gateway/ConfigurationEditor.vue) 编辑网络参数和 credentialRef，并集成 [网络配网面板](../frontend/src/features/gateway/NetworkProvisioningPanel.vue)提供凭据安装、本地状态查看和对账裁决。普通 GET configuration 返回最近补丁；管理 GET configuration/effective 合并已验证补丁，仍标记 `complete=false`、`ACKNOWLEDGED_PATCH_NOT_RUNTIME_READBACK`，不是完整设备实况；设备凭据不能当管理员调用。设备完整快照恢复缺口按统一标准 C08 管理。[合并视图与权限](../server/internal/platform/frontend_api.go)

远程试用仍完全遵循现有协议：保存 candidate → 逐接口探测 → EXECUTING/NONE 的 networkProbe → 对应 RESULT_PERSISTED 持久 ACK → 原子提交 → SUCCEEDED/VERIFIED。必要 probe 必须已在服务器前一事务接管；不得首发 probe 与成功结果在同批绕过校验。窗口最长 120 s 且受任务 TTL 限制，失败/未提交重启回退。[服务器结果校验](../server/internal/platform/control_gw32.go)

### 6.2 秘密投递接口

远程修改 SSID 所指网络的密码时，采用“**安装凭据 → 安装回执 → 下发引用 → 试用提交**”。保留 `network.wifiSta.credentialRef`，不往现有配置 Schema 或普通任务加入 password 字段。

| 接口 | 调用方与内容 | 完成条件 |
| --- | --- | --- |
| `POST /api/v1/gateways/{id}/network-credentials` | 有专项权限管理员，经 TLS 提交 SSID/security/密码、幂等键 | 秘密库加密存储后返回不可变 credentialRef/version；查询和幂等响应不回显密码 |
| `GET /api/v1/gateway-network-credentials/pending` | 当前设备身份，经 TLS 领取仅本设备待安装凭据 | 身份绑定 namespace/gateway，不接受越权查询参数；禁止缓存、代理/访问日志记录正文 |
| `POST /api/v1/gateway-network-credentials/receipts` | 设备提交 deliveryId、credentialRef/version、INSTALLED 或失败 | INSTALLED 仅在加密存储可恢复提交后发出；回执可幂等重放 |

服务器使用秘密库/KMS 保护静态秘密；ESP 使用适合目标 Flash 的加密凭据存储。密码只在受认证 TLS 秘密接口及处理过程的受控内存短暂存在，禁止明文 Flash、普通任务/result spool、URL、日志、诊断或回显。此方案保护传输与静态存储，平台秘密服务属于受信任处理者，**不宣称端到端仅设备可解密**。以后如需设备密封包，须另行定义解密公钥登记/轮换和标准格式，不能拿固件签名密钥或 token 当加密密钥。

平台投递的新凭据只有收到目标设备持久安装回执后才允许创建引用该版本的远程配置任务。本地PUT_CREDENTIAL生成的引用没有云端deliveryId，由第7节设备状态上报携带LOCAL来源的持久安装元数据（ref/version、SSID/security绑定、INSTALLED）；服务器按设备身份登记为仅本设备可引用，支持ADOPT_LOCAL，不索要密码或伪造投递回执。它是设备安装声明，不是平台能独立证明密码正确，仍须实际网络验证。失败/过期/撤销按真实deliveryId记录；撤销仍被当前网络/回退引用的凭据不得直接删除。离线网关可等待投递，但界面必须显示“待上线/待安装”；不能假报密码已经生效。

## 7. 本地改动与平台对账扩展

以下是版本化扩展的设计基线。2026-11-19已同步设备源码、服务端、Schema/OpenAPI、权限和页面；准确线上字段及兼容约束见[平台联调文档](../server/docs/network-provisioning-integration.md)。完整双入口交付仍需实板与客户端验收。

| 接口 | 关键请求与应答 | 约束 |
| --- | --- | --- |
| `POST /api/v1/gateway-network-probes` | 设备发送 local transactionId、interface、snapshotId、随机 nonce；返回同值和 receiptId | 通过设备身份和 TLS，nonce 至少 128 随机位、仅当前 boot/期限有效；逐接口连接绑定；服务端持久回执不等于配置已采用 |
| `PUT /api/v1/gateway-network-state` | 设备上报desiredLocalNetwork与actualNetwork完整脱敏快照、各自snapshotId、当前activeInterface、networkGeneration/localRevision、localOverride、验证状态、最后已知平台修订及本地凭据安装元数据 | 区分保存目标和实际激活配置；以设备身份确定租户/网关；幂等持久后返回同步token，不授予设备分配平台revision的权限 |
| `POST /api/v1/gateways/{id}/network-reconciliation` | 管理员选择 ADOPT_LOCAL/APPLY_PLATFORM，携带状态 ETag、expectedNetworkGeneration、snapshotId | 原子校验当前状态和未完成事务，生成一次性 resolutionToken 与正常 CONFIGURE，不直接改成 verified |

snapshotId 是脱敏完整网络快照的 SHA-256：按 RFC 8785 JSON canonicalization 固化字节，内容只有现有 network 字段及不可变 credentialRef，不含密码或密码散列。所有端点共用测试向量；snapshotId 不是身份认证。算法/字段版本写入接口契约，未知版本拒绝。nonce 在设备内生成，返回值必须经当前 TLS 连接验证；同一接口重试不能从旧连接缓存取“成功”。

本地提交后立即设置 localOverride。网络恢复后先上报实际状态；服务器须在同一事务保存状态并阻止冲突网络任务自动覆盖。设备本身也检查该标记和 generation，防止状态上报与旧命令到达的竞态。

- `ADOPT_LOCAL`：管理员选择当前已保存的desiredLocalNetwork，界面同时展示actualNetwork及其差异；平台以该完整network建立正常配置任务、分配自己的新revision。即使当前已回退到旧网络，也要试用并验证选定目标，不能仅修改数据库就算成功。
- `APPLY_PLATFORM`：管理员明确覆盖本地改动，任务绑定所看到的 generation/snapshotId；若现场又修改，拒绝陈旧裁决并重新展示差异。
- 两种裁决任务均验证候选uplinkOrder中的全部接口，不能只按ESP当前配置计算变化集：平台旧verified快照与设备本地现状可能不同。额外probe仍按既有持久ACK门槛执行。
- 独立任务前置条件 `networkPrecondition={expectedNetworkGeneration,snapshotId,resolutionToken}` 已纳入平台扩展及设备执行器，其中snapshotId指管理员作裁决时看到的本地已保存目标，属于比较基线，不是APPLY_PLATFORM的新候选摘要。resolutionToken须绑定设备、裁决动作、基线世代/摘要、候选network摘要及唯一任务，重放同任务幂等，不能授权另一任务。该前置条件不塞进现有network对象；共同部署时同步任务Schema/验证/hash/签名语义，普通网络任务也应带expectedNetworkGeneration防陈旧写入。
- 正常平台任务提交时推进 networkGeneration、正确回报平台 revision，清除该次裁决覆盖的 localOverride；localRevision 保留历史值。不能把本地 revision 填入 reportedConfigurationRevision。
- 与未实现或未部署扩展的旧平台兼容时，发生本地改动的设备必须保留覆盖保护，拒绝自动接受冲突的远程网络任务，并用现有允许的失败结果表达无法执行；不得忽略 schema 擅加字段。该组合的页面须注明未具备对账能力，完整双入口功能不放行。

设备的 `lastKnownPlatformRevision` 是最近可验证的平台修订，不是专用网络序号；平台其他配置组的修订变化不代表本地网络被同步。离线产生多次本地提交时按最新快照对账，同时保留有界审计历史及未终结事务证据。

裁决前处理现有任务占用：尚未下发的冲突旧任务经合法取消流程原子终结；已发送任务需收到设备终态/对账证据后才能释放占用，UNKNOWN不得直接删除或改成成功。服务器现有单活动任务限制仍适用，不能仅“暂停下发”后越过限制创建裁决任务。无安全终结证据时保持冲突并显示阻塞原因；本地当前可用网络继续用于恢复。

## 8. 并发、持久化和实现分工

| 竞争场景 | 处理 |
| --- | --- |
| 本地/远程同时提交 | 单网络租约 + generation CAS；先受理者执行，另一方 BUSY/CONFLICT 并重新读取；不按“最后收到覆盖” |
| 两个浏览器/重复点击 | 一个操作会话；稳定 requestId/transactionId；重复受理不重复写入或推进修订 |
| OTA 下载/切槽/试运行、重启任务 | 网络变更与升级维护租约互斥，试运行确认责任优先；只读状态可保留，不能失去确认链路 |
| 窗口关闭/浏览器退出 | 已提交状态不撤销；先撤销授权、中断客户端并关闭 AP，未提交候选依既有持久事务完成回退；不等待慢客户端才关闭窗口 |
| Flash 满/损坏 | 准入前预留凭据/事务/回退/审计空间；不可持久不试用，不假报保存成功 |
| Wi-Fi/BLE 共存 | 有界扫描和页面请求；采集/持久接管继续；实际降级/丢包可诊断，不静默改云端 BLE 目标 |

实际组件 `gw_provision` 管理 SoftAP、网页、加密会话和授权窗口；`gw_core` 内部的网络/凭据/存储模块负责候选与实际接口、秘密及不可变引用、持久事务；`gw_cloud` 处理任务、秘密投递、probe 和状态同步。逻辑职责仍按[开发指南](esp32s3-development.md)分离，不假定每个职责都有独立组件目录。当前网络驱动使用 `esp_wifi_set_storage(WIFI_STORAGE_RAM)`，试用不通过驱动自动写 NVS 绕过统一提交/回退；真实掉电恢复仍须验证。

存储扩展详见[持久化设计](esp32s3-persistence.md)。建议物理/逻辑分离：秘密实体在加密凭据区，事务仅引用 credentialRef/version；未发布或写入一半的秘密不可被候选引用。窗口/session 密钥不跨启动恢复；业务 transaction/journal、版本、回退引用和结果必须恢复。

当前本地页面/配置事务、远程秘密安装和本地状态对账已有源码，后续按统一标准关闭差距并完成手机/笔记本与真实双接口验收。平台已有网络表单不能替代 ESP 执行器；ESP-IDF 示例不能直接证明本项目的浏览器安全客户端或整机验证已通过。

## 9. 验收与未冻结项

本功能新增 ESP-PROV-001～020 和 ESP-AT-37～44，完整步骤见[验证计划](esp32s3-validation-plan.md)。至少覆盖首次无网、隐藏 SSID、错误密码、静态冲突、AP 信道迁移、浏览器重连、认证攻击、凭据安装失败、掉电各提交点、完全离线的远程任务、双入口冲突及 OTA 互斥。

当前工程已锁定 ESP-IDF v5.5.6、NimBLE、CH390 0.4.1，并有分区/秘密存储实现、平台扩展 Schema/ACL/幂等及数据库迁移。上线前仍须按目标部署核实现场开窗动作、制造标签和 setup 凭据重置、静态页信任、分区容量/Flash 寿命、秘密库保护与恢复、共同部署/迁移、国家信道、后台重试及共存指标。浏览器/协议/服务器主机测试已有证据；目标构建及其余验证的实际状态见[固件VALIDATION](../firmware/esp32s3/VALIDATION.md)，实板及完整手机兼容验收仍待执行。

工程 API/Kconfig 以已锁定 v5.5.6 核对，浮动 stable 页不改变编译基线。参考 [Wi-Fi 驱动 API](https://docs.espressif.com/projects/esp-idf/en/v5.5.6/esp32s3/api-reference/network/esp_wifi.html) 和 [JSON Canonicalization Scheme](https://www.rfc-editor.org/rfc/rfc8785)。本版 1.1 只修正文档现状、补统一标准引用和兼容说明，不改变既有接口、功能或验收编号。
