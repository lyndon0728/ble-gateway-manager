import { chromium, expect } from '@playwright/test'
import { readFile, mkdir, writeFile } from 'node:fs/promises'
import { resolve } from 'node:path'
import { randomUUID } from 'node:crypto'

// Run only against the dedicated local UI test instance: this creates target,
// rule and reprocessing versions, and activates a copy of its current rules.
const fixture=JSON.parse((await readFile(process.env.GW_TEST_FIXTURE || resolve('../server/.local/frontend-e2e/fixture.json'),'utf8')).trim())
const base=(process.env.GW_TEST_URL || fixture.baseUrl || 'http://127.0.0.1:18781').replace(/\/$/,'')
if(!['127.0.0.1','localhost','::1'].includes(new URL(base).hostname))throw new Error('Pipeline E2E requires a local test instance')
const passwordFile=process.env.GW_TEST_PASSWORD_FILE || fixture.passwordFile
if(!passwordFile)throw new Error('GW_TEST_PASSWORD_FILE or GW_TEST_FIXTURE is required')
const password=(await readFile(passwordFile,'utf8')).trim()
if(!password)throw new Error('Test password file is empty')
const account=process.env.GW_TEST_USER || 'admin',gatewayId=process.env.GW_TEST_GATEWAY_ID || fixture.gatewayId || 'gw32_ui'
const receiver=process.env.GW_TEST_RECEIVER_URL || (fixture.customerUrl?fixture.customerUrl+'/events':'http://127.0.0.1:19191/events')
const reportDirectory=resolve(process.env.GW_TEST_REPORT_DIR || 'test-results')
await mkdir(reportDirectory,{recursive:true})
const report={startedAt:new Date().toISOString(),base,gatewayId,steps:[],requests:[],pageErrors:[],consoleErrors:[],created:{},limitations:[]}
const browser=await chromium.launch({headless:true,executablePath:process.env.GW_TEST_CHROME || 'C:/Program Files/Google/Chrome/Application/chrome.exe'})
const context=await browser.newContext({viewport:{width:1440,height:1050},locale:'zh-CN'})
const page=await context.newPage()
page.setDefaultTimeout(15000)
const secretValues=[password]
const sanitize=value=>secretValues.reduce((text,secret)=>text.split(secret).join('[REDACTED]'),String(value))
page.on('pageerror',error=>report.pageErrors.push(sanitize(error.message)))
page.on('console',message=>{if(message.type()==='error')report.consoleErrors.push(sanitize(message.text()))})
page.on('response',response=>{const url=new URL(response.url());if(url.origin===new URL(base).origin&&url.pathname.startsWith('/api/'))report.requests.push({method:response.request().method(),path:url.pathname,status:response.status()})})
const formItem=(container,label)=>container.locator('.el-form-item').filter({has:page.locator('.el-form-item__label').filter({hasText:label})}).first()
const input=(container,label)=>formItem(container,label).locator('input,textarea').first()
const drawer=()=>page.locator('.el-drawer:visible').last()
async function navigate(path){await page.goto(`${base}/ui${path}`);await expect(page.locator('h1').first()).toBeVisible()}
async function read(path){const response=await context.request.get(`${base}/api/v1${path}`);if(!response.ok())throw new Error(`Read ${path}: HTTP ${response.status()}`);return response.json()}
async function mutation(path,action){const pending=page.waitForResponse(r=>new URL(r.url()).pathname===`/api/v1${path}`&&r.request().method()==='POST');await action();const response=await pending;let data;try{data=await response.json()}catch{throw new Error(`${path}: HTTP ${response.status()}, non-JSON response`)}if(!response.ok())throw new Error(`${path}: ${response.status()} ${data.error?.code || data.code || ''} ${data.error?.message || data.message || ''}`);return data}
async function step(name,fn){const started=Date.now();try{const result=await fn();report.steps.push({name,status:'passed',durationMs:Date.now()-started});console.log(`PASS ${name}`);return result}catch(error){report.steps.push({name,status:'failed',durationMs:Date.now()-started,error:sanitize(error.message)});throw error}}
async function choose(container,label,text){await formItem(container,label).locator('.el-select').click();await page.locator('.el-select-dropdown:visible .el-select-dropdown__item').filter({hasText:text}).first().click()}
let failed
try{
  await step('登录真实本地实例',async()=>{await page.goto(`${base}/ui/login`);await page.locator('#login-name').fill(account);await page.locator('#login-password').fill(password);await page.getByRole('button',{name:'登录',exact:true}).click();await expect(page).not.toHaveURL(/\/login/);await read('/auth/me')})
  const system=await read('/system/status'),activeBundleId=system.dataNodes?.find(n=>n.activeBundleId)?.activeBundleId
  if(!activeBundleId)throw new Error('Seed requires an active rule bundle before this test')
  const existingBundle=await read(`/rule-bundles/${encodeURIComponent(activeBundleId)}`)
  if(!existingBundle.rules?.length)throw new Error('Seed active bundle has no rules')

  await step('温湿度与 BLE UUID 解析器真实试算',async()=>{
    await navigate('/parsers')
    for(const sample of ['温湿度','16 位 UUID','32 位 UUID','128 位 UUID']){
      await page.getByRole('button',{name:sample,exact:true}).click()
      const pending=page.waitForResponse(r=>/\/parsers\/[^/]+\/versions\/[^/]+\/tests$/.test(new URL(r.url()).pathname)&&r.request().method()==='POST')
      await page.getByRole('button',{name:'运行试算',exact:true}).click();const response=await pending,data=await response.json()
      expect(response.status()).toBe(200);expect(data.passed).toBe(true);expect(data.outputs.length).toBeGreaterThan(0)
      await expect(page.getByRole('heading',{name:'试算结果',exact:true})).toBeVisible()
    }
  })

  await step('修改试算输入清除旧结果并拒绝迟到响应',async()=>{
    const resultHeading=page.getByRole('heading',{name:'试算结果',exact:true})
    await expect(resultHeading).toBeVisible();await input(page,'报文内容').fill('00');await expect(resultHeading).toHaveCount(0)
    await page.getByRole('button',{name:'温湿度',exact:true}).click()
    const parserPath='/api/v1/parsers/demo_sensor/versions/1.0.0/tests'
    let readyResolve,readyReject
    const ready=new Promise((resolve,reject)=>{readyResolve=resolve;readyReject=reject})
    const delayResponse=async route=>{try{const response=await route.fetch();readyResolve();await new Promise(resolve=>setTimeout(resolve,800));await route.fulfill({response})}catch(error){readyReject(error);await route.abort('failed')}}
    await page.route(`**${parserPath}`,delayResponse)
    try{const returned=page.waitForResponse(r=>new URL(r.url()).pathname===parserPath);await page.getByRole('button',{name:'运行试算',exact:true}).click();await ready;await input(page,'报文内容').fill('00');await returned;await expect(page.getByRole('button',{name:'运行试算',exact:true})).toBeEnabled();await expect(resultHeading).toHaveCount(0)}finally{await page.unroute(`**${parserPath}`,delayResponse)}
  })

  await step('通过表单创建 HTTP 目标',async()=>{
    await navigate('/targets');await page.getByRole('button',{name:'新增目标',exact:true}).click()
    await input(drawer(),'目标名称').fill(`UI pipeline ${Date.now()}`)
    await input(drawer(),'接收地址').fill(receiver)
    const target=await mutation('/targets',()=>drawer().getByRole('button',{name:'创建目标',exact:true}).click())
    report.created.targetId=target.targetId;expect(target.versions[0].configuration.mappingVersion).toBe('identity-1')
    await expect(page).toHaveURL(new RegExp(`/ui/targets/${target.targetId}$`))
  })

  await step('复制目标版本、编辑映射、预览并发布',async()=>{
    await page.getByRole('button',{name:'复制为新版本',exact:true}).click()
    await formItem(drawer(),'映射模式').locator('.el-switch').click()
    const field=drawer().locator('.mapping-row').first()
    await input(field,'源字段').fill('temperature');await input(field,'目标字段').fill('temperatureC');await input(field,'单位').fill('℃')
    await field.locator('.el-checkbox').click();await expect(field.getByRole('checkbox')).toBeChecked()
    await input(field,'倍率').fill('1');await input(field,'偏移量').fill('0')
    await drawer().getByText('本地映射样本预览（不发送客户平台）',{exact:true}).click()
    await drawer().getByRole('button',{name:'预览映射',exact:true}).click()
    await expect(drawer().locator('.el-table').getByText('temperatureCUnit',{exact:true})).toBeVisible()
    const version=await mutation(`/targets/${report.created.targetId}/versions`,async()=>{await drawer().getByRole('button',{name:'发布新版本',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'发布新版本',exact:true}).click()})
    report.created.targetVersion=version.versions[0].version
    expect(version.versions[0].configuration.mapping.fields[0]).toMatchObject({source:'temperature',target:'temperatureC',scale:1,offset:0,unit:'℃'})
    await expect(page.locator('.el-drawer:visible')).toHaveCount(0)
  })

  await step('MQTT 版本表单与仅引用密钥',async()=>{
    await page.getByRole('button',{name:'复制为新版本',exact:true}).click()
    await drawer().locator('.el-radio-button').filter({hasText:'MQTT'}).click()
    await input(drawer(),'接收地址').fill('mqtts://127.0.0.1:18883')
    await input(drawer(),'数据主题').fill('ui-test/events');await input(drawer(),'应用确认主题').fill('ui-test/acks')
    await formItem(drawer(),'MQTT 凭据引用').getByRole('button',{name:'新建',exact:true}).click()
    const dialog=page.locator('.el-dialog:visible').last(),testSecret=`ui-only-${randomUUID()}`;secretValues.push(testSecret)
    await input(dialog,'名称').fill('UI synthetic MQTT credentials');await input(dialog,'MQTT 用户名').fill('ui-test');await input(dialog,'MQTT 密码').fill(testSecret)
    const secret=await mutation('/secrets',()=>dialog.getByRole('button',{name:'保存并引用',exact:true}).click())
    expect(secret.secretRef).toBeTruthy();expect(secret).not.toHaveProperty('value')
    await expect(input(drawer(),'MQTT 凭据引用')).toHaveValue(secret.secretRef)
    const version=await mutation(`/targets/${report.created.targetId}/versions`,async()=>{await drawer().getByRole('button',{name:'发布新版本',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'发布新版本',exact:true}).click()})
    expect(version.versions[0].configuration).toMatchObject({protocol:'MQTT',ackMode:'MQTT_APPLICATION_ACK',payloadTopic:'ui-test/events',ackTopic:'ui-test/acks',secretRef:secret.secretRef})
    report.created.mqttVersion=version.versions[0].version
    expect(await page.evaluate(secret=>Object.values({...localStorage,...sessionStorage}).some(value=>String(value).includes(secret)),testSecret)).toBe(false)
    report.limitations.push('MQTT 新版本仅验证配置与密钥表单；未将该测试版本绑定到规则，不代表本次浏览器用例验证 MQTT Broker 传输。')
  })

  await step('规则复制、乱序详情保护与激活丢响应幂等恢复',async()=>{
    await navigate('/rule-bundles')
    await page.getByRole('button',{name:activeBundleId,exact:true}).click()
    await page.getByRole('button',{name:'复制为新包',exact:true}).click()
    await input(drawer(),'本次名称').fill(`UI verified copy ${Date.now()}`)
    const bundle=await mutation('/rule-bundles',()=>drawer().getByRole('button',{name:'保存新规则包',exact:true}).click())
    report.created.bundleId=bundle.bundleId;expect(bundle.rules).toEqual(existingBundle.rules)
    const bundleA=`/api/v1/rule-bundles/${activeBundleId}`,bundleB=`/api/v1/rule-bundles/${bundle.bundleId}`
    const reorder=async route=>{const response=await route.fetch();await new Promise(resolve=>setTimeout(resolve,new URL(route.request().url()).pathname===bundleA?1000:50));await route.fulfill({response})}
    await page.route(`**${bundleA}`,reorder);await page.route(`**${bundleB}`,reorder)
    try{const aStarted=page.waitForRequest(r=>new URL(r.url()).pathname===bundleA&&r.method()==='GET'),aReturned=page.waitForResponse(r=>new URL(r.url()).pathname===bundleA&&r.request().method()==='GET'),bReturned=page.waitForResponse(r=>new URL(r.url()).pathname===bundleB&&r.request().method()==='GET');await page.getByRole('button',{name:activeBundleId,exact:true}).click();await aStarted;await page.getByRole('button',{name:bundle.bundleId,exact:true}).click();await Promise.all([aReturned,bReturned]);await expect(page.locator('section.panel').filter({has:page.getByRole('heading',{name:'规则包详情',exact:true})}).locator('p.mono').first()).toHaveText(bundle.bundleId)}finally{await page.unroute(`**${bundleA}`,reorder);await page.unroute(`**${bundleB}`,reorder)}
    const operation=await mutation(`/rule-bundles/${bundle.bundleId}/validations`,()=>page.getByRole('button',{name:'验证',exact:true}).click());expect(operation.state).toBe('SUCCEEDED')
    await expect(page.getByRole('button',{name:'激活',exact:true})).toBeEnabled()
    const activationPath=`/rule-bundles/${bundle.bundleId}/activations`,requestCopies=[]
    let firstCommitted,drop=true,resolveDrop,rejectDrop
    const dropped=new Promise((resolve,reject)=>{resolveDrop=resolve;rejectDrop=reject})
    const intercept=async route=>{
      requestCopies.push({body:route.request().postDataJSON(),key:route.request().headers()['idempotency-key'],etag:route.request().headers()['if-match']})
      if(drop){drop=false;try{const response=await route.fetch();firstCommitted=await response.json();if(!response.ok())throw new Error(`Initial activation HTTP ${response.status()}`);await route.abort('failed');resolveDrop()}catch(error){rejectDrop(error);await route.abort('failed')}}else await route.continue()
    }
    await page.route(`**/api/v1${activationPath}`,intercept)
    try{
      await page.getByRole('button',{name:'激活',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'确认激活',exact:true}).click();await dropped
      await expect(page.locator('.el-alert').filter({hasText:'NETWORK_UNCERTAIN'})).toBeVisible()
      const committedStatus=await read('/system/status'),committedNode=committedStatus.dataNodes.find(n=>n.activeBundleId===bundle.bundleId);expect(committedNode).toBeTruthy()
      const activation=await mutation(activationPath,async()=>{await page.getByRole('button',{name:'激活',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'确认激活',exact:true}).click()});expect(activation.state).toBe('SUCCEEDED');expect(activation.operationId).toBe(firstCommitted.operationId)
      expect(requestCopies).toHaveLength(2);expect(requestCopies[1]).toEqual(requestCopies[0])
      const status=await read('/system/status');expect(status.dataNodes.find(n=>n.activeBundleId===bundle.bundleId)?.activationRevision).toBe(committedNode.activationRevision)
      report.created.activationRecoveredOperationId=activation.operationId
    }finally{await page.unroute(`**/api/v1${activationPath}`,intercept)}
  })

  let event
  await step('事件详情读取真实原文与持久化输出',async()=>{
    await expect.poll(async()=>{const items=(await read(`/events?gatewayId=${encodeURIComponent(gatewayId)}&limit=200`)).items;event=items.find(e=>e.rawAvailable&&e.processing?.some(p=>p.outputIds?.length));return !!event},{timeout:45000}).toBe(true)
    await navigate(`/events?gatewayId=${encodeURIComponent(gatewayId)}`)
    await page.getByRole('button',{name:event.eventId,exact:true}).first().click()
    await expect(page.getByRole('heading',{name:'事件详情',exact:true})).toBeVisible()
    const outputId=event.processing.find(p=>p.outputIds?.length).outputIds[0]
    const pending=page.waitForResponse(r=>new URL(r.url()).pathname===`/api/v1/outputs/${outputId}`)
    await page.getByRole('button',{name:outputId,exact:true}).click();const output=await (await pending).json();expect(output.payloadAvailable).toBe(true);expect(output.record).toBeTruthy()
    await expect(drawer().getByText('输出 ID',{exact:true})).toBeVisible();await drawer().getByRole('button',{name:/关闭此对话框|Close this dialog/}).click()
    await page.getByRole('tab',{name:'原始报文',exact:true}).click();await page.getByRole('button',{name:'读取原文',exact:true}).click()
    await expect(page.getByRole('button',{name:'使用原文试算',exact:true})).toBeVisible()
  })

  await step('基于事件生成重解析预览并创建作业',async()=>{
    await page.getByRole('button',{name:'重解析预览',exact:true}).click()
    await expect(drawer()).toBeVisible();await choose(drawer(),'已经验证的规则包',report.created.bundleId)
    const preview=await mutation('/reprocess-previews',()=>drawer().getByRole('button',{name:'生成预览',exact:true}).click())
    expect(preview.matchedCount).toBeGreaterThan(0);expect(preview.missingRawCount).toBe(0)
    await input(drawer(),'操作原因').fill('UI end-to-end validation on local test instance')
    await drawer().locator('.el-checkbox').filter({hasText:'已确认新增处理修订'}).click();await expect(drawer().getByRole('checkbox',{name:/已确认新增处理修订/})).toBeChecked()
    const job=await mutation('/reprocess-jobs',async()=>{await drawer().getByRole('button',{name:'确认创建重解析作业',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'创建作业',exact:true}).click()})
    report.created.reprocessJobId=job.jobId;expect(job.selectedCount).toBe(preview.matchedCount)
    await expect(page.getByRole('tab',{name:'我的重解析作业',exact:true})).toHaveAttribute('aria-selected','true')
    await expect.poll(async()=>(await read(`/reprocess-jobs/${job.jobId}`)).state,{timeout:45000}).toMatch(/SUCCEEDED|COMPLETED/)
    await page.getByRole('button',{name:'刷新列表',exact:true}).click();await page.getByRole('button',{name:job.jobId,exact:true}).click()
  })

  await step('同一事件输出抽屉拒绝关闭前的迟到结果',async()=>{
    const current=await read(`/gateways/${encodeURIComponent(gatewayId)}/events/${encodeURIComponent(event.eventId)}`),ids=current.processing.flatMap(p=>p.outputIds)
    expect(ids.length).toBeGreaterThanOrEqual(2)
    await navigate(`/gateways/${encodeURIComponent(gatewayId)}/events/${encodeURIComponent(event.eventId)}`)
    const outputA=`/api/v1/outputs/${ids[0]}`,outputB=`/api/v1/outputs/${ids[1]}`
    const reorder=async route=>{const response=await route.fetch();await new Promise(resolve=>setTimeout(resolve,new URL(route.request().url()).pathname===outputA?1800:50));await route.fulfill({response})}
    await page.route(`**${outputA}`,reorder);await page.route(`**${outputB}`,reorder)
    try{const aStarted=page.waitForRequest(r=>new URL(r.url()).pathname===outputA),aReturned=page.waitForResponse(r=>new URL(r.url()).pathname===outputA),bReturned=page.waitForResponse(r=>new URL(r.url()).pathname===outputB);await page.getByRole('button',{name:ids[0],exact:true}).click();await aStarted;await drawer().getByRole('button',{name:/关闭此对话框|Close this dialog/}).click();await expect(page.locator('.el-drawer:visible')).toHaveCount(0);await page.getByRole('button',{name:ids[1],exact:true}).click();await Promise.all([aReturned,bReturned]);await expect(drawer().locator('.el-descriptions__content').first()).toHaveText(ids[1]);await drawer().getByRole('button',{name:/关闭此对话框|Close this dialog/}).click()}finally{await page.unroute(`**${outputA}`,reorder);await page.unroute(`**${outputB}`,reorder)}
  })

  await step('读取冻结客户报文',async()=>{
    let delivery
    await expect.poll(async()=>{const rows=(await read('/deliveries?limit=200')).items;delivery=rows.find(d=>d.gatewayId===gatewayId);return !!delivery},{timeout:45000}).toBe(true)
    await navigate('/deliveries');await input(page,'按投递 ID 查询').fill(delivery.deliveryId);await page.getByRole('button',{name:'打开详情',exact:true}).click()
    await drawer().getByRole('tab',{name:'冻结客户报文',exact:true}).click()
    const pending=page.waitForResponse(r=>new URL(r.url()).pathname===`/api/v1/deliveries/${delivery.deliveryId}/payload`)
    await drawer().getByRole('button',{name:'读取冻结报文',exact:true}).click();const frozen=await (await pending).json()
    expect(frozen.payloadAvailable).toBe(true);expect(frozen.payload.deliveryId).toBe(delivery.deliveryId)
    report.created.inspectedDeliveryId=delivery.deliveryId
    await drawer().getByRole('button',{name:/关闭此对话框|Close this dialog/}).click()
  })

  await step('人工重试保持原投递身份',async()=>{
    const retryId=process.env.GW_TEST_RETRY_DELIVERY_ID
    const rows=(await read(`/deliveries?state=RETRY_WAIT&limit=200${fixture.retryTargetId?'&targetId='+encodeURIComponent(fixture.retryTargetId):''}`)).items
    const candidate=retryId?await read(`/deliveries/${encodeURIComponent(retryId)}`):rows.find(d=>d.gatewayId===gatewayId)
    if(!candidate){report.limitations.push('人工重试未覆盖：未提供 RETRY_WAIT 种子；正常已确认投递不会人为改写。');throw new Error('Retry coverage requires GW_TEST_RETRY_DELIVERY_ID or a RETRY_WAIT delivery for the seed gateway')}
    await input(page,'按投递 ID 查询').fill(candidate.deliveryId);await page.getByRole('button',{name:'打开详情',exact:true}).click()
    await input(drawer(),'重试原因').fill('UI retry validation after controlled receiver failure')
    const retried=await mutation(`/deliveries/${candidate.deliveryId}/retries`,async()=>{await drawer().getByRole('button',{name:'重新调度原投递',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'重新调度',exact:true}).click()})
    for(const field of ['deliveryId','outputId','sourceEventId','processingRevision','targetId','targetVersion','mappingVersion'])expect(retried[field]).toBe(candidate[field])
    report.created.retriedDeliveryId=candidate.deliveryId
    await drawer().getByRole('button',{name:/关闭此对话框|Close this dialog/}).click()
  })

  await step('管道页面无浏览器脚本错误',async()=>{expect(report.pageErrors).toEqual([]);await page.screenshot({path:resolve(reportDirectory,'pipeline-deliveries.png'),fullPage:true})})
}catch(error){failed=error;console.error(sanitize(error.message));try{await page.screenshot({path:resolve(reportDirectory,'pipeline-failure.png'),fullPage:true})}catch{}}
finally{report.finishedAt=new Date().toISOString();report.status=failed?'failed':'passed';await writeFile(resolve(reportDirectory,'pipeline-e2e.json'),JSON.stringify(report,null,2));await context.close();await browser.close()}
if(failed)process.exitCode=1
