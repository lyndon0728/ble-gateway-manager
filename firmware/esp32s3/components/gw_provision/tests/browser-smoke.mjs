// Host-only browser + independent Node crypto server. This is NOT an ESP/HIL test.
import { createServer } from 'node:http';
import { createHash,createCipheriv,createDecipheriv,timingSafeEqual,randomBytes } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { fileURLToPath,pathToFileURL } from 'node:url';
import path from 'node:path';
import assert from 'node:assert/strict';
import { N,binary,integer,cat,field,parse } from '../web/src/security2.js';
const here=path.dirname(fileURLToPath(import.meta.url));
const playwright=await import(pathToFileURL(path.resolve(here,'../../../../../frontend/node_modules/@playwright/test/index.mjs')).href);
const username='setup-fixture-0',password='Public-Test-Only-0';
const h=(...values)=>createHash('sha512').update(Buffer.concat(values.map(x=>Buffer.from(x)))).digest();
const mod=x=>(x%N+N)%N;
function power(x,y){let r=1n;x=mod(x);while(y){if(y&1n)r=mod(r*x);x=mod(x*x);y>>=1n;}return r;}
const pad=x=>binary(x,384),g=5n,k=integer(h(pad(N),pad(g)));
const salt=randomBytes(16),x=integer(h(salt,h(Buffer.from(`${username}:${password}`)))),verifier=power(g,x);
let srp,nonce,sequence=0,closed=false,operations=[];
let gateway={networkGeneration:'0',localRevision:'0',localOverride:false,verificationState:'LOCAL_ONLY',activeInterface:'NONE',credentials:[],desiredLocalNetwork:{uplinkOrder:['WIFI_STA'],ethernet:{enabled:false,ipv4:{mode:'DHCP'}},wifiSta:{enabled:true,ssid:'Plant 2.4G',security:'WPA2_PSK',credentialRef:'',ipv4:{mode:'DHCP'}}}};
const server=createServer(async(req,res)=>{
 try{
  if(req.method==='GET'){
   if(req.url==='/prov-info'){res.setHeader('Content-Type','application/json');res.end(JSON.stringify({securityVersion:2,securityPatchVersion:1}));return;}
   const files={'/':['index.html','text/html'],'/app.js':['app.js','application/javascript'],'/style.css':['style.css','text/css']};
   const item=files[req.url];if(!item){res.writeHead(404);res.end();return;}
   res.setHeader('Content-Type',item[1]);res.setHeader('Content-Security-Policy',"default-src 'none'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'; frame-ancestors 'none'; form-action 'none'");
   res.end(readFileSync(path.resolve(here,'../assets',item[0])));return;
  }
  const input=Buffer.concat(await Array.fromAsync(req));
  assert.equal(req.headers['content-type'],'application/octet-stream');
  assert.equal(req.headers.origin,`http://127.0.0.1:${server.address().port}`);
  res.setHeader('Content-Type','application/octet-stream');
  if(req.url==='/prov-session'){
   const inner=parse(parse(new Uint8Array(input)).get(12)),command=inner.get(1)||0;
   if(command===0){
    const payload=parse(inner.get(20)),Abytes=payload.get(2);assert.equal(Buffer.from(payload.get(1)).toString(),username);
    const A=integer(Abytes),b=integer(randomBytes(32)),B=mod(k*verifier+power(g,b)),Bbytes=binary(B),u=integer(h(pad(A),pad(B)));
    const S=power(A*power(verifier,u),b),K=h(binary(S)),hn=h(pad(N)),hg=h(pad(g));
    const M=h(Uint8Array.from(hn,(a,i)=>a^hg[i]),h(Buffer.from(username)),salt,Abytes,Bbytes,K);
    srp={M,proof:h(Abytes,M,K),key:K.subarray(0,32)};
    res.setHeader('Set-Cookie','gw_session=host-test-only; HttpOnly; SameSite=Strict; Path=/');
    res.end(cat(field(2,2),field(12,cat(field(1,1),field(21,cat(field(2,Bbytes),field(3,salt)))))));return;
   }
   assert.equal(command,2);assert.ok(req.headers.cookie?.includes('gw_session='));
   const proof=parse(inner.get(22)).get(1);assert.ok(timingSafeEqual(srp.M,proof));
   nonce=Buffer.concat([randomBytes(8),Buffer.from([0,0,0,1])]);sequence=1;
   res.end(cat(field(2,2),field(12,cat(field(1,3),field(23,cat(field(2,srp.proof),field(3,nonce)))))));return;
  }
  assert.equal(req.url,'/prov-network');assert.ok(!closed);
  const iv=Buffer.from(nonce);iv.writeUInt32BE(sequence++,8);
  const decrypt=createDecipheriv('aes-256-gcm',srp.key,iv);decrypt.setAuthTag(input.subarray(-16));
  const request=JSON.parse(Buffer.concat([decrypt.update(input.subarray(0,-16)),decrypt.final()]));
  operations.push(request.operation);let body={},code='OK';
  switch(request.operation){
   case 'GET_STATE':body=gateway;break;
   case 'SCAN_WIFI':body=[{ssid:'Plant 2.4G',security:'WPA2_PSK',rssi:-48},{ssid:'<img src=x onerror=alert(1)>',security:'WPA2_PSK',rssi:-70}];break;
   case 'PUT_CREDENTIAL':assert.equal(request.body.password,'A-Fixture-Wifi-Password');gateway.credentials.push({credentialRef:'local-fixture-v1',version:'1',ssid:request.body.ssid,security:request.body.security,source:'LOCAL',status:'INSTALLED'});body={credentialRef:'local-fixture-v1',version:'1'};break;
   case 'VALIDATE_NETWORK':assert.equal(request.body.network.wifiSta.credentialRef,'local-fixture-v1');body={validated:true};break;
   case 'CREATE_CANDIDATE':gateway.transaction={transactionId:'fixture-transaction-1',source:'LOCAL',state:'DRAFT',network:request.body.network};body=gateway.transaction;break;
   case 'SAVE_LOCAL_ONLY':assert.equal(request.body.acknowledgeUnverified,true);gateway.transaction.state='COMMITTED';gateway.transaction.code='LOCAL_ONLY';gateway.desiredLocalNetwork=gateway.transaction.network;gateway.actualNetwork=gateway.desiredLocalNetwork;gateway.networkGeneration='1';gateway.localRevision='1';gateway.localOverride=true;body=gateway.transaction;break;
   case 'GET_TRANSACTION':assert.equal(request.body.transactionId,'fixture-transaction-1');body=gateway.transaction;break;
   case 'CLOSE_WINDOW':closed=true;body={closing:true};break;
   default:throw Error('Unexpected operation '+request.operation);
  }
  const reply=Buffer.from(JSON.stringify({apiVersion:'1.0',requestId:request.requestId,code,body}));iv.writeUInt32BE(sequence++,8);
  const encrypt=createCipheriv('aes-256-gcm',srp.key,iv);res.end(Buffer.concat([encrypt.update(reply),encrypt.final(),encrypt.getAuthTag()]));
 }catch(error){console.error('Mock server failure:',error.message);res.writeHead(500);res.end('mock failure');}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const browser=await playwright.chromium.launch({headless:true,executablePath:process.env.GW_TEST_BROWSER||'C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe'});
try{
 const page=await browser.newPage({viewport:{width:390,height:844}}),errors=[];page.on('pageerror',error=>errors.push(error.message));page.on('dialog',dialog=>dialog.accept());
 await page.goto(`http://127.0.0.1:${server.address().port}/`);
 await page.locator('#username').fill(username);await page.locator('#setup-password').fill(password);await page.locator('#login button').click();
 await page.locator('#config-panel').waitFor({state:'visible'});await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('网络版本 0'));
 await page.locator('#scan').click();await page.waitForFunction(()=>document.querySelector('#ap-list').options.length===3);
 assert.equal(await page.locator('#ap-list img').count(),0);
 await page.locator('#password-action').selectOption('REPLACE');await page.locator('#wifi-password').fill('A-Fixture-Wifi-Password');
 await page.locator('#preview').click();await page.locator('#preview-panel').waitFor({state:'visible'});
 assert.equal(await page.locator('#wifi-password').inputValue(),'');assert.ok(!(await page.locator('#preview-output').textContent()).includes('Password'));
 await page.locator('#local-only').click();await page.waitForFunction(()=>document.querySelector('#connection').textContent.includes('网络版本 1'));
 await page.locator('#query').click();await page.waitForFunction(()=>document.querySelector('#result').textContent.includes('LOCAL_ONLY'));
 await page.setViewportSize({width:1366,height:900});assert.ok(await page.locator('#close').isVisible());
 await page.locator('#close').click();await page.waitForFunction(()=>document.querySelector('#notice').textContent.includes('配网窗口已关闭'));
 assert.deepEqual(errors,[]);assert.ok(closed);
 for(const op of ['GET_STATE','SCAN_WIFI','PUT_CREDENTIAL','VALIDATE_NETWORK','CREATE_CANDIDATE','SAVE_LOCAL_ONLY','GET_TRANSACTION','CLOSE_WINDOW'])assert.ok(operations.includes(op),op);
 console.log('Browser smoke PASS: actual offline bundle, Security2 handshake, AES-GCM, mobile/desktop, scan, secret install, preview, local save, query, close. Host simulator only.');
}finally{await browser.close();await new Promise(resolve=>server.close(resolve));}
