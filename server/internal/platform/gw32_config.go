package platform

import (
	"context"
	"database/sql"
	"encoding/base64"
	"encoding/json"
	"net"
	"regexp"
	"strings"
	"unicode/utf8"
)

var gw32UUIDPattern = regexp.MustCompile(`(?i)^(?:[0-9a-f]{4}|[0-9a-f]{8}|[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})$`)
var gw32CapabilityPattern = regexp.MustCompile(`^[A-Z][A-Z0-9_]{0,95}$`)
var gw32ConnectionPattern = regexp.MustCompile(`^[0-9a-f]{32}$`)
var gw32PeerPattern = regexp.MustCompile(`(?i)^(?:[0-9a-f]{12}|[0-9a-f]{2}(?::[0-9a-f]{2}){5})$`)

func gw32Has(g map[string]any, cap string) bool {
	for _, v := range List(g, "capabilities") {
		if v == cap {
			return true
		}
	}
	return false
}
func gw32Model(g map[string]any) bool {
	return Str(g, "model") == "GW32I33V2" || strings.HasPrefix(Str(g, "model"), "GW32I33V2-")
}
func gw32Require(g map[string]any, caps ...string) error {
	for _, cap := range caps {
		if !gw32Has(g, cap) {
			return Problem(422, "CAPABILITY_UNSUPPORTED", "signed gateway capability required: "+cap)
		}
	}
	return nil
}
func gw32Profile(g map[string]any) error {
	if !gw32Model(g) {
		return Problem(422, "MODEL_UNSUPPORTED", "GW32 control profile requires a signed GW32I33V2 model")
	}
	return gw32Require(g, "GW32_CONTROL_PROFILE_1")
}
func gw32Invalid(message string) error { return Problem(422, "INVALID_CONFIGURATION", message) }
func gw32Keys(m map[string]any, keys ...string) error {
	allowed := map[string]bool{}
	for _, k := range keys {
		allowed[k] = true
	}
	for k := range m {
		if !allowed[k] {
			return gw32Invalid("unknown field: " + k)
		}
	}
	return nil
}
func gw32Number(v any) (int64, bool) {
	switch n := v.(type) {
	case float64:
		return int64(n), n == float64(int64(n))
	case int64:
		return n, true
	case int:
		return int64(n), true
	case json.Number:
		x, e := n.Int64()
		return x, e == nil
	}
	return 0, false
}
func gw32Range(v any, lo, hi int64) bool { n, ok := gw32Number(v); return ok && n >= lo && n <= hi }
func gw32Object(v any) (map[string]any, error) {
	m, ok := v.(map[string]any)
	if !ok || len(m) == 0 {
		return nil, gw32Invalid("a nonempty configuration object is required")
	}
	return m, nil
}
func gw32Bool(m map[string]any, k string) error {
	if v, ok := m[k]; ok {
		if _, ok = v.(bool); !ok {
			return gw32Invalid(k + " must be boolean")
		}
	}
	return nil
}
func gw32Decode(v any, min, max int) ([]byte, error) {
	s, ok := v.(string)
	if !ok {
		return nil, gw32Invalid("base64 string required")
	}
	b, e := base64.StdEncoding.Strict().DecodeString(s)
	if e != nil || len(b) < min || len(b) > max || base64.StdEncoding.EncodeToString(b) != s {
		return nil, gw32Invalid("invalid canonical base64 or decoded length")
	}
	return b, nil
}
func gw32CanonicalUUID(s string) string {
	s = strings.ToLower(s)
	if !gw32UUIDPattern.MatchString(s) {
		return ""
	}
	if len(s) == 4 {
		s = "0000" + s
	}
	if len(s) == 8 {
		s += "-0000-1000-8000-00805f9b34fb"
	}
	return s
}
func gw32Peer(s string) string {
	if !gw32PeerPattern.MatchString(s) {
		return ""
	}
	s = strings.ToUpper(strings.ReplaceAll(s, ":", ""))
	if len(s) != 12 {
		return ""
	}
	for _, c := range s {
		if !(c >= '0' && c <= '9' || c >= 'A' && c <= 'F') {
			return ""
		}
	}
	return s
}

