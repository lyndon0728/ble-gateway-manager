-- Persisted protocol extensions. Previous migrations remain immutable.
CREATE TABLE gateway_component_generations (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), component TEXT NOT NULL,
 generation TEXT NOT NULL, PRIMARY KEY(gateway_id,component)
);
CREATE TABLE gateway_component_challenges (
 challenge_id TEXT PRIMARY KEY, gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 component TEXT NOT NULL, generation TEXT NOT NULL, nonce TEXT NOT NULL,
 expires_at_ms INTEGER NOT NULL, state TEXT NOT NULL,
 request_hash TEXT, receipt_json TEXT CHECK(receipt_json IS NULL OR json_valid(receipt_json))
);
CREATE TABLE gateway_component_inventory (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), component TEXT NOT NULL,
 inventory_json TEXT NOT NULL CHECK(json_valid(inventory_json)), received_at_ms INTEGER NOT NULL,
 PRIMARY KEY(gateway_id,component)
);
CREATE TABLE gateway_runtime_capabilities (
 gateway_id TEXT PRIMARY KEY REFERENCES gateways(gateway_id), report_json TEXT NOT NULL CHECK(json_valid(report_json)),
 available_json TEXT NOT NULL CHECK(json_valid(available_json)), received_at_ms INTEGER NOT NULL
);
CREATE TABLE gateway_configuration_snapshots (
 gateway_id TEXT PRIMARY KEY REFERENCES gateways(gateway_id), generation TEXT NOT NULL,
 content_hash TEXT NOT NULL, snapshot_json TEXT NOT NULL CHECK(json_valid(snapshot_json))
);
CREATE TABLE gateway_configuration_snapshot_history (
 snapshot_id TEXT PRIMARY KEY, gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id),
 snapshot_json TEXT NOT NULL CHECK(json_valid(snapshot_json))
);
CREATE TABLE peripheral_clients (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), client_id TEXT NOT NULL,
 routes_json TEXT NOT NULL CHECK(json_valid(routes_json)), enabled INTEGER NOT NULL,
 updated_at_ms INTEGER NOT NULL, PRIMARY KEY(gateway_id,client_id)
);
CREATE TABLE peripheral_events (
 namespace_id TEXT NOT NULL, gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), event_id TEXT NOT NULL,
 content_hash TEXT NOT NULL, event_json TEXT NOT NULL CHECK(json_valid(event_json)), received_at_ms INTEGER NOT NULL,
 PRIMARY KEY(namespace_id,gateway_id,event_id)
);
CREATE TABLE peripheral_business (
 namespace_id TEXT NOT NULL, gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), client_id TEXT NOT NULL,
 direction TEXT NOT NULL, business_id TEXT NOT NULL, content_hash TEXT NOT NULL,
 event_id TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'PENDING', result_json TEXT CHECK(result_json IS NULL OR json_valid(result_json)),
 PRIMARY KEY(namespace_id,gateway_id,client_id,direction,business_id)
);
CREATE TABLE peripheral_connections (
 gateway_id TEXT NOT NULL REFERENCES gateways(gateway_id), connection_ref TEXT NOT NULL, radio TEXT NOT NULL,
 client_id TEXT NOT NULL, controller_boot_id TEXT NOT NULL, state TEXT NOT NULL,
 received_at_ms INTEGER NOT NULL, PRIMARY KEY(gateway_id,connection_ref)
);
