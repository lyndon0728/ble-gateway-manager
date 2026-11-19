# ESP32-S3 0.3.0 验证记录

日期：2026-11-19。该文件记录实际验证，完整产品放行条件仍为[50项验收计划](../../docs/esp32s3-validation-plan.md)。

## 0.3.0 三方对齐验证

本次源码落实C02～C09/C11及BFLT-2.0，版本和制造前置条件见[实施记录](../../docs/gateway-alignment-implementation.md)。以下是包含最后Peripheral内存故障处理、平台旧boot关联和ESP加密bond初始化修复的最终本地验证；后续0.2.x章节只保留历史证据。

| 对象 | 实际结果与边界 |
| --- | --- |
| ESP完整生产主机入口 | `scripts/test_host.py`全部通过；Zig 0.14.1、锁定ESP-IDF v5.5.6 cJSON/MbedTLS源码，C11 `-Wall -Wextra -Werror` |
| 原功能回归 | 网络候选/本地配网、云任务、GATT、广播、采集、HTTP、双OTA；6项打包、145项独立Ed25519、69个spool断电注入点和7份生产批次Schema检查通过 |
| V2过滤与无线状态 | ESP JSON编译/共享引擎、完整原子配置、掩码/PAIR/长度拒绝、实际广播状态证据通过；nRF七套生产模块主机测试通过，详见[nRF记录](../nrf54l15/VALIDATION.md) |
| MQTT生产适配器 | 订阅/迟到SUBACK、可选业务通道、主题与身份、分片/深度/NUL、互斥后期限、队列OOM、PUBACK与业务ACK区分、绑定接口HTTPS回退通过 |
| Peripheral生产业务 | 独立会话/PG重组、持久STORED、重连同businessId去重、spool/INBOX满、平台逐项ACK及隔离、手机APPLIED和实际CLOSED完成证据通过；SDK密码与设备I/O在本套中为替身 |
| Peripheral逐分配点故障 | 861次cJSON分配失败注入通过：69业务构建、187接收/提交/STORED、248 NOTIFY、195 DISCONNECT、49上传、62 APPLIED、22结果证据、29 UART序号预留；无完整持久数据不回STORED，无完整持久意图不发送动作 |
| 授权/时间/同步 | 独立Ed25519制造与时间签名向量、原始字节/身份/nonce/boot绑定、UTC保守区间/漂移/期限、快照完整组/规范摘要/世代/历史来源、组件当前镜像/boot变化/回执绑定通过 |
| ESP bond持久化 | 默认nvs显式使用已有制造密钥初始化；原始记录及SDK恢复前后核验测试通过，含打开/读取失败、尺寸/密钥非法、恢复缺失/不一致；没有擦除或生成eFuse |
| 本地网页与凭据生成 | 11项Node Security2/OTA协议测试、Generate-Setup的10个独立向量通过；新增peripheral用途与setup凭据隔离 |
| 平台 | 最终`go vet ./...`、`go test ./... -count=1`全部通过；95个Go文件格式检查通过，platform包58.467秒。旧boot迟到数据持久接管但不能新建可回发关联，NOTIFY/DISCONNECT再次核当前boot |
| 管理前端 | Vue类型检查、Vite构建通过，嵌入资源同步；10项Node测试和21项网关输入检查全部通过。PAIR超时提示与实际拒绝行为一致 |
| Windows ESP目标 | 全组件编译/完整链接/镜像生成/分区容量检查通过；228个源码、头、测试、资源及构建输入与独立ASCII副本逐字节一致 |

目标使用ESP-IDF v5.5.6（`880a203ae603d08a94d06aa01e0d6761958e5e9e`）、Xtensa `esp-14.2.0_20260121`、CH390 0.4.1和`sdkconfig.local-setup-test`。确认实际配置 `CONFIG_GW_FACTORY_AUTHORIZED_SETUP=y`，完整本地网页/OTA入口被链接。量产默认授权开窗/板级复位配置保持独立门控，不能把该测试配置作为量产授权。

