<script setup lang="ts">
import { computed } from 'vue'
import { pretty } from '@/lib/format'
const props=defineProps<{ value: unknown }>()
const rows=computed(()=>props.value && typeof props.value==='object' && !Array.isArray(props.value) ? Object.entries(props.value as Record<string,unknown>).map(([name,value])=>({name,value})) : [{name:'结果',value:props.value}])
function display(value:unknown) { return value!==null && typeof value==='object' ? pretty(value) : value===null ? 'null' : String(value ?? '—') }
</script>
<template><el-table :data="rows" border><el-table-column prop="name" label="字段" min-width="160"/><el-table-column label="值" min-width="240"><template #default="{row}"><pre class="record-value">{{ display(row.value) }}</pre></template></el-table-column></el-table></template>
<style scoped>.record-value{margin:0;white-space:pre-wrap;overflow-wrap:anywhere;font:13px/1.6 ui-monospace,monospace}</style>
