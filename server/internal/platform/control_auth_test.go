package platform

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestProvisionGatewayRejectsSharedIdentity(t *testing.T) {
	a, _ := controlFixture(t)
	ctx := context.Background()
	token := "unique-device-token-with-at-least-32-bytes"
	if e := a.ProvisionGateway(ctx, "provision_first", token); e != nil {
		t.Fatal(e)
	}
	e := a.ProvisionGateway(ctx, "provision_second", token)
	var problem *APIError
	if !errors.As(e, &problem) || problem.Code != "DEVICE_CREDENTIAL_CONFLICT" {
		t.Fatalf("shared gateway identity accepted: %v", e)
	}
	var count int
	if e = a.Management.QueryRow("SELECT count(*) FROM gateways WHERE gateway_id='provision_second'").Scan(&count); e != nil || count != 0 {
		t.Fatalf("failed provision left partial gateway %d %v", count, e)
	}
	if e = a.ProvisionGateway(ctx, "provision_first", token); e != nil {
		t.Fatalf("same gateway reprovision rejected: %v", e)
	}
	req := httptest.NewRequest(http.MethodGet, "/api/v1/gateway-ingress/commands", nil)
	req.Header.Set("Authorization", "Bearer "+token)
	id, ok := a.Gateway(req)
	if !ok || id != "provision_first" {
		t.Fatalf("credential identity changed %q %t", id, ok)
	}
}