| 最终Windows 0.3.0产物 | 实测值 |
| --- | --- |
| 应用版本 | `0.3.0`，读取实际bin应用描述 |
| 应用bin | 1,513,712字节，`0x1718F0` |
| 最小OTA槽 | `0x280000`，剩余`0x10E710`，约42% |
| 应用bin SHA-256 | `5a0ff70aece46eb040ca438c689aa359d9bf7c209dd737598e042553e8696ee6` |
| 应用ELF SHA-256 | `5255b9a4e6e052e0334097b8829d6964d31e5fa51917ce0a6388bdb0d4176658` |
| Bootloader | 20,544字节；SHA-256 `ad17e18dd333686b20f2da8929202665dcd3929b587c7a5a812a6c2abb84fd2a` |

实际ELF包含有效地址：`gw_actions_local_ota=0x4201d66c`、`gw_provision_open_window=0x4201023c`、`gw_peripheral_execute=0x420177f0`、`gw_transport_json=0x42058dd4`、`gw_signed_time_verify=0x42012d5c`、`gw_ble_bonds_check=0x420206dc`及`_binary_app_js_start=0x3c1316e8`。GitHub目标工作流同步增加新模块符号检查。

上述镜像摘要属于本次Windows构建；不同平台/构建时间产生的CI镜像须以其独立产物为准。SDK、临时测试密钥、本地日志和未签名构建产物不上传源码仓库。未连接/烧录实板、未执行eFuse；NCS目标构建、双射频空口、手机实机、真实MQTT/TLS切换、100ppm时钟漂移、Flash掉电、复位脉冲及整机双OTA回退仍待验收。

### 0.3.0 GitHub验证证据

- [固件运行44546976480](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44546976480)，提交`7efe497da20a40d7c483e4a2a13ba5f24d9bfc23`：Linux GCC主机全套与Node网页协议测试、ESP32-S3完整目标编译、镜像容量和实际ELF符号检查全部成功，产物按工作流保留14天。
- [平台运行44545398972](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44545398972)，提交`9be7b62c47513d73c4aa01523e781d336b8ccea7`：Windows/Linux检查、Go全套测试、Linux race检查、协议进程联调、浏览器/真实API集成和容器构建全部成功。

Windows镜像及228项输入核对对应`9be7b62`。后续`7efe497`仅整理MQTT测试中两处循环排版，修复GCC的misleading-indentation警告，没有改变生产代码。首次固件运行44545399087的目标job成功、host job因该测试编译警告失败；不能将首次运行标成全通过。上述最终成功运行之后仅补充Markdown证据，不改变已验证源码。

## 0.2.1 需求落实与回归

本次按既有需求修复升级授权边界、启动槽写入结果不确定的恢复、签名大小约束、配网窗口关闭及配置报告版本倒退。逐项对应见[变更记录](CHANGELOG.md)。

[GitHub运行44475822740](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44475822740)，源码提交`9533a14`：Linux主机测试及完整本地入口目标构建均成功，包括实际ELF函数/资源检查和11项网页协议测试。编译产物和日志保留14天；随后仅补充此Markdown验证链接，不改变已验证源码。

| 检查 | 实际结果 |
| --- | --- |
| 完整生产主机套件 | `scripts/test_host.py` 全部通过，包含生产核心/云任务/动作/采集/HTTP、6项打包测试、145项独立Ed25519用例、69个spool断电注入点及7份生产JSON Schema检查 |
| OTA新增故障注入 | 本地/远程journal延迟跨期限时不开始擦写；擦除/中间块耗时超期与撤销被拒绝；UTC回拨不延长原单调预算；显式签名尺寸/格式、超槽和最终长度不符被拒绝；旧制品字段省略仍可用 |
| 本地提交结果不确定 | 模拟启动槽已写成功但返回错误、尚未生效且返回错误两种情况，均保留REBOOT_PENDING及维护租约；重启后按实际槽分别确认或回退，不提前宣告未安装 |
| 本地HTTP | 生产`gw_provision_io`在慢速分片、迟到末块、关闭窗口、EOF/读取失败及socket跟踪/重用场景通过；不是实际无线socket测试 |
| 云端版本 | rev2确认后迟到rev1/DUPLICATE、旧完成路径重入及重启恢复均不退回水位；错误ACK、失败结果、无效数值不能推进；安全整数上界通过 |
| Windows完整目标 | ESP-IDF v5.5.6、完整本地入口配置，编译/链接/镜像生成/分区容量检查通过；152个源码、头、资源及构建配置与独立ASCII构建副本逐字节一致 |

