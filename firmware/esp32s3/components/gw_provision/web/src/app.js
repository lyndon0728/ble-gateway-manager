import { ProvisionClient, requestId, utf8 } from './security2.js';
const client = new ProvisionClient();
const $ = id => document.getElementById(id);
let state = {}, credentialRef = '', candidate = null, prepared = null;
let transactionId = sessionStorage.getItem('gw32.transactionId') || '';
let busy = false, polling = false;
function notice(message) { $('notice').textContent = message || ''; }
function show(value) { $('result').textContent = JSON.stringify(value, null, 2); }
function remember(id) { if (id) { transactionId=id;sessionStorage.setItem('gw32.transactionId',id);$('transaction-id').textContent=`事务：${id}`; } }
function bodyOf(response) { if(response.code!=='OK' && response.code!=='ACCEPTED') throw Error(`${response.code}${response.body?.message?': '+response.body.message:''}`);return response.body || {}; }
function envelope(operation,body={}) {
  return {apiVersion:'1.0',requestId:requestId(),operation,expectedNetworkGeneration:String(state.networkGeneration||'0'),baseLocalRevision:String(state.localRevision||'0'),body};
}
async function call(operation,body={}) { const result=await client.call(envelope(operation,body));show(result);return bodyOf(result); }
async function run(action) {
  if(busy)return;busy=true;notice('');
  try{await action();}catch(e){notice(e.message);if(!client.channel){$('login-panel').hidden=false;$('config-panel').hidden=true;}}
  finally{busy=false;}
}
function ipRead(name) {
  const box=document.querySelector(`[data-interface="${name}"]`),get=key=>box.querySelector(`[data-ip="${key}"]`).value.trim();
  if(get('mode')==='DHCP')return {mode:'DHCP'};
  const address=get('address'),gateway=get('gateway'),prefixLength=Number(get('prefixLength')),dns=get('dns').split(',').map(x=>x.trim()).filter(Boolean);
  return {mode:'STATIC',address,prefixLength,gateway,dns};
}
function ipWrite(name,value={mode:'DHCP'}) {
  const box=document.querySelector(`[data-interface="${name}"]`);
  for(const key of ['mode','address','prefixLength','gateway','dns'])box.querySelector(`[data-ip="${key}"]`).value=key==='dns'?(value.dns||[]).join(','):(value[key]??(key==='prefixLength'?24:''));
  box.querySelector('[data-static]').hidden=value.mode!=='STATIC';
}
function networkRead() {
  const uplinkOrder=$('order').value.split(','),wifi=uplinkOrder.includes('WIFI_STA');
  if(wifi && (utf8.encode($('ssid').value).length<1||utf8.encode($('ssid').value).length>32))throw Error('网络名称必须为 1～32 字节。');
  if(wifi && !credentialRef)throw Error('请安装此网络的新密码。');
  return {uplinkOrder,ethernet:{enabled:uplinkOrder.includes('ETHERNET'),ipv4:ipRead('ethernet')},wifiSta:{enabled:wifi,ssid:$('ssid').value,security:$('security').value,credentialRef,ipv4:ipRead('wifiSta')}};
}
function loadNetwork(network) {
  if(!network)return;
  $('order').value=(network.uplinkOrder||['WIFI_STA']).join(',');
  $('ssid').value=network.wifiSta?.ssid||'';$('security').value=network.wifiSta?.security||'WPA2_PSK';credentialRef=network.wifiSta?.credentialRef||'';
  ipWrite('wifiSta',network.wifiSta?.ipv4);ipWrite('ethernet',network.ethernet?.ipv4);
}
async function refresh(loadForm=false) {
  state=await call('GET_STATE');
  const info=[`本地版本 ${state.localRevision||'0'}`,`网络版本 ${state.networkGeneration||'0'}`];
  if(state.localOverride)info.push('本地修改待平台同步');
  if(state.verificationState)info.push(state.verificationState);
  if(state.activeInterface)info.push(`当前接口 ${state.activeInterface}`);
  $('connection').textContent=info.join(' · ');
  if(loadForm)loadNetwork(state.desiredLocalNetwork||state.activeLocalSnapshot||state.network||state.actualNetwork);
  const credentials=$('credential-list');credentials.replaceChildren(new Option('选择已安装密码或安装新密码',''));
  for(const item of state.credentials||[]){const option=new Option(`${item.ssid} · ${item.security} · v${item.version}`,item.credentialRef);credentials.append(option);}
  credentials.value=credentialRef;
  if(state.transaction?.source==='LOCAL')remember(state.transaction.transactionId);
}
function invalidate() {prepared=null;candidate=null;$('apply').disabled=true;$('local-only').disabled=true;$('preview-panel').hidden=true;}
async function prepare() {
  if($('password-action').value==='REPLACE') {
    const password=$('wifi-password').value,length=utf8.encode(password).length;
    if(length<8||length>63)throw Error('Wi-Fi 密码必须为 8～63 字节。');
    const installed=await call('PUT_CREDENTIAL',{ssid:$('ssid').value,security:$('security').value,password});
    credentialRef=installed.credentialRef;
    $('wifi-password').value='';$('password-action').value='KEEP';$('password-label').hidden=true;
  }
  prepared=networkRead();await call('VALIDATE_NETWORK',{network:prepared});
  $('preview-output').textContent=JSON.stringify(prepared,null,2);$('preview-panel').hidden=false;$('apply').disabled=false;$('local-only').disabled=false;
}
async function create() {
  if(!prepared)throw Error('请先检查并预览参数');
  if(!candidate){candidate=await call('CREATE_CANDIDATE',{network:prepared});remember(candidate.transactionId);}
  if(!transactionId)throw Error('设备未返回事务编号');
}
async function query() {if(!transactionId)throw Error('没有已记录的事务');const result=await call('GET_TRANSACTION',{transactionId});remember(transactionId);if(['COMMITTED','ROLLED_BACK','CANCELLED'].includes(result.state)){polling=false;await refresh();}return result;}
$('login').addEventListener('submit',event=>{event.preventDefault();run(async()=>{
  const password=$('setup-password').value;$('setup-password').value='';await client.login($('username').value,password);
  $('login-panel').hidden=true;$('config-panel').hidden=false;await refresh(true);remember(transactionId);
});});
$('network-form').addEventListener('submit',event=>{event.preventDefault();run(prepare);});
$('network-form').addEventListener('input',invalidate);
$('password-action').addEventListener('change',()=>{$('password-label').hidden=$('password-action').value!=='REPLACE';});
$('credential-list').addEventListener('change',()=>{credentialRef=$('credential-list').value;invalidate();});
for(const box of document.querySelectorAll('.ip-settings'))box.querySelector('[data-ip="mode"]').addEventListener('change',()=>{box.querySelector('[data-static]').hidden=box.querySelector('[data-ip="mode"]').value!=='STATIC';});
$('scan').addEventListener('click',()=>run(async()=>{
  const result=await call('SCAN_WIFI',{mode:$('scan-mode').value}),aps=Array.isArray(result)?result:(result.accessPoints||result.aps||[]);
  $('ap-list').replaceChildren(new Option('手动填写或选择网络',''));
  for(const ap of aps){if(!ap.ssid)continue;const option=new Option(`${ap.ssid} · ${ap.rssi} dBm · ${ap.security||ap.authMode||'未知认证'}`,ap.ssid);option.dataset.security=ap.security||'';$('ap-list').append(option);}
}));
$('ap-list').addEventListener('change',()=>{const option=$('ap-list').selectedOptions[0];if(option.value){$('ssid').value=option.value;if(['WPA2_PSK','WPA3_SAE'].includes(option.dataset.security))$('security').value=option.dataset.security;invalidate();}});
$('refresh').addEventListener('click',()=>run(()=>refresh(true)));
$('apply').addEventListener('click',()=>run(async()=>{await create();await call('APPLY_ONLINE',{transactionId});polling=true;notice('正在验证新网络。热点可能短暂断开，重连后登录并查询此事务。');invalidate();}));
$('local-only').addEventListener('click',()=>run(async()=>{
  if(!confirm('仅保存本地目标，平台连接尚未验证，也尚未同步平台配置。继续保存？'))return;
  await create();await call('SAVE_LOCAL_ONLY',{transactionId,acknowledgeUnverified:true});invalidate();await refresh();
}));
$('query').addEventListener('click',()=>run(query));
$('cancel').addEventListener('click',()=>run(async()=>{if(!transactionId)throw Error('没有已记录的事务');await call('CANCEL_TRANSACTION',{transactionId});invalidate();await refresh();}));
$('restore').addEventListener('click',()=>run(async()=>{
  if(!confirm('以此前可用配置建立新的恢复事务？'))return;
  const result=await call('RESTORE_PREVIOUS',{});remember(result.transactionId);notice('恢复候选已创建；查询结果，确认后按设备返回的阶段验证应用。');
  if(result.transactionId)await call('APPLY_ONLINE',{transactionId:result.transactionId});
}));
$('close').addEventListener('click',()=>run(async()=>{if(!confirm('关闭热点并退出？未提交配置将回退。'))return;await call('CLOSE_WINDOW');notice('配网窗口已关闭，可以重新连接日常网络。');$('config-panel').hidden=true;client.channel?.destroy();client.channel=null;}));
setInterval(()=>{if(polling && !busy && client.channel)run(query);},2500);
