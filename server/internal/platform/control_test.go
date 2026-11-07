package platform

import (
	"context"
	"database/sql"
	"encoding/json"
	"testing"
	"time"
)

func controlFixture(t *testing.T) (*App, string) {
	t.Helper()
	a, e := New(Config{DataDir: t.TempDir(), AllowInsecureDev: true, NamespaceID: "ns_test", InstanceID: "inst_test"})
	if e != nil {
		t.Fatal(e)
	}
	t.Cleanup(func() { a.Close() })
	if e = a.Bootstrap(context.Background(), "admin", "test-password-strong"); e != nil {
		t.Fatal(e)
	}
	var actor string
	if e = a.Management.QueryRow("SELECT principal_id FROM principals WHERE name='admin'").Scan(&actor); e != nil {
		t.Fatal(e)
	}
	for _, id := range []string{"gw_test", "gw_other"} {
		_, e = a.Management.Exec("INSERT INTO gateways(gateway_id,serial_number,scope_id,model,hardware_version,firmware_version,lifecycle,capabilities_json,created_at_ms) VALUES(?,?,'scope_default_group','GW','1','1.0','active',?,?)", id, id, `["CONFIGURE","REBOOT","DIAGNOSTICS","FIRMWARE_UPGRADE"]`, Now())
		if e != nil {
			t.Fatal(e)
		}
		_, e = a.Management.Exec("INSERT INTO gateway_configs(config_id,gateway_id,revision,desired_json,content_hash,state,created_by,created_at_ms) VALUES(?,?,1,'{}',?,'desired',?,?)", ID("cfg"), id, Hash(map[string]any{}), actor, Now())
		if e != nil {
			t.Fatal(e)
		}
	}
	return a, actor
}
func controlNewTask(t *testing.T, a *App, actor, key, kind string, params map[string]any) map[string]any {
	t.Helper()
	req := map[string]any{"gatewayId": "gw_test", "type": kind, "parameters": params, "expiresAt": Timestamp(Now() + 60000)}
	_, v, e := a.DoIdempotent(context.Background(), actor, "task.create", key, req, func(tx *sql.Tx) (int, string, any, error) {
		return a.newTask(context.Background(), tx, actor, key, req)
	})
	if e != nil {
		t.Fatal(e)
	}
	return v.(map[string]any)
}
func controlBatch(a *App, gateway string, result map[string]any) map[string]any {
	return map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": gateway, "results": []any{result}}
}
func controlResult(task, id string, sequence int64, state, verification string, details map[string]any) map[string]any {
	return map[string]any{"taskId": task, "resultId": id, "resultSequence": sequence, "state": state, "verification": verification, "observedAt": Timestamp(Now()), "details": details}
}
func controlAckStatus(v map[string]any) string {
	return Str(List(v, "items")[0].(map[string]any), "status")
}

func TestControlDurableResultsAndIdentity(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	task := controlNewTask(t, a, actor, "reboot-one", "REBOOT", map[string]any{"reason": "test"})
	id := Str(task, "taskId")
	if e := a.Validate("Task", task); e != nil {
		t.Fatal(e)
	}
	commands, e := a.PullCommands(ctx, "gw_test")
	if e != nil || len(commands) != 1 {
		t.Fatalf("pull %v %v", commands, e)
	}
	if Str(commands[0].(map[string]any), "taskId") != id {
		t.Fatal("task identity changed")
	}
	if commands, e = a.PullCommands(ctx, "gw_other"); e != nil || len(commands) != 0 {
		t.Fatal("gateway command isolation failed")
	}
	result := controlResult(id, "result_done", 3, "SUCCEEDED", "VERIFIED", map[string]any{"bootId": "boot_after"})
	ack, e := a.AcceptTaskResults(ctx, "gw_other", controlBatch(a, "gw_other", result))
	if e != nil || controlAckStatus(ack) != "REJECTED_PERMANENT" {
		t.Fatalf("source check %v %v", ack, e)
	}
	ack, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", result))
	if e != nil || controlAckStatus(ack) != "ACCEPTED" {
		t.Fatalf("persist %v %v", ack, e)
	}
	if e = a.Validate("TaskResultAck", ack); e != nil {
		t.Fatal(e)
	}
	var n int
	if e = a.Management.QueryRow("SELECT count(*) FROM command_outbox WHERE kind='result_ack' AND result_id='result_done'").Scan(&n); e != nil || n != 1 {
		t.Fatal("ACK was not persisted with result", e)
	}
	ack, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", result))
	if e != nil || controlAckStatus(ack) != "DUPLICATE" {
		t.Fatalf("duplicate %v %v", ack, e)
	}
	conflict := controlResult(id, "result_done", 3, "FAILED", "NONE", map[string]any{})
	ack, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", conflict))
	if e != nil || controlAckStatus(ack) != "REJECTED_PERMANENT" {
		t.Fatalf("conflict %v %v", ack, e)
	}
	older := controlResult(id, "result_old", 1, "ACCEPTED", "NONE", map[string]any{})
	if _, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", older)); e != nil {
		t.Fatal(e)
	}
	after, e := a.taskView(ctx, a.Management, id)
	if e != nil || Str(after, "state") != "SUCCEEDED" {
		t.Fatalf("old result regressed terminal task %v %v", after, e)
	}
	dataDir := a.Config.DataDir
	config := a.Config
	if e = a.Close(); e != nil {
		t.Fatal(e)
	}
	config.DataDir = dataDir
	b, e := New(config)
	if e != nil {
		t.Fatal(e)
	}
	defer b.Close()
	ack, e = b.AcceptTaskResults(ctx, "gw_test", controlBatch(b, "gw_test", result))
	if e != nil || controlAckStatus(ack) != "DUPLICATE" {
		t.Fatalf("restart lost dedup %v %v", ack, e)
	}
}

