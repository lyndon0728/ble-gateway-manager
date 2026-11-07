package simkit

import (
	"context"
	"encoding/hex"
	"encoding/json"
	"path/filepath"
	"testing"
	"time"
)

func TestGatewayRestartAndStrictACK(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "gateway.db")
	g, e := OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	if e = g.Enqueue(ctx, 2); e != nil {
		t.Fatal(e)
	}
	before, e := g.Batch(ctx)
	if e != nil {
		t.Fatal(e)
	}
	id := Str(before["events"].([]any)[0].(map[string]any), "eventId")
	g.DB.Close()
	g, e = OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	batch, _ := g.Batch(ctx)
	if Str(batch["events"].([]any)[0].(map[string]any), "eventId") != id {
		t.Fatal("event identity changed on restart")
	}
	ack := map[string]any{"schemaVersion": "1.0", "namespaceId": "ns_test", "gatewayId": "wrong", "batchId": batch["batchId"], "durability": "RAW_PERSISTED", "items": []any{map[string]any{"eventId": id, "status": "ACCEPTED"}}}
	if g.Ack(ctx, batch, ack) == nil {
		t.Fatal("accepted mismatched ACK")
	}
	ack["gatewayId"] = "gw_test"
	if e = g.Ack(ctx, batch, ack); e != nil {
		t.Fatal(e)
	}
	pending, _ := g.Batch(ctx)
	if len(pending["events"].([]any)) != 1 {
		t.Fatal("partial ACK removed other event")
	}
}

func TestAcceptedCommandResumesAfterRestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "gateway.db")
	g, e := OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	cmd := map[string]any{"namespaceId": "ns_test", "gatewayId": "gw_test", "taskId": "task_pending", "requestDigest": "digest_pending", "type": "DIAGNOSTICS", "expiresAt": time.Now().Add(time.Hour).UTC().Format(time.RFC3339Nano)}
	raw, _ := json.Marshal(cmd)
	if _, e = g.DB.Exec("INSERT INTO journal(task_id,digest,command_json,state) VALUES(?,?,?,'ACCEPTED')", "task_pending", "digest_pending", string(raw)); e != nil {
		t.Fatal(e)
	}
	g.DB.Close()
	g, e = OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	var state string
	var count int
	if e = g.DB.QueryRow("SELECT state,execution_count FROM journal WHERE task_id='task_pending'").Scan(&state, &count); e != nil || state != "SUCCEEDED" || count != 1 {
		t.Fatalf("accepted task not resumed once: %s %d %v", state, count, e)
	}
}
func TestTaskReplayAndInterruptedIntent(t *testing.T) {
	ctx := context.Background()
	path := filepath.Join(t.TempDir(), "gateway.db")
	g, e := OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	cmd := map[string]any{"namespaceId": "ns_test", "gatewayId": "gw_test", "taskId": "task_1", "requestDigest": "digest1", "type": "REBOOT", "expiresAt": time.Now().Add(time.Hour).UTC().Format(time.RFC3339Nano)}
	if e = g.Command(ctx, cmd); e != nil {
		t.Fatal(e)
	}
	if e = g.Command(ctx, cmd); e != nil {
		t.Fatal(e)
	}
	var count int
	if e = g.DB.QueryRow("SELECT execution_count FROM journal WHERE task_id='task_1'").Scan(&count); e != nil || count != 1 {
		t.Fatalf("duplicate side effect: %d %v", count, e)
	}
	if _, e = g.DB.Exec("UPDATE journal SET state='EXECUTING' WHERE task_id='task_1'"); e != nil {
		t.Fatal(e)
	}
	g.DB.Close()
	g, e = OpenGateway(path, "ns_test", "gw_test")
	if e != nil {
		t.Fatal(e)
	}
	defer g.DB.Close()
	var state string
	if e = g.DB.QueryRow("SELECT state FROM journal WHERE task_id='task_1'").Scan(&state); e != nil || state != "UNKNOWN" {
		t.Fatalf("interrupted intent = %s %v", state, e)
	}
	if e = g.Command(ctx, cmd); e != nil {
		t.Fatal(e)
	}
	if e = g.DB.QueryRow("SELECT execution_count FROM journal WHERE task_id='task_1'").Scan(&count); e != nil || count != 1 {
		t.Fatal("interrupted command reran")
	}
}
func TestReferencePacket(t *testing.T) {
	b := ReferencePacket(1)
	if len(b) != 18 || hex.EncodeToString(b[:16]) != "424701010000000109c413880bb80000" {
		t.Fatalf("unexpected reference bytes: %x", b)
	}
	crc := uint16(0xffff)
	for _, v := range []byte("123456789") {
		crc ^= uint16(v) << 8
		for range 8 {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc <<= 1
			}
		}
	}
	if crc != 0x29b1 {
		t.Fatal("CRC implementation does not match CCITT-FALSE vector")
	}
}
