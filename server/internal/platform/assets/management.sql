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

-- 单实例身份及版本化的部署配置；密钥仅保存引用。
CREATE TABLE instance_settings (
    instance_id TEXT PRIMARY KEY NOT NULL,
    singleton INTEGER NOT NULL DEFAULT 1 UNIQUE CHECK(singleton=1),
    display_name TEXT NOT NULL,
    deployment_mode TEXT NOT NULL CHECK(deployment_mode IN ('A','B','C')),
    settings_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(settings_json)),
    secret_store_ref TEXT NOT NULL,
    config_version INTEGER NOT NULL DEFAULT 1 CHECK(config_version>0),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

-- 项目/设备组授权与查询范围。一个网关首版归属一个主要设备组。
CREATE TABLE resource_scopes (
    scope_id TEXT PRIMARY KEY NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('instance','project','group')),
    parent_scope_id TEXT REFERENCES resource_scopes(scope_id),
    name TEXT NOT NULL,
    status TEXT NOT NULL DEFAULT 'active' CHECK(status IN ('active','archived')),
    version INTEGER NOT NULL DEFAULT 1,
    created_at_ms INTEGER NOT NULL,
    CHECK(parent_scope_id IS NULL OR parent_scope_id<>scope_id),
    UNIQUE(parent_scope_id,name)
);
CREATE INDEX ix_scopes_parent ON resource_scopes(parent_scope_id);

-- 人工账号和客户应用账号；身份与凭据分离。
CREATE TABLE principals (
    principal_id TEXT PRIMARY KEY NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('human','application','service')),
    name TEXT NOT NULL UNIQUE,
    status TEXT NOT NULL CHECK(status IN ('active','disabled')),
    auth_epoch INTEGER NOT NULL DEFAULT 1 CHECK(auth_epoch>0),
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL
);

-- 自有网关档案、认领状态、能力与最新健康快照。
CREATE TABLE gateways (
    gateway_id TEXT PRIMARY KEY NOT NULL,
    serial_number TEXT NOT NULL UNIQUE,
    display_name TEXT NOT NULL DEFAULT '',
    scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
    model TEXT NOT NULL,
    hardware_version TEXT NOT NULL,
    firmware_version TEXT ,
    shipment_ref TEXT ,
    lifecycle TEXT NOT NULL CHECK(lifecycle IN ('unclaimed','active','disabled','retired')),
    capabilities_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(capabilities_json)),
    last_seen_at_ms INTEGER ,
    health_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(health_json)),
    version INTEGER NOT NULL DEFAULT 1,
    created_at_ms INTEGER NOT NULL
);
CREATE INDEX ix_gateway_scope_status ON gateways(scope_id,lifecycle);

-- 账号/网关凭据的校验材料和密钥引用；不存明文密钥。
CREATE TABLE credentials (
    credential_id TEXT PRIMARY KEY NOT NULL,
    label TEXT NOT NULL DEFAULT '',
    principal_id TEXT REFERENCES principals(principal_id),
    gateway_id TEXT REFERENCES gateways(gateway_id),
    kind TEXT NOT NULL CHECK(kind IN ('password','api_key','session','certificate','device_secret')),
    verifier TEXT NOT NULL,
    key_ref TEXT ,
    status TEXT NOT NULL CHECK(status IN ('active','revoked')),
    not_before_ms INTEGER NOT NULL,
    expires_at_ms INTEGER ,
    created_at_ms INTEGER NOT NULL,
    revoked_at_ms INTEGER ,
    CHECK((principal_id IS NOT NULL) <> (gateway_id IS NOT NULL))
);
CREATE INDEX ix_credential_owner ON credentials(principal_id,status);
CREATE INDEX ix_credential_gateway ON credentials(gateway_id,status);

-- 绑定内置角色与实例/项目/组范围。权限表首版随应用版本冻结。
CREATE TABLE role_bindings (
    binding_id TEXT PRIMARY KEY NOT NULL,
    principal_id TEXT NOT NULL REFERENCES principals(principal_id),
    role TEXT NOT NULL CHECK(role IN ('instance_admin','device_admin','developer','operator','auditor')),
    scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
    created_at_ms INTEGER NOT NULL,
    UNIQUE(principal_id,role,scope_id)
);
CREATE INDEX ix_binding_scope ON role_bindings(scope_id);

-- 期望配置不可变版本；设备回报和应用时间独立。
CREATE TABLE gateway_configs (
    config_id TEXT PRIMARY KEY NOT NULL,
    gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
    revision INTEGER NOT NULL CHECK(revision>0),
    base_revision INTEGER ,
    desired_json TEXT NOT NULL CHECK(json_valid(desired_json)),
    content_hash TEXT NOT NULL,
    reported_json TEXT CHECK(reported_json IS NULL OR json_valid(reported_json)),
    state TEXT NOT NULL CHECK(state IN ('desired','accepted','applied','verified','failed','superseded')),
    created_by TEXT NOT NULL REFERENCES principals(principal_id),
    created_at_ms INTEGER NOT NULL,
    verified_at_ms INTEGER ,
    UNIQUE(gateway_id,revision)
);

