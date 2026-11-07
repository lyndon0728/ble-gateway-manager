package platform

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"

	"github.com/eclipse/paho.mqtt.golang/packets"
)

// This MQTT 3.1.1 wire peer tests the real Paho adapter without requiring an
// external broker. It does not replace deployment broker TLS/ACL certification.
func TestGatewayMQTTControlSurvivesBlockedTelemetry(t *testing.T) {
	a, actor := controlFixture(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	task := controlNewTask(t, a, actor, "mqtt-result", "REBOOT", map[string]any{"reason": "wire test"})
	if e := a.ensurePipelineDefault(ctx); e != nil {
		t.Fatal(e)
	}
	a.Pipeline.SetMaxOpenConns(1)
	blocked, e := a.Pipeline.BeginTx(ctx, nil)
	if e != nil {
		t.Fatal(e)
	}
	defer blocked.Rollback()
	listener, e := net.Listen("tcp", "127.0.0.1:0")
	if e != nil {
		t.Fatal(e)
	}
	defer listener.Close()
	a.Config.BrokerURL = "tcp://" + listener.Addr().String()
	received := make(chan map[string]any, 1)
	failures := make(chan error, 1)
	peerDone := make(chan struct{})
	go func() {
		defer close(peerDone)
		e := func() error {
			conn, e := listener.Accept()
			if e != nil {
				return e
			}
			defer conn.Close()
			_ = conn.SetDeadline(time.Now().Add(10 * time.Second))
			for {
				packet, e := packets.ReadPacket(conn)
				if e != nil {
					return e
				}
				switch p := packet.(type) {
				case *packets.ConnectPacket:
					reply := packets.NewControlPacket(packets.Connack).(*packets.ConnackPacket)
					reply.ReturnCode = 0
					if e = reply.Write(conn); e != nil {
						return e
					}
				case *packets.SubscribePacket:
					reply := packets.NewControlPacket(packets.Suback).(*packets.SubackPacket)
					reply.MessageID = p.MessageID
					for range p.Topics {
						reply.ReturnCodes = append(reply.ReturnCodes, 1)
					}
					if e = reply.Write(conn); e != nil {
						return e
					}
					batch := map[string]any{"schemaVersion": "1.0", "namespaceId": a.Config.NamespaceID, "gatewayId": "gw_test", "batchId": "wire_batch", "events": []any{map[string]any{"eventId": "wire:1", "capturedAt": Timestamp(Now()), "timeQuality": "SYNCED", "sourceDeviceId": "sensor", "payloadBase64": "AA==", "sequence": 1}}}
					message := packets.NewControlPacket(packets.Publish).(*packets.PublishPacket)
					message.Qos = 1
					message.MessageID = 100
					message.TopicName = "gw/v1/" + a.Config.NamespaceID + "/gw_test/telemetry"
					message.Payload = []byte(encode(batch))
					if e = message.Write(conn); e != nil {
						return e
					}
					deadline := time.Now().Add(3 * time.Second)
					for a.Pipeline.Stats().WaitCount == 0 && time.Now().Before(deadline) {
						time.Sleep(5 * time.Millisecond)
					}
					if a.Pipeline.Stats().WaitCount == 0 {
						return fmt.Errorf("telemetry did not reach blocked pipeline transaction")
					}
					message = packets.NewControlPacket(packets.Publish).(*packets.PublishPacket)
					message.Qos = 1
					message.MessageID = 101
					message.TopicName = "gw/v1/" + a.Config.NamespaceID + "/gw_test/command-results"
					message.Payload = []byte(encode(controlBatch(a, "gw_test", controlResult(Str(task, "taskId"), "wire_result", 3, "SUCCEEDED", "VERIFIED", map[string]any{"bootId": "wire_boot"}))))
					if e = message.Write(conn); e != nil {
						return e
					}
				case *packets.PublishPacket:
					if strings.HasSuffix(p.TopicName, "/result-ack") {
						var ack map[string]any
						if e = json.Unmarshal(p.Payload, &ack); e != nil {
							return e
						}
						received <- ack
					}
					if p.Qos == 1 {
						reply := packets.NewControlPacket(packets.Puback).(*packets.PubackPacket)
						reply.MessageID = p.MessageID
						if e = reply.Write(conn); e != nil {
							return e
						}
					}
				case *packets.PingreqPacket:
					if e = packets.NewControlPacket(packets.Pingresp).Write(conn); e != nil {
						return e
					}
				case *packets.DisconnectPacket:
					return nil
				}
			}
		}()
		if e != nil && ctx.Err() == nil {
			failures <- e
		}
	}()
	run, e := a.prepareGatewayBroker()
	if e != nil {
		t.Fatal(e)
	}
	finished := make(chan struct{})
	go func() { run(ctx); close(finished) }()
	select {
	case ack := <-received:
		if controlAckStatus(ack) != "ACCEPTED" {
			t.Fatalf("unexpected MQTT ACK %v", ack)
		}
		var count int
		if e = a.Management.QueryRow("SELECT count(*) FROM command_results WHERE result_id='wire_result'").Scan(&count); e != nil || count != 1 {
			t.Fatalf("MQTT ACK preceded durable result %d %v", count, e)
		}
	case e := <-failures:
		t.Fatal(e)
	case <-time.After(5 * time.Second):
		t.Fatal("blocked telemetry starved control result processing")
	}
	cancel()
	select {
	case <-finished:
	case <-time.After(2 * time.Second):
		t.Fatal("MQTT workers failed to stop with blocked data transaction")
	}
	select {
	case <-peerDone:
	case <-time.After(time.Second):
		t.Fatal("MQTT connection not closed")
	}
}
