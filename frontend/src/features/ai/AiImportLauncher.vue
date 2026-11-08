<script setup lang="ts">
import {computed,ref,watch} from 'vue'
import {useRoute,useRouter} from 'vue-router'
import {api,errorText} from '@/lib/api'
import {can,session} from '@/lib/auth'
import {type AIRecord,taskLabels} from './helpers'
const props=withDefaults(defineProps<{label?:string;taskKind?:string;sourceContext?:AIRecord;primary?:boolean}>(),{label:'导入设备资料',taskKind:'DEVICE_PROFILE',primary:false})
const route=useRoute(),router=useRouter(),open=ref(false),busy=ref(false),error=ref(''),status=ref<AIRecord>(),files=ref<File[]>([]),scopeId=ref(''),providerId=ref(''),useModel=ref(false)
const scopes=computed(()=>session.scopes.filter(s=>s.state==='ACTIVE'&&can('document:write',s.scopeId)))
const providers=computed(()=>(status.value?.providers||[]).filter((p:AIRecord)=>p.enabled))
const selectedProvider=computed(()=>providers.value.find((p:AIRecord)=>p.providerId===providerId.value))
const uploaded=ref<AIRecord[]>([])
watch(scopeId,()=>{uploaded.value=[]})
async function begin(){open.value=true;error.value='';scopeId.value=scopes.value.some(s=>s.scopeId===scopeId.value)?scopeId.value:scopes.value[0]?.scopeId||'';try{status.value=await api('/api/v1/ai/status');if(!providers.value.some((p:AIRecord)=>p.providerId===providerId.value))providerId.value=providers.value[0]?.providerId||'';useModel.value=providers.value.length>0}catch(e){error.value=errorText(e)}}
function pick(event:Event){const input=event.target as HTMLInputElement;files.value=Array.from(input.files||[]);uploaded.value=[];error.value=''}
async function submit(){busy.value=true;error.value='';try{if(!scopeId.value)throw new Error('请选择资料所属范围');if(useModel.value&&!files.value.length)throw new Error('自动提取需要至少一份设备说明书或接口资料');if(files.value.length>5||files.value.reduce((sum,f)=>sum+f.size,0)>52428800)throw new Error('每次最多 5 个文件，合计不超过 50 MiB');if(useModel.value&&!providerId.value)throw new Error('请选择可用模型服务')
  for(let i=uploaded.value.length;i<files.value.length;i++){const form=new FormData();form.append('file',files.value[i]!);form.append('scopeId',scopeId.value);uploaded.value.push(await api('/api/v1/source-documents','POST',form))}
  const result=await api('/api/v1/ai/import-jobs','POST',{scopeId:scopeId.value,documentIds:uploaded.value.map(d=>d.documentId),taskKind:props.taskKind,useModel:useModel.value,...(useModel.value?{providerId:providerId.value}:{}),sourceContext:{pageId:String(route.meta.pageId||''),returnTo:route.fullPath,...props.sourceContext}})
  open.value=false;await router.push('/import-jobs/'+result.jobId)
}catch(e){error.value=errorText(e)}finally{busy.value=false}}
</script>
<template>
<el-button v-if="can('document:write')" :type="primary?'primary':undefined" @click="begin">{{label}}</el-button>
<el-dialog v-model="open" :title="'导入资料 · '+taskLabels[taskKind]" width="660px" :close-on-click-modal="!busy" :close-on-press-escape="!busy" :show-close="!busy">
<el-alert v-if="error" :title="error" type="error" :closable="false"/>
<el-alert v-if="status&&!providers.length" title="尚未配置可用模型。资料仍会保存在本机，可创建人工草稿；配置模型服务后可使用自动提取。" type="info" :closable="false"/>
<el-form label-position="top" :disabled="busy">
<el-form-item label="资料所属项目或分组" required><el-select v-model="scopeId" style="width:100%"><el-option v-for="s in scopes" :key="s.scopeId" :label="s.name" :value="s.scopeId"/></el-select></el-form-item>
<el-form-item label="设备说明书、协议或接口文件" :required="useModel"><input class="ai-file-input" aria-label="选择说明书文件" type="file" multiple accept=".pdf,.docx,.txt,.md,.png,.jpg,.jpeg" @change="pick"/><p class="muted">PDF、DOCX、TXT、Markdown、PNG/JPEG；最多 5 份，合计 50 MiB。人工草稿可不选文件。</p></el-form-item>
<div v-for="(file,i) in files" :key="file.name+'-'+i" class="upload-row"><span>{{file.name}}</span><span class="muted">{{(file.size/1024).toFixed(1)}} KiB · {{uploaded[i]?'已保存':'待上传'}}</span></div>
<el-form-item label="提取方式"><el-radio-group v-model="useModel"><el-radio :value="true" :disabled="!providers.length">AI 辅助生成草稿</el-radio><el-radio :value="false">本地提取并人工编辑</el-radio></el-radio-group></el-form-item>
<el-form-item v-if="useModel" label="模型服务"><el-select v-model="providerId" style="width:100%"><el-option v-for="p in providers" :key="p.providerId" :label="p.name+' · '+p.model" :value="p.providerId"/></el-select></el-form-item>
<p v-if="useModel&&selectedProvider" class="notice">本次资料将发送至{{selectedProvider.mode==='CLOUD'?'管理员允许的云端':'客户内网'}}模型服务「{{selectedProvider.name}}」进行分析，可能产生 API 用量。凭据不作为资料发送。</p>
<p class="muted">导入仅生成草稿，不会连接设备、写入特征或激活规则。文档中的示例地址不会自动登记到现场。</p>
</el-form>
<template #footer><el-button :disabled="busy" @click="open=false">取消</el-button><el-button type="primary" :loading="busy" @click="submit">{{useModel?'上传并生成草稿':'上传并创建人工草稿'}}</el-button></template>
</el-dialog>
</template>
<style scoped>.ai-file-input{display:block;max-width:100%;padding:14px;border:1px dashed #9bb7c2;border-radius:7px;background:#f6f9fa;width:100%}.upload-row{display:flex;justify-content:space-between;gap:12px;font-size:12px;margin-bottom:10px;overflow-wrap:anywhere}</style>