func TestControlConfigVerificationAndCAS(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	configuration := map[string]any{"scan": map[string]any{"enabled": true, "intervalMs": float64(100), "windowMs": float64(50)}}
	task := controlNewTask(t, a, actor, "config-one", "CONFIGURE", map[string]any{"configuration": configuration, "expectedReportedRevision": int64(0)})
	id := Str(task, "taskId")
	commands, e := a.PullCommands(ctx, "gw_test")
	if e != nil || len(commands) != 1 {
		t.Fatal(e)
	}
	command := commands[0].(map[string]any)
	if e = a.Validate("Command", command); e != nil {
		t.Fatal(e)
	}
	rev := Num(Obj(command, "parameters"), "desiredRevision")
	if rev != 2 {
		t.Fatalf("desired revision %d", rev)
	}
	incorrect := controlResult(id, "result_wrong", 1, "SUCCEEDED", "VERIFIED", map[string]any{"reportedConfigurationRevision": rev, "configuration": map[string]any{}})
	ack, e := a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", incorrect))
	if e != nil || controlAckStatus(ack) != "REJECTED_PERMANENT" {
		t.Fatalf("unverified config accepted %v %v", ack, e)
	}
	verified := controlResult(id, "result_verified", 2, "SUCCEEDED", "VERIFIED", map[string]any{"reportedConfigurationRevision": rev, "configuration": configuration})
	if _, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", verified)); e != nil {
		t.Fatal(e)
	}
	view, e := a.configurationView(ctx, a.Management, "gw_test")
	if e != nil || Num(view, "reportedRevision") != 2 {
		t.Fatalf("configuration projection %v %v", view, e)
	}
	tx, e := a.Management.BeginTx(ctx, nil)
	if e != nil {
		t.Fatal(e)
	}
	defer tx.Rollback()
	_, _, _, e = a.newTask(ctx, tx, actor, "config-stale", map[string]any{"gatewayId": "gw_test", "type": "CONFIGURE", "parameters": map[string]any{"configuration": configuration, "expectedReportedRevision": int64(0)}, "expiresAt": Timestamp(Now() + 60000)})
	if e == nil {
		t.Fatal("stale reported revision accepted")
	}
}

func TestControlExpiryPreservesUncertainty(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	one := controlNewTask(t, a, actor, "expires-unsent", "REBOOT", map[string]any{"reason": "test"})
	_, e := a.Management.Exec("UPDATE command_tasks SET expires_at_ms=? WHERE task_id=?", Now()-1, Str(one, "taskId"))
	if e != nil {
		t.Fatal(e)
	}
	if e = a.expireCommands(ctx); e != nil {
		t.Fatal(e)
	}
	v, e := a.taskView(ctx, a.Management, Str(one, "taskId"))
	if e != nil || Str(v, "state") != "EXPIRED" {
		t.Fatalf("unsent expiration %v %v", v, e)
	}
	two := controlNewTask(t, a, actor, "expires-sent", "REBOOT", map[string]any{"reason": "test"})
	if _, e = a.PullCommands(ctx, "gw_test"); e != nil {
		t.Fatal(e)
	}
	_, e = a.Management.Exec("UPDATE command_tasks SET expires_at_ms=? WHERE task_id=?", Now()-1, Str(two, "taskId"))
	if e != nil {
		t.Fatal(e)
	}
	if e = a.expireCommands(ctx); e != nil {
		t.Fatal(e)
	}
	v, e = a.taskView(ctx, a.Management, Str(two, "taskId"))
	if e != nil || Str(v, "state") != "UNKNOWN" {
		t.Fatalf("sent expiration incorrectly certain %v %v", v, e)
	}
}

