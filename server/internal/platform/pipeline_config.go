package platform

import (
	"context"
	"database/sql"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/go-chi/chi/v5"
)

func pipePage(items any, next string) pipeMap {
	m := pipeMap{"items": items, "snapshotAt": pts(pnow())}
	if next != "" {
		m["nextCursor"] = next
	}
	return m
}
func pipeLimit(r *http.Request) int {
	n, _ := strconv.Atoi(r.URL.Query().Get("limit"))
	if n <= 0 {
		n = 50
	}
	if n > 200 {
		n = 200
	}
	return n
}
func parserDigest(id, version string) string {
	// Parser identity is immutable across server releases. Preserve the 0.1.0
	// release digest for installed v1.0 implementations and version new behavior.
	implementation := "0.1.0"
	if id == "ble_ad" && version == "1.1.0" {
		implementation = "ble-ad-service-uuid-1"
	}
	return pipeHash([]string{"builtin", implementation, id, version})
}
func (a *App) pipelineParsers(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.Require(w, r, "parser:read"); !ok {
		return
	}
	items := []pipeMap{}
	for _, p := range parserCatalog() {
		schema := p["schemaVersions"].([]string)[0]
		maxBytes := 65536
		if ps(p, "parserId") == "ble_ad" && ps(p, "version") == "1.1.0" {
			maxBytes = 1650
		}
		items = append(items, pipeMap{"parserId": p["parserId"], "version": p["version"], "schemaVersion": schema, "modelIds": []string{}, "protocolVersions": []string{}, "implementationDigest": parserDigest(ps(p, "parserId"), ps(p, "version")), "state": "INSTALLED", "maxInputBytes": maxBytes, "maxOutputRecords": 1})
	}
	pjson(w, 200, pipePage(items, ""))
}
func (a *App) pipelineParserTest(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.Require(w, r, "parser:test"); !ok {
		return
	}
	req, ok := a.Decode(w, r, "ParserTest")
	if !ok {
		return
	}
	id, ver := chi.URLParam(r, "parserId"), chi.URLParam(r, "parserVersion")
	start := time.Now()
	raw, err := base64.StdEncoding.Strict().DecodeString(ps(req, "payloadBase64"))
	var records []ParsedRecord
	if err == nil {
		records, err = ParsePayload(id, ver, raw)
		annotateServiceUUIDSource(records, ps(po(req, "metadata"), "sourceEventId"))
	}
	outputs := []pipeMap{}
	errs := []pipeMap{}
	for _, record := range records {
		outputs = append(outputs, record.Data)
	}
	if err != nil {
		code := "INVALID_BASE64"
		var pf *ParserFailure
		if errors.As(err, &pf) {
			code = pf.Code
		}
		errs = append(errs, pipeMap{"code": code, "message": err.Error(), "retryable": false, "traceId": pipeID("trace")})
	}
	if expected, exists := req["expected"]; exists && err == nil && pipeHash(expected) != pipeHash(outputs) {
		errs = append(errs, pipeMap{"code": "EXPECTED_OUTPUT_MISMATCH", "message": "actual output does not equal expected output", "retryable": false, "traceId": pipeID("trace")})
	}
	pjson(w, 200, pipeMap{"passed": len(errs) == 0, "outputs": outputs, "errors": errs, "cpuTimeMicros": time.Since(start).Microseconds(), "implementationDigest": parserDigest(id, ver)})
}

func (a *App) pipelineSecret(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "secret:write")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "SecretCreate")
	if !ok {
		return
	}
	a.pipeMutation(w, r, p, "secret:create", req, func(tx *sql.Tx) (int, pipeMap, error) {
		ref, err := a.SecretPut(ps(req, "value"))
		return 201, pipeMap{"secretRef": ref, "kind": req["kind"], "createdAt": pts(pnow())}, err
	})
}

