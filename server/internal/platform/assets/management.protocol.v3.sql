CREATE TABLE protocol_parsers (
  parser_id TEXT NOT NULL,
  version TEXT NOT NULL,
  schema_version TEXT NOT NULL,
  scope_id TEXT NOT NULL DEFAULT 'scope_instance' REFERENCES resource_scopes(scope_id),
  spec_json TEXT NOT NULL CHECK(json_valid(spec_json)),
  implementation_digest TEXT NOT NULL,
  compiler_version TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  PRIMARY KEY(parser_id,version)
);
CREATE TRIGGER protocol_parser_immutable BEFORE UPDATE ON protocol_parsers
BEGIN SELECT RAISE(ABORT,'protocol parser versions are immutable'); END;

CREATE TABLE protocol_bindings (
  binding_id TEXT PRIMARY KEY,
  peer_id TEXT NOT NULL REFERENCES ble_peers(peer_id),
  scope_id TEXT NOT NULL REFERENCES resource_scopes(scope_id),
  parser_id TEXT NOT NULL,
  parser_version TEXT NOT NULL,
  schema_version TEXT NOT NULL,
  model_id TEXT NOT NULL DEFAULT '',
  model_version TEXT NOT NULL DEFAULT '',
  address_type TEXT NOT NULL,
  device_address TEXT NOT NULL,
  created_by TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  FOREIGN KEY(parser_id,parser_version) REFERENCES protocol_parsers(parser_id,version)
);
CREATE INDEX protocol_bindings_peer ON protocol_bindings(peer_id,created_at_ms);
CREATE TRIGGER protocol_binding_immutable BEFORE UPDATE ON protocol_bindings
BEGIN SELECT RAISE(ABORT,'protocol bindings are immutable'); END;

CREATE TABLE gatt_discovery_snapshots (
  snapshot_id TEXT PRIMARY KEY,
  namespace_id TEXT NOT NULL,
  gateway_id TEXT NOT NULL,
  radio TEXT NOT NULL,
  connection_ref TEXT NOT NULL,
  address_type TEXT NOT NULL,
  device_address TEXT NOT NULL,
  characteristic_handle INTEGER NOT NULL,
  service_uuid TEXT NOT NULL,
  characteristic_uuid TEXT NOT NULL,
  source_task_id TEXT NOT NULL,
  created_at_ms INTEGER NOT NULL,
  UNIQUE(namespace_id,gateway_id,radio,connection_ref,characteristic_handle,service_uuid,characteristic_uuid)
);
CREATE INDEX gatt_discovery_lookup ON gatt_discovery_snapshots(namespace_id,gateway_id,radio,connection_ref,characteristic_handle);
CREATE TRIGGER gatt_discovery_immutable BEFORE UPDATE ON gatt_discovery_snapshots
BEGIN SELECT RAISE(ABORT,'GATT discovery snapshots are immutable'); END;