Windows应用版本0.2.1，bin为1,412,016字节（`0x158BB0`），最小OTA槽`0x280000`，剩余`0x127450`（约46%）。SHA-256：`f565a40269302304e23200bd6e578a1f2b2069d0df01890935793017cfe7e8c6`。实际ELF包含本地OTA/开窗、`gw_prov_receive`及网页资源有效地址；未被链接器移除。

本次未改网页JavaScript、服务器端或nRF固件，相关旧行为由同一主机入口回归。未连接/烧录实板、未写eFuse；热点实际关闭时延、Flash报错/断电、真实试启动回退仍需实板验收。以下0.2.0和0.1.0数据保留为历史证据，不代表本次bin。

## 0.2.0 本地及远程 OTA 验证

本次新增现场离线网页上传、签名发行包和独立本地升级记录，复用原远程 OTA 的设备信任公钥及应用分区。平台心跳改为读取真实运行镜像版本，避免升级后仍上报固定旧版本。

| 对象 | 实际执行结果与边界 |
| --- | --- |
| 生产固件主机套件 | 完整 `scripts/test_host.py` 通过：配置/云任务/GATT/采集/HTTP及原远程 OTA 回归通过，继续包含145项独立Ed25519验证及69个spool断电注入点 |
| 本地 OTA 状态机 | 生产C代码通过签名/兼容性/大小、互斥、偏移/最后块重放、签名或内容篡改、写入失败、窗口到期/关闭、丢BEGIN/WRITE/COMMIT响应、接收/READY重启中止、离线试启动确认及回退恢复；Flash/时钟/重启使用替身 |
| 云端版本报告 | 两个不同运行镜像版本的心跳均取应用描述，不再使用固定常量；服务器状态入口会更新注册版本，供下一次源版本兼容判断 |
| 发行打包 | 6项Python测试通过：实际签名原文字节、摘要/长度、芯片与应用描述、容量、错误密钥类型、加密PEM、拒绝覆盖；仅使用临时测试密钥 |
| 浏览器协议 | 11项Node测试通过：原8项Security2及新增3项OTA文件、偏移和块编码测试 |
| 最终内置网页 | 独立加密模拟设备配合Chrome通过：原配网流程、错误签名拒绝、丢响应后重新认证续传、上传中取消、确认安装/查询、查询失败撤销旧READY按钮及CONFIRMING显示；手机/桌面视口不代表手机实机 |
| 平台升级兼容性 | `go test ./internal/platform -run 'Artifact\|Firmware' -count=1` 通过，覆盖原始清单签名及组件/兼容性/升级任务；没有连接真实网关 |

完整协议和复现步骤见[OTA设计](../../docs/esp32s3-ota.md)。未使用生产私钥，未烧录设备，未执行eFuse操作。

### 完整本地功能目标验证

[GitHub运行44470481423](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44470481423)，源码提交`51b1230`：Linux主机测试与ESP32-S3目标构建两个job全部成功。目标job执行镜像容量检查，并检查实际ELF中的本地OTA函数、开窗函数及网页资源符号；产物含bin/elf/map、实际sdkconfig、符号列表及日志，保留14天。该证据属于0.2.0历史版本；0.2.1的源码与验证记录见上节。

默认板卡安全配置没有设置按钮/首次开窗授权，两者关闭时链接器会移除本地接口，不能据此证明网页功能已链接。本次Linux CI及Windows完整验证均显式使用`sdkconfig.local-setup-test`，确认`CONFIG_GW_FACTORY_AUTHORIZED_SETUP=y`；该配置只提供首次未配网时一次600秒窗口，量产仍须核实设置按键GPIO或实现并验证其他现场授权入口。

完整入口构建首次暴露并修复了SDK资源符号名称不匹配：ESP-IDF v5.5.6实际生成`_binary_app_js_start`等basename符号，原代码错误引用带`assets_`前缀的名称。修复后Windows和Linux完整链接都通过；不能将修复前失败运行44469256683作为通过证据。

