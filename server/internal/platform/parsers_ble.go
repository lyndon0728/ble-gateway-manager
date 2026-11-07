package platform

import (
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"fmt"
)

// ServiceUUID retains the actual advertising field that carried a service UUID.
// Manufacturer-specific Beacon identifiers are intentionally never service UUIDs.
type ServiceUUID struct {
	UUID          string `json:"uuid"`
	WireBits      int    `json:"wireBits"`
	ADType        int    `json:"adType"`
	Source        string `json:"source"`
	ADOffset      int    `json:"adOffset"`
	UUIDOffset    int    `json:"uuidOffset"`
	SourceEventID string `json:"sourceEventId,omitempty"`
}

type bleADField struct {
	Offset int
	Type   byte
	Data   []byte
}

func readBLEAD11(payload []byte) ([]bleADField, error) {
	if len(payload) > 1650 {
		return nil, &ParserFailure{"PAYLOAD_TOO_LARGE", "BLE AD v1.1 accepts complete advertising data up to 1650 bytes"}
	}
	fields := []bleADField{}
	for offset := 0; offset < len(payload); {
		n := int(payload[offset])
		if n == 0 {
			for _, v := range payload[offset+1:] {
				if v != 0 {
					return nil, &ParserFailure{"AD_TRAILING_DATA", "non-zero data follows the AD terminator"}
				}
			}
			break
		}
		if offset+1+n > len(payload) {
			return nil, &ParserFailure{"AD_TRUNCATED", fmt.Sprintf("AD structure at offset %d exceeds payload", offset)}
		}
		if len(fields) >= 64 {
			return nil, &ParserFailure{"OUTPUT_LIMIT", "more than 64 AD structures"}
		}
		fields = append(fields, bleADField{Offset: offset, Type: payload[offset+1], Data: payload[offset+2 : offset+1+n]})
		offset += n + 1
	}
	return fields, nil
}

func normalizedServiceUUID(raw []byte) string {
	switch len(raw) {
	case 2:
		return fmt.Sprintf("0000%04x-0000-1000-8000-00805f9b34fb", binary.LittleEndian.Uint16(raw))
	case 4:
		return fmt.Sprintf("%08x-0000-1000-8000-00805f9b34fb", binary.LittleEndian.Uint32(raw))
	default:
		var bytes [16]byte
		for i := range bytes {
			bytes[i] = raw[15-i]
		}
		s := hex.EncodeToString(bytes[:])
		return s[:8] + "-" + s[8:12] + "-" + s[12:16] + "-" + s[16:20] + "-" + s[20:]
	}
}

func serviceUUIDsFromFields(fields []bleADField) ([]ServiceUUID, error) {
	result := []ServiceUUID{}
	for _, field := range fields {
		width := 0
		source := "UUID_LIST"
		switch field.Type {
		case 0x02, 0x03:
			width = 2
		case 0x04, 0x05:
			width = 4
		case 0x06, 0x07:
			width = 16
		case 0x16:
			width = 2
			source = "SERVICE_DATA"
		case 0x20:
			width = 4
			source = "SERVICE_DATA"
		case 0x21:
			width = 16
			source = "SERVICE_DATA"
		default:
			continue
		}
		size := len(field.Data)
		if source == "UUID_LIST" && size%width != 0 {
			return nil, &ParserFailure{"UUID_FIELD_LENGTH", fmt.Sprintf("AD type 0x%02x at offset %d has a non-integral %d-bit UUID list", field.Type, field.Offset, width*8)}
		}
		if source == "SERVICE_DATA" {
			if size < width {
				return nil, &ParserFailure{"UUID_FIELD_LENGTH", fmt.Sprintf("AD type 0x%02x at offset %d lacks its %d-bit service UUID prefix", field.Type, field.Offset, width*8)}
			}
			size = width
		}
		for i := 0; i < size; i += width {
			result = append(result, ServiceUUID{UUID: normalizedServiceUUID(field.Data[i : i+width]), WireBits: width * 8, ADType: int(field.Type), Source: source, ADOffset: field.Offset, UUIDOffset: field.Offset + 2 + i})
		}
	}
	return result, nil
}

// ExtractServiceUUIDs returns no candidates if any structure or UUID field is
// malformed, even when an earlier field contained a valid UUID. Callers must
// reject automatic connection evaluation on every non-nil error.
func ExtractServiceUUIDs(payload []byte) ([]ServiceUUID, error) {
	fields, err := readBLEAD11(payload)
	if err != nil {
		return nil, err
	}
	return serviceUUIDsFromFields(fields)
}

func parseBLEAD11(payload []byte) ([]ParsedRecord, error) {
	fields, err := readBLEAD11(payload)
	if err != nil {
		return nil, err
	}
	services, err := serviceUUIDsFromFields(fields)
	if err != nil {
		return nil, err
	}
	structures := []map[string]any{}
	for _, field := range fields {
		structures = append(structures, map[string]any{"type": int(field.Type), "dataBase64": base64.StdEncoding.EncodeToString(field.Data), "offset": field.Offset})
	}
	return []ParsedRecord{{Key: "0", Data: map[string]any{"structures": structures, "serviceUuids": services}, Quality: []string{}}}, nil
}

func annotateServiceUUIDSource(records []ParsedRecord, eventID string) {
	if eventID == "" {
		return
	}
	for _, record := range records {
		if services, ok := record.Data["serviceUuids"].([]ServiceUUID); ok {
			for i := range services {
				services[i].SourceEventID = eventID
			}
			record.Data["serviceUuids"] = services
		}
	}
}
