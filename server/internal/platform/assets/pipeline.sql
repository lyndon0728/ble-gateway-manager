-- V1.7 design baseline; generated schema, not a production migration runner.
-- UTC timestamps: Unix milliseconds. Secrets live outside this database.
PRAGMA foreign_keys=ON;
PRAGMA journal_mode=WAL;
PRAGMA synchronous=FULL;
PRAGMA busy_timeout=5000;
BEGIN IMMEDIATE;

-- 记录已执行的顺序迁移及不可变校验值。
CREATE TABLE schema_migrations (
    version INTEGER PRIMARY KEY CHECK(version > 0),
    checksum TEXT NOT NULL,
    applied_at_ms INTEGER NOT NULL,
    app_version TEXT NOT NULL
);

-- 数据节点持久复制的完整规则快照，不依赖读取管理库执行任务。
CREATE TABLE pipeline_bundles (
    bundle_id TEXT PRIMARY KEY NOT NULL,
    namespace_id TEXT NOT NULL,
    activation_epoch INTEGER NOT NULL CHECK(activation_epoch>0),
    content_hash TEXT NOT NULL,
    manifest_json TEXT NOT NULL CHECK(json_valid(manifest_json)),
    state TEXT NOT NULL CHECK(state IN ('staged','active','retired','blocked')),
    stored_at_ms INTEGER NOT NULL,
    activated_at_ms INTEGER ,
    UNIQUE(namespace_id,activation_epoch)
);
CREATE UNIQUE INDEX ux_active_bundle ON pipeline_bundles(namespace_id) WHERE state='active';

-- 事件身份、内容校验及接管凭证；载荷清理后仍可识别原ID重传。
CREATE TABLE event_receipts (
    event_key TEXT PRIMARY KEY NOT NULL,
    namespace_id TEXT NOT NULL,
    gateway_id TEXT NOT NULL,
    event_id TEXT NOT NULL,
    immutable_hash TEXT NOT NULL,
    bundle_id TEXT NOT NULL REFERENCES pipeline_bundles(bundle_id),
    accepted_at_ms INTEGER NOT NULL,
    latest_revision INTEGER NOT NULL DEFAULT 1 CHECK(latest_revision>0),
    custody_state TEXT NOT NULL CHECK(custody_state IN ('accepted_raw','processing','quarantined','processed','delivered','terminated')),
    dedup_until_ms INTEGER NOT NULL,
    raw_retention_until_ms INTEGER NOT NULL,
    raw_purged_at_ms INTEGER ,
    UNIQUE(namespace_id,gateway_id,event_id)
);
CREATE INDEX ix_receipt_state_time ON event_receipts(custody_state,accepted_at_ms);
CREATE INDEX ix_receipt_retention ON event_receipts(dedup_until_ms);

-- 事件原始字节与采集元数据，可在满足回收条件后单独删除。
CREATE TABLE raw_events (
    event_key TEXT PRIMARY KEY NOT NULL REFERENCES event_receipts(event_key),
    payload BLOB NOT NULL,
    payload_bytes INTEGER NOT NULL CHECK(payload_bytes>=0),
    source_device_id TEXT ,
    captured_at_ms INTEGER ,
    time_quality TEXT NOT NULL CHECK(time_quality IN ('trusted','estimated','unknown')),
    protocol_hint TEXT ,
    metadata_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(metadata_json)),
    CHECK(length(payload)=payload_bytes)
);

-- 持久重解析作业；范围先预览和冻结，按稳定游标逐批产生新修订。
CREATE TABLE reprocess_jobs (
    reprocess_id TEXT PRIMARY KEY NOT NULL,
    bundle_id TEXT NOT NULL REFERENCES pipeline_bundles(bundle_id),
    requested_by TEXT NOT NULL,
    idempotency_key TEXT NOT NULL UNIQUE,
    request_hash TEXT NOT NULL,
    selection_json TEXT NOT NULL CHECK(json_valid(selection_json)),
    cursor_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(cursor_json)),
    reason TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('preview','pending','running','paused','completed','failed','cancelled')),
    created_at_ms INTEGER NOT NULL
);

