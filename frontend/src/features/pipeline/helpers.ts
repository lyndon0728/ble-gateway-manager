import { onBeforeUnmount, ref } from 'vue'
import { api, newKey } from '@/lib/api'

export type RecordData = Record<string, any>
export interface CursorPage { items: RecordData[]; nextCursor?: string; snapshotAt?: string }

export function errorText(error: unknown): string {
  const e = error as { code?: string; message?: string; traceId?: string }
  return [e?.code, e?.message || '请求失败，请重试', e?.traceId ? `追踪 ID：${e.traceId}` : ''].filter(Boolean).join(' · ')
}
export function listText(value: string): string[] {
  return [...new Set(value.split(/[\s,，;；]+/).map(v => v.trim()).filter(Boolean))]
}
export function urlWithQuery(path: string, values: Record<string, unknown>): string {
  const q = new URLSearchParams()
  for (const [k, v] of Object.entries(values)) if (v !== '' && v !== null && v !== undefined) q.set(k, String(v))
  return `${path}${q.size ? `?${q}` : ''}`
}
export function useCursor(path: () => string, filters: () => Record<string, unknown> = () => ({})) {
  const items = ref<RecordData[]>([]), nextCursor = ref(''), snapshotAt = ref(''), loading = ref(false), error = ref('')
  const cursors = ref<string[]>(['']), index = ref(0), loaded = ref(false)
  let requestNumber = 0, controller: AbortController | undefined, activeFilters: Record<string, unknown> = {}
  async function read(cursor: string, nextIndex: number, replace = false) {
    controller?.abort(); controller = new AbortController(); const version = ++requestNumber
    loading.value = true; error.value = ''
    try {
      const result = await api<CursorPage>(urlWithQuery(path(), { ...activeFilters, limit: 30, cursor }), 'GET', undefined, { signal: controller.signal })
      if (version !== requestNumber) return
      if (replace) cursors.value = ['']
      cursors.value[nextIndex] = cursor; index.value = nextIndex
      items.value = result.items || []; nextCursor.value = result.nextCursor || ''; snapshotAt.value = result.snapshotAt || ''; loaded.value = true
    } catch (e) { if (version === requestNumber && (e as Error).name !== 'AbortError') error.value = errorText(e) }
    finally { if (version === requestNumber) loading.value = false }
  }
  const refresh = () => { activeFilters = { ...filters() }; return read('', 0, true) }
  const next = () => nextCursor.value ? read(nextCursor.value, index.value + 1) : Promise.resolve()
  const previous = () => index.value > 0 ? read(cursors.value[index.value - 1] || '', index.value - 1) : Promise.resolve()
  onBeforeUnmount(() => { requestNumber++; controller?.abort() })
  return { items, nextCursor, snapshotAt, loading, error, index, loaded, refresh, next, previous }
}

// Retain the key for an uncertain retry; a different operation body gets a new key.
export function mutationKey() {
  let signature = '', key = ''
  return {
    for(body: unknown) { const next = JSON.stringify(body); if (!key || next !== signature) { signature = next; key = newKey() }; return key },
    clear() { key = ''; signature = '' },
  }
}
export function payloadBase64(text: string, encoding: string): string {
  if (encoding === 'hex') {
    const value = text.replace(/\s+/g, '')
    if (value.length % 2 || !/^[0-9a-f]*$/i.test(value)) throw new Error('Hex 必须由完整的两位十六进制字节组成')
    let binary = ''; for (let i = 0; i < value.length; i += 2) binary += String.fromCharCode(parseInt(value.slice(i, i + 2), 16))
    return btoa(binary)
  }
  const value = text.trim()
  if (btoa(atob(value)) !== value) throw new Error('请输入标准、含正确填充的 Base64')
  return value
}
export function hexPayload(value: string): string {
  try { return [...atob(value)].map(v => v.charCodeAt(0).toString(16).padStart(2, '0')).join(' ').toUpperCase() } catch { return 'Base64 数据不可解码' }
}
export async function copyText(value: string) { await navigator.clipboard.writeText(value) }
