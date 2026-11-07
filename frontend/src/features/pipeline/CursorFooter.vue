<script setup lang="ts">
import { dateTime } from '@/lib/format'
defineProps<{ count: number; page: number; next?: string; snapshot?: string; loading?: boolean }>()
defineEmits<{ previous: []; next: []; refresh: [] }>()
</script>
<template>
  <div class="cursor-footer">
    <div class="muted">第 {{ page + 1 }} 页 · 本页 {{ count }} 条 <span v-if="snapshot">· 快照 {{ dateTime(snapshot) }}</span></div>
    <div><el-button :disabled="page === 0 || loading" @click="$emit('previous')">上一页</el-button><el-button :disabled="!next || loading" @click="$emit('next')">下一页</el-button><el-button :disabled="loading" @click="$emit('refresh')">重新查询</el-button></div>
  </div>
</template>
<style scoped>.cursor-footer{display:flex;justify-content:space-between;gap:12px;align-items:center;flex-wrap:wrap;margin-top:16px}.muted{font-size:13px;color:var(--el-text-color-secondary)}</style>
