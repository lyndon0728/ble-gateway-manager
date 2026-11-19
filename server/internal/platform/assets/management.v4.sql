-- Network provisioning secrets and revision reconciliation. Existing migrations are immutable.
CREATE TABLE gateway_network_states (
 gateway_id TEXT PRIMARY KEY REFERENCES gateways(gateway_id),
 state_json TEXT NOT NULL CHECK(json_valid(state_json)),
 state_hash TEXT NOT NULL,
 version INTEGER NOT NULL DEFAULT 1,
 received_at_ms INTEGER NOT NULL
);
CREATE TABLE gateway_network_credentials (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 credential_ref TEXT NOT NULL,
 credential_version TEXT NOT NULL,
 delivery_id TEXT UNIQUE,
 ssid TEXT NOT NULL,
 security TEXT NOT NULL CHECK(security IN ('WPA2_PSK','WPA3_SAE')),
 source TEXT NOT NULL CHECK(source IN ('LOCAL','PLATFORM')),
 state TEXT NOT NULL CHECK(state IN ('PENDING','INSTALLED','FAILED')),
 ciphertext BLOB,
 created_at_ms INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL,
 PRIMARY KEY(gateway_id,credential_ref)
);
CREATE TABLE gateway_network_probes (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 nonce TEXT NOT NULL,
 request_hash TEXT NOT NULL,
 receipt_id TEXT NOT NULL,
 received_at_ms INTEGER NOT NULL,
 PRIMARY KEY(gateway_id,nonce)
);
CREATE TABLE gateway_network_resolutions (
 resolution_token TEXT PRIMARY KEY,
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 generation TEXT NOT NULL,
 snapshot_id TEXT NOT NULL,
 candidate_hash TEXT NOT NULL,
 task_id TEXT UNIQUE REFERENCES command_tasks(task_id),
 created_at_ms INTEGER NOT NULL
);
