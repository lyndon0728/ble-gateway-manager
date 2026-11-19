# ESP32-S3 0.2.1 实现决策与交付范围

日期：2026-11-19。本文将[开发设计](../../docs/esp32s3-development.md)映射到本次源码；它不替代[验证记录](VALIDATION.md)或[50项整机验收](../../docs/esp32s3-validation-plan.md)。

## 两层协议与模块

服务器通过HTTPS设备接口下发任务，ESP负责身份、授权时限、持久受理、网络配置、结果重传及应用ACK。ESP与nRF之间沿用UART 1.1；nRF负责其射频控制和采集。UART命令响应、CUSTODY、平台RAW_PERSISTED、RESULT_PERSISTED具有不同含义，不能互相替代。

| 逻辑职责 | 实际实现 |
| --- | --- |
| persistence、config、credential、network executor | `gw_core`；公开接口在`include/gw_core.h` |
| provisioning和本地网页 | `gw_provision`；Security2会话内调用核心接口 |
| transport、task ledger、result outbox | `gw_http`与`gw_cloud` |
| UART codec与nRF控制 | `gw_wire`直接编译既有纯C模块；`gw_nrf`提供驱动及协议适配 |
| observation、dedup、raw outbox、peripheral inbox | `gw_capture`及可主机编译的`gw_spool` |
| scan/advertising、GATT、OTA coordinator | `gw_actions`；分别调用ESP NimBLE、nRF UART及OTA实现 |

SDK固定ESP-IDF v5.5.6，BLE Host采用NimBLE，CH390组件固定0.4.1。默认8MiB Flash与Oct29 UART接线基线不代表后续修订板卡已验证。CH390和现场按钮GPIO需按板卡设置。

## 存储后端选择

原设计中的统一追加日志、检查点和通用GC没有按原字节格式实现。本版将低频配置/动作状态与高频观测分开，以减少扫描接管时复制配置树的开销；每项必须一起提交的不变量仍限定在同一后端内。

| 域 | 持久发布方式 | 原子边界 |
| --- | --- | --- |
| `gwstore`加密NVS | 先写另一份完整快照并commit/readback，再commit/readback世代绑定指针 | 同一`gw_store_transaction`中的配置、意图、结果和相关计数器 |
| `gwspool`加密Flash | 记录先完整写入并读回，再发布带世代和校验的轮转根 | nRF批内记录＋接管水位；观测配对＋事件身份；逐项交付状态 |
| `gwstore/secrets` | 独立加密NVS命名空间，安装持久成功后才发布可用凭据引用 | 秘密安装与后续引用按先后屏障衔接；未被引用的孤立秘密不等于配置提交 |
| nRF镜像/ESP OTA槽 | 专用分区/官方OTA API，持久动作journal保存阶段与证据 | 镜像校验、试启动与确认依赖恢复状态机，不宣称跨分区单次原子写入 |

业务对象由各后端互斥串行提交，不以通用单写入任务API暴露。持久写入结果模糊或已发布数据损坏时停止后续接纳，不自动清空日志、不自动选择旧快照重做动作。CRC/SHA用于损坏检测；介质的真实断电保证需板上验证。

`gwstore`完整编码快照上限96KiB，最多256个键，值的Base64编码也占预算。单任务先预留结果空间；任务身份最多保留64项，还受总容量限制。回收ACK后的大载荷仍保留任务摘要与终态。达到预算时拒绝新副作用，不能无限接收任务；长期部署需要另行实现有明确平台保留窗口的归档策略，不能直接清空防重记录。

`gwspool`容量1MiB，固定记录槽与轮转根共同占用该容量。单条小广播也占一个记录槽，因此不能用1MiB除以广播字节数估算离线时长。具体槽数、根轮转和测试见[采集组件](components/gw_capture/README.md)。满盘时返回BUSY并保留已接管数据；永久拒绝进入隔离，不把它当ACK回收。

## 配网与平台闭环

本地入口与远程入口共用网络执行器及排他租约。本地版本使用`localRevision`，所有网络变更推进`networkGeneration`；服务器`desiredRevision`由服务器分配。LOCAL_ONLY允许现场保存但保留最后验证成功网络，不能伪报平台已确认。

远程凭据单独通过设备认证接口投递；CONFIGURE仅引用credentialRef。候选验证把DNS与TLS连接绑定被测接口，平台逐项确认EXECUTING探测结果后才提交。最终SUCCEEDED获得RESULT_PERSISTED后更新平台确认版本并解除租约。旧候选回退后，未确认的旧probe不会经同名旧接口重发成新证明。

0.2.1将已报告配置版本作为单调水位：只有匹配的成功结果ACK可推进，迟到旧结果、DUPLICATE、错误ACK或不安全数值不会使版本倒退。ACK删除与水位推进仍由同一journal提交，不额外写入另一份可被旧任务覆盖的版本。

平台新增数据库v4迁移、OpenAPI/消息Schema和网络配置页面；[平台联调文档](../../server/docs/network-provisioning-integration.md)包含部署主密钥、权限、请求与结果契约。此扩展必须与固件一起部署。

## 本地与远程 ESP OTA

`gw_actions/gw_ota.c` 复用制造发行公钥、原始 manifest 验签、非运行槽写入和运行镜像核验。远程任务继续使用 taskId/digest/TTL 与云端持久结果；本地使用独立 source=LOCAL/uploadId 和 `ota/local`，与远程 `ota/active` 分开，不能把本地结果合成为不存在的云任务或配置 revision。

`gw_provision` 的已认证 Security2 业务通道转交五个 OTA 操作；每块最多 4096 字节，BEGIN 固定本次窗口期限，空闲 5 min 中止。仅最后一块完全相同字节可重试，接收 offset 保留 RAM，避免每块重写整个 NVS 快照。RECEIVING/READY 经掉电或窗口收尾后不自动安装；COMMIT 持久意图和设置启动槽后安排重启，恢复以实际启动槽和镜像为准。

本地试运行确认不等待服务器或 UTC，使用持久签名安装授权及 90 s 限时健康流程；核心、接管、nRF、动作及配网初始化、至少 10 s 启动观察、实际镜像与 nRF 健康条件满足才标有效。远程继续受其授权及网络恢复约束。本地/云端动作共用维护租约，网络候选和另一组件 OTA 不能抢占。完整协议、打包和状态见[OTA 设计](../../docs/esp32s3-ota.md)；真实硬件回滚必须另有证据。

## 明确边界

- 当前上行实现HTTPS轮询及HTTP接入；MQTT不是本版设备传输路径。
- 当前时间信任由制造指定SNTP源与合理UTC构成，尚非密码学认证授时。仍严格检查TLS证书与主机名。
- 出厂安全材料由工具生成，实际Flash加密、NVS密钥烧录、eFuse设置属于制造步骤；固件不自动执行不可逆制造操作。
- 未认证的Peripheral RX只进入独立本地inbox，不作为网络配置、云任务或任意写入指令。手机业务认证/云端双向业务契约仍需单独定义。
- 无物理网关测试证据时，不声明CH390、射频共存、Flash寿命、手机兼容、真实双组件升级或72小时稳定性已达标。

以上边界均保留在验收计划中。本版交付可审查源码、测试入口、平台扩展及构建流程；生产放行以对应实测记录为准。