-- 离线固件、解析实现、schema、映射与规则包制品登记。
CREATE TABLE artifacts (
    artifact_id TEXT PRIMARY KEY NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('device_manifest','firmware','parser','schema','mapping','bundle','installer')),
    name TEXT NOT NULL,
    version TEXT NOT NULL,
    sha256 TEXT NOT NULL,
    storage_ref TEXT NOT NULL,
    size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),
    signature_ref TEXT NOT NULL,
    compatibility_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(compatibility_json)),
    state TEXT NOT NULL CHECK(state IN ('staged','verified','blocked','retired')),
    created_at_ms INTEGER NOT NULL,
    UNIQUE(kind,name,version)
);

-- 每个网关一个持久下行任务；批量操作由 batch_id 关联。
CREATE TABLE command_tasks (
    task_id TEXT PRIMARY KEY NOT NULL,
    batch_id TEXT ,
    gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
    created_by TEXT NOT NULL REFERENCES principals(principal_id),
    operation TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    request_hash TEXT NOT NULL,
    request_json TEXT NOT NULL CHECK(json_valid(request_json)),
    config_id TEXT REFERENCES gateway_configs(config_id),
    artifact_id TEXT REFERENCES artifacts(artifact_id),
    state TEXT NOT NULL CHECK(state IN ('accepted','queued','sent','device_accepted','executing','applied','verified','succeeded','failed','unknown','expired','cancelled')),
    risk_class TEXT NOT NULL CHECK(risk_class IN ('idempotent','readback','nonrepeatable')),
    expires_at_ms INTEGER NOT NULL,
    version INTEGER NOT NULL DEFAULT 1,
    created_at_ms INTEGER NOT NULL,
    updated_at_ms INTEGER NOT NULL,
    UNIQUE(created_by,operation,idempotency_key),
    UNIQUE(task_id,gateway_id)
);
CREATE INDEX ix_task_gateway_state ON command_tasks(gateway_id,state,created_at_ms);
CREATE INDEX ix_task_batch ON command_tasks(batch_id);

-- 设备回报的持久结果与去重凭证；先提交再确认网关。
CREATE TABLE command_results (
    result_id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL REFERENCES command_tasks(task_id),
    gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
    sequence INTEGER NOT NULL CHECK(sequence>=0),
    stage TEXT NOT NULL CHECK(stage IN ('device_accepted','executing','applied','verified','succeeded','failed','unknown','expired','cancelled')),
    content_hash TEXT NOT NULL,
    result_json TEXT CHECK(result_json IS NULL OR json_valid(result_json)),
    received_at_ms INTEGER NOT NULL,
    dedup_until_ms INTEGER NOT NULL,
    UNIQUE(task_id,sequence),
    FOREIGN KEY(task_id,gateway_id) REFERENCES command_tasks(task_id,gateway_id)
);
CREATE INDEX ix_result_gateway ON command_results(gateway_id,received_at_ms);

-- 可靠发送下行命令或持久结果ACK；与任务/结果同库提交。
CREATE TABLE command_outbox (
    message_id TEXT PRIMARY KEY NOT NULL,
    task_id TEXT NOT NULL REFERENCES command_tasks(task_id),
    result_id TEXT REFERENCES command_results(result_id),
    kind TEXT NOT NULL CHECK(kind IN ('command','result_ack')),
    payload_json TEXT NOT NULL CHECK(json_valid(payload_json)),
    state TEXT NOT NULL CHECK(state IN ('pending','leased','awaiting_receipt','done','expired','cancelled')),
    attempts INTEGER NOT NULL DEFAULT 0 CHECK(attempts>=0),
    next_attempt_at_ms INTEGER NOT NULL,
    lease_owner TEXT ,
    lease_token INTEGER NOT NULL DEFAULT 0 CHECK(lease_token>=0),
    lease_until_ms INTEGER ,
    last_error TEXT ,
    created_at_ms INTEGER NOT NULL,
    CHECK((kind='result_ack' AND result_id IS NOT NULL) OR (kind='command' AND result_id IS NULL))
);
CREATE UNIQUE INDEX ux_command_once ON command_outbox(task_id) WHERE kind='command';
CREATE UNIQUE INDEX ux_result_ack_once ON command_outbox(result_id) WHERE kind='result_ack';
CREATE INDEX ix_command_due ON command_outbox(state,next_attempt_at_ms);

