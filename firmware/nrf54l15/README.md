# GW32 nRF54L15 嵌入式软件

这是 GW32 网关的 nRF54L15 蓝牙协处理器应用，当前版本 **0.2.0**，基于 **nRF Connect SDK v3.2.1 / Zephyr**。固件实现 ESP32-S3 ↔ nRF 的二进制 UART 协议；ESP 的 Wi-Fi、跨芯片去重、服务器连接和服务器规则引擎仍由 ESP/服务器负责。

SDK 版本是本次新工程选定的基线，原工程没有已有 SDK 锁定文件。本文提供构建入口；主机测试与真实 SDK 构建、实板验证分别记录，不能互相替代。当前开发机未安装完整 NCS 工作区及 ARM toolchain，因此尚未完成 NCS 目标编译，也未烧录或做实板射频验证；板级定义和关键 API 已对照固定版本的官方源码检查。

## 实现范围

- COBS + CRC32C 流式解码、长度和会话校验、HELLO 能力协商、控制请求幂等重放。
- Legacy 主动/被动扫描、RSSI/服务 UUID/厂商 ID/Beacon 掩码过滤；原始广播内容和元数据上报。
- 数据 RAM 队列、批次重传、ESP `CUSTODY/BUSY/FAULT` ACK；跨无线电去重由 ESP 执行。
- 服务器经 ESP 下发连接目标；nRF 执行单个 Central 连接、GATT 发现、按 UUID 查服务、读写、订阅和通知上报。
- Legacy 非连接、可扫描非连接、可连接三种对外广播；服务器可配置启停、内容和间隔。只更新内容时在线写入，失败恢复旧内容；模式或间隔改变时重启广播。
- 固定网关 Peripheral INFO/RX/TX 服务，手机可读状态、写入不透明数据及订阅 ESP 的回复。连接释放后按目标配置恢复广播。
- 可选 MCUboot 双槽 UART DFU：ESP 下载签名固件后分块交给 nRF，支持进度查询、重传、读回 SHA-256、试启动、运行镜像校验与确认、未确认复位回退。
- RTT 日志、UART 驱动恢复、状态和丢弃计数。蓝牙 bond 使用 Zephyr settings/NVS；扫描目标配置由 ESP 重启后重新对账下发。

当前不声明完整扩展广播接收或并行扫描/广播/连接组合。Central 与 Peripheral 共用一个连接资源；启动冲突角色返回 BUSY。HELLO bit10 为广播管理、bit11 为 Peripheral，bit12 仅在实际具备可用 MCUboot 双槽 DFU 时声明。首次配对不在当前协议中；Peripheral 的公开 INFO 和不透明 RX/TX 不自动执行配置命令，ESP 须在解释数据前完成应用身份认证和授权。含网络凭据的业务不能假设该公开 BLE 通道已加密。

Zephyr 的扫描回调可能将已绑定设备的空中 RPA 替换为 identity address，且不会保留原始地址类型。为避免把 identity 冒充空中 MAC，本版本对匹配已保存 bond identity 的报告发送 `addr_type=0xFF`、全零地址，原始广播数据仍可上传。服务器不能按此地址自动连接；真实 identity 广播也会被保守地标为未知。关闭 `CONFIG_BT_PRIVACY` 无法关闭这条 Host IRK 解析路径。要让已绑定设备的 RPA 也参与原始地址去重/自动连接，需要后续接入原始 HCI 报告或扩展显式 identity 字段。

三方共同基准为 [GW32-E2E-1.1 端到端统一标准](../../docs/gateway-end-to-end-standard.md)。协议来源：[UART 协议 1.1](../../server/docs/gateway-uart-protocol.md)、[ESP ↔ 服务器协议](../../server/docs/gateway-protocol.md)；UART 线上帧版本仍为 1，平台 Schema 保持 1.0/1.1。详细要求见 [nRF 专项需求](../../docs/nrf54l15-requirements.md) 与 [nRF 开发文档](../../docs/nrf54l15-development.md)，完整范围见 [需求与验收清单](../../docs/gateway-requirements.md) 和 [系统对接指南](../../docs/gateway-integration.md)。UUID 16/32/128 位的服务器规则解析与标准化由服务器完成，nRF 接收协议规定的标准形式及连接参数。

## 文件结构