// Target destinations are administrator-owned and must be allowlisted in production.
// DNS is checked again by the delivery dialer so rebinding cannot reach metadata endpoints.
func (a *App) validateTarget(ctx context.Context, c pipeMap) error {
	u, err := url.Parse(ps(c, "endpoint"))
	if err != nil || u.Hostname() == "" || u.User != nil || u.Fragment != "" {
		return errors.New("endpoint requires an absolute URL without credentials or fragment")
	}
	switch ps(c, "protocol") {
	case "HTTP":
		if u.Scheme != "https" && !(a.Config.AllowInsecureDev && u.Scheme == "http") {
			return errors.New("HTTP delivery requires https")
		}
		if ps(c, "ackMode") != "HTTP_DURABLE_RESPONSE" {
			return errors.New("HTTP target requires durable-response ACK")
		}
	case "MQTT":
		if u.Scheme != "mqtts" && u.Scheme != "ssl" && u.Scheme != "tls" && !(a.Config.AllowInsecureDev && (u.Scheme == "mqtt" || u.Scheme == "tcp")) {
			return errors.New("MQTT delivery requires a TLS broker URL")
		}
		if ps(c, "ackMode") != "MQTT_APPLICATION_ACK" {
			return errors.New("MQTT requires application ACK")
		}
		if u.Path != "" && u.Path != "/" {
			return errors.New("MQTT broker endpoint cannot include path")
		}
		for _, k := range []string{"payloadTopic", "ackTopic"} {
			topic := ps(c, k)
			if topic == "" || len(topic) > 65535 || strings.ContainsAny(topic, "+#\x00") {
				return errors.New("MQTT topics must be exact, nonempty and without wildcards")
			}
		}
		if ps(c, "payloadTopic") == ps(c, "ackTopic") {
			return errors.New("payload and ACK topics must differ")
		}
	default:
		return errors.New("unsupported target protocol")
	}
	if err := ValidateOutputMapping(c); err != nil {
		return err
	}
	if len(a.Config.AllowedTargetHosts) == 0 && !a.Config.AllowInsecureDev {
		return errors.New("configure GW_ALLOWED_TARGET_HOSTS before publishing a target")
	}
	if len(a.Config.AllowedTargetHosts) > 0 {
		allowed := false
		for _, host := range a.Config.AllowedTargetHosts {
			if strings.EqualFold(strings.TrimSpace(host), u.Hostname()) {
				allowed = true
			}
		}
		if !allowed {
			return errors.New("target hostname is not allowlisted")
		}
	}
	if ip := net.ParseIP(u.Hostname()); ip != nil {
		if err = a.checkTargetIP(ip); err != nil {
			return err
		}
	}
	for _, k := range []string{"secretRef", "tlsTrustRef"} {
		if ref := ps(c, k); ref != "" {
			if _, err = a.SecretGet(ref); err != nil {
				return fmt.Errorf("%s does not resolve", k)
			}
		}
	}
	return nil
}
func (a *App) checkTargetIP(ip net.IP) error {
	if ip.IsUnspecified() || ip.IsMulticast() || ip.IsLinkLocalUnicast() || ip.IsLinkLocalMulticast() {
		return errors.New("unsafe target address")
	}
	if ip.IsLoopback() && !a.Config.AllowInsecureDev {
		return errors.New("loopback targets are development-only")
	}
	return nil
}
func (a *App) targetView(ctx context.Context, id string) (pipeMap, error) {
	rows, err := a.Management.QueryContext(ctx, "SELECT name,version,config_json,content_hash FROM delivery_targets WHERE target_id=? ORDER BY CAST(version AS INTEGER)", id)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	versions := []pipeMap{}
	name := ""
	for rows.Next() {
		var version, raw, digest string
		if err = rows.Scan(&name, &version, &raw, &digest); err != nil {
			return nil, err
		}
		versions = append(versions, pipeMap{"version": version, "configuration": pm(raw), "contentDigest": digest})
	}
	if err = rows.Err(); err != nil {
		return nil, err
	}
	if len(versions) == 0 {
		return nil, sql.ErrNoRows
	}
	return pipeMap{"targetId": id, "name": name, "versions": versions}, nil
}
func (a *App) pipelineTargetGet(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.pipeGlobal(w, r, "target:read"); !ok {
		return
	}
	v, err := a.targetView(r.Context(), chi.URLParam(r, "targetId"))
	if err != nil {
		pfail(w, r, 404, "NOT_FOUND", "target not found")
		return
	}
	pjson(w, 200, v)
}
func (a *App) pipelineTargets(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "target:read")
	if !ok {
		return
	}
	c, ok := a.readPipeCursor(w, r, p, "targets", "delivery_targets", a.Management)
	if !ok {
		return
	}
	rows, err := a.Management.QueryContext(r.Context(), "SELECT DISTINCT target_id FROM delivery_targets WHERE target_id>? AND rowid<=? ORDER BY target_id LIMIT ?", c.After, c.Upper, pipeLimit(r)+1)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "targets unavailable")
		return
	}
	ids := []string{}
	for rows.Next() {
		var id string
		_ = rows.Scan(&id)
		ids = append(ids, id)
	}
	rows.Close()
	next := ""
	if len(ids) > pipeLimit(r) {
		ids = ids[:pipeLimit(r)]
		next = ids[len(ids)-1]
	}
	items := []pipeMap{}
	for _, id := range ids {
		v, e := a.targetView(r.Context(), id)
		if e == nil {
			items = append(items, v)
		}
	}
	pjson(w, 200, a.cursorPage(items, next, c))
}
func (a *App) pipelineTargetCreate(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "target:write")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "TargetCreate")
	if !ok {
		return
	}
	config := po(req, "configuration")
	if err := a.validateTarget(r.Context(), config); err != nil {
		pfail(w, r, 422, "INVALID_TARGET", err.Error())
		return
	}
	a.pipeMutation(w, r, p, "target:create", req, func(tx *sql.Tx) (int, pipeMap, error) {
		var scope string
		if err := tx.QueryRowContext(r.Context(), "SELECT scope_id FROM resource_scopes WHERE kind='instance' LIMIT 1").Scan(&scope); err != nil {
			return 0, nil, err
		}
		id := pipeID("target")
		digest := pipeHash(config)
		_, err := tx.ExecContext(r.Context(), "INSERT INTO delivery_targets(target_id,version,name,scope_id,protocol,config_json,secret_ref,content_hash,state,created_by,created_at_ms) VALUES(?,'1',?,?,?,?,?,?,'published',?,?)", id, ps(req, "name"), scope, strings.ToLower(ps(config, "protocol")), pipeJSON(config), ps(config, "secretRef"), digest, p.ID, pnow())
		return 201, pipeMap{"targetId": id, "name": req["name"], "versions": []pipeMap{{"version": "1", "configuration": config, "contentDigest": digest}}}, err
	})
}
func (a *App) pipelineTargetVersion(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "target:write")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "TargetVersionCreate")
	if !ok {
		return
	}
	if err := a.validateTarget(r.Context(), req); err != nil {
		pfail(w, r, 422, "INVALID_TARGET", err.Error())
		return
	}
	id := chi.URLParam(r, "targetId")
	a.pipeMutation(w, r, p, "target:version:"+id, req, func(tx *sql.Tx) (int, pipeMap, error) {
		var name, scope string
		var version int
		if err := tx.QueryRowContext(r.Context(), "SELECT name,scope_id,MAX(CAST(version AS INTEGER)) FROM delivery_targets WHERE target_id=? GROUP BY target_id", id).Scan(&name, &scope, &version); err != nil {
			return 0, nil, err
		}
		v := strconv.Itoa(version + 1)
		hash := pipeHash(req)
		_, err := tx.ExecContext(r.Context(), "INSERT INTO delivery_targets(target_id,version,name,scope_id,protocol,config_json,secret_ref,content_hash,state,created_by,created_at_ms) VALUES(?,?,?,?,?,?,?,?,'published',?,?)", id, v, name, scope, strings.ToLower(ps(req, "protocol")), pipeJSON(req), ps(req, "secretRef"), hash, p.ID, pnow())
		return 201, pipeMap{"targetId": id, "name": name, "versions": []pipeMap{{"version": v, "configuration": req, "contentDigest": hash}}}, err
	})
}

