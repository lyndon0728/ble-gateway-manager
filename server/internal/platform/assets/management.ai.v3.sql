CREATE TABLE ai_providers (
 provider_id TEXT PRIMARY KEY, revision INTEGER NOT NULL, config_json TEXT NOT NULL,
 last_check_json TEXT NOT NULL DEFAULT '{}', created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL
);
CREATE TABLE ai_documents (
 document_id TEXT PRIMARY KEY, scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
 owner_id TEXT NOT NULL REFERENCES principals(principal_id), name TEXT NOT NULL, mime_type TEXT NOT NULL,
 sha256 TEXT NOT NULL, size_bytes INTEGER NOT NULL, extraction_state TEXT NOT NULL,
 evidence_json TEXT NOT NULL, warnings_json TEXT NOT NULL, created_at_ms INTEGER NOT NULL
);
CREATE INDEX ai_documents_scope ON ai_documents(scope_id,created_at_ms);
CREATE TABLE ai_jobs (
 job_id TEXT PRIMARY KEY, scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
 owner_id TEXT NOT NULL REFERENCES principals(principal_id), task_kind TEXT NOT NULL,
 document_ids_json TEXT NOT NULL, source_context_json TEXT NOT NULL, provider_id TEXT NOT NULL,
 provider_revision INTEGER NOT NULL DEFAULT 0, state TEXT NOT NULL, phase TEXT NOT NULL,
 draft_id TEXT NOT NULL DEFAULT '', error_code TEXT NOT NULL DEFAULT '', message TEXT NOT NULL DEFAULT '',
 usage_json TEXT NOT NULL DEFAULT '{}', attempts INTEGER NOT NULL DEFAULT 0,
 lease_until_ms INTEGER NOT NULL DEFAULT 0, created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL
);
CREATE INDEX ai_jobs_queue ON ai_jobs(state,created_at_ms);
CREATE INDEX ai_jobs_owner ON ai_jobs(owner_id,created_at_ms);
CREATE TABLE ai_drafts (
 draft_id TEXT PRIMARY KEY, job_id TEXT NOT NULL UNIQUE REFERENCES ai_jobs(job_id),
 scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id), revision INTEGER NOT NULL,
 state TEXT NOT NULL, content_json TEXT NOT NULL, validation_json TEXT NOT NULL DEFAULT '{}',
 published_model_id TEXT NOT NULL DEFAULT '', created_at_ms INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL
);
CREATE TABLE ai_model_versions (
 model_id TEXT PRIMARY KEY, scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
 draft_id TEXT NOT NULL UNIQUE REFERENCES ai_drafts(draft_id), version TEXT NOT NULL,
 model_json TEXT NOT NULL, created_at_ms INTEGER NOT NULL
);
CREATE TABLE ai_apply_plans (
 plan_id TEXT PRIMARY KEY, owner_id TEXT NOT NULL REFERENCES principals(principal_id),
 model_id TEXT NOT NULL REFERENCES ai_model_versions(model_id), plan_json TEXT NOT NULL,
 created_at_ms INTEGER NOT NULL
);
CREATE TABLE ai_usage (
 usage_id TEXT PRIMARY KEY, provider_id TEXT NOT NULL, job_id TEXT NOT NULL,
 actor_id TEXT NOT NULL, state TEXT NOT NULL, usage_json TEXT NOT NULL DEFAULT '{}', result_json TEXT NOT NULL DEFAULT '{}',
 created_at_ms INTEGER NOT NULL
);
CREATE INDEX ai_usage_budget ON ai_usage(provider_id,created_at_ms);
CREATE UNIQUE INDEX ai_usage_dispatch ON ai_usage(provider_id,job_id);
