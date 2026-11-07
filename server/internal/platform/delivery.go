package platform

import (
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"database/sql"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

type deliveryItem struct {
	ID             string
	Lease, Attempt int64
	Payload        []byte
}
type deliveryBatch struct {
	ID, Target, Version, Protocol string
	Config                        pipeMap
	Items                         []deliveryItem
}
type mqttDeliveryClient struct {
	client    mqtt.Client
	signature string
}
type appDeliveryClients struct {
	mu          sync.Mutex
	clients     map[string]mqttDeliveryClient
	httpClients map[string]*http.Client
	closed      bool
	callbacks   sync.WaitGroup
}

var deliveryClientStores sync.Map

func (a *App) deliveryClients() *appDeliveryClients {
	v, _ := deliveryClientStores.LoadOrStore(a, &appDeliveryClients{clients: map[string]mqttDeliveryClient{}, httpClients: map[string]*http.Client{}})
	return v.(*appDeliveryClients)
}
func (a *App) closeDeliveryClients() {
	if v, ok := deliveryClientStores.LoadAndDelete(a); ok {
		s := v.(*appDeliveryClients)
		s.mu.Lock()
		s.closed = true
		s.mu.Unlock()
		for _, c := range s.clients {
			if c.client.IsConnected() {
				c.client.Disconnect(250)
			}
		}
		for _, c := range s.httpClients {
			c.CloseIdleConnections()
		}
		s.callbacks.Wait()
	}
}
func deliveryBackoff(attempt int64) int64 {
	if attempt > 10 {
		attempt = 10
	}
	delay := int64(1000) << uint(attempt)
	if delay > 300000 {
		delay = 300000
	}
	return delay
}

func (a *App) claimDeliveryBatch(ctx context.Context) (*deliveryBatch, error) {
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return nil, err
	}
	defer tx.Rollback()
	now := pnow()
	_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET state='retry_wait',last_error='ACK_TIMEOUT',lease_owner=NULL,lease_until_ms=NULL,next_attempt_at_ms=? WHERE state IN ('leased','awaiting_ack') AND lease_until_ms<=?", now, now)
	if err != nil {
		return nil, err
	}
	b := &deliveryBatch{ID: pipeID("batch")}
	var config string
	err = tx.QueryRowContext(ctx, "SELECT d.target_id,d.target_config_version,d.protocol,d.destination_json FROM delivery_outbox d WHERE d.state IN ('pending','retry_wait') AND d.next_attempt_at_ms<=? AND NOT EXISTS(SELECT 1 FROM delivery_outbox active WHERE active.target_id=d.target_id AND active.state IN ('leased','awaiting_ack')) ORDER BY d.next_attempt_at_ms,d.created_at_ms,d.delivery_id LIMIT 1", now).Scan(&b.Target, &b.Version, &b.Protocol, &config)
	if errors.Is(err, sql.ErrNoRows) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	b.Config = pm(config)
	var inflight int64
	if err = tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM delivery_outbox WHERE target_id=? AND state IN ('leased','awaiting_ack')", b.Target).Scan(&inflight); err != nil {
		return nil, err
	}
	capacity := pn(b.Config, "maxInflightEvents") - inflight
	limit := pn(b.Config, "maxBatchEvents")
	if limit > 200 {
		limit = 200
	}
	if capacity < limit {
		limit = capacity
	}
	if limit < 1 {
		return nil, nil
	}
	rows, err := tx.QueryContext(ctx, "SELECT delivery_id,lease_token,attempts,wire_payload FROM delivery_outbox WHERE target_id=? AND target_config_version=? AND state IN ('pending','retry_wait') AND next_attempt_at_ms<=? ORDER BY next_attempt_at_ms,created_at_ms,delivery_id LIMIT ?", b.Target, b.Version, now, limit)
	if err != nil {
		return nil, err
	}
	total := 0
	for rows.Next() {
		var item deliveryItem
		if err = rows.Scan(&item.ID, &item.Lease, &item.Attempt, &item.Payload); err != nil {
			rows.Close()
			return nil, err
		}
		if len(b.Items) > 0 && total+len(item.Payload) > 1048576 {
			break
		}
		total += len(item.Payload)
		item.Lease++
		item.Attempt++
		b.Items = append(b.Items, item)
	}
	rows.Close()
	members := []pipeMap{}
	leaseUntil := now + max(int64(30000), 2*a.Config.HTTPTimeout.Milliseconds())
	for _, item := range b.Items {
		_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET state='awaiting_ack',lease_owner=?,lease_token=?,lease_until_ms=?,attempts=attempts+1 WHERE delivery_id=?", b.ID, item.Lease, leaseUntil, item.ID)
		if err != nil {
			return nil, err
		}
		members = append(members, pipeMap{"deliveryId": item.ID, "leaseToken": item.Lease})
	}
	_, err = tx.ExecContext(ctx, "INSERT INTO delivery_batches(batch_id,target_id,target_version,delivery_ids_json,created_at_ms,expires_at_ms) VALUES(?,?,?,?,?,?)", b.ID, b.Target, b.Version, pipeJSON(members), now, now+86400000)
	if err != nil {
		return nil, err
	}
	if err = tx.Commit(); err != nil {
		return nil, err
	}
	return b, nil
}
func (a *App) DeliverOnce(ctx context.Context) (bool, error) {
	b, err := a.claimDeliveryBatch(ctx)
	if err != nil || b == nil {
		return false, err
	}
	events := []json.RawMessage{}
	for _, item := range b.Items {
		events = append(events, json.RawMessage(item.Payload))
	}
	payload, err := json.Marshal(pipeMap{"batchId": b.ID, "events": events})
	if err != nil {
		return true, err
	}
	if err = a.validateTarget(ctx, b.Config); err == nil {
		if b.Protocol == "http" {
			err = a.sendHTTPDelivery(ctx, b, payload)
		} else {
			err = a.sendMQTTDelivery(ctx, b, payload)
		}
	}
	if err != nil {
		markErr := a.failDeliveryBatch(ctx, b, err)
		if markErr != nil {
			return true, markErr
		}
		return true, err
	}
	return true, nil
}
func (a *App) failDeliveryBatch(ctx context.Context, b *deliveryBatch, cause error) error {
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	code := "TRANSPORT_ERROR"
	var ae *deliveryAckError
	if errors.As(cause, &ae) {
		code = ae.code
	}
	for _, item := range b.Items {
		_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET state='retry_wait',next_attempt_at_ms=?,last_error=?,lease_owner=NULL,lease_until_ms=NULL WHERE delivery_id=? AND lease_token=? AND state='awaiting_ack'", pnow()+deliveryBackoff(item.Attempt), code, item.ID, item.Lease)
		if err != nil {
			return err
		}
	}
	return tx.Commit()
}

