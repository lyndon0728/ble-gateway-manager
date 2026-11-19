# ESP32-S3 本地网页配网与 OTA 组件

版本：0.2.1；日期：2026-11-19。关联[配网设计](../../../../docs/esp32s3-network-provisioning.md)、[本地与远程 OTA 协议](../../../../docs/esp32s3-ota.md)，对应 ESP-PROV-001～020、ESP-EOTA-009～020。

本组件实现本机离线网页、Security 2 浏览器客户端、受限 HTTP transport、认证窗口、`gw_core` 网络事务适配和 `gw_actions` 本地固件升级入口。网页不依赖 CDN 或手机 App。代码、主机测试、目标构建与实板验收分别记录；实际通过范围见[固件验证记录](../../VALIDATION.md)。

## 运行接线

`gw_provision_init()`创建窗口监督任务但不开热点。经现场授权后调用`gw_provision_open_window(600)`，仅接受1～1800秒；未认证访问不能延长窗口。5分钟无已认证业务操作关闭；本地在线提交成功后最多保留60秒查看期。`gw_provision_close_window()`异步收尾，撤销未提交本地候选并通知 OTA 取消未提交上传，再停止HTTP与SoftAP；已提交配置保持，已提交 OTA 的启动确认/回滚义务继续。

核心必须先初始化Wi-Fi/网络/加密存储。组件调用`gw_core_ap_start/stop/ap_netif`，不另建STA状态机。制造区须具有`setup_user`、`sec2_salt`（16字节）、`sec2_verifier`（384字节）、独立`ap_ssid/ap_password`；缺少任一必需材料拒绝开窗。本仓库测试口令仅是公开测试数据，不能用于制造。

0.2.1增加`gw_provision_io`固定接收期限与socket跟踪。POST正文预算15 s，单次阻塞接收仍受SDK的5 s超时约束；每次接收前后检查窗口及固定期限，迟到的最后一块也不分派。关窗主动shutdown所有已接受连接，打断HTTP头/正文读取，再停止AP和HTTP服务。shutdown和最终close使用同一mutex避免fd重用竞态；最终close由HTTP任务负责，不使用只会排队的关闭API等待慢客户端。新增`tests/io_test.c`直接测试生产helper，主机I/O替身不等同真实Wi-Fi验证。

固件必须启用`CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2`，禁用Security 0/1兼容入口。当前实现要求官方Security 2 `patch_ver=1`，每次请求和响应递增96位nonce的末32位大端计数；旧固定nonce方案拒绝连接。浏览器SHA-512/AES-GCM由锁定Noble库提供，SRP和protobuf适配依据ESP-IDF v5.5.6官方源码实现。

## HTTP与页面

| 入口 | 行为 |
| --- | --- |
| `/`、`/app.js`、`/style.css` | 全部由固件内嵌资源返回，严格CSP，无外部请求 |
| `GET /prov-info` | 仅接口/安全版本与锁定状态，无网络详情或秘密 |
| `POST /prov-session` | 官方Security 2 protobuf握手，校验账户和完整消息结构 |
| `POST /prov-network` | 仅已认证 AES-GCM 密文，解密后将配网操作交给核心 dispatcher，将 OTA 操作交给 gw_actions |

所有请求检查socket本地地址为当前SoftAP地址，拒绝STA/Ethernet侧访问；Host必须为实际热点IPv4，POST还要求匹配Origin和`application/octet-stream`。会话使用256位随机HttpOnly/SameSite cookie并绑定客户端IP，最多一个认证会话。窗口内最多5次认证/受保护请求失败，重新连接不能清零。解密请求16KiB、响应32KiB、JSON深度8；重复JSON字段、无效UTF-8/JSON和非法握手不能直达核心。最大会话消息数限制在nonce回绕之前。

页面包括登录、SSID扫描/隐藏名称输入、凭据保持/选择/替换、WPA2/WPA3、DHCP/静态IPv4、Ethernet/Wi-Fi优先级、验证预览、在线应用、明确LOCAL_ONLY保存、事务查询/取消、恢复前一配置和关闭窗口。还提供 ESP 应用签名包选择、固件上传、进度/状态、取消和明确安装确认。SSID 和升级状态以文本节点显示，不能执行外部输入中的 HTML。

密文响应丢失后客户端立即废弃会话，不重发已经使用过的密文；重新登录后由 GET_STATE 找回本地网络事务，查询原 transactionId；OTA 通过 OTA_STATUS 和原 uploadId 对账。浏览器只在 sessionStorage 保留网络事务 ID 与 OTA 上传 ID，不持久保存固件文件、密码、SRP 材料或会话密钥。密码写入后清空输入框，回显仅包含凭据引用。密钥和输入缓冲尽可能清零；JavaScript 运行时字符串回收不能声称具有物理内存擦除保证。

HTTP静态页的信任边界与[专项设计](../../../../docs/esp32s3-network-provisioning.md)一致：受保护业务协议不单独保证恶意替代静态页面的完整性。没有可信制造材料不启动配网，不以浏览器兼容问题降级成明文。自动captive portal/DNS不是本版本的必需入口，使用热点默认网关的直接HTTP地址。

## 本地 OTA 操作

