package simkit

import (
	"context"
	"encoding/json"
	"path/filepath"
	"testing"
)

func event(id string, revision int64) map[string]any {
	return map[string]any{"deliveryId": id, "outputId": "o_" + id, "namespaceId": "ns", "gatewayId": "gw", "sourceEventId": "boot:1", "recordKey": "0", "processingRevision": revision, "targetId": "t", "targetVersion": "1", "data": map[string]any{"revision": revision}}
}
func batch(e map[string]any) map[string]any {
	return map[string]any{"batchId": "b", "events": []any{e}}
}
func TestCustomerCustodySurvivesRestartAndDuplicate(t *testing.T) {
	p := filepath.Join(t.TempDir(), "inbox.db")
	c, err := OpenCustomer(p, "t", "1")
	if err != nil {
		t.Fatal(err)
	}
	ack, err := c.Receive(context.Background(), batch(event("d1", 1)))
	if err != nil {
		t.Fatal(err)
	}
	if ack["durability"] != "CUSTOMER_BUSINESS_PERSISTED" {
		t.Fatal(ack)
	}
	c.DB.Close()
	c, err = OpenCustomer(p, "t", "1")
	if err != nil {
		t.Fatal(err)
	}
	defer c.DB.Close()
	ack, err = c.Receive(context.Background(), batch(event("d1", 1)))
	if err != nil {
		t.Fatal(err)
	}
	if ack["items"].([]map[string]any)[0]["status"] != "DUPLICATE" {
		t.Fatal(ack)
	}
	stats, _ := c.Stats(context.Background())
	if stats["deliveries"] != int64(1) || stats["businessRecords"] != int64(1) {
		t.Fatal(stats)
	}
}
func TestCustomerOldCorrectionCannotOverwriteNew(t *testing.T) {
	c, err := OpenCustomer(filepath.Join(t.TempDir(), "db"), "t", "1")
	if err != nil {
		t.Fatal(err)
	}
	defer c.DB.Close()
	for _, e := range []map[string]any{event("d2", 2), event("d1", 1)} {
		if _, err = c.Receive(context.Background(), batch(e)); err != nil {
			t.Fatal(err)
		}
	}
	var rev int64
	if err = c.DB.QueryRow("SELECT revision FROM records").Scan(&rev); err != nil {
		t.Fatal(err)
	}
	if rev != 2 {
		t.Fatal(rev)
	}
	conflict := event("d2", 3)
	ack, err := c.Receive(context.Background(), batch(conflict))
	if err != nil {
		t.Fatal(err)
	}
	if ack["items"].([]map[string]any)[0]["status"] != "REJECTED" {
		t.Fatal(ack)
	}
}
func TestCustomerMixedTargetsRollBackBatch(t *testing.T) {
	c, err := OpenCustomer(filepath.Join(t.TempDir(), "db"), "t", "1")
	if err != nil {
		t.Fatal(err)
	}
	defer c.DB.Close()
	bad := event("bad", 1)
	bad["targetId"] = "other"
	_, err = c.Receive(context.Background(), map[string]any{"batchId": "b", "events": []any{event("good", 1), bad}})
	if err == nil {
		t.Fatal("accepted wrong target")
	}
	var n int
	if err = c.DB.QueryRow("SELECT count(*) FROM deliveries").Scan(&n); err != nil {
		t.Fatal(err)
	}
	if n != 0 {
		t.Fatal(n)
	}
	// Canonical hashes are independent of map iteration order.
	var a, b map[string]any
	_ = json.Unmarshal([]byte(`{"x":1,"y":2}`), &a)
	_ = json.Unmarshal([]byte(`{"y":2,"x":1}`), &b)
	if Hash(a) != Hash(b) {
		t.Fatal("unstable digest")
	}
}