Windows使用独立ASCII源码副本、独立SDKCONFIG完成全部目标编译及链接，并核对149个源文件/头文件/资源/构建配置与仓库逐字节相同。实际链接地址：`gw_actions_local_ota=0x420178ac`、`gw_provision_open_window=0x4200fa08`、`_binary_app_js_start=0x3c11e23c`，均不是被丢弃的零地址符号。

| 0.2.0 Windows完整本地功能镜像 | 实测值 |
| --- | --- |
| 应用描述版本 | 0.2.0 |
| 应用bin | 1,410,064字节，`0x158410` |
| 最小OTA槽 | `0x280000`，剩余`0x127BF0`，约46% |
| 应用bin SHA-256 | `ec2e7339476001791d719735e2750d939c34e307faa2cedb89a0517d255bc0b8` |
| Bootloader | 20,544字节，`0x5040` |
| 实际镜像打包验证 | 对上述bin使用临时测试Ed25519密钥生成并独立验签，版本/长度/摘要通过；临时材料已销毁，未生成生产签名包 |

上述摘要仅属于本次Windows完整本地功能构建，不能替代Linux CI或未来量产配置的镜像摘要。代码和目标构建已验证，现场按键、真实AP上传、手机兼容、Flash断电和90秒试启动回退仍需按验收计划执行。

## 0.1.0 基线记录

以下保留本次功能扩展之前的构建与系统测试证据；其中历史bin摘要、资源数及CI链接仅属于相应旧构建，不能代表0.2.0的新镜像。

## 已完成的验证

| 对象 | 方法 | 当前结果与边界 |
| --- | --- | --- |
| 平台网络扩展 | Go单元/HTTP/权限/迁移测试及既有全套测试 | 通过；设备探测由模拟请求提供，不是物理网口证明 |
| 平台页面 | Vue类型检查、Vite生产构建、API输入测试、实际服务端Chrome烟测 | 通过；26页、4种角色、75项响应式检查及断网恢复，无浏览器异常；产物随服务端嵌入资源提交 |
| Security2浏览器 | 官方Python/OpenSSL向量、设备公式前导零边界、篡改/重放/nonce/protobuf测试 | 8组测试通过，详见配网组件README |
| 浏览器流程 | 最终离线bundle、独立Node模拟设备、Chrome手机/桌面视口 | 通过；不是Android/iOS实机或Safari验证 |
| 配网C输入守卫 | 实际生产C函数的UTF-8、长度、深度和NUL边界 | 主机测试通过 |
| 核心规则与记录 | `Test-Core.ps1`、Zig 0.14.1 | 无损整数、UTF-8、IPv4、快照/指针截断、损坏及世代绑定通过 |
| 本地/远程网络状态机 | `Test-Core-State.ps1`，生产core/validate/rules与网络/存储替身 | LOCAL_ONLY重启不延长试用、迟到时间证据、原事务快照绑定、probe回调不持锁、最终平台ACK重启及幂等通过 |
| 云任务状态机 | `Test-Cloud.ps1`、Zig 0.14.1、cJSON 1.7.19 | 持久受理、ACK丢失后推进、命令重放/摘要冲突、写入失败、到期、模糊执行UNKNOWN、回退probe隔离通过 |
| nRF GATT执行器 | 生产`gw_gatt_nrf.c`配合UART/Flash/时钟替身 | 意图先持久、op_seq、防模糊重写、UUID/MAC校验、READBACK同连接、临时断开、旧观测拒绝及结果原子发布通过 |
| 采集与Flash spool | 生产持久层/适配器、真实cJSON/MbedTLS、分区与HTTP替身 | 69个断电注入点、重放/水位、满盘/损坏、双源配对、逐项ACK/隔离、inbox与稳定事件通过；动态upload批量/刷新、200条拆包及7份生产JSON通过平台Schema |
| OTA签名验证 | 未修改Monocypher 4.0.3生产代码；独立Python/OpenSSL与RFC8032 | 145项互通、改字节、错误签名/公钥、非规范签名拒绝通过；不是实板bootloader验签 |
| 动作与OTA状态机 | 生产actions/OTA模块，设备I/O和时钟替身 | 意图写入屏障、配置原子成功/只读恢复、冲突/UNKNOWN、ESP试启动与到期回退、nRF分块与确认应答丢失恢复通过 |
| HTTPS期限与响应解析 | 生产TLS适配器及HTTP解析器，非阻塞TLS/时钟替身 | 连续chunk流、连接/写/读WANT期限、超长/冲突响应、sink失败与连接释放通过；真实TLS证书、DNS绑定仍需实网验证 |

