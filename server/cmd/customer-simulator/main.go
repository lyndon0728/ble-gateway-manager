package main

import (
	"context"
	"crypto/subtle"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/simkit"
)

func main() {
	if err := run(); err != nil {
		log.Print(err)
		os.Exit(1)
	}
}
func run() error {
	listen := flag.String("listen", "127.0.0.1:9090", "listen address")
	dbPath := flag.String("db", "./sim-data/customer.db", "durable customer inbox")
	target := flag.String("target", "target_demo", "expected target ID")
	version := flag.String("target-version", "1", "expected immutable target version")
	tokenFile := flag.String("token-file", "", "optional bearer token file")
	cert := flag.String("tls-cert", "", "HTTPS certificate")
	key := flag.String("tls-key", "", "HTTPS private key")
	insecure := flag.Bool("allow-insecure", false, "allow HTTP on non-loopback test networks")
	drop := flag.Int64("drop-acks", 0, "persist but return HTTP 503 for the first N accepted requests")
	delay := flag.Duration("ack-delay", 0, "delay after durable commit")
	flag.Parse()
	host, _, err := net.SplitHostPort(*listen)
	if err != nil {
		return err
	}
	ip := net.ParseIP(host)
	if *cert == "" && !*insecure && (ip == nil || !ip.IsLoopback()) {
		return fmt.Errorf("non-loopback listener requires TLS; use --allow-insecure only for an isolated test")
	}
	token, err := simkit.Secret(*tokenFile)
	if err != nil {
		return err
	}
	store, err := simkit.OpenCustomer(*dbPath, *target, *version)
	if err != nil {
		return err
	}
	defer store.DB.Close()
	var requests atomic.Int64
	auth := func(r *http.Request) bool {
		if token == "" {
			return true
		}
		return subtle.ConstantTimeCompare([]byte(strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")), []byte(token)) == 1
	}
	mux := http.NewServeMux()
	mux.HandleFunc("POST /events", func(w http.ResponseWriter, r *http.Request) {
		if !auth(r) {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, 1<<20)
		d := json.NewDecoder(r.Body)
		d.UseNumber()
		var batch map[string]any
		if err := d.Decode(&batch); err != nil {
			http.Error(w, "invalid JSON", 400)
			return
		}
		if err := d.Decode(new(any)); err != io.EOF {
			http.Error(w, "only one JSON value is allowed", 400)
			return
		}
		ack, err := store.Receive(r.Context(), batch)
		if err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		n := requests.Add(1)
		if n <= *drop {
			http.Error(w, "simulated lost acknowledgement after durable commit", 503)
			return
		}
		if *delay > 0 {
			select {
			case <-r.Context().Done():
				return
			case <-time.After(*delay):
			}
		}
		simkit.WriteJSON(w, 200, ack)
	})
	mux.HandleFunc("GET /stats", func(w http.ResponseWriter, r *http.Request) {
		if !auth(r) {
			http.Error(w, "unauthorized", 401)
			return
		}
		stats, err := store.Stats(r.Context())
		if err != nil {
			http.Error(w, "storage unavailable", 503)
			return
		}
		simkit.WriteJSON(w, 200, stats)
	})
	mux.HandleFunc("GET /health/live", func(w http.ResponseWriter, r *http.Request) { simkit.WriteJSON(w, 200, map[string]any{"status": "ok"}) })
	srv := &http.Server{Addr: *listen, Handler: mux, ReadHeaderTimeout: 5 * time.Second, ReadTimeout: 20 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 30 * time.Second}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	go func() {
		<-ctx.Done()
		c, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = srv.Shutdown(c)
	}()
	log.Printf("customer simulator listening on %s; durable DB %s", *listen, *dbPath)
	if *cert != "" {
		err = srv.ListenAndServeTLS(*cert, *key)
	} else {
		err = srv.ListenAndServe()
	}
	if err == http.ErrServerClosed {
		return nil
	}
	return err
}
