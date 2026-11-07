package platform

import (
	"context"
	"fmt"
	"strings"
	"testing"
)

// This measures only durable T1 ingest (WAL synchronous=FULL), not a hardware,
// HTTP, parser, TLS or customer-delivery capacity promise. Each batch has 100
// distinct 18-byte reference frames and new durable identities.
func BenchmarkDurableIngress100(b *testing.B) {
	a, e := New(Config{DataDir: b.TempDir(), AllowInsecureDev: true, MaxQueueBytes: 4 << 30})
	if e != nil {
		b.Fatal(e)
	}
	defer a.Close()
	ctx := context.Background()
	if e = a.Bootstrap(ctx, "bench", "benchmark-local-only-password"); e != nil {
		b.Fatal(e)
	}
	if e = a.ProvisionGateway(ctx, "gw_test", strings.Repeat("b", 43)); e != nil {
		b.Fatal(e)
	}
	if e = a.ensurePipelineDefault(ctx); e != nil {
		b.Fatal(e)
	}
	b.ReportAllocs()
	b.SetBytes(1800)
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		ids := make([]string, 100)
		for j := range ids {
			ids[j] = fmt.Sprintf("event_%d_%d", i, j)
		}
		ack, e := a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, ids...))
		if e != nil {
			b.Fatal(e)
		}
		for _, v := range ack["items"].([]pipeMap) {
			if ps(v, "status") != "ACCEPTED" {
				b.Fatalf("ingress rejected: %v", v)
			}
		}
	}
	b.ReportMetric(float64(b.N*100)/b.Elapsed().Seconds(), "events/s")
}
