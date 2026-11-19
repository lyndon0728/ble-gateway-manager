# GW32 蓝牙网关与管理平台

本仓库包含网关管理服务器、管理前端、协议文档和 nRF54L15 蓝牙协处理器固件。系统链路为 **服务器 → ESP32-S3 → nRF54L15**；采集数据按相反方向上传。ESP32-S3 同时承担网络管理、持久队列和双射频观测合并。

## 本次交付

2026-11-19源码交付：nRF与ESP应用版本均为 `0.3.0`，SDK基线为NCS `v3.2.1` / ESP-IDF `v5.5.6`。已落实三方对齐、Beacon组合掩码、MQTT、独立Peripheral业务、镜像盘点/配置恢复及签名时间，详见[实现与制造配置](docs/gateway-alignment-implementation.md)。UART线上帧版本仍为 `1`，新增功能使用能力位协商；固件版本不代替消息协议版本。

| 文档 / 目录 | 内容 |
| --- | --- |
| [详细需求与验收清单](docs/gateway-requirements.md) | 需求编号、职责、优先级、实现边界、验收条件和待开发项 |
| [ESP32-S3 详细软件需求](docs/esp32s3-requirements.md) | 网络、UART、持久接管、双路去重、任务、广播、GATT、双组件 OTA 与专项验收；2026-11-19 补充 |
| [ESP32-S3 开发指南](docs/esp32s3-development.md) | 工程结构、模块接口、任务调度、状态机、配置与升级执行方案 |
| [本地网页配网与平台远程配置](docs/esp32s3-network-provisioning.md) | 手机/笔记本连接热点、网络参数、凭据安装、候选回退和双入口版本对账 |
| [ESP 持久化与恢复设计](docs/esp32s3-persistence.md) | 事务、接管水位、事件/任务日志、重放与 GC |
| [ESP 验证计划](docs/esp32s3-validation-plan.md) | 50项验收的前置、注入、步骤、预期及证据要求 |
| [人工测试执行总册](docs/gateway-manual-test-plan.md) | 后续人工操作步骤、准备门槛、分册、记录模板与原验收覆盖；不代表实测已通过 |
| [系统流程与对接指南](docs/gateway-integration.md) | 启动、扫描去重、自动连接、广播、UART OTA 和异常恢复时序 |
| [ESP32-S3 ↔ nRF54L15 UART 协议](server/docs/gateway-uart-protocol.md) | 串口参数、二进制帧、能力、命令、扫描/GATT/Peripheral 数据和 DFU |
| [ESP32-S3 ↔ 服务器协议](server/docs/gateway-protocol.md) | MQTT/HTTP、配置、任务、应用 ACK、广播和组件升级 |
| [nRF54L15 固件工程](firmware/nrf54l15/README.md) | 源码、板定义、构建配置、广播及升级联调工具 |
| [固件验证记录](firmware/nrf54l15/VALIDATION.md) | 已执行测试、复现命令及未执行的目标/实板验证 |
| [服务器协议联调](server/docs/protocol-v11-integration.md) | 模拟器、自动连接规则 API 和网络候选配置验证 |
| [服务器](server/) / [前端](frontend/) | 平台实现及各自构建、运行文档 |

## 实现状态

- nRF 源码覆盖扫描过滤、原始数据队列、Central/GATT、Beacon/自定义/可连接广播、Peripheral 数据服务和可选 MCUboot UART DFU。
- 服务器包含配置与能力校验、任务、采集上下文、UUID 规则解析、自动连接和 NRF54 组件升级结果校验。
- **ESP32-S3 固件源码已加入**，见 [工程说明](firmware/esp32s3/README.md)。本地配网、远程配置、UART、采集与升级的验证状态见 [验证记录](firmware/esp32s3/VALIDATION.md)；主机测试不能替代实板验收。
- 主机软件测试已通过；**未完成 NCS 目标编译、真实烧录、射频验证或整机 OTA 验收**。本次上传为源码与协议交付，不是量产固件发布。

## 使用顺序

1. 先读需求清单，确认系统职责与当前边界。
2. 部署和联调时同时遵循两份协议；按实现记录准备制造信任材料，以系统对接指南连接两个接口。
3. 按固件 README 在匹配的官方 NCS 环境中构建。自定义板默认关闭射频，完成板级核对与测量后再启用。
4. 使用项目专用签名密钥建立 MCUboot 升级链，并逐项完成验收清单。

签名私钥、设备凭据、SDK、编译产物和本机硬件资料不作为此次源码交付内容。仓库中的本机原理图/网表路径仅用于标明历史依据，不能代替实际使用板卡的版本核对。

后续项目文档统一以 Markdown（`.md`）维护，目录、状态标注和记录模板见 [文档索引与维护约定](docs/README.md)。