func bundleView(id, hash, state, manifest string, created int64) pipeMap {
	m := pm(manifest)
	deps := []string{}
	for _, v := range po(m, "targets") {
		deps = append(deps, pipeHash(v))
	}
	sort.Strings(deps)
	return pipeMap{"bundleId": id, "contentDigest": hash, "state": strings.ToUpper(state), "rules": pl(m, "rules"), "defaultAction": "QUARANTINE", "createdAt": pts(created), "dependencyDigests": deps}
}
func (a *App) pipelineBundleGet(w http.ResponseWriter, r *http.Request) {
	if _, ok := a.pipeGlobal(w, r, "rule:read"); !ok {
		return
	}
	id := chi.URLParam(r, "bundleId")
	var hash, state, manifest string
	var created int64
	if err := a.Management.QueryRowContext(r.Context(), "SELECT content_hash,state,manifest_json,created_at_ms FROM rule_bundles WHERE bundle_id=?", id).Scan(&hash, &state, &manifest, &created); err != nil {
		pfail(w, r, 404, "NOT_FOUND", "bundle not found")
		return
	}
	pjson(w, 200, bundleView(id, hash, state, manifest, created))
}
func (a *App) pipelineBundles(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "rule:read")
	if !ok {
		return
	}
	c, ok := a.readPipeCursor(w, r, p, "bundles", "rule_bundles", a.Management)
	if !ok {
		return
	}
	rows, err := a.Management.QueryContext(r.Context(), "SELECT bundle_id,content_hash,state,manifest_json,created_at_ms FROM rule_bundles WHERE bundle_id>? AND rowid<=? ORDER BY bundle_id LIMIT ?", c.After, c.Upper, pipeLimit(r)+1)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "bundles unavailable")
		return
	}
	defer rows.Close()
	items := []pipeMap{}
	for rows.Next() {
		var id, hash, state, manifest string
		var created int64
		if err = rows.Scan(&id, &hash, &state, &manifest, &created); err != nil {
			break
		}
		items = append(items, bundleView(id, hash, state, manifest, created))
	}
	next := ""
	if len(items) > pipeLimit(r) {
		items = items[:pipeLimit(r)]
		next = ps(items[len(items)-1], "bundleId")
	}
	pjson(w, 200, a.cursorPage(items, next, c))
}
func pipeMatchOverlap(x, y pipeMap) bool {
	for _, k := range []string{"modelIds", "groupIds", "protocolVersions", "packetTypes"} {
		a, b := pl(x, k), pl(y, k)
		if len(a) == 0 || len(b) == 0 {
			continue
		}
		overlap := false
		for _, v := range a {
			for _, z := range b {
				if v == z {
					overlap = true
				}
			}
		}
		if !overlap {
			return false
		}
	}
	return true
}
func (a *App) compileBundle(ctx context.Context, tx *sql.Tx, req pipeMap) (pipeMap, error) {
	targets := pipeMap{}
	rules := pl(req, "rules")
	seen := map[string]bool{}
	for i, v := range rules {
		rule := v.(map[string]any)
		id := ps(rule, "ruleId")
		if seen[id] {
			return nil, errors.New("duplicate ruleId")
		}
		seen[id] = true
		for _, prev := range rules[:i] {
			p := prev.(map[string]any)
			if pn(rule, "priority") == pn(p, "priority") && pipeMatchOverlap(po(rule, "match"), po(p, "match")) {
				return nil, errors.New("same-priority rules overlap")
			}
		}
		if ps(rule, "action") == "DECODE" && !parserKnown(ps(rule, "parserId"), ps(rule, "parserVersion"), ps(rule, "schemaVersion")) {
			return nil, errors.New("parser/schema dependency not installed")
		}
		seenTarget := map[string]bool{}
		for _, t := range pl(rule, "targetVersions") {
			ref := t.(map[string]any)
			key := ps(ref, "targetId") + "@" + ps(ref, "version")
			if seenTarget[key] {
				return nil, errors.New("duplicate target in rule")
			}
			seenTarget[key] = true
			var raw string
			if err := tx.QueryRowContext(ctx, "SELECT config_json FROM delivery_targets WHERE target_id=? AND version=? AND state='published'", ps(ref, "targetId"), ps(ref, "version")).Scan(&raw); err != nil {
				return nil, errors.New("target version not published")
			}
			c := pm(raw)
			if ps(c, "mappingVersion") != ps(ref, "mappingVersion") {
				return nil, errors.New("mapping dependency mismatch")
			}
			if err := a.validateTarget(ctx, c); err != nil {
				return nil, err
			}
			targets[key] = c
		}
	}
	m := pm(pipeJSON(req))
	m["targets"] = targets
	return m, nil
}
func (a *App) pipelineBundleCreate(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "rule:write")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "RuleBundleCreate")
	if !ok {
		return
	}
	a.pipeMutation(w, r, p, "bundle:create", req, func(tx *sql.Tx) (int, pipeMap, error) {
		m, err := a.compileBundle(r.Context(), tx, req)
		if err != nil {
			return 0, nil, err
		}
		id, artifact := pipeID("bundle"), pipeID("artifact")
		raw := pipeJSON(m)
		hash := pipeHash(m)
		var version int
		if err = tx.QueryRowContext(r.Context(), "SELECT COALESCE(MAX(version),0)+1 FROM rule_bundles").Scan(&version); err != nil {
			return 0, nil, err
		}
		now := pnow()
		_, err = tx.ExecContext(r.Context(), "INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,state,created_at_ms) VALUES(?,'bundle',?,?,?,?,?,'local-administrator','verified',?)", artifact, id, strconv.Itoa(version), hash, "management:rule_bundles/"+id, len(raw), now)
		if err == nil {
			_, err = tx.ExecContext(r.Context(), "INSERT INTO rule_bundles(bundle_id,version,artifact_id,content_hash,manifest_json,state,created_by,created_at_ms) VALUES(?,?,?,?,?,'draft',?,?)", id, version, artifact, hash, raw, p.ID, now)
		}
		return 201, bundleView(id, hash, "draft", raw, now), err
	})
}
func pipeOperation(kind, id string) pipeMap {
	return pipeMap{"operationId": pipeID("op"), "kind": kind, "state": "SUCCEEDED", "createdAt": pts(pnow()), "progress": 1, "resourceIds": []string{id}}
}
func (a *App) pipelineBundleValidate(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "rule:validate")
	if !ok {
		return
	}
	id := chi.URLParam(r, "bundleId")
	a.pipeMutation(w, r, p, "bundle:validate:"+id, pipeMap{"bundleId": id}, func(tx *sql.Tx) (int, pipeMap, error) {
		var raw, hash string
		if err := tx.QueryRowContext(r.Context(), "SELECT manifest_json,content_hash FROM rule_bundles WHERE bundle_id=?", id).Scan(&raw, &hash); err != nil {
			return 0, nil, err
		}
		if pipeHash(pm(raw)) != hash {
			return 0, nil, errors.New("bundle digest mismatch")
		}
		if _, err := a.compileBundle(r.Context(), tx, pm(raw)); err != nil {
			return 0, nil, err
		}
		_, err := tx.ExecContext(r.Context(), "UPDATE rule_bundles SET state='validated' WHERE bundle_id=? AND state='draft'", id)
		return 202, pipeOperation("BUNDLE_VALIDATION", id), err
	})
}

