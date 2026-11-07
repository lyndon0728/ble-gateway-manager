import test from 'node:test'
import assert from 'node:assert/strict'
import fs from 'node:fs/promises'
import ts from 'typescript'

const source=await fs.readFile(new URL('../src/lib/api.ts',import.meta.url),'utf8')
const {outputText}=ts.transpileModule(source,{compilerOptions:{target:ts.ScriptTarget.ES2022,module:ts.ModuleKind.ESNext}})
const client=await import('data:text/javascript;base64,'+Buffer.from(outputText).toString('base64'))
const saved=new Map()
globalThis.sessionStorage={getItem:k=>saved.get(k)||null,setItem:(k,v)=>saved.set(k,v),removeItem:k=>saved.delete(k)}
globalThis.window=new EventTarget()
const json=(body,status=200,headers={})=>new Response(JSON.stringify(body),{status,headers:{'Content-Type':'application/json',...headers}})

test('uncertain mutation retains identity; changed intent gets another key; confirmed result clears it',async()=>{
 client.clearPendingRequests();const keys=[];let fail=true
 globalThis.fetch=async(_path,options)=>{keys.push(options.headers['Idempotency-Key']);if(fail)throw new TypeError('offline');return json({taskId:'task_verified'})}
 await assert.rejects(client.api('/api/v1/tasks','POST',{x:1}),e=>e.code==='NETWORK_UNCERTAIN')
 fail=false;await client.api('/api/v1/tasks','POST',{x:1});assert.equal(keys[0],keys[1])
 await client.api('/api/v1/tasks','POST',{x:1});assert.notEqual(keys[1],keys[2])
 await client.api('/api/v1/tasks','POST',{x:2});assert.notEqual(keys[2],keys[3])
})
test('503 and 429 preserve write identity, ETag and CSRF; server trace remains visible',async()=>{
 client.clearPendingRequests();saved.set('gw_csrf','csrf-proof');const calls=[];let count=0
 globalThis.fetch=async(_path,options)=>{calls.push(options);count++;return count===1?json({error:{code:'BUSY',message:'later',traceId:'trace_example'}},503):count===2?json({error:{code:'RATE_LIMITED',message:'later'}},429,{'Retry-After':'4'}):json({ok:true})}
 await assert.rejects(client.api('/api/v1/scopes','PATCH',{name:'new'},{etag:'"3"'}),e=>client.errorText(e).includes('trace_example'))
 await assert.rejects(client.api('/api/v1/scopes','PATCH',{name:'new'},{etag:'"3"'}),e=>e.retryAfter==='4')
 await client.api('/api/v1/scopes','PATCH',{name:'new'},{etag:'"3"'})
 assert.equal(new Set(calls.map(v=>v.headers['Idempotency-Key'])).size,1)
 assert.equal(calls[0].headers['If-Match'],'"3"');assert.equal(calls[0].headers['X-CSRF-Token'],'csrf-proof')
})
test('new browser tab recovers CSRF from authenticated me; 401 emits expiration without retry',async()=>{
 saved.clear();let expired=0;window.addEventListener('gateway-session-expired',()=>expired++)
 globalThis.fetch=async()=>json({principalId:'p1'},200,{'X-CSRF-Token':'restored-proof'})
 await client.api('/api/v1/auth/me');assert.equal(saved.get('gw_csrf'),'restored-proof')
 globalThis.fetch=async()=>json({error:{code:'UNAUTHENTICATED',message:'expired'}},401)
 await assert.rejects(client.api('/api/v1/gateways'));assert.equal(expired,1)
 await assert.rejects(client.api('/api/v1/auth/sessions/current','DELETE'));assert.equal(expired,2)
 await assert.rejects(client.api('/api/v1/auth/sessions','POST',{name:'invalid',password:'invalid'}));assert.equal(expired,2)
})
test('multipart leaves boundary to browser; relative URL guard prevents external credential requests',async()=>{
 const form=new FormData();form.set('manifest','example');let captured
 globalThis.fetch=async(_path,options)=>{captured=options;return json({artifactId:'artifact_test'})}
 await client.api('/api/v1/artifacts','POST',form)
 assert.equal(captured.body,form);assert.equal(captured.headers['Content-Type'],undefined)
 await assert.rejects(client.api('https://external.invalid/api'))
 await assert.rejects(client.api('//external.invalid/api'))
})
