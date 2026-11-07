<script setup lang="ts">
import { computed, onMounted, reactive, ref } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api, newKey } from '@/lib/api'
import { can } from '@/lib/auth'
import { dateTime, pretty, statusLabel } from '@/lib/format'
import CursorFooter from './CursorFooter.vue'
import { errorText, listText, mutationKey, useCursor, type RecordData } from './helpers'

const list=useCursor(()=>'/api/v1/rule-bundles'),selected=ref<RecordData|null>(null),detailLoading=ref(false),error=ref(''),busy=ref(false)
const parsers=ref<RecordData[]>([]),targets=ref<RecordData[]>([]),nodes=ref<RecordData[]>([]),nodeId=ref(''),operation=ref<RecordData|null>(null),choicesLoading=ref(true)
const editorOpen=ref(false),name=ref(''),description=ref(''),rules=ref<RecordData[]>([]),editorError=ref(''),saveKey=mutationKey()
const mayWrite=computed(()=>can('rule:write','scope_instance'))
const targetOptions=computed(()=>targets.value.flatMap(t=>(t.versions || []).map((v:RecordData)=>({key:`${t.targetId}@${v.version}`,targetId:t.targetId,version:v.version,mappingVersion:v.configuration.mappingVersion,label:`${t.name || t.targetId} · v${v.version} · ${v.configuration.mappingVersion}`}))))
const node=computed(()=>nodes.value.find(v=>v.dataNodeId===nodeId.value))
const activationRequests=new Map<string,{body:RecordData;etag:string;key:string}>()
let selectionEpoch=0
async function loadAll(path:string){let out:RecordData[]=[];let cursor='';do{const p=await api(`${path}?limit=200${cursor?'&cursor='+encodeURIComponent(cursor):''}`);out=out.concat(p.items || []);cursor=p.nextCursor || '';if(out.length>=10000)break}while(cursor);return out}
async function refreshSystem(){if(!can('system:read','scope_instance'))return;const status=await api('/api/v1/system/status');nodes.value=status.dataNodes || [];if(!nodes.value.some(n=>n.dataNodeId===nodeId.value))nodeId.value=nodes.value[0]?.dataNodeId || ''}
async function initialize(){choicesLoading.value=true;error.value='';await list.refresh();try{if(can('parser:read'))parsers.value=await loadAll('/api/v1/parsers');if(can('target:read','scope_instance'))targets.value=await loadAll('/api/v1/targets');await refreshSystem()}catch(e){error.value=errorText(e)}finally{choicesLoading.value=false}}
async function select(row:RecordData){if(busy.value)return;const current=++selectionEpoch;detailLoading.value=true;selected.value=null;error.value='';operation.value=null;try{const response=await api(`/api/v1/rule-bundles/${encodeURIComponent(row.bundleId)}`);if(current===selectionEpoch)selected.value=response}catch(e){if(current===selectionEpoch)error.value=errorText(e)}finally{if(current===selectionEpoch)detailLoading.value=false}}
function newRule(){return {ruleId:`rule_${crypto.randomUUID().replaceAll('-','').slice(0,16)}`,priority:100,modelIds:'',groupIds:'',protocolVersions:'',packetTypes:'',action:'DECODE',parserKey:parsers.value[0]?`${parsers.value[0].parserId}@${parsers.value[0].version}`:'',targetKeys:[],targetRequired:{}}}
function openEditor(copy=false){
  editorError.value='';saveKey.clear();name.value='';description.value=''
  rules.value=copy&&selected.value ? selected.value.rules.map((r:RecordData)=>({ruleId:r.ruleId,priority:r.priority,modelIds:(r.match.modelIds || []).join(', '),groupIds:(r.match.groupIds || []).join(', '),protocolVersions:(r.match.protocolVersions || []).join(', '),packetTypes:(r.match.packetTypes || []).join(', '),action:r.action,parserKey:r.parserId?`${r.parserId}@${r.parserVersion}`:'',targetKeys:(r.targetVersions || []).map((t:RecordData)=>`${t.targetId}@${t.version}`),targetRequired:Object.fromEntries((r.targetVersions || []).map((t:RecordData)=>[`${t.targetId}@${t.version}`,t.required]))})) : [newRule()]
  editorOpen.value=true
}
function buildRule(r:RecordData){
  const match:RecordData={modelIds:listText(r.modelIds)}
  if(!match.modelIds.length)throw new Error('每条规则至少指定一个网关型号')
  for(const k of ['groupIds','protocolVersions','packetTypes']){const values=listText(r[k]);if(values.length)match[k]=values}
  const rule:RecordData={ruleId:r.ruleId.trim(),priority:r.priority,match,action:r.action,targetVersions:[]}
  if(!/^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/.test(rule.ruleId))throw new Error('规则 ID 只能使用字母、数字、下划线、点、冒号和连字符')
  if(r.action==='DECODE'){const p=parsers.value.find(p=>`${p.parserId}@${p.version}`===r.parserKey);if(!p)throw new Error('请选择已安装的解析器版本');Object.assign(rule,{parserId:p.parserId,parserVersion:p.version,schemaVersion:p.schemaVersion})}
  if(r.action!=='QUARANTINE'){
    if(!r.targetKeys.length || r.targetKeys.length>8)throw new Error('透传和解码规则必须选择 1–8 个目标版本')
    rule.targetVersions=r.targetKeys.map((key:string)=>{const t=targetOptions.value.find(v=>v.key===key);if(!t)throw new Error('目标版本已不可用，请刷新选择器');return {targetId:t.targetId,version:t.version,mappingVersion:t.mappingVersion,required:r.targetRequired[key]!==false}})
  }
  return rule
}
async function save(){
  busy.value=true;editorError.value=''
  try{if(!name.value.trim())throw new Error('请输入本次规则包名称');if(!rules.value.length || rules.value.length>200)throw new Error('规则包必须包含 1–200 条规则');const compiled=rules.value.map(buildRule);if(new Set(compiled.map(r=>r.ruleId)).size!==compiled.length)throw new Error('同一包内规则 ID 不能重复');const body={name:name.value.trim(),description:description.value,defaultAction:'QUARANTINE',rules:compiled};const created=await api('/api/v1/rule-bundles','POST',body,{key:saveKey.for(body)});saveKey.clear();editorOpen.value=false;selectionEpoch++;detailLoading.value=false;selected.value=created;operation.value=null;await list.refresh();ElMessage.success('已创建不可变规则包，下一步请验证并激活')}catch(e){editorError.value=errorText(e)}finally{busy.value=false}
}
async function validate(){if(!selected.value || busy.value)return;const bundle=selected.value.bundleId;busy.value=true;error.value='';try{operation.value=await api(`/api/v1/rule-bundles/${encodeURIComponent(bundle)}/validations`,'POST',{});selected.value=await api(`/api/v1/rule-bundles/${encodeURIComponent(bundle)}`);await list.refresh();ElMessage.success('规则包验证完成')}catch(e){error.value=errorText(e)}finally{busy.value=false}}
async function activate(){
  if(!selected.value || busy.value)return;const bundle=selected.value.bundleId;error.value='';busy.value=true
  let requestId=''
  try{
    await refreshSystem();if(!node.value)throw new Error('当前没有可激活的数据节点')
    const current={...node.value};requestId=`${bundle}:${current.dataNodeId}`
    const pending=activationRequests.get(requestId)
    await ElMessageBox.confirm(pending?`将重试上次对 ${bundle} 的激活请求，保留原始修订与幂等身份以核实结果。`:`将节点 ${current.dataNodeId} 从 ${current.activeBundleId || '未激活'} 切换到 ${bundle}。新接管事件使用该包，已有事件保留原处理版本。`,'激活规则包',{confirmButtonText:'确认激活',cancelButtonText:'取消',type:'warning'})
    const request=pending || {body:{dataNodeId:current.dataNodeId,expectedActivationRevision:current.activationRevision},etag:current.activationEtag,key:newKey()}
    activationRequests.set(requestId,request)
    operation.value=await api(`/api/v1/rule-bundles/${encodeURIComponent(bundle)}/activations`,'POST',request.body,{etag:request.etag,key:request.key})
    activationRequests.delete(requestId);await refreshSystem();selected.value=await api(`/api/v1/rule-bundles/${encodeURIComponent(bundle)}`);await list.refresh();ElMessage.success('节点激活状态已更新')
  }catch(e){if(e!=='cancel'&&e!=='close'){const status=(e as {status?:number}).status;if(status&&status<500&&status!==409&&status!==429)activationRequests.delete(requestId);error.value=errorText(e)}}finally{busy.value=false}
}
onMounted(initialize)
</script>
<template>
  <div class="page-heading"><div><h1>解析规则包</h1><p class="muted">按不可变版本定义透传、解码、隔离和转发目标。</p></div><div class="toolbar"><el-button @click="initialize" :loading="list.loading.value">刷新</el-button><el-button v-if="mayWrite" type="primary" :disabled="choicesLoading" @click="openEditor()">创建规则包</el-button></div></div>
  <el-alert v-if="list.error.value || error" :title="list.error.value || error" type="error" :closable="false" show-icon/>
  <section v-if="nodes.length" class="panel"><div class="toolbar"><span>当前数据节点</span><el-select v-if="nodes.length>1" v-model="nodeId"><el-option v-for="n in nodes" :key="n.dataNodeId" :value="n.dataNodeId"/></el-select><strong v-else>{{ nodeId }}</strong><span class="muted">激活修订 {{ node?.activationRevision }}</span></div><p class="mono">当前规则包：{{ node?.activeBundleId || '未激活' }}</p></section>
  <section class="panel"><el-table :data="list.items.value" v-loading="list.loading.value" highlight-current-row @row-click="select" empty-text="还没有规则包"><el-table-column prop="bundleId" label="规则包 ID" min-width="240"><template #default="{row}"><el-button link type="primary" @click.stop="select(row)">{{ row.bundleId }}</el-button></template></el-table-column><el-table-column label="状态" width="130"><template #default="{row}">{{ statusLabel(row.state) }}</template></el-table-column><el-table-column label="规则数" width="90"><template #default="{row}">{{ row.rules.length }}</template></el-table-column><el-table-column label="创建时间" min-width="180"><template #default="{row}">{{ dateTime(row.createdAt) }}</template></el-table-column><el-table-column label="摘要" min-width="180"><template #default="{row}"><code>{{ row.contentDigest.slice(0,16) }}…</code></template></el-table-column></el-table><CursorFooter :count="list.items.value.length" :page="list.index.value" :next="list.nextCursor.value" :snapshot="list.snapshotAt.value" :loading="list.loading.value" @next="list.next()" @previous="list.previous()" @refresh="list.refresh()"/></section>
  <section v-if="selected" class="panel" v-loading="detailLoading"><div class="section-title"><div><h2>规则包详情</h2><p class="mono">{{ selected.bundleId }}</p></div><div class="toolbar"><el-button v-if="mayWrite" :disabled="choicesLoading" @click="openEditor(true)">复制为新包</el-button><el-button v-if="can('rule:validate','scope_instance')" :loading="busy" @click="validate">验证</el-button><el-button v-if="can('rule:activate','scope_instance')" type="primary" :loading="busy" :disabled="!['VALIDATED','RELEASED'].includes(selected.state)" @click="activate">激活</el-button></div></div><el-table :data="selected.rules"><el-table-column prop="priority" label="优先级" width="90"/><el-table-column prop="ruleId" label="规则" min-width="170"/><el-table-column label="型号 / 协议" min-width="200"><template #default="{row}">{{ row.match.modelIds.join(', ') }}<div class="muted">{{ row.match.protocolVersions?.join(', ') || '全部协议' }}</div></template></el-table-column><el-table-column prop="action" label="动作" width="130"/><el-table-column label="解析器" min-width="170"><template #default="{row}">{{ row.parserId?`${row.parserId}@${row.parserVersion}`:'—' }}</template></el-table-column><el-table-column label="目标版本" min-width="180"><template #default="{row}"><div v-for="t in row.targetVersions" :key="t.targetId">{{ t.targetId }} · v{{ t.version }}</div></template></el-table-column></el-table><p class="muted">未匹配事件：隔离。更新内容需要创建新包。</p><el-alert v-if="operation" :title="`最近操作：${statusLabel(operation.state)} · ${operation.operationId}`" type="info" :closable="false"/><el-collapse><el-collapse-item title="完整规则与依赖摘要"><pre>{{ pretty(selected) }}</pre></el-collapse-item></el-collapse></section>
  <el-drawer v-model="editorOpen" title="创建新的规则包" size="min(1080px, 97vw)" :close-on-click-modal="false">
    <el-alert v-if="editorError" :title="editorError" type="error" :closable="false" show-icon/>
    <el-form label-position="top" :disabled="busy"><div class="form-grid"><el-form-item label="本次名称" required><el-input v-model="name" maxlength="100" placeholder="用于本次创建；列表以规则包 ID 识别"/></el-form-item><el-form-item label="说明"><el-input v-model="description" maxlength="500"/></el-form-item></div>
      <section class="rule-editor" v-for="(r,index) in rules" :key="index"><div class="section-title"><h3>规则 {{ index+1 }}</h3><el-button type="danger" link @click="rules.splice(index,1)">移除此规则</el-button></div><div class="form-grid"><el-form-item label="规则 ID" required><el-input v-model="r.ruleId" maxlength="128"/></el-form-item><el-form-item label="优先级（较大先匹配）"><el-input-number v-model="r.priority" :min="0" :max="100000"/></el-form-item><el-form-item label="网关型号，逗号分隔" required><el-input v-model="r.modelIds" placeholder="例如 GW32I33V2"/></el-form-item><el-form-item label="分组 ID，可选"><el-input v-model="r.groupIds" placeholder="逗号分隔，留空表示全部可匹配组"/></el-form-item><el-form-item label="协议版本，可选"><el-input v-model="r.protocolVersions" placeholder="如 ble-ad-v1"/></el-form-item><el-form-item label="包类型，可选"><el-input v-model="r.packetTypes" placeholder="逗号分隔"/></el-form-item><el-form-item label="处理动作"><el-select v-model="r.action"><el-option label="解码后转发" value="DECODE"/><el-option label="原文透传" value="PASSTHROUGH"/><el-option label="隔离" value="QUARANTINE"/></el-select></el-form-item><el-form-item v-if="r.action==='DECODE'" label="解析器版本" required><el-select v-model="r.parserKey"><el-option v-for="p in parsers" :key="`${p.parserId}@${p.version}`" :label="`${p.parserId}@${p.version} · ${p.schemaVersion}`" :value="`${p.parserId}@${p.version}`"/></el-select></el-form-item></div><el-form-item v-if="r.action!=='QUARANTINE'" label="转发目标版本（最多 8 个）" required><el-select v-model="r.targetKeys" multiple filterable :multiple-limit="8" style="width:100%"><el-option v-for="t in targetOptions" :key="t.key" :label="t.label" :value="t.key"/></el-select></el-form-item><div v-if="r.action!=='QUARANTINE'" class="target-flags"><el-checkbox v-for="targetKey in r.targetKeys" :key="targetKey" :model-value="r.targetRequired[targetKey]!==false" @change="value=>r.targetRequired[targetKey]=value">必达：{{ targetOptions.find(t=>t.key===targetKey)?.label || targetKey }}</el-checkbox></div></section>
      <el-button :disabled="rules.length>=200" @click="rules.push(newRule())">添加规则</el-button><p class="muted">默认动作固定为隔离。目标与字段映射版本会随规则包冻结。</p>
    </el-form><template #footer><el-button @click="editorOpen=false" :disabled="busy">取消</el-button><el-button type="primary" @click="save" :loading="busy">保存新规则包</el-button></template>
  </el-drawer>
</template>
<style scoped>h1{font-size:26px;margin:0}h2{font-size:18px;margin:0}h3{font-size:16px;margin:0}.panel{margin-bottom:18px}.mono{overflow-wrap:anywhere}.rule-editor{border:1px solid var(--el-border-color);border-radius:8px;padding:18px;margin-bottom:18px}.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 18px}.form-grid :deep(.el-select){width:100%}.target-flags{display:flex;flex-direction:column;gap:8px}.target-flags :deep(.el-checkbox){height:auto;white-space:normal;margin:0}.target-flags :deep(.el-checkbox__label){white-space:normal;overflow-wrap:anywhere}pre{white-space:pre-wrap;overflow-wrap:anywhere}@media(max-width:700px){.form-grid{grid-template-columns:minmax(0,1fr)}}</style>