| 文件 | 作用 |
| --- | --- |
| `src/gw_core.c` | 与 RTOS 无关的帧、过滤、队列与批次逻辑 |
| `src/gw_app.c` | UART EasyDMA、会话、命令分发、ACK 与主循环 |
| `src/gw_radio.c` | 扫描、过滤上报、广播与资源状态 |
| `src/gw_gatt.c` | 异步 Central/GATT 生命周期和通知 |
| `src/gw_peripheral.c` | 手机连接、固定服务、不透明数据接收及回复 |
| `src/gw_dfu.c` | 经 UART 接收签名镜像、双槽试启动和确认 |
| `tools/adv_payload.py`、`tools/uart_probe.py` | 广播内容生成、联调命令和 ESP 升级传输参考 |
| `boards/gw32/gw32_nrf54l15` | Oct29 GW32 独立板级定义 |
| `tests` | 可执行的主机协议测试、模拟 Zephyr 回调的 GATT 测试 |

## UART 与板级基线

**1,000,000 baud、8N1、硬件 RTS/CTS、3.3 V、UARTE20**。UART 不承担控制台输出。持续 RX 期间通过 Zephyr 的时钟 on/off manager 持有 HFXO 请求，与 MPSL 共用时钟控制。

| nRF 引脚 | 方向/用途 | 对端或器件 |
| --- | --- | --- |
| P1.04 | TX | ESP GPIO18 / RX |
| P1.05 | RX | ESP GPIO17 / TX |
| P1.06 | RTS | ESP GPIO16 / CTS |
| P1.07 | CTS | ESP GPIO15 / RTS |
| P1.09 | FEM TXEN，高有效 | AT2401C pin 5 |
| P1.10 | FEM RXEN，高有效 | AT2401C pin 6 |

自定义板基于 `D:/chat/GW32_AT2401C_Optimized_20261029/evidence/final_v6.net`。U9 为 nRF54L15-QFAA-R；X1 为 32 MHz，C54/C55 为外部 12 pF，故 HFXO 禁用内部负载电容。HFXO 启动预算暂设 2000 us，须按实际 XOSTART→XOTUNED 的测量结果确认。X2 为 32.768 kHz/9 pF；本版本使用经过校准的 LFRC、500 ppm，避免在未测板级寄生前猜测内部电容值。供电采用 LDO 初始模式，尚未启用 DCDC 优化。

AT2401C 通过 MPSL generic two-pin FEM 自动切换，禁止应用层直接拉高 TXEN/RXEN。本地 datasheet 列出 800 ns **典型**切换/关断开启时间、TX 增益 22 dB、RX 增益 14 dB。DTS 的 10 us settle 值是上板测量起点，不是已经验证的最坏值。射频链路输出包含外部 PA 增益，不能把 nRF 配置功率当作天线口实测功率。

`gw32_nrf54l15/nrf54l15/cpuapp` 默认 `CONFIG_GW_RADIO_VERIFIED=n`：UART 和状态管理可运行，BLE 不启动。使用自定义板射频前，完成引脚/版本核对、时钟启动、FEM TX/RX 互斥及时序、供电电流、天线口输出功率检查，然后使用 commissioning 配置启用。新版原理图不能未经网表对比直接沿用此板定义。

## 构建

安装官方 NCS **v3.2.1** 及匹配 toolchain，在其 toolchain terminal 中运行。SDK workspace 应包含 `nrf`、`zephyr`、`nrfxlib` 等由官方 `west.yml` 解析的仓库。不要只克隆 Zephyr，也不要使用不匹配的系统 ARM 编译器替代官方工具链。

```powershell
# 官方 nRF54L15 DK：可立即用于软件开发，射频开启。
.\scripts\Build.ps1 -NcsRoot C:\ncs\v3.2.1 -Target dk

# GW32 Oct29 自定义板：先联调 UART，默认不启动射频。
.\scripts\Build.ps1 -NcsRoot C:\ncs\v3.2.1 -Target gw32

# 完成上述自定义板测量后启用真实 BLE 功能。
.\scripts\Build.ps1 -NcsRoot C:\ncs\v3.2.1 -Target gw32 -CommissionedRadio
```

DK overlay 同样使用 P1.04/05/06/07。串口适配器或 ESP 必须接好 TX/RX、交叉 RTS/CTS 和公共 GND；确认 DK 板上调试器/VCOM 路由不会占用同一信号。构建脚本仅生成镜像，不烧录、不擦除芯片。

等价的开发命令（在本目录、官方 SDK 环境下）：

```text
west build -p always -b nrf54l15dk/nrf54l15/cpuapp -d build-dk .
west build -p always -b gw32_nrf54l15/nrf54l15/cpuapp -d build-gw32 .
```

## 服务器控制对外广播

