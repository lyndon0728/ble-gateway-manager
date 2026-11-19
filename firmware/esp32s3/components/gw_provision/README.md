# ESP32-S3 本地网页配网组件

版本：0.1.0；日期：2026-11-19。关联[配网设计](../../../../docs/esp32s3-network-provisioning.md)和 ESP-PROV-001～020。

本组件实现本机离线网页、Security 2 浏览器客户端、受限 HTTP transport、认证窗口及 `gw_core` 网络事务适配。网页不依赖 CDN 或手机 App。代码存在和主机测试通过不代表已完成 ESP 目标构建、制造配置、无线共存或实板验收；目标构建结果由工程级验证记录另行记录。

## 运行接线

`gw_provision_init()`创建窗口监督任务但不开热点。经现场授权后调用`gw_provision_open_window(600)`，仅接受1～1800秒；未认证访问不能延长窗口。5分钟无已认证业务操作关闭；本地在线提交成功后最多保留60秒查看期。`gw_provision_close_window()`异步收尾，先撤销未提交本地候选，再停止HTTP与SoftAP；已提交配置保持。

核心必须先初始化Wi-Fi/网络/加密存储。组件调用`gw_core_ap_start/stop/ap_netif`，不另建STA状态机。制造区须具有`setup_user`、`sec2_salt`（16字节）、`sec2_verifier`（384字节）、独立`ap_ssid/ap_password`；缺少任一必需材料拒绝开窗。本仓库测试口令仅是公开测试数据，不能用于制造。

固件必须启用`CONFIG_ESP_PROTOCOMM_SUPPORT_SECURITY_VERSION_2`，禁用Security 0/1兼容入口。当前实现要求官方Security 2 `patch_ver=1`，每次请求和响应递增96位nonce的末32位大端计数；旧固定nonce方案拒绝连接。浏览器SHA-512/AES-GCM由锁定Noble库提供，SRP和protobuf适配依据ESP-IDF v5.5.6官方源码实现。

## HTTP与页面

| 入口 | 行为 |
| --- | --- |
| `/`、`/app.js`、`/style.css` | 全部由固件内嵌资源返回，严格CSP，无外部请求 |
| `GET /prov-info` | 仅接口/安全版本与锁定状态，无网络详情或秘密 |
| `POST /prov-session` | 官方Security 2 protobuf握手，校验账户和完整消息结构 |
| `POST /prov-network` | 仅已认证AES-GCM密文，解密后调用共享核心dispatcher |

所有请求检查socket本地地址为当前SoftAP地址，拒绝STA/Ethernet侧访问；Host必须为实际热点IPv4，POST还要求匹配Origin和`application/octet-stream`。会话使用256位随机HttpOnly/SameSite cookie并绑定客户端IP，最多一个认证会话。窗口内最多5次认证/受保护请求失败，重新连接不能清零。解密请求16KiB、响应32KiB、JSON深度8；重复JSON字段、无效UTF-8/JSON和非法握手不能直达核心。最大会话消息数限制在nonce回绕之前。

页面包括登录、SSID扫描/隐藏名称输入、凭据保持/选择/替换、WPA2/WPA3、DHCP/静态IPv4、Ethernet/Wi-Fi优先级、验证预览、在线应用、明确LOCAL_ONLY保存、事务查询/取消、恢复前一配置和关闭窗口。SSID以文本节点显示，不能执行扫描结果中的HTML。

密文响应丢失后客户端立即废弃会话，不重发已经使用过的密文；重新登录后由GET_STATE找回本地事务，查询原transactionId。浏览器只在sessionStorage保留事务ID，不保存密码、SRP材料或会话密钥。密码写入后清空输入框，回显仅包含凭据引用。密钥和输入缓冲尽可能清零；JavaScript运行时字符串回收不能声称具有物理内存擦除保证。

HTTP静态页的信任边界与[专项设计](../../../../docs/esp32s3-network-provisioning.md)一致：受保护业务协议不单独保证恶意替代静态页面的完整性。没有可信制造材料不启动配网，不以浏览器兼容问题降级成明文。自动captive portal/DNS不是本版本的必需入口，使用热点默认网关的直接HTTP地址。

## 构建网页

固件构建直接嵌入已提交的`assets/app.js`，不需要在线下载JavaScript库。开发修改`web/src`后用固定esbuild 0.25.5重新打包；`ESBUILD`指定其可执行文件，未设置时使用仓库忽略目录`.tools/esp-provision-web/esbuild/package/esbuild.exe`。

```powershell
# 在仓库根目录，预先安装/指定 esbuild 0.25.5。
node firmware/esp32s3/components/gw_provision/build-web.mjs
node --test firmware/esp32s3/components/gw_provision/tests/security2.test.mjs
```

源码和依赖的版权、版本及唯一导入路径调整见[第三方说明](THIRD_PARTY.md)。脚本生成bundle SHA-256；目标构建不会在缺少资源时悄悄拉取未锁定依赖。

## 已执行的主机验证

2026-11-19执行：

- 8组Node协议测试通过：8组上游Python SRP与OpenSSL AES-GCM向量、2组设备C算法的前导零边界、伪证明/非法公钥、密文篡改/重放、nonce溢出、protobuf异常、无损版本字段和丢响应不重发密文。
- Chrome无头浏览器烟测通过：运行最终打包资源和独立Node crypto模拟服务器，完成SRP握手、AES-GCM、登录、扫描、密码安装、预览、LOCAL_ONLY、查询和关闭。测试390×844与1366×900视口；这是桌面Chrome视口测试，不代替真实Android/iOS或Safari。
- 实际C输入守卫经Zig 0.14.1主机编译和测试通过：UTF-8非法序列、NUL/转义NUL、深度8边界、输入长度及带引号字符串。cJSON仍负责完整JSON语法校验。
- 上述测试使用模拟核心/API，不证明ESP HTTP接口绑定、实际Flash持久化或无线操作；ESP-AT-37～44的实板部分仍需执行。

```powershell
# 需仓库 frontend 的 @playwright/test 依赖和本机Chrome。
# 其他浏览器可通过 GW_TEST_BROWSER 指定可执行文件路径。
node firmware/esp32s3/components/gw_provision/tests/browser-smoke.mjs

# 仅在变更参考版本时重新生成公开向量；需要 Python cryptography。
python firmware/esp32s3/components/gw_provision/tests/generate_vectors.py
```

C输入守卫可用任意C11编译器构建`gw_provision_guard.c`和`tests/guard_test.c`后运行。当前Windows验证采用Zig `cc -std=c11 -Wall -Wextra -Werror`；初次并行编译遇到LLVM内存不足，限制编译进程为两个CPU后通过，不是源文件编译错误。

发现并覆盖的兼容性边界：v5.5.6 Python参考`calculate_x`把内层SHA-512转换为整数，首字节为零时会截短；真实设备C保留完整64字节。本浏览器采用设备C行为，单独的回归向量验证前导零hash及salt，不为匹配Python边界缺陷而损害设备互通。制造verifier应按设备C公式生成。
