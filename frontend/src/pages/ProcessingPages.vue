<script setup lang="ts">
import { useRoute } from 'vue-router'
import ParserWorkbench from '@/features/pipeline/ParserWorkbench.vue'
import RuleBundlePanel from '@/features/pipeline/RuleBundlePanel.vue'
import ReprocessPanel from '@/features/pipeline/ReprocessPanel.vue'
const route=useRoute()
</script>
<template><ParserWorkbench v-if="route.meta.pageId==='P10'" :key="route.fullPath"/><RuleBundlePanel v-else-if="route.meta.pageId==='P11'"/><ReprocessPanel v-else :key="route.fullPath"/></template>