func gw32ScanShape(m map[string]any) error {
	if e := gw32Keys(m, "enabled", "active", "intervalMs", "windowMs"); e != nil {
		return e
	}
	for _, k := range []string{"enabled", "active"} {
		if e := gw32Bool(m, k); e != nil {
			return e
		}
	}
	for _, k := range []string{"intervalMs", "windowMs"} {
		if v, ok := m[k]; ok && !gw32Range(v, 10, 10240) {
			return gw32Invalid("scan " + k + " must be 10..10240 integer milliseconds")
		}
	}
	if i, ok := m["intervalMs"]; ok {
		if w, ok := m["windowMs"]; ok {
			a, _ := gw32Number(i)
			b, _ := gw32Number(w)
			if b > a {
				return gw32Invalid("scan window must not exceed interval")
			}
		}
	}
	return nil
}
func gw32FiltersShape(m map[string]any, extended bool) error {
	if e := gw32Keys(m, "minRssi", "serviceUuids", "manufacturerIds"); e != nil {
		return e
	}
	if v, ok := m["minRssi"]; ok && !(extended && v == nil) && !gw32Range(v, -127, 20) {
		return gw32Invalid("invalid RSSI threshold")
	}
	for _, key := range []string{"serviceUuids", "manufacturerIds"} {
		v, present := m[key]
		if !present {
			continue
		}
		list, ok := v.([]any)
		if !ok || len(list) > 128 {
			return gw32Invalid("filter list requires 0..128 entries")
		}
		for _, v := range list {
			if key == "serviceUuids" {
				s, ok := v.(string)
				if !ok || gw32CanonicalUUID(s) == "" {
					return gw32Invalid("invalid Bluetooth UUID")
				}
			} else if !gw32Range(v, 0, 65535) {
				return gw32Invalid("invalid manufacturer identifier")
			}
		}
	}
	return nil
}
func gw32BeaconShape(v any) error {
	list, ok := v.([]any)
	if !ok || len(list) > 8 {
		return gw32Invalid("beaconRules requires 0..8 rules")
	}
	for _, item := range list {
		m, e := gw32Object(item)
		if e != nil {
			return e
		}
		if e = gw32Keys(m, "action", "reportKinds", "adType", "offset", "valueBase64", "maskBase64"); e != nil {
			return e
		}
		if len(m) != 6 || Str(m, "action") != "ALLOW" && Str(m, "action") != "DENY" || !gw32Range(m["adType"], 1, 255) || !gw32Range(m["offset"], 0, 253) {
			return gw32Invalid("invalid Beacon rule fields")
		}
		kinds, ok := m["reportKinds"].([]any)
		if !ok || len(kinds) < 1 || len(kinds) > 4 {
			return gw32Invalid("Beacon reportKinds requires 1..4 distinct values")
		}
		seen := map[string]bool{}
		for _, kind := range kinds {
			s, ok := kind.(string)
			if !ok || seen[s] || s != "LEGACY_ADV" && s != "LEGACY_SCAN_RSP" && s != "EXT_ADV" && s != "EXT_SCAN_RSP" {
				return gw32Invalid("invalid or duplicate Beacon report kind")
			}
			seen[s] = true
		}
		value, e := gw32Decode(m["valueBase64"], 1, 24)
		if e != nil {
			return e
		}
		mask, e := gw32Decode(m["maskBase64"], 1, 24)
		if e != nil {
			return e
		}
		nonzero := false
		for _, b := range mask {
			nonzero = nonzero || b != 0
		}
		if len(value) != len(mask) || Num(m, "offset")+int64(len(value)) > 254 || !nonzero {
			return gw32Invalid("Beacon mask must match value length, contain a set bit, and fit inside one AD field")
		}
	}
	return nil
}
func gw32AD(b []byte) error {
	for offset := 0; offset < len(b); {
		n := int(b[offset])
		if n == 0 {
			for _, v := range b[offset:] {
				if v != 0 {
					return gw32Invalid("nonzero AD data after zero padding")
				}
			}
			return nil
		}
		if n < 1 || offset+n+1 > len(b) {
			return gw32Invalid("incomplete advertising AD structure")
		}
		offset += n + 1
	}
	return nil
}
func gw32AdvertisingShape(m map[string]any) error {
	if e := gw32Keys(m, "enabled", "mode", "intervalMs", "advDataBase64", "scanResponseBase64"); e != nil {
		return e
	}
	if e := gw32Bool(m, "enabled"); e != nil {
		return e
	}
	if v, ok := m["mode"]; ok && v != "NONCONNECTABLE" && v != "SCANNABLE_NONCONNECTABLE" {
		return gw32Invalid("invalid advertising mode")
	}
	if v, ok := m["intervalMs"]; ok && !gw32Range(v, 100, 10240) {
		return gw32Invalid("advertising interval must be 100..10240 ms")
	}
	for _, k := range []string{"advDataBase64", "scanResponseBase64"} {
		if v, ok := m[k]; ok {
			b, e := gw32Decode(v, 0, 31)
			if e != nil {
				return e
			}
			if e = gw32AD(b); e != nil {
				return e
			}
		}
	}
	if Str(m, "mode") == "NONCONNECTABLE" && Str(m, "scanResponseBase64") != "" {
		return gw32Invalid("non-scannable advertising cannot have scan response data")
	}
	return nil
}

