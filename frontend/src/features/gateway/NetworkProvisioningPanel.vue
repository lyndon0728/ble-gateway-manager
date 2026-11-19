<script setup lang="ts">
import { computed, onBeforeUnmount, ref, watch } from 'vue'
import { ElMessageBox } from 'element-plus'
import { api, apiResult, newKey } from '../../lib/api'
import { canGateway } from '../../lib/auth'
import { pretty } from '../../lib/format'
import { enc, errorText, taskIntent, type Row } from './utils'

const props=defineProps<{gateway:Row;draftNetwork?:Row}>()
const emit=defineEmits<{state:[Row|null];credential:[Row];task:[Row]}>()
const state=ref<Row|null>(null),etag=ref(''),credentials=ref<Row[]>([]),error=ref(''),notice=ref(''),busy=ref(false)
const ssid=ref(''),security=ref('WPA2_PSK'),password=ref(''),secretKey=ref(newKey())
const reconciliationIntent=taskIntent()
watch([ssid,security,password],()=>{secretKey.value=newKey()})
const canSecrets=computed(()=>canGateway('gateway:credentials',props.gateway))
const canConfigure=computed(()=>canGateway('gateway:configure',props.gateway)&&canGateway('task:create',props.gateway))
const base=computed(()=>`/api/v1/gateways/${enc(props.gateway.gatewayId)}`)
async function refresh(){error.value='';try{
 try{const response=await apiResult<Row>(base.value+'/network-state');state.value=response.data;etag.value=response.etag;emit('state',state.value)}catch(e:any){if(e.status!==404)throw e;state.value=null;etag.value='';emit('state',null)}
 credentials.value=(await api(base.value+'/network-credentials')).items
 }catch(e){error.value=errorText(e)}}
async function install(){if(!canSecrets.value)return;error.value='';notice.value='';busy.value=true
 try{const result=await api(base.value+'/network-credentials','POST',{ssid:ssid.value,security:security.value,password:password.value},{key:secretKey.value});password.value='';secretKey.value=newKey();notice.value='密码已安全保存，等待网关上线并确认安装。安装完成后才可用于配置。';await refresh();if(result.state==='INSTALLED')notice.value='凭据已安装。'}catch(e){error.value=errorText(e)}finally{busy.value=false}}
function choose(c:Row){if(c.state!=='INSTALLED')return;emit('credential',c);notice.value='已将已安装凭据填入配置草稿，请核对变更后提交。'}
function fullCandidate(){const target=structuredClone(state.value?.desiredLocalNetwork||{}),patch=props.draftNetwork||{};for(const [key,value]of Object.entries(patch)){if(key==='ethernet'||key==='wifiSta')target[key]={...target[key],...value as Row};else target[key]=value};return target}
async function reconcile(action:string){if(!state.value||!canConfigure.value)return;try{
 await ElMessageBox.confirm(action==='ADOPT_LOCAL'?'采用网关本地保存的目标，取消尚未下发的网络任务，再创建正式任务验证全部上行接口。已发送或状态不明的任务必须先处理。':'用当前编辑草稿覆盖本地修改。将取消尚未下发的网络任务；已经发送或状态不明的任务必须先处理。','确认网络对账',{confirmButtonText:'创建验证任务',cancelButtonText:'返回',type:'warning'})
 busy.value=true;error.value='';const body:Row={action,expectedNetworkGeneration:state.value.networkGeneration,snapshotId:state.value.snapshotId,cancelQueued:canGateway('task:cancel',props.gateway)}
 if(action==='APPLY_PLATFORM')body.network=fullCandidate()
 const request=reconciliationIntent({...body,etagGuard:etag.value},600),{etagGuard,...payload}=request.body
 const task=await api(base.value+'/network-reconciliation','POST',payload,{etag:etagGuard,key:request.key});reconciliationIntent.clear();emit('task',task);notice.value='对账任务已创建，等待设备验证及结果。';await refresh()
 }catch(e){if(e!=='cancel'&&e!=='close')error.value=errorText(e)}finally{busy.value=false}}
watch(()=>props.gateway.gatewayId,()=>{password.value='';secretKey.value=newKey();reconciliationIntent.clear();void refresh()},{immediate:true})
onBeforeUnmount(()=>{password.value=''})
</script>
<template><section class="network-provisioning">
 <h3>本地配置与远程网络管理</h3>
 <el-alert v-if="error" :title="error" type="error" :closable="false"/><el-alert v-if="notice" :title="notice" type="info" :closable="false"/>
 <el-button :disabled="busy" @click="refresh">刷新设备状态与凭据安装结果</el-button>
 <p v-if="!state">网关尚未上报网络状态。本地配网需连接设备的限时热点；此页面显示的平台配置不等于设备当前实际网络。</p>
 <template v-else>
  <p>网络世代 {{state.networkGeneration}} · 本地版本 {{state.localRevision}} · 当前接口 {{state.activeInterface}} · {{state.verificationState}}</p>
  <el-alert v-if="state.localOverride" title="设备存在本地修改，需选择采用本地目标或应用平台草稿，普通配置任务不会静默覆盖。" type="warning" :closable="false"/>
  <el-collapse><el-collapse-item title="本地保存目标"><pre>{{pretty(state.desiredLocalNetwork)}}</pre></el-collapse-item><el-collapse-item title="设备实际激活配置"><pre>{{pretty(state.actualNetwork)}}</pre></el-collapse-item></el-collapse>
  <el-button :disabled="busy||!canConfigure" @click="reconcile('ADOPT_LOCAL')">采用本地目标</el-button>
  <el-button :disabled="busy||!canConfigure||!draftNetwork" @click="reconcile('APPLY_PLATFORM')">应用当前平台草稿</el-button>
 </template>
 <h4>Wi-Fi 密码安装</h4><p>新密码先安全投递到指定网关。旧密码不可查看；只有设备确认安装后，凭据才能用于切换网络。</p>
 <div v-if="canSecrets" class="credential-fields"><el-input v-model="ssid" :disabled="busy" aria-label="待安装凭据的 SSID" placeholder="SSID" :maxlength="32"/><el-select v-model="security" :disabled="busy" aria-label="凭据安全模式"><el-option label="WPA2 PSK" value="WPA2_PSK"/><el-option label="WPA3 SAE" value="WPA3_SAE"/></el-select><el-input v-model="password" :disabled="busy" type="password" autocomplete="new-password" aria-label="新 Wi-Fi 密码" placeholder="新密码（8–63 UTF-8 字节）"/><el-button :loading="busy" :disabled="!ssid||!password" @click="install">安全安装新密码</el-button></div>
 <p v-else>当前账号没有安装网络凭据的权限，可选择已安装凭据。</p>
 <el-table :data="credentials"><el-table-column prop="ssid" label="SSID"/><el-table-column prop="security" label="安全模式"/><el-table-column prop="state" label="安装状态"/><el-table-column prop="source" label="来源"/><el-table-column label="操作"><template #default="{row}"><el-button :disabled="row.state!=='INSTALLED'||!canConfigure" @click="choose(row)">用于配置</el-button></template></el-table-column></el-table>
 </section></template>
<style scoped>.network-provisioning{border:1px solid #dce3eb;border-radius:8px;padding:16px;margin:16px 0}.credential-fields{display:grid;grid-template-columns:1fr 160px 1fr auto;gap:10px}.el-alert{margin-bottom:12px}pre{white-space:pre-wrap;overflow-wrap:anywhere}p{line-height:1.6}@media(max-width:800px){.credential-fields{grid-template-columns:1fr}}</style>
