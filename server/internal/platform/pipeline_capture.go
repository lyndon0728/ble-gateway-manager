package platform

import (
	"encoding/json"
	"fmt"
	"regexp"
)

const CaptureContextCapability = "GW32_CAPTURE_CONTEXT_1"

var captureAddress = regexp.MustCompile(`^[0-9A-F]{12}$`)
var captureConnection = regexp.MustCompile(`^[0-9a-f]{32}$`)

type PipelineIngressError struct {
	Status       int
	Code, Detail string
}

func (e *PipelineIngressError) Error() string { return e.Code + ": " + e.Detail }

func captureHasCapability(raw string, required string) bool {
	var capabilities []string
	if json.Unmarshal([]byte(raw), &capabilities) != nil {
		return false
	}
	for _, v := range capabilities {
		if v == required {
			return true
		}
	}
	return false
}

// ValidateCaptureEvent checks semantic relationships beyond JSON field shapes.
// It does not execute GATT, inspect live connections or mutate management state:
// delayed notifications retain the connection snapshot captured in the spool.
func ValidateCaptureEvent(version string, event map[string]any, payload []byte) error {
	bad := func(detail string) error { return &PipelineIngressError{400, "CAPTURE_INVALID", detail} }
	capture, present := event["capture"]
	if version == "1.0" {
		if present {
			return bad("schemaVersion 1.0 cannot contain capture")
		}
		return nil
	}
	if version != "1.1" {
		return &PipelineIngressError{400, "INGRESS_VERSION_UNSUPPORTED", "supported ingress versions are 1.0 and 1.1"}
	}
	c, ok := capture.(map[string]any)
	if !present || !ok {
		return bad("schemaVersion 1.1 requires capture")
	}
	if ps(event, "timeQuality") == "SYNCED" && ps(event, "capturedAt") == "" {
		return bad("SYNCED capture requires its original observation timestamp")
	}
	if ps(event, "timeQuality") == "UNSYNCED" {
		if _, present := event["capturedAt"]; present {
			return bad("UNSYNCED capture must omit capturedAt")
		}
	}
	radio, kind := ps(c, "radio"), ps(c, "kind")
	if radio != "NRF54" && radio != "ESP32" {
		return bad("capture.radio must be NRF54 or ESP32")
	}
	base := map[string]bool{"radio": true, "kind": true}
	allowed := map[string]bool{}
	for k, v := range base {
		allowed[k] = v
	}
	isAdvertisement := kind == "ADV" || kind == "SCAN_RSP"
	if isAdvertisement {
		for _, k := range []string{"addressType", "address", "legacy", "observedBy", "rssiDbm", "txPowerDbm", "primaryPhy", "secondaryPhy", "sid"} {
			allowed[k] = true
		}
		if kind == "ADV" {
			allowed["connectable"] = true
		}
	} else if kind == "GATT_NOTIFY" || kind == "GATT_INDICATE" {
		for _, k := range []string{"addressType", "address", "connectionRef", "characteristicHandle"} {
			allowed[k] = true
		}
	} else if kind == "GATT_LINK_DOWN" {
		allowed["connectionRef"] = true
		allowed["linkReason"] = true
	} else {
		return bad("unsupported capture.kind")
	}
	for k := range c {
		if !allowed[k] {
			return bad(fmt.Sprintf("capture.%s is not valid for %s", k, kind))
		}
	}
	address, hasAddress := c["address"]
	addressType, hasAddressType := c["addressType"]
	if hasAddress != hasAddressType {
		return bad("address and addressType must be provided together")
	}
	if hasAddress {
		a, ok := address.(string)
		typ, okType := addressType.(string)
		if !ok || !okType || !captureAddress.MatchString(a) || (typ != "PUBLIC" && typ != "RANDOM") {
			return bad("capture address must be 12 uppercase hex digits with PUBLIC or RANDOM type")
		}
	}
	if isAdvertisement {
		legacy, ok := c["legacy"].(bool)
		if !ok {
			return bad("ADV and SCAN_RSP require boolean legacy")
		}
		if v, present := c["connectable"]; present {
			if _, ok := v.(bool); !ok {
				return bad("connectable must be boolean")
			}
		} else if kind == "ADV" && radio == "NRF54" {
			return bad("NRF54 ADV requires the observed connectable property")
		}
		for _, k := range []string{"rssiDbm", "txPowerDbm"} {
			if v, present := c[k]; present {
				n, valid := captureInteger(v)
				if !valid || n < -127 || n > 127 {
					return bad(k + " must be a known signed 8-bit dBm value; omit unknown -128")
				}
			}
		}
		if v, present := c["sid"]; present {
			n, valid := captureInteger(v)
			if !valid || n < 0 || n > 15 || legacy {
				return bad("sid is 0..15 and is only valid for extended reports")
			}
		}
		for _, k := range []string{"primaryPhy", "secondaryPhy"} {
			if v, present := c[k]; present {
				phy, valid := v.(string)
				if !valid || (phy != "LE_1M" && phy != "LE_2M" && phy != "LE_CODED") {
					return bad("invalid PHY")
				}
				if k == "primaryPhy" && phy == "LE_2M" {
					return bad("LE_2M is not an advertising primary PHY")
				}
				if legacy && ((k == "primaryPhy" && phy != "LE_1M") || k == "secondaryPhy") {
					return bad("legacy reports have only an LE_1M primary PHY")
				}
			}
		}
		if value, present := c["observedBy"]; present {
			if radio != "NRF54" || pipeHash(value) != pipeHash([]string{"NRF54", "ESP32"}) || !hasAddress {
				return bad("observedBy requires the exact merged pair, NRF54 primary and known address")
			}
			if !legacy {
				if _, hasSID := c["sid"]; !hasSID {
					return bad("merged extended reports require SID")
				}
			}
		}
		hint := ps(event, "protocolHint")
		if hint == "ble-gatt-value-v1" || hint == "ble-gatt-link-v1" {
			return bad("advertising reports cannot use a GATT protocol hint")
		}
		return nil
	}
	if !captureConnection.MatchString(ps(c, "connectionRef")) {
		return bad("connectionRef must be 32 lowercase hex digits")
	}
	if kind == "GATT_LINK_DOWN" {
		reason := ps(c, "linkReason")
		if reason != "DISCONNECTED" && reason != "NRF_RESTART" && reason != "ESP_RESTART" {
			return bad("invalid linkReason")
		}
		if ps(event, "protocolHint") != "ble-gatt-link-v1" || len(payload) != 0 {
			return bad("GATT_LINK_DOWN requires ble-gatt-link-v1 and empty payload")
		}
		if reason == "NRF_RESTART" && radio != "NRF54" {
			return bad("NRF_RESTART can only invalidate an NRF54 connection")
		}
		return nil
	}
	handle, valid := captureInteger(c["characteristicHandle"])
	if !valid || handle < 1 || handle > 65535 {
		return bad("GATT notification/indication requires characteristicHandle 1..65535")
	}
	if ps(event, "protocolHint") != "ble-gatt-value-v1" || len(payload) > 512 {
		return bad("GATT values require ble-gatt-value-v1 and 0..512 value bytes")
	}
	return nil
}

func captureInteger(v any) (int64, bool) {
	switch n := v.(type) {
	case int:
		return int64(n), true
	case int64:
		return n, true
	case float64:
		i := int64(n)
		return i, float64(i) == n
	case json.Number:
		f, err := n.Float64()
		if err != nil {
			return 0, false
		}
		i := int64(f)
		return i, float64(i) == f
	default:
		return 0, false
	}
}
