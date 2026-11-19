package platform

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"strings"
)

type beaconAD struct {
	kind byte
	data []byte
}

func beaconADs(raw []byte) ([]beaconAD, bool) {
	out := []beaconAD{}
	for p := 0; p < len(raw); {
		n := int(raw[p])
		if n == 0 {
			return out, !beaconNonzero(raw[p:])
		}
		if n > len(raw)-p-1 {
			return nil, false
		}
		out = append(out, beaconAD{raw[p+1], raw[p+2 : p+n+1]})
		p += n + 1
	}
	return out, true
}
func beaconMasked(actual, value, mask []byte) bool {
	if len(actual) < len(value) || len(value) != len(mask) {
		return false
	}
	for i, v := range value {
		if actual[i]&mask[i] != v&mask[i] {
			return false
		}
	}
	return true
}
func beaconUUIDBytes(p map[string]any) ([]byte, []byte) {
	v, _ := hex.DecodeString(strings.ReplaceAll(Str(p, "value"), "-", ""))
	m, _ := hex.DecodeString(Str(p, "maskHex"))
	return v, m
}
func beaconServiceMatch(ads []beaconAD, value, mask []byte) bool {
	for _, ad := range ads {
		width := 0
		switch ad.kind {
		case 2, 3, 0x16:
			width = 2
		case 4, 5, 0x20:
			width = 4
		case 6, 7, 0x21:
			width = 16
		}
		if width == 0 {
			continue
		}
		single := ad.kind == 0x16 || ad.kind == 0x20 || ad.kind == 0x21
		if len(ad.data) < width || !single && len(ad.data)%width != 0 {
			continue
		}
		for off := 0; off+width <= len(ad.data); off += width {
			u, _ := hex.DecodeString("0000000000001000800000805f9b34fb")
			start := 0
			if width == 2 {
				start = 2
			}
			for i := 0; i < width; i++ {
				u[start+i] = ad.data[off+width-1-i]
			}
			if beaconMasked(u, value, mask) {
				return true
			}
			if single {
				break
			}
		}
	}
	return false
}
func beaconName(ads []beaconAD, source string) ([]byte, bool) {
	read := func(kind byte) ([]byte, bool) {
		var found []byte
		exists := false
		for _, ad := range ads {
			if ad.kind != kind {
				continue
			}
			if exists && !bytes.Equal(found, ad.data) {
				return nil, false
			}
			found = ad.data
			exists = true
		}
		return found, true
	}
	if source == "SHORTENED" {
		return read(8)
	}
	v, ok := read(9)
	if !ok || v != nil || source == "COMPLETE" {
		return v, ok
	}
	return read(8)
}
func beaconIBeacon(ads []beaconAD, r map[string]any) bool {
	for _, ad := range ads {
		if ad.kind != 255 || len(ad.data) != 25 || !bytes.Equal(ad.data[:4], []byte{0x4c, 0, 2, 0x15}) {
			continue
		}
		if p := Obj(r, "ibeaconUuid"); len(p) > 0 {
			v, m := beaconUUIDBytes(p)
			if !beaconMasked(ad.data[4:20], v, m) {
				continue
			}
		}
		match := true
		for i, key := range []string{"major", "minor"} {
			if p := Obj(r, key); len(p) > 0 {
				value := int64(binary.BigEndian.Uint16(ad.data[20+i*2:]))
				if value&Num(p, "mask") != Num(p, "value")&Num(p, "mask") {
					match = false
				}
			}
		}
		if match {
			return true
		}
	}
	return false
}

