<script setup lang="ts">
import {computed,ref,onMounted,onBeforeUnmount} from 'vue'
import {useRoute,useRouter} from 'vue-router'
import {session,can,logout,clearSession} from './lib/auth'
import {errorText} from './lib/api'
import {ElMessage} from 'element-plus'
const route=useRoute(),router=useRouter(),collapsed=ref(window.innerWidth<760)
const windowWidthSmall=()=>window.innerWidth<900
const groups=[
 {name:'运行概览',items:[['/overview','运行概览','system:read','global']]},
 {name:'网关设备',items:[['/gateways','网关列表','gateway:read']]},
 {name:'蓝牙连接',items:[['/ble-peers','蓝牙对端','gatt:read'],['/gatt-connections','GATT连接与操作','gatt:read'],['/auto-connect-rules','自动连接规则','gatt:read'],['/device-models','型号模板库','profile:read']]},
 {name:'数据处理',items:[['/events','数据事件','data:read'],['/parsers','解析器与试算','parser:read'],['/rule-bundles','解析规则包','rule:read','global'],['/quarantine','隔离与重解析','data:read']]},
 {name:'客户转发',items:[['/targets','转发目标','target:read','global'],['/deliveries','投递记录','delivery:read']]},
 {name:'任务与升级',items:[['/tasks','命令任务','task:read'],['/artifacts','固件与签名制品','artifact:read','global']]},
 {name:'系统管理',items:[['/scopes','项目与分组','scope:read'],['/principals','账号与应用凭据','identity:write','global'],['/system','系统运维','system:read','global'],['/audit','审计日志','audit:read'],['/integration','接口与对接指南',''],['/system/ai','模型服务设置','identity:write','global']]}
]
const visibleGroups=computed(()=>groups.map(g=>({...g,items:g.items.filter(i=>!i[2]||can(i[2],i[3]==='global'?'scope_instance':undefined))})).filter(g=>g.items.length))
const active=(path:string)=>route.meta.pageId==='P09'?path==='/events':route.meta.pageId==='P25'?path==='/device-models':path==='/system'?route.path==='/system':route.path===path||route.path.startsWith(path+'/')
async function signOut(){try{await logout();await router.push('/login')}catch(e){ElMessage.error(errorText(e))}}
const expired=()=>{clearSession();router.replace({path:'/login',query:{returnTo:route.fullPath}})}
onMounted(()=>window.addEventListener('gateway-session-expired',expired));onBeforeUnmount(()=>window.removeEventListener('gateway-session-expired',expired))
</script>
<template>
 <router-view v-if="route.path==='/login'"/>
 <div v-else class="app-shell" :class="{'nav-collapsed':collapsed}">
  <aside class="sidebar"><a class="brand" href="/ui/"><span class="brand-mark">GW</span><span>蓝牙网关管理<small>GATEWAY CONSOLE</small></span></a>
   <nav aria-label="主导航"><section v-for="group in visibleGroups" :key="group.name" class="nav-group"><div class="nav-group-label">{{group.name}}</div><router-link v-for="item in group.items" :key="item[0]" :to="item[0]" :class="{'is-active':active(item[0])}" @click="collapsed=windowWidthSmall()">{{item[1]}}</router-link></section></nav>
   <footer class="sidebar-footer"><span class="status-dot"></span> 客户本地部署 <small>v0.4.0</small></footer>
  </aside>
  <div class="workspace"><header class="topbar"><div class="topbar-title"><el-button text class="menu-toggle" aria-label="切换导航" @click="collapsed=!collapsed">☰</el-button><span>管理控制台</span><span class="breadcrumb-sep">/</span><strong>{{route.meta.title}}</strong></div><div class="identity"><span>{{session.principal?.name}}</span><el-button text @click="signOut">退出</el-button></div></header><main class="page-content"><router-view :key="String(route.meta.pageId)"/></main><footer class="workspace-footer">本地管理 · 可靠采集 · 受控执行 <span>状态以服务器最新确认结果为准</span></footer></div>
 </div>
</template>
