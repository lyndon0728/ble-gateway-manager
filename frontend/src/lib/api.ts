export class ApiError extends Error {
  constructor(public status:number, public code:string, message:string, public traceId='',public retryAfter=''){super(message)}
}
export type RequestOptions={etag?:string;key?:string;signal?:AbortSignal}
export const newKey=()=>crypto.randomUUID()
const pendingKeys=new Map<string,string>()
const formKeys=new WeakMap<FormData,string>()
export function clearPendingRequests(){pendingKeys.clear()}
export async function apiResult<T=any>(path:string,method='GET',body?:any,options:RequestOptions={}):Promise<{data:T;etag:string;status:number}> {
  if(!path.startsWith('/')||path.startsWith('//'))throw new Error('仅允许访问本机平台接口')
  const headers:Record<string,string>={Accept:'application/json'}
  const csrf=sessionStorage.getItem('gw_csrf');if(csrf)headers['X-CSRF-Token']=csrf
  if(options.etag)headers['If-Match']=options.etag
  const isForm=body instanceof FormData
  if(body!==undefined&&!isForm)headers['Content-Type']='application/json'
  const encoded=body===undefined?undefined:isForm?body:JSON.stringify(body)
  let fingerprint=''
  if(!['GET','HEAD'].includes(method)&&!path.includes('/auth/')&&!path.includes('/credentials')){
    if(options.key)headers['Idempotency-Key']=options.key
    else if(isForm){if(!formKeys.has(body))formKeys.set(body,newKey());headers['Idempotency-Key']=formKeys.get(body)!}
    else {fingerprint=method+' '+path+' '+(encoded||'');if(!pendingKeys.has(fingerprint)){if(pendingKeys.size>=128)throw new Error('存在过多结果待核实的操作，请刷新并查询任务后再继续');pendingKeys.set(fingerprint,newKey())}headers['Idempotency-Key']=pendingKeys.get(fingerprint)!}
  }
  let response:Response
  try{response=await fetch(path,{method,headers,credentials:'same-origin',body:encoded,signal:options.signal})}
  catch(e){if(e instanceof DOMException&&e.name==='AbortError')throw e;throw new ApiError(0,'NETWORK_UNCERTAIN','网络暂不可达；写入结果可能待核实。请先查询已有任务，重试同一内容将保留幂等身份。')}
  let data:any={};if(response.status!==204){const text=await response.text();try{data=text?JSON.parse(text):{}}catch{throw new ApiError(response.status,'INVALID_RESPONSE','服务器返回了无法识别的响应',response.headers.get('X-Trace-Id')||'')}}
  if(response.ok){if(path==='/api/v1/auth/me'&&response.headers.get('X-CSRF-Token'))sessionStorage.setItem('gw_csrf',response.headers.get('X-CSRF-Token')!);if(fingerprint)pendingKeys.delete(fingerprint);return {data,etag:response.headers.get('ETag')||'',status:response.status}}
  if(fingerprint&&response.status<500&&response.status!==429)pendingKeys.delete(fingerprint)
  if(response.status===401&&!(path==='/api/v1/auth/sessions'&&method==='POST'))window.dispatchEvent(new Event('gateway-session-expired'))
  throw new ApiError(response.status,data.error?.code||'HTTP_'+response.status,data.error?.message||'请求未完成',data.error?.traceId||response.headers.get('X-Trace-Id')||'',response.headers.get('Retry-After')||'')
}
export async function api<T=any>(path:string,method='GET',body?:any,options:RequestOptions={}):Promise<T>{return (await apiResult<T>(path,method,body,options)).data}
export function errorText(error:unknown){return error instanceof ApiError?`${error.code} · ${error.message}${error.traceId?'（追踪 '+error.traceId+'）':''}`:error instanceof Error?error.message:String(error)}
export function query(path:string,params:Record<string,any>){const q=new URLSearchParams();for(const [k,v]of Object.entries(params))if(v!==undefined&&v!==null&&v!=='')q.set(k,String(v));return path+(q.size?'?'+q.toString():'')}
