# GW32 ESP32-S3 固件

版本：0.3.0；日期：2026-11-19。芯片目标ESP32-S3，ESP-IDF固定v5.5.6，8MiB Flash布局。依据[需求](../../docs/esp32s3-requirements.md)、[开发设计](../../docs/esp32s3-development.md)、[本地/远程配网设计](../../docs/esp32s3-network-provisioning.md)及[本地/远程 OTA 协议](../../docs/esp32s3-ota.md)。本次需求落实与修复见[变更记录](CHANGELOG.md)。

本目录提供实际C固件、离线网页、制造/构建工具和测试。源码存在、主机测试通过、ESP目标编译通过、实板通过是不同状态；以[验证记录](VALIDATION.md)为准，不将未连接的硬件写为已验收。

## 工程组成

| 组件 | 责任 |
| --- | --- |
| gw_core | 制造身份读取、加密配置事务、Wi-Fi/CH390接口、候选试用、LOCAL_ONLY、版本CAS、回退、凭据；gw_time维护签名UTC误差区间/单调锚 |
| gw_provision | 限时SoftAP、离线网页、protocomm Security2、认证限流及本地操作 |
| gw_http | HTTPS设备身份、CA/主机名及整个可信时间区间验证、逐接口DNS/TLS绑定、有界无凭据时间挑战 |
| gw_cloud | 共用任务入口/journal、应用ACK及秘密安装；gw_sync核组件库存、完整快照及能力；gw_signed_time刷新签名时间锚 |
| gw_wire | 直接编译nRF工程纯C COBS/CRC32C及gw_filter_v2，共享同一谓词/PAIR实现 |
| gw_authority / gw_crypto | 制造双层签名、设备身份/SHA绑定、授权与板验证能力交集；公共Ed25519实现 |
| gw_transport | MQTT QoS1和HTTPS共用任务/spool/应用ACK，切换保留业务身份 |
| gw_peripheral | 两radio独立Security2业务身份、PG分片、route授权、持久业务spool/ACK与回发 |
| gw_nrf | 1Mbaud UART1 RTS/CTS、HELLO、统一V2 JSON编译、完整配置/状态读回、受控reset及持久接管入口 |
| gw_capture | 原始批次与水位提交、Flash spool、跨射频定稿、平台逐项ACK和隔离、Peripheral独立inbox |
| gw_actions | 配置执行、ESP BLE、nRF GATT与发现、动作journal、双组件升级及恢复 |

默认UART来自已有Oct29基线：TX17/RX18/RTS15/CTS16；默认GPIO8只释放为输入，不因普通串口超时自动复位nRF。受控复位须启用GW_NRF_RESET_BOARD_VERIFIED并核实GPIO/脉宽/释放等待；先持久恢复意图并持有维护租约，开漏拉低后释放，HELLO核新boot。开关默认关闭，默认10ms/100ms不是实板验证结论。CH390 SPI及现场配网按钮需在menuconfig中按实际板卡设置；默认未指定GPIO时不启动对应硬件。源码不能替代后续PCB引脚复核。

**默认配置没有可开启本地热点的入口。** `GW_SETUP_BUTTON_GPIO=-1` 且 `GW_FACTORY_AUTHORIZED_SETUP=n` 时，`app_main` 不调用开窗入口，链接器可能裁除 HTTP、本地 OTA 路由与网页资源；默认镜像构建成功不能作为完整本地功能已链接的证据。量产前须按实际板卡核实设置按钮 GPIO（低电平持续 3 s 开窗），或实现并验证其他现场授权入口；不得猜测 GPIO。

## 构建

使用已安装的官方ESP-IDF v5.5.6环境，依赖版本由组件manifest和生成的lock约束；不要跟踪master构建发布镜像。

```powershell
# 在仓库根目录的ESP-IDF PowerShell中执行。
idf.py --version
Set-Location -LiteralPath firmware/esp32s3
idf.py -DIDF_TARGET=esp32s3 build
idf.py size
```

工程级GitHub Actions会使用相同版本容器构建并保存日志和产物。工作流文件存在不等于已成功运行，实际运行结论见VALIDATION。构建不会烧录、擦除设备或写eFuse。

### 完整本地入口的构建验证配置

仓库提供明确用于开发验证的 `sdkconfig.local-setup-test`，只设置 `CONFIG_GW_FACTORY_AUTHORIZED_SETUP=y`。它保留默认的按钮 GPIO 禁用状态；未配置网络且尚无 `core/setup-used` 标记的首次启动，在持久记录已用标记后尝试打开一次 600 s 窗口。窗口仍要求合法制造身份、加密状态及独立 setup 凭据；开窗失败也不自动清除已用标记。此配置不是量产按钮方案，不提供后续任意重开窗口的能力。

