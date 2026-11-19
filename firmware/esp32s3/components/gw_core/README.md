# ESP32-S3 核心、网络与配置存储

版本0.1.0；日期2026-11-19。代码基线锁定ESP-IDF v5.5.6，CH390组件锁定espressif/ch390 0.4.1。目标构建入口见[Build.ps1](../../scripts/Build.ps1)，SDK安装入口见[Install-IDF.ps1](../../scripts/Install-IDF.ps1)。安装脚本不烧录、不擦除设备、不修改eFuse。

## 已编写的执行路径

- Wi-Fi STA、受控SoftAP、DHCP/静态IPv4、被动/主动AP扫描、重新连接及主备默认路由；可选CH390 SPI Ethernet适配。
- 两入口共享候选事务与networkGeneration CAS，本地另维护localRevision；独立云端任务身份、120秒试用、接口绑定验证回调、提交/失败回退和重启恢复。
- LOCAL_ONLY显式保存，保留lastVerified；试用目标链路最多120秒，超时或期间重启有历史验证网络时回退实际链路，保存目标继续保留，重启不延长试用。若实际链路仍为已保存目标，可在时间/平台稍后可用时做有界证据验证；不会切回已经回退的目标。证明绑定原提交快照及事务，成功只更新证明状态，不再次推进配置版本。
- 加密NVS低频控制快照、同一快照中的请求意图/结果、基于HMAC的稳定本地凭据引用、不可变凭据元数据和独立加密秘密实体。
- SoftAP与现有/候选上行网段冲突检测，使用4个候选私网段；不配置NAT或桥接。地址变化会使客户端重新获取IP，网页通过当前网关地址重新进入。

这些是源码实现，不代表射频、目标SDK、真实Flash掉电或平台整机联调已通过。调用接口定义在[gw_core.h](include/gw_core.h)。上层必须先认证，再调用dispatch；core不接受任意公开HTTP请求作为授权。

## 存储提交与容量

gwstore为独立加密NVS分区。控制KV使用两个snapshot和一个active指针：写新snapshot、commit、读回验证，再发布active、commit、读回；只有完成后返回成功。任何模糊写入失败使当前实例停止写入。启动只恢复active指定且CRC/世代绑定正确的snapshot，不用猜测回退掩盖已承诺数据损坏，不自动格式化NVS。

`gw_store_transaction`支持最多64个操作、255字节逻辑key、256个对象；全snapshot序列化上限96KiB，Base64和元数据占用计算在内。调用者必须做容量准入，不承诺所有上限可同时取满。该存储用于低频配置和任务，BLE原始流使用独立gwspool组件，不把每个扫描包写入整份NVS快照。

Wi-Fi密码实体存于同一加密分区的独立secrets命名空间，按需读取并清理临时明文，不在通用KV的常驻RAM快照中缓存。先写并验证秘密实体，再发布元数据；断电可留下不可引用的孤儿，重试同一不可变引用会补完发布。索引未发布时网络不能使用该凭据。最多16个凭据，目前没有自动删除入口；达到配额明确拒绝，不回收仍用于当前/回退的秘密。

本地请求按规范化JSON的HMAC防重，先持久意图，配置事务同时记录create/actionRequestId。掉电后从真实事务或已发布凭据恢复结果。同一generation最多32个请求记录，满时拒绝；较旧世代记录可回收，迟到请求由版本CAS拒绝产生第二次副作用。保留最近8个终态网络事务用于查询，云端任务的长期防重由上层task journal负责。

## 制造与板配置

默认运行时要求Flash加密已经由制造流程配置，并存在已生成的nvs_keys。固件只读取密钥配置，不自动生成可公开的生产密钥或烧eFuse。gwfactory和gwstore使用NVS加密；缺失/损坏的密钥、身份或分区需按诊断处理，禁止用自动擦除“修复”。

项目明确排除SDK的`nvs_sec_provider`自动注册组件。该组件负责默认`nvs_flash_init()`的自动密钥生成，而本项目只调用带制造密钥的`nvs_flash_secure_init_partition()`。保留`CONFIG_NVS_ENCRYPTION=y`和SDK实际XTS加密实现，不为HMAC指定默认eFuse块，也不开启bootloader自动加密。不得新增默认`nvs_flash_init()`调用来绕过本项目的制造密钥检查。

