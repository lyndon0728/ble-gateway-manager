import { chromium, expect } from '@playwright/test'
import fs from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..')
const fixtureFile = process.env.GW_TEST_FIXTURE || path.join(root, 'server/.local/frontend-e2e/fixture.json')
const fixture = await fs.readFile(fixtureFile, 'utf8').then(s => JSON.parse(s.replace(/^\uFEFF/, ''))).catch(error => { if (error.code === 'ENOENT' && !process.env.GW_TEST_FIXTURE) return {}; throw error })
const base = process.env.GW_TEST_URL || fixture.baseUrl || 'http://127.0.0.1:18781'
const passwordFile = process.env.GW_TEST_PASSWORD_FILE || fixture.passwordFile
if (!passwordFile) throw new Error('GW_TEST_PASSWORD_FILE must point to the isolated test instance password file')
const gatewayId = process.env.GW_TEST_GATEWAY || fixture.gatewayId || 'gw32_ui'
const runId = Date.now().toString(16)
const password = (await fs.readFile(passwordFile, 'utf8')).trim()
const evidence = path.resolve(process.env.GW_TEST_EVIDENCE || path.join(root, 'server/.testdata/frontend-gateway-e2e'))
await fs.mkdir(evidence, { recursive: true })
const browser = await chromium.launch({ channel: process.env.GW_TEST_BROWSER || 'chrome', headless: true })
const context = await browser.newContext({ baseURL: base, viewport: { width: 1500, height: 1100 } })
const page = await context.newPage()
page.setDefaultTimeout(15000)
const errors = []
page.on('pageerror', error => errors.push(error.message))
const report = { passed: false, gatewayId, at: new Date().toISOString(), tasks: [], checks: [], browser: 'Chromium', physicalHardwareValidated: false }
const exact = value => new RegExp('^' + [...value].map(c => '\\^$.*+?()[]{}|'.includes(c) ? '\\' + c : c).join('') + '$')
const field = (label, scope = page) => scope.locator('.el-form-item').filter({ has: page.locator('.el-form-item__label').filter({ hasText: exact(label) }) })
async function select(label, option, scope = page) {
  await field(label, scope).locator('.el-select').click()
  await page.getByRole('option', { name: option, exact: true }).click()
  await page.keyboard.press('Escape')
}
async function fill(label, value, scope = page) { await field(label, scope).locator('input:not([type=checkbox]),textarea').first().fill(String(value)) }
async function checkLabel(scope, label) {
  const control = scope.locator('label.el-checkbox').filter({ hasText: exact(label) })
  if (!(await control.locator('input[type=checkbox]').isChecked())) await control.click()
}
async function getJson(url) {
  const response = await page.request.get(url)
  if (!response.ok()) throw new Error(`${response.status()} ${url}: ${await response.text()}`)
  return response.json()
}
async function waitTask(task) {
  let current = task
  const deadline = Date.now() + 25000
  while (Date.now() < deadline) {
    current = await getJson('/api/v1/tasks/' + task.taskId)
    if (current.state === 'SUCCEEDED') { report.tasks.push({ taskId: current.taskId, type: current.type, state: current.state, verification: current.result?.verification }); return current }
    if (['FAILED', 'EXPIRED', 'CANCELLED', 'UNKNOWN'].includes(current.state)) throw new Error(JSON.stringify(current))
    await new Promise(resolve => setTimeout(resolve, 250))
  }
  throw new Error('Task did not complete: ' + JSON.stringify(current))
}
async function mutate(button, method, endpoint, confirmation) {
  const responsePromise = page.waitForResponse(response => response.request().method() === method && new URL(response.url()).pathname === endpoint)
  await button.click()
  if (confirmation) await page.getByRole('dialog').filter({ has: page.getByRole('button', { name: confirmation, exact: true }) }).getByRole('button', { name: confirmation, exact: true }).click()
  const response = await responsePromise
  if (!response.ok()) throw new Error(`Mutation failed ${response.status()}: ${await response.text()}`)
  return response.json()
}
async function configureScan() {
  await page.goto('/ui/gateways/' + gatewayId + '/configuration')
  await page.getByRole('menuitem', { name: 'nRF 扫描与过滤', exact: true }).click()
  const enabled = field('启用扫描')
  await checkLabel(enabled, '本次修改')
  if (await enabled.getByRole('switch').getAttribute('aria-checked') !== 'true') await enabled.locator('.el-switch__core').click()
  for (const [name, value] of [['扫描间隔 ms', 100], ['扫描窗口 ms', 50]]) {
    const group = field(name); await checkLabel(group, '本次修改'); await group.getByRole('spinbutton').fill(String(value))
  }
  const result = await mutate(page.getByRole('button', { name: '提交配置任务', exact: true }), 'PUT', '/api/v1/gateways/' + gatewayId + '/configuration', '创建配置任务')
  await waitTask(result)
  const stored = await getJson('/api/v1/tasks/' + result.taskId + '/request')
  expect(Object.keys(stored.request.parameters.configuration)).toEqual(['radios'])
  expect(stored.request.parameters.configuration.radios.nrf54.scan).toEqual({ enabled: true, intervalMs: 100, windowMs: 50 })
  report.checks.push('selected scan fields only; no unknown defaults submitted')
}
async function configureNetwork() {
  await page.goto('/ui/gateways/' + gatewayId + '/configuration')
  const order = field('上行优先顺序（选中顺序即优先级，整体替换）')
  await checkLabel(order, '本次修改')
  const ethernet = field('以太网')
  await checkLabel(ethernet, '本次修改启用状态')
  if (await ethernet.getByRole('switch').getAttribute('aria-checked') !== 'true') await ethernet.locator('.el-switch__core').click()
  const ip = page.locator('fieldset').filter({ has: page.getByText('以太网 IPv4', { exact: true }) })
  await checkLabel(ip, '整体替换此 IPv4 对象')
  const result = await mutate(page.getByRole('button', { name: '提交配置任务', exact: true }), 'PUT', '/api/v1/gateways/' + gatewayId + '/configuration', '创建配置任务')
  const completed = await waitTask(result)
  expect(completed.result.verification).toBe('VERIFIED')
  await page.getByRole('button', { name: '查看执行结果', exact: true }).click()
  await page.getByRole('tab', { name: '持久化结果历史', exact: true }).click()
  const history = await getJson('/api/v1/tasks/' + result.taskId + '/results')
  expect(history.items.some(v => v.result?.details?.networkProbe?.interface === 'ETHERNET')).toBe(true)
  await expect(page.getByText('服务器接收时间', { exact: true })).toBeVisible()
  await page.screenshot({ path: path.join(evidence, 'task-network-history.png'), fullPage: true })
  report.checks.push('network configuration waits for durable probe and VERIFIED result; history visible')
}
async function openConnection(connectionRef) {
  await page.goto('/ui/gatt-connections?gatewayId=' + encodeURIComponent(gatewayId))
  if (connectionRef) await page.getByRole('row').filter({ hasText: connectionRef }).click()
  return page.locator('aside.panel')
}
async function gattAction(connectionRef, action, extra) {
  const aside = await openConnection(connectionRef)
  await select('动作', action, aside)
  if (!['持久连接', '断开'].includes(action)) { await fill('服务 UUID', '12345678-1234-5678-9abc-def012345678', aside); await fill('特征 UUID', '12345678-1234-5678-9abc-def012345679', aside) }
  if (extra) await extra(aside)
  const confirmation = action === '写入' ? '创建写入任务' : action === '断开' ? '确定' : undefined
  const task = await mutate(aside.getByRole('button', { name: '创建操作任务', exact: true }), 'POST', '/api/v1/tasks', confirmation)
  return waitTask(task)
}
async function writeAfterLostResponse(connectionRef) {
  const aside = await openConnection(connectionRef)
  await select('动作', '写入', aside)
  await fill('服务 UUID', '12345678-1234-5678-9abc-def012345678', aside)
  await fill('特征 UUID', '12345678-1234-5678-9abc-def012345679', aside)
  await field('写入值', aside).locator('textarea').fill('2A')
  const requests = []; let accepted
  await page.route('**/api/v1/tasks', async route => {
    if (route.request().method() !== 'POST') return route.continue()
    requests.push({ key: route.request().headers()['idempotency-key'], body: route.request().postDataJSON() })
    if (requests.length === 1) {
      const response = await route.fetch()
      if (!response.ok()) throw new Error('Lost-response setup failed: ' + await response.text())
      accepted = await response.json()
      return route.abort('failed')
    }
    return route.continue()
  })
  try {
    await aside.getByRole('button', { name: '创建操作任务', exact: true }).click()
    await page.getByRole('dialog').getByRole('button', { name: '创建写入任务', exact: true }).click()
    await expect(page.locator('.el-alert--error')).toContainText('NETWORK_UNCERTAIN')
    expect(requests).toHaveLength(1)
    const retry = await mutate(aside.getByRole('button', { name: '创建操作任务', exact: true }), 'POST', '/api/v1/tasks', '创建写入任务')
    expect(requests).toHaveLength(2)
    expect(requests[1]).toEqual(requests[0])
    expect(retry.taskId).toBe(accepted.taskId)
    await waitTask(retry)
    report.checks.push('lost write response has no automatic retry; explicit retry preserves exact body/key and original task')
  } finally { await page.unroute('**/api/v1/tasks') }
}
try {
  await page.goto('/ui/login')
  await page.getByLabel('账号', { exact: true }).fill(process.env.GW_TEST_USERNAME || 'admin')
  await page.getByLabel('密码', { exact: true }).fill(password)
  await page.getByRole('button', { name: '登录', exact: true }).click()
  await expect(page).not.toHaveURL(/\/login/)
  const gateway = await getJson('/api/v1/gateways/' + gatewayId)
  expect(gateway.capabilities).toContain('GW32_CONTROL_PROFILE_1')
  await configureScan()
  await configureNetwork()

  await page.goto('/ui/ble-peers')
  await page.getByRole('button', { name: '登记对端', exact: true }).click()
  const drawer = page.getByRole('dialog')
  const peerName = 'UI 对端 ' + runId
  await fill('对端名称', peerName, drawer)
  await fill('蓝牙地址', 'C0' + runId.slice(-10).padStart(10, '0'), drawer)
  const scopePage = await getJson('/api/v1/resource-scopes?limit=200')
  const scope = scopePage.items.find(v => v.scopeId === gateway.groupId)
  if (!scope) throw new Error('Test gateway group not visible')
  await select('资源范围', scope.name, drawer)
  const peer = await mutate(drawer.getByRole('button', { name: '保存对端', exact: true }), 'POST', '/api/v1/ble-peers')
  await expect(page.getByText(peerName, { exact: true })).toBeVisible()

  const connected = await gattAction('', '持久连接', aside => fill('对端地址', 'AABBCCDDEEFF', aside))
  const connectionRef = connected.result.details.connectionRef
  expect(connectionRef).toMatch(/^[0-9a-f]{32}$/)
  const firstRead = await gattAction(connectionRef, '读取')
  // Re-select within the same mounted component, so this catches a stale in-memory intent.
  await page.getByRole('row').filter({ hasText: connectionRef }).click()
  const nextRead = await waitTask(await mutate(page.locator('aside.panel').getByRole('button', { name: '创建操作任务', exact: true }), 'POST', '/api/v1/tasks'))
  expect(nextRead.taskId).not.toBe(firstRead.taskId)
  report.checks.push('two explicitly requested identical reads create two separate task identities')
  await writeAfterLostResponse(connectionRef)
  await gattAction(connectionRef, '订阅')
  let subscriptions = await getJson('/api/v1/gatt-subscriptions?gatewayId=' + gatewayId + '&connectionRef=' + connectionRef)
  expect(subscriptions.items.some(v => v.state === 'ACTIVE')).toBe(true)
  await openConnection(connectionRef)
  await page.screenshot({ path: path.join(evidence, 'gatt-active-subscription.png'), fullPage: true })
  await gattAction(connectionRef, '取消订阅')
  subscriptions = await getJson('/api/v1/gatt-subscriptions?gatewayId=' + gatewayId + '&connectionRef=' + connectionRef)
  expect(subscriptions.items.every(v => v.state === 'CLOSED')).toBe(true)
  await gattAction(connectionRef, '断开')
  const connections = await getJson('/api/v1/gatt-connections?gatewayId=' + gatewayId)
  expect(connections.items.find(v => v.connectionRef === connectionRef)?.state).toBe('CLOSED')
  report.checks.push('GATT connect/read/write/readback/subscribe/unsubscribe/disconnect task chain')

  await page.goto('/ui/auto-connect-rules')
  await page.getByRole('button', { name: '新建规则', exact: true }).click()
  const ruleDrawer = page.getByRole('dialog')
  const ruleName = 'UI 规则 ' + runId
  await fill('规则名称', ruleName, ruleDrawer)
  await select('注册对端', peerName + ' · ' + peer.deviceAddress, ruleDrawer)
  await select('允许自动连接的网关', gateway.displayName || gateway.gatewayId, ruleDrawer)
  await ruleDrawer.getByRole('tab', { name: '匹配条件', exact: true }).click()
  await fill('服务 UUID', '180f', ruleDrawer)
  await ruleDrawer.getByRole('tab', { name: '执行条件', exact: true }).click()
  const rule = await mutate(ruleDrawer.getByRole('button', { name: '保存规则', exact: true }), 'POST', '/api/v1/auto-connect-rules')
  expect(rule.enabled).toBe(false)
  const row = page.getByRole('row').filter({ hasText: ruleName })
  await expect(row.getByText('已停用', { exact: true })).toBeVisible()
  const enabled = await mutate(row.getByRole('button', { name: '启用', exact: true }), 'PATCH', '/api/v1/auto-connect-rules/' + rule.ruleId, '确定')
  expect(enabled.enabled).toBe(true)
  const disabled = await mutate(row.getByRole('button', { name: '停用', exact: true }), 'PATCH', '/api/v1/auto-connect-rules/' + rule.ruleId, '确定')
  expect(disabled.enabled).toBe(false)
  expect(disabled.revision).toBe(3)
  report.checks.push('peer creation and revision-protected automatic-rule create/enable/disable')
  await page.screenshot({ path: path.join(evidence, 'automatic-rules.png'), fullPage: true })
  expect(errors).toEqual([])
  report.passed = true
} catch (error) {
  report.error = error.stack || String(error)
  report.pageErrors = errors
  await page.screenshot({ path: path.join(evidence, 'failure.png'), fullPage: true }).catch(() => {})
  process.exitCode = 1
} finally {
  await fs.writeFile(path.join(evidence, 'gateway-report.json'), JSON.stringify(report, null, 2))
  console.log(JSON.stringify(report, null, 2))
  await browser.close()
}
