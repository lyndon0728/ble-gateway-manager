<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api } from '@/lib/api'
import { can } from '@/lib/auth'
import AiImportLauncher from '@/features/ai/AiImportLauncher.vue'
import ModelPicker from '@/features/ai/ModelPicker.vue'
import {type AIRecord,clone} from '@/features/ai/helpers'
import { bytes } from '@/lib/format'
import CursorFooter from '@/features/pipeline/CursorFooter.vue'
import TargetVersionForm from '@/features/pipeline/TargetVersionForm.vue'
import { errorText, mutationKey, useCursor, type RecordData } from '@/features/pipeline/helpers'

const route=useRoute(),router=useRouter(),isDetail=computed(()=>route.meta.pageId==='P14')
const list=useCursor(()=>'/api/v1/targets'),target=ref<RecordData|null>(null),version=ref(''),error=ref(''),loading=ref(false)
const selectedVersion=computed(()=>target.value?.versions.find((v:RecordData)=>v.version===version.value))
const drawer=ref(false),mode=ref<'target'|'version'>('target'),targetName=ref(''),editorInitial=ref<RecordData>(),editorError=ref(''),busy=ref(false),editorSerial=ref(0)
const editor=ref<InstanceType<typeof TargetVersionForm>|null>(null),saveKey=mutationKey(),mayWrite=computed(()=>can('target:write','scope_instance'))
let generation=0,appliedModelId=''
function versionSummary(row:RecordData){return row.versions?.[row.versions.length-1]}
async function loadDetail(preserve=false){
  const current=++generation;loading.value=true;error.value='';if(!preserve)target.value=null
  try{const result=await api(`/api/v1/targets/${encodeURIComponent(String(route.params.targetId))}`);if(current!==generation)return;target.value=result;if(!result.versions.some((v:RecordData)=>v.version===version.value))version.value=result.versions.at(-1)?.version || '';if(route.query.modelId&&mayWrite.value&&appliedModelId!==route.query.modelId){const m=await api('/api/v1/device-models/'+encodeURIComponent(String(route.query.modelId)));if(current===generation){applyModel(m);appliedModelId=String(route.query.modelId)}}}catch(e){if(current===generation)error.value=errorText(e)}finally{if(current===generation)loading.value=false}
}
function openCreate(){mode.value='target';targetName.value='';editorInitial.value=undefined;editorError.value='';saveKey.clear();editorSerial.value++;drawer.value=true}
function copyVersion(){if(!selectedVersion.value)return;mode.value='version';editorInitial.value=JSON.parse(JSON.stringify(selectedVersion.value.configuration));editorError.value='';saveKey.clear();editorSerial.value++;drawer.value=true}
function applyModel(model:AIRecord){if(!selectedVersion.value||!model.content?.mappingSpec)return;copyVersion();editorInitial.value={...clone(selectedVersion.value.configuration),mapping:clone(model.content.mappingSpec),mappingVersion:('ai-'+model.modelId+'-'+model.version).slice(0,64)};ElMessage.success('字段映射已带入新版本草稿；请核对目标地址和应用确认契约')}
async function save(){
  busy.value=true;editorError.value=''
  try{if(!editor.value)throw new Error('编辑表单尚未准备好');const configuration=editor.value.build();let body:RecordData,path:string
    if(mode.value==='target'){if(!targetName.value.trim())throw new Error('请输入目标名称');body={name:targetName.value.trim(),configuration};path='/api/v1/targets'}else{if(!target.value)throw new Error('目标详情已失效，请重新打开');body=configuration;path=`/api/v1/targets/${encodeURIComponent(target.value.targetId)}/versions`;await ElMessageBox.confirm('发布后会生成新的不可变版本。已有规则包继续使用其冻结版本；请创建、验证并激活新规则包以切换转发配置。','发布目标新版本',{confirmButtonText:'发布新版本',cancelButtonText:'返回编辑',type:'warning'})}
    const result=await api(path,'POST',body,{key:saveKey.for(body)});saveKey.clear();drawer.value=false;ElMessage.success(mode.value==='target'?'转发目标已创建':'新版本已发布')
    if(mode.value==='target')await router.push(`/targets/${encodeURIComponent(result.targetId)}`);else{version.value=result.versions[0].version;await loadDetail(true)}
  }catch(e){if(e!=='cancel'&&e!=='close')editorError.value=errorText(e)}finally{busy.value=false}
}
watch(()=>[route.meta.pageId,route.params.targetId,route.query.modelId],()=>{drawer.value=false;error.value='';if(isDetail.value){version.value='';void loadDetail()}else{generation++;target.value=null;void list.refresh()}},{immediate:true})
</script>
<template>
  <template v-if="!isDetail">
    <div class="page-heading"><div><h1>转发目标</h1><p class="muted">通过 HTTP 或 MQTT 将冻结后的数据投递至客户系统。</p></div><div class="toolbar"><el-button @click="list.refresh()" :loading="list.loading.value">刷新</el-button><el-button v-if="mayWrite" type="primary" @click="openCreate">新增目标</el-button></div></div>
    <el-alert v-if="list.error.value" :title="list.error.value" type="error" :closable="false" show-icon/>
    <section class="panel"><el-table :data="list.items.value" v-loading="list.loading.value" empty-text="还没有配置转发目标"><el-table-column label="目标" min-width="220"><template #default="{row}"><el-button link type="primary" @click="router.push(`/targets/${encodeURIComponent(row.targetId)}`)">{{ row.name || row.targetId }}</el-button><div class="muted mono">{{ row.targetId }}</div></template></el-table-column><el-table-column label="已发布版本" width="120"><template #default="{row}">{{ row.versions.length }} 个</template></el-table-column><el-table-column label="最新发布协议" width="135"><template #default="{row}"><el-tag>{{ versionSummary(row)?.configuration.protocol || '—' }}</el-tag></template></el-table-column><el-table-column label="最新发布地址" min-width="260"><template #default="{row}"><span class="mono">{{ versionSummary(row)?.configuration.endpoint || '—' }}</span></template></el-table-column><el-table-column label="最新映射版本" min-width="150"><template #default="{row}">{{ versionSummary(row)?.configuration.mappingVersion || '—' }}</template></el-table-column><el-table-column label="操作" width="110"><template #default="{row}"><el-button v-if="can('delivery:read')" link type="primary" @click="router.push({path:'/deliveries',query:{targetId:row.targetId}})">查看投递</el-button></template></el-table-column></el-table><CursorFooter :count="list.items.value.length" :page="list.index.value" :next="list.nextCursor.value" :snapshot="list.snapshotAt.value" :loading="list.loading.value" @next="list.next()" @previous="list.previous()" @refresh="list.refresh()"/><p class="muted">列表展示最新发布版本；实际投递使用规则包中冻结的目标版本。</p></section>
  </template>
  <div v-else v-loading="loading">
    <div class="page-heading"><div><el-button link @click="router.push('/targets')">← 返回转发目标</el-button><h1>{{ target?.name || '目标详情' }}</h1><p class="muted mono">{{ target?.targetId }}</p></div><div class="toolbar"><AiImportLauncher label="导入客户接口说明" task-kind="CUSTOMER_INTERFACE" :source-context="{targetId:String(route.params.targetId)}"/><ModelPicker v-if="mayWrite" label="使用映射模板" kind="mapping" @select="applyModel"/><el-button @click="loadDetail(true)">刷新</el-button><el-button v-if="target && can('delivery:read')" @click="router.push({path:'/deliveries',query:{targetId:target.targetId}})">投递记录</el-button><el-button v-if="target && mayWrite" type="primary" @click="copyVersion">复制为新版本</el-button></div></div>
    <el-alert v-if="error" :title="error" type="error" :closable="false" show-icon/>
    <template v-if="target && selectedVersion"><section class="panel"><div class="toolbar"><strong>已发布版本</strong><el-select v-model="version" class="version-select" aria-label="选择目标版本"><el-option v-for="v in target.versions" :key="v.version" :value="v.version" :label="`版本 ${v.version} · ${v.configuration.protocol}`"/></el-select><el-tag type="info">不可变</el-tag></div><p class="muted">队列上限 {{ bytes(selectedVersion.configuration.maxQueueBytes) }}；并发未确认事件 {{ selectedVersion.configuration.maxInflightEvents }}。</p><p class="mono muted">内容摘要 {{ selectedVersion.contentDigest }}</p></section><section class="panel"><TargetVersionForm :key="version" :initial="selectedVersion.configuration" read-only/></section></template>
  </div>
  <el-drawer v-model="drawer" :title="mode==='target'?'新增转发目标':'发布目标新版本'" size="min(920px, 97vw)" :close-on-click-modal="false" :close-on-press-escape="!busy" :show-close="!busy" destroy-on-close>
    <el-alert v-if="editorError" :title="editorError" type="error" :closable="false" show-icon/>
    <el-form v-if="mode==='target'" label-position="top"><el-form-item label="目标名称" required><el-input v-model="targetName" maxlength="100" :disabled="busy" placeholder="例如 客户生产系统"/></el-form-item></el-form>
    <TargetVersionForm :key="editorSerial" ref="editor" :initial="editorInitial" :busy="busy"/>
    <template #footer><el-button @click="drawer=false" :disabled="busy">取消</el-button><el-button type="primary" @click="save" :loading="busy">{{ mode==='target'?'创建目标':'发布新版本' }}</el-button></template>
  </el-drawer>
</template>
<style scoped>h1{font-size:26px;margin:6px 0}.panel{margin-bottom:18px}.mono{overflow-wrap:anywhere}.version-select{width:240px;max-width:100%}</style>
