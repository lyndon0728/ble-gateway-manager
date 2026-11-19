package platform

import (
	"crypto/rand"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"strconv"
	"strings"

	"github.com/go-chi/chi/v5"
)

func (a *App) RegisterGatewayIntegration(r chi.Router) {
	r.Post("/api/v1/gateway-time-challenges", a.gatewayTime)
	r.Post("/api/v1/gateway-component-challenges", a.componentChallenge)
	r.Put("/api/v1/gateway-components", a.putComponent)
	r.Get("/api/v1/gateway-component-artifact", a.componentArtifact)
	r.Get("/api/v1/gateways/{gatewayId}/components", a.getComponents)
	r.Put("/api/v1/gateway-runtime-capabilities", a.putRuntimeCapabilities)
	r.Get("/api/v1/gateway-configuration-snapshot", a.deviceConfigurationSnapshot)
	r.Get("/api/v1/gateways/{gatewayId}/configuration-snapshot", a.adminConfigurationSnapshot)
	r.Post("/api/v1/gateway-peripheral/batches", a.peripheralBatch)
	r.Put("/api/v1/gateways/{gatewayId}/peripheral-clients/{clientId}", a.putPeripheralClient)
	r.Get("/api/v1/gateways/{gatewayId}/peripheral-events", a.getPeripheralEvents)
	r.Post("/api/v1/gateways/{gatewayId}/scan-profile-preview", a.scanProfilePreview)
}
func (a *App) integrationIdentity(w http.ResponseWriter, r *http.Request, id string, b map[string]any) bool {
	if Str(b, "namespaceId") != a.Config.NamespaceID || Str(b, "gatewayId") != id {
		Fail(w, r, 403, "IDENTITY_MISMATCH", "device and payload identity differ")
		return false
	}
	return true
}
func (a *App) componentChallenge(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	b, ok := a.Decode(w, r, "GatewayComponentChallenge")
	if !ok {
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	g, e := a.gatewayView(r.Context(), tx, id)
	if e == nil {
		e = gw32Require(g, "COMPONENT_INVENTORY_1")
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	component := Str(b, "component")
	_, e = tx.ExecContext(r.Context(), "INSERT OR IGNORE INTO gateway_component_generations(gateway_id,component,generation) VALUES(?,?,'0')", id, component)
	var generation string
	if e == nil {
		e = tx.QueryRowContext(r.Context(), "SELECT generation FROM gateway_component_generations WHERE gateway_id=? AND component=?", id, component).Scan(&generation)
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	_, e = tx.ExecContext(r.Context(), "UPDATE gateway_component_challenges SET state='SUPERSEDED' WHERE gateway_id=? AND component=? AND state='PENDING'", id, component)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	nonce := make([]byte, 32)
	if _, e = rand.Read(nonce); e != nil {
		RespondError(w, r, e)
		return
	}
	challenge, expires := ID("inv"), Now()+60000
	encoded := base64.StdEncoding.EncodeToString(nonce)
	_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_component_challenges(challenge_id,gateway_id,component,generation,nonce,expires_at_ms,state) VALUES(?,?,?,?,?,?,'PENDING')", challenge, id, component, generation, encoded, expires)
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 201, map[string]any{"schemaVersion": "1.0", "challengeId": challenge, "component": component, "inventoryGeneration": generation, "nonce": encoded, "expiresAt": Timestamp(expires), "expiresAtUnixMs": strconv.FormatInt(expires, 10)})
}
func (a *App) putComponent(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	b, ok := a.Decode(w, r, "GatewayComponentReport")
	if !ok || !a.integrationIdentity(w, r, id, b) {
		return
	}
	level := Str(b, "evidenceLevel")
	if level == "CONFIRMED_ARTIFACT" {
		if Str(b, "controllerBootId") == "" || Str(b, "firmwareVersion") == "" || Str(b, "imageSha256") == "" || Str(b, "artifactId") == "" || b["bootConfirmed"] != true {
			Fail(w, r, 422, "INVALID_COMPONENT_EVIDENCE", "confirmed artifact requires current boot and complete confirmed image evidence")
			return
		}
	}
	if level == "VERSION_ONLY" {
		if Str(b, "firmwareVersion") == "" || b["imageSha256"] != nil || b["artifactId"] != nil || b["bootConfirmed"] == true {
			Fail(w, r, 422, "INVALID_COMPONENT_EVIDENCE", "version evidence cannot assert image identity")
			return
		}
	}
	if level == "UNKNOWN" {
		for _, key := range []string{"firmwareVersion", "imageSha256", "artifactId", "bootConfirmed"} {
			if _, present := b[key]; present {
				Fail(w, r, 422, "INVALID_COMPONENT_EVIDENCE", "unknown evidence cannot assert version or image")
				return
			}
		}
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var gateway, component, generation, nonce, state string
	var expires int64
	var priorHash, receipt sql.NullString
	e = tx.QueryRowContext(r.Context(), "SELECT gateway_id,component,generation,nonce,expires_at_ms,state,request_hash,receipt_json FROM gateway_component_challenges WHERE challenge_id=?", Str(b, "challengeId")).Scan(&gateway, &component, &generation, &nonce, &expires, &state, &priorHash, &receipt)
	digest := Hash(b)
	if e != nil || gateway != id || component != Str(b, "component") || nonce != Str(b, "nonce") || generation != Str(b, "inventoryGeneration") {
		Fail(w, r, 409, "COMPONENT_CHALLENGE_MISMATCH", "challenge does not match report")
		return
	}
	if state == "ACCEPTED" {
		if priorHash.String != digest {
			Fail(w, r, 409, "COMPONENT_EVIDENCE_CONFLICT", "challenge was used for different evidence")
			return
		}
		JSON(w, 200, pm(receipt.String))
		return
	}
	if state != "PENDING" || expires <= Now() {
		Fail(w, r, 409, "COMPONENT_CHALLENGE_EXPIRED", "challenge superseded or expired")
		return
	}
	var current string
	e = tx.QueryRowContext(r.Context(), "SELECT generation FROM gateway_component_generations WHERE gateway_id=? AND component=?", id, component).Scan(&current)
	n, ce := networkCounter(generation)
	if e != nil || ce != nil || current != generation || n == ^uint64(0) {
		Fail(w, r, 409, "INVENTORY_GENERATION_CONFLICT", "inventory generation changed")
		return
	}
	if level == "CONFIRMED_ARTIFACT" {
		var hash, version, compat string
		e = tx.QueryRowContext(r.Context(), "SELECT sha256,version,compatibility_json FROM artifacts WHERE artifact_id=? AND kind='firmware' AND state='verified'", Str(b, "artifactId")).Scan(&hash, &version, &compat)
		signed := pm(compat)
		c, ce := firmwareComponent(signed)
		if e != nil || ce != nil || c != component || hash != Str(b, "imageSha256") || version != Str(b, "firmwareVersion") {
			Fail(w, r, 422, "COMPONENT_ARTIFACT_MISMATCH", "running image must match verified signed artifact")
			return
		}
		g, ge := a.gatewayView(r.Context(), tx, id)
		if ge != nil {
			RespondError(w, r, ge)
			return
		}
		compatible := func(k, v string) bool {
			l := List(signed, k)
			if len(l) == 0 {
				return k != "compatibleModels"
			}
			for _, x := range l {
				if x == v {
					return true
				}
			}
			return false
		}
		if !compatible("compatibleModels", Str(g, "model")) || !compatible("hardwareVersions", Str(g, "hardwareVersion")) {
			Fail(w, r, 422, "COMPONENT_ARTIFACT_MISMATCH", "artifact is incompatible with this hardware")
			return
		}
		if task := Str(b, "upgradeTaskId"); task != "" {
			var count int
			e = tx.QueryRowContext(r.Context(), "SELECT count(*) FROM command_tasks WHERE task_id=? AND gateway_id=? AND artifact_id=? AND operation='FIRMWARE_UPGRADE' AND state IN ('applied','verified','succeeded')", task, id, Str(b, "artifactId")).Scan(&count)
			if e != nil || count != 1 {
				Fail(w, r, 422, "COMPONENT_TASK_MISMATCH", "upgrade evidence task is not verified for this artifact")
				return
			}
		}
	}
	next := strconv.FormatUint(n+1, 10)
	at := Now()
	b["inventoryGeneration"] = next
	b["evidenceReceivedAt"] = Timestamp(at)
	result := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": id, "challengeId": b["challengeId"], "component": component, "inventoryGeneration": next, "evidenceReceivedAt": Timestamp(at), "inventory": b}
	_, e = tx.ExecContext(r.Context(), "UPDATE gateway_component_generations SET generation=? WHERE gateway_id=? AND component=? AND generation=?", next, id, component, generation)
	if e == nil {
		_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_component_inventory(gateway_id,component,inventory_json,received_at_ms) VALUES(?,?,?,?) ON CONFLICT(gateway_id,component) DO UPDATE SET inventory_json=excluded.inventory_json,received_at_ms=excluded.received_at_ms", id, component, encode(b), at)
	}
	if e == nil {
		_, e = tx.ExecContext(r.Context(), "UPDATE gateway_component_challenges SET state='ACCEPTED',request_hash=?,receipt_json=? WHERE challenge_id=?", digest, encode(result), Str(b, "challengeId"))
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, result)
}
func (a *App) getComponents(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if !ok {
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT inventory_json FROM gateway_component_inventory WHERE gateway_id=? ORDER BY component", id)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	items := []any{}
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			RespondError(w, r, e)
			return
		}
		items = append(items, pm(raw))
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"items": items})
}
func (a *App) putRuntimeCapabilities(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	b, ok := a.Decode(w, r, "GatewayRuntimeCapabilities")
	if !ok || !a.integrationIdentity(w, r, id, b) {
		return
	}
	if b["nrfBootId"] == nil && len(List(b, "nrfHelloCapabilities")) != 0 {
		Fail(w, r, 422, "NRF_IDENTITY_REQUIRED", "unavailable nRF identity requires empty HELLO capabilities")
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	var digest, caps string
	e = tx.QueryRowContext(r.Context(), "SELECT a.sha256,g.capabilities_json FROM gateways g JOIN artifacts a ON a.artifact_id=g.shipment_ref AND a.kind='device_manifest' AND a.state='verified' WHERE g.gateway_id=?", id).Scan(&digest, &caps)
	if e != nil || digest != Str(b, "manifestDigest") {
		Fail(w, r, 422, "MANIFEST_NOT_VERIFIED", "runtime capabilities require the registered signed manufacturing manifest digest")
		return
	}
	var authorized []string
	if e = json.Unmarshal([]byte(caps), &authorized); e != nil {
		RespondError(w, r, e)
		return
	}
	contains := func(key, cap string) bool {
		for _, x := range List(b, key) {
			if x == cap {
				return true
			}
		}
		return false
	}
	available := []any{}
	reasons := map[string]any{}
	for _, cap := range authorized {
		reason := ""
		if !contains("espImplemented", cap) {
			reason = "NOT_IMPLEMENTED"
		}
		if strings.HasPrefix(cap, "NRF54_") && !contains("nrfHelloCapabilities", cap) {
			reason = "NRF_CAPABILITY_ABSENT"
		}
		if !contains("boardVerified", cap) {
			reason = "BOARD_NOT_VERIFIED"
		}
		if reason == "" {
			available = append(available, cap)
		} else {
			reasons[cap] = reason
		}
	}
	_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_runtime_capabilities(gateway_id,report_json,available_json,received_at_ms) VALUES(?,?,?,?) ON CONFLICT(gateway_id) DO UPDATE SET report_json=excluded.report_json,available_json=excluded.available_json,received_at_ms=excluded.received_at_ms", id, encode(b), encode(available), Now())
	if e == nil {
		_, e = tx.ExecContext(r.Context(), "DELETE FROM peripheral_connections WHERE gateway_id=? AND ((radio='ESP32' AND controller_boot_id<>?) OR (radio='NRF54' AND controller_boot_id<>?))", id, Str(b, "espBootId"), Str(b, "nrfBootId"))
	}
	if e == nil {
		e = tx.Commit()
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": id, "espBootId": b["espBootId"], "nrfBootId": b["nrfBootId"], "availableCapabilities": available, "unavailableReasons": reasons})
}
func (a *App) scanProfilePreview(w http.ResponseWriter, r *http.Request) {
	_, id, ok := a.networkAdmin(w, r, "gateway:read")
	if !ok {
		return
	}
	b, ok := a.Decode(w, r, "ScanProfilePreview")
	if !ok {
		return
	}
	tx, e := a.Management.BeginTx(r.Context(), nil)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer tx.Rollback()
	g, e := a.gatewayView(r.Context(), tx, id)
	c := map[string]any{"radios": map[string]any{Str(b, "radio"): b["profile"]}}
	if e == nil {
		e = a.validateGatewayConfiguration(r.Context(), tx, g, c)
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	before, e := a.gw32Snapshot(r.Context(), tx, id, 1<<62)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	after := gw32ApplyConfig(before, c)
	compiled, e := CompileScanProfileV2(Obj(Obj(after, "radios"), Str(b, "radio")), 1)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	result := map[string]any{"profile": Obj(Obj(after, "radios"), Str(b, "radio")), "bodyLength": len(compiled), "maximumBodyLength": 3070, "bodyBase64": base64.StdEncoding.EncodeToString(compiled), "revisionForPreview": 1}
	if samples, ok := b["samples"]; ok {
		results := []any{}
		for _, sample := range samples.([]any) {
			results = append(results, BeaconV2Preview(Obj(Obj(after, "radios"), Str(b, "radio")), sample.(map[string]any)))
		}
		result["sampleResults"] = results
	}
	JSON(w, 200, result)
}
