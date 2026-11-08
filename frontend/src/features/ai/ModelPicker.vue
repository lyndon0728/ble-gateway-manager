<script setup lang="ts">
import {ref,computed} from 'vue'
import {api,errorText} from '@/lib/api'
import {can} from '@/lib/auth'
import {aiList,verificationLabel,type AIRecord} from './helpers'
const props=withDefaults(defineProps<{label?:string;kind?:'parser'|'connection'|'mapping'|'any'}>(),{label:'选择型号模板',kind:'any'})
const emit=defineEmits<{select:[model:AIRecord]}>()
const open=ref(false),busy=ref(false),error=ref(''),models=ref<AIRecord[]>([]),selected=ref(''),query=ref('')
const visible=computed(()=>models.value.filter(m=>(props.kind==='any'||props.kind==='parser'&&m.parserRef||props.kind==='connection'&&m.content?.connectionSpec||props.kind==='mapping'&&m.content?.mappingSpec)&&(!query.value||[m.modelName,m.manufacturer,m.version].join(' ').toLowerCase().includes(query.value.toLowerCase()))))
const model=computed(()=>models.value.find(m=>m.modelId===selected.value))
async function load(){open.value=true;error.value='';busy.value=true;try{models.value=await aiList('/api/v1/device-models')}catch(e){error.value=errorText(e)}finally{busy.value=false}}
async function choose(){if(!model.value)return;busy.value=true;error.value='';try{const selectedModel=await api('/api/v1/device-models/'+encodeURIComponent(model.value.modelId));emit('select',selectedModel);open.value=false}catch(e){error.value=errorText(e)}finally{busy.value=false}}
</script>
<template><el-button v-if="can('profile:read')" @click="load">{{label}}</el-button><el-dialog v-model="open" title="使用已发布型号模板" width="760px"><el-alert v-if="error" type="error" :title="error" :closable="false"/><el-input v-model="query" placeholder="搜索厂家、型号或版本" clearable class="model-query"/><el-table :data="visible" v-loading="busy" highlight-current-row @row-click="(row:AIRecord)=>selected=row.modelId" empty-text="暂无符合用途的已发布模板"><el-table-column width="45"><template #default="{row}"><el-radio v-model="selected" :value="row.modelId" :aria-label="'选择 '+row.modelName"><span/></el-radio></template></el-table-column><el-table-column prop="modelName" label="型号" min-width="150"/><el-table-column prop="manufacturer" label="厂家"/><el-table-column prop="version" label="版本" width="100"/><el-table-column label="验证等级" min-width="130"><template #default="{row}">{{verificationLabel(row.verificationLevel)}}</template></el-table-column></el-table><p class="muted">选用模板只带入参数，仍需核对实际对象、网关能力和执行范围。</p><template #footer><el-button @click="open=false">取消</el-button><el-button :disabled="!model" :loading="busy" type="primary" @click="choose">带入模板参数</el-button></template></el-dialog></template>
<style scoped>.model-query{margin-bottom:18px}</style>
