import {reactive} from 'vue'
import {api,ApiError,clearPendingRequests} from './api'
export type Principal={principalId:string;name:string;type:string;state:string;revision:number;scopes:string[];roleBindings:{role:string;scopeId:string}[]}
export const session=reactive<{principal:Principal|null;scopes:any[];ready:boolean}>({principal:null,scopes:[],ready:false})
const roles:Record<string,string[]>={
 ADMIN:['*'],
 DEVICE_ADMIN:['scope:read','gateway:read','gateway:write','gateway:configure','gateway:reboot','diagnostics:run','gatt:read','gatt:manage','gatt:execute','firmware:deploy','task:read','task:create','task:cancel','artifact:read','artifact:write','operation:read','system:read','parser:read','target:read'],
 OPERATOR:['scope:read','gateway:read','gatt:read','gateway:configure','gateway:reboot','diagnostics:run','task:read','task:create','task:cancel','data:read','delivery:read','delivery:retry','operation:read','system:read','artifact:read'],
 DEVELOPER:['scope:read','gateway:read','gatt:read','task:read','data:read','data:raw','data:reprocess','delivery:read','delivery:retry','parser:read','parser:test','rule:read','target:read','operation:read'],
 AUDITOR:['scope:read','gateway:read','gatt:read','task:read','audit:read','operation:read','system:read','rule:read','target:read','parser:read']
}
export function can(permission:string,scopeId?:string){
 const principal=session.principal;if(!principal)return false
 return principal.roleBindings.some(binding=>{
  if(!(roles[binding.role]||[]).some(p=>p==='*'||p===permission))return false
  if(!scopeId)return true
  let current=scopeId;const seen=new Set<string>()
  while(current&&!seen.has(current)){seen.add(current);const scope=session.scopes.find(v=>v.scopeId===current);if(scope&&scope.state!=='ACTIVE')return false;if(binding.scopeId===current)return true;current=scope?.parentScopeId||''}
  return false
 })
}
export const canGateway=(permission:string,gateway:any)=>!!gateway?.groupId&&can(permission,gateway.groupId)
export async function loadScopes(){if(!can('scope:read'))return;const scopes:any[]=[];let cursor='';do{const p=await api('/api/v1/resource-scopes?limit=200'+(cursor?'&cursor='+encodeURIComponent(cursor):''));scopes.push(...p.items);cursor=p.nextCursor||'';if(scopes.length>10000)break}while(cursor);session.scopes=scopes}
export async function restoreSession(){try{session.principal=await api('/api/v1/auth/me');await loadScopes()}catch{session.principal=null;session.scopes=[]}finally{session.ready=true}}
export async function login(name:string,password:string){const result=await api('/api/v1/auth/sessions','POST',{name,password});sessionStorage.setItem('gw_csrf',result.csrfToken);session.principal=result.principal;session.ready=true;await loadScopes()}
export async function logout(){try{await api('/api/v1/auth/sessions/current','DELETE')}catch(e){if(!(e instanceof ApiError&&e.status===401))throw e}clearSession()}
export function clearSession(){sessionStorage.removeItem('gw_csrf');session.principal=null;session.scopes=[];clearPendingRequests()}
export function landing(){if(can('system:read','scope_instance'))return '/overview';if(can('data:read'))return '/events';if(can('gateway:read'))return '/gateways';return '/integration'}
