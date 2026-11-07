package platform

import (
	"encoding/binary"
	"errors"
	"testing"
)

func referenceFrame() []byte {
	b := []byte{'B', 'G', 1, 1, 0, 0, 0, 7, 0x09, 0xc4, 0x13, 0x88, 0x0c, 0xe4, 0, 0, 0, 0}
	binary.BigEndian.PutUint16(b[16:], ReferenceCRC16(b[:16]))
	return b
}
func TestReferenceParsersValidateBinaryBounds(t *testing.T) {
	if ReferenceCRC16([]byte("123456789")) != 0x29b1 {
		t.Fatal("CRC check vector")
	}
	frame := referenceFrame()
	records, err := ParsePayload("reference_sensor", "1.0.0", frame)
	if err != nil || records[0].Data["temperature"] != float64(25) || records[0].Data["humidity"] != float64(50) {
		t.Fatalf("parse %v %v", records, err)
	}
	for _, tt := range []struct {
		name, id, version string
		data              []byte
		code              string
	}{{"truncated", "reference_sensor", "1.0.0", frame[:17], "INVALID_LENGTH"}, {"crc", "reference_sensor", "1.0.0", append(append([]byte{}, frame[:17]...), frame[17]^1), "CRC_MISMATCH"}, {"unknown", "vendor_guessed", "1.0.0", frame, "PARSER_UNKNOWN"}, {"unknown-version", "reference_sensor", "2", frame, "PARSER_VERSION_UNSUPPORTED"}, {"ad-truncated", "ble_ad", "1.0.0", []byte{3, 0xff, 1}, "AD_TRUNCATED"}, {"ad-tail", "ble_ad", "1.0.0", []byte{0, 1}, "AD_TRAILING_DATA"}} {
		t.Run(tt.name, func(t *testing.T) {
			_, err := ParsePayload(tt.id, tt.version, tt.data)
			var failure *ParserFailure
			if !errors.As(err, &failure) || failure.Code != tt.code {
				t.Fatalf("got %v", err)
			}
		})
	}
	if _, err = ParsePayload("ble_ad", "1.0.0", []byte{2, 1, 6, 0, 0}); err != nil {
		t.Fatal(err)
	}
	r, err := ParsePayload("demo_sensor", "1.0.0", []byte{9, 0xc4, 3, 0xe8})
	if err != nil || r[0].Data["humidity"] != float64(10) {
		t.Fatal(r, err)
	}
}