选择发行工具生成的 `verification.json` 和 `firmware.bin`。页面检查包结构、组件与文件长度，设备独立核验 Ed25519 签名、型号/硬件/源版本、完整长度/摘要和 ESP 应用格式；浏览器检查不替代设备判定。固件只写非运行应用槽，不提供匿名上传、任意 Flash 地址或本地 nRF 升级入口。

| 操作 | 页面行为 |
| --- | --- |
| OTA_BEGIN | 创建独立 uploadId，发送原始 manifest 与 signature；同一 ID 不得换包 |
| OTA_WRITE | 每块至多 4096 字节，Base64 经 Security2 发送；按设备 offset 推进进度 |
| OTA_STATUS | 登录/重连后查询；只有同启动 RECEIVING 可按偏移续传，READY 等待明确安装 |
| OTA_ABORT | 用户可在上传过程中取消；当前请求完成后立即提交取消，避免被界面忙碌状态吞掉 |
| OTA_COMMIT | READY 后经用户确认提交；显示准备重启，不把 ACCEPTED 误报为安装成功 |

本地 OTA 不依赖互联网或 UTC。两入口共用动作与维护租约；网络候选、远程 ESP OTA 或 nRF OTA 占用时返回 BUSY，不悄悄覆盖。BEGIN 固定该次窗口截止，空闲 5 min 或窗口关闭终止未提交上传；设备重启中断的接收需新 ID 从零开始。

页面将未知响应/查询失败后的旧 READY 状态失效，必须重新查询才可安装。最终成功是 CONFIRMED，失败/回退分别显示 FAILED/ROLLED_BACK；新应用独立完成 90 s 内健康确认，无须页面保持连接。具体字段、错误码、10 s 启动观察、分区及断电策略见[OTA 协议](../../../../docs/esp32s3-ota.md)。

## 构建网页

固件构建直接嵌入已提交的`assets/app.js`，不需要在线下载JavaScript库。开发修改`web/src`后用固定esbuild 0.25.5重新打包；`ESBUILD`指定其可执行文件，未设置时使用仓库忽略目录`.tools/esp-provision-web/esbuild/package/esbuild.exe`。

```powershell
# 在仓库根目录，预先安装/指定 esbuild 0.25.5。
node firmware/esp32s3/components/gw_provision/build-web.mjs
node --test firmware/esp32s3/components/gw_provision/tests/security2.test.mjs firmware/esp32s3/components/gw_provision/tests/ota.test.mjs
```

源码和依赖的版权、版本及唯一导入路径调整见[第三方说明](THIRD_PARTY.md)。脚本生成bundle SHA-256；目标构建不会在缺少资源时悄悄拉取未锁定依赖。

## 已执行的主机验证

2026-11-19执行：

- 11 项 Node 测试通过：8 项 Security2 协议测试覆盖 8 组上游 Python SRP/OpenSSL AES-GCM 向量、设备 C 前导零边界、伪证明/非法公钥、篡改/重放、nonce 溢出、protobuf、无损版本和丢响应不重发密文；另 3 项 OTA 测试覆盖签名原文保留与文件选择拒绝、上传 ID/长度/偏移续传检查、完整字节 Base64 分块与请求上限。
- Chrome 无头浏览器烟测通过：运行最终内置包及独立 Node crypto 模拟服务器，完成 SRP/AES-GCM、登录、扫描、凭据安装、预览、LOCAL_ONLY、查询/关闭；新增 OTA 签名拒绝响应、块响应丢失后的重新认证/续传、在途取消、READY 明确安装及结果查询，并检查查询失败后不复用旧 READY。覆盖 390×844 与 1366×900 视口；属于桌面 Chrome 测试，不代替真实 Android/iOS 或 Safari。模拟服务不验证真实 Flash 镜像签名或执行启动自检。
- 实际C输入守卫经Zig 0.14.1主机编译和测试通过：UTF-8非法序列、NUL/转义NUL、深度8边界、输入长度及带引号字符串。cJSON仍负责完整JSON语法校验。
- 上述测试使用模拟核心/API，不证明 ESP HTTP 接口绑定、Flash 持久化、OTA 试启动/回滚或无线操作；ESP-AT-37～50 的适用实板/服务器联调部分仍需执行。

```powershell
# 需仓库 frontend 的 @playwright/test 依赖和本机Chrome。
# 其他浏览器可通过 GW_TEST_BROWSER 指定可执行文件路径。
node firmware/esp32s3/components/gw_provision/tests/browser-smoke.mjs

# 仅在变更参考版本时重新生成公开向量；需要 Python cryptography。
python firmware/esp32s3/components/gw_provision/tests/generate_vectors.py
```

C输入守卫可用任意C11编译器构建`gw_provision_guard.c`和`tests/guard_test.c`后运行。当前Windows验证采用Zig `cc -std=c11 -Wall -Wextra -Werror`；初次并行编译遇到LLVM内存不足，限制编译进程为两个CPU后通过，不是源文件编译错误。

发现并覆盖的兼容性边界：v5.5.6 Python参考`calculate_x`把内层SHA-512转换为整数，首字节为零时会截短；真实设备C保留完整64字节。本浏览器采用设备C行为，单独的回归向量验证前导零hash及salt，不为匹配Python边界缺陷而损害设备互通。制造verifier应按设备C公式生成。
