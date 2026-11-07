package platform

import (
	"encoding/base64"
	"encoding/binary"
	"fmt"
)

// ParserFailure is deterministic: retain the original event in quarantine.
type ParserFailure struct {
	Code   string
	Detail string
}

func (e *ParserFailure) Error() string { return e.Code + ": " + e.Detail }

type ParsedRecord struct {
	Key     string
	Data    map[string]any
	Quality []string
}

func parserCatalog() []map[string]any {
	return []map[string]any{
		{"parserId": "demo_sensor", "version": "1.0.0", "schemaVersions": []string{"temperature-1"}, "description": "Reference example: signed BE temperature/100, unsigned BE humidity/100; exactly four bytes."},
		{"parserId": "reference_sensor", "version": "1.0.0", "schemaVersions": []string{"reference-sensor-1"}, "description": "BG reference frame v1: 18 bytes, sequence, temperature, humidity, battery, flags and CRC16-CCITT-FALSE."},
		{"parserId": "ble_ad", "version": "1.0.0", "schemaVersions": []string{"ble-ad-1"}, "description": "Bluetooth advertising data structures. Preserves each AD type and raw value; does not guess vendor protocols."},
	}
}

func parserKnown(id, version, schema string) bool {
	for _, p := range parserCatalog() {
		if p["parserId"] == id && p["version"] == version {
			for _, s := range p["schemaVersions"].([]string) {
				if schema == s {
					return true
				}
			}
		}
	}
	return false
}

// ReferenceCRC16 implements CRC16-CCITT-FALSE (check("123456789") = 0x29b1).
func ReferenceCRC16(data []byte) uint16 {
	crc := uint16(0xffff)
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	return crc
}

func ParsePayload(id, version string, data []byte) ([]ParsedRecord, error) {
	bad := func(code, detail string) ([]ParsedRecord, error) { return nil, &ParserFailure{code, detail} }
	if len(data) > 65536 {
		return bad("PAYLOAD_TOO_LARGE", "raw payload exceeds 64 KiB")
	}
	if version != "1.0.0" {
		return bad("PARSER_VERSION_UNSUPPORTED", "the requested parser implementation is not installed")
	}
	quality := []string{}
	var values map[string]any
	switch id {
	case "demo_sensor":
		if len(data) != 4 {
			return bad("INVALID_LENGTH", "demo_sensor requires exactly four bytes")
		}
		t := int16(binary.BigEndian.Uint16(data[:2]))
		h := binary.BigEndian.Uint16(data[2:])
		if t < -5000 || t > 12500 || h > 10000 {
			return bad("VALUE_OUT_OF_RANGE", "temperature/humidity outside reference protocol range")
		}
		values = map[string]any{"temperature": float64(t) / 100, "temperatureUnit": "Cel", "humidity": float64(h) / 100, "humidityUnit": "%"}
	case "reference_sensor":
		if len(data) != 18 {
			return bad("INVALID_LENGTH", "reference_sensor requires exactly 18 bytes")
		}
		if data[0] != 'B' || data[1] != 'G' {
			return bad("INVALID_MAGIC", "expected BG header")
		}
		if data[2] != 1 || data[3] != 1 {
			return bad("PROTOCOL_UNSUPPORTED", "supported version=1 and type=1")
		}
		if ReferenceCRC16(data[:16]) != binary.BigEndian.Uint16(data[16:]) {
			return bad("CRC_MISMATCH", "CRC16-CCITT-FALSE check failed")
		}
		t := int16(binary.BigEndian.Uint16(data[8:10]))
		h := binary.BigEndian.Uint16(data[10:12])
		b := binary.BigEndian.Uint16(data[12:14])
		flags := binary.BigEndian.Uint16(data[14:16])
		if t < -5000 || t > 12500 || h > 10000 || b > 6000 || flags&^uint16(1) != 0 {
			return bad("VALUE_OUT_OF_RANGE", "reference field or reserved flag outside range")
		}
		if flags&1 != 0 {
			quality = append(quality, "SENSOR_FAULT")
		}
		values = map[string]any{"temperature": float64(t) / 100, "temperatureUnit": "Cel", "humidity": float64(h) / 100, "humidityUnit": "%", "batteryMillivolts": int(b), "sampleSequence": uint64(binary.BigEndian.Uint32(data[4:8])), "sensorFault": flags&1 != 0}
	case "ble_ad":
		structures := []map[string]any{}
		for pos := 0; pos < len(data); {
			n := int(data[pos])
			pos++
			if n == 0 {
				for _, b := range data[pos:] {
					if b != 0 {
						return bad("AD_TRAILING_DATA", "non-zero bytes after AD terminator")
					}
				}
				break
			}
			if pos+n > len(data) {
				return bad("AD_TRUNCATED", fmt.Sprintf("AD structure at offset %d exceeds payload", pos-1))
			}
			if len(structures) >= 64 {
				return bad("OUTPUT_LIMIT", "more than 64 AD structures")
			}
			structures = append(structures, map[string]any{"type": int(data[pos]), "dataBase64": base64.StdEncoding.EncodeToString(data[pos+1 : pos+n])})
			pos += n
		}
		values = map[string]any{"structures": structures}
	default:
		return bad("PARSER_UNKNOWN", "no installed parser with this identifier")
	}
	return []ParsedRecord{{Key: "0", Data: values, Quality: quality}}, nil
}
