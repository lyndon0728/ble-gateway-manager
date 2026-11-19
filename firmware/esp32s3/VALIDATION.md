# ESP32-S3 0.1.0 验证记录

日期：2026-11-19。该文件记录实际验证，完整产品放行条件仍为[44项验收计划](../../docs/esp32s3-validation-plan.md)。

## 已完成的验证

| 对象 | 方法 | 当前结果与边界 |
| --- | --- | --- |
| 平台网络扩展 | Go单元/HTTP/权限/迁移测试及既有全套测试 | 通过；设备探测由模拟请求提供，不是物理网口证明 |
| 平台页面 | Vue类型检查、Vite生产构建、API输入测试、实际服务端Chrome烟测 | 通过；26页、4种角色、75项响应式检查及断网恢复，无浏览器异常；产物随服务端嵌入资源提交 |
| Security2浏览器 | 官方Python/OpenSSL向量、设备公式前导零边界、篡改/重放/nonce/protobuf测试 | 8组测试通过，详见配网组件README |
| 浏览器流程 | 最终离线bundle、独立Node模拟设备、Chrome手机/桌面视口 | 通过；不是Android/iOS实机或Safari验证 |
| 配网C输入守卫 | 实际生产C函数的UTF-8、长度、深度和NUL边界 | 主机测试通过 |
| 核心规则与记录 | `Test-Core.ps1`、Zig 0.14.1 | 无损整数、UTF-8、IPv4、快照/指针截断、损坏及世代绑定通过 |
| 云任务状态机 | `Test-Cloud.ps1`、Zig 0.14.1、cJSON 1.7.19 | 持久受理、ACK丢失后推进、命令重放/摘要冲突、写入失败、到期、模糊执行UNKNOWN、回退probe隔离通过 |
| nRF GATT执行器 | 生产`gw_gatt_nrf.c`配合UART/Flash/时钟替身 | 意图先持久、op_seq、防模糊重写、UUID/MAC校验、READBACK同连接、临时断开、旧观测拒绝及结果原子发布通过 |
| 采集与Flash spool | 生产持久层/适配器、真实cJSON/MbedTLS、分区与HTTP替身 | 69个断电注入点、重放/水位、满盘/损坏、双源配对、逐项ACK/隔离、inbox与稳定事件通过；动态upload批量/刷新、200条拆包及7份生产JSON通过平台Schema |
| OTA签名验证 | 未修改Monocypher 4.0.3生产代码；独立Python/OpenSSL与RFC8032 | 145项互通、改字节、错误签名/公钥、非规范签名拒绝通过；不是实板bootloader验签 |
| 动作与OTA状态机 | 生产actions/OTA模块，设备I/O和时钟替身 | 意图写入屏障、配置原子成功/只读恢复、冲突/UNKNOWN、ESP试启动与到期回退、nRF分块与确认应答丢失恢复通过 |

跨模块入口：`python scripts/test_host.py --cc <C11编译器或zig> --idf <ESP-IDF v5.5.6源码>`，从固件目录执行，Python需cryptography和jsonschema。GitHub工作流另执行Linux GCC主机测试及Node Security2向量。主机测试直接编译相应生产逻辑，替换设备I/O；不能替代目标运行。

## 目标构建与待完成记录

ESP-IDF已选择v5.5.6，SDK实际源码提交为`880a203ae603d08a94d06aa01e0d6761958e5e9e`。首次获取SDK遇到Git默认TLS连接空响应；切换Git OpenSSL/HTTP1.1或原生Python的正常证书验证后可下载。没有通过跳过证书验证解决网络问题。

目标编译、捕获/动作最终测试及本次GitHub CI结果将在实际完成后更新本节。在结果更新前，不能仅凭CI配置或源码存在宣布目标构建通过。

## 实板边界

本次没有连接/烧录GW32实板，没有执行eFuse或Flash加密制造，没有测量UART CTS、BLE空口、CH390、SPI/引脚、Flash掉电和寿命。未运行真实双组件签名升级、MCUboot试启动/回滚或72小时压力测试；也未运行真实手机系统兼容矩阵。

验收计划中的H/K/B/S分层须分别记录，当前主机证据不能把ESP-AT-01～44整体标为通过。生产密钥/身份/网络地址以及任何本地未跟踪研究文件不进入此次交付。