func TestControlTaskIdempotency(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	req := map[string]any{"gatewayId": "gw_test", "type": "REBOOT", "parameters": map[string]any{"reason": "test"}, "expiresAt": Timestamp(time.Now().Add(time.Minute).UnixMilli())}
	create := func(request map[string]any) (any, error) {
		_, v, e := a.DoIdempotent(ctx, actor, "task.create", "same", request, func(tx *sql.Tx) (int, string, any, error) { return a.newTask(ctx, tx, actor, "same", request) })
		return v, e
	}
	one, e := create(req)
	if e != nil {
		t.Fatal(e)
	}
	two, e := create(req)
	if e != nil {
		t.Fatal(e)
	}
	aJSON, _ := json.Marshal(one)
	bJSON, _ := json.Marshal(two)
	if string(aJSON) != string(bJSON) {
		t.Fatal("idempotency changed response")
	}
	req["gatewayId"] = "gw_other"
	if _, e = create(req); e == nil {
		t.Fatal("idempotency key accepted different target")
	}
}

func TestControlRejectsUnknownConfiguration(t *testing.T) {
	if ValidateGatewayConfiguration(map[string]any{"remoteShell": true}) == nil {
		t.Fatal("unknown configuration accepted")
	}
	if ValidateGatewayConfiguration(map[string]any{"scan": map[string]any{"intervalMs": float64(100), "windowMs": float64(200)}}) == nil {
		t.Fatal("invalid BLE timing accepted")
	}
}

func TestControlRecoveryAndLeaseFence(t *testing.T) {
	a, actor := controlFixture(t)
	ctx := context.Background()
	task := controlNewTask(t, a, actor, "lease-one", "REBOOT", map[string]any{"reason": "test"})
	id := Str(task, "taskId")
	if _, e := a.Management.Exec("UPDATE command_outbox SET state='leased',lease_token=7,lease_owner='dead-process',lease_until_ms=? WHERE task_id=?", Now()-1, id); e != nil {
		t.Fatal(e)
	}
	commands, e := a.PullCommands(ctx, "gw_test")
	if e != nil || len(commands) != 1 {
		t.Fatalf("stale lease was stranded %v %v", commands, e)
	}
	var token int64
	if e = a.Management.QueryRow("SELECT lease_token FROM command_outbox WHERE task_id=? AND kind='command'", id).Scan(&token); e != nil || token != 8 {
		t.Fatalf("lease fencing token %d %v", token, e)
	}
	if _, e = a.Management.Exec("UPDATE command_outbox SET next_attempt_at_ms=0 WHERE task_id=?", id); e != nil {
		t.Fatal(e)
	}
	a.Publish = func(_ context.Context, topic string, payload any) error {
		result := controlResult(id, "during_publish", 3, "SUCCEEDED", "VERIFIED", map[string]any{"bootId": "new-boot"})
		_, e := a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", result))
		return e
	}
	if e = a.dispatchControl(ctx); e != nil {
		t.Fatal(e)
	}
	var state string
	if e = a.Management.QueryRow("SELECT state FROM command_outbox WHERE task_id=? AND kind='command'", id).Scan(&state); e != nil || state != "done" {
		t.Fatalf("publish completion overwrote receipt %s %v", state, e)
	}
	late := controlResult(id, "late_accepted", 5, "ACCEPTED", "NONE", map[string]any{})
	if _, e = a.AcceptTaskResults(ctx, "gw_test", controlBatch(a, "gw_test", late)); e != nil {
		t.Fatal(e)
	}
	view, e := a.taskView(ctx, a.Management, id)
	if e != nil || Str(view, "state") != "SUCCEEDED" || Str(Obj(view, "result"), "state") != "SUCCEEDED" {
		t.Fatalf("late result corrupted terminal view %v %v", view, e)
	}
}

func TestControlStopsWorkersBeforeDatabaseClose(t *testing.T) {
	a, _ := controlFixture(t)
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { a.StartControl(ctx); close(done) }()
	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("control workers did not stop")
	}
	if e := a.Management.Ping(); e != nil {
		t.Fatal("workers closed caller-owned database")
	}
}
