import {ref,onBeforeUnmount} from 'vue'
import {api,query,errorText} from './api'
export function usePaged(path:string){
 const items=ref<any[]>([]),loading=ref(false),error=ref(''),nextCursor=ref(''),snapshotAt=ref(''),limit=ref(50)
 const past=ref<string[]>([]);let active='',generation=0,controller:AbortController|undefined,activeFilters:Record<string,any>={}
 async function load(filters:Record<string,any>={},cursor=''){controller?.abort();controller=new AbortController();const current=++generation;loading.value=true;error.value='';try{const p=await api(query(path,{...filters,limit:limit.value,cursor}),'GET',undefined,{signal:controller.signal});if(current!==generation)return false;items.value=p.items||[];nextCursor.value=p.nextCursor||'';snapshotAt.value=p.snapshotAt||'';active=cursor;activeFilters={...filters};return true}catch(e){if(current===generation&&(e as Error).name!=='AbortError')error.value=errorText(e);return false}finally{if(current===generation)loading.value=false}}
 const refresh=async(filters:Record<string,any>={})=>{if(await load(filters))past.value=[]}
 const next=async(_filters:Record<string,any>={})=>{if(!nextCursor.value||loading.value)return;const previous=active;if(await load(activeFilters,nextCursor.value))past.value.push(previous)}
 const previous=async(_filters:Record<string,any>={})=>{if(loading.value)return;const cursor=past.value.at(-1);if(cursor!==undefined&&await load(activeFilters,cursor))past.value.pop()}
 onBeforeUnmount(()=>{generation++;controller?.abort()})
 return {items,loading,error,nextCursor,snapshotAt,limit,past,load,refresh,next,previous}
}
