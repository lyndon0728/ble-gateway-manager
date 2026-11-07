<script setup lang="ts">import {pretty} from '@/lib/format';defineProps<{value:any;label?:string}>()</script>
<template><section class="json-view"><div v-if="label" class="section-title">{{label}}</div><pre>{{pretty(value)}}</pre></section></template>