跨模块入口：`python scripts/test_host.py --cc <C11编译器或zig> --idf <ESP-IDF v5.5.6源码>`，从固件目录执行，Python需cryptography和jsonschema。GitHub工作流另执行Linux GCC主机测试及Node Security2向量。主机测试直接编译相应生产逻辑，替换设备I/O；不能替代目标运行。

## 目标构建与CI证据

实际执行了ESP32-S3目标编译、完整链接、镜像生成和分区容量检查。SDK为ESP-IDF v5.5.6，源码提交`880a203ae603d08a94d06aa01e0d6761958e5e9e`，Xtensa工具链为`esp-14.2.0_20260121`；CH390 0.4.1依赖锁已提交。

| 证据 | 结果 |
| --- | --- |
| [Linux固件CI，868a932](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44463523783) | 目标构建和主机job全部通过；生成bin/elf/map/sdkconfig/分区表及日志，包含生产核心状态、云任务、动作/OTA、GATT、捕获、HTTP和Security2测试 |
| [平台Server CI，5300b79](https://github.com/lyndon0728/ble-gateway-manager/actions/runs/44462214515) | Ubuntu、Windows和容器job全部通过，包含Go全套测试、构建、协议模拟联调及浏览器实API测试 |

目标CI使用官方`espressif/idf:v5.5.6`镜像，固定manifest摘要`sha256:bf72fe7a15c22849a9a94147301165a545fb8905d97c176cf748e3e4614cac84`。完整日志和编译产物在对应GitHub运行的Artifacts中保留14天；源码、依赖锁、分区和构建脚本长期保存在仓库。持续运行状态可查[固件工作流](https://github.com/lyndon0728/ble-gateway-manager/actions/workflows/esp32s3-firmware.yml)。

Windows本地也完成实际目标构建。因Xtensa工具的非ASCII对象路径限制，构建使用独立ASCII源码副本，逐文件核对源文件后编译；`Build.ps1`已包含该流程。NVS使用显式制造密钥与加密分区初始化，工程排除默认自动密钥生成provider，未通过设置任意eFuse块或关闭加密绕过构建错误。

最后一次本地构建还包含“最后一块sink写入结束后再检查截止时间”的修复，相关主机测试和完整目标链接均通过。核对144个源文件/头/资源/构建配置与仓库逐字节相同后，结果如下；此摘要专属于该次Windows产物，不能拿它替代其他构建的摘要。

| 本地最终产物指标 | 实测结果 |
| --- | --- |
| 应用镜像 | 1,283,728字节，`0x139690` |
| 最小OTA应用分区 | `0x280000`，剩余`0x146970`，约51% |
| 静态D/IRAM | 180,447 / 341,760字节，52.8%；不是运行期堆峰值 |
| Bootloader镜像 | 20,544字节 |
| 应用bin SHA-256 | `1e1e0dd79c8788e3cfbf130979879231daee2fc50f20c10962bfbce5ad7aa137` |

验证日志未包含烧录操作。上述镜像不携带设备生产凭据，也未据此宣告可直接替代制造签名/安全配置流程。

## 实板边界

本次没有连接/烧录GW32实板，没有执行eFuse或Flash加密制造，没有测量UART CTS、BLE空口、CH390、SPI/引脚、Flash掉电和寿命。未运行真实双组件签名升级、MCUboot试启动/回滚或72小时压力测试；也未运行真实手机系统兼容矩阵。

验收计划中的H/K/B/S分层须分别记录，当前主机证据不能把ESP-AT-01～50整体标为通过。生产密钥/身份/网络地址以及任何本地未跟踪研究文件不进入此次交付。