服务器沿用 `CONFIGURE` 的 `configuration.radios.nrf54.advertising`，字段为 `enabled`、`mode`、`intervalMs`、`advDataBase64`、`scanResponseBase64`。三种模式分别是 `NONCONNECTABLE`、`SCANNABLE_NONCONNECTABLE`、`CONNECTABLE_SCANNABLE`。服务器校验设备清单能力，ESP 校验 UART 实际能力后下发；配置成功以配置读回和实际状态为依据。完整任务示例见 [服务器协议](../../server/docs/gateway-protocol.md)。

UART `0x07/0x08` 保持既有 AdvConfig 布局，`mode=2` 表示可连接广播；`0x0B` 只切换启停并提交新 revision，`0x0C` 返回实际状态、最近驱动错误、应用时间、在线更新次数和成功启动次数。间隔是 **100–10240 ms**，250 ms 约为 4 次/秒；实际空中发射含 BLE 调度及随机延迟。AdvData 和 ScanRspData 各最多 31 字节。iBeacon 的 measured power 是 1 米校准 RSSI，不是 AT2401C 发射功率。

```powershell
python tools/uart_probe.py --port COM8 adv status
python tools/uart_probe.py --port COM8 adv off
python tools/uart_probe.py --port COM8 adv on
python tools/uart_probe.py --port COM8 adv ibeacon --uuid 00112233-4455-6677-8899-aabbccddeeff --major 1 --minor 2 --interval-ms 500
python tools/uart_probe.py --port COM8 adv service --mode connectable --uuid 7be00001-4b5a-4c31-9d91-6c2f9a0b5400 --data-hex 0100 --name GW32 --interval-ms 250
python tools/uart_probe.py --port COM8 adv raw --mode scannable --adv-hex 020106 --response-hex 050947573332 --interval-ms 1000
```

如果扫描仍在运行，先由任务执行器明确关闭冲突角色。广播热更新的 AdvData/ScanRspData 是两个 HCI 写入，不能保证空口原子切换；恢复失败将停止广播并报 FAULT。已连接手机时暂停新的广播，关闭广播配置不会隐式切断该手机连接；`0x0F` 用于显式断开。

手机 GATT 服务 UUID 为 `7be00001-4b5a-4c31-9d91-6c2f9a0b5400`；同一 UUID 后缀下 `7be00002` 为只读 INFO，`7be00003` 为 RX Write Request，`7be00004` 为 TX Notify。接收数据以 `0x22` 批次交给 ESP，队列满时 ATT 写入报错；ESP 用 `0x0E` 回复，通知受理不等于手机业务已接收。详见 [UART 协议](../../server/docs/gateway-uart-protocol.md)。

## 经 ESP 串口升级 nRF

普通构建默认不开放 DFU。首次安装需通过 SWD 写入匹配分区的 MCUboot 和应用；已有无 bootloader 的应用不能凭 UART 命令获得双槽回退能力。准备本型号专用的 ECDSA P-256 签名私钥后，在 NCS toolchain terminal 中构建：

```powershell
.\scripts\Build.ps1 -NcsRoot C:\ncs\v3.2.1 -Target gw32 -CommissionedRadio -Dfu -SigningKey D:\keys\gw32-nrf54-release.pem
```

脚本不生成私钥、不烧录，拒绝 SDK 中公开的示例私钥。后续 UART 上传使用该构建的 **`zephyr.signed.bin`**。每次发布同步更新 `VERSION`、CMake 项目版本及 `gw_app.c` 的 HELLO 版本。DFU 使用 swap-using-offset，运行时校验槽位和几何，不写当前执行分区；接受大小以 `GET_DFU_STATUS.max_image_size` 为准。

流程为：服务器下发 `FIRMWARE_UPGRADE(component=NRF54)` → ESP 验证签名 manifest、型号及文件 SHA，持久化任务 journal → 停止/断开冲突蓝牙角色 → BEGIN → CHUNK → FINISH → REBOOT → 新启动 HELLO、健康检查 → VERIFY_RUNNING → CONFIRM → 报告结果。MCUboot 独立验证镜像签名；仅传输 SHA 正确不能证明镜像可信。确认前读取当前运行槽的完整镜像 SHA，防止因相同版本字符串或启动拒绝而误报成功。

```powershell
python tools/uart_probe.py --port COM8 dfu status
python tools/uart_probe.py --port COM8 dfu upload D:\release\zephyr.signed.bin --reboot
# 重连、核实新版本及业务健康后，使用相同的预期镜像文件：
python tools/uart_probe.py --port COM8 dfu confirm D:\release\zephyr.signed.bin --accept-current-image
```

