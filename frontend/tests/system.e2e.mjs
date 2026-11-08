import { chromium, expect } from '@playwright/test'
import fs from 'node:fs/promises'
import path from 'node:path'
import { randomUUID } from 'node:crypto'

// Runs only against the isolated local fixture; created audit evidence is kept.
const fixtureFile = process.env.GW_TEST_FIXTURE || path.resolve('../server/.local/frontend-e2e/fixture.json')
const fixture = JSON.parse((await fs.readFile(fixtureFile, 'utf8')).trim())
const base = process.env.GW_TEST_URL || fixture.baseUrl || 'http://127.0.0.1:18781'
if (!['127.0.0.1', 'localhost', '[::1]'].includes(new URL(base).hostname)) throw new Error('System E2E requires a local test instance')
const passwordFile = process.env.GW_TEST_PASSWORD_FILE || fixture.passwordFile
if (!passwordFile) throw new Error('An isolated fixture password file is required')
const adminPassword = (await fs.readFile(passwordFile, 'utf8')).trim(), userPassword = 'UI-test-' + randomUUID()
const evidence = path.resolve(process.env.GW_TEST_EVIDENCE || '../server/.testdata/frontend-system-e2e')
await fs.mkdir(evidence, { recursive: true })
const browser = await chromium.launch({ channel: process.env.GW_TEST_BROWSER || 'chrome', headless: true })
const context = await browser.newContext({ baseURL: base, viewport: { width: 1440, height: 1080 } })
const page = await context.newPage(); page.setDefaultTimeout(15000)
const report = { passed: false, startedAt: new Date().toISOString(), checks: [], created: {}, pageErrors: [] }
let applicationToken = ''
const sanitize = text => [adminPassword, userPassword, applicationToken].filter(Boolean).reduce((out, secret) => out.split(secret).join('[REDACTED]'), String(text))
page.on('pageerror', e => report.pageErrors.push(sanitize(e.message)))
const run = Date.now().toString(16)
const drawer = () => page.locator('.el-drawer:visible').last()
const field = (label, scope = drawer()) => scope.locator('.el-form-item').filter({ has: page.locator('.el-form-item__label').filter({ hasText: new RegExp('^' + label + '$') }) })
const fill = (label, value, scope) => field(label, scope).locator('input,textarea').first().fill(value)
async function read(url) { const r = await context.request.get(url); if (!r.ok()) throw new Error('GET ' + url + ': ' + r.status()); return r.json() }
async function mutate(method, endpoint, act) {
  const pending = page.waitForResponse(r => new URL(r.url()).pathname === endpoint && r.request().method() === method)
  await act(); const response = await pending
  if (!response.ok()) throw new Error(method + ' ' + endpoint + ': HTTP ' + response.status())
  return { data: response.status() === 204 ? {} : await response.json(), request: response.request() }
}
async function step(name, fn) { const start = Date.now(); await fn(); report.checks.push({ name, durationMs: Date.now() - start }); console.log('PASS ' + name) }
let failure
try {
  await step('登录隔离实例', async () => {
    await page.goto('/ui/login'); await page.locator('#login-name').fill('admin'); await page.locator('#login-password').fill(adminPassword)
    await page.getByRole('button', { name: '登录', exact: true }).click(); await expect(page).not.toHaveURL(/\/login/)
  })
  await step('通过范围树创建项目与分组，并携带 ETag 归档空分组', async () => {
    await page.goto('/ui/scopes'); await page.getByRole('button', { name: '新建项目', exact: true }).click()
    await fill('名称', 'UI 项目 ' + run)
    const project = await mutate('POST', '/api/v1/resource-scopes', () => drawer().getByRole('button', { name: '保存范围', exact: true }).click())
    report.created.projectId = project.data.scopeId; expect(project.data.kind).toBe('PROJECT')
    await expect(drawer()).toHaveCount(0)
    await page.getByRole('button', { name: '新建分组', exact: true }).click(); await fill('名称', 'UI 分组 ' + run)
    const group = await mutate('POST', '/api/v1/resource-scopes', () => drawer().getByRole('button', { name: '保存范围', exact: true }).click())
    report.created.groupId = group.data.scopeId; expect(group.data.parentScopeId).toBe(project.data.scopeId)
    await expect(drawer()).toHaveCount(0)
    await page.getByRole('button', { name: '修改范围', exact: true }).click(); await drawer().getByText('归档', { exact: true }).click()
    const archived = await mutate('PATCH', '/api/v1/resource-scopes/' + group.data.scopeId, async () => {
      await drawer().getByRole('button', { name: '保存范围', exact: true }).click()
      await page.getByRole('button', { name: '提交归档', exact: true }).click()
    })
    expect(archived.request.headers()['if-match']).toBe('"1"'); expect(archived.data.state).toBe('ARCHIVED'); expect(archived.data.revision).toBe(2)
    await expect(drawer()).toHaveCount(0)
  })
  await step('创建人员账号并使用 ETag 修改角色绑定', async () => {
    await page.goto('/ui/principals'); await page.getByRole('button', { name: '新建人员账号', exact: true }).click()
    await fill('账号名称', 'ui_user_' + run); await fill('初始密码', userPassword)
    const user = await mutate('POST', '/api/v1/principals', () => drawer().getByRole('button', { name: '保存账号', exact: true }).click())
    report.created.userId = user.data.principalId; expect(user.data.type).toBe('USER')
    await expect(drawer()).toHaveCount(0)
    const row = page.locator('.el-table__row').filter({ hasText: 'ui_user_' + run })
    await row.getByRole('button', { name: '管理账号', exact: true }).click()
    await drawer().locator('.binding-row .el-select').first().click(); await page.getByRole('option', { name: '审计员', exact: true }).click()
    const edited = await mutate('PATCH', '/api/v1/principals/' + user.data.principalId, () => drawer().getByRole('button', { name: '保存账号', exact: true }).click())
    expect(edited.request.headers()['if-match']).toBe('"1"'); expect(edited.data.roleBindings[0].role).toBe('AUDITOR')
    expect(edited.data.revision).toBe(2); await expect(drawer()).toHaveCount(0)
  })
  await step('创建应用、一次展示令牌、实际认证、刷新清除、撤销后拒绝认证', async () => {
    await page.getByRole('tab', { name: '应用账号 · 本批', exact: true }).click()
    await page.getByRole('button', { name: '新建应用账号', exact: true }).click(); await fill('账号名称', 'ui_app_' + run)
    const app = await mutate('POST', '/api/v1/principals', () => drawer().getByRole('button', { name: '保存账号', exact: true }).click())
    report.created.applicationId = app.data.principalId; expect(app.data.type).toBe('APPLICATION'); await expect(drawer()).toHaveCount(0)
    await page.locator('.el-table__row').filter({ hasText: 'ui_app_' + run }).getByRole('button', { name: '管理与凭据', exact: true }).click()
    await drawer().getByRole('tab', { name: '应用凭据', exact: true }).click(); await fill('用途标签', 'UI 一次令牌测试')
    const issued = await mutate('POST', '/api/v1/principals/' + app.data.principalId + '/credentials', () => drawer().getByRole('button', { name: '签发一次性令牌', exact: true }).click())
    applicationToken = issued.data.token; report.created.credentialId = issued.data.credentialId
    expect(Date.parse(issued.data.expiresAt) > Date.now()).toBe(true)
    expect(Date.parse(issued.data.expiresAt) < Date.now() + 366 * 86400000).toBe(true)
    await expect(drawer().locator('.token-text')).toBeVisible()
    expect((await drawer().locator('.token-text').textContent()) === applicationToken).toBe(true)
    const tokenContext = await browser.newContext({ baseURL: base, extraHTTPHeaders: { Authorization: 'Bearer ' + applicationToken } })
    const accepted = await tokenContext.request.get('/api/v1/auth/me'); expect(accepted.status()).toBe(200)
    expect((await accepted.json()).principalId).toBe(app.data.principalId)
    await drawer().getByRole('button', { name: '已保存，清除明文', exact: true }).click(); await expect(page.locator('.token-text')).toHaveCount(0)
    await page.reload(); await page.getByRole('tab', { name: '应用账号 · 本批', exact: true }).click()
    await page.locator('.el-table__row').filter({ hasText: 'ui_app_' + run }).getByRole('button', { name: '管理与凭据', exact: true }).click()
    await drawer().getByRole('tab', { name: '应用凭据', exact: true }).click(); await expect(page.locator('.token-text')).toHaveCount(0)
    const metadata = await read('/api/v1/principals/' + app.data.principalId + '/credentials')
    expect(metadata.items.some(item => Object.hasOwn(item, 'token'))).toBe(false)
    const item = drawer().locator('.el-table__row').filter({ hasText: issued.data.credentialId })
    await mutate('DELETE', '/api/v1/principals/' + app.data.principalId + '/credentials/' + issued.data.credentialId, async () => {
      await item.getByRole('button', { name: '撤销', exact: true }).click()
      await page.getByRole('dialog').filter({ hasText: '撤销应用令牌' }).getByRole('button', { name: '撤销', exact: true }).click()
    })
    await expect(item).toContainText('已撤销'); const denied = await tokenContext.request.get('/api/v1/auth/me'); expect(denied.status()).toBe(401)
    await tokenContext.close(); applicationToken = ''
  })
  await step('生成实际诊断作业并下载非空 ZIP', async () => {
    await page.goto('/ui/system'); await page.getByRole('tab', { name: '诊断与作业', exact: true }).click()
    await fill('导出原因', 'UI integration ' + run, page)
    const result = await mutate('POST', '/api/v1/maintenance/jobs', () => page.getByRole('button', { name: '生成诊断包', exact: true }).click())
    report.created.operationId = result.data.operationId; expect(result.data.state).toBe('SUCCEEDED')
    expect(result.data.result.containsRawPayload).toBe(false)
    const wait = page.waitForEvent('download'); await page.getByRole('link', { name: '下载已完成诊断包', exact: true }).click(); const download = await wait
    const file = path.join(evidence, 'diagnostic.zip'); await download.saveAs(file)
    const bytes = await fs.readFile(file); expect(bytes.length).toBeGreaterThan(100); expect(bytes.subarray(0, 4).toString('hex')).toBe('504b0304')
    await page.screenshot({ path: path.join(evidence, 'system-diagnostic.png'), fullPage: true })
  })
  await step('审计时间查询与真实记录详情', async () => {
    await page.goto('/ui/audit'); await expect(page.locator('.el-table__row').first()).toBeVisible()
    const queryResponse = page.waitForResponse(r => new URL(r.url()).pathname === '/api/v1/audit-events')
    await page.getByRole('button', { name: '查询时间区间', exact: true }).click(); const response = await queryResponse
    expect(response.status()).toBe(200); const url = new URL(response.url()); expect(!!url.searchParams.get('from') && !!url.searchParams.get('to')).toBe(true)
    let batch = await response.json(), pages = 1
    const matching = e => e.action === 'diagnostic.export' && e.resourceId === report.created.operationId
    while (!batch.items.some(matching) && batch.nextCursor && pages < 101) {
      expect(batch.items.every(e => ['SUCCESS', 'DENIED', 'FAILED'].includes(e.outcome))).toBe(true)
      const pending = page.waitForResponse(r => new URL(r.url()).pathname === '/api/v1/audit-events' && new URL(r.url()).searchParams.has('cursor'))
      await page.getByRole('button', { name: '下一批', exact: true }).click(); const next = await pending; expect(next.ok()).toBe(true); batch = await next.json(); pages++
    }
    expect(batch.items.some(matching)).toBe(true)
    expect(batch.items.every(e => ['SUCCESS', 'DENIED', 'FAILED'].includes(e.outcome))).toBe(true)
    report.auditPages = pages
    await page.locator('.el-table__row').filter({ hasText: report.created.operationId }).click()
    await expect(drawer().getByText('记录的变更', { exact: true })).toBeVisible()
    await page.screenshot({ path: path.join(evidence, 'audit-detail.png'), fullPage: true })
  })
  expect(report.pageErrors).toEqual([]); report.passed = true
} catch (e) { failure = e; report.error = sanitize(e.message) }
finally { report.finishedAt = new Date().toISOString(); await fs.writeFile(path.join(evidence, 'report.json'), JSON.stringify(report, null, 2)); await browser.close() }
if (failure) throw new Error(report.error)
console.log('System UI integration passed; evidence: ' + evidence)
