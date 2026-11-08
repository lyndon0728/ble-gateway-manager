import {chromium,expect} from '@playwright/test'
import fs from 'node:fs/promises'
import path from 'node:path'
import {fileURLToPath} from 'node:url'
import crypto from 'node:crypto'
const dir=path.dirname(fileURLToPath(import.meta.url)),root=path.resolve(dir,'../..')
const fixture=JSON.parse((await fs.readFile(process.env.GW_TEST_FIXTURE||path.join(root,'server/.local/frontend-e2e/fixture.json'),'utf8')).replace(/^\uFEFF/,''))
const base=process.env.GW_TEST_URL||fixture.baseUrl,password=(await fs.readFile(process.env.GW_TEST_PASSWORD_FILE||fixture.passwordFile,'utf8')).trim()
const out=path.resolve(dir,'../test-results');await fs.mkdir(out,{recursive:true})
const browser=await chromium.launch({channel:process.env.GW_TEST_BROWSER||'chrome',headless:true})
const context=await browser.newContext({viewport:{width:1440,height:1000}}),page=await context.newPage()
const errors=[],unexpected=[],external=[];let expectedFailure=false
page.on('pageerror',e=>errors.push(e.message))
page.on('response',r=>{if(!expectedFailure&&r.status()>=400&&!['/api/v1/auth/me','/favicon.ico'].includes(new URL(r.url()).pathname))unexpected.push({path:new URL(r.url()).pathname,status:r.status()})})
await context.route('**/*',async route=>{const url=new URL(route.request().url());if(url.origin!==base&&['http:','https:'].includes(url.protocol)){external.push(url.origin);await route.abort()}else await route.continue()})
const report={passed:false,pages:[],roles:[],responsive:[],browserErrors:errors,unexpectedResponses:unexpected,externalRequests:external}
async function login(p,name,secret){await p.goto(base+'/ui/login');await p.locator('#login-name').fill(name);await p.locator('#login-password').fill(secret);await p.getByRole('button',{name:'登录',exact:true}).click();await expect(p.locator('.sidebar')).toBeVisible()}
async function api(method,url,body,extra={}){const csrf=await page.evaluate(()=>sessionStorage.getItem('gw_csrf'));const r=await context.request.fetch(base+url,{method,data:body,headers:{'X-CSRF-Token':csrf,'Idempotency-Key':crypto.randomUUID(),...extra}});if(!r.ok())throw new Error(`${method} ${url}: ${r.status()}`);return r.status()===204?null:r.json()}
async function go(url){await page.goto(base+'/ui'+url);await expect(page.locator('main h1').first()).toBeVisible();await page.waitForLoadState('networkidle');await expect(page.locator('.el-alert--error')).toHaveCount(0)}
try{
 await login(page,'admin',password);await expect(page.locator('.sidebar nav a')).toHaveCount(20)
 const events=await api('GET','/api/v1/events?gatewayId=gw_reference_ui&limit=50');expect(events.items.length).toBeGreaterThan(0)
 const existing=(await api('GET','/api/v1/tasks?gatewayId='+fixture.gatewayId+'&state=SUCCEEDED&limit=200')).items.find(t=>t.type==='DIAGNOSTICS')
 const task=existing||await api('POST','/api/v1/tasks',{gatewayId:fixture.gatewayId,type:'DIAGNOSTICS',parameters:{profile:'HEALTH',durationSeconds:1},expiresAt:new Date(Date.now()+120000).toISOString()})
 await expect.poll(async()=>(await api('GET','/api/v1/tasks/'+task.taskId)).state,{timeout:25000}).toBe('SUCCEEDED')
 const routes=[['P01','/overview'],['P02','/gateways'],['P03','/gateways/'+fixture.gatewayId],['P04','/gateways/'+fixture.gatewayId+'/configuration'],['P05','/ble-peers'],['P06','/gatt-connections'],['P07','/auto-connect-rules'],['P08','/events'],['P09','/gateways/gw_reference_ui/events/'+events.items[0].eventId],['P10','/parsers'],['P11','/rule-bundles'],['P12','/quarantine'],['P13','/targets'],['P14','/targets/'+fixture.targetId],['P15','/deliveries'],['P16','/tasks'],['P17','/tasks/'+task.taskId],['P18','/artifacts'],['P19','/scopes'],['P20','/principals'],['P21','/system'],['P22','/audit'],['P23','/integration'],['P24','/device-models'],['P26','/system/ai']]
 report.pages.push({id:'P00',passed:true})
 for(const [id,url]of routes){await go(url);report.pages.push({id,passed:true});if(id==='P01')await page.screenshot({path:path.join(out,'overview-desktop.png'),fullPage:true});if(id==='P09')await page.screenshot({path:path.join(out,'event-detail-desktop.png'),fullPage:true})}
 // A separate tab inherits HttpOnly cookies but no sessionStorage proof.
 const tab=await context.newPage();await tab.goto(base+'/ui/scopes');await expect(tab.locator('main h1').first()).toBeVisible();expect(await tab.evaluate(()=>Boolean(sessionStorage.getItem('gw_csrf')))).toBe(true);await tab.close();report.newTabCsrf=true
 for(const width of [320,736,1024]){await page.setViewportSize({width,height:950});for(const [id,url]of routes){await go(url);const overflow=await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth+2);report.responsive.push({id,width,overflow})}if(width===736)await page.screenshot({path:path.join(out,'integration-tablet.png'),fullPage:true})}
 await page.setViewportSize({width:1440,height:1000});await go('/gateways')
 // No auto retry after a transport error. UI should give an actionable failure.
 expectedFailure=true;await context.route('**/api/v1/gateways?**',r=>r.abort('failed'));await page.getByRole('button',{name:'刷新',exact:true}).click();await expect(page.locator('.el-alert--error')).toContainText('NETWORK_UNCERTAIN');await context.unroute('**/api/v1/gateways?**');await page.getByRole('button',{name:'刷新',exact:true}).click();await expect(page.locator('.el-alert--error')).toHaveCount(0);expectedFailure=false;report.networkRecovery=true
 const gw=await api('GET','/api/v1/gateways/'+fixture.gatewayId)
 for(const role of ['DEVICE_ADMIN','OPERATOR','DEVELOPER','AUDITOR']){
   const name='ui_'+role.toLowerCase()+'_'+crypto.randomBytes(3).toString('hex'),secret=crypto.randomBytes(24).toString('base64url')
   await api('POST','/api/v1/principals',{type:'USER',name,password:secret,roleBindings:[{role,scopeId:role==='DEVELOPER'?'scope_instance':gw.groupId}]})
   const c=await browser.newContext(),p=await c.newPage();const failures=[];p.on('pageerror',e=>failures.push(e.message));await login(p,name,secret)
   await expect(p.locator('.sidebar a[href="/ui/principals"]')).toHaveCount(0)
   await p.goto(base+'/ui/principals');await expect(p.locator('main')).toContainText('权限')
   if(role==='DEVELOPER'){await p.goto(base+'/ui/rule-bundles');await expect(p.locator('main h1')).toBeVisible();await expect(p.locator('.el-alert--error')).toHaveCount(0);await expect(p.getByRole('button',{name:'创建规则包',exact:true})).toHaveCount(0)}
   expect(failures).toEqual([]);report.roles.push({role,passed:true});await c.close()
 }
 expect(errors).toEqual([]);expect(unexpected).toEqual([]);expect(external).toEqual([]);expect(report.responsive.filter(v=>v.overflow)).toEqual([])
 report.passed=true;report.at=new Date().toISOString();await fs.writeFile(path.join(out,'smoke-e2e.json'),JSON.stringify(report,null,2));console.log(JSON.stringify({passed:true,pages:report.pages.length,roles:report.roles.length,responsiveChecks:report.responsive.length,newTabCsrf:true,networkRecovery:true,browserErrors:errors.length}))
}catch(e){report.failure=e.message;await fs.writeFile(path.join(out,'smoke-e2e.json'),JSON.stringify(report,null,2));await page.screenshot({path:path.join(out,'smoke-failure.png'),fullPage:true}).catch(()=>{});throw e}finally{await browser.close()}
