package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"github.com/lyndon0728/ble-gateway-manager/server/cmd/internal/simkit"
	"io"
	"log"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"
)

func main() {
	if e := run(); e != nil {
		log.Print(e)
		os.Exit(1)
	}
}
func run() error {
	base := flag.String("server", "http://127.0.0.1:8080", "server URL")
	db := flag.String("db", "sim-data/gateway.db", "durable spool and task journal")
	ns := flag.String("namespace", "ns_local", "namespace")
	gateway := flag.String("gateway", "gw_demo", "gateway ID")
	secret := flag.String("token-file", "", "provisioned token file")
	ca := flag.String("ca", "", "server CA PEM")
	cert := flag.String("cert", "", "client certificate PEM")
	key := flag.String("key", "", "client key PEM")
	count := flag.Int("count", 10, "new reference sensor packets on this invocation; use 0 to resume only")
	duration := flag.Duration("duration", 30*time.Second, "run duration; 0 until signal")
	interval := flag.Duration("interval", time.Second, "network retry/poll interval")
	drop := flag.Int("ignore-acks", 0, "ignore first N successful telemetry ACKs to inject retransmission")
	offline := flag.Duration("offline", 0, "persist packets then delay network until elapsed")
	flag.Parse()
	if *interval < 100*time.Millisecond {
		return errors.New("interval must be >=100ms")
	}
	u, e := url.Parse(*base)
	if e != nil {
		return e
	}
	ip := net.ParseIP(u.Hostname())
	if u.Scheme != "https" && (u.Scheme != "http" || ip == nil || !ip.IsLoopback()) {
		return errors.New("HTTPS required except explicit loopback HTTP")
	}
	token, e := simkit.Secret(*secret)
	if e != nil {
		return e
	}
	if token == "" && *cert == "" {
		return errors.New("token-file or mTLS certificate required")
	}
	client, e := simkit.Client(*ca, *cert, *key)
	if e != nil {
		return e
	}
	g, e := simkit.OpenGateway(*db, *ns, *gateway)
	if e != nil {
		return e
	}
	defer g.DB.Close()
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if *duration > 0 {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, *duration)
		defer cancel()
	}
	if e = g.Enqueue(ctx, *count); e != nil {
		return e
	}
	request := func(method, path string, body any) (map[string]any, error) {
		var reader io.Reader
		if body != nil {
			b, e := json.Marshal(body)
			if e != nil {
				return nil, e
			}
			reader = bytes.NewReader(b)
		}
		req, e := http.NewRequestWithContext(ctx, method, strings.TrimRight(*base, "/")+path, reader)
		if e != nil {
			return nil, e
		}
		req.Header.Set("Content-Type", "application/json")
		if token != "" {
			req.Header.Set("Authorization", "Bearer "+token)
		}
		req.Header.Set("X-Gateway-ID", *gateway)
		req.Header.Set("X-Namespace-ID", *ns)
		resp, e := client.Do(req)
		if e != nil {
			return nil, e
		}
		defer resp.Body.Close()
		if resp.StatusCode < 200 || resp.StatusCode > 299 {
			return nil, fmt.Errorf("%s returned HTTP %d", path, resp.StatusCode)
		}
		var result map[string]any
		e = json.NewDecoder(io.LimitReader(resp.Body, 2<<20)).Decode(&result)
		if e == io.EOF && resp.StatusCode == 204 {
			e = nil
		}
		return result, e
	}
	start := time.Now()
	ticker := time.NewTicker(*interval)
	defer ticker.Stop()
	for {
		if time.Since(start) >= *offline {
			batch, e := g.Batch(ctx)
			if e != nil {
				return e
			}
			if len(batch["events"].([]any)) > 0 {
				ack, e := request("POST", "/api/v1/gateway-ingress/batches", batch)
				if e != nil {
					log.Printf("telemetry retained: %v", e)
				} else if *drop > 0 {
					*drop--
					log.Print("fault injection: ignored telemetry ACK")
				} else if e = g.Ack(ctx, batch, ack); e != nil {
					log.Printf("telemetry retained: %v", e)
				}
			}
			commands, e := request("GET", "/api/v1/gateway-ingress/commands", nil)
			if e != nil {
				log.Printf("command polling: %v", e)
			} else {
				items, _ := commands["commands"].([]any)
				for _, item := range items {
					cmd, ok := item.(map[string]any)
					if ok {
						if e = g.Command(ctx, cmd); e != nil {
							log.Printf("command rejected: %v", e)
						}
					}
				}
			}
			results, e := g.Results(ctx)
			if e != nil {
				return e
			}
			if len(results["results"].([]any)) > 0 {
				ack, e := request("POST", "/api/v1/gateway-ingress/results", results)
				if e != nil {
					log.Printf("result retained: %v", e)
				} else if e = g.AckResults(ctx, results, ack); e != nil {
					log.Printf("result retained: %v", e)
				}
			}
			status, e := g.Status(ctx)
			if e != nil {
				return e
			}
			if _, e = request("POST", "/api/v1/gateway-ingress/status", status); e != nil && ctx.Err() == nil {
				log.Printf("status: %v", e)
			}
		}
		select {
		case <-ctx.Done():
			log.Print("simulator stopped; pending data remains in its local database")
			return nil
		case <-ticker.C:
		}
	}
}
