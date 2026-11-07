<script setup lang="ts">
import { computed, onMounted, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api } from '@/lib/api'
import { can } from '@/lib/auth'
import { dateTime, pretty, statusLabel } from '@/lib/format'
import CursorFooter from './CursorFooter.vue'
import { useCursor, errorText, listText, mutationKey, type RecordData } from './helpers'

const route=useRoute(),router=useRouter(),activeTab=ref('quarantine'),errorCode=ref('')
const quarantine=useCursor(()=>'/api/v1/quarantine',()=>({errorCode:errorCode.value})),jobs=useCursor(()=>'/api/v1/reprocess-jobs')
const wizard=ref(false),busy=ref(false),error=ref(''),preview=ref<RecordData|null>(null),selectedJob=ref<RecordData|null>(null),jobId=ref(''),confirmed=ref(false)
const gateways=ref<RecordData[]>([]),bundles=ref<RecordData[]>([])
const form=reactive({gatewayIds:[] as string[],from:new Date(Date.now()-3600000),to:new Date(),eventIds:'',errorCodes:'',bundleId:'',maxSampleEvents:10,reason:'',maxEventsPerSecond:100,correctionMode:'UPSERT_RECORD'})
const key=mutationKey()
const canSubmit=computed(()=>!!preview.value && confirmed.value && preview.value.matchedCount>0 && preview.value.missingRawCount===0 && new Date(preview.value.expiresAt).getTime()>Date.now() && !!form.reason.trim())
async function prepareChoices(){try{if(can('gateway:read'))gateways.value=(await api('/api/v1/gateways?limit=200')).items;if(can('rule:read','scope_instance'))bundles.value=(await api('/api/v1/rule-bundles?limit=200')).items.filter((b:RecordData)=>['VALIDATED','RELEASED'].includes(b.state))}catch(e){error.value=errorText(e)}}
function showWizard(){preview.value=null;confirmed.value=false;error.value='';key.clear();wizard.value=true;void prepareChoices()}
async function createPreview(){
  busy.value=true;error.value='';preview.value=null;confirmed.value=false;key.clear()
  try{
    if(!form.gatewayIds.length)throw new Error('至少选择或输入一个网关 ID')
    if(!form.bundleId.trim())throw new Error('请选择或输入已经验证的规则包 ID')
    if(!form.from || !form.to || form.to.getTime()<=form.from.getTime() || form.to.getTime()-form.from.getTime()>31*86400000)throw new Error('接管时间范围必须大于零且不超过 31 天')
    const selector:RecordData={gatewayIds:form.gatewayIds,from:form.from.toISOString(),to:form.to.toISOString()}
    if(form.eventIds.trim())selector.eventIds=listText(form.eventIds)
    if(form.errorCodes.trim())selector.errorCodes=listText(form.errorCodes)
    preview.value=await api('/api/v1/reprocess-previews','POST',{selector,bundleId:form.bundleId.trim(),maxSampleEvents:form.maxSampleEvents})
  }catch(e){error.value=errorText(e)}finally{busy.value=false}
}
async function submit(){
  if(!canSubmit.value)return;busy.value=true;error.value=''
  try{
    if(new Date(preview.value!.expiresAt).getTime()<=Date.now())throw new Error('预览已过期，请重新预览')
    await ElMessageBox.confirm(`将为 ${preview.value!.matchedCount} 个事件创建新的处理修订，并可能向客户再次投递。原有事件和投递记录不会被覆盖。`,'确认重解析',{confirmButtonText:'创建作业',cancelButtonText:'返回检查',type:'warning'})
    const body={previewId:preview.value!.previewId,selectionDigest:preview.value!.selectionDigest,reason:form.reason.trim(),maxEventsPerSecond:form.maxEventsPerSecond,correctionMode:form.correctionMode}
    selectedJob.value=await api('/api/v1/reprocess-jobs','POST',body,{key:key.for(body)});jobId.value=selectedJob.value!.jobId;key.clear();wizard.value=false;activeTab.value='jobs';await jobs.refresh();ElMessage.success('已创建重解析作业')
  }catch(e){if(e!=='cancel'&&e!=='close')error.value=errorText(e)}finally{busy.value=false}
}
async function readJob(id=jobId.value){if(!id.trim())return;error.value='';busy.value=true;try{selectedJob.value=await api(`/api/v1/reprocess-jobs/${encodeURIComponent(id.trim())}`);jobId.value=id.trim()}catch(e){error.value=errorText(e)}finally{busy.value=false}}
function openEvent(row:RecordData){router.push(`/gateways/${encodeURIComponent(row.gatewayId)}/events/${encodeURIComponent(row.eventId)}`)}
watch(()=>[form.gatewayIds,form.from,form.to,form.eventIds,form.errorCodes,form.bundleId,form.maxSampleEvents],()=>{preview.value=null;confirmed.value=false;key.clear()},{deep:true})
watch(activeTab,v=>{if(v==='jobs'&&can('data:reprocess'))void jobs.refresh()})
onMounted(()=>{void quarantine.refresh();if(route.query.gatewayId&&can('data:reprocess')){form.gatewayIds=[String(route.query.gatewayId)];form.eventIds=String(route.query.eventId||'');const at=new Date(String(route.query.acceptedAt||''));if(!isNaN(at.getTime())){form.from=new Date(at.getTime()-1000);form.to=new Date(at.getTime()+1000)};showWizard()}})
</script>
<template>
  <div class="page-heading"><div><h1>隔离与重解析</h1><p class="muted">先预览固定范围和影响，再创建新的处理修订。</p></div><el-button v-if="can('data:reprocess')" type="primary" @click="showWizard">重解析预览</el-button></div>
  <el-alert v-if="error && !wizard" :title="error" type="error" :closable="false" show-icon/>
  <div class="panel"><el-tabs v-model="activeTab"><el-tab-pane label="隔离事件" name="quarantine">
    <el-form inline @submit.prevent="quarantine.refresh()"><el-form-item label="错误码"><el-input v-model="errorCode" clearable placeholder="全部错误码"/></el-form-item><el-form-item><el-button type="primary" native-type="submit">查询</el-button></el-form-item></el-form><el-alert v-if="quarantine.error.value" :title="quarantine.error.value" type="error" :closable="false"/>
    <el-table :data="quarantine.items.value" v-loading="quarantine.loading.value" empty-text="没有匹配的隔离事件"><el-table-column prop="eventId" label="事件" min-width="210"><template #default="{row}"><el-button link type="primary" @click="openEvent(row)">{{ row.eventId }}</el-button></template></el-table-column><el-table-column prop="gatewayId" label="网关" min-width="140"/><el-table-column prop="errorCode" label="错误码" min-width="200"/><el-table-column prop="processingRevision" label="修订" width="80"/><el-table-column label="原文" width="100"><template #default="{row}">{{ row.rawAvailable?'可用':'已回收' }}</template></el-table-column><el-table-column label="隔离时间" min-width="170"><template #default="{row}">{{ dateTime(row.createdAt) }}</template></el-table-column></el-table>
    <CursorFooter :count="quarantine.items.value.length" :page="quarantine.index.value" :next="quarantine.nextCursor.value" :snapshot="quarantine.snapshotAt.value" :loading="quarantine.loading.value" @next="quarantine.next()" @previous="quarantine.previous()" @refresh="quarantine.refresh()"/>
  </el-tab-pane><el-tab-pane v-if="can('data:reprocess')" label="我的重解析作业" name="jobs">
    <el-form inline @submit.prevent="readJob()"><el-form-item label="作业 ID"><el-input v-model="jobId" clearable placeholder="按 ID 查询"/></el-form-item><el-form-item><el-button native-type="submit" :loading="busy">查询作业</el-button><el-button @click="jobs.refresh()">刷新列表</el-button></el-form-item></el-form><el-alert v-if="jobs.error.value" :title="jobs.error.value" type="error" :closable="false"/>
    <el-table :data="jobs.items.value" v-loading="jobs.loading.value" empty-text="没有本人创建的重解析作业"><el-table-column prop="jobId" label="作业 ID" min-width="220"><template #default="{row}"><el-button link type="primary" @click="readJob(row.jobId)">{{ row.jobId }}</el-button></template></el-table-column><el-table-column label="状态" width="120"><template #default="{row}">{{ statusLabel(row.state) }}</template></el-table-column><el-table-column prop="selectedCount" label="选择数" width="90"/><el-table-column prop="processedCount" label="已处理" width="90"/><el-table-column prop="failedCount" label="失败" width="80"/><el-table-column label="创建时间" min-width="180"><template #default="{row}">{{ dateTime(row.createdAt) }}</template></el-table-column></el-table><CursorFooter :count="jobs.items.value.length" :page="jobs.index.value" :next="jobs.nextCursor.value" :snapshot="jobs.snapshotAt.value" :loading="jobs.loading.value" @next="jobs.next()" @previous="jobs.previous()" @refresh="jobs.refresh()"/>
    <el-descriptions v-if="selectedJob" class="job-summary" title="选中作业" :column="2" border><el-descriptions-item label="作业">{{ selectedJob.jobId }}</el-descriptions-item><el-descriptions-item label="状态">{{ statusLabel(selectedJob.state) }}</el-descriptions-item><el-descriptions-item label="规则包">{{ selectedJob.bundleId }}</el-descriptions-item><el-descriptions-item label="处理结果">{{ selectedJob.processedCount }} 已处理 / {{ selectedJob.failedCount }} 失败 / {{ selectedJob.selectedCount }} 选择</el-descriptions-item></el-descriptions>
  </el-tab-pane></el-tabs></div>
  <el-drawer v-model="wizard" title="重解析预览与确认" size="min(920px, 97vw)" :close-on-click-modal="false"><el-alert v-if="error" :title="error" type="error" :closable="false" show-icon/>
    <el-form label-position="top" :disabled="busy"><el-form-item label="网关 ID，最多 100 个" required><el-select v-model="form.gatewayIds" multiple filterable allow-create default-first-option :multiple-limit="100" style="width:100%" placeholder="选择或输入完整网关 ID"><el-option v-for="g in gateways" :key="g.gatewayId" :value="g.gatewayId" :label="`${g.displayName || g.gatewayId} · ${g.gatewayId}`"/></el-select></el-form-item><div class="form-grid"><el-form-item label="接管开始时间（含）" required><el-date-picker v-model="form.from" type="datetime"/></el-form-item><el-form-item label="接管结束时间（不含）" required><el-date-picker v-model="form.to" type="datetime"/></el-form-item><el-form-item label="事件 ID，可选，逗号分隔"><el-input v-model="form.eventIds" maxlength="26000"/></el-form-item><el-form-item label="错误码，可选，逗号分隔"><el-input v-model="form.errorCodes"/></el-form-item></div><el-form-item label="已经验证的规则包" required><el-select v-model="form.bundleId" filterable allow-create default-first-option style="width:100%" placeholder="选择或输入规则包 ID"><el-option v-for="b in bundles" :key="b.bundleId" :value="b.bundleId" :label="`${b.bundleId} · ${statusLabel(b.state)}`"/></el-select></el-form-item><el-form-item label="预览样本数"><el-input-number v-model="form.maxSampleEvents" :min="1" :max="100"/></el-form-item><el-button @click="createPreview" :loading="busy">生成预览</el-button></el-form>
    <section v-if="preview" class="preview-section"><h2>预览结果</h2><el-descriptions :column="2" border><el-descriptions-item label="匹配事件">{{ preview.matchedCount }}</el-descriptions-item><el-descriptions-item label="缺失原文">{{ preview.missingRawCount }}</el-descriptions-item><el-descriptions-item label="有效期至">{{ dateTime(preview.expiresAt) }}</el-descriptions-item><el-descriptions-item label="固定选择">{{ preview.selectionDigest.slice(0,16) }}…</el-descriptions-item></el-descriptions><el-alert v-if="preview.missingRawCount" title="选择中包含已回收原文，不能提交；请缩小范围重新预览。" type="warning" :closable="false"/><el-table :data="preview.sampleDifferences"><el-table-column prop="eventKey" label="样本事件" min-width="200"/><el-table-column prop="action" label="动作" width="130"/><el-table-column prop="newRecordCount" label="新记录数" width="100"/><el-table-column prop="error" label="预览错误" min-width="180"/></el-table><el-form label-position="top"><el-form-item label="操作原因" required><el-input v-model="form.reason" type="textarea" maxlength="300" show-word-limit/></el-form-item><div class="form-grid"><el-form-item label="限速（事件/秒）"><el-input-number v-model="form.maxEventsPerSecond" :min="1" :max="10000"/></el-form-item><el-form-item label="客户修订模式"><el-select v-model="form.correctionMode"><el-option value="UPSERT_RECORD" label="按记录更新（UPSERT_RECORD）"/><el-option value="REPLACE_EVENT_SET" label="替换事件记录集（REPLACE_EVENT_SET）"/></el-select></el-form-item></div><el-checkbox v-model="confirmed">已确认新增处理修订可能再次向客户投递，客户支持所选修订模式。</el-checkbox></el-form><el-collapse><el-collapse-item title="完整预览摘要"><pre>{{ pretty(preview) }}</pre></el-collapse-item></el-collapse></section>
    <template #footer><el-button @click="wizard=false" :disabled="busy">关闭</el-button><el-button type="primary" :disabled="!canSubmit" :loading="busy" @click="submit">确认创建重解析作业</el-button></template>
  </el-drawer>
</template>
<style scoped>h1{font-size:26px;margin:0}h2{font-size:18px}.panel{margin-bottom:18px}.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 18px}.form-grid :deep(.el-date-editor),.form-grid :deep(.el-select){width:100%}.job-summary,.preview-section{margin-top:22px}:deep(.el-checkbox){height:auto;white-space:normal}:deep(.el-checkbox__label){white-space:normal}:deep(.el-descriptions__table){table-layout:fixed}:deep(.el-descriptions__content){overflow-wrap:anywhere}.preview-section :deep(.el-table){margin:18px 0}pre{white-space:pre-wrap;overflow-wrap:anywhere}@media(max-width:650px){.form-grid{grid-template-columns:minmax(0,1fr)}}</style>
