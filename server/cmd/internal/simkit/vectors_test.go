package simkit

import (
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"hash/crc32"
	"os"
	"testing"
)

// Tests only the published frame examples. There is deliberately no UART
// receiver or BLE_ACK generation in the server-side simulator.
func TestPublishedProtocolVectors(t *testing.T) {
	raw, e := os.ReadFile("testdata/gw32-protocol-v11.json")
	if e != nil {
		t.Fatal(e)
	}
	var fixture struct {
		ConnectionReferences []struct {
			Radio, ControllerBootID, ConnectionRef string
			ConnectionNumber                       uint32
		}
		UARTFrames []struct {
			Type      byte
			Name, Hex string
		}
	}
	if e = json.Unmarshal(raw, &fixture); e != nil {
		t.Fatal(e)
	}
	for _, v := range fixture.ConnectionReferences {
		ref, e := ConnectionReference(v.Radio, v.ControllerBootID, v.ConnectionNumber)
		if e != nil || ref != v.ConnectionRef {
			t.Fatalf("connection reference vector mismatch: %v %s", e, ref)
		}
	}
	for _, v := range fixture.UARTFrames {
		frame, e := hex.DecodeString(v.Hex)
		if e != nil {
			t.Fatal(e)
		}
		if frame[0] != 0 || frame[len(frame)-1] != 0 {
			t.Fatal("missing UART frame delimiters")
		}
		encoded := frame[1 : len(frame)-1]
		decoded := []byte{}
		for len(encoded) > 0 {
			count := int(encoded[0])
			if count == 0 || count > len(encoded) {
				t.Fatal("invalid COBS vector")
			}
			decoded = append(decoded, encoded[1:count]...)
			encoded = encoded[count:]
			if count < 255 && len(encoded) > 0 {
				decoded = append(decoded, 0)
			}
		}
		if len(decoded) < 22 || !bytes.Equal(decoded[:3], []byte{'G', 'W', 1}) || decoded[3] != v.Type {
			t.Fatalf("bad %s header", v.Name)
		}
		if int(binary.LittleEndian.Uint16(decoded[4:6])) != len(decoded)-22 {
			t.Fatal("UART payload length mismatch")
		}
		want := binary.LittleEndian.Uint32(decoded[len(decoded)-4:])
		got := crc32.Checksum(decoded[:len(decoded)-4], crc32.MakeTable(crc32.Castagnoli))
		if got != want {
			t.Fatalf("%s CRC32C mismatch: %08x != %08x", v.Name, got, want)
		}
	}
}
