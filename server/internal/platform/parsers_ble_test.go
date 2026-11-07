package platform

import (
	"bytes"
	"encoding/hex"
	"errors"
	"testing"
)

func TestServiceUUIDWireWidthsAndProvenance(t *testing.T) {
	for _, tc := range []struct {
		name, hex, uuid, source string
		bits, adType            int
	}{
		{"incomplete16", "03 02 0F 18", "0000180f-0000-1000-8000-00805f9b34fb", "UUID_LIST", 16, 2},
		{"complete16", "03 03 0F 18", "0000180f-0000-1000-8000-00805f9b34fb", "UUID_LIST", 16, 3},
		{"incomplete32", "05 04 78 56 34 12", "12345678-0000-1000-8000-00805f9b34fb", "UUID_LIST", 32, 4},
		{"complete32", "05 05 78 56 34 12", "12345678-0000-1000-8000-00805f9b34fb", "UUID_LIST", 32, 5},
		{"incomplete128", "11 06 78 56 34 12 F0 DE BC 9A 78 56 34 12 78 56 34 12", "12345678-1234-5678-9abc-def012345678", "UUID_LIST", 128, 6},
		{"complete128", "11 07 78 56 34 12 F0 DE BC 9A 78 56 34 12 78 56 34 12", "12345678-1234-5678-9abc-def012345678", "UUID_LIST", 128, 7},
		{"service16", "06 16 0F 18 AA BB CC", "0000180f-0000-1000-8000-00805f9b34fb", "SERVICE_DATA", 16, 0x16},
		{"service32", "07 20 78 56 34 12 AA BB", "12345678-0000-1000-8000-00805f9b34fb", "SERVICE_DATA", 32, 0x20},
		{"service128", "13 21 78 56 34 12 F0 DE BC 9A 78 56 34 12 78 56 34 12 AA BB", "12345678-1234-5678-9abc-def012345678", "SERVICE_DATA", 128, 0x21},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw, err := hex.DecodeString(string(bytes.ReplaceAll([]byte(tc.hex), []byte(" "), nil)))
			if err != nil {
				t.Fatal(err)
			}
			raw = append([]byte{2, 1, 6}, raw...)
			values, err := ExtractServiceUUIDs(raw)
			if err != nil || len(values) != 1 {
				t.Fatal(values, err)
			}
			got := values[0]
			if got.UUID != tc.uuid || got.WireBits != tc.bits || got.ADType != tc.adType || got.Source != tc.source || got.ADOffset != 3 || got.UUIDOffset != 5 || got.SourceEventID != "" {
				t.Fatalf("unexpected provenance %+v", got)
			}
		})
	}
}

func TestServiceUUIDListPreservesOccurrencesAndWireSize(t *testing.T) {
	raw := []byte{5, 3, 0x0f, 0x18, 0x0f, 0x18, 17, 7, 0xfb, 0x34, 0x9b, 0x5f, 0x80, 0, 0, 0x80, 0, 0x10, 0, 0, 0x0f, 0x18, 0, 0}
	values, err := ExtractServiceUUIDs(raw)
	if err != nil || len(values) != 3 {
		t.Fatal(values, err)
	}
	for _, v := range values {
		if v.UUID != "0000180f-0000-1000-8000-00805f9b34fb" {
			t.Fatal(v)
		}
	}
	if values[0].UUIDOffset != 2 || values[1].UUIDOffset != 4 || values[2].WireBits != 128 || values[2].ADOffset != 6 {
		t.Fatal(values)
	}
	records, err := ParsePayload("ble_ad", "1.1.0", raw)
	if err != nil {
		t.Fatal(err)
	}
	annotateServiceUUIDSource(records, "event_provenance")
	services := records[0].Data["serviceUuids"].([]ServiceUUID)
	for _, v := range services {
		if v.SourceEventID != "event_provenance" {
			t.Fatal(v)
		}
	}
}

func TestMalformedServiceUUIDRejectsEntireCandidateSet(t *testing.T) {
	validPrefix := []byte{3, 3, 0x0f, 0x18}
	for _, tc := range []struct {
		name string
		tail []byte
		code string
	}{
		{"list16", []byte{2, 3, 0x0f}, "UUID_FIELD_LENGTH"},
		{"list32", []byte{4, 5, 1, 2, 3}, "UUID_FIELD_LENGTH"},
		{"list128", append([]byte{16, 7}, make([]byte, 15)...), "UUID_FIELD_LENGTH"},
		{"service16", []byte{2, 0x16, 1}, "UUID_FIELD_LENGTH"},
		{"service32", []byte{4, 0x20, 1, 2, 3}, "UUID_FIELD_LENGTH"},
		{"service128", append([]byte{16, 0x21}, make([]byte, 15)...), "UUID_FIELD_LENGTH"},
		{"truncated", []byte{4, 0xff, 1}, "AD_TRUNCATED"},
		{"bad-terminator", []byte{0, 1}, "AD_TRAILING_DATA"},
	} {
		t.Run(tc.name, func(t *testing.T) {
			raw := append(append([]byte{}, validPrefix...), tc.tail...)
			values, err := ExtractServiceUUIDs(raw)
			var pf *ParserFailure
			if values != nil || !errors.As(err, &pf) || pf.Code != tc.code {
				t.Fatal("partial candidates escaped", values, err)
			}
			records, err := ParsePayload("ble_ad", "1.1.0", raw)
			if records != nil || err == nil {
				t.Fatal("new parser did not quarantine", records, err)
			}
		})
	}
}

func TestBLEADLegacyVersionRemainsFrozen(t *testing.T) {
	malformedUUID := []byte{2, 3, 0x0f}
	old, err := ParsePayload("ble_ad", "1.0.0", malformedUUID)
	if err != nil || len(old) != 1 {
		t.Fatal("old parser behavior changed", old, err)
	}
	if _, present := old[0].Data["serviceUuids"]; present {
		t.Fatal("new fields leaked into frozen old parser")
	}
	if _, err = ParsePayload("ble_ad", "1.1.0", malformedUUID); err == nil {
		t.Fatal("new version accepted invalid UUID")
	}
	large := make([]byte, 1651)
	if _, err = ParsePayload("ble_ad", "1.0.0", large); err != nil {
		t.Fatal("old input limit changed", err)
	}
	if _, err = ExtractServiceUUIDs(large); err == nil {
		t.Fatal("new bounded parser accepted oversized AD")
	}
	if parserDigest("ble_ad", "1.0.0") != pipeHash([]string{"builtin", "0.1.0", "ble_ad", "1.0.0"}) {
		t.Fatal("server version changed immutable parser digest")
	}
	if !parserKnown("ble_ad", "1.1.0", "ble-ad-2") || parserKnown("ble_ad", "1.0.0", "ble-ad-2") {
		t.Fatal("schema version was not pinned")
	}
}

func TestManufacturerBeaconUUIDIsNotServiceUUID(t *testing.T) {
	beacon := append([]byte{21, 0xff, 0x4c, 0, 2, 0x15}, make([]byte, 16)...)
	values, err := ExtractServiceUUIDs(beacon)
	if err != nil || len(values) != 0 {
		t.Fatal("manufacturer identifier became a service", values, err)
	}
	values, err = ExtractServiceUUIDs([]byte{1, 3, 0, 0})
	if err != nil || len(values) != 0 {
		t.Fatal("empty UUID list or terminator mishandled", values, err)
	}
	fields := bytes.Repeat([]byte{1, 0x09}, 65)
	if values, err = ExtractServiceUUIDs(fields); values != nil || err == nil {
		t.Fatal("structure bound ignored")
	}
}
