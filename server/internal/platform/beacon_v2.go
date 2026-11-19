package platform

import (
	"encoding/binary"
	"encoding/hex"
	"sort"
	"strings"
)

var beaconV2Fields = []string{"name", "ibeaconUuid", "rssi", "major", "minor", "serviceUuid"}
var beaconKinds = []string{"LEGACY_ADV", "LEGACY_SCAN_RSP", "EXT_ADV", "EXT_SCAN_RSP"}

func beaconU16(b []byte, n uint16) []byte { return binary.LittleEndian.AppendUint16(b, n) }
func beaconNonzero(b []byte) bool {
	for _, v := range b {
		if v != 0 {
			return true
		}
	}
	return false
}

// BeaconV2Shape validates the complete, replace-only object, including field
// presence. Mask bits do not give permission to silently ignore unknown fields.
func BeaconV2Shape(v any) error { _, e := beaconV2Rules(v); return e }
func beaconV2Rules(v any) ([]byte, error) {
	m, e := gw32Object(v)
	if e != nil {
		return nil, e
	}
	if e = gw32Keys(m, "version", "scope", "pairWindowMs", "rules"); e != nil {
		return nil, e
	}
	pair := Str(m, "scope") == "ADV_SCAN_RSP_PAIR"
	if !gw32Range(m["version"], 2, 2) || (!pair && Str(m, "scope") != "REPORT") {
		return nil, gw32Invalid("invalid Beacon V2 version/scope")
	}
	if pair {
		if !gw32Range(m["pairWindowMs"], 500, 500) {
			return nil, gw32Invalid("PAIR window must be 500 ms")
		}
	} else if _, ok := m["pairWindowMs"]; ok {
		return nil, gw32Invalid("REPORT cannot contain pairWindowMs")
	}
	rules, ok := m["rules"].([]any)
	if !ok || len(rules) < 1 || len(rules) > 8 {
		return nil, gw32Invalid("Beacon V2 requires 1..8 rules")
	}
	out := []byte{}
	for _, raw := range rules {
		r, e := gw32Object(raw)
		if e != nil {
			return nil, e
		}
		if e = gw32Keys(r, "action", "reportKinds", "fieldMask", "name", "ibeaconUuid", "rssi", "major", "minor", "serviceUuid"); e != nil {
			return nil, e
		}
		action := Str(r, "action")
		if action != "ALLOW" && action != "DENY" {
			return nil, gw32Invalid("invalid rule action")
		}
		if !gw32Range(r["fieldMask"], 1, 63) {
			return nil, gw32Invalid("invalid fieldMask")
		}
		fm := Num(r, "fieldMask")
		kinds, ok := r["reportKinds"].([]any)
		if !ok || len(kinds) < 1 || len(kinds) > 4 {
			return nil, gw32Invalid("invalid reportKinds")
		}
		km := byte(0)
		for _, kind := range kinds {
			found := false
			for i, name := range beaconKinds {
				if kind == name {
					bit := byte(1 << i)
					if km&bit != 0 {
						return nil, gw32Invalid("duplicate report kind")
					}
					km |= bit
					found = true
				}
			}
			if !found {
				return nil, gw32Invalid("unknown report kind")
			}
		}
		if pair && km != 1 {
			return nil, gw32Invalid("PAIR rules require LEGACY_ADV")
		}
		b := []byte{0, km}
		if action == "ALLOW" {
			b[0] = 1
		}
		b = beaconU16(b, uint16(fm))
		for i, key := range beaconV2Fields {
			raw, present := r[key]
			enabled := fm&(1<<i) != 0
			if enabled != present {
				return nil, gw32Invalid("fieldMask and predicate presence differ")
			}
			if !enabled {
				continue
			}
			p, e := gw32Object(raw)
			if e != nil {
				return nil, e
			}
			switch key {
			case "name":
				if e = gw32Keys(p, "mode", "source", "valueBase64", "maskBase64"); e != nil {
					return nil, e
				}
				mode := Str(p, "mode")
				source := Str(p, "source")
				if mode != "EXACT" && mode != "PREFIX" {
					return nil, gw32Invalid("invalid name mode")
				}
				sm := map[string]byte{"COMPLETE": 0, "SHORTENED": 1, "ANY": 2}
				sv, ok := sm[source]
				if !ok {
					return nil, gw32Invalid("invalid name source")
				}
				value, e := gw32Decode(p["valueBase64"], 1, 29)
				if e != nil {
					return nil, e
				}
				mask, e := gw32Decode(p["maskBase64"], 1, 29)
				if e != nil {
					return nil, e
				}
				if len(value) != len(mask) || !beaconNonzero(mask) {
					return nil, gw32Invalid("invalid name mask")
				}
				mv := byte(0)
				if mode == "PREFIX" {
					mv = 1
				}
				b = append(b, mv, sv, byte(len(value)))
				b = append(b, value...)
				b = append(b, mask...)
			case "ibeaconUuid", "serviceUuid":
				if e = gw32Keys(p, "value", "maskHex"); e != nil {
					return nil, e
				}
				value := Str(p, "value")
				mask := Str(p, "maskHex")
				if len(value) != 36 || gw32CanonicalUUID(value) != value || len(mask) != 32 || strings.ToLower(mask) != mask {
					return nil, gw32Invalid("UUID/mask must be canonical")
				}
				vb, ve := hex.DecodeString(strings.ReplaceAll(value, "-", ""))
				mb, me := hex.DecodeString(mask)
				if ve != nil || me != nil || !beaconNonzero(mb) {
					return nil, gw32Invalid("invalid UUID mask")
				}
				b = append(b, vb...)
				b = append(b, mb...)
			case "rssi":
				switch Str(p, "mode") {
				case "RANGE":
					if e = gw32Keys(p, "mode", "minDbm", "maxDbm"); e != nil {
						return nil, e
					}
					if !gw32Range(p["minDbm"], -127, 20) || !gw32Range(p["maxDbm"], -127, 20) || Num(p, "minDbm") > Num(p, "maxDbm") {
						return nil, gw32Invalid("invalid RSSI range")
					}
					b = append(b, 0, byte(int8(Num(p, "minDbm"))), byte(int8(Num(p, "maxDbm"))))
				case "BITS":
					if e = gw32Keys(p, "mode", "valueDbm", "mask"); e != nil {
						return nil, e
					}
					if !gw32Range(p["valueDbm"], -127, 20) || !gw32Range(p["mask"], 1, 255) {
						return nil, gw32Invalid("invalid RSSI mask")
					}
					b = append(b, 1, byte(int8(Num(p, "valueDbm"))), byte(Num(p, "mask")))
				default:
					return nil, gw32Invalid("invalid RSSI mode")
				}
			case "major", "minor":
				if e = gw32Keys(p, "value", "mask"); e != nil {
					return nil, e
				}
				if !gw32Range(p["value"], 0, 65535) || !gw32Range(p["mask"], 1, 65535) {
					return nil, gw32Invalid("invalid major/minor mask")
				}
				b = beaconU16(b, uint16(Num(p, "value")))
				b = beaconU16(b, uint16(Num(p, "mask")))
			}
		}
		out = beaconU16(out, uint16(len(b)))
		out = append(out, b...)
	}
	return out, nil
}

