# 验证记录

## 0.3.0 三方对齐与 Beacon V2

日期：2026-11-19。应用版本：`gw-nrf54l15/0.3.0`；UART文档修订1.2，线上frame version仍为1，V2使用HELLO bit13和命令0x19/0x1A/0x1B。实现映射见[三方交付记录](../../docs/gateway-alignment-implementation.md)。

`scripts/Test-Host.ps1 -Compiler <zig.exe>`在Zig 0.14.1下执行七套生产C模块测试，全部通过：`test_core`、`test_filter_v2`、`test_gatt`、`test_radio`、`test_app`、`test_peripheral`、`test_dfu`。原扫描/GATT/广播/Peripheral/DFU回归保留。

| 新增验证 | 实际结果和边界 |
| --- | --- |
| 组合掩码引擎 | 名称来源/前缀/完整匹配、UUID及major/minor位掩码、RSSI区间/补码掩码、服务UUID规范化、规则顺序、完整iBeacon字段组合、畸形AD通过 |
| PAIR上下文 | 同radio/boot/revision、实际地址、主动legacy范围、唯一前置ADV、500ms边界、歧义/超时拒绝、有界缓存和原始时间保留通过 |
| 原子配置 | V2完整配置/旧模式切换、非法参数/长度拒绝、配置读回、能力位及96字节诊断通过；共同87字节向量一致 |
| 无线与控制集成 | 实际生产gw_radio/gw_app链接共享过滤器，重启和重握手生命周期、配置回滚、禁用/恢复及队列行为通过；底层SDK API由替身提供 |
| 未定义行为检查 | V2引擎5000个变异输入在UBSan下通过；不是空口随机负载或硬件内存证明 |
| 三方兼容 | ESP共享引擎/JSON编译与平台V2校验、编译和离线预览均有软件回归；逐项见各组件验证记录 |

没有完整NCS v3.2.1 workspace和匹配ARM工具链，**尚未执行nRF目标west build/sysbuild**，没有可宣称通过的HEX、目标链接或RAM/Flash预算。ESP执行器已接入并有ESP目标构建，但未连接/烧录双芯片实板，真实UART流控、RF/FEM、手机加密会话、MCUboot验签/交换、掉电回退及整机OTA仍待验收。自定义板 `GW_RADIO_VERIFIED=n` 和ESP板级复位默认关闭；软件测试不授予硬件能力。

以下保留0.2.0原始验证记录；其“ESP执行器尚未接入”等描述仅说明当时基线，不代表0.3.0源码状态。

## 0.2.0 历史记录

日期：2026-11-18。应用版本：`gw-nrf54l15/0.2.0`。协议基线：仓库内 UART 1.1 及能力协商扩展（线上帧 version 1）。

## 已执行

- Windows 主机使用 Zig 0.14.1 的 C 编译器，启用 C11、`-Wall -Wextra -Werror`。
- `test_core`：真实 `gw_core.c`，覆盖官方 CRC/帧向量、最大帧和损坏输入、2755 字节配置、16/32/128 位 UUID、规则优先级、畸形 AD 边界、FIFO 容量/回绕、冻结批次、ACK 和序号耗尽。
- `test_gatt`：真实 `gw_gatt.c` 配合 Zephyr API 替身，7 组生命周期场景通过，包括操作重放、写入超时/断链 UNKNOWN、迟到连接关闭、连接回收屏障、读取上限、订阅及通知、CCC 范围与多匹配歧义。
- `test_radio`：真实 `gw_radio.c` 和 `gw_core.c` 配合蓝牙 API 替身，通过配置冲突/回滚、广播模式、扫描数据保留、绑定身份地址处理、角色冲突、连接期间停止扫描及恢复测试。
- `test_app`：真实 `gw_app.c` 和 `gw_core.c` 配合 UART/内核替身，6 组场景通过，包括 HELLO 会话隔离、命令幂等/冲突、发送失败后的响应缓存、重握手保留 FIFO、旧会话 ACK、BUSY 退避、CUSTODY 回收及 FAULT 暂停。
- `test_peripheral`：8 组场景通过，包括手机角色隔离、INFO、RX/MTU/队列满、通知缓存与完成回调、快速连接断开及维护准入。
- `test_dfu`：真实 DFU 模块配合 Flash/MCUboot 替身，通过边界、次槽写入、重复/乱序、读回失败、终止和重启、运行槽 SHA 不匹配、确认门槛和未确认复位。替身没有执行真正的数字签名验证或 MCUboot 镜像交换。
- 广播扩展回归通过：mode 2、在线数据更新及部分失败恢复、专用启停、35 字节状态、FAULT 角色阻断、Peripheral 快速连接断开事件、升级前连接对象排空。
- `gw_core.c` 的随机/畸形输入与队列测试另在 UBSan 下通过，未报告未定义行为。
- `uart_probe.py --self-test`：CRC32C 标准值、协议 HELLO 黄金向量和最大载荷往返通过。
- Python 广播及升级工具 6 项单元测试通过，包含 iBeacon/UUID 字节序、31 字节边界、1024 字节分块、VERIFY_RUNNING 先于 CONFIRM，以及未支持能力/错误文件不发起写入。
- 服务器 `go test ./internal/platform -count=1` 通过；增加成功结果与制品 SHA-256 绑定后，`TestNRF54|TestFirmware|TestArtifact` 定向回归通过，覆盖广播能力校验、nRF 固件任务和升级成功证据。
- PowerShell 构建与测试脚本语法检查通过；板级 DTS、UART/HFXO、MPSL FEM、广播及 GATT 关键接口对照 NCS v3.2.1 官方源码检查。
- 文档交付检查：UART 协议 5 条帧向量解码/重编码通过；服务器协议 11 个 JSON 示例分别通过 OpenAPI 与消息 Schema 校验，广播 Base64/AD 边界及 PowerShell 示例语法通过。需求清单含 93 个唯一需求编号、18 个验收场景，相对文件链接已检查；示例没有向真实服务器写入。

所有模拟测试运行实际应用模块；其 API 替身不构成对 Nordic SDK ABI、控制器实现、抢占时序或射频性能的验证。

## 尚未执行

开发机没有完整 NCS v3.2.1 workspace 和匹配 ARM toolchain，尚未执行目标 `west build` 或 `--sysbuild`，因此没有经过目标链接验证的 HEX、RAM/Flash map 或可宣称通过的 SDK 构建结果。未连接/烧录目标硬件，也未执行实板 RF、UART EasyDMA/CTS、持续负载、真实镜像验签启动、掉电回退或空中抓包测试。ESP 的固件执行器尚未接入，整机 OTA 不能宣称已完成。

GW32 板定义对应 2026-10-29 网表。HFXO 启动预算及 AT2401C settle 参数仍需实测；默认自定义板关闭 BLE 射频，DK 开发配置开启。已绑定私有地址的扫描受 SDK 地址解析限制，具体行为和启用步骤见 [README](README.md)。

## 复现

在本工程目录、Windows PowerShell 中执行：

```powershell
.\scripts\Test-Host.ps1 -Compiler 'D:\00物联网平台\.tools\zig\zig-x86_64-windows-0.14.1\zig.exe'
python tools/uart_probe.py --self-test
python -m unittest discover -s tests -p 'test_*.py' -v
```

目标构建须进入官方 NCS v3.2.1 toolchain terminal，按 [构建说明](README.md#构建) 指定实际 SDK 路径。
