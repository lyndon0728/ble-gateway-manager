# 蓝牙网关私有部署服务器

这是服务于自有网关、由客户开发业务应用的服务器实现。软件运行在客户服务器上，提供设备管理、可靠上下行、按规则解析或原样转发、持久投递与应用确认；不包含定位功能，也不内置客户业务系统。

当前软件版本为 `0.3.0`，API 契约 `1.9.0`，兼容原 1.0 数据及 GW32 1.1 采集与控制扩展，新增完整管理前端与 13 个只读接口。服务器原有实现及运行边界见 [实现范围](docs/implementation-status.md)，0.3.0 前端与联调记录见 [前端实现说明](docs/frontend-implementation.md)。`REFERENCE_GW`、`reference_sensor` 和 GW32 模拟 profile 均不代表实机已验证。真实固件需要按 [网关协议](docs/gateway-protocol.md) 及 [UART 协议](docs/gateway-uart-protocol.md) 实现并联调；软件联调方式见 [1.1 对接说明](docs/protocol-v11-integration.md)。

完整管理前端已按 [前端总体规划](docs/frontend-plan.md) 实现：7 个功能大类、23 个业务页面加 1 个登录页，共 24 页；入口 `/ui/`，访问根目录自动进入控制台。Vue 构建产物随 Go 程序交付，客户运行时不需要 Node.js。结构化目录见 [frontend-pages.json](docs/frontend-pages.json)，开发与测试见 [前端 README](../frontend/README.md)。

## 当前可执行入口

| 程序 | 用途 |
| --- | --- |
| `gateway-server` | HTTP/TLS API、完整管理控制台、管理与数据后台任务；`--role all/manager/pipeline` |
| `gatewayctl` | 初始化、设备令牌登记、签名密钥与签名、加密停机备份、恢复与 revision 对账 |
| `gateway-simulator` | 持久事件 spool、命令 journal、结果重传；模拟断网和丢 ACK |
| `customer-simulator` | 持久客户 inbox、业务幂等与 revision 防回退，提交后故意丢 ACK |

`gateway-simulator --profile gw32-profile-1` 增加 ADV/SCAN_RSP 与双射频 capture 样本、GATT 连接/订阅/通知/断链，以及依赖应用 ACK 才提交的网络配置模拟；默认 `reference-1` 保留原 1.0 行为。服务器端不替代 ESP 的 UART 接管和双路采集去重。`scripts/e2e-v11.ps1` 使用独立临时数据目录和空闲端口验证新旧协议共存及扩展闭环。

服务器包含独立的 `management.db`、`pipeline.db` 和加密密钥 `master.key`。默认 `all` 适合本机验证；Compose 将管理和解析投递 worker 分为两个进程，共享**同一 Linux 主机的本地磁盘目录**。当前实现不是跨主机数据库集群，不能将 SQLite 数据目录放在 NFS/SMB 上。

## Windows 本机验证

新版控制台独立后台部署：运行 `scripts/deploy-local.ps1`，访问 `http://127.0.0.1:18780/ui/`；首次安装默认账号 `admin`、默认密码 `123456`，初始化密码文件为 `server/.local/frontend-local/secrets/admin-password`。已有实例及自定义密码文件不会被默认值覆盖。该实例使用独立数据目录，不预置模拟网关；以 `-Action Status` 查看状态，以 `-Action Stop` 停止。前端源码修改后先在仓库根目录运行 `pnpm --dir frontend build`。

需要 [go.mod](go.mod) 指定的 Go。PowerShell 脚本也能找到工作区 `.tools/go/bin/go.exe`。

```powershell
cd server
.\scripts\build.ps1 -Test
.\scripts\run-local.ps1
```

打开 `http://127.0.0.1:8080/`，本地首次安装默认账号 `admin`、默认密码 `123456`，初始化密码文件为 `server/secrets/admin-password`。再次启动保留已有账号、自定义密码、数据和密钥。HTTP 开关仅允许显式回环 IP，不能用此方式开放生产服务。

可以运行完整本机验收脚本，它使用单独的临时目录和 18080/19090 端口：

```powershell
.\scripts\e2e.ps1
```

脚本会创建目标、规则、启用版本，运行真实的服务器及两端模拟器进程，注入网关断网、网关忽略 ACK、客户持久提交后丢 ACK，验证 10 条事件只形成 10 条客户业务记录，同时验证 REBOOT 模拟结果和加密备份恢复。最终 JSON 中给出本次证据目录。它会保留临时数据用于排查，并结束它启动的进程；不会改动现有客户数据目录。

Linux 原生模式使用 `bash scripts/build.sh --test`、`bash scripts/run-local.sh`。原生版本与容器运行相同的 Go 程序。

## 生产部署与离线交付

按 [部署运维说明](docs/operations.md) 准备客户 CA、服务证书、Broker 证书，然后运行 `sudo bash scripts/install-linux.sh --build`。没有提供证书时安装会停止并报告缺少的文件。生产 TLS 验证不会通过跳过证书检查来绕过。

联网构建机可生成离线镜像包：

```bash
bash scripts/package-offline.sh /absolute/new-output-directory
# 将整个目录交付到客户主机后：
bash scripts/load-offline.sh
sudo bash scripts/install-linux.sh
```

离线包包含 Docker 镜像、镜像身份记录、SHA256 校验、Compose 和安装脚本；不包含客户凭据、CA 私钥、服务器私钥或运行数据。客户主机仍需事先安装 Docker Engine 和 Compose。Linux 安装、Compose 完整服务栈、真实 MQTT Broker 以及生产 TLS 联调必须在目标环境执行；没有执行记录时，不应将配置文件的存在视为部署验收完成。

## 客户对接方式

1. 通过管理员建立客户目标的不可变版本；生产目标必须显式加入 `GW_ALLOWED_TARGET_HOSTS`。
2. 创建规则：匹配网关型号、分组或 `protocolHint`，选择 `PASSTHROUGH`、`DECODE` 或 `QUARANTINE`，固定解析器、schema 和目标版本。
3. 验证并激活整个规则包；没有匹配规则的报文默认进入隔离区，避免自动猜协议。
4. 客户持久化接收 inbox/业务后，按 `deliveryId` 返回应用 ACK。HTTP 200 或 MQTT PUBACK 本身不代表业务接管完成。
5. 调用任务 API 下发配置、重启或诊断。任务已受理与设备已执行分别显示；服务器仅在收到相符的执行结果后完成任务。

登录后可读取 `/openapi.json`；源码契约位于 `internal/platform/assets/openapi.json`。客户字段重命名、单位换算和包装见 [客户映射](docs/customer-mapping.md)。具体消息字段、幂等键、ACK 与恢复规则见 [协议说明](docs/gateway-protocol.md)，参考二进制报文见 [传感数据格式](docs/reference-sensor.md)。

## 本版能力边界

已实现的路径通过 Go 单元/集成测试和本机端到端脚本验证。测试范围、执行结果由实际运行命令和报告决定。当前内置解析器是固定代码，支持参考传感帧、四字节演示帧和 BLE AD 结构；不支持上传任意脚本执行。多输出解析器需要扩展组替换协议后再交付。

GATT 和固件升级任务的设备侧能力必须由实际型号声明；参考网关只声明 CONFIGURE、REBOOT、DIAGNOSTICS。设备清单与制品具备验签、hash 和兼容性校验，实际无线动作、固件写入、启动回滚需要固件实现。服务器安装升级不能用上传文件替代停机维护与迁移验收。跨主机扩容、自动主备切换、万级设备容量保证和断电硬件测试不在本次本机验证结论内。