type deliveryAckError struct{ code string }

func (e *deliveryAckError) Error() string { return e.code }

func (a *App) targetTLS(c pipeMap, host string) (*tls.Config, error) {
	tc := &tls.Config{MinVersion: tls.VersionTLS12, ServerName: host}
	if ref := ps(c, "tlsTrustRef"); ref != "" {
		pem, err := a.SecretGet(ref)
		if err != nil {
			return nil, err
		}
		pool, err := x509.SystemCertPool()
		if err != nil {
			pool = x509.NewCertPool()
		}
		if !pool.AppendCertsFromPEM([]byte(pem)) {
			return nil, errors.New("TLS trust secret must contain CA PEM")
		}
		tc.RootCAs = pool
	}
	return tc, nil
}
func (a *App) targetDial(ctx context.Context, network, address string) (net.Conn, error) {
	host, port, err := net.SplitHostPort(address)
	if err != nil {
		return nil, err
	}
	ips, err := net.DefaultResolver.LookupIPAddr(ctx, host)
	if err != nil {
		return nil, err
	}
	var last error
	for _, entry := range ips {
		if err = a.checkTargetIP(entry.IP); err != nil {
			return nil, err
		}
		dialer := net.Dialer{Timeout: a.Config.HTTPTimeout}
		conn, e := dialer.DialContext(ctx, network, net.JoinHostPort(entry.IP.String(), port))
		if e == nil {
			return conn, nil
		}
		last = e
	}
	if last == nil {
		last = errors.New("target hostname resolved no addresses")
	}
	return nil, last
}
func (a *App) sendHTTPDelivery(ctx context.Context, b *deliveryBatch, payload []byte) error {
	u, _ := url.Parse(ps(b.Config, "endpoint"))
	tlsConfig, err := a.targetTLS(b.Config, u.Hostname())
	if err != nil {
		return err
	}
	store := a.deliveryClients()
	store.mu.Lock()
	cacheKey := b.Target + "@" + b.Version
	client := store.httpClients[cacheKey]
	if client == nil {
		if len(store.httpClients) >= 128 {
			for key, c := range store.httpClients {
				c.CloseIdleConnections()
				delete(store.httpClients, key)
				break
			}
		}
		transport := &http.Transport{Proxy: nil, DialContext: a.targetDial, TLSClientConfig: tlsConfig, TLSHandshakeTimeout: a.Config.HTTPTimeout, ResponseHeaderTimeout: a.Config.HTTPTimeout, MaxConnsPerHost: 4, MaxIdleConns: 4, IdleConnTimeout: 30 * time.Second}
		client = &http.Client{Timeout: a.Config.HTTPTimeout, Transport: transport, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}
		store.httpClients[cacheKey] = client
	}
	store.mu.Unlock()
	request, err := http.NewRequestWithContext(ctx, http.MethodPost, u.String(), bytes.NewReader(payload))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", "application/json")
	request.Header.Set("Idempotency-Key", b.ID)
	if ref := ps(b.Config, "secretRef"); ref != "" {
		token, e := a.SecretGet(ref)
		if e != nil {
			return e
		}
		request.Header.Set("Authorization", "Bearer "+token)
	}
	response, err := client.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		return &deliveryAckError{fmt.Sprintf("HTTP_%d", response.StatusCode)}
	}
	raw, err := io.ReadAll(io.LimitReader(response.Body, 1048577))
	if err != nil {
		return err
	}
	if len(raw) > 1048576 {
		return &deliveryAckError{"ACK_TOO_LARGE"}
	}
	ack := pipeMap{}
	if err = json.Unmarshal(raw, &ack); err != nil {
		return &deliveryAckError{"ACK_INVALID_JSON"}
	}
	if ps(ack, "batchId") != b.ID || ps(ack, "targetVersion") != b.Version {
		return &deliveryAckError{"ACK_CORRELATION_MISMATCH"}
	}
	return a.AcceptCustomerAck(ctx, b.Target, ack)
}

