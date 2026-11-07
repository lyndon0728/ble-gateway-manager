# 控制台 0.3.0 验证报告

执行日期：2026-11-08（Asia/Shanghai）。本地环境：Windows amd64、Go 1.27.3、Node.js 24.22.1、pnpm 11.25.0、Chrome / Playwright 1.62.1。对象为管理前端、真实 Go API、持久数据库和两端软件模拟器。

| 检查 | 实际结果 |
| --- | --- |
| TypeScript 类型检查与 Vite 生产构建 | 通过；所有静态资源随 Go 程序嵌入，无 CDN 依赖 |
| API 客户端行为测试 | 4 项通过：不确定重试、503/429、CSRF恢复/401、multipart/同源限制 |
| 网关输入与请求身份测试 | 21 项通过：UUID/AD/Base64/MTU、静态对端、幂等/重复读取、签名上传身份、旧协议临时GATT |
| `go test ./... -json -count=1 -timeout=180s` | 99 项主测试、105 项子测试通过，0 失败 |
| `go vet ./...` | 通过 |
| OpenAPI / 消息 JSON Schema 校验 | API 1.9.0、78 操作、127 模型通过；CustomerBatch/CustomerAck 指南示例通过校验 |
| 页面与权限 Chrome 联调 | 24 页面，4 种非管理员角色 + 管理员；新标签 CSRF 恢复、网络失败后恢复通过，0 脚本异常 |
| 响应式页面检查 | 23 业务页 × 320/736/1024 像素 = 69 次检查通过，无文档横向溢出；宽屏和页面截图人工查看 |
| 网关控制 Chrome 联调 | 9 个任务均 SUCCEEDED / VERIFIED；扫描选中字段、网络确认、GATT连接/重复读/写/订阅/取消/断开，以及规则启停通过 |
| 数据与转发 Chrome 联调 | 13 步、76 条浏览器 API 响应；解析样本、映射/版本、规则验证激活、实际输出、重解析、冻结报文及投递重试通过 |
| 系统 Chrome 联调 | 6 步通过：项目分组、账号角色、应用令牌、实际Bearer认证/撤销、诊断ZIP、审计；另遍历67条实际审计结果均符合枚举 |
| 原 1.0 / GW32 1.1 进程级回归 | 通过；客户5条业务记录，丢ACK后2次请求仍不重复；自动连接仅1任务，订阅/断开/网络配置成功 |
| 本地交付实例 | 18780端口启动、真实登录、版本0.3.0、健康就绪；停止再启动保留实例身份和密码，重新登录通过 |

## 本次修复并回归的故障

1. Go 默认目录嵌入漏掉下划线开头的 Vite 辅助资源。改为 `all:`，逐个比对构建文件是否嵌入，缺失JS不能返回HTML冒充成功。
2. 投递重试地址与契约不一致。改为 `/retries`，浏览器实际重试保留原 delivery/output/revision/target 身份。
3. 试算输入变更、规则选择及输出抽屉的乱序请求覆盖新状态。通过延迟真实响应复现，代际保护后验证最新选择保持正确。
4. 网络丢响应导致重复任务或错误复用旧任务。实际让服务器完成激活/接受WRITE后丢弃浏览器响应，人工重试保持原 body、ETag/expiry、幂等键及任务/操作ID；明确的新READ生成新任务。
5. 制品上传重建 FormData 导致丢失幂等身份。按文件内容摘要、元数据、原manifest和签名保存上传身份；改内容或明确开始新上传才更新。
6. 会话过期后退出仍留在已登录界面。401 对退出请求生效，新标签通过同源响应恢复 CSRF。
7. 原范围与审计列表持有查询连接时再次借连接授权。关闭候选游标后再授权；单连接、范围权限与过宽查询回归通过。
8. 历史管道审计 `succeeded` 不符合公开枚举。读取时规范为 `SUCCESS`，兼容已有记录。

## 证据与复跑

仓库中的测试脚本：

- `frontend/tests/smoke.e2e.mjs`
- `frontend/tests/gateway.e2e.mjs`
- `frontend/tests/pipeline.e2e.mjs`
- `frontend/tests/system.e2e.mjs`
- `frontend/tests/fixture.ps1`、`restart-fixture.ps1`
- `server/scripts/e2e-v11.ps1`

本机 JSON/截图位于 `frontend/test-results/` 和 `server/.testdata/frontend-*-e2e/`；Go逐项结果在 `server/.testdata/frontend-server-tests.jsonl`。这些运行产物与测试数据不进入Git；CI已接入前端构建/单测和Windows浏览器联调，另保留Linux race与容器构建。

交付实例数据在 `server/.local/frontend-local/`，测试数据在 `server/.local/frontend-e2e/`；二者分离。交付实例没有预置模拟网关。初始密码只在本机文件，不写入文档、源码或截图。

本报告不是实机认证、规模容量保证或生产网络验收。真实无线动作、固件升级回滚、真实MQTT Broker/TLS、目标现场负载与断电恢复仍需对应环境验证；MQTT本轮只验证表单、版本和凭据管理，旧协议临时GATT只验证能力分支和请求构造。
