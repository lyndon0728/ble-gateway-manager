import {createRouter,createWebHistory} from 'vue-router'
import {session,restoreSession,can,landing} from './lib/auth'
const gateway=()=>import('./pages/GatewayPages.vue'),bluetooth=()=>import('./pages/BluetoothPages.vue'),data=()=>import('./pages/DataPages.vue'),processing=()=>import('./pages/ProcessingPages.vue'),targets=()=>import('./pages/TargetPages.vue'),tasks=()=>import('./pages/TaskPages.vue'),system=()=>import('./pages/SystemPages.vue')
const page=(path:string,id:string,title:string,component:any,permission='',global=false)=>({path,name:id,component,meta:{pageId:id,title,permission,global}})
export const routes=[
 page('/login','P00','登录',()=>import('./pages/LoginPage.vue')),
 page('/overview','P01','运行概览',()=>import('./pages/OverviewPage.vue'),'system:read',true),
 page('/gateways','P02','网关列表',gateway,'gateway:read'),page('/gateways/:gatewayId','P03','网关详情',gateway,'gateway:read'),page('/gateways/:gatewayId/configuration','P04','网关配置',gateway,'gateway:read'),
 page('/ble-peers','P05','蓝牙对端',bluetooth,'gatt:read'),page('/gatt-connections','P06','GATT连接与操作',bluetooth,'gatt:read'),page('/auto-connect-rules','P07','自动连接规则',bluetooth,'gatt:read'),
 page('/events','P08','数据事件',data,'data:read'),page('/gateways/:gatewayId/events/:eventId','P09','事件详情',data,'data:read'),
 page('/parsers','P10','解析器与试算',processing,'parser:read'),page('/rule-bundles','P11','解析规则包',processing,'rule:read',true),page('/quarantine','P12','隔离与重解析',processing,'data:read'),
 page('/targets','P13','转发目标',targets,'target:read',true),page('/targets/:targetId','P14','目标版本与字段映射',targets,'target:read',true),page('/deliveries','P15','投递记录',()=>import('./pages/DeliveryPages.vue'),'delivery:read'),
 page('/tasks','P16','命令任务',tasks,'task:read'),page('/tasks/:taskId','P17','任务详情',tasks,'task:read'),page('/artifacts','P18','固件与签名制品',tasks,'artifact:read',true),
 page('/scopes','P19','项目与分组',system,'scope:read'),page('/principals','P20','账号与应用凭据',system,'identity:write',true),page('/system','P21','系统运维',system,'system:read',true),page('/audit','P22','审计日志',system,'audit:read'),page('/integration','P23','接口与对接指南',system),
 page('/device-models','P24','型号模板库',()=>import('./pages/DeviceModelsPage.vue'),'profile:read'),page('/import-jobs/:jobId','P25','导入审阅工作台',()=>import('./pages/ImportWorkbenchPage.vue'),'profile:write'),page('/system/ai','P26','模型服务设置',()=>import('./pages/AISettingsPage.vue'),'identity:write',true),
 {path:'/',redirect:()=>session.ready?landing():'/login'},page('/forbidden','forbidden','暂无访问权限',()=>import('./pages/MessagePage.vue')),
 {path:'/:pathMatch(.*)*',component:()=>import('./pages/MessagePage.vue'),meta:{title:'页面未找到'}}
]
export const router=createRouter({history:createWebHistory('/ui/'),routes,scrollBehavior:()=>({top:0})})
router.beforeEach(async to=>{if(!session.ready)await restoreSession();if(!session.principal&&to.path!=='/login')return {path:'/login',query:{returnTo:to.fullPath}};if(session.principal&&to.path==='/login')return landing();const permission=String(to.meta.permission||'');if(permission&&!can(permission,to.meta.global?'scope_instance':undefined))return '/forbidden';document.title=(to.meta.title||'控制台')+' · 蓝牙网关管理'})