func gw32IPv4(m map[string]any) error {
	if Str(m, "mode") == "DHCP" {
		if len(m) != 1 {
			return gw32Invalid("DHCP ipv4 only permits mode")
		}
		return nil
	}
	if Str(m, "mode") != "STATIC" {
		return gw32Invalid("ipv4 mode must be DHCP or STATIC")
	}
	if e := gw32Keys(m, "mode", "address", "prefixLength", "gateway", "dns"); e != nil {
		return e
	}
	if len(m) != 5 || !gw32Range(m["prefixLength"], 1, 30) {
		return gw32Invalid("STATIC ipv4 requires address, prefixLength 1..30, gateway and DNS")
	}
	address, gateway := net.ParseIP(Str(m, "address")).To4(), net.ParseIP(Str(m, "gateway")).To4()
	valid := func(ip net.IP) bool { return ip != nil && !ip.IsUnspecified() && !ip.IsMulticast() && !ip.IsLoopback() }
	if !valid(address) || !valid(gateway) || address.Equal(gateway) {
		return gw32Invalid("invalid static IPv4 address or gateway")
	}
	mask := net.CIDRMask(int(Num(m, "prefixLength")), 32)
	network := address.Mask(mask)
	if !network.Equal(gateway.Mask(mask)) {
		return gw32Invalid("IPv4 gateway must be in the same subnet")
	}
	broadcast := append(net.IP(nil), network...)
	for i := range broadcast {
		broadcast[i] |= ^mask[i]
	}
	if address.Equal(network) || address.Equal(broadcast) || gateway.Equal(network) || gateway.Equal(broadcast) {
		return gw32Invalid("network and broadcast addresses cannot be host addresses")
	}
	dns, ok := m["dns"].([]any)
	if !ok || len(dns) < 1 || len(dns) > 2 {
		return gw32Invalid("STATIC ipv4 requires 1..2 DNS IPv4 addresses")
	}
	for _, v := range dns {
		s, ok := v.(string)
		if !ok || !valid(net.ParseIP(s).To4()) {
			return gw32Invalid("invalid DNS IPv4 address")
		}
	}
	return nil
}
func gw32NetworkShape(m map[string]any) error {
	if e := gw32Keys(m, "uplinkOrder", "ethernet", "wifiSta"); e != nil {
		return e
	}
	if v, ok := m["uplinkOrder"]; ok {
		list, ok := v.([]any)
		if !ok || len(list) < 1 || len(list) > 2 {
			return gw32Invalid("uplinkOrder requires 1..2 interfaces")
		}
		seen := map[string]bool{}
		for _, v := range list {
			s, ok := v.(string)
			if !ok || seen[s] || s != "ETHERNET" && s != "WIFI_STA" {
				return gw32Invalid("invalid or repeated uplink interface")
			}
			seen[s] = true
		}
	}
	for _, k := range []string{"ethernet", "wifiSta"} {
		value, ok := m[k]
		if !ok {
			continue
		}
		iface, e := gw32Object(value)
		if e != nil {
			return e
		}
		keys := []string{"enabled", "ipv4"}
		if k == "wifiSta" {
			keys = append(keys, "ssid", "security", "credentialRef")
		}
		if e = gw32Keys(iface, keys...); e != nil {
			return e
		}
		if e = gw32Bool(iface, "enabled"); e != nil {
			return e
		}
		if v, ok := iface["ipv4"]; ok {
			o, e := gw32Object(v)
			if e != nil {
				return e
			}
			if e = gw32IPv4(o); e != nil {
				return e
			}
		}
		if k == "wifiSta" {
			if v, ok := iface["ssid"]; ok {
				s, ok := v.(string)
				if !ok || !utf8.ValidString(s) || len([]byte(s)) > 32 {
					return gw32Invalid("Wi-Fi SSID must contain at most 32 UTF-8 bytes")
				}
			}
			if v, ok := iface["security"]; ok && v != "WPA2_PSK" && v != "WPA3_SAE" {
				return gw32Invalid("Wi-Fi requires WPA2_PSK or WPA3_SAE")
			}
			if v, ok := iface["credentialRef"]; ok {
				s, ok := v.(string)
				if !ok || !artifactIDPattern.MatchString(s) {
					return gw32Invalid("Wi-Fi credentialRef must reference a device-local credential")
				}
			}
		}
	}
	return nil
}