-- 客户目标的稳定身份与配置版本；发布包引用精确版本，修改已有投递须显式迁移。
CREATE TABLE delivery_targets (
    target_id TEXT NOT NULL,
    version TEXT NOT NULL,
    name TEXT NOT NULL,
    scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
    protocol TEXT NOT NULL CHECK(protocol IN ('http','mqtt')),
    config_json TEXT NOT NULL CHECK(json_valid(config_json)),
    secret_ref TEXT ,
    content_hash TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('draft','validated','published','retired')),
    row_version INTEGER NOT NULL DEFAULT 1 CHECK(row_version>0),
    created_by TEXT NOT NULL REFERENCES principals(principal_id),
    created_at_ms INTEGER NOT NULL,
    PRIMARY KEY(target_id,version)
);
CREATE INDEX ix_target_scope ON delivery_targets(scope_id,target_id);

-- 所有需要幂等的管理API写操作的受理记录；与本库资源同事务写入。
CREATE TABLE api_idempotency (
    operation_id TEXT PRIMARY KEY NOT NULL,
    principal_id TEXT NOT NULL REFERENCES principals(principal_id),
    operation TEXT NOT NULL,
    idempotency_key TEXT NOT NULL,
    request_hash TEXT NOT NULL,
    state TEXT NOT NULL CHECK(state IN ('reserved','committed','failed')),
    resource_type TEXT ,
    resource_id TEXT ,
    http_status INTEGER CHECK(http_status IS NULL OR http_status BETWEEN 100 AND 599),
    response_json TEXT CHECK(response_json IS NULL OR json_valid(response_json)),
    created_at_ms INTEGER NOT NULL,
    expires_at_ms INTEGER NOT NULL,
    UNIQUE(principal_id,operation,idempotency_key),
    CHECK(state<>'committed' OR (resource_id IS NOT NULL AND http_status IS NOT NULL))
);
CREATE INDEX ix_api_idem_expiry ON api_idempotency(state,expires_at_ms);

-- 管理端不可变规则包；路由、解析、映射、出口及依赖一并冻结。
CREATE TABLE rule_bundles (
    bundle_id TEXT PRIMARY KEY NOT NULL,
    version INTEGER NOT NULL UNIQUE CHECK(version>0),
    artifact_id TEXT NOT NULL REFERENCES artifacts(artifact_id),
    content_hash TEXT NOT NULL,
    manifest_json TEXT NOT NULL CHECK(json_valid(manifest_json)),
    state TEXT NOT NULL CHECK(state IN ('draft','validated','released','retired')),
    created_by TEXT NOT NULL REFERENCES principals(principal_id),
    created_at_ms INTEGER NOT NULL
);

-- 管理端与数据节点的规则分发/激活握手，替代跨库事务。
CREATE TABLE bundle_deployments (
    deployment_id TEXT PRIMARY KEY NOT NULL,
    bundle_id TEXT NOT NULL REFERENCES rule_bundles(bundle_id),
    node_id TEXT NOT NULL,
    activation_epoch INTEGER NOT NULL CHECK(activation_epoch>0),
    state TEXT NOT NULL CHECK(state IN ('requested','staged','active','failed','superseded')),
    reported_hash TEXT ,
    error_code TEXT ,
    created_at_ms INTEGER NOT NULL,
    acknowledged_at_ms INTEGER ,
    UNIQUE(node_id,activation_epoch)
);
CREATE INDEX ix_deployment_due ON bundle_deployments(state,created_at_ms);

-- 关键动作追加审计，批量和任务操作具有共同关联ID。
CREATE TABLE audit_events (
    audit_id TEXT PRIMARY KEY NOT NULL,
    actor_id TEXT ,
    action TEXT NOT NULL,
    object_type TEXT NOT NULL,
    object_id TEXT NOT NULL,
    outcome TEXT NOT NULL,
    correlation_id TEXT NOT NULL,
    detail_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(detail_json)),
    occurred_at_ms INTEGER NOT NULL,
    retention_until_ms INTEGER NOT NULL
);
CREATE INDEX ix_audit_time ON audit_events(occurred_at_ms);
CREATE INDEX ix_audit_object ON audit_events(object_type,object_id,occurred_at_ms);

-- 备份、恢复、平台升级、诊断导出和出货导入的持久作业。
CREATE TABLE maintenance_jobs (
    job_id TEXT PRIMARY KEY NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN ('backup','restore','upgrade','diagnostic','shipment_import')),
    created_by TEXT NOT NULL REFERENCES principals(principal_id),
    request_json TEXT NOT NULL CHECK(json_valid(request_json)),
    state TEXT NOT NULL CHECK(state IN ('pending','running','succeeded','failed','cancelled')),
    checkpoint_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(checkpoint_json)),
    result_ref TEXT ,
    manifest_hash TEXT ,
    created_at_ms INTEGER NOT NULL,
    finished_at_ms INTEGER 
);
CREATE INDEX ix_maintenance_state ON maintenance_jobs(state,created_at_ms);
COMMIT;
