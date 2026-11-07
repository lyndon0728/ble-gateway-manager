// Examples use public protocol identities only; never insert live credentials.
export const roles = [
  { value: 'ADMIN', label: '实例管理员', description: '实例全部权限；只能绑定实例范围。' },
  { value: 'DEVICE_ADMIN', label: '设备管理员', description: '网关、配置、固件及 GATT 管理。' },
  { value: 'OPERATOR', label: '运维操作员', description: '配置、设备诊断、数据与投递状态查询。' },
  { value: 'DEVELOPER', label: '开发者', description: '原文、解析试算、重解析及客户对接查询。' },
  { value: 'AUDITOR', label: '审计员', description: '只读资源与审计记录。' },
]

export const customerBatch = {
  batchId: 'delivery_batch_001',
  events: [{
    deliveryId: 'delivery_demo', outputId: 'output_demo', namespaceId: 'ns_demo',
    gatewayId: 'gw_demo', sourceEventId: 'boot_a:1', processingRevision: 1,
    recordKey: '0', targetId: 'target_demo', targetVersion: '1',
    schemaVersion: 'temperature-1', mappingVersion: 'map-1', mode: 'DECODE',
    correction: false, capturedAt: '2026-11-07T10:00:00Z',
    receivedAt: '2026-11-07T10:00:00Z', timeQuality: 'SYNCED', quality: [],
    data: { temperature: 25, temperatureUnit: 'Cel', humidity: 10, humidityUnit: '%' },
  }],
}
export const customerAck = {
  batchId: 'delivery_batch_001', targetId: 'target_demo', targetVersion: '1',
  durability: 'CUSTOMER_INBOX_PERSISTED',
  items: [{ deliveryId: 'delivery_demo', status: 'ACCEPTED' }],
}
export const topics = [
  { suffix: 'telemetry', direction: '网关 → 平台', http: 'POST /api/v1/gateway-ingress/batches' },
  { suffix: 'telemetry-ack', direction: '平台 → 网关', http: 'batches 响应' },
  { suffix: 'commands', direction: '平台 → 网关', http: 'GET /api/v1/gateway-ingress/commands' },
  { suffix: 'command-results', direction: '网关 → 平台', http: 'POST /api/v1/gateway-ingress/results' },
  { suffix: 'result-ack', direction: '平台 → 网关', http: 'results 响应' },
  { suffix: 'status', direction: '网关 → 平台', http: 'POST /api/v1/gateway-ingress/status' },
]
export const backupCommands = `# 在安装目录操作；先停止所有使用该 data 目录的进程。
docker compose -f deploy/compose.yaml stop manager pipeline
# 原生安装使用同版本 gatewayctl；以下路径需要替换为实际安全目录。
bin/gatewayctl backup --data-dir data --out /secure-backup/backup-001.bgb --key-file /separate-key-store/backup.key
bin/gatewayctl restore --data-dir restored --archive /secure-backup/backup-001.bgb --key-file /separate-key-store/backup.key`
export const reconcileCommand = 'bin/gatewayctl reconcile-revisions --data-dir restored --namespace ns_local --file merged-customer-revisions.json --confirm-complete'
export const revisionExample = {
  namespaceId: 'ns_local', complete: true,
  revisions: [{ gatewayId: 'gw_demo', eventId: 'boot_01-1', processingRevision: 7 }],
}
export const errorGuide = [
  { code: '401 / INVALID_CREDENTIALS', action: '检查应用身份是否启用、令牌是否到期或撤销；人员会话过期后重新登录。' },
  { code: '403 / FORBIDDEN', action: '核对固定角色及实例、项目、分组绑定；有些全局接口明确要求实例授权。' },
  { code: '404 / NOT_FOUND', action: '核对资源身份及可见范围；不可见资源也可能返回 404。' },
  { code: '409 / IDEMPOTENCY_CONFLICT', action: '同一幂等键不能对应不同请求；先查询先前创建的任务或操作。' },
  { code: '412 / 版本冲突', action: '保留草稿，重新获取当前 revision/ETag 后核对差异；不要直接覆盖他人修改。' },
  { code: '422 / QUERY_TOO_BROAD', action: '缩小服务器支持的时间或资源筛选，重新开始游标查询。' },
  { code: '429 / 503', action: '遵守 Retry-After 或退避重试；事件身份和请求幂等键保持稳定。' },
  { code: 'RESTORE_RECONCILIATION_REQUIRED', action: '恢复后的重解析被锁定；停机汇总所有客户接收端最高修订后完成对账。' },
]