// Shape validation is independent of a device. Capabilities and the complete
// post-merge snapshot are checked transactionally when a task is admitted.
func validateGW32ConfigurationShape(c map[string]any) error {
	if _, ok := c["network"]; ok {
		if len(c) != 1 {
			return gw32Invalid("network changes must be submitted alone")
		}
		m, e := gw32Object(c["network"])
		if e != nil {
			return e
		}
		return gw32NetworkShape(m)
	}
	if _, ok := c["radios"]; ok {
		if len(c) != 1 {
			return gw32Invalid("radio changes must be submitted alone")
		}
		radios, e := gw32Object(c["radios"])
		if e != nil {
			return e
		}
		if len(radios) != 1 {
			return gw32Invalid("one radio target per configuration task")
		}
		for target, v := range radios {
			if target != "nrf54" && target != "esp32" {
				return gw32Invalid("unknown radio target")
			}
			m, e := gw32Object(v)
			if e != nil {
				return e
			}
			if e = gw32Keys(m, "scan", "filters", "beaconRules", "advertising"); e != nil {
				return e
			}
			if ad, ok := m["advertising"]; ok {
				if len(m) != 1 {
					return gw32Invalid("scan and advertising groups have independent revisions")
				}
				o, e := gw32Object(ad)
				if e != nil {
					return e
				}
				return gw32AdvertisingShape(o)
			}
			if v, ok := m["scan"]; ok {
				o, e := gw32Object(v)
				if e != nil {
					return e
				}
				if e = gw32ScanShape(o); e != nil {
					return e
				}
			}
			if v, ok := m["filters"]; ok {
				o, e := gw32Object(v)
				if e != nil {
					return e
				}
				if e = gw32FiltersShape(o, true); e != nil {
					return e
				}
			}
			if v, ok := m["beaconRules"]; ok {
				if e = gw32BeaconShape(v); e != nil {
					return e
				}
			}
		}
		return nil
	}
	return nil
}