Windows 可从仓库根目录运行；脚本对中文路径自动创建独立 ASCII 源码副本：

```powershell
.\firmware\esp32s3\scripts\Build.ps1 `
  -SdkRoot "$env:LOCALAPPDATA/GW32/idf-v5.5.6" `
  -ToolsRoot "$env:LOCALAPPDATA/GW32/espressif" `
  -LocalSetupTest
```

在其他已安装官方 SDK 的环境，进入 `firmware/esp32s3` 后使用新构建目录和独立配置：

```sh
idf.py -B build-local-setup-test \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG=build-local-setup-test/sdkconfig.local-setup-test \
  '-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.local-setup-test' build
idf.py -B build-local-setup-test size
```

`SDKCONFIG_DEFAULTS` 不覆盖已生成 sdkconfig 中的缓存值。验证切换配置时必须换用尚不存在的构建目录及独立 `SDKCONFIG`，并相应修改命令的两个路径；不要复用默认构建或生产板配置。`Build.ps1 -LocalSetupTest` 强制新目录、自动隔离 sdkconfig，并保留原目录。目标验证应同时确认最终 sdkconfig 的授权开关，以及 map/ELF 中本地处理函数和内置网页处于有效链接区；仅看到 archive/object 文件编译过并不够。实际目标结果见 VALIDATION。

## 制造配置与首次部署

普通编译产物不含生产设备秘密。启动必须具有匹配的Flash加密状态、NVS密钥、加密制造分区及设备身份；缺失时停止敏感服务，不自动清空NVS或使用通用密码。制造工具生成材料与实际烧录/不可逆安全配置是独立步骤，不能把测试密钥部署到生产设备。

NimBLE持久bond使用SDK固定的默认`nvs`分区/`nimble_bond`命名空间。启动以既有制造NVS密钥显式初始化`gwfactory`、`gwstore`和`nvs`三分区，未调用自动生成密钥/eFuse的默认初始化路径。BLE启动前检查预置key记录，SDK恢复后逐条核安全材料及数量；读错、长度不符或恢复不完整则停止BLE启动。空库允许基础BLE，但BONDED_ENCRYPTED明确返回未找到绑定。预置记录必须匹配锁定IDF/NimBLE结构与容量；旧明文/不兼容NVS须经过授权制造迁移，设备不会自动擦除或清空bond。


云端制造字段至少包括`api_url`（HTTPS管理origin）、`namespace`、`gateway`、设备`token`、`root_ca`；分进程部署可指定`ingest_url`。热点密码、setup账号/SRP salt/verifier、时钟源和固件签名公钥使用独立字段，按制造工具/组件说明交付。禁止将工厂CSV、设备密码、签名私钥或NVS bin提交Git。

固件严格验证TLS证书和主机名，禁止重定向携带设备token至其他origin。0.3.0已实现HTTPS与MQTT适配，制造cloud_link选择HTTPS/MQTT/AUTO；MQTT需要broker origin、独立设备凭据和ACL，PUBACK不回收业务。SNTP仅作候选；独立制造时间公钥验证nonce/boot绑定的签名时间包，误差区间及有效期限制TLS和远程动作。100ppm上界须有板级依据，缺合格制造时间材料不提升为可信时间。

## 本地网页配网

通过已配置并验证的现场按钮/制造授权窗口打开热点，手机或笔记本连接设备独立密码的GW32热点，访问DHCP默认网关地址（通常`http://192.168.4.1/`）。所有资源本地提供；不依赖手机App或外网CDN。热点与上行子网冲突时地址可能改变，应以当前默认网关为准。

必须先满足上述构建与现场授权条件；默认未选择入口的固件不会因手机连接需求而自动开启热点。测试配置只允许首次未配网开窗，无法代替升级后重新查询所需的正式按钮或其他经验证的现场入口。

登录使用独立setup账号材料，网页业务经Security2加密。可扫描/手填SSID、安装新凭据、选择DHCP/静态IPv4及主备顺序，预览后在线验证或明确LOCAL_ONLY保存。原密码不回显。信道切换/浏览器断连后重新认证并查询已有transactionId，不重复执行旧密文。

热点默认10min、认证空闲5min、授权上限30min；普通网络故障不会开启永久管理热点。完整客户端/安全边界与测试见[配网组件](components/gw_provision/README.md)。

## 平台远程配置

部署本提交的服务端和页面，数据库迁移会增加配网状态及凭据记录。远程修改密码还需服务器秘密库主密钥；准确配置与API见[平台配网联调](../../server/docs/network-provisioning-integration.md)。

流程为秘密安装并获得设备持久回执 → CONFIGURE只携带credentialRef → 候选逐接口探测 → RESULT_PERSISTED → 提交 → 最终结果ACK。本地改动通过generation/快照保护；管理员明确采用或覆盖，不静默覆盖现场配置。完全离线的设备只能等待上线或现场恢复。