func mqttWait(ctx context.Context, token mqtt.Token) error {
	select {
	case <-ctx.Done():
		return ctx.Err()
	case <-token.Done():
		return token.Error()
	}
}
func (a *App) sendMQTTDelivery(ctx context.Context, b *deliveryBatch, payload []byte) error {
	s := a.deliveryClients()
	key := b.Target + "@" + b.Version
	signature := pipeHash(b.Config)
	s.mu.Lock()
	entry, exists := s.clients[key]
	if exists && entry.signature != signature {
		s.mu.Unlock()
		return errors.New("immutable MQTT target changed")
	}
	if !exists {
		if len(s.clients) >= 64 {
			s.mu.Unlock()
			return errors.New("active MQTT target limit (64) reached")
		}
		u, err := url.Parse(ps(b.Config, "endpoint"))
		if err != nil {
			s.mu.Unlock()
			return err
		}
		tlsConfig, err := a.targetTLS(b.Config, u.Hostname())
		if err != nil {
			s.mu.Unlock()
			return err
		}
		scheme := "ssl"
		if u.Scheme == "mqtt" || u.Scheme == "tcp" {
			scheme = "tcp"
		}
		u.Scheme = scheme
		port := u.Port()
		if port == "" {
			port = "8883"
			if scheme == "tcp" {
				port = "1883"
			}
			u.Host = net.JoinHostPort(u.Hostname(), port)
		}
		opts := mqtt.NewClientOptions().AddBroker(u.String()).SetClientID(pipeStable("delivery", []string{a.Config.InstanceID, b.Target, b.Version})[:40]).SetCleanSession(true).SetAutoReconnect(true).SetConnectRetry(false).SetConnectTimeout(a.Config.HTTPTimeout).SetWriteTimeout(a.Config.HTTPTimeout).SetTLSConfig(tlsConfig).SetOrderMatters(false)
		if ref := ps(b.Config, "secretRef"); ref != "" {
			secret, e := a.SecretGet(ref)
			if e != nil {
				s.mu.Unlock()
				return e
			}
			credentials := pm(secret)
			if ps(credentials, "username") != "" {
				opts.SetUsername(ps(credentials, "username")).SetPassword(ps(credentials, "password"))
			} else {
				opts.SetUsername(b.Target).SetPassword(secret)
			}
		}
		opts.SetCustomOpenConnectionFn(func(uri *url.URL, options mqtt.ClientOptions) (net.Conn, error) {
			dialCtx, cancel := context.WithTimeout(context.Background(), a.Config.HTTPTimeout)
			defer cancel()
			conn, e := a.targetDial(dialCtx, "tcp", uri.Host)
			if e != nil {
				return nil, e
			}
			if uri.Scheme == "ssl" {
				secured := tls.Client(conn, tlsConfig)
				if e = secured.HandshakeContext(dialCtx); e != nil {
					conn.Close()
					return nil, e
				}
				return secured, nil
			}
			return conn, nil
		})
		targetID, version, ackTopic := b.Target, b.Version, ps(b.Config, "ackTopic")
		opts.SetDefaultPublishHandler(func(_ mqtt.Client, msg mqtt.Message) {
			s.mu.Lock()
			if s.closed {
				s.mu.Unlock()
				return
			}
			s.callbacks.Add(1)
			s.mu.Unlock()
			defer s.callbacks.Done()
			if msg.Topic() != ackTopic || msg.Retained() || len(msg.Payload()) > 1048576 {
				return
			}
			ack := pipeMap{}
			if json.Unmarshal(msg.Payload(), &ack) != nil || ps(ack, "targetId") != targetID || ps(ack, "targetVersion") != version {
				return
			}
			ackCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			if err := a.AcceptCustomerAck(ackCtx, targetID, ack); err != nil {
				a.Logger.Warn("customer MQTT ACK rejected", "target", targetID, "error", err)
			}
		})
		opts.SetOnConnectHandler(func(client mqtt.Client) {
			token := client.Subscribe(ackTopic, 1, nil)
			if !token.WaitTimeout(a.Config.HTTPTimeout) || token.Error() != nil {
				a.Logger.Warn("customer ACK subscription unavailable", "target", targetID)
			}
		})
		entry = mqttDeliveryClient{client: mqtt.NewClient(opts), signature: signature}
		s.clients[key] = entry
	}
	client := entry.client
	s.mu.Unlock()
	if !client.IsConnectionOpen() {
		connectCtx, cancel := context.WithTimeout(ctx, a.Config.HTTPTimeout)
		err := mqttWait(connectCtx, client.Connect())
		cancel()
		if err != nil {
			return err
		}
	}
	publishCtx, cancel := context.WithTimeout(ctx, a.Config.HTTPTimeout)
	defer cancel()
	if err := mqttWait(publishCtx, client.Subscribe(ps(b.Config, "ackTopic"), 1, nil)); err != nil {
		return err
	}
	return mqttWait(publishCtx, client.Publish(ps(b.Config, "payloadTopic"), 1, false, payload))
}

