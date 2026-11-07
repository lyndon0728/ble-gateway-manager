<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import { ElMessage } from 'element-plus'
import { api, newKey } from '@/lib/api'
import { can } from '@/lib/auth'
import { pretty } from '@/lib/format'
import StructuredRecord from './StructuredRecord.vue'
import { errorText, type RecordData } from './helpers'

const props=defineProps<{initial?:RecordData;readOnly?:boolean;busy?:boolean}>()
const form=reactive<RecordData>({}),fields=ref<RecordData[]>([]),constants=ref<RecordData[]>([]),custom=ref(false)
const sample=ref('{\n  "temperature": 25,\n  "humidity": 50\n}'),sampleResult=ref<RecordData|null>(null),sampleError=ref('')
const secretOpen=ref(false),secretLabel=ref(''),secretValue=ref(''),secretUsername=ref(''),secretKind=ref('HTTP_BEARER'),secretFor=ref('secretRef'),secretBusy=ref(false),secretError=ref('')
let secretKey=''
const disabled=computed(()=>props.readOnly || props.busy)
const keyPattern=/^[A-Za-z][A-Za-z0-9_]{0,63}$/
watch(()=>props.initial,initial=>{
  Object.assign(form,{protocol:'HTTP',endpoint:'',secretRef:'',tlsTrustRef:'',payloadTopic:'',ackTopic:'',maxBatchEvents:100,maxInflightEvents:1000,maxQueueBytes:1073741824,mappingVersion:'identity-1',wrapKey:''},initial?JSON.parse(JSON.stringify(initial)):{})
  custom.value=!!initial?.mapping
  form.wrapKey=initial?.mapping?.wrapKey || ''
  fields.value=(initial?.mapping?.fields || []).map((f:RecordData)=>({...f,numeric:('scale' in f)||('offset' in f),scale:f.scale??1,offset:f.offset??0}))
  constants.value=Object.entries(initial?.mapping?.constants || {}).map(([key,value])=>({key,type:value===null?'null':typeof value,value:value===null?'':value}))
  sampleResult.value=null;sampleError.value=''
},{immediate:true})
function switchMapping(value:unknown){
  if(value){if(['identity-1','1'].includes(form.mappingVersion))form.mappingVersion='mapping-1';if(!fields.value.length)addField()}
  else form.mappingVersion='identity-1'
  sampleResult.value=null
}
function addField(){fields.value.push({source:'',target:'',numeric:false,scale:1,offset:0,unit:''})}
function buildMapping():RecordData|undefined{
  if(!custom.value)return undefined
  if(!fields.value.length || fields.value.length>64)throw new Error('自定义映射必须包含 1–64 个字段')
  const claimed=new Set<string>()
  function claim(key:string){if(!keyPattern.test(key))throw new Error('映射名称需以字母开头，只含字母、数字或下划线，最长 64 字符');if(claimed.has(key))throw new Error(`映射输出字段重复：${key}`);claimed.add(key)}
  const mapping:RecordData={fields:fields.value.map(f=>{
    const source=f.source.trim(),target=f.target.trim();if(!keyPattern.test(source))throw new Error(`源字段无效：${source || '空白'}`);claim(target)
    const result:RecordData={source,target}
    if(f.numeric){for(const name of ['scale','offset']){if(typeof f[name]!=='number'||!Number.isFinite(f[name])||Math.abs(f[name])>1000000)throw new Error('倍率与偏移量必须是 ±1000000 之间的数字');result[name]=f[name]}}
    const unit=(f.unit || '').trim();if(unit){if(new TextEncoder().encode(unit).length>32)throw new Error('单位最多 32 个 UTF-8 字节');claim(`${target}Unit`);result.unit=unit}
    return result
  })}
  if(constants.value.length>32)throw new Error('固定字段最多 32 个')
  if(constants.value.length){mapping.constants={};for(const c of constants.value){const key=c.key.trim();claim(key);let value:any=c.value;if(c.type==='null')value=null;else if(c.type==='number'){if(value===''||!Number.isFinite(Number(value)))throw new Error(`固定字段 ${key} 需要有效数字`);value=Number(value)}else if(c.type==='boolean')value=c.value===true;else{value=String(value);if(new TextEncoder().encode(value).length>256)throw new Error('固定字符串最多 256 个 UTF-8 字节')};mapping.constants[key]=value}}
  if(form.wrapKey.trim()){if(!keyPattern.test(form.wrapKey.trim()))throw new Error('外层字段名无效');mapping.wrapKey=form.wrapKey.trim()}
  return mapping
}
function build():RecordData{
  let endpoint:URL;try{endpoint=new URL(form.endpoint.trim())}catch{throw new Error('请输入完整目标地址')}
  if(endpoint.username || endpoint.password || endpoint.hash)throw new Error('目标地址不能包含用户名、密码或片段，请使用密钥引用')
  const protocols=form.protocol==='HTTP'?['https:','http:']:['mqtts:','ssl:','tls:','mqtt:','tcp:']
  if(!protocols.includes(endpoint.protocol))throw new Error('目标地址协议与所选类型不符')
  if(form.protocol==='MQTT'&&((endpoint.pathname&&endpoint.pathname!=='/')||endpoint.search))throw new Error('MQTT 地址只能包含主机与端口')
  const out:RecordData={protocol:form.protocol,endpoint:form.endpoint.trim(),ackMode:form.protocol==='HTTP'?'HTTP_DURABLE_RESPONSE':'MQTT_APPLICATION_ACK',maxBatchEvents:form.maxBatchEvents,maxInflightEvents:form.maxInflightEvents,maxQueueBytes:form.maxQueueBytes,mappingVersion:form.mappingVersion.trim()}
  if(!out.mappingVersion || out.mappingVersion.length>64)throw new Error('映射版本不能为空且最长 64 字符')
  for(const [key,min,max] of [['maxBatchEvents',1,200],['maxInflightEvents',1,100000],['maxQueueBytes',1048576,Number.MAX_SAFE_INTEGER]] as const)if(!Number.isSafeInteger(out[key])||out[key]<min||out[key]>max)throw new Error(`${key} 不在允许范围内`)
  for(const key of ['secretRef','tlsTrustRef']){if(form[key]?.trim()){if(!/^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$/.test(form[key].trim()))throw new Error('密钥或 CA 引用格式无效');out[key]=form[key].trim()}}
  if(form.protocol==='MQTT'){for(const key of ['payloadTopic','ackTopic']){const topic=form[key].trim();if(!topic || topic.length>256 || /[+#\0]/.test(topic))throw new Error('MQTT 数据和确认主题必须是不同的固定主题，不能含通配符');out[key]=topic}if(out.payloadTopic===out.ackTopic)throw new Error('数据主题与确认主题不能相同')}
  const mapping=buildMapping();if(mapping){if(['1','identity-1'].includes(out.mappingVersion))throw new Error('自定义映射需要独立版本名称，不能使用 identity-1 或 1');out.mapping=mapping}else if(!['1','identity-1'].includes(out.mappingVersion))throw new Error('保留原字段时，映射版本必须为 identity-1 或 1')
  return out
}
function preview(){
  sampleError.value='';sampleResult.value=null
  try{const input=JSON.parse(sample.value);if(!input||Array.isArray(input)||typeof input!=='object')throw new Error('样本需为 data 对象');const mapping=buildMapping();if(!mapping){sampleResult.value=input;return}let result:RecordData={};for(const f of mapping.fields){if(!Object.hasOwn(input,f.source))throw new Error(`缺少源字段：${f.source}`);let v=input[f.source];if('scale' in f || 'offset' in f){if(typeof v!=='number')throw new Error(`需要数值源字段：${f.source}`);v=v*(f.scale??1)+(f.offset??0);if(!Number.isFinite(v))throw new Error('计算结果溢出')};result[f.target]=v;if(f.unit)result[f.target+'Unit']=f.unit}Object.assign(result,mapping.constants || {});if(mapping.wrapKey)result={[mapping.wrapKey]:result};sampleResult.value=result}catch(e){sampleError.value=errorText(e)}
}
function openSecret(forField:string){secretFor.value=forField;secretKind.value=forField==='tlsTrustRef'?'CLIENT_CERTIFICATE':form.protocol==='MQTT'?'MQTT_PASSWORD':'HTTP_BEARER';secretLabel.value='';secretValue.value='';secretUsername.value='';secretError.value='';secretKey=newKey();secretOpen.value=true}
watch([secretLabel,secretKind,secretValue,secretUsername],()=>{secretKey=newKey()})
async function saveSecret(){
  secretBusy.value=true;secretError.value=''
  try{if(!secretLabel.value.trim() || !secretValue.value)throw new Error('请填写名称与密钥内容');if(secretKind.value==='MQTT_PASSWORD'&&!secretUsername.value.trim())throw new Error('请填写 MQTT 用户名');const value=secretKind.value==='MQTT_PASSWORD'?JSON.stringify({username:secretUsername.value.trim(),password:secretValue.value}):secretValue.value;const result=await api('/api/v1/secrets','POST',{label:secretLabel.value.trim(),kind:secretKind.value,value},{key:secretKey});form[secretFor.value]=result.secretRef;secretValue.value='';secretOpen.value=false;ElMessage.success('密钥已写入服务器，表单仅保留引用')}catch(e){secretError.value=errorText(e)}finally{secretBusy.value=false}
}
defineExpose({build})
</script>
<template>
  <el-form label-position="top" :disabled="disabled">
    <h3>连接与接收确认</h3>
    <div class="form-grid"><el-form-item label="接入协议" required><el-radio-group v-model="form.protocol"><el-radio-button value="HTTP">HTTP</el-radio-button><el-radio-button value="MQTT">MQTT</el-radio-button></el-radio-group></el-form-item><el-form-item label="确认方式"><el-input :model-value="form.protocol==='HTTP'?'HTTP 持久接收确认':'MQTT 应用层确认'" disabled/></el-form-item></div>
    <el-form-item label="接收地址" required><el-input v-model="form.endpoint" :placeholder="form.protocol==='HTTP'?'https://customer.example.com/events':'mqtts://broker.example.com:8883'" maxlength="2048"/><span class="muted">生产环境使用 TLS；地址须符合服务器允许列表。创建版本不会发送探测数据。</span></el-form-item>
    <div class="form-grid" v-if="form.protocol==='MQTT'"><el-form-item label="数据主题" required><el-input v-model="form.payloadTopic" maxlength="256" placeholder="customer/gateway/events"/></el-form-item><el-form-item label="应用确认主题" required><el-input v-model="form.ackTopic" maxlength="256" placeholder="customer/gateway/acks"/></el-form-item></div>
    <div class="form-grid"><el-form-item :label="form.protocol==='HTTP'?'Bearer 密钥引用，可选':'MQTT 凭据引用，可选'"><div class="input-action"><el-input v-model="form.secretRef" maxlength="128" placeholder="secret_…"/><el-button v-if="!readOnly && can('secret:write','scope_instance')" @click="openSecret('secretRef')">新建</el-button></div></el-form-item><el-form-item label="信任 CA 引用，可选"><div class="input-action"><el-input v-model="form.tlsTrustRef" maxlength="128" placeholder="留空使用服务器系统信任库"/><el-button v-if="!readOnly && can('secret:write','scope_instance')" @click="openSecret('tlsTrustRef')">新建</el-button></div></el-form-item></div>
    <h3>容量与背压</h3><div class="form-grid thirds"><el-form-item label="每批最多事件"><el-input-number v-model="form.maxBatchEvents" :min="1" :max="200" :precision="0"/></el-form-item><el-form-item label="最多未确认事件"><el-input-number v-model="form.maxInflightEvents" :min="1" :max="100000" :precision="0"/></el-form-item><el-form-item label="队列上限（字节）"><el-input-number v-model="form.maxQueueBytes" :min="1048576" :step="1048576" :precision="0"/></el-form-item></div>
    <h3>字段映射</h3><div class="form-grid"><el-form-item label="映射模式"><el-switch v-model="custom" active-text="自定义 data 字段" inactive-text="保留原字段" @change="switchMapping"/></el-form-item><el-form-item label="冻结映射版本" required><el-input v-model="form.mappingVersion" maxlength="64" :disabled="!custom"/></el-form-item></div>
    <template v-if="custom"><p class="muted">只映射解析结果 data 的顶层字段。事件身份和修订仍由服务器维护。单位会生成「目标字段 + Unit」字段。</p>
      <section class="mapping-row" v-for="(field,index) in fields" :key="index"><div class="section-title"><strong>字段 {{ index+1 }}</strong><el-button v-if="!readOnly" link type="danger" @click="fields.splice(index,1)">移除</el-button></div><div class="form-grid thirds"><el-form-item label="源字段"><el-input v-model="field.source" maxlength="64" placeholder="temperature"/></el-form-item><el-form-item label="目标字段"><el-input v-model="field.target" maxlength="64" placeholder="temperature"/></el-form-item><el-form-item label="单位，可选"><el-input v-model="field.unit" maxlength="32" placeholder="℃"/></el-form-item></div><el-checkbox v-model="field.numeric">数值换算：结果 = 源值 × 倍率 + 偏移量</el-checkbox><div class="form-grid" v-if="field.numeric"><el-form-item label="倍率"><el-input-number v-model="field.scale" :min="-1000000" :max="1000000"/></el-form-item><el-form-item label="偏移量"><el-input-number v-model="field.offset" :min="-1000000" :max="1000000"/></el-form-item></div></section>
      <el-button v-if="!readOnly" @click="addField" :disabled="fields.length>=64">添加映射字段</el-button>
      <h4>固定字段</h4><div class="constant-row" v-for="(constant,index) in constants" :key="index"><el-input v-model="constant.key" aria-label="固定字段名称" placeholder="字段名称" maxlength="64"/><el-select v-model="constant.type" aria-label="固定字段类型" @change="constant.value=constant.type==='boolean'?false:''"><el-option label="文本" value="string"/><el-option label="数字" value="number"/><el-option label="布尔" value="boolean"/><el-option label="空值" value="null"/></el-select><el-switch v-if="constant.type==='boolean'" v-model="constant.value" active-text="true" inactive-text="false"/><el-input v-else-if="constant.type!=='null'" v-model="constant.value" aria-label="固定字段值" placeholder="字段值"/><span v-else>null</span><el-button v-if="!readOnly" link type="danger" @click="constants.splice(index,1)">移除</el-button></div><el-button v-if="!readOnly" @click="constants.push({key:'',type:'string',value:''})" :disabled="constants.length>=32">添加固定字段</el-button>
      <el-form-item label="外层字段名，可选" class="wrap-field"><el-input v-model="form.wrapKey" maxlength="64" placeholder="例如 readings，留空则不嵌套"/></el-form-item>
    </template>
  </el-form>
  <el-collapse><el-collapse-item title="本地映射样本预览（不发送客户平台）"><p class="muted">输入一个 data 对象，仅验证字段转换。最终发布仍由服务器验证。</p><el-input v-model="sample" type="textarea" :rows="5" aria-label="本地映射输入样本"/><el-button class="preview-button" @click="preview">预览映射</el-button><el-alert v-if="sampleError" :title="sampleError" type="error" :closable="false"/><StructuredRecord v-if="sampleResult" :value="sampleResult"/></el-collapse-item><el-collapse-item v-if="readOnly && initial" title="完整版本配置"><pre>{{ pretty(initial) }}</pre></el-collapse-item></el-collapse>
  <el-dialog v-model="secretOpen" :title="secretFor==='tlsTrustRef'?'保存信任 CA':'保存连接密钥'" width="min(640px, 94vw)" append-to-body :close-on-click-modal="false" :close-on-press-escape="!secretBusy" :show-close="!secretBusy" @closed="secretValue=''">
    <el-alert title="密钥写入服务器后不会回显。本页面仅保留 secretRef 引用。" type="info" :closable="false"/>
    <el-alert v-if="secretError" :title="secretError" type="error" :closable="false"/>
    <el-form label-position="top" :disabled="secretBusy"><el-form-item label="名称" required><el-input v-model="secretLabel" maxlength="100"/></el-form-item><el-form-item v-if="secretKind==='MQTT_PASSWORD'" label="MQTT 用户名" required><el-input v-model="secretUsername" autocomplete="off"/></el-form-item><el-form-item :label="secretFor==='tlsTrustRef'?'PEM 格式 CA 证书':secretKind==='MQTT_PASSWORD'?'MQTT 密码':'Bearer 密钥'" required><el-input v-model="secretValue" :type="secretFor==='tlsTrustRef'?'textarea':'password'" :rows="7" :show-password="secretFor!=='tlsTrustRef'" autocomplete="new-password" maxlength="131072"/></el-form-item></el-form>
    <template #footer><el-button @click="secretOpen=false" :disabled="secretBusy">取消</el-button><el-button type="primary" @click="saveSecret" :loading="secretBusy">保存并引用</el-button></template>
  </el-dialog>
</template>
<style scoped>h3{margin:22px 0 16px;font-size:17px}.form-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:0 18px}.thirds{grid-template-columns:repeat(3,minmax(0,1fr))}.input-action{display:flex;width:100%;gap:8px}.input-action .el-input{flex:1;min-width:0}.mapping-row{border:1px solid var(--el-border-color);padding:16px;border-radius:8px;margin:12px 0}.mapping-row .section-title{margin-bottom:12px}.constant-row{display:grid;grid-template-columns:1fr 110px 1fr auto;gap:10px;align-items:center;margin-bottom:10px}.wrap-field{margin-top:20px}.preview-button{margin:12px 0}.form-grid :deep(.el-input-number){width:100%}.muted{font-size:13px;line-height:1.7}pre{white-space:pre-wrap;overflow-wrap:anywhere}:deep(.el-checkbox){height:auto;white-space:normal;margin-bottom:12px}:deep(.el-checkbox__label){white-space:normal}@media(max-width:700px){.form-grid,.thirds{grid-template-columns:minmax(0,1fr)}.constant-row{grid-template-columns:minmax(0,1fr) 100px}.input-action{flex-wrap:wrap}}</style>
