-- Additive GW32 protocol 1.1 migration. Never modify migration 1.
CREATE TABLE ble_peers (
 peer_id TEXT PRIMARY KEY,
 namespace_id TEXT NOT NULL,
 address_type TEXT NOT NULL CHECK(address_type IN ('PUBLIC','RANDOM')),
 device_address TEXT NOT NULL,
 scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
 display_name TEXT NOT NULL,
 state TEXT NOT NULL CHECK(state IN ('active','disabled')),
 revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
 created_by TEXT NOT NULL REFERENCES principals(principal_id),
 created_at_ms INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL,
 UNIQUE(namespace_id,address_type,device_address)
);
CREATE TABLE auto_connect_rules (
 rule_id TEXT PRIMARY KEY,
 peer_id TEXT NOT NULL REFERENCES ble_peers(peer_id),
 revision INTEGER NOT NULL DEFAULT 1 CHECK(revision>0),
 name TEXT NOT NULL,
 enabled INTEGER NOT NULL CHECK(enabled IN (0,1)),
 priority INTEGER NOT NULL,
 rule_json TEXT NOT NULL CHECK(json_valid(rule_json)),
 created_by TEXT NOT NULL REFERENCES principals(principal_id),
 created_at_ms INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL
);
CREATE INDEX ix_auto_rules_enabled ON auto_connect_rules(enabled,priority DESC,rule_id);
CREATE TABLE auto_connect_triggers (
 trigger_id TEXT PRIMARY KEY,
 namespace_id TEXT NOT NULL,
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 rule_id TEXT NOT NULL REFERENCES auto_connect_rules(rule_id),
 rule_revision INTEGER NOT NULL,
 adv_event_id TEXT NOT NULL,
 uuid_event_id TEXT NOT NULL,
 service_uuid TEXT NOT NULL,
 task_id TEXT NOT NULL REFERENCES command_tasks(task_id),
 created_at_ms INTEGER NOT NULL,
 UNIQUE(namespace_id,gateway_id,rule_id,rule_revision,adv_event_id,service_uuid)
);
CREATE TABLE gatt_connections (
 namespace_id TEXT NOT NULL,
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 connection_ref TEXT NOT NULL,
 radio TEXT NOT NULL CHECK(radio IN ('NRF54','ESP32')),
 address_type TEXT NOT NULL DEFAULT '',
 peer_address TEXT NOT NULL DEFAULT '',
 security_state TEXT NOT NULL DEFAULT '',
 att_mtu INTEGER NOT NULL DEFAULT 0,
 state TEXT NOT NULL CHECK(state IN ('active','closed','unknown')),
 opened_task_id TEXT REFERENCES command_tasks(task_id),
 closed_event_key TEXT,
 details_json TEXT NOT NULL DEFAULT '{}' CHECK(json_valid(details_json)),
 created_at_ms INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL,
 closed_at_ms INTEGER,
 PRIMARY KEY(namespace_id,gateway_id,connection_ref)
);
CREATE INDEX ix_gatt_gateway_state ON gatt_connections(gateway_id,state);
CREATE TABLE gatt_subscriptions (
 namespace_id TEXT NOT NULL,
 gateway_id TEXT NOT NULL,
 connection_ref TEXT NOT NULL,
 service_uuid TEXT NOT NULL,
 characteristic_uuid TEXT NOT NULL,
 characteristic_handle INTEGER NOT NULL,
 mode TEXT NOT NULL CHECK(mode IN ('NOTIFY','INDICATE')),
 state TEXT NOT NULL CHECK(state IN ('active','closed')),
 updated_at_ms INTEGER NOT NULL,
 PRIMARY KEY(namespace_id,gateway_id,connection_ref,service_uuid,characteristic_uuid),
 FOREIGN KEY(namespace_id,gateway_id,connection_ref) REFERENCES gatt_connections(namespace_id,gateway_id,connection_ref)
);
CREATE TABLE connection_claims (
 namespace_id TEXT NOT NULL,
 address_type TEXT NOT NULL,
 peer_address TEXT NOT NULL,
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 radio TEXT NOT NULL CHECK(radio IN ('NRF54','ESP32')),
 task_id TEXT NOT NULL REFERENCES command_tasks(task_id),
 connection_ref TEXT,
 state TEXT NOT NULL CHECK(state IN ('attempting','active','unknown','cooldown')),
 cooldown_until_ms INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL,
 PRIMARY KEY(namespace_id,address_type,peer_address)
);
CREATE INDEX ix_connection_claim_task ON connection_claims(task_id);
CREATE INDEX ix_connection_claim_ref ON connection_claims(gateway_id,connection_ref);
