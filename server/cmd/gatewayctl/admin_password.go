package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"net/url"
	"os"
	"path/filepath"

	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/runtimeguard"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/simkit"
	"github.com/lyndon0728/ble-gateway-manager/server/internal/platform"
)

func resetAdminPassword(dir, namespace, name, passwordFile string, localDefault bool) error {
	if localDefault && name != platform.LocalDefaultAdministratorName {
		return errors.New("--local-default-admin requires --admin admin")
	}
	dbPath, err := filepath.Abs(filepath.Join(dir, "management.db"))
	if err != nil {
		return err
	}
	if _, err = os.Stat(dbPath); err != nil {
		return errors.New("password reset requires an existing initialized data directory")
	}
	password := platform.LocalDefaultAdministratorPassword
	if !localDefault {
		password, err = simkit.Secret(passwordFile)
		if err != nil {
			return err
		}
		if len(password) < 12 || len(password) > 256 {
			return errors.New("password file must contain a 12-256 character password")
		}
	}
	release, err := runtimeguard.Acquire(dir, true)
	if err != nil {
		return err
	}
	defer release()
	uriPath := filepath.ToSlash(dbPath)
	if filepath.VolumeName(dbPath) != "" {
		uriPath = "/" + uriPath
	}
	u := url.URL{Scheme: "file", Path: uriPath}
	query := u.Query()
	query.Set("mode", "rw")
	u.RawQuery = query.Encode()
	db, err := sql.Open("sqlite", u.String())
	if err != nil {
		return err
	}
	defer db.Close()
	db.SetMaxOpenConns(1)
	if _, err = db.Exec("PRAGMA foreign_keys=ON"); err != nil {
		return err
	}
	// Prepare and fsync the local output before touching credentials. A failed
	// directory/write check must not reset the database without an output file.
	temporary := ""
	if localDefault {
		output, err := filepath.Abs(passwordFile)
		if err != nil {
			return err
		}
		dataPath, err := filepath.Abs(dir)
		if err != nil {
			return err
		}
		if inside(output, dataPath) {
			return errors.New("password output must be outside the data directory")
		}
		if info, err := os.Lstat(output); err == nil && !info.Mode().IsRegular() {
			return errors.New("password output must be a regular file")
		} else if err != nil && !os.IsNotExist(err) {
			return err
		}
		if err = os.MkdirAll(filepath.Dir(output), 0700); err != nil {
			return err
		}
		file, err := os.CreateTemp(filepath.Dir(output), ".admin-password-*")
		if err != nil {
			return err
		}
		temporary = file.Name()
		defer os.Remove(temporary)
		_, err = file.WriteString(password + "\n")
		if err == nil {
			err = file.Sync()
		}
		err = errors.Join(err, file.Close())
		if err != nil {
			return err
		}
		passwordFile = output
	}
	if err = platform.ResetAdministratorPassword(context.Background(), db, namespace, name, password, localDefault); err != nil {
		return err
	}
	if temporary != "" {
		if err = os.Rename(temporary, passwordFile); err != nil {
			return fmt.Errorf("administrator password was reset, but the password file could not be replaced; rerun this explicit local reset to synchronize it: %w", err)
		}
	}
	fmt.Println("Administrator password reset; previous browser sessions revoked. Password remains in the specified file.")
	return nil
}
