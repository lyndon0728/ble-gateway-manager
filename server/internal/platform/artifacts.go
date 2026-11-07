package platform

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/sha256"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strings"

	"github.com/go-chi/chi/v5"
)

var artifactIDPattern = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$`)
var artifactHashPattern = regexp.MustCompile(`^[a-f0-9]{64}$`)

func (a *App) artifactView(ctx context.Context, q ctlQuery, id string) (map[string]any, error) {
	var kind, digest, state, version, compat string
	var size int64
	e := q.QueryRowContext(ctx, "SELECT kind,sha256,size_bytes,state,version,compatibility_json FROM artifacts WHERE artifact_id=?", id).Scan(&kind, &digest, &size, &state, &version, &compat)
	if errors.Is(e, sql.ErrNoRows) {
		return nil, Problem(404, "ARTIFACT_NOT_FOUND", "artifact not found")
	}
	if e != nil {
		return nil, e
	}
	apiKind := strings.ToUpper(kind)
	if kind == "installer" {
		apiKind = "PLATFORM_PACKAGE"
	}
	signature := "PENDING"
	if state == "verified" {
		signature = "VERIFIED"
	}
	if state == "blocked" || state == "retired" {
		signature = "REJECTED"
	}
	m := map[string]any{"artifactId": id, "kind": apiKind, "sha256": digest, "sizeBytes": size, "signatureState": signature, "version": version}
	var c map[string]any
	if json.Unmarshal([]byte(compat), &c) == nil {
		if models, ok := c["compatibleModels"]; ok {
			m["compatibleModels"] = models
		}
	}
	return m, nil
}
func (a *App) getArtifact(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "artifact:read")
	if !ok {
		return
	}
	if !a.Can(p, "artifact:read", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance artifact permission required")
		return
	}
	v, e := a.artifactView(r.Context(), a.Management, chi.URLParam(r, "artifactId"))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, v)
}

func (a *App) trustedArtifactKeys() map[string]string {
	keys := map[string]string{}
	if b, e := os.ReadFile(filepath.Join(a.Config.DataDir, "trust", "keys.json")); e == nil {
		_ = json.Unmarshal(b, &keys)
	}
	for k, v := range a.Config.ArtifactPublicKeys {
		keys[k] = v
	}
	return keys
}

// VerifyArtifactManifest authenticates exact bytes, avoiding JSON canonicalization ambiguity.
func VerifyArtifactManifest(raw, signature []byte, keys map[string]string) (map[string]any, error) {
	var m map[string]any
	if len(raw) > 65536 || json.Unmarshal(raw, &m) != nil {
		return nil, Problem(400, "INVALID_ARTIFACT_MANIFEST", "manifest must be a JSON object up to 64 KiB")
	}
	key, err := base64.StdEncoding.DecodeString(keys[Str(m, "keyId")])
	if err != nil || len(key) != ed25519.PublicKeySize || !ed25519.Verify(key, raw, signature) {
		return nil, Problem(422, "INVALID_SIGNATURE", "artifact signature or trust key is invalid")
	}
	if !artifactHashPattern.MatchString(Str(m, "sha256")) || Str(m, "name") == "" || len(Str(m, "name")) > 128 || Str(m, "version") == "" || len(Str(m, "version")) > 64 {
		return nil, Problem(422, "INVALID_ARTIFACT_MANIFEST", "signed name, version, and SHA-256 are required")
	}
	for _, key := range []string{"compatibleModels", "hardwareVersions", "fromFirmwareVersions"} {
		if x, present := m[key]; present {
			list, ok := x.([]any)
			if !ok || len(list) > 128 {
				return nil, Problem(422, "INVALID_ARTIFACT_MANIFEST", "compatibility lists must have at most 128 strings")
			}
			for _, item := range list {
				s, ok := item.(string)
				if !ok || s == "" || len(s) > 128 {
					return nil, Problem(422, "INVALID_ARTIFACT_MANIFEST", "invalid compatibility identifier")
				}
			}
		}
	}
	return m, nil
}
func (a *App) uploadArtifact(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "artifact:write")
	if !ok {
		return
	}
	if !a.Can(p, "artifact:write", "scope_instance") {
		Fail(w, r, 403, "FORBIDDEN", "instance artifact permission required")
		return
	}
	r.Body = http.MaxBytesReader(w, r.Body, 256<<20)
	if e := r.ParseMultipartForm(1 << 20); e != nil {
		Fail(w, r, 400, "INVALID_UPLOAD", "multipart artifact must be at most 256 MiB")
		return
	}
	defer r.MultipartForm.RemoveAll()
	raw, signature := []byte(r.FormValue("manifest")), r.FormValue("signature")
	sig, e := base64.StdEncoding.DecodeString(signature)
	if e != nil {
		Fail(w, r, 422, "INVALID_SIGNATURE", "signature must be base64 Ed25519")
		return
	}
	manifest, e := VerifyArtifactManifest(raw, sig, a.trustedArtifactKeys())
	if e != nil {
		RespondError(w, r, e)
		return
	}
	kind := r.FormValue("kind")
	dbKind := map[string]string{"DEVICE_MANIFEST": "device_manifest", "FIRMWARE": "firmware", "PLATFORM_PACKAGE": "installer"}[kind]
	if dbKind == "" || Str(manifest, "kind") != kind || r.FormValue("sha256") != Str(manifest, "sha256") {
		Fail(w, r, 422, "ARTIFACT_METADATA_MISMATCH", "upload metadata must match signed manifest")
		return
	}
	if kind == "FIRMWARE" && len(List(manifest, "compatibleModels")) == 0 {
		Fail(w, r, 422, "FIRMWARE_COMPATIBILITY_REQUIRED", "signed firmware model compatibility required")
		return
	}
	file, _, e := r.FormFile("file")
	if e != nil {
		Fail(w, r, 400, "ARTIFACT_FILE_REQUIRED", "file part required")
		return
	}
	defer file.Close()
	dir := filepath.Join(a.Config.DataDir, "artifacts")
	if e = os.MkdirAll(dir, 0700); e != nil {
		RespondError(w, r, e)
		return
	}
	id := ID("artifact")
	path := filepath.Join(dir, id+".bin")
	f, e := os.OpenFile(path, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	keep := false
	defer func() {
		if !keep {
			_ = os.Remove(path)
		}
	}()
	h := sha256.New()
	size, e := io.Copy(io.MultiWriter(f, h), file)
	if e == nil {
		e = f.Sync()
	}
	e = errors.Join(e, f.Close())
	if e != nil {
		RespondError(w, r, e)
		return
	}
	digest := hex.EncodeToString(h.Sum(nil))
	if digest != Str(manifest, "sha256") {
		Fail(w, r, 422, "ARTIFACT_HASH_MISMATCH", "payload hash differs from signed hash")
		return
	}
	request := map[string]any{"kind": kind, "sha256": digest, "manifest": string(raw), "signature": signature}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "artifact.upload", r.Header.Get("Idempotency-Key"), request, func(tx *sql.Tx) (int, string, any, error) {
		var old string
		if e := tx.QueryRowContext(r.Context(), "SELECT artifact_id FROM artifacts WHERE kind=? AND name=? AND version=?", dbKind, Str(manifest, "name"), Str(manifest, "version")).Scan(&old); e == nil {
			return 0, "", nil, Problem(409, "ARTIFACT_VERSION_EXISTS", "artifact name and version are immutable")
		}
		_, e := tx.ExecContext(r.Context(), "INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES(?,?,?,?,?,?,?,?,?,'verified',?)", id, dbKind, Str(manifest, "name"), Str(manifest, "version"), digest, id+".bin", size, encode(map[string]any{"manifest": string(raw), "signature": signature}), encode(manifest), Now())
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "artifact.upload", "artifact", id, map[string]any{"kind": kind, "sha256": digest})
		}
		if e != nil {
			return 0, "", nil, e
		}
		v, e := a.artifactView(r.Context(), tx, id)
		return 201, id, v, e
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	if m, ok := v.(map[string]any); ok && Str(m, "artifactId") == id {
		keep = true
	}
	JSON(w, status, v)
}

func (a *App) checkFirmware(ctx context.Context, q ctlQuery, id string, g map[string]any, expected string) error {
	var kind, state, compat, version string
	e := q.QueryRowContext(ctx, "SELECT kind,state,compatibility_json,version FROM artifacts WHERE artifact_id=?", id).Scan(&kind, &state, &compat, &version)
	if e != nil || kind != "firmware" || state != "verified" {
		return Problem(422, "FIRMWARE_NOT_VERIFIED", "verified firmware artifact required")
	}
	if expected != version {
		return Problem(422, "FIRMWARE_VERSION_MISMATCH", "expected firmware must equal signed artifact version")
	}
	var c map[string]any
	_ = json.Unmarshal([]byte(compat), &c)
	for k, v := range map[string]string{"compatibleModels": Str(g, "model"), "hardwareVersions": Str(g, "hardwareVersion"), "fromFirmwareVersions": Str(g, "firmwareVersion")} {
		list := List(c, k)
		if len(list) == 0 && k != "compatibleModels" {
			continue
		}
		match := false
		for _, x := range list {
			if x == v {
				match = true
			}
		}
		if !match {
			return Problem(422, "FIRMWARE_INCOMPATIBLE", "firmware compatibility mismatch: "+k)
		}
	}
	return nil
}

func (a *App) downloadArtifact(w http.ResponseWriter, r *http.Request) {
	id := chi.URLParam(r, "artifactId")
	if !a.authorizeArtifactDownload(w, r, id) {
		return
	}
	a.serveArtifact(w, r, id)
}

func (a *App) authorizeArtifactDownload(w http.ResponseWriter, r *http.Request, id string) bool {
	if g, ok := a.Gateway(r); ok {
		var count int
		e := a.Management.QueryRowContext(r.Context(), "SELECT count(*) FROM command_tasks WHERE artifact_id=? AND gateway_id=? AND operation='FIRMWARE_UPGRADE' AND state IN ('queued','sent','device_accepted','executing','applied') AND expires_at_ms>?", id, g, Now()).Scan(&count)
		if e != nil || count == 0 {
			Fail(w, r, 403, "FORBIDDEN", "firmware download requires an active authorized task")
			return false
		}
	} else {
		p, ok := a.Require(w, r, "artifact:read")
		if !ok {
			return false
		}
		if !a.Can(p, "artifact:read", "scope_instance") {
			Fail(w, r, 403, "FORBIDDEN", "instance artifact permission required")
			return false
		}
	}
	return true
}

func (a *App) artifactVerification(w http.ResponseWriter, r *http.Request) {
	id := chi.URLParam(r, "artifactId")
	if !a.authorizeArtifactDownload(w, r, id) {
		return
	}
	var ref string
	if e := a.Management.QueryRowContext(r.Context(), "SELECT signature_ref FROM artifacts WHERE artifact_id=? AND state='verified'", id).Scan(&ref); e != nil {
		Fail(w, r, 404, "ARTIFACT_NOT_FOUND", "verified artifact not found")
		return
	}
	var v map[string]any
	if e := json.Unmarshal([]byte(ref), &v); e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, 200, v)
}

func (a *App) serveArtifact(w http.ResponseWriter, r *http.Request, id string) {
	var ref, digest, signature string
	e := a.Management.QueryRowContext(r.Context(), "SELECT storage_ref,sha256,signature_ref FROM artifacts WHERE artifact_id=? AND state='verified'", id).Scan(&ref, &digest, &signature)
	if e != nil {
		Fail(w, r, 404, "ARTIFACT_NOT_FOUND", "verified artifact not found")
		return
	}
	if filepath.Base(ref) != ref {
		Fail(w, r, 500, "INVALID_STORAGE_REFERENCE", "invalid artifact storage reference")
		return
	}
	f, e := os.Open(filepath.Join(a.Config.DataDir, "artifacts", ref))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer f.Close()
	st, e := f.Stat()
	if e != nil {
		RespondError(w, r, e)
		return
	}
	w.Header().Set("ETag", `"`+digest+`"`)
	w.Header().Set("X-Content-SHA256", digest)
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Disposition", `attachment; filename="`+id+`.bin"`)
	http.ServeContent(w, r, id+".bin", st.ModTime(), f)
}

type DeviceManifest struct {
	SchemaVersion string           `json:"schemaVersion"`
	Devices       []ManifestDevice `json:"devices"`
}
type ManifestDevice struct {
	GatewayID       string   `json:"gatewayId"`
	SN              string   `json:"sn"`
	Model           string   `json:"model"`
	HardwareVersion string   `json:"hardwareVersion"`
	FirmwareVersion string   `json:"firmwareVersion,omitempty"`
	DisplayName     string   `json:"displayName,omitempty"`
	GroupID         string   `json:"groupId,omitempty"`
	Capabilities    []string `json:"capabilities"`
	CredentialHash  string   `json:"credentialHash"`
}

func (a *App) importGateways(w http.ResponseWriter, r *http.Request) {
	p, ok := a.Require(w, r, "gateway:write")
	if !ok {
		return
	}
	body, ok := a.Decode(w, r, "GatewayImport")
	if !ok {
		return
	}
	group := Str(body, "groupId")
	if group == "" {
		group = "scope_default_group"
	}
	if !a.Can(p, "gateway:write", group) {
		Fail(w, r, 403, "FORBIDDEN", "import group denied")
		return
	}
	artifact := Str(body, "artifactId")
	var ref, digest string
	e := a.Management.QueryRowContext(r.Context(), "SELECT storage_ref,sha256 FROM artifacts WHERE artifact_id=? AND kind='device_manifest' AND state='verified'", artifact).Scan(&ref, &digest)
	if e != nil || filepath.Base(ref) != ref {
		Fail(w, r, 422, "MANIFEST_NOT_VERIFIED", "verified device manifest required")
		return
	}
	f, e := os.Open(filepath.Join(a.Config.DataDir, "artifacts", ref))
	if e != nil {
		RespondError(w, r, e)
		return
	}
	payload, readErr := io.ReadAll(io.LimitReader(f, (16<<20)+1))
	f.Close()
	actual := sha256.Sum256(payload)
	if readErr != nil || len(payload) > 16<<20 || hex.EncodeToString(actual[:]) != digest {
		Fail(w, r, 422, "ARTIFACT_HASH_MISMATCH", "device manifest file failed integrity verification")
		return
	}
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.DisallowUnknownFields()
	var manifest DeviceManifest
	e = decoder.Decode(&manifest)
	if e == nil && decoder.Decode(new(any)) != io.EOF {
		e = errors.New("trailing manifest data")
	}
	if e != nil || manifest.SchemaVersion != "1.0" || len(manifest.Devices) < 1 || len(manifest.Devices) > 10000 {
		Fail(w, r, 422, "INVALID_DEVICE_MANIFEST", "manifest version 1.0 and 1..10000 devices required")
		return
	}
	ids, sns, tokens := map[string]bool{}, map[string]bool{}, map[string]bool{}
	for i := range manifest.Devices {
		d := &manifest.Devices[i]
		if d.GroupID == "" {
			d.GroupID = group
		}
		if !a.Can(p, "gateway:write", d.GroupID) {
			Fail(w, r, 403, "FORBIDDEN", "manifest device group denied")
			return
		}
		if !artifactIDPattern.MatchString(d.GatewayID) || d.SN == "" || len(d.SN) > 128 || d.Model == "" || len(d.Model) > 128 || d.HardwareVersion == "" || len(d.HardwareVersion) > 64 || len(d.FirmwareVersion) > 64 || len(d.DisplayName) > 100 || !artifactHashPattern.MatchString(d.CredentialHash) || ids[d.GatewayID] || sns[d.SN] || tokens[d.CredentialHash] {
			Fail(w, r, 422, "INVALID_DEVICE_MANIFEST", "invalid or duplicate identity or token hash")
			return
		}
		ids[d.GatewayID] = true
		sns[d.SN] = true
		tokens[d.CredentialHash] = true
		if d.Capabilities == nil {
			d.Capabilities = []string{}
		}
		caps := map[string]bool{}
		for _, cap := range d.Capabilities {
			if _, ok := taskPermission[cap]; !ok || caps[cap] {
				Fail(w, r, 422, "INVALID_DEVICE_CAPABILITY", "capabilities must be unique supported task types")
				return
			}
			caps[cap] = true
		}
	}
	status, v, e := a.DoIdempotent(r.Context(), p.ID, "gateway.import", r.Header.Get("Idempotency-Key"), body, func(tx *sql.Tx) (int, string, any, error) {
		dry, _ := body["dryRun"].(bool)
		id := ID("job")
		results := []any{}
		for _, d := range manifest.Devices {
			var count int
			if e := tx.QueryRowContext(r.Context(), "SELECT count(*) FROM credentials WHERE verifier=? AND kind='device_secret' AND status='active'", d.CredentialHash).Scan(&count); e != nil {
				return 0, "", nil, e
			}
			if count > 0 {
				return 0, "", nil, Problem(409, "DEVICE_CREDENTIAL_CONFLICT", "device credential is already bound")
			}
			if e := tx.QueryRowContext(r.Context(), "SELECT count(*) FROM gateways WHERE gateway_id=? OR serial_number=?", d.GatewayID, d.SN).Scan(&count); e != nil {
				return 0, "", nil, e
			}
			if count > 0 {
				return 0, "", nil, Problem(409, "GATEWAY_EXISTS", "manifest identity conflicts with existing registry: "+d.GatewayID)
			}
			var scopeKind, status string
			if e := tx.QueryRowContext(r.Context(), "SELECT kind,status FROM resource_scopes WHERE scope_id=?", d.GroupID).Scan(&scopeKind, &status); e != nil || scopeKind != "group" || status != "active" {
				return 0, "", nil, Problem(422, "INVALID_GROUP", "active group required: "+d.GroupID)
			}
			results = append(results, map[string]any{"gatewayId": d.GatewayID, "status": "VALIDATED"})
			if dry {
				continue
			}
			_, e := tx.ExecContext(r.Context(), "INSERT INTO gateways(gateway_id,serial_number,display_name,scope_id,model,hardware_version,firmware_version,shipment_ref,lifecycle,capabilities_json,created_at_ms) VALUES(?,?,?,?,?,?,?,?,'active',?,?)", d.GatewayID, d.SN, d.DisplayName, d.GroupID, d.Model, d.HardwareVersion, d.FirmwareVersion, artifact, encode(d.Capabilities), Now())
			if e != nil {
				return 0, "", nil, e
			}
			_, e = tx.ExecContext(r.Context(), "INSERT INTO credentials(credential_id,label,gateway_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,?,?,'device_secret',?,'active',?,?)", ID("cred"), "factory token", d.GatewayID, d.CredentialHash, Now(), Now())
			if e != nil {
				return 0, "", nil, e
			}
			_, e = tx.ExecContext(r.Context(), "INSERT INTO gateway_configs(config_id,gateway_id,revision,desired_json,content_hash,state,created_by,created_at_ms) VALUES(?,?,1,'{}',?,'desired',?,?)", ID("cfg"), d.GatewayID, Hash(map[string]any{}), p.ID, Now())
			if e != nil {
				return 0, "", nil, e
			}
		}
		result := map[string]any{"dryRun": dry, "validated": len(results), "imported": 0, "devices": results}
		if !dry {
			result["imported"] = len(results)
		}
		_, e := tx.ExecContext(r.Context(), "INSERT INTO maintenance_jobs(job_id,kind,created_by,request_json,state,checkpoint_json,created_at_ms,finished_at_ms) VALUES(?,'shipment_import',?,?,'succeeded',?,?,?)", id, p.ID, encode(body), encode(result), Now(), Now())
		if e == nil {
			e = a.Audit(r.Context(), tx, p.ID, "gateway.import", "operation", id, map[string]any{"artifactId": artifact, "dryRun": dry, "count": len(results)})
		}
		if e != nil {
			return 0, "", nil, e
		}
		return 202, id, operationView(id, "shipment_import", "succeeded", Now(), Now(), result), nil
	})
	if e != nil {
		RespondError(w, r, e)
		return
	}
	JSON(w, status, v)
}