-- 每事件每修订一个持久处理任务；隔离也是可查询状态。
CREATE TABLE processing_jobs (
    processing_id TEXT PRIMARY KEY NOT NULL,
    event_key TEXT NOT NULL REFERENCES event_receipts(event_key),
    revision INTEGER NOT NULL CHECK(revision>0),
    bundle_id TEXT NOT NULL REFERENCES pipeline_bundles(bundle_id),
    reprocess_id TEXT REFERENCES reprocess_jobs(reprocess_id),
    mode TEXT CHECK(mode IS NULL OR mode IN ('passthrough','decode','quarantine')),
    plan_json TEXT CHECK(plan_json IS NULL OR json_valid(plan_json)),
    state TEXT NOT NULL CHECK(state IN ('pending','leased','retry_wait','completed','quarantined','filtered','terminated')),
    attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts>=0),
    next_attempt_at_ms INTEGER NOT NULL,
    lease_owner TEXT ,
    lease_token INTEGER NOT NULL DEFAULT 0 CHECK(lease_token>=0),
    lease_until_ms INTEGER ,
    error_code TEXT ,
    error_detail_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(error_detail_json)),
    output_count INTEGER NOT NULL DEFAULT 0 CHECK(output_count>=0),
    created_at_ms INTEGER NOT NULL,
    completed_at_ms INTEGER ,
    UNIQUE(event_key,revision),
    UNIQUE(event_key,reprocess_id)
);
CREATE INDEX ix_processing_due ON processing_jobs(state,next_attempt_at_ms);
CREATE INDEX ix_processing_error ON processing_jobs(state,error_code,created_at_ms);

-- 标准化/透传结果；保存原始来源、版本和稳定子记录身份。
CREATE TABLE processing_results (
    output_id TEXT PRIMARY KEY NOT NULL,
    processing_id TEXT NOT NULL REFERENCES processing_jobs(processing_id),
    record_key TEXT NOT NULL,
    parser_id TEXT ,
    parser_version TEXT ,
    schema_version TEXT NOT NULL,
    result_json TEXT CHECK(result_json IS NULL OR json_valid(result_json)),
    content_hash TEXT NOT NULL,
    quality_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(quality_json)),
    created_at_ms INTEGER NOT NULL,
    purged_at_ms INTEGER ,
    UNIQUE(processing_id,record_key)
);

-- 逐目标持久待发项和客户接收凭证；一个目标确认只推进自己的状态。
CREATE TABLE delivery_outbox (
    delivery_id TEXT PRIMARY KEY NOT NULL,
    output_id TEXT NOT NULL REFERENCES processing_results(output_id),
    target_id TEXT NOT NULL,
    target_config_version TEXT NOT NULL,
    mapping_version TEXT NOT NULL,
    required INTEGER NOT NULL CHECK(required IN (0,1)),
    protocol TEXT NOT NULL CHECK(protocol IN ('http','mqtt')),
    destination_json TEXT NOT NULL CHECK(json_valid(destination_json)),
    wire_payload BLOB ,
    content_hash TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('pending','leased','awaiting_ack','retry_wait','delivered','terminated')),
    attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts>=0),
    next_attempt_at_ms INTEGER NOT NULL,
    lease_owner TEXT ,
    lease_token INTEGER NOT NULL DEFAULT 0 CHECK(lease_token>=0),
    lease_until_ms INTEGER ,
    last_error TEXT ,
    ack_json TEXT CHECK(ack_json IS NULL OR json_valid(ack_json)),
    delivered_at_ms INTEGER ,
    terminated_reason TEXT ,
    payload_purged_at_ms INTEGER ,
    created_at_ms INTEGER NOT NULL,
    UNIQUE(output_id,target_id,target_config_version,mapping_version),
    CHECK(state<>'delivered' OR (ack_json IS NOT NULL AND delivered_at_ms IS NOT NULL)),
    CHECK(state<>'terminated' OR terminated_reason IS NOT NULL),
    CHECK(state IN ('delivered','terminated') OR wire_payload IS NOT NULL)
);
CREATE INDEX ix_delivery_due ON delivery_outbox(target_id,state,next_attempt_at_ms);
CREATE INDEX ix_delivery_lease ON delivery_outbox(state,lease_until_ms);
-- Durable transport batch membership and per-item fencing; PUBACK is never custody.
CREATE TABLE delivery_batches (
    batch_id TEXT PRIMARY KEY NOT NULL,
    target_id TEXT NOT NULL,
    target_version TEXT NOT NULL,
    delivery_ids_json TEXT NOT NULL CHECK(json_valid(delivery_ids_json)),
    created_at_ms INTEGER NOT NULL,
    expires_at_ms INTEGER NOT NULL
);
CREATE INDEX ix_delivery_batch_expiry ON delivery_batches(expires_at_ms);
COMMIT;
