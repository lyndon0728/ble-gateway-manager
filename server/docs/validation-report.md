# 服务器 0.2.0 验证报告

执行日期：2026-11-07。环境：Windows amd64、Go 1.27.3。协议输入为 GW32I33V2 ESP—服务器 1.1 与板内 UART 1.1 设计稿；验证对象是服务端、API 契约与软件模拟器。

| 检查 | 结果 |
| --- | --- |
| `go test ./... -json -count=1 -timeout=120s` | 85 项主测试、97 项子测试通过，0 失败 |
| `go vet ./...`、gofmt | 通过 |
| OpenAPI 3.1 / JSON Schema | 65 个操作、112 个 schema；所有操作均有路由；Draft 2020-12 结构验证通过 |
| 数据库升级 | 保留 v1 数据；重复启动、v1/v2 校验和篡改拒绝、未来版本拒绝、DDL 中途失败回滚测试通过 |
| 权限与并发 | 事务内撤权检查；单连接池下列表/修改/自动任务无嵌套查询阻塞；8 并发控制消费者仅创建一项触发任务 |
| 脚本 | PowerShell 解析、Git Bash `bash -n` 通过 |
| Windows 进程验证 | 服务器、网关模拟器、客户模拟器与维护 CLI 实际执行，见下文 |

本地执行日志位于交付包 `validation/`。源码仓库不提交包含运行凭据的临时数据目录。GitHub 的 Ubuntu/Windows、Linux race detector、镜像构建与 Windows GW32 进程测试结果，以同一源码提交的 `validation/github-ci.json` 为准。本机没有 Docker 或 C 编译器，未把交叉构建当成 Linux 部署验收。

## GW32 1.1 实际进程闭环

执行 `scripts/e2e-v11.ps1`，报告时间 `2026-11-07T15:25:43Z`；使用独立临时目录、HTTP 回环接口和随机空闲端口：

- 旧 1.0 温湿度两条与新 1.1 ADV/SCAN_RSP 同时接入，重试保留各自版本。
- nRF 可连接 ADV 不带服务 UUID；ESP SCAN_RSP 含 128 位服务 UUID，规则自动创建且仅创建一次 GATT_CONNECT。
- GATT_SUBSCRIBE 后上传一条通知，连同前述数据在客户侧形成 5 条业务记录；客户丢弃应用 ACK 后保持业务幂等。
- UNSUBSCRIBE、DISCONNECT 成功；LINK_DOWN 原文被持久保存，服务端记录 CLOSED，断链事件不进入业务转发。
- 双上行网络配置在服务器持久接管各接口探测后才返回 VERIFIED/SUCCEEDED，reported revision 为 2。
- 重启模拟器后另执行 NETWORK/PASSIVE 诊断，保留 ETHERNET/WIFI_STA 配置快照，返回模拟 AP 观测并明确标记 `physicalMeasurements=false`。

该闭环没有真实 BLE、UART、Wi-Fi 或以太网故障切换参与；不会据此证明物理网络接口已经切换。

## 旧版与故障回归

`e2e.ps1` 使用最终源码重复验证：10 条独立事件形成 10 条客户业务记录，客户两次请求、丢 ACK 重试、REBOOT VERIFIED，以及停机加密备份/空目录恢复成功。

新增测试覆盖：16/32/128 位 UUID 端序及九类 AD 类型；错误后续字段不能泄漏前缀候选；ADV/RSP 两种到达顺序；14 类不安全或不新鲜观测不触发连接；跨网关同对端独占；M 库提交后 P 库消费记录失败的重放去重；LINK_DOWN 先到时晚到 CONNECT 结果不得复活引用；网络探测和成功放在同批时拒绝；网络部分配置合并；MTU/实际安全状态/CCC/写回读验证；设备能力交集、私有 autoTrigger 注入拒绝、作用域 RBAC/CAS。

旧 T1/T2 回滚、MQTT QoS1 应用确认、乱序结果、慢目标隔离、恢复 fencing、签名制品和维护锁等回归仍包含在全量套件内。

## 尚未取得的证据

真实 GW32 板卡、nRF/ESP UART、双射频配对、通知吞吐、Flash 掉电、生产 Broker/TLS/ACL、完整 Compose 部署和客户服务器容量仍需验收。本版不提供设备台数或持续吞吐承诺。原 0.1.0 T1 局部基准不能作为 0.2.0 含自动连接的容量结论。
