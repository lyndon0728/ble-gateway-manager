<script setup lang="ts">
import { computed, onMounted, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api } from '@/lib/api'
import { can, canGateway } from '@/lib/auth'
import { dateTime, pretty, statusLabel } from '@/lib/format'
import CursorFooter from '@/features/pipeline/CursorFooter.vue'
import StructuredRecord from '@/features/pipeline/StructuredRecord.vue'
import { errorText, mutationKey, useCursor, type RecordData } from '@/features/pipeline/helpers'

const route=useRoute(),router=useRouter(),targetFilter=ref(''),stateFilter=ref(''),lookupId=ref(''),targets=ref<RecordData[]>([])
const list=useCursor(()=>'/api/v1/deliveries',()=>({targetId:targetFilter.value,state:stateFilter.value}))
const drawer=ref(false),detail=ref<RecordData|null>(null),gateway=ref<RecordData|null>(null),loading=ref(false),error=ref('')
const payload=ref<RecordData|null>(null),payloadLoading=ref(false),payloadError=ref(''),tab=ref('detail')
const reason=ref(''),retryBusy=ref(false),retryKey=mutationKey()
let generation=0
const states=['PENDING','IN_FLIGHT','RETRY_WAIT','DELIVERED','TERMINATED']
const mayRaw=computed(()=>canGateway('data:raw',gateway.value))
const mayRetry=computed(()=>canGateway('delivery:retry',gateway.value)&&detail.value&&['PENDING','RETRY_WAIT','IN_FLIGHT'].includes(detail.value.state))
async function openDetail(id:string,preserve=false){
  const current=++generation;drawer.value=true;loading.value=true;error.value='';if(!preserve){detail.value=null;gateway.value=null;payload.value=null;payloadError.value='';reason.value='';retryKey.clear();tab.value='detail'}
  try{const result=await api(`/api/v1/deliveries/${encodeURIComponent(id)}`);if(current!==generation)return;detail.value=result;if(can('gateway:read')){const g=await api(`/api/v1/gateways/${encodeURIComponent(result.gatewayId)}`);if(current===generation)gateway.value=g}}
  catch(e){if(current===generation)error.value=errorText(e)}finally{if(current===generation)loading.value=false}
}
async function loadPayload(){
  if(!detail.value || !mayRaw.value)return;const current=generation;payloadLoading.value=true;payloadError.value=''
  try{const result=await api(`/api/v1/deliveries/${encodeURIComponent(detail.value.deliveryId)}/payload`);if(current===generation)payload.value=result}catch(e){if(current===generation)payloadError.value=errorText(e)}finally{if(current===generation)payloadLoading.value=false}
}
async function retry(){
  if(!detail.value || !mayRetry.value)return;error.value='';retryBusy.value=true
  try{if(!reason.value.trim())throw new Error('请填写人工重试原因');const id=detail.value.deliveryId,body={reason:reason.value.trim()};await ElMessageBox.confirm(`重新调度投递 ${id}。将复用原始 deliveryId、处理修订、目标版本和冻结报文；客户系统需按 deliveryId 去重。`,'确认人工重试',{type:'warning',confirmButtonText:'重新调度',cancelButtonText:'取消'});await api(`/api/v1/deliveries/${encodeURIComponent(id)}/retries`,'POST',body,{key:retryKey.for(body)});retryKey.clear();ElMessage.success('重试已调度，等待客户应用确认');await openDetail(id,true);await list.refresh()}
  catch(e){if(e!=='cancel'&&e!=='close')error.value=errorText(e)}finally{retryBusy.value=false}
}
async function loadTargets(){if(!can('target:read','scope_instance'))return;try{let cursor='';do{const p=await api(`/api/v1/targets?limit=200${cursor?'&cursor='+encodeURIComponent(cursor):''}`);targets.value.push(...p.items);cursor=p.nextCursor||'';if(targets.value.length>=10000)break}while(cursor)}catch{/* An exact target ID filter remains usable without the optional catalog. */}}
function eventLink(){if(detail.value)router.push(`/gateways/${encodeURIComponent(detail.value.gatewayId)}/events/${encodeURIComponent(detail.value.sourceEventId)}`)}
watch(()=>route.query.targetId,()=>{targetFilter.value=String(route.query.targetId||'');void list.refresh()},{immediate:true})
watch(drawer,open=>{if(!open){generation++;payload.value=null;payloadLoading.value=false}})
onMounted(loadTargets)
</script>
<template>
  <div class="page-heading"><div><h1>投递记录</h1><p class="muted">跟踪客户系统的应用确认，以及相同冻结报文的可靠重试。</p></div><el-button @click="list.refresh()" :loading="list.loading.value">刷新</el-button></div>
  <section class="panel"><el-form inline class="toolbar" @submit.prevent="list.refresh()"><el-form-item label="目标"><el-select v-if="targets.length" v-model="targetFilter" clearable filterable allow-create default-first-option placeholder="全部可见目标" style="width:260px"><el-option v-for="t in targets" :key="t.targetId" :value="t.targetId" :label="t.name || t.targetId"/></el-select><el-input v-else v-model="targetFilter" clearable placeholder="目标 ID，可留空"/></el-form-item><el-form-item label="状态"><el-select v-model="stateFilter" clearable placeholder="全部状态" style="width:165px"><el-option v-for="s in states" :key="s" :value="s" :label="statusLabel(s)"/></el-select></el-form-item><el-form-item><el-button native-type="submit" type="primary" :loading="list.loading.value">查询</el-button></el-form-item></el-form>
    <el-alert v-if="list.error.value" :title="list.error.value" type="error" :closable="false" show-icon/>
    <el-table :data="list.items.value" v-loading="list.loading.value" row-key="deliveryId" empty-text="当前范围没有投递记录"><el-table-column label="投递 ID" min-width="245"><template #default="{row}"><el-button link type="primary" @click="openDetail(row.deliveryId)">{{ row.deliveryId }}</el-button></template></el-table-column><el-table-column label="客户目标" min-width="200"><template #default="{row}">{{ targets.find(t=>t.targetId===row.targetId)?.name || row.targetId }}<div class="muted">目标 v{{ row.targetVersion }} · 映射 {{ row.mappingVersion }}</div></template></el-table-column><el-table-column label="源事件" min-width="215"><template #default="{row}">{{ row.sourceEventId }}<div class="muted">{{ row.gatewayId }} · 修订 {{ row.processingRevision }}</div></template></el-table-column><el-table-column label="状态" width="130"><template #default="{row}"><el-tag :type="row.state==='DELIVERED'?'success':row.state==='TERMINATED'?'danger':'info'">{{ statusLabel(row.state) }}</el-tag></template></el-table-column><el-table-column prop="attempts" label="尝试次数" width="100"/><el-table-column label="下次尝试 / 已确认" min-width="180"><template #default="{row}">{{ dateTime(row.confirmedAt || row.nextAttemptAt) }}</template></el-table-column><el-table-column prop="lastErrorCode" label="最后错误" min-width="180"/></el-table>
    <CursorFooter :count="list.items.value.length" :page="list.index.value" :next="list.nextCursor.value" :snapshot="list.snapshotAt.value" :loading="list.loading.value" @next="list.next()" @previous="list.previous()" @refresh="list.refresh()"/>
  </section>
  <section class="panel"><el-form inline @submit.prevent="lookupId.trim()&&openDetail(lookupId.trim())"><el-form-item label="按投递 ID 查询"><el-input v-model="lookupId" placeholder="delivery_…" clearable/></el-form-item><el-form-item><el-button native-type="submit" :disabled="!lookupId.trim()">打开详情</el-button></el-form-item></el-form></section>
  <el-drawer v-model="drawer" title="投递详情" size="min(890px, 97vw)" :close-on-click-modal="false" :close-on-press-escape="!retryBusy" :show-close="!retryBusy">
    <el-alert v-if="error" :title="error" type="error" :closable="false" show-icon/>
    <div v-loading="loading"><template v-if="detail"><div class="toolbar detail-heading"><el-tag :type="detail.state==='DELIVERED'?'success':'info'">{{ statusLabel(detail.state) }}</el-tag><el-button @click="openDetail(detail.deliveryId,true)" :disabled="retryBusy">刷新详情</el-button><el-button v-if="canGateway('data:read',gateway)" @click="eventLink">查看源事件</el-button><el-button v-if="can('target:read','scope_instance')" @click="router.push(`/targets/${encodeURIComponent(detail.targetId)}`)">目标配置</el-button></div>
      <el-tabs v-model="tab"><el-tab-pane label="状态与身份" name="detail"><el-descriptions :column="1" border><el-descriptions-item label="投递 ID"><span class="mono">{{ detail.deliveryId }}</span></el-descriptions-item><el-descriptions-item label="网关 / 源事件">{{ detail.gatewayId }} / {{ detail.sourceEventId }}</el-descriptions-item><el-descriptions-item label="处理修订 / 记录">{{ detail.processingRevision }} / {{ detail.recordKey }}</el-descriptions-item><el-descriptions-item label="目标版本">{{ detail.targetId }} · v{{ detail.targetVersion }}</el-descriptions-item><el-descriptions-item label="映射版本">{{ detail.mappingVersion }}</el-descriptions-item><el-descriptions-item label="尝试次数">{{ detail.attempts }}</el-descriptions-item><el-descriptions-item label="下次尝试">{{ dateTime(detail.nextAttemptAt) }}</el-descriptions-item><el-descriptions-item label="应用确认时间">{{ dateTime(detail.confirmedAt) }}</el-descriptions-item><el-descriptions-item label="最后错误">{{ detail.lastErrorCode || '—' }}</el-descriptions-item><el-descriptions-item v-if="detail.terminationReason" label="终止原因">{{ detail.terminationReason }}</el-descriptions-item></el-descriptions>
        <el-alert v-if="detail.state==='IN_FLIGHT'" title="正在发送或等待应用确认。服务器会拒绝重试仍被发送进程占用的记录。" type="info" :closable="false"/>
        <section v-if="mayRetry" class="retry-panel"><h3>人工重试</h3><el-form label-position="top" :disabled="retryBusy"><el-form-item label="重试原因" required><el-input v-model="reason" type="textarea" :rows="2" maxlength="300" show-word-limit placeholder="例如 客户接口恢复，重新调度"/></el-form-item><el-button type="primary" :loading="retryBusy" :disabled="!reason.trim()" @click="retry">重新调度原投递</el-button></el-form></section>
        <p v-else-if="['DELIVERED','TERMINATED'].includes(detail.state)" class="muted">此记录已结束，不能人工重试。需要重新解析时，请从源事件发起独立预览。</p>
      </el-tab-pane><el-tab-pane label="冻结客户报文" name="payload"><el-alert v-if="!mayRaw" title="当前账号没有此网关的原文读取权限。" type="info" :closable="false"/><template v-else><p class="muted">这里读取实际持久化的客户报文，字段映射和身份不会随后续规则变更。</p><el-button v-if="!payload" type="primary" @click="loadPayload" :loading="payloadLoading">读取冻结报文</el-button><el-alert v-if="payloadError" :title="payloadError" type="error" :closable="false"/><template v-if="payload"><p class="mono muted">内容摘要 {{ payload.contentDigest }}</p><el-empty v-if="!payload.payloadAvailable" description="报文内容已按保留策略回收，投递身份仍保留"/><template v-else><StructuredRecord :value="payload.payload"/><el-collapse><el-collapse-item title="完整冻结报文 JSON"><pre>{{ pretty(payload.payload) }}</pre></el-collapse-item></el-collapse></template></template></template></el-tab-pane></el-tabs>
      <el-collapse><el-collapse-item title="完整投递元数据"><pre>{{ pretty(detail) }}</pre></el-collapse-item></el-collapse>
    </template></div>
  </el-drawer>
</template>
<style scoped>h1{font-size:26px;margin:0}.panel{margin-bottom:18px}.mono,pre{overflow-wrap:anywhere}pre{white-space:pre-wrap}.detail-heading{margin-bottom:18px}.retry-panel{border-top:1px solid var(--el-border-color);margin-top:24px;padding-top:8px}.el-alert{margin:12px 0}:deep(.el-descriptions__content){overflow-wrap:anywhere}</style>
