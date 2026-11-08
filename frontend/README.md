# 蓝牙网关管理控制台

Vue 3 + TypeScript + Vite + Element Plus，7 大类 / 27 页面 / 20 个侧栏入口。采用同源 Cookie 会话、CSRF、资源范围授权、ETag 和幂等任务接口。业务对象来自真实服务器；没有浏览器内的模拟数据库。

## 构建与运行

在仓库根目录执行（Node.js 24、pnpm 11.25.0；Go 版本见 `server/go.mod`）：

```powershell
pnpm --dir frontend install --frozen-lockfile
pnpm --dir frontend build
.\server\scripts\build.ps1 -Test
.\server\scripts\deploy-local.ps1 -SkipBuild
```

访问 `http://127.0.0.1:18780/ui/`。本地首次部署的默认账号为 `admin`，默认密码为 `123456`，初始化密码文件为 `server/.local/frontend-local/secrets/admin-password`。已有实例或已有自定义密码文件会保留原密码，不在重启时恢复默认值。后台进程在脚本结束后继续运行，数据目录为 `server/.local/frontend-local/data`。

```powershell
.\server\scripts\deploy-local.ps1 -Action Status
.\server\scripts\deploy-local.ps1 -Action Stop
```

重复 Start 保留账号和数据。更新程序时先 Stop，再构建，最后 Start。此入口是本机回环部署，不注册开机服务；正式客户安装使用服务器现有 TLS / Linux 安装流程。

已有本地实例如需明确恢复 `admin / 123456`，先停止实例，再执行一次性重置；旧浏览器会话会同时失效，其他账号和业务数据保留：

```powershell
.\server\scripts\deploy-local.ps1 -Action Stop
.\server\bin\gatewayctl.exe reset-admin-password --data-dir server/.local/frontend-local/data --password-file server/.local/frontend-local/secrets/admin-password --local-default-admin
.\server\scripts\deploy-local.ps1 -SkipBuild
```

产物写入 `server/internal/platform/assets/ui` 并嵌入 Go 可执行文件。客户运行时不需要 Node.js、pnpm 或外网 CDN。仓库保留构建产物，纯 Go 构建亦可使用；修改前端源码后必须重新执行前端构建。浏览器深链接刷新由 Go 返回 SPA 入口；不存在的 JS/CSS 仍返回 404。

开发时先启动本地服务，再运行 `pnpm --dir frontend dev`。Vite `/api` 代理到 `127.0.0.1:18780`；浏览器 Origin 应与服务端一致，因此需要表单写入联调时优先使用构建后的同源 `/ui/`。若使用 Vite 写入联调，将隔离测试服务器 `GW_PUBLIC_URL` 设置为 Vite 的实际地址后启动，不修改生产 Origin 策略。

## 目录

| 位置 | 用途 |
| --- | --- |
| `src/App.vue`、`src/router.ts` | 7 大类导航、27 页面、权限与会话恢复 |
| `src/pages` | 业务页面及上下文详情 |
| `src/features/gateway` | 配置差异、输入校验、任务身份 |
| `src/features/pipeline` | 解析、版本化规则、客户映射与重解析 |
| `src/features/ai` | 资料上传、服务状态、模板复用与草稿辅助 |
| `src/features/system` | 中文对接指南、契约一致的消息示例 |
| `src/lib` | 同源 API、CSRF、角色范围、分页、格式 |
| `tests` | API 行为测试、浏览器流程与隔离测试进程 |

## 前后端联调

Windows 安装 Chrome 后，在仓库根目录执行：

```powershell
.\frontend\tests\fixture.ps1
pnpm --dir frontend test
pnpm --dir frontend test:e2e
.\frontend\tests\fixture.ps1 -Action Stop
```

fixture 启动真正的 Go 服务器、GW32 与参考网关模拟器、客户持久接收器，使用 `server/.local/frontend-e2e`。它不接触本地交付实例。也可设置 `GW_TEST_FIXTURE` 指向该目录的 `fixture.json`；账号密码只从文件读取。通过 `-Instance` 可使用新的隔离目录（仅字母、数字、下划线和连字符）；默认目录不变。首次 Start 要求空目录，避免覆盖证据；已有 fixture 更新程序使用 `fixture.ps1 -Action Restart`。测试是串行流程，同一网关同一时刻只允许一个控制任务。

测试报告和截图位于 `frontend/test-results` 以及 `server/.testdata/frontend-*-e2e`，均不进入 Git。`tests/smoke.e2e.mjs` 检查路由、320/736/1024 像素布局、角色访问、会话恢复与网络错误；另外四个脚本验证网关控制、解析投递、系统管理及 AI 导入。AI 脚本启动本机模拟兼容接口，不调用真实外部模型。敏感令牌不写入报告或截图。

## 交付边界

已确认配置是设备已验证变更的累计视图，不是完整运行时回读。未知字段不自动提交；实际下行结果以任务验证为准。协议、BLE 物理动作、固件写入和回滚仍需真实硬件联调。MQTT 目标表单可以配置与保存，真实 Broker/TLS 的传输验收按部署环境另行执行。

逐页布局与验收标准见 [总体规划](../server/docs/frontend-plan.md)，接口和交付边界见 [实现说明](../server/docs/frontend-implementation.md)。

本版 AI 页面和兼容接口配置见 [0.4.0 实现说明](../server/docs/ai-implementation.md)。当前尚未配置真实模型，API 密钥仅在设置页提交，不能写入前端源码。
