# GW32 ESP32-S3 固件

版本：0.1.0；日期：2026-11-19。芯片目标ESP32-S3，ESP-IDF固定v5.5.6，8MiB Flash布局。依据[需求](../../docs/esp32s3-requirements.md)、[开发设计](../../docs/esp32s3-development.md)及[本地/远程配网设计](../../docs/esp32s3-network-provisioning.md)。

本目录提供实际C固件、离线网页、制造/构建工具和测试。源码存在、主机测试通过、ESP目标编译通过、实板通过是不同状态；以[验证记录](VALIDATION.md)为准，不将未连接的硬件写为已验收。

## 工程组成

| 组件 | 责任 |
| --- | --- |
| gw_core | 制造身份读取、加密配置事务、Wi-Fi/CH390接口、候选试用、LOCAL_ONLY、版本CAS、回退、凭据存储 |
| gw_provision | 限时SoftAP、离线网页、protocomm Security2、认证限流及本地操作 |
| gw_http | HTTPS设备身份、CA/主机名验证、逐接口DNS和TLS绑定、响应大小/时限限制 |
| gw_cloud | HTTP任务轮询、持久受理/执行意图、稳定结果与应用ACK、秘密安装、网络对账和状态上报 |
| gw_wire | 直接共享nRF工程的纯C COBS/CRC32C和过滤代码；不复制协议实现 |
| gw_nrf | 1Mbaud UART1 RTS/CTS、单请求与重试、HELLO、扫描/广播读回和异步持久接管入口 |
| gw_capture | 原始批次与水位提交、Flash spool、跨射频定稿、平台逐项ACK和隔离、Peripheral独立inbox |
| gw_actions | 配置执行、ESP BLE、nRF GATT与发现、动作journal、双组件升级及恢复 |

默认UART来自已有Oct29基线：TX17/RX18/RTS15/CTS16；GPIO8只释放为输入，不因串口超时自动复位nRF。CH390 SPI及现场配网按钮需在menuconfig中按实际板卡设置；默认未指定GPIO时不启动对应硬件。源码不能替代后续PCB引脚复核。

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

## 制造配置与首次部署

普通编译产物不含生产设备秘密。启动必须具有匹配的Flash加密状态、NVS密钥、加密制造分区及设备身份；缺失时停止敏感服务，不自动清空NVS或使用通用密码。制造工具生成材料与实际烧录/不可逆安全配置是独立步骤，不能把测试密钥部署到生产设备。

云端制造字段至少包括`api_url`（HTTPS管理origin）、`namespace`、`gateway`、设备`token`、`root_ca`；分进程部署可指定`ingest_url`。热点密码、setup账号/SRP salt/verifier、时钟源和固件签名公钥使用独立字段，按制造工具/组件说明交付。禁止将工厂CSV、设备密码、签名私钥或NVS bin提交Git。

固件严格验证TLS证书和主机名，禁止重定向携带设备token至其他origin。当前HTTP等价接口是实际联网路径；MQTT broker部署仍可供其他网关使用，本固件没有伪造MQTT已接入的状态。制造指定SNTP服务器用于时间同步；SNTP不是经过密码学认证的时间源，现场网络/时间信任须单独验收。

## 本地网页配网

通过已配置并验证的现场按钮/制造授权窗口打开热点，手机或笔记本连接设备独立密码的GW32热点，访问DHCP默认网关地址（通常`http://192.168.4.1/`）。所有资源本地提供；不依赖手机App或外网CDN。热点与上行子网冲突时地址可能改变，应以当前默认网关为准。

登录使用独立setup账号材料，网页业务经Security2加密。可扫描/手填SSID、安装新凭据、选择DHCP/静态IPv4及主备顺序，预览后在线验证或明确LOCAL_ONLY保存。原密码不回显。信道切换/浏览器断连后重新认证并查询已有transactionId，不重复执行旧密文。

热点默认10min、认证空闲5min、授权上限30min；普通网络故障不会开启永久管理热点。完整客户端/安全边界与测试见[配网组件](components/gw_provision/README.md)。

## 平台远程配置

部署本提交的服务端和页面，数据库迁移会增加配网状态及凭据记录。远程修改密码还需服务器秘密库主密钥；准确配置与API见[平台配网联调](../../server/docs/network-provisioning-integration.md)。

流程为秘密安装并获得设备持久回执 → CONFIGURE只携带credentialRef → 候选逐接口探测 → RESULT_PERSISTED → 提交 → 最终结果ACK。本地改动通过generation/快照保护；管理员明确采用或覆盖，不静默覆盖现场配置。完全离线的设备只能等待上线或现场恢复。

## 存储、恢复与资源边界

低频配置/任务使用双快照和提交指针，原始采集使用独立Flash spool；具体编码、损坏和断电规则见组件及验证记录。秘密区和普通业务引用分离，配置回退仍引用的凭据不能提前回收。

任务日志最多64个保留身份且受96KiB整体配置快照预算限制，接纳新任务前为结果预留空间。空间不足时拒绝新动作，不删除旧防重记录；长期部署必须定义经审计的任务保留/归档策略。CUSTODY仅在原始记录和水位提交后发送；HTTP成功或MQTT PUBACK不替代应用ACK。

默认固件不会把未认证Peripheral RX当配网或执行指令，也不创建不存在的云端Peripheral消息类型。当前生产签名、Flash寿命、独立/整板掉电、CH390引脚、RF共存、真实移动浏览器和完整44项验收均须依据实测放行。

## 开发与测试

使用`scripts/Test-Core.ps1`验证生产存储/参数规则，`scripts/Test-Cloud.ps1`验证实际云任务状态机（依赖cJSON主机源码）；配网组件提供Node协议向量、输入守卫和浏览器模拟器。捕获和动作组件各自保留有意义的故障测试。不得把I/O替身的通过结果写为Flash/RF/OTA实板成功。

所有后续说明、验证和发布记录均以Markdown维护。机器配置、Schema、源码和测试向量保留必要格式。