擦除和完整镜像校验使用 30 秒命令等待；重发须沿用同一控制 ID 和相同内容。下载可跨 UART 重握手续传；nRF 自身复位后从头开始下载，不能把 RAM 中的 accepted_offset 当作断电检查点。重启前 ESP 应接管剩余采集 FIFO，nRF 复位不保留 RAM 记录。未确认试启动默认 120 秒后由运行中的主循环复位并回退，逾期确认返回 STALE；若新程序无法启动或主循环挂起，ESP 必须通过硬件复位触发 MCUboot 回退。不会自动把试启动固件标为永久有效。

服务器组件路由、制品约束和成功结果校验已有实现；[ESP32-S3 固件](../esp32s3/README.md) 当前已有下载、manifest 验签、任务 journal、nRF UART DFU 传输及新启动校验/确认执行器。上述探针继续作为独立 UART 联调参考。ESP GPIO8 目前仅初始化为输入释放，挂死 nRF 的主动硬件恢复路径仍待完成；独立可信 nRF 版本的平台登记也尚未贯通，详见统一标准 C04、C06。本轮文档核对不代表 nRF NCS 目标构建、真实 MCUboot 验签/掉电回退或整机 OTA 已验收。

## 主机测试与验证边界

使用 C11 编译器执行真实 C 实现；例如提供本地 `zig.exe` 路径：

```powershell
.\scripts\Test-Host.ps1 -Compiler D:\tools\zig\zig.exe
```

`test_core` 检查帧、过滤和 FIFO；`test_gatt` 检查 Central 生命周期；`test_radio` 检查扫描/广播、热更新恢复、快速连接断开及 DFU 维护准入；`test_app` 检查 UART 会话与可靠传输；`test_peripheral` 检查手机连接、订阅、写入背压及回复去重；`test_dfu` 检查升级状态机、运行镜像校验和确认。后五组使用精简 Zephyr/蓝牙/串口/Flash 替身，不验证控制器射频、真实 ATT 时序、DMA 并发、镜像数字签名或 SDK ABI。

2026-11-18 已通过 `Test-Host.ps1` 使用 Zig 0.14.1 的 C 编译器执行以上六组测试，编译参数为 `-std=c11 -Wall -Wextra -Werror`。其中 Central 为 7 组、Peripheral 为 8 组生命周期场景，UART 应用为 6 组传输场景。Python 工具测试运行 `python -m unittest discover -s tests -p 'test_*.py' -v`。当前仍未完成 SDK 目标编译、MCUboot 真实签名启动、掉电回退和实板联调。

串口探针需安装 Python `pyserial`，在本目录运行：

```powershell
python tools/uart_probe.py --self-test
python tools/uart_probe.py --port COM8 status
python tools/uart_probe.py --port COM8 --listen 5 scan on
python tools/uart_probe.py --port COM8 command 0x17 HEX
```

最后一行的 `HEX` 替换为该操作码所需的参数十六进制串。探针只显示上报，没有可靠持久队列，因此不会发送 `CUSTODY` ACK；看到同批次重传属于预期行为。

实板必须覆盖：CTS 阻塞与恢复、突发 RX、坏帧重同步、ESP 重启后的相同批次重放、RAM 队列满、主动/被动扫描与扫描响应、GATT 失败/超时/断连、通知流量和空中抓包。nRF 队列位于 RAM，整机掉电不持久；ESP 接收 `CUSTODY` 前必须可靠接管数据。不要将主机测试通过描述为固件已在板上验证。

## 依据

- 官方 Nordic MCP：NCS v3.2.1 release notes、nRF54L15 compatibility matrix、Simple GPIO FEM、HFXO/LFXO/UARTE 文档。
- 固定版本源码：[sdk-nrf v3.2.1](https://github.com/nrfconnect/sdk-nrf/tree/v3.2.1)、[sdk-zephyr ncs-v3.2.1](https://github.com/nrfconnect/sdk-zephyr/tree/ncs-v3.2.1)、[sdk-nrfxlib v3.2.1](https://github.com/nrfconnect/sdk-nrfxlib/tree/v3.2.1)。该版本 FEM compatible 为 `generic-fem-two-ctrl-pins`，不要混用 latest 文档中的新名称。
- 本地 AT2401C 数据手册：`D:/chat/HardwareKnowledgeBase/originals/81fa54ff022fee2be9d7817f0d3ba41065b6f8fd180a871b7588eea448ceb366.pdf`；库内元数据为用户提供、候选证据，官方产品页为 https://www.icofchina.com/FEM/2451.html。
