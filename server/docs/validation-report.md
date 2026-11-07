# 服务器 0.1.0 验证报告

执行日期：2026-11-07。环境：Windows amd64，Go 1.27.3；本机 Intel Core i7-1165G7。以下结论来自实际执行，不将配置文件、模拟器或交叉构建等同于生产验收。

| 检查 | 结果 |
| --- | --- |
| `go test ./... -count=1 -timeout=90s` | 46 项顶层测试、23 项子测试通过 |
| `go vet ./...` | 通过 |
| Go 格式化 | 通过 |
| OpenAPI 3.1 验证 | 58 个操作、84 个 schema，验证通过，全部操作有服务端路由 |
| 独立 JSON Schema | Draft 2020-12 结构验证通过 |
| 数据库 | 管理库 18 表、数据库 9 表；WAL、FULL 同步、迁移校验和及外键测试通过 |
| Windows 构建 | 四个程序构建成功并实际运行 |
| Linux amd64 构建 | 四个程序交叉构建成功，尚未在 Linux 主机执行 |
| 部署脚本 | PowerShell 语法检查、Git Bash `bash -n`、Compose YAML 解析通过 |
| 容器构建 | GitHub Linux CI 的 Compose 配置检查及镜像构建通过；尚未运行完整 Compose 服务栈 |
| Race detector | 本机缺少 CGO 工具链；GitHub Linux CI 的 `go test -race ./...` 通过 |

GitHub 同时执行 Ubuntu 与 Windows 自动测试、构建，以及容器构建。运行记录可在 [Server CI](https://github.com/lyndon0728/ble-gateway-manager/actions/workflows/server-ci.yml) 查询；交付包 `release.json` 指明源码提交，`validation/github-ci.json` 保存该提交的运行结果。

## 实际进程端到端验证

执行 `scripts/e2e.ps1 -ServerPort 18181 -CustomerPort 19191`，使用独立临时目录与 HTTP 回环接口，启动服务器、网关模拟器、客户模拟器，最后调用维护 CLI。最终一次执行时间为 `2026-11-07T12:34:07Z`。

- 网关断网期间持久保留数据，重连后发送 10 条唯一事件。
- 网关故意忽略一次服务器应用确认，使用相同事件身份重试。
- 客户先持久提交，再故意丢弃一次 ACK；实际接收请求 2 次，持久投递 10 条，业务记录仍为 10 条。
- REBOOT 任务达到 `SUCCEEDED`，执行结果为 `VERIFIED`。
- 服务停止后，加密备份和空目录恢复通过；恢复标记存在，新的重解析需先对账最高 revision。
- 测试结束后关闭本次启动的进程，保留证据，不修改现有客户数据。

交付包 `validation/e2e-report.json` 与 `validation/go-test.json` 保存结果；原始本机证据目录记录在 JSON 中。证据目录包含测试运行凭据，**不会**打包或提交到代码库。

## 故障与安全测试重点

覆盖 T1/T2 事务故障回滚、16 路并发重复投递、服务重启后稳定身份与载荷、HTTP 部分/无应用 ACK、MQTT QoS1 PUBACK 与 retained ACK 不代表接管、遥测阻塞时控制结果仍可确认、旧租约 fencing、跨库规则激活失败恢复、慢目标隔离、映射失败隔离、签名与兼容校验、配置 CAS、任务 TTL/UNKNOWN、结果乱序不回退、跨组权限、CSRF、凭据撤销、重复设备凭据拒绝、恢复 revision floor 9 后新修订为 10、备份破坏/截断拒绝及维护运行锁。

MQTT 测试使用真实 TCP 与 MQTT 3.1.1 测试 peer，能够检查应用与线级协议交互；它不替代 Mosquitto/EMQX 的生产 TLS、账户和 ACL 验收。

## 局部吞吐测量

`BenchmarkDurableIngress100`：每批 100 个不同身份的 18 字节参考帧，SQLite WAL / synchronous=FULL，约 **3,054 条事件/秒**，约 33 ms/批；执行 90 批，使用新的临时数据库。

这只测量本机 T1 原始持久接管函数，**不包含** HTTP/TLS、无线扫描、后续解析、客户投递、长期元数据增长或真实磁盘断电。不能据此换算承诺网关台数；整体容量必须在客户机器、实际报文与目标接收端组合下进行长时间测试。

真实硬件、生产证书/Broker、Linux 容器实际运行、物理断电恢复与生产容量仍是后续验收项，详见 `implementation-status.md`。
