import {chromium,expect} from '@playwright/test'
import fs from 'node:fs/promises'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import http from 'node:http'
import crypto from 'node:crypto'

const dir=path.dirname(fileURLToPath(import.meta.url)),root=path.resolve(dir,'../..')
const fixture=JSON.parse((await fs.readFile(process.env.GW_TEST_FIXTURE||path.join(root,'server/.local/frontend-e2e/fixture.json'),'utf8')).replace(/^\uFEFF/,''))
const base=process.env.GW_TEST_URL||fixture.baseUrl,password=(await fs.readFile(process.env.GW_TEST_PASSWORD_FILE||fixture.passwordFile,'utf8')).trim()
if(!/^http:\/\/127\.0\.0\.1:/.test(base)||base.includes(':18780'))throw new Error('AI tests require an isolated loopback fixture, never the delivery instance')
const out=path.resolve(dir,'../test-results');await fs.mkdir(out,{recursive:true})
const nonce=crypto.randomBytes(4).toString('hex'),modelName='AI UI Sensor '+nonce,providerName='AI UI Local '+nonce,requests=[]
const serviceUuid='11111111-1111-1111-1111-111111111111',characteristicUuid='22222222-2222-2222-2222-222222222222'
const docText='Synthetic UI fixture only. Sensor '+nonce+'. Four bytes, signed little-endian int16 temperature /100 at offset 0, unsigned little-endian uint16 humidity /100 at offset 2. C4 09 88 13 => 25 and 50. Invalid length 00 must fail. GATT service '+serviceUuid+', characteristic '+characteristicUuid+', NOTIFY without encryption.'
const modelServer=http.createServer(async(req,res)=>{
 try{
  let raw='';for await(const chunk of req)raw+=chunk
  const body=JSON.parse(raw);requests.push({path:req.url,body})
  const check=body.messages?.[0]?.content?.includes('Return JSON only')
  let content={ok:true}
  if(!check){
   const prompt=body.messages?.find(m=>m.role==='user')?.content
   const payload=JSON.parse(typeof prompt==='string'?prompt:prompt.find(p=>p.type==='text').text)
   const evidence=payload.evidence.find(e=>e.text?.includes('Synthetic UI fixture'))
   if(!evidence)throw new Error('Expected document evidence in model request')
   content={manufacturer:'UI Fixture',modelName,firmwareRange:'test-only',parserSpec:{dslVersion:'1.0',inputKind:'GATT_VALUE',length:4,fields:[{name:'temperature',offset:0,type:'int16',endian:'little',scale:0.01},{name:'humidity',offset:2,type:'uint16',endian:'little',scale:0.01,min:0,max:100}]},connectionSpec:{serviceUuid,characteristicUuid,security:'UNENCRYPTED',operation:'GATT_SUBSCRIBE',mode:'NOTIFY'},mappingSpec:null,tests:[{name:'Known valid packet',payloadHex:'C4 09 88 13',expected:[{temperature:25,humidity:50}],source:'DOCUMENT'},{name:'Rejected short packet',payloadHex:'00',expectError:true,source:'DOCUMENT'}],sources:[{path:'parserSpec',evidenceId:evidence.evidenceId,quote:evidence.text,state:'DOCUMENT'},{path:'connectionSpec',evidenceId:evidence.evidenceId,quote:evidence.text,state:'DOCUMENT'}],issues:[]}
   if(payload.taskKind==='CUSTOMER_INTERFACE')content={manufacturer:'UI Fixture',modelName:'AI UI Mapping '+nonce,firmwareRange:'',parserSpec:null,connectionSpec:null,mappingSpec:{fields:[{source:'temperature',target:'tempC'}]},tests:[{name:'Valid mapped value',input:{temperature:25},expected:[{tempC:25}],source:'DOCUMENT'},{name:'Missing required source',input:{humidity:50},expectError:true,source:'DOCUMENT'}],sources:[{path:'mappingSpec',evidenceId:evidence.evidenceId,quote:evidence.text,state:'DOCUMENT'}],issues:[]}
  }
  res.writeHead(200,{'Content-Type':'application/json'});res.end(JSON.stringify({choices:[{finish_reason:'stop',message:{content:JSON.stringify(content)}}],usage:{prompt_tokens:100,completion_tokens:150,total_tokens:250}}))
 }catch(e){res.writeHead(500,{'Content-Type':'application/json'});res.end(JSON.stringify({error:{message:e.message}}))}
})
await new Promise(resolve=>modelServer.listen(0,'127.0.0.1',resolve))
const modelBase='http://127.0.0.1:'+modelServer.address().port+'/v1'
const browser=await chromium.launch({channel:process.env.GW_TEST_BROWSER||'chrome',headless:true})
const context=await browser.newContext({viewport:{width:1540,height:1080}}),page=await context.newPage(),errors=[],external=[],taskMutations=[]
page.on('pageerror',e=>errors.push(e.message))
page.on('request',r=>{const u=new URL(r.url());if(['http:','https:'].includes(u.protocol)&&u.origin!==base)external.push(u.origin);if(r.method()==='POST'&&u.pathname==='/api/v1/tasks')taskMutations.push(u.pathname)})
let provider,jobId,modelId
const report={passed:false,steps:[],browserErrors:errors,externalRequests:external,responsive:[]}
async function login(p,name,secret){await p.goto(base+'/ui/login');await p.locator('#login-name').fill(name);await p.locator('#login-password').fill(secret);await p.getByRole('button',{name:'登录',exact:true}).click();await expect(p.locator('.sidebar')).toBeVisible()}
async function api(method,url,data,headers={}){const csrf=await page.evaluate(()=>sessionStorage.getItem('gw_csrf'));const r=await context.request.fetch(base+url,{method,data,headers:{'X-CSRF-Token':csrf,'Idempotency-Key':crypto.randomUUID(),...headers}});if(!r.ok())throw new Error(method+' '+url+' '+r.status()+' '+await r.text());return r.status()===204?null:r.json()}
async function go(url){await page.goto(base+'/ui'+url);await expect(page.locator('main h1').first()).toBeVisible();await page.waitForLoadState('networkidle')}
try{
 await login(page,'admin',password)
 provider=await api('POST','/api/v1/ai/providers',{name:providerName,kind:'OPENAI_COMPATIBLE',mode:'LOCAL',baseUrl:modelBase,model:'ui-contract-fixture',enabled:true,allowExternal:false,allowInsecureLocal:true,vision:false,schemaMode:'JSON_OBJECT',timeoutSeconds:30,maxOutputTokens:4096,dailyRequestLimit:20,tokenParameter:'max_tokens'})
 await go('/system/ai');await expect(page.locator('.sidebar nav a.is-active')).toHaveCount(1);await expect(page.locator('.sidebar nav a.is-active')).toHaveText('模型服务设置')
 const providerRow=page.locator('.el-table__row').filter({hasText:providerName})
 await providerRow.getByRole('button',{name:'测试连接',exact:true}).click();await expect(page.getByText('最近连接测试 · '+providerName,{exact:true})).toBeVisible()
 await expect(page.locator('.el-alert--success')).toBeVisible();report.steps.push('provider status and local compatibility check')
 await go('/parsers')
 await page.getByRole('button',{name:'导入说明书生成解析方案',exact:true}).click()
 const dialog=page.getByRole('dialog').filter({hasText:'导入资料'})
 await dialog.locator('input[type=file]').setInputFiles({name:'synthetic-protocol.txt',mimeType:'text/plain',buffer:Buffer.from(docText)})
 const providerSelect=dialog.locator('.el-form-item').filter({hasText:'模型服务'}).locator('.el-select')
 await providerSelect.click();await page.getByRole('option',{name:providerName+' · ui-contract-fixture',exact:true}).click()
 await dialog.getByRole('button',{name:'上传并生成草稿',exact:true}).click()
 await expect(page).toHaveURL(/\/ui\/import-jobs\//)
 jobId=new URL(page.url()).pathname.split('/').at(-1)
 await expect(page.locator('.test-card')).toHaveCount(2,{timeout:30000});await page.getByRole('tab',{name:'解析字段',exact:true}).click();await expect(page.locator('.field-card').first()).toBeVisible()
 await expect(page.locator('.evidence-card').first()).toContainText('Synthetic UI fixture')
 await expect(page.locator('.test-card')).toHaveCount(2)
 await page.screenshot({path:path.join(out,'ai-workbench-generated.png'),fullPage:true})
 report.steps.push('document upload, provider request, evidence and visual draft')
 await page.getByRole('button',{name:'运行验证',exact:true}).click()
 await expect(page.getByText('INDEPENDENT_SAMPLE_REQUIRED',{exact:true})).toBeVisible()
 report.steps.push('model-supplied sample cannot self-approve')
 for(const card of await page.locator('.test-card').all()){await card.locator('.el-select').click();await page.getByRole('option',{name:'人工已核对',exact:true}).last().click();await expect(page.locator('.el-select-dropdown__item:visible')).toHaveCount(0)}
 await page.getByRole('button',{name:'保存草稿',exact:true}).click()
 await page.getByRole('button',{name:'运行验证',exact:true}).click()
 await expect(page.getByText('当前草稿验证通过',{exact:true})).toBeVisible()
 await page.reload();await expect(page.getByText('当前草稿验证通过',{exact:true})).toBeVisible()
 report.steps.push('explicit sample review, deterministic validation and reload recovery')
 await page.getByRole('button',{name:'确认发布',exact:true}).click();await page.getByRole('button',{name:'发布模板',exact:true}).click()
 await expect(page.getByText('模板已发布',{exact:true})).toBeVisible()
 const job=await api('GET','/api/v1/ai/import-jobs/'+jobId),draft=await api('GET','/api/v1/protocol-drafts/'+job.draftId)
 modelId=draft.publishedModelId;const model=await api('GET','/api/v1/device-models/'+modelId)
 expect(model.verificationLevel).toBe('SAMPLE_VERIFIED');expect(model.parserRef.parserId).toBeTruthy()
 await page.getByRole('button',{name:'到型号库应用',exact:true}).click()
 await expect(page.getByText(modelName,{exact:true}).first()).toBeVisible()
 report.steps.push('immutable model publication and library')
 await go('/parsers?modelId='+modelId);await page.getByRole('button',{name:'运行试算',exact:true}).click();await expect(page.locator('.output-record')).toContainText('temperature')
 await go('/gatt-connections?modelId='+modelId);await expect(page.getByText('已带入 '+modelName,{exact:false}).first()).toBeVisible()
 await go('/auto-connect-rules?modelId='+modelId);await expect(page.getByRole('dialog').filter({hasText:'新建自动规则'})).toBeVisible();await page.getByText('执行条件',{exact:true}).click();await expect(page.getByRole('switch')).not.toBeChecked()
 await go('/rule-bundles?modelId='+modelId);await expect(page.getByText('指定实际蓝牙对端',{exact:true})).toBeVisible();await expect(page.getByText('服务 UUID 限定，可选',{exact:true})).toBeVisible()
 expect(taskMutations).toEqual([])
 report.steps.push('P06 P07 P10 P11 template parameters without physical tasks')
 await go('/ble-peers?modelId='+modelId);await expect(page.getByRole('checkbox',{name:'保存时创建到此模板版本的协议绑定'})).toBeChecked()
 const peerDialog=page.getByRole('dialog').filter({hasText:'登记蓝牙对端'}),peerAddress=('AA:BB:CC:'+nonce.slice(0,2)+':'+nonce.slice(2,4)+':'+nonce.slice(4,6)).toUpperCase()
 const gateway=await api('GET','/api/v1/gateways/'+fixture.gatewayId),scopes=await api('GET','/api/v1/resource-scopes')
 const scope=scopes.items.find(s=>s.scopeId===gateway.groupId)||scopes.items.find(s=>s.state==='ACTIVE'&&s.scopeId!=='scope_instance')
 expect(scope).toBeTruthy()
 await peerDialog.locator('.el-form-item').filter({hasText:'蓝牙地址'}).locator('input').fill(peerAddress)
 await peerDialog.locator('.el-form-item').filter({hasText:'资源范围'}).locator('.el-select').click();await page.getByRole('option',{name:scope.name,exact:true}).click()
 await peerDialog.getByRole('button',{name:'保存对端',exact:true}).click();await expect(page.locator('.el-drawer:visible')).toHaveCount(0)
 const peers=await api('GET','/api/v1/ble-peers'),peer=peers.items.find(p=>p.deviceAddress.replaceAll(':','')===peerAddress.replaceAll(':',''))
 expect(peer).toBeTruthy();const bindings=await api('GET','/api/v1/protocol-bindings?peerId='+peer.peerId)
 expect(bindings.items.some(b=>b.peerId===peer.peerId&&b.modelId===modelId&&b.parserId===model.parserRef.parserId&&b.parserVersion===model.parserRef.version)).toBe(true)
 report.steps.push('P05 saved real peer and immutable published-parser binding')
 const targetBefore=await api('GET','/api/v1/targets/'+fixture.targetId),previousConfig=targetBefore.versions.at(-1).configuration
 await go('/targets/'+fixture.targetId);await page.getByRole('button',{name:'导入客户接口说明',exact:true}).click()
 const mappingDialog=page.getByRole('dialog').filter({hasText:'导入资料'})
 await mappingDialog.locator('input[type=file]').setInputFiles({name:'synthetic-customer-interface.txt',mimeType:'text/plain',buffer:Buffer.from('Synthetic UI fixture only. Customer interface maps temperature to tempC. Input {"temperature":25} outputs {"tempC":25}. Missing temperature must fail. Keep HTTP_DURABLE_RESPONSE confirmation unchanged.')})
 await mappingDialog.locator('.el-form-item').filter({hasText:'模型服务'}).locator('.el-select').click();await page.getByRole('option',{name:providerName+' · ui-contract-fixture',exact:true}).click()
 await mappingDialog.getByRole('button',{name:'上传并生成草稿',exact:true}).click();await expect(page).toHaveURL(/\/ui\/import-jobs\//)
 const mappingJobId=new URL(page.url()).pathname.split('/').at(-1)
 await expect(page.locator('.test-card')).toHaveCount(2,{timeout:30000});await expect(page.getByText('映射输入 JSON 对象',{exact:true})).toHaveCount(2)
 for(const card of await page.locator('.test-card').all()){await card.locator('.el-select').click();await page.getByRole('option',{name:'人工已核对',exact:true}).last().click();await expect(page.locator('.el-select-dropdown__item:visible')).toHaveCount(0)}
 await page.getByRole('button',{name:'保存草稿',exact:true}).click();await page.getByRole('button',{name:'运行验证',exact:true}).click();await expect(page.getByText('当前草稿验证通过',{exact:true})).toBeVisible()
 await page.getByRole('button',{name:'确认发布',exact:true}).click();await page.getByRole('button',{name:'发布模板',exact:true}).click();await expect(page.getByText('模板已发布',{exact:true})).toBeVisible()
 const mappingJob=await api('GET','/api/v1/ai/import-jobs/'+mappingJobId),mappingDraft=await api('GET','/api/v1/protocol-drafts/'+mappingJob.draftId),mappingModel=await api('GET','/api/v1/device-models/'+mappingDraft.publishedModelId)
 await page.getByRole('button',{name:'带回客户转发目标',exact:true}).click();await expect(page).toHaveURL(new RegExp('/ui/targets/'+fixture.targetId))
 const targetDialog=page.getByRole('dialog').filter({hasText:'发布目标新版本'})
 await expect(targetDialog).toBeVisible();await expect(targetDialog.locator('.el-form-item').filter({hasText:'目标字段'}).locator('input')).toHaveValue('tempC')
 await targetDialog.getByRole('button',{name:'发布新版本',exact:true}).click();await page.locator('.el-message-box').getByRole('button',{name:'发布新版本',exact:true}).click();await expect(targetDialog).toHaveCount(0)
 const targetAfter=await api('GET','/api/v1/targets/'+fixture.targetId),newConfig=targetAfter.versions.at(-1).configuration
 expect(targetAfter.versions).toHaveLength(targetBefore.versions.length+1)
 for(const old of targetBefore.versions){const same=targetAfter.versions.find(v=>v.version===old.version);expect(same.configuration).toEqual(old.configuration);expect(same.contentDigest).toBe(old.contentDigest)}
 expect(newConfig.mapping).toEqual(mappingModel.content.mappingSpec);expect(newConfig.mappingVersion).toContain(mappingModel.modelId);expect(newConfig.ackMode).toBe(previousConfig.ackMode);expect(newConfig.endpoint).toBe(previousConfig.endpoint)
 report.steps.push('P14 customer interface imported, validated, published and returned to target')
 report.steps.push('P14 new immutable target version preserves old versions and ACK contract')
 const developerName='ai_dev_'+nonce,developerSecret=crypto.randomBytes(24).toString('base64url')
 await api('POST','/api/v1/principals',{type:'USER',name:developerName,password:developerSecret,roleBindings:[{role:'DEVELOPER',scopeId:'scope_instance'}]})
 const dc=await browser.newContext({viewport:{width:1440,height:1000}}),dp=await dc.newPage()
 await login(dp,developerName,developerSecret);await dp.goto(base+'/ui/import-jobs/'+jobId);await expect(dp.locator('main h1')).toHaveText('导入审阅工作台');await expect(dp.getByRole('button',{name:'确认发布',exact:true})).toHaveCount(0)
 await dp.goto(base+'/ui/system/ai');await expect(dp.locator('main')).toContainText('权限');await dc.close()
 report.steps.push('developer cannot publish or configure provider')
 for(const width of [390,768,1440]){await page.setViewportSize({width,height:1000});for(const [name,url]of [['P24','/device-models'],['P25','/import-jobs/'+jobId],['P26','/system/ai']]){await go(url);const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+2);report.responsive.push({page:name,width,overflow});if(width===390&&name==='P25')await page.screenshot({path:path.join(out,'ai-workbench-mobile.png'),fullPage:true})}}
 expect(report.responsive.filter(v=>v.overflow)).toEqual([]);expect(errors).toEqual([]);expect(external).toEqual([])
 expect(requests.length).toBeGreaterThanOrEqual(2);expect(requests.every(r=>r.path==='/v1/chat/completions')).toBe(true);expect(requests.every(r=>'max_tokens'in r.body)).toBe(true)
 expect(taskMutations).toEqual([]);report.passed=true;report.jobId=jobId;report.modelId=modelId;report.mappingModelId=mappingModel.modelId;report.peerId=peer.peerId;report.localModelRequests=requests.length;report.at=new Date().toISOString()
 console.log(JSON.stringify({passed:true,steps:report.steps.length,responsiveChecks:report.responsive.length,modelRequests:requests.length,physicalTasks:taskMutations.length}))
}catch(e){report.failure=e.message;await page.screenshot({path:path.join(out,'ai-e2e-failure.png'),fullPage:true}).catch(()=>{});throw e}
finally{if(provider){try{await api('PATCH','/api/v1/ai/providers/'+provider.providerId,{enabled:false},{'If-Match':'"'+provider.revision+'"'})}catch{}}await fs.writeFile(path.join(out,'ai-e2e.json'),JSON.stringify(report,null,2));await browser.close();await new Promise(resolve=>modelServer.close(resolve))}