## ESP32-S3 本地与远程 OTA

本地升级：在授权热点窗口登录网页，选择发行包内 `verification.json` 和 `firmware.bin`，分块上传并查看进度；完整验签、长度/摘要及应用校验后显示 READY，再明确确认安装。数据沿用 Security2 加密通道，不提供匿名/明文写 Flash 入口。断连后重新认证查询原 uploadId；同次 ESP 启动可按设备偏移继续，设备重启中断的上传需新 ID 从零开始。

平台升级：上传相同 bin、原始 manifest 和签名创建 FIRMWARE 制品，向网关下发 `FIRMWARE_UPGRADE`，`component=GATEWAY`、artifactId 和 expectedFirmwareVersion。设备通过授权 HTTPS 接口下载，独立验签并检查任务期限，完成真实启动确认后持久回传结果直到 RESULT_PERSISTED。

两入口共用双应用槽及维护互斥，保留网络配置、身份和已接管数据。ESP 试启动确认窗口为 90 s；本地升级可完全离线确认，远程升级仍需符合任务授权和在线恢复条件。失败采用 SDK 应用回滚；不会仅因文件上传完就显示升级成功，也不会自动烧写 Secure Boot/eFuse。

发行工具为 `scripts/package_ota.py`，使用仓库外 Ed25519 PEM 私钥生成同包双入口制品，命令、具体字段、断电策略及完整验收见[OTA 文档](../../docs/esp32s3-ota.md)。签名公钥须已在设备制造配置和服务器发行信任中登记。首次安装本功能需要现有远程或有线更新路径。

## 存储、恢复与资源边界

低频配置/任务使用双快照和提交指针，原始采集使用独立Flash spool；与原设计的后端差异、容量和恢复边界见[实现决策](IMPLEMENTATION.md)。秘密区和普通业务引用分离，配置回退仍引用的凭据不能提前回收。

任务日志最多64个保留身份且受96KiB整体配置快照预算限制，接纳新任务前为结果预留空间。空间不足时拒绝新动作，不删除旧防重记录；长期部署必须定义经审计的任务保留/归档策略。CUSTODY仅在原始记录和水位提交后发送；HTTP成功或MQTT PUBACK不替代应用ACK。

0.3.0已接独立Peripheral事件/ACK/回发合同；公开RX仍不是管理入口。业务Security2身份、route授权及持久STORED由gw_peripheral处理，不复用现场配网管理员会话。当前生产签名、Flash寿命、独立/整板掉电、CH390引脚、RF共存、真实移动浏览器和完整50项验收均须依据实测放行。

## 无线对齐与能力启用

V2具名能力为`NRF54_BLE_FILTER_V2`、`ESP32_BLE_FILTER_V2`，nRF还需HELLO bit1/bit13。新设备用0x19/0x1A完整wrapper原子切换LEGACY/V2，0x1B读96B计数；旧设备继续0x05/0x06且拒绝V2。名称、UUID、RSSI、Major/Minor及Service UUID组合掩码与500ms PAIR见[专项](../../docs/beacon-filter-spec.md)。本机回调只复制报告，worker判定/过期；输入队列和PAIR缓存以静态断言限定≤4096B。

ESP注册与nRF一致的INFO/RX/TX服务，`ESP32_BLE_PERIPHERAL`按制造授权与实板交集启用。Central/Peripheral共用一连接，扫描与广播互斥；完整目标同时启用二者时拒绝并保留原目标，不暗停另一业务。已有Peripheral期间新revision的enabled=true广播更新拒绝，enabled=false不隐式断连。广播成功需完整配置及ACTIVE/STOPPED证据，PAUSED须有真实连接原因；缺证据保留UNKNOWN。

BONDED_ENCRYPTED仅从安全预置bond恢复加密并核实际安全级；缺bond拒绝，不降级明文或自动首次配对。自动连接规则仍只选NRF54。软件实现、制造授权、实板验证分别约束能力发布，见[实施记录](../../docs/gateway-alignment-implementation.md)。

## 开发与测试

使用`scripts/test_host.py --cc <编译器> --idf <SDK源码目录>`运行跨模块生产逻辑测试；Python需cryptography和jsonschema。单模块入口包括`Test-Core.ps1`、`Test-Cloud.ps1`、`Test-Gatt.ps1`以及采集组件测试脚本；配网组件提供Node协议向量、输入守卫和浏览器模拟器。不得把I/O替身的通过结果写为Flash/RF/OTA实板成功。

所有后续说明、验证和发布记录均以Markdown维护。机器配置、Schema、源码和测试向量保留必要格式。