// Preview evaluates only the supplied raw sample. Pairing is an explicit test
// scenario; it never borrows names from the platform's historical device data.
func BeaconV2Preview(profile, sample map[string]any) map[string]any {
	result := func(passed bool, reason string, index int) map[string]any {
		return map[string]any{"passed": passed, "reason": reason, "matchedRuleIndex": index}
	}
	config := Obj(profile, "beaconFilterV2")
	if len(config) == 0 {
		return result(false, "V2_NOT_ENABLED", -1)
	}
	pair := Str(config, "scope") == "ADV_SCAN_RSP_PAIR"
	kind := Str(sample, "reportKind")
	if pair && kind != "LEGACY_ADV" {
		return result(false, "UNSUPPORTED_CONTEXT", -1)
	}
	raw, e := gw32Decode(sample["payloadBase64"], 0, 1650)
	if e != nil {
		return result(false, "INVALID_SAMPLE", -1)
	}
	ads, ok := beaconADs(raw)
	if !ok {
		return result(false, "MALFORMED_AD", -1)
	}
	if pair {
		if sample["responseBase64"] == nil {
			return result(false, "PAIR_TIMEOUT", -1)
		}
		if Num(sample, "candidateCount") != 1 {
			return result(false, "PAIR_AMBIGUOUS", -1)
		}
		if Num(sample, "responseDelayMs") > 500 {
			return result(false, "PAIR_TIMEOUT", -1)
		}
		response, e := gw32Decode(sample["responseBase64"], 0, 31)
		if e != nil {
			return result(false, "INVALID_SAMPLE", -1)
		}
		other, ok := beaconADs(response)
		if !ok {
			return result(false, "MALFORMED_AD", -1)
		}
		ads = append(ads, other...)
	} else if sample["responseBase64"] != nil {
		return result(false, "UNSUPPORTED_CONTEXT", -1)
	}
	filters := Obj(profile, "filters")
	rules := List(config, "rules")
	needsRSSI := filters["minRssi"] != nil
	for _, item := range rules {
		r := item.(map[string]any)
		if p := Obj(r, "name"); len(p) > 0 {
			if _, ok = beaconName(ads, Str(p, "source")); !ok {
				return result(false, "UNSUPPORTED_CONTEXT", -1)
			}
		}
		if r["rssi"] != nil {
			needsRSSI = true
		}
	}
	rssi := Num(sample, "rssiDbm")
	if needsRSSI && !gw32Range(sample["rssiDbm"], -127, 20) {
		return result(false, "UNKNOWN_RSSI", -1)
	}
	if Obj(profile, "scan")["enabled"] != true || filters["minRssi"] != nil && rssi < Num(filters, "minRssi") {
		return result(false, "BASE_FILTER", -1)
	}
	if list := List(filters, "serviceUuids"); len(list) > 0 {
		found := false
		for _, v := range list {
			u, _ := hex.DecodeString(strings.ReplaceAll(gw32CanonicalUUID(v.(string)), "-", ""))
			found = found || beaconServiceMatch(ads, u, bytes.Repeat([]byte{255}, 16))
		}
		if !found {
			return result(false, "BASE_FILTER", -1)
		}
	}
	if list := List(filters, "manufacturerIds"); len(list) > 0 {
		found := false
		for _, ad := range ads {
			if ad.kind != 255 || len(ad.data) < 2 {
				continue
			}
			for _, v := range list {
				n, _ := gw32Number(v)
				found = found || int64(binary.LittleEndian.Uint16(ad.data)) == n
			}
		}
		if !found {
			return result(false, "BASE_FILTER", -1)
		}
	}
	hasAllow := false
	for index, item := range rules {
		r := item.(map[string]any)
		hasAllow = hasAllow || Str(r, "action") == "ALLOW"
		kindMatch := false
		for _, v := range List(r, "reportKinds") {
			kindMatch = kindMatch || v == kind
		}
		if !kindMatch {
			continue
		}
		if p := Obj(r, "name"); len(p) > 0 {
			name, _ := beaconName(ads, Str(p, "source"))
			value, _ := gw32Decode(p["valueBase64"], 1, 29)
			mask, _ := gw32Decode(p["maskBase64"], 1, 29)
			if name == nil || Str(p, "mode") == "EXACT" && len(name) != len(value) || !beaconMasked(name, value, mask) {
				continue
			}
		}
		if Num(r, "fieldMask")&26 != 0 && !beaconIBeacon(ads, r) {
			continue
		}
		if p := Obj(r, "rssi"); len(p) > 0 {
			if Str(p, "mode") == "RANGE" {
				if rssi < Num(p, "minDbm") || rssi > Num(p, "maxDbm") {
					continue
				}
			} else if byte(int8(rssi))&byte(Num(p, "mask")) != byte(int8(Num(p, "valueDbm")))&byte(Num(p, "mask")) {
				continue
			}
		}
		if p := Obj(r, "serviceUuid"); len(p) > 0 {
			v, m := beaconUUIDBytes(p)
			if !beaconServiceMatch(ads, v, m) {
				continue
			}
		}
		return result(Str(r, "action") == "ALLOW", Str(r, "action"), index)
	}
	return result(!hasAllow, "DEFAULT", -1)
}
