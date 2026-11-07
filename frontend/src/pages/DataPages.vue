<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage } from 'element-plus'
import { api } from '@/lib/api'
import { can, canGateway } from '@/lib/auth'
import { dateTime, pretty, statusLabel } from '@/lib/format'
import CursorFooter from '@/features/pipeline/CursorFooter.vue'
import RawViewer from '@/features/pipeline/RawViewer.vue'
import StructuredRecord from '@/features/pipeline/StructuredRecord.vue'
import { useCursor, errorText, copyText, type RecordData } from '@/features/pipeline/helpers'

const route=useRoute(),router=useRouter(),isDetail=computed(()=>route.meta.pageId==='P09')
const gatewayFilter=ref(''),range=ref<[Date,Date]|null>(null)
const list=useCursor(()=>'/api/v1/events',()=>({gatewayId:gatewayFilter.value,from:range.value?.[0]?.toISOString(),to:range.value?.[1]?.toISOString()}))
const detail=ref<RecordData|null>(null),gateway=ref<RecordData|null>(null),loading=ref(false),error=ref(''),tab=ref('processing')
const rawLoading=ref(false),output=ref<RecordData|null>(null),outputOpen=ref(false),outputError=ref(''),outputLoading=ref(false)
let generation=0,outputGeneration=0
const canRaw=computed(()=>gateway.value ? canGateway('data:raw',gateway.value) : false)
const canReprocess=computed(()=>gateway.value ? canGateway('data:reprocess',gateway.value) : false)
async function loadDetail(){
  const current=++generation;loading.value=true;error.value='';detail.value=null;gateway.value=null;output.value=null;outputOpen.value=false;rawLoading.value=false
  try{
    const id=String(route.params.gatewayId),event=String(route.params.eventId)
    const result=await api(`/api/v1/gateways/${encodeURIComponent(id)}/events/${encodeURIComponent(event)}`)
    if(current!==generation)return;detail.value=result
    if(can('gateway:read')){const g=await api(`/api/v1/gateways/${encodeURIComponent(id)}`);if(current===generation)gateway.value=g}
  }catch(e){if(current===generation)error.value=errorText(e)}finally{if(current===generation)loading.value=false}
}
async function loadRaw(){
  if(!detail.value || !canRaw.value || !detail.value.rawAvailable)return
  const current=generation;rawLoading.value=true;error.value=''
  try{const result=await api(`/api/v1/gateways/${encodeURIComponent(detail.value.gatewayId)}/events/${encodeURIComponent(detail.value.eventId)}?includeRaw=true`);if(current===generation){detail.value=result;tab.value='raw'}}catch(e){if(current===generation)error.value=errorText(e)}finally{if(current===generation)rawLoading.value=false}
}
function openEvent(row:RecordData){router.push(`/gateways/${encodeURIComponent(row.gatewayId)}/events/${encodeURIComponent(row.eventId)}`)}
async function openOutput(id:string){
  const current=generation,request=++outputGeneration;outputOpen.value=true;output.value=null;outputError.value='';outputLoading.value=true
  try{const result=await api(`/api/v1/outputs/${encodeURIComponent(id)}`);if(current===generation&&request===outputGeneration)output.value=result}catch(e){if(current===generation&&request===outputGeneration)outputError.value=errorText(e)}finally{if(current===generation&&request===outputGeneration)outputLoading.value=false}
}
function reprocess(){if(detail.value)router.push({path:'/quarantine',query:{gatewayId:detail.value.gatewayId,eventId:detail.value.eventId,acceptedAt:detail.value.acceptedAt}})}
function testRaw(){if(detail.value?.raw)router.push({path:'/parsers',query:{gatewayId:detail.value.gatewayId,eventId:detail.value.eventId}})}
async function copyIdentity(){if(!detail.value)return;try{await copyText(`${detail.value.gatewayId}/${detail.value.eventId}`);ElMessage.success('事件身份已复制')}catch(e){ElMessage.error(errorText(e))}}
watch(outputOpen,open=>{if(!open){outputGeneration++;output.value=null;outputError.value='';outputLoading.value=false}},{flush:'sync'})
watch(()=>[route.meta.pageId,route.params.gatewayId,route.params.eventId,route.query.gatewayId],()=>{
  if(isDetail.value){tab.value='processing';void loadDetail()}else{generation++;gatewayFilter.value=String(route.query.gatewayId||'');void list.refresh()}
},{immediate:true})
</script>

