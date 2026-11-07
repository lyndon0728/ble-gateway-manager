<script setup lang="ts">
import { computed, reactive, ref, watch } from 'vue'
import { useRouter } from 'vue-router'
import { ElMessageBox } from 'element-plus'
import { api } from '../../lib/api'
import { canGateway } from '../../lib/auth'
import { errorText, hasCapability, taskIntent, type Row } from './utils'
const props = defineProps<{ gateways: Row[]; gatewayId?: string }>()
const emit = defineEmits<{ submitted: [task: Row] }>()
const router=useRouter(), busy=ref(false), error=ref(''), result=ref<Row>(), intent=taskIntent()
const form=reactive({gatewayId:props.gatewayId||'',type:'DIAGNOSTICS',profile:'HEALTH',durationSeconds:5,wifiScan:'OFF',reason:'',ttl:120})
watch(()=>props.gatewayId,v=>{if(v)form.gatewayId=v})
const gateway=computed(()=>props.gateways.find(g=>g.gatewayId===form.gatewayId))
const allowed=computed(()=>!!gateway.value && gateway.value.state==='ACTIVE' && canGateway('task:create',gateway.value) && canGateway(form.type==='REBOOT'?'gateway:reboot':'diagnostics:run',gateway.value) && hasCapability(gateway.value,form.type))
async function submit(){error.value='';if(!allowed.value)return;try{
 if(form.type==='REBOOT'){if(!form.reason.trim())throw new Error('请填写重启原因');await ElMessageBox.confirm('重启会中断该网关的采集和连接。是否创建重启任务？','确认重启',{type:'warning',confirmButtonText:'创建任务',cancelButtonText:'返回'})}
 const parameters:Row=form.type==='REBOOT'?{reason:form.reason.trim()}:{profile:form.profile,durationSeconds:form.durationSeconds}
 if(form.type==='DIAGNOSTICS'&&form.profile==='NETWORK'&&hasCapability(gateway.value,'GW32_CONTROL_PROFILE_1')&&hasCapability(gateway.value,'NETWORK_WIFI_STA'))parameters.wifiScan=form.wifiScan
 const request=intent({gatewayId:form.gatewayId,type:form.type,parameters},form.ttl)
 busy.value=true;result.value=await api('/api/v1/tasks','POST',request.body,{key:request.key});intent.clear();emit('submitted',result.value!)
}catch(e){if(e!=='cancel'&&e!=='close')error.value=errorText(e)}finally{busy.value=false}}
</script>
<template><el-form label-position="top" @submit.prevent="submit">
 <el-alert v-if="error" :title="error" type="error" :closable="false" class="mb" />
 <el-form-item label="目标网关"><el-select v-model="form.gatewayId" filterable :disabled="!!gatewayId" placeholder="选择有操作权限的网关"><el-option v-for="g in gateways" :key="g.gatewayId" :label="g.displayName||g.gatewayId" :value="g.gatewayId" /></el-select></el-form-item>
 <el-form-item label="任务类型"><el-radio-group v-model="form.type"><el-radio-button value="DIAGNOSTICS">设备诊断</el-radio-button><el-radio-button value="REBOOT">重启</el-radio-button></el-radio-group></el-form-item>
 <template v-if="form.type==='DIAGNOSTICS'"><el-form-item label="诊断范围"><el-select v-model="form.profile"><el-option label="健康状态" value="HEALTH"/><el-option label="网络" value="NETWORK"/><el-option label="蓝牙" value="BLE"/></el-select></el-form-item><el-form-item label="采样时长（秒）"><el-input-number v-model="form.durationSeconds" :min="1" :max="60"/></el-form-item><el-form-item v-if="form.profile==='NETWORK'&&hasCapability(gateway,'NETWORK_WIFI_STA')" label="Wi-Fi 扫描"><el-select v-model="form.wifiScan"><el-option label="不扫描" value="OFF"/><el-option label="被动扫描" value="PASSIVE"/><el-option label="主动扫描" value="ACTIVE"/></el-select></el-form-item></template>
 <el-form-item v-else label="重启原因"><el-input v-model="form.reason" type="textarea" maxlength="300" show-word-limit/></el-form-item>
 <el-form-item label="任务有效期（秒）"><el-input-number v-model="form.ttl" :min="30" :max="3600"/></el-form-item>
 <p class="muted" v-if="!allowed">请选择具有相应能力且在授权范围内的活动网关。</p>
 <el-button type="primary" native-type="submit" :disabled="!allowed||!!result" :loading="busy">创建任务</el-button>
 <el-button v-if="result" @click="router.push('/tasks/'+result.taskId)">查看任务 {{result.taskId}}</el-button>
 </el-form></template>
<style scoped>.mb{margin-bottom:16px}.el-select{width:100%}.muted{font-size:13px;color:#64748b}</style>