// AcceptCustomerAck checks transport-bound identity, durable batch membership and
// each current lease. Missing items retain custody; broker PUBACK never calls this.
func (a *App) AcceptCustomerAck(ctx context.Context, targetID string, ack pipeMap) error {
	if err := a.Validate("CustomerAck", ack); err != nil {
		return &deliveryAckError{"ACK_SCHEMA_INVALID"}
	}
	if ps(ack, "targetId") != targetID {
		return &deliveryAckError{"ACK_TARGET_MISMATCH"}
	}
	tx, err := a.Pipeline.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	var boundTarget, version, raw string
	var expires int64
	if err = tx.QueryRowContext(ctx, "SELECT target_id,target_version,delivery_ids_json,expires_at_ms FROM delivery_batches WHERE batch_id=?", ps(ack, "batchId")).Scan(&boundTarget, &version, &raw, &expires); err != nil {
		return &deliveryAckError{"ACK_BATCH_UNKNOWN"}
	}
	if boundTarget != targetID || version != ps(ack, "targetVersion") || expires < pnow() {
		return &deliveryAckError{"ACK_CORRELATION_MISMATCH"}
	}
	var members []pipeMap
	if err = json.Unmarshal([]byte(raw), &members); err != nil {
		return err
	}
	leases := map[string]int64{}
	for _, m := range members {
		leases[ps(m, "deliveryId")] = pn(m, "leaseToken")
	}
	seen := map[string]bool{}
	for _, v := range pl(ack, "items") {
		item := v.(map[string]any)
		id := ps(item, "deliveryId")
		if leases[id] == 0 || seen[id] {
			return &deliveryAckError{"ACK_ITEM_UNKNOWN_OR_DUPLICATE"}
		}
		seen[id] = true
	}
	now := pnow()
	for _, v := range pl(ack, "items") {
		item := v.(map[string]any)
		id := ps(item, "deliveryId")
		status := ps(item, "status")
		if status == "ACCEPTED" || status == "DUPLICATE" {
			_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET state='delivered',ack_json=?,delivered_at_ms=?,last_error=NULL,lease_owner=NULL,lease_until_ms=NULL WHERE delivery_id=? AND target_id=? AND target_config_version=? AND lease_token=? AND state='awaiting_ack'", pipeJSON(ack), now, id, targetID, version, leases[id])
		} else {
			code := "CUSTOMER_" + status
			if ps(item, "code") != "" {
				code += ":" + ps(item, "code")
			}
			_, err = tx.ExecContext(ctx, "UPDATE delivery_outbox SET state='retry_wait',next_attempt_at_ms=?,last_error=?,lease_owner=NULL,lease_until_ms=NULL WHERE delivery_id=? AND target_id=? AND target_config_version=? AND lease_token=? AND state='awaiting_ack'", now+30000, code, id, targetID, version, leases[id])
		}
		if err != nil {
			return err
		}
	}
	_, err = tx.ExecContext(ctx, `UPDATE event_receipts SET custody_state='delivered' WHERE custody_state='processed' AND EXISTS(SELECT 1 FROM processing_jobs j WHERE j.event_key=event_receipts.event_key AND j.revision=event_receipts.latest_revision AND j.state='completed') AND NOT EXISTS(SELECT 1 FROM delivery_outbox d JOIN processing_results r ON r.output_id=d.output_id JOIN processing_jobs j ON j.processing_id=r.processing_id WHERE j.event_key=event_receipts.event_key AND j.revision=event_receipts.latest_revision AND d.required=1 AND d.state<>'delivered') AND event_key IN (SELECT j.event_key FROM processing_jobs j JOIN processing_results r ON r.processing_id=j.processing_id JOIN delivery_outbox d ON d.output_id=r.output_id WHERE d.target_id=? AND d.delivery_id IN (SELECT json_extract(value,'$.deliveryId') FROM json_each(?)))`, targetID, pipeJSON(pl(ack, "items")))
	if err != nil {
		return err
	}
	return tx.Commit()
}