// The management intent commits before activation; replay reconciles an interrupted handshake.
func (a *App) pipelineBundleActivate(w http.ResponseWriter, r *http.Request) {
	p, ok := a.pipeGlobal(w, r, "rule:activate")
	if !ok {
		return
	}
	req, ok := a.Decode(w, r, "BundleActivation")
	if !ok {
		return
	}
	id := chi.URLParam(r, "bundleId")
	req["bundleId"] = id
	req["ifMatch"] = r.Header.Get("If-Match")
	key := r.Header.Get("Idempotency-Key")
	if key == "" || len(key) > 128 {
		pfail(w, r, 400, "IDEMPOTENCY_REQUIRED", "Idempotency-Key required")
		return
	}
	if ps(req, "dataNodeId") != a.Config.NodeID {
		pfail(w, r, 422, "NODE_UNAVAILABLE", "this deployment activates only the configured local data node")
		return
	}
	expected := pn(req, "expectedActivationRevision")
	if r.Header.Get("If-Match") != fmt.Sprintf("\"activation-%d\"", expected) {
		pfail(w, r, 412, "PRECONDITION_FAILED", "copy activationEtag from system/status")
		return
	}
	ctx := r.Context()
	tx, err := a.Management.BeginTx(ctx, nil)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	defer tx.Rollback()
	op := pipeID("op")
	var priorHash, response, state string
	err = tx.QueryRowContext(ctx, "SELECT operation_id,request_hash,COALESCE(response_json,''),state FROM api_idempotency WHERE principal_id=? AND operation='bundle:activate' AND idempotency_key=?", p.ID, key).Scan(&op, &priorHash, &response, &state)
	if err == nil {
		if priorHash != pipeHash(req) {
			pfail(w, r, 409, "IDEMPOTENCY_CONFLICT", "key request differs")
			return
		}
		if state == "committed" {
			pjson(w, 202, pm(response))
			return
		}
	} else if !errors.Is(err, sql.ErrNoRows) {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	var raw, hash, bstate string
	if err = tx.QueryRowContext(ctx, "SELECT manifest_json,content_hash,state FROM rule_bundles WHERE bundle_id=?", id).Scan(&raw, &hash, &bstate); err != nil || !(bstate == "validated" || bstate == "released") {
		pfail(w, r, 422, "BUNDLE_NOT_VALIDATED", "validate bundle before activation")
		return
	}
	if priorHash == "" {
		var current int64
		if err = a.Pipeline.QueryRowContext(ctx, "SELECT activation_epoch FROM pipeline_bundles WHERE namespace_id=? AND state='active'", a.Config.NamespaceID).Scan(&current); err != nil || current != expected {
			pfail(w, r, 412, "PRECONDITION_FAILED", "activation revision changed")
			return
		}
		_, err = tx.ExecContext(ctx, "INSERT INTO api_idempotency(operation_id,principal_id,operation,idempotency_key,request_hash,state,resource_type,resource_id,created_at_ms,expires_at_ms) VALUES(?,?,'bundle:activate',?,?,'reserved','pipeline',?,?,?)", op, p.ID, key, pipeHash(req), id, pnow(), pnow()+7*86400000)
		if err == nil {
			_, err = tx.ExecContext(ctx, "INSERT INTO bundle_deployments(deployment_id,bundle_id,node_id,activation_epoch,state,created_at_ms) VALUES(?,?,?,?,'requested',?)", op, id, a.Config.NodeID, expected+1, pnow())
		}
		if err == nil {
			err = pipeAudit(ctx, tx, p.ID, "bundle:activate", id)
		}
		if err != nil {
			pfail(w, r, 409, "ACTIVATION_CONFLICT", err.Error())
			return
		}
	}
	if err = tx.Commit(); err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", err.Error())
		return
	}
	if err = a.activatePipelineBundle(ctx, id, raw, hash, expected); err != nil {
		pfail(w, r, 409, "ACTIVATION_CONFLICT", err.Error())
		return
	}
	result := pipeOperation("BUNDLE_ACTIVATION", id)
	result["operationId"] = op
	result["result"] = pipeMap{"activationRevision": expected + 1, "dataNodeId": a.Config.NodeID}
	tx, err = a.Management.BeginTx(ctx, nil)
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "retry same operation to reconcile activation")
		return
	}
	defer tx.Rollback()
	_, err = tx.ExecContext(ctx, "UPDATE bundle_deployments SET state='superseded' WHERE node_id=? AND state='active' AND deployment_id<>?", a.Config.NodeID, op)
	if err == nil {
		_, err = tx.ExecContext(ctx, "UPDATE bundle_deployments SET state='active',reported_hash=?,acknowledged_at_ms=? WHERE deployment_id=?", hash, pnow(), op)
	}
	if err == nil {
		_, err = tx.ExecContext(ctx, "UPDATE rule_bundles SET state='released' WHERE bundle_id=?", id)
	}
	if err == nil {
		_, err = tx.ExecContext(ctx, "UPDATE api_idempotency SET state='committed',http_status=202,response_json=? WHERE operation_id=?", pipeJSON(result), op)
	}
	if err == nil {
		err = tx.Commit()
	}
	if err != nil {
		pfail(w, r, 503, "STORE_UNAVAILABLE", "activation persisted; retry same key to reconcile")
		return
	}
	w.Header().Set("ETag", fmt.Sprintf("\"activation-%d\"", expected+1))
	w.Header().Set("Location", "/api/v1/operations/"+op)
	pjson(w, 202, result)
}
func (a *App) activatePipelineBundle(ctx context.Context, id, raw, hash string, expected int64) error {
	if pipeHash(pm(raw)) != hash {
		return errors.New("bundle digest mismatch")
	}
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var current int64
	var active string
	if err = tx.QueryRowContext(ctx, "SELECT activation_epoch,bundle_id FROM pipeline_bundles WHERE namespace_id=? AND state='active'", a.Config.NamespaceID).Scan(&current, &active); err != nil {
		return err
	}
	if current == expected+1 && active == id {
		return nil
	}
	if current != expected {
		return errors.New("activation revision changed")
	}
	if _, err = tx.ExecContext(ctx, "UPDATE pipeline_bundles SET state='retired' WHERE namespace_id=? AND state='active'", a.Config.NamespaceID); err != nil {
		return err
	}
	_, err = tx.ExecContext(ctx, "INSERT INTO pipeline_bundles(bundle_id,namespace_id,activation_epoch,content_hash,manifest_json,state,stored_at_ms,activated_at_ms) VALUES(?,?,?,?,?,'active',?,?) ON CONFLICT(bundle_id) DO UPDATE SET activation_epoch=excluded.activation_epoch,state='active',activated_at_ms=excluded.activated_at_ms", id, a.Config.NamespaceID, expected+1, hash, raw, pnow(), pnow())
	if err != nil {
		return err
	}
	return tx.Commit()
}
