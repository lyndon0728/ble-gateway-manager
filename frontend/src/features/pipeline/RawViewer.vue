<script setup lang="ts">
import { computed, ref } from 'vue'
import { ElMessage } from 'element-plus'
import { pretty } from '@/lib/format'
import { hexPayload, copyText, errorText } from './helpers'
const props = defineProps<{ raw: Record<string, any> }>()
const encoding = ref('hex')
const value = computed(() => encoding.value === 'hex' ? hexPayload(props.raw.payloadBase64 || '') : props.raw.payloadBase64 || '')
async function copy() { try { await copyText(value.value); ElMessage.success('已复制当前编码的原文') } catch (e) { ElMessage.error(errorText(e)) } }
</script>
<template>
  <div class="toolbar"><el-radio-group v-model="encoding"><el-radio-button value="hex">Hex</el-radio-button><el-radio-button value="base64">Base64</el-radio-button></el-radio-group><el-button @click="copy">复制原文</el-button></div>
  <pre class="payload">{{ value || '空载荷' }}</pre>
  <el-descriptions :column="2" border><el-descriptions-item label="协议">{{ raw.protocolHint || '未提供' }}</el-descriptions-item><el-descriptions-item label="时间质量">{{ raw.timeQuality || '未提供' }}</el-descriptions-item><el-descriptions-item label="采集时间">{{ raw.capturedAt || '未提供' }}</el-descriptions-item><el-descriptions-item label="包类型">{{ raw.packetType || '未提供' }}</el-descriptions-item></el-descriptions>
  <el-collapse class="advanced"><el-collapse-item title="完整原文元数据（只读）"><pre>{{ pretty(raw) }}</pre></el-collapse-item></el-collapse>
</template>
<style scoped>.toolbar{display:flex;gap:12px;flex-wrap:wrap;margin-bottom:12px}.payload{white-space:pre-wrap;overflow-wrap:anywhere;padding:16px;background:var(--el-fill-color-light);border-radius:8px;max-height:420px;overflow:auto;font:13px/1.8 ui-monospace,monospace}.advanced{margin-top:16px}pre{white-space:pre-wrap;overflow-wrap:anywhere}</style>