gwfactory的NVS namespace为`factory`。使用锁定SDK的NVS分区生成/加密工具，把以下条目放入加密制造镜像。秘密、生成的CSV、key和镜像放在源码仓库之外，交付给有权限的制造环境；这些不是运行时默认凭据。

| NVS键 | 类型 / 用途 |
| --- | --- |
| setup_user | string，本地Security2账号，1～64 UTF-8字节 |
| sec2_salt / sec2_verifier | blob，16/384字节；[Generate-Setup.py](../../scripts/Generate-Setup.py)生成并保留前导零 |
| ap_ssid / ap_password | string，设备独立热点标识和至少16字符随机密码，不与setup密码共用 |
| api_url / ingest_url | string，管理HTTPS根地址及可选上报地址；按cloud组件契约 |
| namespace / gateway / token | string，平台设备身份；token是秘密 |
| root_ca | PEM信任链，类型与HTTP组件读取方式一致；不得关闭主机名/证书检查 |
| ntp_server | string，配置的SNTP服务器；缺失时没有自动默认时间源 |
| model / hardware | 型号和硬件版本字符串，用于OTA兼容性检查 |
| artifact_keys | JSON对象，keyId映射Base64 Ed25519公钥；仅制造流程安装，按OTA组件契约 |

当前timeTrusted表示从配置SNTP源得到合理UTC，并非密码学认证时间。TLS仍检查证书和主机名；需要认证授时的部署必须补充可信时间引导后再验收，不能将此标志当作硬件安全证明。

CH390默认关闭，GPIO默认-1；实际PCB核对后配置MOSI/MISO/SCLK/CS/INT/RESET并启用。没有可用Ethernet netif时明确拒绝含启用Ethernet的候选，不虚构网口已工作。首次现场窗口默认不开；可配置经验证的低有效实体输入，按住3秒开600秒窗口。制造授权的一次性首次窗口由独立Kconfig开启，使用持久标志避免每次断电重新授权。

## 验证记录与限制

`scripts/Test-Core.ps1`使用实际生产gw_rules.c，主机测试覆盖无损uint64、UTF-8/IPv4边界以及snapshot/active指针逐字节截断、位损坏和世代绑定。`Generate-Setup.py --self-test`对照10组独立Security2服务端公开值，包括前导零回归。它们不使用真实NVS，也不证明目标编译/掉电成立。

`scripts/Test-Core-State.ps1`直接编译生产gw_core.c/gw_validate.c/gw_rules.c，以边界替身验证LOCAL_ONLY重启回退、首次离线保存后的迟到证明、原提交快照绑定、回调释放互斥锁、验证响应抵达最终期限时的回退，以及远程成功结果ACK的重启恢复/幂等性。该测试的摘要替身只检查身份绑定逻辑，不作为密码学验证；真实Flash掉电和网络时序仍需实板验证。

已获取官方ESP-IDF v5.5.6源代码及全部锁定子模块，提交为`880a203ae603d08a94d06aa01e0d6761958e5e9e`；目标工具链安装与目标编译记录见工程VALIDATION。CI使用官方v5.5.6容器及固定镜像摘要。配置状态机、网络驱动及制造流程还需要ESP-AT-17/18、37～44的实板验证；源码审查和主机测试不能替代目标构建或实板证据。

Windows Xtensa汇编器不能可靠处理非ASCII对象路径。`Build.ps1`遇到中文源码路径时，在ASCII的`AsciiBuildRoot`下建立独立、只用于构建的源码副本，并保留输出目录；SDK和工具链同样须使用ASCII路径。不通过junction绕过此限制，因为idf.py会解析项目junction回真实路径。

已授权的平台诊断可调用`gw_core_wifi_scan`，本地网页使用相同扫描实现。真实主动/被动扫描最多32条、最长15秒，受更早请求截止时间限制，扫描期间持有网络互斥租约。扫描超时会停止当前扫描；为防迟到SCAN_DONE事件误配到下个请求，该驱动实例禁止后续扫描直至重启，其余网络连接继续工作。
