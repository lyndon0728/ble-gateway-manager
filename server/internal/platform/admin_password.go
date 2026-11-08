package platform

import (
	"context"
	"database/sql"
	"errors"
	"time"
)

const LocalDefaultAdministratorName = "admin"
const LocalDefaultAdministratorPassword = "123456"

// ResetAdministratorPassword updates only an existing active instance
// administrator. The offline caller must own the data directory's exclusive
// maintenance lock. Passwords and browser sessions change in the same transaction.
func ResetAdministratorPassword(ctx context.Context, db *sql.DB, namespace, name, password string, localDefault bool) error {
	if localDefault {
		if name != LocalDefaultAdministratorName || password != LocalDefaultAdministratorPassword {
			return errors.New("local default mode only supports the fixed local administrator")
		}
	} else if len(name) < 1 || len(password) < 12 || len(password) > 256 {
		return errors.New("administrator name and 12-256 character password required")
	}
	hash, err := passwordHash(password)
	if err != nil {
		return err
	}
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var storedNamespace string
	if err = tx.QueryRowContext(ctx, "SELECT json_extract(settings_json,'$.namespaceId') FROM instance_settings WHERE singleton=1").Scan(&storedNamespace); err != nil {
		return err
	}
	if storedNamespace != namespace {
		return errors.New("namespace does not match initialized instance")
	}
	var id string
	err = tx.QueryRowContext(ctx, `SELECT p.principal_id FROM principals p WHERE p.name=? AND p.kind='human' AND p.status='active' AND EXISTS(SELECT 1 FROM role_bindings b WHERE b.principal_id=p.principal_id AND b.role='instance_admin' AND b.scope_id='scope_instance')`, name).Scan(&id)
	if errors.Is(err, sql.ErrNoRows) {
		return errors.New("an existing active human instance administrator is required")
	}
	if err != nil {
		return err
	}
	now := Now()
	if _, err = tx.ExecContext(ctx, "UPDATE principals SET auth_epoch=auth_epoch+1,updated_at_ms=? WHERE principal_id=?", now, id); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "UPDATE credentials SET status='revoked',revoked_at_ms=? WHERE principal_id=? AND kind IN ('password','session') AND status='active'", now, id); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "INSERT INTO credentials(credential_id,label,principal_id,kind,verifier,status,not_before_ms,created_at_ms) VALUES(?,'Administrator password',?,'password',?,'active',?,?)", ID("credential"), id, hash, now, now); err != nil {
		return err
	}
	if _, err = tx.ExecContext(ctx, "INSERT INTO audit_events VALUES(?,?,?,?,?,?,?,?,?,?)", ID("audit"), id, "administrator.password.reset", "principal", id, "success", ID("corr"), encode(map[string]any{"passwordChanged": true, "sessionsRevoked": true, "localDefault": localDefault}), now, now+int64(90*24*time.Hour/time.Millisecond)); err != nil {
		return err
	}
	return tx.Commit()
}
