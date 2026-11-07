package platform

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"os"
	"strings"
	"sync"
	"time"

	mqtt "github.com/eclipse/paho.mqtt.golang"
)

// The gateway listener is intentionally separate from per-customer delivery clients.
// Broker ACLs MUST bind each device credential to its own namespace/gateway topics.
func (a *App) prepareGatewayBroker() (func(context.Context), error) {
	if a.Config.BrokerURL == "" {
		return func(ctx context.Context) { <-ctx.Done() }, nil
	}
	u, e := url.Parse(a.Config.BrokerURL)
	if e != nil {
		return nil, e
	}
	if !a.Config.AllowInsecureDev && u.Scheme != "ssl" && u.Scheme != "tls" && u.Scheme != "wss" {
		return nil, errors.New("gateway MQTT requires TLS outside explicit development mode")
	}
	tlsConfig := &tls.Config{MinVersion: tls.VersionTLS12}
	if a.Config.BrokerCAFile != "" {
		b, e := os.ReadFile(a.Config.BrokerCAFile)
		if e != nil {
			return nil, e
		}
		pool := x509.NewCertPool()
		if !pool.AppendCertsFromPEM(b) {
			return nil, errors.New("invalid MQTT CA")
		}
		tlsConfig.RootCAs = pool
	}
	type incoming struct {
		topic string
		data  []byte
	}
	dataQueue := make(chan incoming, 128)
	controlQueue := make(chan incoming, 64)
	statusQueue := make(chan incoming, 32)
	options := mqtt.NewClientOptions().AddBroker(a.Config.BrokerURL).SetClientID("manager-" + a.Config.InstanceID + "-" + a.Config.NodeID).SetUsername(a.Config.BrokerUser).SetPassword(a.Config.BrokerPassword).SetTLSConfig(tlsConfig).SetCleanSession(false).SetAutoReconnect(true).SetConnectRetry(false).SetOrderMatters(false).SetConnectTimeout(10 * time.Second).SetWriteTimeout(10 * time.Second).SetKeepAlive(30 * time.Second)
	handler := func(_ mqtt.Client, m mqtt.Message) {
		if len(m.Payload()) > 2<<20 {
			return
		}
		packet := incoming{m.Topic(), append([]byte(nil), m.Payload()...)}
		queue := dataQueue
		if strings.HasSuffix(packet.topic, "/command-results") {
			queue = controlQueue
		} else if strings.HasSuffix(packet.topic, "/status") {
			queue = statusQueue
		}
		select {
		case queue <- packet:
		default:
			a.Logger.Warn("gateway MQTT ingress full; gateway must retry until application ACK")
		}
	}
	// A persistent session can deliver queued messages before SUBSCRIBE completes.
	options.SetDefaultPublishHandler(handler)
	options.SetOnConnectHandler(func(c mqtt.Client) {
		topic := "gw/v1/" + a.Config.NamespaceID + "/+/"
		subs := map[string]byte{topic + "telemetry": 1, topic + "command-results": 1, topic + "status": 1}
		token := c.SubscribeMultiple(subs, handler)
		if !token.WaitTimeout(10*time.Second) || token.Error() != nil {
			a.Logger.Error("gateway MQTT subscribe failed")
		}
	})
	client := mqtt.NewClient(options)
	a.Publish = func(ctx context.Context, topic string, payload any) error {
		if !client.IsConnectionOpen() {
			return errors.New("gateway broker disconnected")
		}
		b, e := json.Marshal(payload)
		if e != nil {
			return e
		}
		token := client.Publish(topic, 1, false, b)
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-token.Done():
			return token.Error()
		case <-time.After(12 * time.Second):
			return errors.New("gateway MQTT publish timeout")
		}
	}
	run := func(ctx context.Context) {
		defer client.Disconnect(500)
		var workers sync.WaitGroup
		for _, queue := range []chan incoming{controlQueue, statusQueue, dataQueue} {
			workers.Add(1)
			go func(q <-chan incoming) {
				defer workers.Done()
				for {
					select {
					case <-ctx.Done():
						return
					case packet := <-q:
						if ctx.Err() != nil {
							return
						}
						if e := a.handleGatewayMessage(ctx, packet.topic, packet.data); e != nil && !errors.Is(e, context.Canceled) {
							a.Logger.Warn("gateway MQTT message rejected", "topic", packet.topic, "error", e)
						}
					}
				}
			}(queue)
		}
		defer workers.Wait()
		retry := time.NewTicker(5 * time.Second)
		defer retry.Stop()
		connect := func() {
			if ctx.Err() != nil || client.IsConnectionOpen() {
				return
			}
			t := client.Connect()
			select {
			case <-ctx.Done():
				return
			case <-time.After(12 * time.Second):
				a.Logger.Warn("gateway broker connection timeout")
			case <-t.Done():
				if t.Error() != nil {
					a.Logger.Warn("gateway broker connection failed")
				}
			}
		}
		connect()
		for {
			select {
			case <-ctx.Done():
				return
			case <-retry.C:
				if !client.IsConnected() {
					connect()
				}
			}
		}
	}
	return run, nil
}

func (a *App) handleGatewayMessage(ctx context.Context, topic string, payload []byte) error {
	parts := strings.Split(topic, "/")
	if len(parts) != 5 || parts[0] != "gw" || parts[1] != "v1" || parts[2] != a.Config.NamespaceID || !artifactIDPattern.MatchString(parts[3]) {
		return errors.New("invalid gateway MQTT topic")
	}
	gateway := parts[3]
	var state string
	if e := a.Management.QueryRowContext(ctx, "SELECT lifecycle FROM gateways WHERE gateway_id=?", gateway).Scan(&state); e != nil || state != "active" {
		return errors.New("unregistered or inactive MQTT gateway")
	}
	var m map[string]any
	if json.Unmarshal(payload, &m) != nil {
		return errors.New("invalid MQTT JSON")
	}
	if e := ctlNamespace(a, m, gateway); e != nil {
		return e
	}
	var response map[string]any
	var e error
	suffix := ""
	switch parts[4] {
	case "telemetry":
		response, e = a.AcceptIngress(ctx, a.Config.NamespaceID, gateway, m)
		suffix = "telemetry-ack"
	case "command-results":
		response, e = a.AcceptTaskResults(ctx, gateway, m)
		suffix = "result-ack"
	case "status":
		return a.RecordGatewayStatus(ctx, gateway, m)
	default:
		return errors.New("unsupported gateway MQTT topic")
	}
	if e != nil {
		return e
	}
	return a.Publish(ctx, fmt.Sprintf("gw/v1/%s/%s/%s", a.Config.NamespaceID, gateway, suffix), response)
}