<template>
  <div v-if="!isDetail">
    <div class="page-heading"><div><h1>数据事件</h1><p class="muted">查看服务器已持久接管的数据，以及后续解析和客户确认状态。</p></div><el-button @click="list.refresh()" :loading="list.loading.value">刷新</el-button></div>
    <div class="panel">
      <el-form inline class="toolbar" @submit.prevent="list.refresh()"><el-form-item label="网关 ID"><el-input v-model="gatewayFilter" clearable placeholder="全部可见网关"/></el-form-item><el-form-item label="接管时间"><el-date-picker v-model="range" type="datetimerange" range-separator="至" start-placeholder="开始时间" end-placeholder="结束时间"/></el-form-item><el-form-item><el-button type="primary" native-type="submit" :loading="list.loading.value">查询</el-button></el-form-item></el-form>
      <el-alert v-if="list.error.value" :title="list.error.value" type="error" :closable="false" show-icon/>
      <el-table :data="list.items.value" v-loading="list.loading.value" :row-key="(row:RecordData)=>`${row.gatewayId}/${row.eventId}`" @row-dblclick="openEvent" empty-text="当前范围没有已接管事件">
        <el-table-column prop="eventId" label="事件 ID" min-width="230"><template #default="{row}"><el-button link type="primary" @click="openEvent(row)">{{ row.eventId }}</el-button></template></el-table-column><el-table-column prop="gatewayId" label="网关" min-width="150"/>
        <el-table-column label="接管时间" min-width="180"><template #default="{row}">{{ dateTime(row.acceptedAt) }}</template></el-table-column><el-table-column label="处理状态" min-width="120"><template #default="{row}"><el-tag>{{ statusLabel(row.state) }}</el-tag></template></el-table-column><el-table-column prop="latestProcessingRevision" label="处理修订" width="100"/><el-table-column label="原文" width="100"><template #default="{row}">{{ row.rawAvailable?'可用':'已回收' }}</template></el-table-column>
      </el-table>
      <CursorFooter :count="list.items.value.length" :page="list.index.value" :next="list.nextCursor.value" :snapshot="list.snapshotAt.value" :loading="list.loading.value" @next="list.next()" @previous="list.previous()" @refresh="list.refresh()"/>
    </div>
  </div>
  <div v-else v-loading="loading">
    <div class="page-heading"><div><el-button link @click="router.push({path:'/events',query:{gatewayId:String(route.params.gatewayId)}})">← 返回数据事件</el-button><h1>事件详情</h1></div><div class="toolbar"><el-button @click="loadDetail">刷新</el-button><el-button v-if="detail" @click="copyIdentity">复制事件身份</el-button></div></div>
    <el-alert v-if="error" :title="error" type="error" show-icon :closable="false"/>
    <template v-if="detail">
      <div class="panel"><el-descriptions :column="2" border><el-descriptions-item label="事件">{{ detail.eventId }}</el-descriptions-item><el-descriptions-item label="网关">{{ detail.gatewayId }}</el-descriptions-item><el-descriptions-item label="状态"><el-tag>{{ statusLabel(detail.state) }}</el-tag></el-descriptions-item><el-descriptions-item label="接管时间">{{ dateTime(detail.acceptedAt) }}</el-descriptions-item><el-descriptions-item label="最新修订">{{ detail.latestProcessingRevision }}</el-descriptions-item><el-descriptions-item label="原文">{{ detail.rawAvailable?'仍在保留期':'已回收，处理身份仍保留' }}</el-descriptions-item></el-descriptions></div>
      <div class="panel"><el-tabs v-model="tab"><el-tab-pane label="处理修订" name="processing"><el-table :data="detail.processing" empty-text="暂无处理修订"><el-table-column prop="processingRevision" label="修订" width="80"/><el-table-column label="状态" width="130"><template #default="{row}">{{ statusLabel(row.state) }}</template></el-table-column><el-table-column prop="bundleId" label="规则包" min-width="180"/><el-table-column prop="errorCode" label="错误码" min-width="170"/><el-table-column label="输出记录" min-width="220"><template #default="{row}"><div v-for="id in row.outputIds" :key="id"><el-button v-if="canRaw" link type="primary" @click="openOutput(id)">{{ id }}</el-button><span v-else class="mono">{{ id }}</span></div></template></el-table-column></el-table></el-tab-pane>
        <el-tab-pane label="原始报文" name="raw"><el-alert v-if="!canRaw" title="当前账号没有此网关的原文读取权限。" type="info" :closable="false"/><el-empty v-else-if="!detail.rawAvailable" description="原文已按保留策略回收"/><template v-else><el-button v-if="!detail.raw" type="primary" @click="loadRaw" :loading="rawLoading">读取原文</el-button><RawViewer v-else :raw="detail.raw"/></template></el-tab-pane>
        <el-tab-pane label="采集上下文" name="capture"><StructuredRecord v-if="detail.raw?.capture" :value="detail.raw.capture"/><el-empty v-else :description="detail.raw ? '此版本事件未提供 capture' : '读取有权限的原文后查看采集上下文'"/><el-button v-if="canRaw && detail.rawAvailable && !detail.raw" @click="loadRaw" :loading="rawLoading">读取采集上下文</el-button></el-tab-pane></el-tabs>
        <div class="toolbar detail-actions"><el-button v-if="can('parser:test') && detail.raw" @click="testRaw">使用原文试算</el-button><el-button v-if="canReprocess && detail.rawAvailable" type="primary" @click="reprocess">重解析预览</el-button></div>
      </div>
    </template>
    <el-drawer v-model="outputOpen" title="已持久化的解析输出" size="min(760px, 94vw)"><el-alert v-if="outputError" :title="outputError" type="error" :closable="false"/><div v-loading="outputLoading"><template v-if="output"><el-descriptions :column="1" border><el-descriptions-item label="输出 ID">{{ output.outputId }}</el-descriptions-item><el-descriptions-item label="记录 / 修订">{{ output.recordKey }} / {{ output.processingRevision }}</el-descriptions-item><el-descriptions-item label="解析器">{{ output.parserId || '—' }} @ {{ output.parserVersion || '—' }}</el-descriptions-item></el-descriptions><StructuredRecord v-if="output.payloadAvailable" :value="output.record"/><el-empty v-else description="输出内容已回收，输出身份仍保留"/><el-collapse><el-collapse-item title="完整输出元数据"><pre>{{ pretty(output) }}</pre></el-collapse-item></el-collapse></template></div></el-drawer>
  </div>
</template>
<style scoped>h1{margin:6px 0;font-size:26px}.detail-actions{margin-top:20px}.panel{margin-bottom:18px}pre{white-space:pre-wrap;overflow-wrap:anywhere}.mono{font-family:ui-monospace,monospace;overflow-wrap:anywhere}:deep(.el-date-editor){max-width:100%}:deep(.el-descriptions__content){overflow-wrap:anywhere}</style>
