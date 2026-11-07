import { api, newKey } from '../../lib/api'

export type Row = Record<string, any>
export type Page = { items: Row[]; nextCursor?: string; snapshotAt?: string }
export const enc = (value: unknown) => encodeURIComponent(String(value ?? ''))
export const errorText = (error: any) => [error?.code, error?.message || String(error), error?.traceId ? `追踪 ID：${error.traceId}` : ''].filter(Boolean).join(' · ')
export function stableKey() {
  let last = '', key = ''
  const choose = (body: unknown) => { const next = JSON.stringify(body); if (next !== last || !key) { last = next; key = newKey() }; return key }
  choose.clear = () => { last = ''; key = '' }
  return choose
}
export function taskIntent() {
  let last='', saved: {body:Row;key:string}|undefined
  const choose = (input:Row,seconds=120) => { const sig=JSON.stringify({input,seconds});if(!saved||sig!==last){last=sig;saved={body:{...input,expiresAt:ttl(seconds)},key:newKey()}};return saved }
  // Only clear after a successful create response. A lost response must retain the original intent.
  choose.clear = () => { last = ''; saved = undefined }
  return choose
}
export function artifactUploadIntent() {
  const choose = stableKey()
  const keyFor = (input:{kind:string;sha256:string;manifest:string;signature:string;file:{name:string;size:number;type:string;lastModified:number}}) => choose({
    kind:input.kind,sha256:input.sha256,manifest:input.manifest,signature:input.signature,
    file:{name:input.file.name,size:input.file.size,type:input.file.type,lastModified:input.file.lastModified},
  })
  keyFor.clear = choose.clear
  return keyFor
}
export async function loadChoices(path: string): Promise<Row[]> {
  const result: Row[] = []; let cursor = ''; let pages = 0
  do {
    const page = await api<Page>(`${path}${path.includes('?') ? '&' : '?'}limit=200${cursor ? '&cursor=' + enc(cursor) : ''}`)
    result.push(...page.items); cursor = page.nextCursor || ''; pages++
    if (pages >= 50 && cursor) throw new Error('选项超过 10000 条，请缩小资源范围后重试')
  } while (cursor)
  return result
}
export function canonicalUuid(input: string): string {
  let s = input.trim().toLowerCase()
  if (/^[0-9a-f]{4}$/.test(s)) s = '0000' + s
  if (/^[0-9a-f]{8}$/.test(s)) s += '-0000-1000-8000-00805f9b34fb'
  if (!/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(s)) throw new Error('UUID 应为 4 位、8 位或标准 128 位十六进制格式')
  return s
}
export function address(input: string, type: string, requireStatic = false) {
  const v = input.replace(/[:-]/g, '').trim().toUpperCase()
  if (!/^[0-9A-F]{12}$/.test(v) || ['000000000000','FFFFFFFFFFFF'].includes(v)) throw new Error('请输入有效的 6 字节蓝牙地址')
  if (requireStatic && type === 'RANDOM' && (parseInt(v.slice(0,2),16) & 0xc0) !== 0xc0) throw new Error('自动连接对端只接受 PUBLIC 或 random-static 地址')
  return v
}
export function payload(text: string, encoding: string, max: number, min = 0, ad = false): string {
  let binary = ''
  if (encoding === 'HEX') {
    const v = text.replace(/\s/g, '')
    if (!/^(?:[0-9a-fA-F]{2})*$/.test(v)) throw new Error('Hex 必须为完整字节，可用空格分隔')
    binary = (v.match(/../g) || []).map(v => String.fromCharCode(parseInt(v,16))).join('')
  } else {
    try { binary = atob(text.trim()) } catch { throw new Error('Base64 编码无效') }
    if (btoa(binary) !== text.trim()) throw new Error('请使用标准、带填充的 Base64')
  }
  if (binary.length < min || binary.length > max) throw new Error(`载荷长度应为 ${min}–${max} 字节，当前 ${binary.length} 字节`)
  if (ad) for (let i=0;i<binary.length;) { const n=binary.charCodeAt(i); if (!n) { if ([...binary.slice(i)].some(c => c.charCodeAt(0)!==0)) throw new Error('AD 终止字节后不能包含非零数据'); break }; if (i+n+1>binary.length) throw new Error('AD Length 超出剩余载荷'); i+=n+1 }
  return btoa(binary)
}
export function hasCapability(gateway: Row | undefined, operation: string) { return !!gateway?.capabilities?.includes(operation) }
export function gattProtocol(gateway: Row | undefined): 'GW32'|'LEGACY'|'UNAVAILABLE' {
  if (!gateway) return 'UNAVAILABLE'
  const model=String(gateway.model||''), gw32=model==='GW32I33V2'||model.startsWith('GW32I33V2-')
  return gw32 ? hasCapability(gateway,'GW32_CONTROL_PROFILE_1')?'GW32':'UNAVAILABLE' : 'LEGACY'
}
export function gattParameters(gateway:Row, form:Row, connection?:Row):Row {
  const protocol=gattProtocol(gateway),op=String(form.type),legacy=protocol==='LEGACY'
  if(protocol==='UNAVAILABLE'||!hasCapability(gateway,op))throw new Error('网关未声明此操作所需能力')
  if(legacy&&(!['GATT_READ','GATT_WRITE'].includes(op)||form.connectionRef))throw new Error('兼容网关仅支持不带持久连接引用的临时读写')
  if(!Number.isInteger(form.timeoutMs)||form.timeoutMs<100||form.timeoutMs>30000)throw new Error('操作超时应为 100–30000 ms')
  const params:Row={timeoutMs:form.timeoutMs}
  if(!legacy){if(!['NRF54','ESP32'].includes(form.radio)||!hasCapability(gateway,form.radio+'_BLE_GATT_CENTRAL'))throw new Error('网关未声明所选射频的 GATT 能力');params.radio=form.radio;if(form.connectionRef)params.connectionRef=form.connectionRef}
  if(['GATT_CONNECT','GATT_READ','GATT_WRITE'].includes(op)){params.deviceAddress=address(form.deviceAddress,form.addressType);params.addressType=form.addressType}
  if(op==='GATT_CONNECT')params.security=form.security
  if(!['GATT_CONNECT','GATT_DISCONNECT'].includes(op)){params.serviceUuid=canonicalUuid(form.serviceUuid);params.characteristicUuid=canonicalUuid(form.characteristicUuid)}
  if(op==='GATT_SUBSCRIBE')params.mode=form.mode
  if(op==='GATT_WRITE'){
    const modes=legacy?['WITH_RESPONSE','WITHOUT_RESPONSE']:['WITH_RESPONSE'],verifications=legacy?['READBACK','APPLICATION_REPLY','NONE']:['READBACK','NONE']
    params.writeMode=legacy?form.writeMode:'WITH_RESPONSE';params.verification=form.verification
    if(!modes.includes(params.writeMode)||!verifications.includes(params.verification))throw new Error('所选写入或验证方式不受当前网关协议支持')
    const maximum=legacy?3072:Math.min(512,connection?.attMtu?connection.attMtu-3:512)
    params.valueBase64=payload(form.value,form.encoding,maximum,1)
  }
  return params
}
export function ttl(seconds = 60) { return new Date(Date.now()+seconds*1000).toISOString() }
export const taskStates = ['QUEUED','DISPATCHED','ACCEPTED','EXECUTING','SUCCEEDED','FAILED','EXPIRED','CANCELLED','UNKNOWN']
