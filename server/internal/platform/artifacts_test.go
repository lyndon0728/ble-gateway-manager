package platform

import (
	"bytes"
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"mime/multipart"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestArtifactSignatureBindsExactManifest(t *testing.T) {
	pub, priv, e := ed25519.GenerateKey(rand.Reader)
	if e != nil {
		t.Fatal(e)
	}
	raw := []byte(`{"keyId":"factory-v1","kind":"FIRMWARE","name":"gw","version":"2","sha256":"0000000000000000000000000000000000000000000000000000000000000000","compatibleModels":["GW"]}`)
	signature := ed25519.Sign(priv, raw)
	keys := map[string]string{"factory-v1": base64.StdEncoding.EncodeToString(pub)}
	if _, e = VerifyArtifactManifest(raw, signature, keys); e != nil {
		t.Fatal(e)
	}
	tampered := append([]byte(nil), raw...)
	tampered[len(tampered)-3] = 'X'
	if _, e = VerifyArtifactManifest(tampered, signature, keys); e == nil {
		t.Fatal("tampered compatibility accepted")
	}
	if _, e = VerifyArtifactManifest(raw, signature, map[string]string{}); e == nil {
		t.Fatal("unknown signer accepted")
	}
}

func TestFirmwareCompatibilityCannotBeBypassed(t *testing.T) {
	a, _ := controlFixture(t)
	_, e := a.Management.Exec("INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES('artifact_test','firmware','test','2.0',?,'unused',1,'sig',?,'verified',?)", Hash("firmware"), `{"compatibleModels":["GW"],"hardwareVersions":["1"],"fromFirmwareVersions":["1.0"]}`, Now())
	if e != nil {
		t.Fatal(e)
	}
	gateway := map[string]any{"model": "GW", "hardwareVersion": "1", "firmwareVersion": "1.0"}
	if e = a.checkFirmware(t.Context(), a.Management, "artifact_test", gateway, "2.0"); e != nil {
		t.Fatal(e)
	}
	gateway["hardwareVersion"] = "2"
	if e = a.checkFirmware(t.Context(), a.Management, "artifact_test", gateway, "2.0"); e == nil {
		t.Fatal("wrong hardware accepted")
	}
	gateway["hardwareVersion"] = "1"
	if e = a.checkFirmware(t.Context(), a.Management, "artifact_test", gateway, "3.0"); e == nil {
		t.Fatal("unsigned version accepted")
	}
}

func TestSignedDeviceImportHTTP(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	p, e := a.principal(ctx, actor)
	if e != nil {
		t.Fatal(e)
	}
	pub, priv, e := ed25519.GenerateKey(rand.Reader)
	if e != nil {
		t.Fatal(e)
	}
	a.Config.ArtifactPublicKeys = map[string]string{"factory": base64.StdEncoding.EncodeToString(pub)}
	secret := "reference-import-secret-32-byte-minimum"
	payload := []byte(encode(DeviceManifest{SchemaVersion: "1.0", Devices: []ManifestDevice{{GatewayID: "gw_imported", SN: "sn_imported", Model: "GW", HardwareVersion: "1", Capabilities: []string{"CONFIGURE", "REBOOT", "DIAGNOSTICS"}, CredentialHash: TokenHash(secret)}}}))
	hash := sha256.Sum256(payload)
	digest := hex.EncodeToString(hash[:])
	manifest := []byte(encode(map[string]any{"keyId": "factory", "name": "test-shipment", "version": "1", "kind": "DEVICE_MANIFEST", "sha256": digest}))
	signature := base64.StdEncoding.EncodeToString(ed25519.Sign(priv, manifest))
	var body bytes.Buffer
	writer := multipart.NewWriter(&body)
	for k, v := range map[string]string{"kind": "DEVICE_MANIFEST", "sha256": digest, "manifest": string(manifest), "signature": signature} {
		if e = writer.WriteField(k, v); e != nil {
			t.Fatal(e)
		}
	}
	file, e := writer.CreateFormFile("file", "shipment.json")
	if e != nil {
		t.Fatal(e)
	}
	file.Write(payload)
	writer.Close()
	req := httptest.NewRequest(http.MethodPost, "/api/v1/artifacts", &body)
	req.Header.Set("Content-Type", writer.FormDataContentType())
	req.Header.Set("Idempotency-Key", "upload-one")
	req = req.WithContext(context.WithValue(req.Context(), authKey{}, &authInfo{Principal: p}))
	res := httptest.NewRecorder()
	a.uploadArtifact(res, req)
	if res.Code != 201 {
		t.Fatalf("upload %d %s", res.Code, res.Body.String())
	}
	var artifact map[string]any
	if e = json.Unmarshal(res.Body.Bytes(), &artifact); e != nil {
		t.Fatal(e)
	}
	if e = a.Validate("Artifact", artifact); e != nil {
		t.Fatal(e)
	}
	importBody := map[string]any{"artifactId": Str(artifact, "artifactId"), "dryRun": false}
	request := httptest.NewRequest(http.MethodPost, "/api/v1/gateway-imports", bytes.NewBufferString(encode(importBody)))
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Idempotency-Key", "import-one")
	request = request.WithContext(context.WithValue(request.Context(), authKey{}, &authInfo{Principal: p}))
	response := httptest.NewRecorder()
	a.importGateways(response, request)
	if response.Code != 202 {
		t.Fatalf("import %d %s", response.Code, response.Body.String())
	}
	var operation map[string]any
	json.Unmarshal(response.Body.Bytes(), &operation)
	if e = a.Validate("Operation", operation); e != nil {
		t.Fatal(e)
	}
	authRequest := httptest.NewRequest(http.MethodGet, "/api/v1/gateway-ingress/commands", nil)
	authRequest.Header.Set("Authorization", "Bearer "+secret)
	id, ok := a.Gateway(authRequest)
	if !ok || id != "gw_imported" {
		t.Fatal("signed device credential did not authenticate")
	}
	view, e := a.gatewayView(ctx, a.Management, id)
	if e != nil || Str(view, "state") != "ACTIVE" {
		t.Fatalf("imported gateway %v %v", view, e)
	}
	if e = a.Validate("Gateway", view); e != nil {
		t.Fatal(e)
	}
	cfg, e := a.configurationView(ctx, a.Management, id)
	if e != nil {
		t.Fatal(e)
	}
	if e = a.Validate("Configuration", cfg); e != nil {
		t.Fatal(e)
	}
}
