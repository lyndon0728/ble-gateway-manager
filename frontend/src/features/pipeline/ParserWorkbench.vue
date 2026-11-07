<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useRoute } from 'vue-router'
import { ElMessage } from 'element-plus'
import { api } from '@/lib/api'
import { can } from '@/lib/auth'
import { pretty } from '@/lib/format'
import StructuredRecord from './StructuredRecord.vue'
import CursorFooter from './CursorFooter.vue'
import { useCursor, errorText, payloadBase64, copyText, type RecordData } from './helpers'
const route=useRoute(),catalog=useCursor(()=>'/api/v1/parsers'),selected=ref(''),encoding=ref('hex'),payload=ref(''),sourceEventId=ref('')
const metadataText=ref('{}'),expectedText=ref(''),result=ref<RecordData|null>(null),busy=ref(false),error=ref('')
const parser=computed(()=>catalog.items.value.find(v=>`${v.parserId}@${v.version}`===selected.value))
const inputBytes=computed(()=>{try{return atob(payloadBase64(payload.value,encoding.value)).length}catch{return null}})
let testGeneration=0
async function initialize(){
  await catalog.refresh()
  selected.value=catalog.items.value.some(v=>`${v.parserId}@${v.version}`===selected.value)?selected.value:catalog.items.value[0]?`${catalog.items.value[0].parserId}@${catalog.items.value[0].version}`:''
  if(route.query.gatewayId && route.query.eventId && can('data:raw')) {
    try{const e=await api(`/api/v1/gateways/${encodeURIComponent(String(route.query.gatewayId))}/events/${encodeURIComponent(String(route.query.eventId))}?includeRaw=true`);if(e.raw){payload.value=e.raw.payloadBase64;encoding.value='base64';sourceEventId.value=e.eventId;metadataText.value=pretty({protocolHint:e.raw.protocolHint,sourceEventId:e.eventId});if(e.raw.protocolHint==='ble-ad-v1')selected.value='ble_ad@1.1.0'}}catch(e){error.value=errorText(e)}
  }
}
function sample(kind:string){
  const examples:Record<string,string>={sensor:'09 C4 13 88',uuid16:'03 03 0F 18',uuid32:'05 05 78 56 34 12',uuid128:'11 07 78 56 34 12 F0 DE BC 9A 78 56 34 12 78 56 34 12'}
  selected.value=kind==='sensor'?'demo_sensor@1.0.0':'ble_ad@1.1.0';encoding.value='hex';payload.value=examples[kind] || '';result.value=null;sourceEventId.value='';error.value=''
}
async function test(){
  if(busy.value || !parser.value || !can('parser:test'))return
  const current=++testGeneration;busy.value=true;error.value='';result.value=null
  try{
    const data=payloadBase64(payload.value,encoding.value)
    if(atob(data).length>parser.value.maxInputBytes)throw new Error(`当前版本最多接受 ${parser.value.maxInputBytes} 字节`)
    const metadata=JSON.parse(metadataText.value || '{}');if(!metadata || Array.isArray(metadata) || typeof metadata!=='object')throw new Error('元数据必须为 JSON 对象')
    if(sourceEventId.value)metadata.sourceEventId=sourceEventId.value
    const body:RecordData={payloadBase64:data,metadata}
    if(expectedText.value.trim()){body.expected=JSON.parse(expectedText.value);if(!Array.isArray(body.expected))throw new Error('预期结果必须为 JSON 数组')}
    const response=await api(`/api/v1/parsers/${encodeURIComponent(parser.value.parserId)}/versions/${encodeURIComponent(parser.value.version)}/tests`,'POST',body)
    if(current===testGeneration)result.value=response
  }catch(e){if(current===testGeneration)error.value=errorText(e)}finally{busy.value=false}
}
async function copy(){if(result.value){try{await copyText(pretty(result.value.outputs));ElMessage.success('试算结果已复制')}catch(e){error.value=errorText(e)}}}
watch([selected,encoding,payload,sourceEventId,metadataText,expectedText],()=>{testGeneration++;result.value=null;error.value=''},{flush:'sync'})
watch(()=>catalog.items.value,items=>{if(!items.some(v=>`${v.parserId}@${v.version}`===selected.value))selected.value=items[0]?`${items[0].parserId}@${items[0].version}`:''})
onBeforeUnmount(()=>{testGeneration++})
onMounted(initialize)
</script>
<template>
  <div class="page-heading"><div><h1>解析器与调试</h1><p class="muted">用已安装的可信解析器试算报文，不写入客户业务数据。</p></div><el-button @click="initialize" :loading="catalog.loading.value">刷新目录</el-button></div>
  <el-alert v-if="catalog.error.value || error" :title="catalog.error.value || error" type="error" :closable="false" show-icon/>
  <div class="parser-layout">
    <section class="panel"><h2>安装目录</h2><el-radio-group v-model="selected" class="parser-options"><el-radio v-for="p in catalog.items.value" :key="`${p.parserId}@${p.version}`" :value="`${p.parserId}@${p.version}`"><strong>{{ p.parserId }}</strong><small>{{ p.version }} · {{ p.schemaVersion }}</small></el-radio></el-radio-group><el-descriptions v-if="parser" :column="1" border><el-descriptions-item label="输入上限">{{ parser.maxInputBytes }} 字节</el-descriptions-item><el-descriptions-item label="输出记录上限">{{ parser.maxOutputRecords }}</el-descriptions-item><el-descriptions-item label="状态">{{ parser.state }}</el-descriptions-item></el-descriptions><CursorFooter v-if="catalog.nextCursor.value || catalog.index.value" :count="catalog.items.value.length" :page="catalog.index.value" :next="catalog.nextCursor.value" :snapshot="catalog.snapshotAt.value" :loading="catalog.loading.value" @next="catalog.next()" @previous="catalog.previous()" @refresh="initialize"/></section>
    <section class="panel"><div class="section-title"><h2>测试报文</h2><el-tag type="info">当前试算</el-tag></div><div class="toolbar samples"><span class="muted">示例</span><el-button size="small" @click="sample('sensor')">温湿度</el-button><el-button size="small" @click="sample('uuid16')">16 位 UUID</el-button><el-button size="small" @click="sample('uuid32')">32 位 UUID</el-button><el-button size="small" @click="sample('uuid128')">128 位 UUID</el-button></div>
      <el-form label-position="top" @submit.prevent="test"><el-form-item label="编码"><el-radio-group v-model="encoding"><el-radio-button value="hex">Hex</el-radio-button><el-radio-button value="base64">Base64</el-radio-button></el-radio-group></el-form-item><el-form-item label="报文内容"><el-input v-model="payload" type="textarea" :rows="7" maxlength="180000" placeholder="输入原始字节；Hex 可用空格分隔"/><span class="muted">{{ inputBytes === null ? '编码尚不完整' : `${inputBytes} 字节` }}</span></el-form-item><el-form-item label="来源事件 ID（可选）"><el-input v-model="sourceEventId" maxlength="128"/></el-form-item>
      <el-collapse><el-collapse-item title="高级：元数据与预期结果"><el-form-item label="元数据 JSON"><el-input v-model="metadataText" type="textarea" :rows="3"/></el-form-item><el-form-item label="预期 outputs JSON 数组（留空不比较）"><el-input v-model="expectedText" type="textarea" :rows="4"/></el-form-item></el-collapse-item></el-collapse>
      <el-button class="run-button" type="primary" native-type="submit" :loading="busy" :disabled="!parser || !can('parser:test')">运行试算</el-button><p v-if="!can('parser:test')" class="muted">当前角色可查看目录，没有运行试算权限。</p></el-form>
    </section>
  </div>
  <section class="panel" v-if="result"><div class="section-title"><h2>试算结果</h2><div class="toolbar"><el-tag :type="result.passed?'success':'danger'">{{ result.passed?'通过':'未通过' }}</el-tag><span class="muted">{{ result.cpuTimeMicros }} μs</span><el-button @click="copy">复制输出</el-button></div></div><el-alert v-for="(item,i) in result.errors" :key="i" :title="`${item.code} · ${item.message}`" type="error" :closable="false" show-icon/><div v-for="(record,i) in result.outputs" :key="i" class="output-record"><h3>记录 {{ Number(i) + 1 }}</h3><StructuredRecord :value="record"/></div><el-empty v-if="!result.outputs?.length" description="没有产生输出记录"/><el-collapse><el-collapse-item title="实现摘要与完整响应"><pre>{{ pretty(result) }}</pre></el-collapse-item></el-collapse></section>
</template>
<style scoped>h1{font-size:26px;margin:0}h2{font-size:18px;margin:0 0 14px}h3{font-size:15px}.parser-layout{display:grid;grid-template-columns:260px minmax(0,1fr);gap:18px}.parser-options{display:flex;flex-direction:column;align-items:stretch;margin-bottom:18px;gap:5px}.parser-options :deep(.el-radio){height:auto;min-height:48px;white-space:normal;margin-right:0}.parser-options small{display:block;color:var(--el-text-color-secondary);margin-top:3px}.panel{margin-bottom:18px;min-width:0}.samples{margin-bottom:18px;flex-wrap:wrap}.run-button{margin-top:18px}.output-record{margin-top:18px}pre{white-space:pre-wrap;overflow-wrap:anywhere}@media(max-width:850px){.parser-layout{grid-template-columns:minmax(0,1fr)}}</style>
