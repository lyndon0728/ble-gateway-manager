package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
)

type protocolQuery interface {
	QueryRowContext(context.Context, string, ...any) *sql.Row
}

func loadProtocolParser(ctx context.Context, q protocolQuery, id, version string) (pipeMap, error) {
	var spec, schema, digest, compiler, scope string
	e := q.QueryRowContext(ctx, "SELECT spec_json,schema_version,implementation_digest,compiler_version,scope_id FROM protocol_parsers WHERE parser_id=? AND version=?", id, version).Scan(&spec, &schema, &digest, &compiler, &scope)
	if e != nil {
		return nil, e
	}
	s := pm(spec)
	if compiler != ProtocolCompilerVersion || digest != protocolDigest(s) {
		return nil, errors.New("parser runtime or digest mismatch")
	}
	if e = ValidateProtocolSpec(s); e != nil {
		return nil, e
	}
	return pipeMap{"parserId": id, "version": version, "schemaVersion": schema, "spec": s, "implementationDigest": digest, "compilerVersion": compiler, "scopeId": scope}, nil
}
func (a *App) parseRegisteredPayload(ctx context.Context, principal *Principal, id, version string, raw []byte) ([]ParsedRecord, string, error) {
	for _, p := range parserCatalog() {
		if ps(p, "parserId") == id && ps(p, "version") == version {
			records, e := ParsePayload(id, version, raw)
			return records, parserDigest(id, version), e
		}
	}
	p, e := loadProtocolParser(ctx, a.Management, id, version)
	if e != nil || !a.Can(principal, "parser:test", ps(p, "scopeId")) {
		return nil, "", protocolBad("PARSER_UNKNOWN", "published parser version not available")
	}
	records, e := ParseProtocolSpec(po(p, "spec"), raw)
	return records, ps(p, "implementationDigest"), e
}
func (a *App) protocolCatalog(ctx context.Context, principal *Principal) ([]pipeMap, error) {
	rows, e := a.Management.QueryContext(ctx, "SELECT parser_id,version,schema_version,spec_json,implementation_digest,compiler_version,scope_id FROM protocol_parsers ORDER BY parser_id,version LIMIT 10001")
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	collected := []pipeMap{}
	for rows.Next() {
		var id, version, schema, raw, digest, compiler, scope string
		if e = rows.Scan(&id, &version, &schema, &raw, &digest, &compiler, &scope); e != nil {
			return nil, e
		}
		collected = append(collected, pipeMap{"parserId": id, "version": version, "schemaVersion": schema, "implementationDigest": digest, "compilerVersion": compiler, "scopeId": scope, "rawSpec": raw})
	}
	if e = rows.Err(); e != nil {
		return nil, e
	}
	rows.Close()
	if len(collected) > 10000 {
		return nil, errors.New("parser catalog limit exceeded")
	}
	items := []pipeMap{}
	for _, row := range collected {
		if !a.Can(principal, "parser:read", ps(row, "scopeId")) {
			continue
		}
		s := pm(ps(row, "rawSpec"))
		typed, e := decodeProtocolSpec(s)
		if e != nil || ps(row, "compilerVersion") != ProtocolCompilerVersion || protocolDigest(s) != ps(row, "implementationDigest") {
			return nil, errors.New("parser registry integrity failed")
		}
		_, max := protocolLimits(typed)
		if typed.Source != nil {
			max = 1650
		}
		// Parser execution metadata is reusable by device managers, while source
		// excerpts require the same document permission as the import workspace.
		// Verify the original digest above, then redact only this response copy.
		if !a.Can(principal, "document:read", ps(row, "scopeId")) {
			delete(s, "description")
			for _, raw := range pl(s, "fields") {
				field, _ := raw.(map[string]any)
				delete(field, "description")
				delete(field, "evidence")
			}
		}
		delete(row, "rawSpec")
		row["spec"] = s
		row["modelIds"] = []string{}
		row["protocolVersions"] = []string{}
		row["state"] = "INSTALLED"
		row["maxInputBytes"] = max
		row["maxOutputRecords"] = 1
		row["kind"] = "DECLARATIVE"
		items = append(items, row)
	}
	return items, nil
}
func (a *App) RegisterProtocol(r interface {
	Get(string, http.HandlerFunc)
	Post(string, http.HandlerFunc)
}) {
	r.Get("/api/v1/protocol-bindings", a.protocolBindings)
	r.Post("/api/v1/protocol-bindings", a.protocolBindingCreate)
	r.Get("/api/v1/gatt-discovery-snapshots", a.protocolDiscoverySnapshots)
}