// CompileScanProfileV2 returns the exact UART body, including the legacy
// profile. It never truncates a list to fit a frame. Revision is supplied by
// the transaction owner, not incremented by the compiler.
func CompileScanProfileV2(radio map[string]any, revision uint64) ([]byte, error) {
	scan, filters := Obj(radio, "scan"), Obj(radio, "filters")
	if e := gw32ScanShape(scan); e != nil {
		return nil, e
	}
	if e := gw32FiltersShape(filters, true); e != nil {
		return nil, e
	}
	if !gw32Range(scan["intervalMs"], 10, 10240) || !gw32Range(scan["windowMs"], 10, 10240) {
		return nil, gw32Invalid("complete scan profile required")
	}
	flags := byte(0)
	if scan["enabled"] == true {
		flags |= 1
	}
	if scan["active"] == true {
		flags |= 2
	}
	c := binary.LittleEndian.AppendUint64(nil, revision)
	c = append(c, flags)
	c = beaconU16(c, uint16((Num(scan, "intervalMs")*8+2)/5))
	c = beaconU16(c, uint16((Num(scan, "windowMs")*8+2)/5))
	rssi := int8(-128)
	if v := filters["minRssi"]; v != nil {
		rssi = int8(Num(filters, "minRssi"))
	}
	c = append(c, byte(rssi))
	uuids := map[string]bool{}
	for _, v := range List(filters, "serviceUuids") {
		uuids[gw32CanonicalUUID(v.(string))] = true
	}
	us := make([]string, 0, len(uuids))
	for u := range uuids {
		us = append(us, u)
	}
	sort.Strings(us)
	c = append(c, byte(len(us)))
	for _, u := range us {
		b, _ := hex.DecodeString(strings.ReplaceAll(u, "-", ""))
		c = append(c, b...)
	}
	companies := map[int64]bool{}
	for _, v := range List(filters, "manufacturerIds") {
		n, _ := gw32Number(v)
		companies[n] = true
	}
	cs := make([]int, 0, len(companies))
	for n := range companies {
		cs = append(cs, int(n))
	}
	sort.Ints(cs)
	c = append(c, byte(len(cs)))
	for _, n := range cs {
		c = beaconU16(c, uint16(n))
	}
	legacy := beaconU16(nil, uint16(len(c)))
	legacy = append(legacy, c...)
	old := List(radio, "beaconRules")
	if e := gw32BeaconShape(old); e != nil {
		return nil, e
	}
	legacy = append(legacy, byte(len(old)))
	for _, raw := range old {
		r := raw.(map[string]any)
		action := byte(0)
		if Str(r, "action") == "ALLOW" {
			action = 1
		}
		km := byte(0)
		for _, v := range List(r, "reportKinds") {
			for i, k := range beaconKinds {
				if v == k {
					km |= 1 << i
				}
			}
		}
		legacy = append(legacy, action, km, byte(Num(r, "adType")))
		legacy = beaconU16(legacy, uint16(Num(r, "offset")))
		v, _ := gw32Decode(r["valueBase64"], 1, 24)
		m, _ := gw32Decode(r["maskBase64"], 1, 24)
		legacy = append(legacy, byte(len(v)))
		legacy = append(legacy, v...)
		legacy = append(legacy, m...)
	}
	engine, scope, window, count := byte(0), byte(0), uint16(0), byte(0)
	var rules []byte
	if v := radio["beaconFilterV2"]; v != nil {
		if len(old) != 0 {
			return nil, gw32Invalid("legacy and V2 rules cannot both be active")
		}
		var e error
		rules, e = beaconV2Rules(v)
		if e != nil {
			return nil, e
		}
		m := v.(map[string]any)
		engine = 1
		count = byte(len(List(m, "rules")))
		if Str(m, "scope") == "ADV_SCAN_RSP_PAIR" {
			if scan["active"] != true {
				return nil, gw32Invalid("PAIR requires active scanning")
			}
			scope = 1
			window = 500
		}
	}
	out := []byte{2, engine}
	out = beaconU16(out, uint16(len(legacy)))
	out = append(out, legacy...)
	out = append(out, scope)
	out = beaconU16(out, window)
	out = append(out, count)
	out = append(out, rules...)
	if len(out) > 3070 {
		return nil, gw32Invalid("complete scan profile exceeds 3070 byte UART budget")
	}
	return out, nil
}
