<script setup lang="ts">
import { computed } from 'vue'
import type { Row } from './utils'
const model=defineModel<Row|null>({required:true})
const fields=[{key:'name',label:'名称'},{key:'ibeaconUuid',label:'iBeacon UUID'},{key:'rssi',label:'信号强度 RSSI'},{key:'major',label:'Major'},{key:'minor',label:'Minor'},{key:'serviceUuid',label:'服务 UUID'}]
const full='ffffffffffffffffffffffffffffffff', uuid='00000000-0000-0000-0000-000000000000'
const enabled=computed({get:()=>model.value!==null,set:(v:boolean)=>{model.value=v?{version:2,scope:'REPORT',rules:[newRule()]}:null}})
function newRule(){return {action:'ALLOW',reportKinds:['LEGACY_ADV'],fieldMask:4,rssi:{mode:'RANGE',minDbm:-80,maxDbm:20}}}
function scopeChanged(){if(!model.value)return;if(model.value.scope==='ADV_SCAN_RSP_PAIR'){model.value.pairWindowMs=500;for(const r of model.value.rules)r.reportKinds=['LEGACY_ADV']}else delete model.value.pairWindowMs}
function selected(r:Row,i:number){return (r.fieldMask&(1<<i))!==0}
function select(r:Row,i:number,on:any){const key=fields[i]!.key;if(!on){r.fieldMask&=~(1<<i);delete r[key];return}r.fieldMask|=1<<i;r[key]=key==='name'?{mode:'PREFIX',source:'ANY',valueBase64:'QmNuLQ==',maskBase64:'/////w=='}:key==='rssi'?{mode:'RANGE',minDbm:-80,maxDbm:20}:key==='major'||key==='minor'?{value:0,mask:65535}:{value:uuid,maskHex:full}}
function rssiMode(r:Row){r.rssi=r.rssi.mode==='RANGE'?{mode:'RANGE',minDbm:-80,maxDbm:20}:{mode:'BITS',valueDbm:-64,mask:255}}
function nameText(r:Row){try{return new TextDecoder('utf-8',{fatal:true}).decode(Uint8Array.from(atob(r.name.valueBase64),(c:string)=>c.charCodeAt(0)))}catch{return ''}}
function setName(r:Row,text:string){const b=new TextEncoder().encode(text);r.name.valueBase64=btoa(String.fromCharCode(...b));r.name.maskBase64=btoa(String.fromCharCode(...new Uint8Array(b.length).fill(255)))}
</script>
<template><div class="beacon-v2">
 <el-switch v-model="enabled" active-text="启用组合掩码过滤" inactive-text="关闭 V2 过滤"/>
 <template v-if="model">
  <el-form-item label="匹配范围"><el-select v-model="model.scope" @change="scopeChanged"><el-option label="仅当前广播报告" value="REPORT"/><el-option label="广播与扫描响应配对（500 ms）" value="ADV_SCAN_RSP_PAIR"/></el-select></el-form-item>
  <el-alert v-if="model.scope==='ADV_SCAN_RSP_PAIR'" title="需同时启用主动扫描。仅同一无线芯片、启动和配置版本的传统广播可配对；超时、缺少扫描响应或配对有歧义时拒绝该广播。" type="info" :closable="false"/>
  <p>每条规则内部条件同时满足；规则从上到下首次匹配决定允许或拒绝。存在允许规则时，未命中默认拒绝。掩码为 0 的位不参与比较。</p>
  <fieldset v-for="(r,index) in model.rules" :key="index"><legend>规则 {{Number(index)+1}}</legend>
   <el-select v-model="r.action" aria-label="规则动作"><el-option label="允许" value="ALLOW"/><el-option label="拒绝" value="DENY"/></el-select>
   <el-select v-model="r.reportKinds" multiple :disabled="model.scope==='ADV_SCAN_RSP_PAIR'" aria-label="报告类型"><el-option v-for="kind in ['LEGACY_ADV','LEGACY_SCAN_RSP','EXT_ADV','EXT_SCAN_RSP']" :key="kind" :value="kind" :label="kind"/></el-select>
   <div class="predicates"><el-checkbox v-for="(f,i) in fields" :key="f.key" :model-value="selected(r,i)" @change="select(r,i,$event)">{{f.label}}</el-checkbox></div>
   <fieldset v-if="r.name"><legend>名称掩码</legend>
    <el-select v-model="r.name.mode" aria-label="名称匹配方式"><el-option label="完全匹配" value="EXACT"/><el-option label="前缀匹配" value="PREFIX"/></el-select>
    <el-select v-model="r.name.source" aria-label="名称来源"><el-option label="完整名称" value="COMPLETE"/><el-option label="短名称" value="SHORTENED"/><el-option label="两者均可" value="ANY"/></el-select>
    <label>名称文本（UTF-8，编辑后掩码全部置为 FF）<el-input :model-value="nameText(r)" @update:model-value="setName(r,$event)"/></label>
    <label>名称原始字节 Base64（1–29 字节）<el-input v-model="r.name.valueBase64"/></label><label>逐字节掩码 Base64（必须等长且非全零）<el-input v-model="r.name.maskBase64"/></label>
   </fieldset>
   <fieldset v-for="key in ['ibeaconUuid','serviceUuid'].filter(k=>r[k])" :key="key"><legend>{{key==='ibeaconUuid'?'iBeacon UUID':'服务 UUID'}}</legend><label>规范 UUID<el-input v-model="r[key].value" placeholder="xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"/></label><label>128 位掩码（32 位小写十六进制）<el-input v-model="r[key].maskHex"/></label></fieldset>
   <fieldset v-if="r.rssi"><legend>RSSI</legend><el-select v-model="r.rssi.mode" @change="rssiMode(r)"><el-option label="范围" value="RANGE"/><el-option label="有符号字节位掩码" value="BITS"/></el-select><template v-if="r.rssi.mode==='RANGE'"><label>最小 dBm<el-input-number v-model="r.rssi.minDbm" :min="-127" :max="20"/></label><label>最大 dBm<el-input-number v-model="r.rssi.maxDbm" :min="-127" :max="20"/></label></template><template v-else><p>位掩码比较 8 位补码。例如值 -64、掩码 240 命中 -64～-49 dBm，不能用作“至少 -64 dBm”的阈值。</p><label>值 dBm<el-input-number v-model="r.rssi.valueDbm" :min="-127" :max="20"/></label><label>掩码（十进制 1–255）<el-input-number v-model="r.rssi.mask" :min="1" :max="255"/></label></template></fieldset>
   <fieldset v-for="key in ['major','minor'].filter(k=>r[k])" :key="key"><legend>{{key}}</legend><label>值（十进制 0–65535）<el-input-number v-model="r[key].value" :min="0" :max="65535"/></label><label>掩码（十进制 1–65535）<el-input-number v-model="r[key].mask" :min="1" :max="65535"/></label></fieldset>
   <el-button :disabled="Number(index)===0" @click="model.rules.splice(Number(index)-1,0,...model.rules.splice(Number(index),1))">上移</el-button><el-button type="danger" :disabled="model.rules.length===1" @click="model.rules.splice(Number(index),1)">删除规则</el-button>
  </fieldset><el-button :disabled="model.rules.length>=8" @click="model.rules.push(newRule())">添加规则（最多 8 条）</el-button>
 </template>
</div></template>
<style scoped>.beacon-v2{width:100%}fieldset{border:1px solid #dce3eb;padding:12px;margin:12px 0;border-radius:8px;display:flex;flex-wrap:wrap;gap:10px}fieldset>fieldset{width:100%}legend{font-weight:600}label{display:grid;gap:5px;flex:1;min-width:190px}.el-select{min-width:180px}.predicates{width:100%}p{font-size:13px;color:#526175}</style>