func gw32Merge(base, patch map[string]any) map[string]any {
	out := map[string]any{}
	_ = json.Unmarshal([]byte(encode(base)), &out)
	for k, v := range patch {
		if child, ok := v.(map[string]any); ok && k != "ipv4" {
			out[k] = gw32Merge(Obj(out, k), child)
		} else {
			out[k] = v
		}
	}
	return out
}
func gw32Defaults() map[string]any {
	radio := func() map[string]any {
		return map[string]any{"scan": map[string]any{"enabled": false, "active": false, "intervalMs": int64(100), "windowMs": int64(50)}, "filters": map[string]any{}, "beaconRules": []any{}, "advertising": map[string]any{"enabled": false, "mode": "NONCONNECTABLE", "intervalMs": int64(100), "advDataBase64": "", "scanResponseBase64": ""}}
	}
	return map[string]any{"network": map[string]any{"uplinkOrder": []any{"ETHERNET"}, "ethernet": map[string]any{"enabled": true, "ipv4": map[string]any{"mode": "DHCP"}}, "wifiSta": map[string]any{"enabled": false}}, "radios": map[string]any{"nrf54": radio(), "esp32": radio()}}
}
func gw32ApplyConfig(base, patch map[string]any) map[string]any {
	out := gw32Merge(base, patch)
	for _, k := range []string{"scan", "filters"} {
		if v, ok := patch[k]; ok {
			radio := Obj(Obj(out, "radios"), "nrf54")
			radio[k] = gw32Merge(Obj(radio, k), v.(map[string]any))
			Obj(out, "radios")["nrf54"] = radio
		}
	}
	return out
}
func (a *App) gw32Snapshot(ctx context.Context, tx *sql.Tx, gateway string, maxRevision int64) (map[string]any, error) {
	snapshot := gw32Defaults()
	rows, e := tx.QueryContext(ctx, "SELECT desired_json FROM gateway_configs WHERE gateway_id=? AND state='verified' AND revision<=? ORDER BY revision", gateway, maxRevision)
	if e != nil {
		return nil, e
	}
	defer rows.Close()
	for rows.Next() {
		var raw string
		if e = rows.Scan(&raw); e != nil {
			return nil, e
		}
		var patch map[string]any
		if e = json.Unmarshal([]byte(raw), &patch); e != nil {
			return nil, e
		}
		snapshot = gw32ApplyConfig(snapshot, patch)
	}
	return snapshot, rows.Err()
}
func (a *App) validateGatewayConfiguration(ctx context.Context, tx *sql.Tx, g map[string]any, c map[string]any) error {
	if e := ValidateGatewayConfiguration(c); e != nil {
		return e
	}
	_, network := c["network"]
	_, radios := c["radios"]
	if !gw32Model(g) && !network && !radios {
		return nil
	}
	if e := gw32Profile(g); e != nil {
		return e
	}
	before, e := a.gw32Snapshot(ctx, tx, Str(g, "gatewayId"), 1<<62)
	if e != nil {
		return e
	}
	after := gw32ApplyConfig(before, c)
	if network {
		if _, ok := Obj(c, "network")["ethernet"]; ok {
			if e = gw32Require(g, "NETWORK_ETHERNET"); e != nil {
				return e
			}
		}
		if _, ok := Obj(c, "network")["wifiSta"]; ok {
			if e = gw32Require(g, "NETWORK_WIFI_STA"); e != nil {
				return e
			}
		}
		m := Obj(after, "network")
		if e = gw32NetworkShape(m); e != nil {
			return e
		}
		for _, name := range []string{"ethernet", "wifiSta"} {
			iface := Obj(m, name)
			if enabled, _ := iface["enabled"].(bool); enabled {
				if len(Obj(iface, "ipv4")) == 0 {
					return gw32Invalid("enabled interface requires ipv4 configuration")
				}
				if name == "wifiSta" && (Str(iface, "ssid") == "" || Str(iface, "security") == "" || Str(iface, "credentialRef") == "") {
					return gw32Invalid("enabled Wi-Fi requires SSID, security and credentialRef")
				}
			}
		}
		for _, v := range List(m, "uplinkOrder") {
			name, cap := "ethernet", "NETWORK_ETHERNET"
			if v == "WIFI_STA" {
				name = "wifiSta"
				cap = "NETWORK_WIFI_STA"
			}
			iface := Obj(m, name)
			enabled, _ := iface["enabled"].(bool)
			if !enabled {
				return gw32Invalid("uplinkOrder references a disabled interface")
			}
			if e = gw32Require(g, cap); e != nil {
				return e
			}
			if len(Obj(iface, "ipv4")) == 0 {
				return gw32Invalid("enabled uplink requires ipv4 configuration")
			}
			if name == "wifiSta" && (Str(iface, "ssid") == "" || Str(iface, "security") == "" || Str(iface, "credentialRef") == "") {
				return gw32Invalid("enabled Wi-Fi requires SSID, security and device-local credentialRef")
			}
		}
		return nil
	}
	if radios {
		for target, raw := range Obj(c, "radios") {
			patch := raw.(map[string]any)
			prefix := strings.ToUpper(target) + "_"
			merged := Obj(Obj(after, "radios"), target)
			if _, ok := patch["advertising"]; ok {
				if e = gw32Require(g, prefix+"BLE_ADV"); e != nil {
					return e
				}
				return gw32AdvertisingShape(Obj(merged, "advertising"))
			}
			if e = gw32Require(g, prefix+"BLE_SCAN"); e != nil {
				return e
			}
			if _, ok := patch["beaconRules"]; ok {
				if e = gw32Require(g, prefix+"BLE_BEACON_FILTER"); e != nil {
					return e
				}
			}
			return gw32ScanShape(Obj(merged, "scan"))
		}
	}
	if _, scan := c["scan"]; scan {
		if e = gw32Require(g, "NRF54_BLE_SCAN"); e != nil {
			return e
		}
	}
	if _, filters := c["filters"]; filters {
		if e = gw32Require(g, "NRF54_BLE_SCAN"); e != nil {
			return e
		}
	}
	if _, scan := c["scan"]; scan || c["filters"] != nil {
		if len(List(Obj(Obj(before, "radios"), "nrf54"), "beaconRules")) > 0 {
			return Problem(409, "SCAN_PROFILE_REQUIRED", "active Beacon rules require a radio scan-profile update")
		}
		return gw32ScanShape(Obj(Obj(Obj(after, "radios"), "nrf54"), "scan"))
	}
	return nil
}
