-- A processing revision freezes its chosen capture evidence separately from raw data.
ALTER TABLE processing_jobs ADD COLUMN protocol_context_json TEXT CHECK(protocol_context_json IS NULL OR json_valid(protocol_context_json));