func bindingView(id, peer, scope, parser, version, schema, model, modelVersion, addressType, address string, at int64) pipeMap {
	return pipeMap{"bindingId": id, "peerId": peer, "scopeId": scope, "parserId": parser, "parserVersion": version, "schemaVersion": schema, "modelId": model, "modelVersion": modelVersion, "addressType": addressType, "deviceAddress": address, "createdAt": Timestamp(at)}
}
func (a *App) protocolBindings(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "profile:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT b.binding_id,b.peer_id,b.scope_id,b.parser_id,b.parser_version,b.schema_version,b.model_id,b.model_version,b.address_type,b.device_address,b.created_at_ms,p.scope_id FROM protocol_bindings b JOIN protocol_parsers p ON p.parser_id=b.parser_id AND p.version=b.parser_version WHERE (?='' OR b.peer_id=?) ORDER BY b.created_at_ms,b.binding_id LIMIT 10001", r.URL.Query().Get("peerId"), r.URL.Query().Get("peerId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var id, peer, scope, parser, version, schema, model, mv, typ, addr, parserScope string
		var at int64
		if e = rows.Scan(&id, &peer, &scope, &parser, &version, &schema, &model, &mv, &typ, &addr, &at, &parserScope); e != nil {
			RespondError(w, r, e)
			return
		}
		item := bindingView(id, peer, scope, parser, version, schema, model, mv, typ, addr, at)
		item["_parserScope"] = parserScope
		items = append(items, item)
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	rows.Close()
	if len(items) > 10000 {
		Fail(w, r, 422, "QUERY_TOO_BROAD", "filter by peerId")
		return
	}
	visible := []any{}
	for _, v := range items {
		if a.Can(p, "profile:read", ps(v.(pipeMap), "scopeId")) && a.Can(p, "profile:read", ps(v.(pipeMap), "_parserScope")) {
			delete(v.(pipeMap), "_parserScope")
			visible = append(visible, v)
		}
	}
	a.Page(w, r, visible)
}

type protocolBindingRequest struct {
	PeerID        string `json:"peerId"`
	ParserID      string `json:"parserId"`
	ParserVersion string `json:"parserVersion"`
	ModelID       string `json:"modelId,omitempty"`
	ModelVersion  string `json:"modelVersion,omitempty"`
}

func (a *App) protocolBindingCreate(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gatt:manage")
	if !ok {
		return
	}
	var req protocolBindingRequest
	d := json.NewDecoder(http.MaxBytesReader(w, r.Body, 8192))
	d.DisallowUnknownFields()
	if e := d.Decode(&req); e != nil || !protocolResourceID.MatchString(req.PeerID) || !protocolResourceID.MatchString(req.ParserID) || !protocolVersionID.MatchString(req.ParserVersion) || len(req.ModelID) > 128 || len(req.ModelVersion) > 64 {
		Fail(w, r, 422, "INVALID_BINDING", "valid peer and published parser identities required")
		return
	}
	var trailing any
	if d.Decode(&trailing) != io.EOF {
		Fail(w, r, 400, "INVALID_JSON", "one JSON object required")
		return
	}
	// Recheck scope before idempotency replay as well as inside the transaction.
	// A cached response must not survive a caller's resource authorization loss.
	var peerScope, parserScope string
	if e := a.Management.QueryRowContext(r.Context(), "SELECT scope_id FROM ble_peers WHERE peer_id=? AND namespace_id=? AND state='active'", req.PeerID, a.Config.NamespaceID).Scan(&peerScope); e != nil || !a.Can(p, "gatt:manage", peerScope) || !a.Can(p, "profile:read", peerScope) {
		Fail(w, r, 404, "NOT_FOUND", "authorized active peer required")
		return
	}
	if e := a.Management.QueryRowContext(r.Context(), "SELECT scope_id FROM protocol_parsers WHERE parser_id=? AND version=?", req.ParserID, req.ParserVersion).Scan(&parserScope); e != nil || !a.Can(p, "profile:read", parserScope) {
		Fail(w, r, 422, "PARSER_UNKNOWN", "authorized published parser required")
		return
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "protocol-binding.create", r.Header.Get("Idempotency-Key"), req, func(tx *sql.Tx) (int, string, any, error) {
		var scope, typ, addr, state string
		if e := tx.QueryRowContext(r.Context(), "SELECT scope_id,address_type,device_address,state FROM ble_peers WHERE peer_id=? AND namespace_id=?", req.PeerID, a.Config.NamespaceID).Scan(&scope, &typ, &addr, &state); e != nil || state != "active" {
			return 0, "", nil, Problem(404, "NOT_FOUND", "active peer not found")
		}
		if !canQuery(r.Context(), tx, p, "gatt:manage", scope) || !canQuery(r.Context(), tx, p, "profile:read", scope) {
			return 0, "", nil, Problem(403, "FORBIDDEN", "peer binding requires device management and template access")
		}
		parser, e := loadProtocolParser(r.Context(), tx, req.ParserID, req.ParserVersion)
		if e != nil || !canQuery(r.Context(), tx, p, "profile:read", ps(parser, "scopeId")) {
			return 0, "", nil, Problem(422, "PARSER_UNKNOWN", "authorized published parser required")
		}
		id, now := ID("binding"), Now()
		_, e = tx.ExecContext(r.Context(), "INSERT INTO protocol_bindings(binding_id,peer_id,scope_id,parser_id,parser_version,schema_version,model_id,model_version,address_type,device_address,created_by,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)", id, req.PeerID, scope, req.ParserID, req.ParserVersion, ps(parser, "schemaVersion"), req.ModelID, req.ModelVersion, typ, addr, p.ID, now)
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "protocol-binding.create", "scope", scope, pipeMap{"bindingId": id, "peerId": req.PeerID, "parserId": req.ParserID, "parserVersion": req.ParserVersion})
		}
		return 201, id, bindingView(id, req.PeerID, scope, req.ParserID, req.ParserVersion, ps(parser, "schemaVersion"), req.ModelID, req.ModelVersion, typ, addr, now), e
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}

// freezeProtocolDependencies copies reviewed code and static identities into the
// immutable bundle. No worker consults mutable peer registration or AI tables.
func freezeProtocolDependencies(ctx context.Context, tx *sql.Tx, m pipeMap) error {
	parsers, peers, bindings := pipeMap{}, pipeMap{}, pipeMap{}
	for _, rv := range pl(m, "rules") {
		r := rv.(map[string]any)
		match := po(r, "match")
		for _, key := range []string{"peerIds", "protocolBindingIds", "captureKinds", "serviceUuids", "characteristicUuids"} {
			values, exists := match[key]
			if !exists {
				continue
			}
			arr, ok := values.([]any)
			if !ok {
				return fmt.Errorf("%s must be an array", key)
			}
			if len(arr) > 200 {
				return errors.New("protocol match too broad")
			}
			seen := map[string]bool{}
			for _, value := range arr {
				s, ok := value.(string)
				if !ok || s == "" || len(s) > 128 || seen[s] {
					return errors.New("invalid protocol match value")
				}
				seen[s] = true
				if key == "serviceUuids" || key == "characteristicUuids" {
					if gw32CanonicalUUID(s) == "" || s != gw32CanonicalUUID(s) {
						return errors.New("routing UUID must be canonical lowercase 128-bit UUID")
					}
				}
				if key == "captureKinds" && s != "ADV" && s != "SCAN_RSP" && s != "GATT_NOTIFY" && s != "GATT_INDICATE" {
					return errors.New("unsupported capture kind")
				}
			}
		}
		if ps(r, "action") == "DECODE" && !parserKnown(ps(r, "parserId"), ps(r, "parserVersion"), ps(r, "schemaVersion")) {
			if len(pl(match, "peerIds")) == 0 && len(pl(match, "protocolBindingIds")) == 0 {
				return errors.New("custom decoder must explicitly select registered peers or protocol bindings")
			}
			p, e := loadProtocolParser(ctx, tx, ps(r, "parserId"), ps(r, "parserVersion"))
			if e != nil || ps(p, "schemaVersion") != ps(r, "schemaVersion") {
				return errors.New("custom parser/schema dependency unavailable")
			}
			if ps(po(p, "spec"), "inputKind") == "GATT_VALUE" && (len(pl(match, "serviceUuids")) == 0 || len(pl(match, "characteristicUuids")) == 0) {
				return errors.New("GATT value decoder requires explicit service and characteristic UUID matching")
			}
			parsers[ps(r, "parserId")+"@"+ps(r, "parserVersion")] = p
		}
		for _, idv := range pl(match, "protocolBindingIds") {
			id := idv.(string)
			var peer, scope, parser, version, schema, model, mv, typ, addr string
			var at int64
			e := tx.QueryRowContext(ctx, "SELECT peer_id,scope_id,parser_id,parser_version,schema_version,model_id,model_version,address_type,device_address,created_at_ms FROM protocol_bindings WHERE binding_id=?", id).Scan(&peer, &scope, &parser, &version, &schema, &model, &mv, &typ, &addr, &at)
			if e != nil {
				return errors.New("protocol binding not found")
			}
			if ps(r, "action") == "DECODE" && (parser != ps(r, "parserId") || version != ps(r, "parserVersion") || schema != ps(r, "schemaVersion")) {
				return errors.New("binding and decoder version differ")
			}
			bindings[id] = bindingView(id, peer, scope, parser, version, schema, model, mv, typ, addr, at)
		}
		for _, idv := range pl(match, "peerIds") {
			id := idv.(string)
			var scope, typ, addr, state string
			if e := tx.QueryRowContext(ctx, "SELECT scope_id,address_type,device_address,state FROM ble_peers WHERE peer_id=?", id).Scan(&scope, &typ, &addr, &state); e != nil || state != "active" {
				return errors.New("active peer not found")
			}
			peers[id] = pipeMap{"peerId": id, "scopeId": scope, "addressType": typ, "deviceAddress": addr}
		}
	}
	// Validation of an already-compiled bundle must not rewrite its peer identities.
	for key, want := range map[string]pipeMap{"protocolParsers": parsers, "protocolPeers": peers, "protocolBindings": bindings} {
		if existing, ok := m[key]; ok && pipeHash(existing) != pipeHash(want) {
			return errors.New("frozen protocol dependency changed")
		}
		m[key] = want
	}
	return nil
}
func protocolPeerMatches(identity, capture pipeMap) bool {
	return ps(identity, "deviceAddress") != "" && ps(identity, "deviceAddress") == ps(capture, "address") && ps(identity, "addressType") == ps(capture, "addressType")
}
func protocolRuleMatch(match, manifest, meta pipeMap, raw []byte) bool {
	capture := po(po(meta, "rawEvent"), "capture")
	if !pcontains(match["captureKinds"], ps(capture, "kind")) {
		return false
	}
	for _, kind := range []struct{ filter, registry string }{{"peerIds", "protocolPeers"}, {"protocolBindingIds", "protocolBindings"}} {
		if len(pl(match, kind.filter)) == 0 {
			continue
		}
		found := false
		for _, id := range pl(match, kind.filter) {
			if protocolPeerMatches(po(po(manifest, kind.registry), id.(string)), capture) {
				found = true
			}
		}
		if !found {
			return false
		}
	}
	if len(pl(match, "characteristicUuids")) > 0 && !pcontains(match["characteristicUuids"], ps(po(meta, "protocolContext"), "characteristicUuid")) {
		return false
	}
	if len(pl(match, "serviceUuids")) > 0 {
		services := []string{}
		if strings.HasPrefix(ps(capture, "kind"), "GATT_") {
			services = append(services, ps(po(meta, "protocolContext"), "serviceUuid"))
		} else {
			list, e := ExtractServiceUUIDs(raw)
			if e != nil {
				return false
			}
			for _, v := range list {
				services = append(services, v.UUID)
			}
		}
		found := false
		for _, uuid := range services {
			if pcontains(match["serviceUuids"], uuid) {
				found = true
			}
		}
		if !found {
			return false
		}
	}
	return true
}
func parseFrozenProtocol(rule, manifest, meta pipeMap, raw []byte) ([]ParsedRecord, error) {
	id, version := ps(rule, "parserId"), ps(rule, "parserVersion")
	if parserKnown(id, version, ps(rule, "schemaVersion")) {
		return ParsePayload(id, version, raw)
	}
	p := po(po(manifest, "protocolParsers"), id+"@"+version)
	spec := po(p, "spec")
	if len(spec) == 0 || ps(p, "compilerVersion") != ProtocolCompilerVersion || ps(p, "implementationDigest") != protocolDigest(spec) {
		return nil, protocolBad("PARSER_DEPENDENCY_INVALID", "frozen parser unavailable")
	}
	kind := ps(po(po(meta, "rawEvent"), "capture"), "kind")
	if ps(spec, "inputKind") == "BLE_AD" && kind != "ADV" && kind != "SCAN_RSP" {
		return nil, protocolBad("INPUT_KIND_MISMATCH", "expected advertising data")
	}
	if kind == "GATT_NOTIFY" || kind == "GATT_INDICATE" {
		if ps(po(meta, "protocolContext"), "state") != "VERIFIED" {
			return nil, protocolBad("GATT_EVIDENCE_MISSING", "historical connection/handle evidence required")
		}
	}
	if ps(spec, "inputKind") == "GATT_VALUE" {
		if kind != "GATT_NOTIFY" && kind != "GATT_INDICATE" {
			return nil, protocolBad("INPUT_KIND_MISMATCH", "expected GATT value")
		}
		context := po(meta, "protocolContext")
		if ps(context, "state") != "VERIFIED" {
			return nil, protocolBad("GATT_EVIDENCE_MISSING", "historical connection/handle evidence required")
		}
	}
	return ParseProtocolSpec(spec, raw)
}
