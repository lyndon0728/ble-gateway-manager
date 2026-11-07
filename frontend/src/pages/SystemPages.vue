<script setup lang="ts">
import { computed, onBeforeUnmount, reactive, ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'
import { ElMessage, ElMessageBox } from 'element-plus'
import { api, apiResult, ApiError, errorText, query } from '@/lib/api'
import { can, clearSession, loadScopes, session } from '@/lib/auth'
import { usePaged } from '@/lib/paged'
import { bytes, dateTime, pretty, statusLabel } from '@/lib/format'
import Pager from '@/components/Pager.vue'
import JsonView from '@/components/JsonView.vue'
import { backupCommands, customerAck, customerBatch, errorGuide, reconcileCommand, revisionExample, roles, topics } from '@/features/system/guide'

type Row = Record<string, any>
type Binding = { role: string; scopeId: string }
const route = useRoute(), router = useRouter()
const pageId = computed(() => String(route.meta.pageId))
const error = ref(''), busy = ref(false), loading = ref(false)
const identityAllowed = computed(() => can('identity:write', 'scope_instance'))
const systemAllowed = computed(() => can('system:read', 'scope_instance'))
const maintainAllowed = computed(() => can('system:maintain', 'scope_instance'))
const enc = (v: unknown) => encodeURIComponent(String(v))
const cancelled = (e: unknown) => e === 'cancel' || e === 'close'
const roleLabel = (v: string) => roles.find(r => r.value === v)?.label || v
const scopeLabel = (id: string) => session.scopes.find(s => s.scopeId === id)?.name || id
const activeScopes = computed(() => session.scopes.filter(s => s.state === 'ACTIVE'))
async function copy(value: string) {
  try { await navigator.clipboard.writeText(value); ElMessage.success('已复制') }
  catch { ElMessage.warning('浏览器未允许复制，请选择文本后手动复制') }
}

// All visible scope pages are loaded by the shared auth store. Missing parents
// remain visible roots here; their absence is never interpreted as deletion.
const selectedScope = ref<Row>()
const scopeTree = computed(() => {
  const map = new Map<string, Row>()
  session.scopes.forEach(s => map.set(s.scopeId, { ...s, children: [] }))
  const roots: Row[] = []
  map.forEach(s => { const parent = map.get(s.parentScopeId); if (parent) parent.children.push(s); else roots.push(s) })
  return roots
})
const scopeDialog = ref(false), editingScope = ref<Row>()
const scopeForm = reactive({ kind: 'PROJECT', parentScopeId: 'scope_instance', name: '', state: 'ACTIVE' })
const scopeParents = computed(() => activeScopes.value.filter(s => s.kind === (scopeForm.kind === 'PROJECT' ? 'INSTANCE' : 'PROJECT') && can('scope:write', s.scopeId)))
const scopeWritable = computed(() => !!selectedScope.value && (can('scope:write', selectedScope.value.scopeId) || can('scope:write', 'scope_instance')))
async function refreshScopes() {
  if (!can('scope:read')) return
  loading.value = true; error.value = ''
  try {
    await loadScopes()
    const id = selectedScope.value?.scopeId || String(route.query.scopeId || '')
    selectedScope.value = session.scopes.find(s => s.scopeId === id) || session.scopes[0]
  } catch (e) { error.value = errorText(e) } finally { loading.value = false }
}
function openScope(kind: string, edit = false) {
  error.value = ''; editingScope.value = edit ? selectedScope.value : undefined
  const s = editingScope.value
  scopeForm.kind = s?.kind || kind
  scopeForm.parentScopeId = s?.parentScopeId || (kind === 'GROUP' ? (selectedScope.value?.kind === 'PROJECT' ? selectedScope.value.scopeId : selectedScope.value?.parentScopeId || '') : 'scope_instance')
  if (!s && !scopeParents.value.some(p => p.scopeId === scopeForm.parentScopeId)) scopeForm.parentScopeId = scopeParents.value[0]?.scopeId || ''
  scopeForm.name = s?.name || ''; scopeForm.state = s?.state || 'ACTIVE'; scopeDialog.value = true
}
async function saveScope() {
  if (busy.value) return
  try {
    if (!scopeForm.name.trim()) throw new Error('请输入范围名称')
    const s = editingScope.value
    if (s && !can('scope:write', s.scopeId) && !can('scope:write', 'scope_instance')) throw new Error('没有此范围的修改权限')
    if (!s && !scopeParents.value.some(p => p.scopeId === scopeForm.parentScopeId)) throw new Error('请选择允许写入的活动父范围')
    if (s && scopeForm.state === 'ARCHIVED' && s.state !== 'ARCHIVED') await ElMessageBox.confirm('归档前须处理活动子范围、网关分配和角色绑定。归档后此范围不再参与授权。服务器会检查引用。', '归档范围', { type: 'warning', confirmButtonText: '提交归档', cancelButtonText: '返回' })
    busy.value = true; error.value = ''
    const body = s ? { name: scopeForm.name.trim(), state: scopeForm.state } : { name: scopeForm.name.trim(), kind: scopeForm.kind, parentScopeId: scopeForm.parentScopeId }
    selectedScope.value = await api(s ? '/api/v1/resource-scopes/' + enc(s.scopeId) : '/api/v1/resource-scopes', s ? 'PATCH' : 'POST', body, s ? { etag: '"' + s.revision + '"' } : {})
    scopeDialog.value = false; await refreshScopes(); ElMessage.success('范围已保存')
  } catch (e) { if (!cancelled(e)) error.value = errorText(e) } finally { busy.value = false }
}

const principals = reactive(usePaged('/api/v1/principals'))
const principalType = ref('USER'), principalSearch = ref(''), principalDialog = ref(false), principalTab = ref('account')
const selectedPrincipal = ref<Row>()
const principalForm = reactive({ name: '', type: 'USER', state: 'ACTIVE', password: '', roleBindings: [] as Binding[] })
const principalRows = computed(() => principals.items.filter(p => p.type === principalType.value && (!principalSearch.value || p.name.toLowerCase().includes(principalSearch.value.toLowerCase()))))
const issued = ref<Row>(), credentialUncertain = ref(false), issuanceReconciled = ref(false)
const credentialForm = reactive({ label: '', expiresAt: '' })
const credentialRows = ref<Row[]>([]), credentialNext = ref(''), credentialPast = ref<string[]>([]), credentialSnapshot = ref(''), credentialLoading = ref(false)
let credentialCursor = '', credentialGeneration = 0
function clearIssued() { issued.value = undefined }
function resetCredentialView() {
  credentialGeneration++; credentialLoading.value = false; clearIssued(); credentialRows.value = []; credentialNext.value = ''; credentialPast.value = []; credentialSnapshot.value = ''; credentialCursor = ''; credentialUncertain.value = false; issuanceReconciled.value = false
}
function openPrincipal(row?: Row) {
  if (!identityAllowed.value) return
  resetCredentialView(); selectedPrincipal.value = row; principalTab.value = 'account'; error.value = ''
  Object.assign(principalForm, { name: row?.name || '', type: row?.type || principalType.value, state: row?.state || 'ACTIVE', password: '', roleBindings: row ? row.roleBindings.map((b: Binding) => ({ ...b })) : [{ role: 'DEVELOPER', scopeId: activeScopes.value.find(s => s.kind === 'GROUP')?.scopeId || 'scope_instance' }] })
  credentialForm.label = ''; credentialForm.expiresAt = new Date(Date.now() + 30 * 86400000).toISOString(); principalDialog.value = true
}
function setBindingRole(binding: Binding) { if (binding.role === 'ADMIN') binding.scopeId = 'scope_instance' }
function bindingScopes(binding: Binding) { return activeScopes.value.filter(s => binding.role !== 'ADMIN' || s.kind === 'INSTANCE') }
async function savePrincipal() {
  if (!identityAllowed.value || busy.value) return
  try {
    const p = selectedPrincipal.value
    if (!p && !principalForm.name.trim()) throw new Error('请输入账号名称')
    if ((!p && principalForm.type === 'USER') || principalForm.password) if (principalForm.password.length < 12 || principalForm.password.length > 256) throw new Error('人员账号密码需要 12–256 个字符')
    const bindings = principalForm.roleBindings
    if (!bindings.length || bindings.some(b => !bindingScopes(b).some(s => s.scopeId === b.scopeId))) throw new Error('每个角色都需要一个允许绑定的活动范围')
    if (new Set(bindings.map(b => b.role + ':' + b.scopeId)).size !== bindings.length) throw new Error('不能重复绑定相同角色与范围')
    if (p?.principalId === session.principal?.principalId) await ElMessageBox.confirm('修改当前账号后当前登录会话将失效，请确认已记住有效密码并保留管理员访问能力。', '更新当前账号', { type: 'warning', confirmButtonText: '保存并重新登录', cancelButtonText: '返回' })
    busy.value = true; error.value = ''
    const body: Row = p ? { state: principalForm.state, roleBindings: bindings } : { type: principalForm.type, name: principalForm.name.trim(), roleBindings: bindings }
    if (principalForm.type === 'USER' && principalForm.password) body.password = principalForm.password
    const result = await apiResult<Row>(p ? '/api/v1/principals/' + enc(p.principalId) : '/api/v1/principals', p ? 'PATCH' : 'POST', body, p ? { etag: '"' + p.revision + '"' } : {})
    principalForm.password = ''; selectedPrincipal.value = result.data
    if (p?.principalId === session.principal?.principalId) { clearSession(); await router.replace('/login'); return }
    principalDialog.value = false; await principals.refresh(); ElMessage.success('账号与权限已保存')
  } catch (e) { if (!cancelled(e)) error.value = errorText(e) } finally { busy.value = false }
}
async function loadCredentials(cursor = '', navigation: 'refresh' | 'next' | 'previous' = 'refresh') {
  if (!identityAllowed.value || selectedPrincipal.value?.type !== 'APPLICATION') return
  const id = selectedPrincipal.value.principalId, generation = ++credentialGeneration
  credentialLoading.value = true; error.value = ''
  try {
    const p = await api(query('/api/v1/principals/' + enc(id) + '/credentials', { limit: 50, cursor }))
    if (generation !== credentialGeneration || selectedPrincipal.value?.principalId !== id) return
    if (navigation === 'refresh') credentialPast.value = []
    else if (navigation === 'next') credentialPast.value.push(credentialCursor)
    else credentialPast.value.pop()
    credentialCursor = cursor; credentialRows.value = p.items; credentialNext.value = p.nextCursor || ''; credentialSnapshot.value = p.snapshotAt
  } catch (e) { if (generation === credentialGeneration) error.value = errorText(e) } finally { if (generation === credentialGeneration) credentialLoading.value = false }
}
async function issueCredential() {
  if (!identityAllowed.value || selectedPrincipal.value?.type !== 'APPLICATION' || selectedPrincipal.value.state !== 'ACTIVE' || busy.value) return
  const id = selectedPrincipal.value.principalId
  try {
    if (credentialUncertain.value && !issuanceReconciled.value) throw new Error('先刷新凭据元数据，核对上次是否签发成功；必要时撤销不再使用的凭据')
    const expires = new Date(credentialForm.expiresAt).getTime(), now = Date.now()
    if (!Number.isFinite(expires) || expires <= now || expires > now + 366 * 86400000) throw new Error('到期时间必须在未来 366 天内')
    if (credentialUncertain.value) await ElMessageBox.confirm('这会签发另一枚新令牌。上次请求可能已成功，请确认已核对凭据清单。', '再次签发', { type: 'warning', confirmButtonText: '签发新令牌', cancelButtonText: '返回' })
    busy.value = true; error.value = ''; clearIssued()
    const value = await api('/api/v1/principals/' + enc(id) + '/credentials', 'POST', { label: credentialForm.label.trim(), expiresAt: new Date(expires).toISOString() })
    if (pageId.value === 'P20' && principalDialog.value && selectedPrincipal.value?.principalId === id) issued.value = value
    credentialUncertain.value = false; issuanceReconciled.value = false; await loadCredentials()
  } catch (e) {
    if (!cancelled(e)) { error.value = errorText(e); if (e instanceof ApiError && (e.status === 0 || e.status >= 500 || e.code === 'INVALID_RESPONSE')) { credentialUncertain.value = true; issuanceReconciled.value = false } }
  } finally { busy.value = false }
}
async function revokeCredential(row: Row) {
  if (!identityAllowed.value || !selectedPrincipal.value || busy.value) return
  try {
    await ElMessageBox.confirm('撤销后持有此令牌的应用将无法继续认证。', '撤销应用令牌', { type: 'warning', confirmButtonText: '撤销', cancelButtonText: '返回' })
    busy.value = true; error.value = ''
    await api('/api/v1/principals/' + enc(selectedPrincipal.value.principalId) + '/credentials/' + enc(row.credentialId), 'DELETE')
    if (issued.value?.credentialId === row.credentialId) clearIssued()
    await loadCredentials(); ElMessage.success('令牌已撤销')
  } catch (e) { if (!cancelled(e)) error.value = errorText(e) } finally { busy.value = false }
}
watch(principalTab, tab => { if (tab === 'credentials') void loadCredentials() })
watch(principalDialog, open => { if (!open) { principalForm.password = ''; resetCredentialView() } })

const system = ref<Row>(), systemReadAt = ref(''), systemTab = ref('status'), reason = ref(''), operationId = ref(''), operation = ref<Row>()
const operations = reactive(usePaged('/api/v1/operations'))
const operationFilters = { kind: 'DIAGNOSTIC' }
async function loadSystem() {
  if (!systemAllowed.value || pageId.value !== 'P21') return
  loading.value = true; error.value = ''
  try { system.value = await api('/api/v1/system/status'); systemReadAt.value = new Date().toISOString() } catch (e) { error.value = errorText(e) } finally { loading.value = false }
}
async function lookupOperation(id = operationId.value) {
  if (!systemAllowed.value || !can('operation:read') || !id.trim()) return
  busy.value = true; error.value = ''; operation.value = undefined
  try { operation.value = await api('/api/v1/operations/' + enc(id.trim())); operationId.value = id.trim() }
  catch (e) { error.value = errorText(e) } finally { busy.value = false }
}
async function exportDiagnostic() {
  if (!maintainAllowed.value || busy.value) return
  try {
    if (!reason.value.trim()) throw new Error('请填写诊断导出原因')
    busy.value = true; error.value = ''
    operation.value = await api('/api/v1/maintenance/jobs', 'POST', { kind: 'DIAGNOSTIC_EXPORT', reason: reason.value.trim(), includeRawPayload: false })
    operationId.value = operation.value!.operationId; await operations.refresh(operationFilters)
  } catch (e) { error.value = errorText(e) } finally { busy.value = false }
}
const diagnosticPath = computed(() => operation.value?.kind === 'DIAGNOSTIC' && operation.value?.state === 'SUCCEEDED' && maintainAllowed.value ? '/api/v1/operations/' + enc(operation.value.operationId) + '/content' : '')

const audits = reactive(usePaged('/api/v1/audit-events'))
const auditRange = ref<string[]>([new Date(Date.now() - 86400000).toISOString(), new Date().toISOString()])
const auditFilters = reactive({ from: '', to: '' }), auditSearch = ref(''), auditDetail = ref<Row>(), auditDialog = ref(false)
const auditRows = computed(() => audits.items.filter(r => !auditSearch.value || [r.actorId, r.action, r.resourceId, r.traceId].join(' ').toLowerCase().includes(auditSearch.value.toLowerCase())))
async function searchAudit() {
  if (!can('audit:read')) return
  try {
    if (!auditRange.value || auditRange.value.length !== 2) throw new Error('请选择完整的审计时间区间')
    const [from, to] = auditRange.value.map(v => new Date(v).getTime())
    if (!Number.isFinite(from) || !Number.isFinite(to) || from! > to!) throw new Error('审计时间区间无效')
    auditFilters.from = new Date(from!).toISOString(); auditFilters.to = new Date(to!).toISOString(); error.value = ''; await audits.refresh(auditFilters)
  } catch (e) { error.value = errorText(e) }
}
const auditLink = computed(() => {
  const a = auditDetail.value; if (!a) return ''
  if (a.resourceType === 'gateway' && can('gateway:read')) return '/gateways/' + enc(a.resourceId)
  if (a.resourceType === 'scope' && can('scope:read')) return '/scopes?scopeId=' + enc(a.resourceId)
  if (a.resourceType === 'principal' && identityAllowed.value) return '/principals'
  if (a.resourceType === 'task' && can('task:read')) return '/tasks/' + enc(a.resourceId)
  return ''
})

const contractVersion = ref(''), contractOperations = ref(0), guideSection = ref('auth')
const guideSections = [{ id: 'auth', title: '身份与管理 API' }, { id: 'gateway', title: '网关上下行' }, { id: 'customer', title: '客户接收与确认' }, { id: 'gatt', title: 'GATT 与任务' }, { id: 'errors', title: '错误与恢复' }, { id: 'verify', title: '模拟验证' }]
async function loadContract() {
  loading.value = true; error.value = ''
  try {
    const doc = await api('/openapi.json'); contractVersion.value = doc.info.version
    contractOperations.value = Object.values(doc.paths as Record<string, Row>).reduce((n, path) => n + Object.keys(path).filter(k => ['get', 'post', 'put', 'patch', 'delete'].includes(k)).length, 0)
  } catch (e) { error.value = errorText(e) } finally { loading.value = false }
}
watch(() => route.meta.pageId, async () => {
  resetCredentialView(); principalDialog.value = false; scopeDialog.value = false; auditDialog.value = false; error.value = ''; operation.value = undefined
  if (pageId.value === 'P19') await refreshScopes()
  else if (pageId.value === 'P20' && identityAllowed.value) await principals.refresh()
  else if (pageId.value === 'P21' && systemAllowed.value) { await loadSystem(); if (can('operation:read')) await operations.refresh(operationFilters) }
  else if (pageId.value === 'P22' && can('audit:read')) await searchAudit()
  else if (pageId.value === 'P23') await loadContract()
}, { immediate: true })
onBeforeUnmount(() => { resetCredentialView(); principalForm.password = '' })
</script>

<template>
  <div class="system-pages">
    <el-alert v-if="error" :title="error" type="error" :closable="false" />
    <template v-if="pageId === 'P19'">
      <header class="page-heading"><div><h1>项目与分组</h1><p>将网关与角色绑定到清晰的资源范围</p></div><div class="row-actions"><el-button v-if="can('scope:write', 'scope_instance')" type="primary" :disabled="busy || loading" @click="openScope('PROJECT')">新建项目</el-button><el-button v-if="can('scope:write')" :disabled="busy || loading" @click="openScope('GROUP')">新建分组</el-button><el-button :loading="loading" :disabled="busy" @click="refreshScopes">刷新</el-button></div></header>
      <div class="scope-layout">
        <section class="panel"><h3>可见资源树</h3><p class="muted">已加载 {{ session.scopes.length }} 个可见范围；不显示无权读取的父级。</p><el-tree :data="scopeTree" node-key="scopeId" :props="{ label: 'name', children: 'children' }" default-expand-all highlight-current :current-node-key="selectedScope?.scopeId" @node-click="(node: Row) => selectedScope = node"><template #default="{ data }"><span class="scope-node">{{ data.name }} <small>{{ data.kind === 'INSTANCE' ? '实例' : data.kind === 'PROJECT' ? '项目' : '分组' }}{{ data.state === 'ARCHIVED' ? ' · 已归档' : '' }}</small></span></template></el-tree><el-empty v-if="!scopeTree.length && !loading" description="没有可见范围" /></section>
        <section class="panel" v-if="selectedScope"><div class="section-heading"><h2>{{ selectedScope.name }}</h2><el-button v-if="scopeWritable" @click="openScope(selectedScope.kind, true)">修改范围</el-button></div><el-descriptions :column="1" border><el-descriptions-item label="范围 ID"><span class="mono">{{ selectedScope.scopeId }}</span></el-descriptions-item><el-descriptions-item label="类型">{{ selectedScope.kind }}</el-descriptions-item><el-descriptions-item label="父范围">{{ selectedScope.parentScopeId ? scopeLabel(selectedScope.parentScopeId) : '实例根节点' }}</el-descriptions-item><el-descriptions-item label="状态">{{ statusLabel(selectedScope.state) }}</el-descriptions-item><el-descriptions-item label="版本">{{ selectedScope.revision }}</el-descriptions-item></el-descriptions><p class="notice spaced">角色在绑定范围及其活动子范围内生效。项目和分组用于本地资源授权；归档前需要处理网关、子范围和角色绑定。</p><el-button v-if="selectedScope.kind === 'GROUP' && can('gateway:read', selectedScope.scopeId)" @click="router.push({ path: '/gateways', query: { groupId: selectedScope.scopeId } })">查看分组网关</el-button></section>
      </div>
      <el-drawer v-model="scopeDialog" :title="editingScope ? '修改范围' : '新建范围'" size="480px" :close-on-click-modal="false"><el-alert v-if="error" :title="error" type="error" :closable="false" /><el-form label-position="top" @submit.prevent="saveScope"><el-form-item label="类型"><el-select v-model="scopeForm.kind" :disabled="!!editingScope" @change="scopeForm.parentScopeId = scopeParents[0]?.scopeId || ''"><el-option label="项目" value="PROJECT" /><el-option label="分组" value="GROUP" /><el-option v-if="editingScope?.kind === 'INSTANCE'" label="实例" value="INSTANCE" /></el-select></el-form-item><el-form-item v-if="!editingScope" label="父范围"><el-select v-model="scopeForm.parentScopeId" filterable><el-option v-for="s in scopeParents" :key="s.scopeId" :label="s.name + ' · ' + s.scopeId" :value="s.scopeId" /></el-select></el-form-item><el-form-item label="名称"><el-input v-model="scopeForm.name" maxlength="100" show-word-limit /></el-form-item><el-form-item v-if="editingScope && editingScope.kind !== 'INSTANCE'" label="状态"><el-radio-group v-model="scopeForm.state"><el-radio-button value="ACTIVE">活动</el-radio-button><el-radio-button value="ARCHIVED">归档</el-radio-button></el-radio-group></el-form-item><p class="muted">父范围不可修改。版本冲突时保留草稿，请刷新列表并核对当前版本后重新编辑。</p><div class="form-actions"><el-button @click="scopeDialog = false">取消</el-button><el-button type="primary" native-type="submit" :loading="busy">保存范围</el-button></div></el-form></el-drawer>
    </template>

    <template v-else-if="pageId === 'P20'">
      <header class="page-heading"><div><h1>账号与应用凭据</h1><p>人员会话与客户应用分别授权，令牌只在签发时显示</p></div><el-button v-if="identityAllowed" type="primary" @click="openPrincipal()">新建{{ principalType === 'USER' ? '人员账号' : '应用账号' }}</el-button></header>
      <el-alert v-if="!identityAllowed" title="账号及凭据管理需要实例范围 identity:write 权限。" type="warning" :closable="false" />
      <section v-else class="panel"><el-tabs v-model="principalType"><el-tab-pane label="人员账号 · 本批" name="USER" /><el-tab-pane label="应用账号 · 本批" name="APPLICATION" /></el-tabs><div class="toolbar"><el-input v-model="principalSearch" placeholder="本批账号名称筛选" clearable /><span class="muted">类型和名称筛选仅针对当前批次；继续翻页可查看其余账号。</span></div><el-alert v-if="principals.error" :title="principals.error" type="error" :closable="false" /><el-table :data="principalRows" v-loading="principals.loading" stripe><el-table-column label="账号" min-width="190"><template #default="{ row }"><strong>{{ row.name }}</strong><div class="mono muted">{{ row.principalId }}</div></template></el-table-column><el-table-column label="状态" width="105"><template #default="{ row }"><el-tag :type="row.state === 'ACTIVE' ? 'success' : 'info'">{{ statusLabel(row.state) }}</el-tag></template></el-table-column><el-table-column label="角色与范围" min-width="260"><template #default="{ row }"><div v-for="(b, i) in row.roleBindings" :key="i">{{ roleLabel(b.role) }} · {{ scopeLabel(b.scopeId) }}</div></template></el-table-column><el-table-column prop="revision" label="版本" width="70" /><el-table-column label="操作" width="150"><template #default="{ row }"><el-button link type="primary" @click="openPrincipal(row)">管理{{ row.type === 'APPLICATION' ? '与凭据' : '账号' }}</el-button></template></el-table-column></el-table><Pager :count="principals.items.length" :snapshot="principals.snapshotAt" :loading="principals.loading" :has-next="!!principals.nextCursor" :has-previous="principals.past.length > 0" @next="principals.next()" @previous="principals.previous()" @refresh="principals.refresh()" /></section>
      <el-drawer v-model="principalDialog" :title="selectedPrincipal ? selectedPrincipal.name : '新建账号'" size="650px" :close-on-click-modal="false" destroy-on-close>
        <el-alert v-if="error" :title="error" type="error" :closable="false" />
        <el-tabs v-model="principalTab"><el-tab-pane label="账号与授权" name="account"><el-form label-position="top" @submit.prevent="savePrincipal"><el-form-item label="账号类型"><el-radio-group v-model="principalForm.type" :disabled="!!selectedPrincipal" @change="principalForm.password = ''"><el-radio-button value="USER">人员账号</el-radio-button><el-radio-button value="APPLICATION">应用账号</el-radio-button></el-radio-group></el-form-item><el-form-item label="账号名称"><el-input v-model="principalForm.name" :disabled="!!selectedPrincipal" maxlength="100" /></el-form-item><el-form-item v-if="selectedPrincipal" label="状态"><el-radio-group v-model="principalForm.state"><el-radio-button value="ACTIVE">启用</el-radio-button><el-radio-button value="DISABLED">停用</el-radio-button></el-radio-group></el-form-item><el-form-item v-if="principalForm.type === 'USER'" :label="selectedPrincipal ? '新密码（留空保留原密码）' : '初始密码'"><el-input v-model="principalForm.password" type="password" show-password autocomplete="new-password" minlength="12" maxlength="256" /><span class="muted">12–256 个字符；修改账号会使现有人员会话失效。</span></el-form-item><h3>角色绑定</h3><div v-for="(b, i) in principalForm.roleBindings" :key="i" class="binding-row"><el-select v-model="b.role" @change="setBindingRole(b)"><el-option v-for="r in roles" :key="r.value" :label="r.label" :value="r.value" /></el-select><el-select v-model="b.scopeId" filterable placeholder="活动范围"><el-option v-for="s in bindingScopes(b)" :key="s.scopeId" :label="s.name + ' · ' + s.scopeId" :value="s.scopeId" /></el-select><el-button text type="danger" :disabled="principalForm.roleBindings.length <= 1" @click="principalForm.roleBindings.splice(i, 1)">移除</el-button></div><el-button @click="principalForm.roleBindings.push({ role: 'DEVELOPER', scopeId: activeScopes[0]?.scopeId || '' })">添加绑定</el-button><div class="role-guide"><p v-for="r in roles" :key="r.value"><strong>{{ r.label }}</strong> {{ r.description }}</p></div><p v-if="selectedPrincipal" class="muted">当前编辑版本 {{ selectedPrincipal.revision }}。冲突时草稿保留；请刷新列表核对版本后重新打开。系统会保护最后一个活动人员管理员。</p><div class="form-actions"><el-button @click="principalDialog = false">取消</el-button><el-button native-type="submit" type="primary" :loading="busy">保存账号</el-button></div></el-form></el-tab-pane>
          <el-tab-pane v-if="selectedPrincipal?.type === 'APPLICATION'" label="应用凭据" name="credentials"><el-alert title="令牌明文只出现一次。离开页面或关闭面板后即清除；请直接存入客户应用的安全配置。" type="info" :closable="false" />
            <section v-if="issued" class="token-once"><h3>新令牌 · {{ issued.credentialId }}</h3><pre class="code-block token-text">{{ issued.token }}</pre><p class="muted">到期 {{ dateTime(issued.expiresAt) }}</p><el-button @click="copy(issued.token)">复制令牌</el-button><el-button type="primary" @click="clearIssued">已保存，清除明文</el-button></section>
            <el-table :data="credentialRows" v-loading="credentialLoading"><el-table-column label="凭据" min-width="160"><template #default="{ row }">{{ row.label || '未命名' }}<div class="mono muted">{{ row.credentialId }}</div></template></el-table-column><el-table-column label="到期" min-width="160"><template #default="{ row }">{{ dateTime(row.expiresAt) }}</template></el-table-column><el-table-column label="状态" width="85"><template #default="{ row }">{{ statusLabel(row.state) }}</template></el-table-column><el-table-column label="操作" width="75"><template #default="{ row }"><el-button v-if="row.state !== 'REVOKED'" link type="danger" :disabled="busy" @click="revokeCredential(row)">撤销</el-button></template></el-table-column></el-table><Pager :count="credentialRows.length" :snapshot="credentialSnapshot" :loading="credentialLoading" :has-next="!!credentialNext" :has-previous="credentialPast.length > 0" @next="loadCredentials(credentialNext, 'next')" @previous="loadCredentials(credentialPast[credentialPast.length - 1] || '', 'previous')" @refresh="loadCredentials()" />
            <el-divider /><h3>签发新令牌</h3><el-alert v-if="selectedPrincipal?.state !== 'ACTIVE'" title="仅活动应用账号可以签发令牌；请先启用账号。" type="warning" :closable="false" /><el-alert v-if="credentialUncertain" title="上次签发结果不确定，已禁止直接重复提交。请刷新并核对凭据元数据，必要时撤销无法取回明文的凭据。" type="warning" :closable="false" /><el-checkbox v-if="credentialUncertain" v-model="issuanceReconciled">我已核对凭据清单，需要签发另一枚新令牌</el-checkbox><el-form label-position="top" @submit.prevent="issueCredential"><el-form-item label="用途标签"><el-input v-model="credentialForm.label" maxlength="100" /></el-form-item><el-form-item label="到期时间（未来 366 天内）"><el-date-picker v-model="credentialForm.expiresAt" type="datetime" value-format="YYYY-MM-DDTHH:mm:ssZ" placeholder="请选择到期时间" /></el-form-item><el-button native-type="submit" type="primary" :loading="busy" :disabled="selectedPrincipal?.state !== 'ACTIVE' || (!!issued) || (credentialUncertain && !issuanceReconciled)">签发一次性令牌</el-button></el-form>
          </el-tab-pane></el-tabs>
      </el-drawer>
    </template>

    <template v-else-if="pageId === 'P21'">
      <header class="page-heading"><div><h1>系统运维</h1><p>实例就绪、容量与可追溯的维护记录<span v-if="systemReadAt"> · 最近读取 {{ dateTime(systemReadAt) }}</span></p></div><el-button :loading="loading" @click="loadSystem">刷新状态</el-button></header><el-alert v-if="!systemAllowed" title="系统状态需要实例范围 system:read 权限。" type="warning" :closable="false" />
      <template v-else><div v-if="system" class="stats-grid"><div class="stat-card"><span>实例版本</span><strong>{{ system.softwareVersion }}</strong><small>{{ system.instanceId }}</small></div><div class="stat-card"><span>就绪状态</span><strong :class="system.ready ? 'text-success' : 'text-warning'">{{ system.ready ? '就绪' : '待处理' }}</strong><small>进程与存储检查结果</small></div><div class="stat-card"><span>持久数据占用</span><strong>{{ bytes(system.storageBytes) }}</strong><small>配置额度 {{ bytes(system.storageLimitBytes) }}</small></div><div class="stat-card"><span>磁盘可用</span><strong>{{ bytes(system.diskFreeBytes) }}</strong><small>预留 {{ bytes(system.diskReserveBytes) }}</small></div></div><el-tabs v-model="systemTab"><el-tab-pane label="组件与容量" name="status"><div class="two-column"><section class="panel"><h3>组件状态</h3><el-table :data="system?.components || []" v-loading="loading"><el-table-column prop="name" label="组件" /><el-table-column label="状态"><template #default="{ row }"><el-tag :type="row.state === 'READY' ? 'success' : 'warning'">{{ statusLabel(row.state) }}</el-tag></template></el-table-column><el-table-column prop="code" label="代码" /></el-table></section><section class="panel"><h3>数据链路</h3><el-descriptions :column="1" border><el-descriptions-item label="接管/待投递字节">{{ bytes(system?.queueBytes) }}</el-descriptions-item><el-descriptions-item label="最旧待投递年龄">{{ system?.oldestPendingAgeSeconds ?? '—' }} 秒</el-descriptions-item></el-descriptions><p class="muted spaced">这是当前快照。队列增长需要结合客户接收端、应用确认和容量一起排查。</p><el-table :data="system?.dataNodes || []"><el-table-column prop="dataNodeId" label="节点" /><el-table-column prop="activeBundleId" label="活动规则包" /><el-table-column prop="activationRevision" label="激活修订" /></el-table></section></div></el-tab-pane>
        <el-tab-pane label="诊断与作业" name="diagnostics"><div class="two-column"><section class="panel"><h3>导出脱敏诊断</h3><p class="muted">导出实例身份、版本与表记录数量；不包含原始报文或凭据。</p><el-form v-if="maintainAllowed" label-position="top" @submit.prevent="exportDiagnostic"><el-form-item label="导出原因"><el-input v-model="reason" type="textarea" :rows="3" maxlength="300" show-word-limit /></el-form-item><el-button native-type="submit" type="primary" :loading="busy">生成诊断包</el-button></el-form><el-alert v-else title="生成和下载诊断包需要实例 system:maintain 权限。" type="info" :closable="false" /></section><section class="panel"><h3>查询作业</h3><div class="toolbar"><el-input v-model="operationId" placeholder="operationId" @keyup.enter="lookupOperation()" /><el-button :loading="busy" @click="lookupOperation()">查询</el-button></div><template v-if="operation"><el-descriptions :column="1" border><el-descriptions-item label="操作">{{ operation.operationId }}</el-descriptions-item><el-descriptions-item label="类型">{{ operation.kind }}</el-descriptions-item><el-descriptions-item label="状态">{{ statusLabel(operation.state) }}</el-descriptions-item><el-descriptions-item label="创建">{{ dateTime(operation.createdAt) }}</el-descriptions-item></el-descriptions><p v-if="operation.result?.sha256" class="mono spaced">SHA-256: {{ operation.result.sha256 }}</p><a v-if="diagnosticPath" :href="diagnosticPath" download class="download-link">下载已完成诊断包</a><JsonView :value="operation.result || {}" label="持久化结果" /></template></section></div><section class="panel"><h3>诊断历史</h3><el-alert v-if="operations.error" :title="operations.error" type="error" :closable="false" /><el-table :data="operations.items" v-loading="operations.loading"><el-table-column prop="operationId" label="作业" min-width="220" /><el-table-column label="状态" width="120"><template #default="{ row }">{{ statusLabel(row.state) }}</template></el-table-column><el-table-column label="创建时间" min-width="170"><template #default="{ row }">{{ dateTime(row.createdAt) }}</template></el-table-column><el-table-column label="详情" width="80"><template #default="{ row }"><el-button link type="primary" @click="lookupOperation(row.operationId)">查看</el-button></template></el-table-column></el-table><Pager :count="operations.items.length" :snapshot="operations.snapshotAt" :loading="operations.loading" :has-next="!!operations.nextCursor" :has-previous="operations.past.length > 0" @next="operations.next(operationFilters)" @previous="operations.previous(operationFilters)" @refresh="operations.refresh(operationFilters)" /></section></el-tab-pane>
        <el-tab-pane label="停机备份与恢复" name="backup"><article class="panel help-article"><h2>保护两个数据库的一致状态</h2><ol><li>安排维护窗口，停止 manager、pipeline 或单进程服务，确认数据目录无其他运行进程占用。</li><li>使用相同版本 gatewayctl 加密备份，备份文件与独立密钥分开保管。归档包括实例 master.key，不得遗漏。</li><li>恢复到空目录；保留原目录，先验证恢复副本。工具会拒绝损坏备份和覆盖非空目录。</li></ol><pre class="code-block">{{ backupCommands }}</pre><el-button @click="copy(backupCommands)">复制命令模板</el-button><h2>恢复后先完成修订对账</h2><p>出现 <code>RESTORE_RECONCILIATION_REQUIRED</code> 时，重解析会保持锁定。从所有客户接收端收集每个源事件已处理的最高 processingRevision，按事件取最大值合并。下例仅说明格式，不能直接替代真实对账结果。</p><JsonView :value="revisionExample" /><pre class="code-block">{{ reconcileCommand }}</pre><p>命令必须停机运行；只有确认全部客户参与后才能提交 <code>complete:true</code>。不要猜测修订、用空数组代替缺失记录或删除恢复标记。重启后核验组件、队列、网关结果与客户去重行为。</p></article></el-tab-pane>
        <el-tab-pane label="升级与离线排查" name="upgrade"><article class="panel help-article"><h2>受控升级</h2><ol><li>核对发行包签名、版本与数据库兼容性；manager 和 pipeline 使用同一版本。</li><li>停止服务并加密备份，在恢复副本上验证新版本后替换程序或镜像。</li><li>启动并检查组件、规则激活修订、设备结果与客户应用确认。回退前确认旧程序能读取当前数据。</li></ol><p>没有在线跨版本升级或自动回滚按钮。数据库迁移由版本与校验和验证，不能跳过不兼容错误。</p><h2>页面无法连接服务器时</h2><p>在服务器检查进程/容器日志、数据目录挂载、磁盘余量、TLS 证书和系统时钟；再检查 Broker 身份与 ACL、客户目标白名单。安装包的 <code>server/docs/operations.md</code> 包含完整操作指引。</p><p>磁盘或队列达到额度时，网关需要保留待发事件并按退避重传；不要删除数据库或队列来消除告警。</p></article></el-tab-pane></el-tabs></template>
    </template>

    <template v-else-if="pageId === 'P22'">
      <header class="page-heading"><div><h1>审计日志</h1><p>查看已经记录的管理动作及对象变更</p></div></header><section class="panel"><div class="toolbar"><el-date-picker v-model="auditRange" type="datetimerange" start-placeholder="开始时间" end-placeholder="结束时间" range-separator="至" value-format="YYYY-MM-DDTHH:mm:ssZ" /><el-button type="primary" :loading="audits.loading" @click="searchAudit">查询时间区间</el-button><el-input v-model="auditSearch" placeholder="本批执行者 / 动作 / 对象 / traceId" clearable /></div><p class="muted">只查询当前权限可见的审计事件；执行者与动作搜索仅作用于当前批次。审计记录不代表全部 HTTP 请求或失败登录。</p><el-alert v-if="audits.error" :title="audits.error" type="error" :closable="false" /><el-table :data="auditRows" v-loading="audits.loading" stripe @row-click="(row: Row) => { auditDetail = row; auditDialog = true }"><el-table-column label="时间" min-width="165"><template #default="{ row }">{{ dateTime(row.at) }}</template></el-table-column><el-table-column prop="actorId" label="执行者" min-width="165" /><el-table-column prop="action" label="动作" min-width="160" /><el-table-column label="对象" min-width="180"><template #default="{ row }">{{ row.resourceType }}<div class="mono">{{ row.resourceId }}</div></template></el-table-column><el-table-column prop="outcome" label="结果" width="95" /><el-table-column label="追踪" min-width="150"><template #default="{ row }"><el-button link @click.stop="copy(row.traceId)">{{ row.traceId }}</el-button></template></el-table-column></el-table><Pager :count="audits.items.length" :snapshot="audits.snapshotAt" :loading="audits.loading" :has-next="!!audits.nextCursor" :has-previous="audits.past.length > 0" @next="audits.next(auditFilters)" @previous="audits.previous(auditFilters)" @refresh="audits.refresh(auditFilters)" /></section><el-drawer v-model="auditDialog" title="审计详情" size="600px"><template v-if="auditDetail"><el-descriptions :column="1" border><el-descriptions-item label="动作">{{ auditDetail.action }}</el-descriptions-item><el-descriptions-item label="执行者">{{ auditDetail.actorId }}</el-descriptions-item><el-descriptions-item label="时间">{{ dateTime(auditDetail.at) }}</el-descriptions-item><el-descriptions-item label="追踪">{{ auditDetail.traceId }} <el-button link @click="copy(auditDetail.traceId)">复制</el-button></el-descriptions-item></el-descriptions><JsonView :value="auditDetail.changes || {}" label="记录的变更" /><el-button v-if="auditLink" @click="auditDialog = false; router.push(auditLink)">查看关联对象</el-button></template></el-drawer>
    </template>

    <template v-else-if="pageId === 'P23'">
      <header class="page-heading"><div><h1>接口与对接指南</h1><p>API {{ contractVersion || (loading ? '正在读取本机契约' : '契约版本暂不可用') }}<span v-if="contractOperations"> · {{ contractOperations }} 个操作</span> · 网关上行 1.0 / 1.1 · 命令控制 1.0</p></div><a href="/openapi.json" download="ble-gateway-openapi.json" class="download-link">下载本机 OpenAPI</a></header>
      <div class="panel help-layout"><nav aria-label="对接指南目录"><button v-for="s in guideSections" :key="s.id" class="guide-nav" :class="{ selected: guideSection === s.id }" @click="guideSection = s.id">{{ s.title }}</button></nav><article class="help-article">
        <template v-if="guideSection === 'auth'"><h2>为客户应用分配身份</h2><ol><li>管理员创建 APPLICATION 账号，为需要读取或执行的资源绑定固定角色。</li><li>签发有到期时间的应用令牌，直接保存到客户服务的秘密配置中。</li><li>客户服务访问管理 API 时发送 <code>Authorization: Bearer &lt;APPLICATION_TOKEN&gt;</code>；采用 HTTPS 并验证服务器证书。</li></ol><pre class="code-block">GET /api/v1/gateways?limit=50
Authorization: Bearer &lt;APPLICATION_TOKEN&gt;
Accept: application/json</pre><p>分页使用响应中的 nextCursor，保持其他筛选不变。变更请求按契约提供 Idempotency-Key；修改版本化资源还需 If-Match。人员浏览器使用安全会话 Cookie 与 CSRF，不能把应用令牌作为人员登录密码。</p><el-button v-if="identityAllowed" @click="router.push('/principals')">管理应用账号</el-button><h3 class="spaced">资源范围</h3><p>权限同时检查动作和实例/项目/分组范围。系统状态、转发目标、身份等全局资源要求实例授权；DEVELOPER 不具备系统状态权限。</p></template>
        <template v-else-if="guideSection === 'gateway'"><h2>双向可靠传输</h2><div class="flow-strip"><span>网关持久事件</span><i>→</i><span>平台原文持久化</span><i>→</i><span>解析 / 目标 outbox</span><i>→</i><span>客户持久确认</span></div><p>网关必须等待 RAW_PERSISTED 应用确认才能释放事件。MQTT PUBACK 仅确认传输层接收；重复发送保持 namespaceId、gatewayId、eventId 和原文内容不变。</p><p>Topic 前缀为 <code>gw/v1/{namespaceId}/{gatewayId}/</code>，全部业务消息 QoS 1、retain=false。MQTT 用户名为 gatewayId，并在 Broker 配置每设备 ACL。</p><el-table :data="topics"><el-table-column prop="suffix" label="Topic 后缀" min-width="140" /><el-table-column prop="direction" label="方向" width="120" /><el-table-column prop="http" label="HTTP 等价接口" min-width="260" /></el-table><h3 class="spaced">数据与控制分开确认</h3><p>命令先持久任务再派发；设备持久 journal 去重执行，发送带序列的结果。服务器持久结果后返回 RESULT_PERSISTED。结果 UNKNOWN 时查询诊断和后续回执，避免重复物理写入。</p><p>GW32 的 1.1 上行携带 capture 上下文，保留双射频和连接身份；1.0 报文没有 capture 时仍按原契约接入。真实设备身份只通过设备接口使用，管理账号不能冒充设备。</p></template>
        <template v-else-if="guideSection === 'customer'"><h2>客户持久接收与应用 ACK</h2><ol><li>创建版本化 HTTP 或 MQTT 目标，配置允许的目标地址、认证引用及 ACK 模式。</li><li>接收 CustomerBatch，以 deliveryId 建立唯一 inbox 记录并在持久事务中完成去重。</li><li>事务成功后返回 CustomerAck，逐项对应收到的 deliveryId；相同目标版本和 batchId 必须匹配。</li></ol><p>HTTP 仅收到 2xx 不代表可靠应用确认；MQTT PUBACK 同样不够。MQTT 客户数据与 ACK topic 使用目标配置中的 payloadTopic / ackTopic。</p><JsonView :value="customerBatch" label="CustomerBatch 示例" /><el-button @click="copy(pretty(customerBatch))">复制批次示例</el-button><JsonView :value="customerAck" label="CustomerAck 示例" /><el-button @click="copy(pretty(customerAck))">复制确认示例</el-button><p class="spaced">ACCEPTED 表示持久接收，DUPLICATE 表示此前已持久处理，RETRYABLE 请求稍后重试，REJECTED 表示永久拒绝。CUSTOMER_INBOX_PERSISTED 后的后续业务消费由客户负责；只有业务事务已完成才报告 CUSTOMER_BUSINESS_PERSISTED。</p><p>重解析会生成新的 processingRevision/outputId；客户按源事件与最高修订处理更正，并参考 correction、replacesOutputIds，避免把更正重复计入业务。</p><el-button v-if="can('target:read', 'scope_instance')" @click="router.push('/targets')">查看转发目标</el-button></template>
        <template v-else-if="guideSection === 'gatt'"><h2>通过任务控制 GATT</h2><ol><li>查询实际网关能力和操作授权，创建 GATT_CONNECT 任务。</li><li>等待设备确认后取得 connectionRef，查询连接状态并手动提供服务与特征 UUID。</li><li>依次创建 GATT_READ / GATT_WRITE / GATT_SUBSCRIBE 等任务，核查设备结果与验证证据。</li></ol><p>connectionRef 是设备返回的不透明引用。CLOSED / UNKNOWN 引用不能当作活动连接；GW32 写入使用 WITH_RESPONSE 并遵守 MTU−3 和 512 字节限制。任务派发不等于物理写入成功。</p><p>自动连接规则明确指定已登记对端、网关范围、服务 UUID、安全要求与冷却时间。停用规则阻止后续触发，不会取消已经派发的任务。</p><el-button v-if="can('gatt:read')" @click="router.push('/gatt-connections')">连接与任务</el-button></template>
        <template v-else-if="guideSection === 'errors'"><h2>错误与恢复处理</h2><el-table :data="errorGuide"><el-table-column prop="code" label="代码 / 状态" min-width="210" /><el-table-column prop="action" label="处理方式" min-width="300" /></el-table><p class="spaced">保留响应 traceId 关联排查。浏览器不会自动重试没有幂等包装的令牌签发。请求超时不表示操作未发生；先检查任务或持久化结果。</p></template>
        <template v-else><h2>本地模拟验收</h2><ol><li>使用安装包中的 gatewayctl 准备隔离测试目录和模拟网关身份。</li><li>运行网关与客户接收模拟器，依次验证上行持久化、规则解析、目标投递与应用确认。</li><li>注入重复报文、内容冲突、丢 ACK、客户离线、进程重启、过期任务和容量不足场景。</li><li>完成实际网关固件联调后，再验证 BLE 采集边界、Flash 断电恢复、配置读回、固件签名与回滚。</li></ol><p>参考安装包 <code>server/docs/operations.md</code>、<code>server/docs/gateway-protocol.md</code> 与模拟器命令说明。UI 中所有事件和任务来自持久 API；本页不产生设备报文或向任意客户地址发探测请求。</p><p>这份指南和页面资源随软件本地安装，不依赖外网 CDN。实际接口版本以当前下载的 OpenAPI 为准；模拟验证不替代真实硬件验收。</p></template>
      </article></div>
    </template>
  </div>
</template>

<style scoped>
.scope-layout{display:grid;grid-template-columns:minmax(230px,.8fr) minmax(0,1.6fr);gap:20px}.scope-node{display:flex;gap:9px;align-items:center;max-width:100%;font-size:13px}.scope-node small{color:#82929b;font-size:10px}.section-heading{display:flex;justify-content:space-between;align-items:center;gap:12px;margin-bottom:18px}.section-heading h2{margin:0;font-size:20px}.spaced{margin-top:22px}.binding-row{display:grid;grid-template-columns:150px minmax(0,1fr) 50px;gap:8px;margin-bottom:10px}.role-guide{margin-top:20px;padding:14px;background:#f6f8fa;border-radius:6px}.role-guide p{font-size:12px;color:#71848f;margin-bottom:7px}.token-once{background:#eff8f4;border:1px solid #b8dccc;border-radius:7px;padding:16px;margin-bottom:20px}.token-text{user-select:all;word-break:break-all}.download-link{display:inline-flex;align-items:center;justify-content:center;border:1px solid #156d7a;border-radius:6px;background:#fff;padding:8px 15px;font-size:13px;min-height:34px}.guide-nav{border:0;text-align:left;background:transparent;color:#536d79;padding:10px 12px;border-radius:5px;cursor:pointer;font-size:13px}.guide-nav.selected{background:#eaf3f4;color:#156d7a;font-weight:600}.flow-strip{display:flex;align-items:center;flex-wrap:wrap;gap:8px;margin:20px 0}.flow-strip span{border:1px solid #d4e4e8;border-radius:6px;padding:9px 12px;background:#f4f9fa;font-size:12px}.flow-strip i{font-style:normal;color:#6d98a3}.system-pages :deep(.el-form-item .el-select){width:100%}.system-pages :deep(.el-date-editor){max-width:100%}.system-pages :deep(.el-tree-node__content){min-height:34px;height:auto;white-space:normal}.system-pages :deep(.el-tree-node__label){overflow-wrap:anywhere}@media(max-width:900px){.scope-layout{grid-template-columns:1fr}.binding-row{grid-template-columns:1fr}.binding-row .el-button{justify-self:start}.scope-node{flex-wrap:wrap}.help-article{max-width:100%}}
</style>
