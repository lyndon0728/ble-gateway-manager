package platform

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/base64"
	"strings"
	"testing"
)

func nrf54Artifact(t *testing.T, a *App, metadata map[string]any) {
	t.Helper()
	_, err := a.Management.Exec("INSERT INTO artifacts(artifact_id,kind,name,version,sha256,storage_ref,size_bytes,signature_ref,compatibility_json,state,created_at_ms) VALUES('nrf54_image','firmware','gw32-nrf54','2.0',?,'unused',1,'sig',?,'verified',?)", Hash("signed nrf54 image"), encode(metadata), Now())
	if err != nil {
		t.Fatal(err)
	}
}

func nrf54Metadata() map[string]any {
	return map[string]any{"component": "NRF54", "imageFormat": "MCUBOOT_SIGNED_BIN", "compatibleModels": []any{"GW32I33V2"}, "hardwareVersions": []any{"1"}}
}

func TestNRF54SignedManifestBindsImageFormat(t *testing.T) {
	public, private, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	manifest := nrf54Metadata()
	manifest["keyId"] = "release"
	manifest["kind"] = "FIRMWARE"
	manifest["name"] = "gw32-nrf54"
	manifest["version"] = "2.0"
	manifest["sha256"] = strings.Repeat("0", 64)
	keys := map[string]string{"release": base64.StdEncoding.EncodeToString(public)}
	raw := []byte(encode(manifest))
	if _, err = VerifyArtifactManifest(raw, ed25519.Sign(private, raw), keys); err != nil {
		t.Fatal(err)
	}
	for _, format := range []any{"RAW_BIN", "", nil} {
		manifest["imageFormat"] = format
		raw = []byte(encode(manifest))
		if _, err = VerifyArtifactManifest(raw, ed25519.Sign(private, raw), keys); err == nil {
			t.Fatalf("invalid NRF54 image format accepted: %v", format)
		}
	}
}

func TestNRF54FirmwareComponentCompatibility(t *testing.T) {
	a, _ := controlFixture(t)
	metadata := nrf54Metadata()
	nrf54Artifact(t, a, metadata)
	gateway := map[string]any{"model": "GW32I33V2", "hardwareVersion": "1", "firmwareVersion": "esp-1"}
	if err := a.checkFirmwareComponent(t.Context(), a.Management, "nrf54_image", gateway, "2.0", "NRF54"); err != nil {
		t.Fatal(err)
	}
	if err := a.checkFirmware(t.Context(), a.Management, "nrf54_image", gateway, "2.0"); err == nil {
		t.Fatal("NRF54 image accepted as a gateway-host image")
	}
	for _, key := range []string{"model", "hardwareVersion"} {
		old := gateway[key]
		gateway[key] = "wrong"
		if err := a.checkFirmwareComponent(t.Context(), a.Management, "nrf54_image", gateway, "2.0", "NRF54"); err == nil {
			t.Fatal("component routing bypassed compatibility", key)
		}
		gateway[key] = old
	}
	metadata["fromFirmwareVersions"] = []any{"esp-1"}
	if _, err := a.Management.Exec("UPDATE artifacts SET compatibility_json=? WHERE artifact_id='nrf54_image'", encode(metadata)); err != nil {
		t.Fatal(err)
	}
	if err := a.checkFirmwareComponent(t.Context(), a.Management, "nrf54_image", gateway, "2.0", "NRF54"); err == nil {
		t.Fatal("ESP firmware version was incorrectly used as nRF source-version evidence")
	}
	delete(metadata, "fromFirmwareVersions")
	delete(metadata, "component")
	if _, err := a.Management.Exec("UPDATE artifacts SET compatibility_json=? WHERE artifact_id='nrf54_image'", encode(metadata)); err != nil {
		t.Fatal(err)
	}
	if err := a.checkFirmwareComponent(t.Context(), a.Management, "nrf54_image", gateway, "2.0", "NRF54"); err == nil {
		t.Fatal("unbound legacy artifact accepted as NRF54")
	}
}

