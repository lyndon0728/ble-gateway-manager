package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/lyndon0728/ble-gateway-manager/server/internal/platform"
)

func main() {
	if err := run(); err != nil {
		slog.Error("server stopped", "error", err)
		os.Exit(1)
	}
}
func run() error {
	flags := map[string]*string{}
	for key, env := range map[string]string{"data-dir": "GW_DATA_DIR", "listen": "GW_LISTEN", "role": "GW_ROLE", "namespace": "GW_NAMESPACE", "tls-cert": "GW_TLS_CERT", "tls-key": "GW_TLS_KEY", "client-ca": "GW_CLIENT_CA"} {
		flags[env] = flag.String(key, "", "override "+env)
	}
	insecure := flag.Bool("allow-insecure-dev", false, "allow HTTP on a loopback address only")
	version := flag.Bool("version", false, "print version")
	flag.Parse()
	if *version {
		fmt.Println(platform.Version)
		return nil
	}
	for env, value := range flags {
		if *value != "" {
			if err := os.Setenv(env, *value); err != nil {
				return err
			}
		}
	}
	if *insecure {
		_ = os.Setenv("GW_ALLOW_INSECURE_DEV", "true")
	}
	// Keep credentials out of command lines and Compose's resolved environment.
	if file := os.Getenv("GW_BROKER_PASSWORD_FILE"); file != "" {
		b, e := os.ReadFile(file)
		if e != nil {
			return e
		}
		_ = os.Setenv("GW_BROKER_PASSWORD", stringTrim(b))
	}
	cfg, err := platform.ConfigFromEnv()
	if err != nil {
		return err
	}
	if cfg.TLSCertFile == "" {
		host, _, e := net.SplitHostPort(cfg.ListenAddr)
		if e != nil {
			return e
		}
		ip := net.ParseIP(host)
		if ip == nil || !ip.IsLoopback() {
			return errors.New("insecure development HTTP must listen on an explicit loopback IP")
		}
	}
	app, err := platform.New(cfg)
	if err != nil {
		return err
	}
	defer app.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	server := &http.Server{Addr: cfg.ListenAddr, Handler: app.Handler(), ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 30 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second, MaxHeaderBytes: 32 << 10, TLSConfig: &tls.Config{MinVersion: tls.VersionTLS12}}
	if cfg.ClientCAFile != "" {
		pem, e := os.ReadFile(cfg.ClientCAFile)
		if e != nil {
			return e
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(pem) {
			return errors.New("client CA contains no certificates")
		}
		server.TLSConfig.ClientCAs = pool
		server.TLSConfig.ClientAuth = tls.VerifyClientCertIfGiven
	}
	workersDone := make(chan struct{})
	go func() { app.Run(ctx); close(workersDone) }()
	defer func() { stop(); <-workersDone }()
	done := make(chan error, 1)
	go func() {
		if cfg.TLSCertFile != "" {
			done <- server.ListenAndServeTLS(cfg.TLSCertFile, cfg.TLSKeyFile)
		} else {
			done <- server.ListenAndServe()
		}
	}()
	slog.Info("gateway server started", "version", platform.Version, "role", cfg.Role, "listen", cfg.ListenAddr)
	select {
	case err = <-done:
		stop()
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	case <-ctx.Done():
	}
	shutdown, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()
	return server.Shutdown(shutdown)
}
func stringTrim(b []byte) string {
	for len(b) > 0 && (b[len(b)-1] == '\n' || b[len(b)-1] == '\r') {
		b = b[:len(b)-1]
	}
	return string(b)
}
