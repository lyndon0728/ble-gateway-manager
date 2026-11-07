-- Keep 1.1 capture until the durable control/automation consumer acknowledges it.
CREATE TABLE capture_consumptions (
 event_key TEXT PRIMARY KEY REFERENCES event_receipts(event_key),
 consumed_at_ms INTEGER NOT NULL,
 outcome TEXT NOT NULL,
 error_code TEXT
);
CREATE INDEX ix_capture_peer_time ON raw_events(
 json_extract(metadata_json,'$.rawEvent.capture.address'),
 captured_at_ms
);