func TestNRF54FirmwareTaskCapabilityAndBootConfirmation(t *testing.T) {
	a, actor := controlFixture(t)
	nrf54Artifact(t, a, nrf54Metadata())
	if _, err := a.Management.Exec("UPDATE gateways SET model='GW32I33V2' WHERE gateway_id='gw_test'"); err != nil {
		t.Fatal(err)
	}
	params := map[string]any{"component": "NRF54", "artifactId": "nrf54_image", "expectedFirmwareVersion": "2.0"}
	request := map[string]any{"gatewayId": "gw_test", "type": "FIRMWARE_UPGRADE", "parameters": params, "expiresAt": Timestamp(Now() + 60000)}
	if err := a.Validate("TaskRequest", request); err != nil {
		t.Fatal("component missing from task wire schema", err)
	}
	if _, err := gw32TryTask(a, actor, "missing-cap", "gw_test", "FIRMWARE_UPGRADE", params); err == nil {
		t.Fatal("NRF54 task accepted without signed UART DFU capability")
	}
	if _, err := a.Management.Exec("UPDATE gateways SET capabilities_json=? WHERE gateway_id='gw_test'", encode([]string{"FIRMWARE_UPGRADE", "NRF54_UART_DFU_1"})); err != nil {
		t.Fatal(err)
	}
	task, err := gw32TryTask(a, actor, "nrf-upgrade", "gw_test", "FIRMWARE_UPGRADE", params)
	if err != nil {
		t.Fatal(err)
	}
	id := Str(task, "taskId")
	details := map[string]any{"firmwareVersion": "2.0"}
	for i := 0; i < 3; i++ {
		if i == 1 {
			details["component"] = "NRF54"
			details["nrfBootId"] = strings.Repeat("a", 32)
			details["bootConfirmed"] = false
		}
		if i == 2 {
			details["bootConfirmed"] = true
			details["nrfBootId"] = "invalid"
		}
		ack, resultError := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", controlResult(id, ID("invalid"), int64(i+1), "SUCCEEDED", "VERIFIED", details)))
		if resultError != nil || controlAckStatus(ack) != "REJECTED_PERMANENT" {
			t.Fatalf("unverified component result accepted: %v %v", ack, resultError)
		}
	}
	details["nrfBootId"] = strings.Repeat("b", 32)
	imageHash := Hash("signed nrf54 image")
	for i, wrongHash := range []string{"", strings.Repeat("0", 64), strings.ToUpper(imageHash)} {
		if wrongHash == "" {
			delete(details, "imageSha256")
		} else {
			details["imageSha256"] = wrongHash
		}
		ack, resultError := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", controlResult(id, ID("wrong-image"), int64(i+4), "SUCCEEDED", "VERIFIED", details)))
		if resultError != nil || controlAckStatus(ack) != "REJECTED_PERMANENT" || Str(List(ack, "items")[0].(map[string]any), "code") != "NRF54_FIRMWARE_IMAGE_MISMATCH" {
			t.Fatalf("missing/different same-version image digest accepted: %v %v", ack, resultError)
		}
	}
	details["imageSha256"] = imageHash
	ack, err := a.AcceptTaskResults(t.Context(), "gw_test", controlBatch(a, "gw_test", controlResult(id, "nrf-confirmed", 7, "SUCCEEDED", "VERIFIED", details)))
	if err != nil || controlAckStatus(ack) != "ACCEPTED" {
		t.Fatal("verified component result rejected", ack, err)
	}
	var hostVersion string
	if err = a.Management.QueryRow("SELECT firmware_version FROM gateways WHERE gateway_id='gw_test'").Scan(&hostVersion); err != nil || hostVersion != "1.0" {
		t.Fatal("nRF result overwrote gateway host version", hostVersion, err)
	}
}
