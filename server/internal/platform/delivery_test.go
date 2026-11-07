package platform

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

func mqttPacket(header byte, payload []byte) []byte {
	out := []byte{header}
	n := len(payload)
	for {
		v := byte(n % 128)
		n /= 128
		if n > 0 {
			v |= 128
		}
		out = append(out, v)
		if n == 0 {
			break
		}
	}
	return append(out, payload...)
}
func mqttApplicationPacket(topic string, ack pipeMap, retained bool) []byte {
	payload := []byte{byte(len(topic) >> 8), byte(len(topic))}
	payload = append(payload, []byte(topic)...)
	payload = append(payload, []byte(pipeJSON(ack))...)
	header := byte(0x30)
	if retained {
		header |= 1
	}
	return mqttPacket(header, payload)
}
func TestDeliveryMQTTPubackAndRetainedAckDoNotTransferCustody(t *testing.T) {
	a, _ := pipelineFixture(t)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()
	connections := make(chan net.Conn, 1)
	published := make(chan pipeMap, 1)
	var writeMu sync.Mutex
	go func() {
		conn, e := listener.Accept()
		if e != nil {
			return
		}
		connections <- conn
		reader := bufio.NewReader(conn)
		for {
			header, e := reader.ReadByte()
			if e != nil {
				return
			}
			length, multiplier := 0, 1
			for {
				b, e := reader.ReadByte()
				if e != nil {
					return
				}
				length += int(b&127) * multiplier
				if b&128 == 0 {
					break
				}
				multiplier *= 128
				if multiplier > 2097152 {
					return
				}
			}
			body := make([]byte, length)
			if _, e = io.ReadFull(reader, body); e != nil {
				return
			}
			writeMu.Lock()
			switch header >> 4 {
			case 1:
				_, _ = conn.Write([]byte{0x20, 2, 0, 0})
			case 8:
				_, _ = conn.Write([]byte{0x90, 3, body[0], body[1], 1})
			case 3:
				n := int(binary.BigEndian.Uint16(body[:2]))
				offset := 2 + n
				if (header>>1)&3 == 1 {
					_, _ = conn.Write([]byte{0x40, 2, body[offset], body[offset+1]})
					offset += 2
				}
				var batch pipeMap
				_ = json.Unmarshal(body[offset:], &batch)
				published <- batch
				event := pl(batch, "events")[0].(map[string]any)
				ack := pipeMap{"batchId": batch["batchId"], "targetId": event["targetId"], "targetVersion": "1", "durability": "CUSTOMER_INBOX_PERSISTED", "items": []any{pipeMap{"deliveryId": event["deliveryId"], "status": "ACCEPTED"}}}
				_, _ = conn.Write(mqttApplicationPacket("customer/acks", ack, true))
			case 12:
				_, _ = conn.Write([]byte{0xd0, 0})
			case 14:
				writeMu.Unlock()
				return
			}
			writeMu.Unlock()
		}
	}()
	config := pipeMap{"protocol": "MQTT", "endpoint": "mqtt://" + listener.Addr().String(), "ackMode": "MQTT_APPLICATION_ACK", "payloadTopic": "customer/data", "ackTopic": "customer/acks", "mappingVersion": "1", "maxBatchEvents": 200, "maxInflightEvents": 200, "maxQueueBytes": 1048576}
	rule := pipeMap{"ruleId": "mqtt", "priority": 1, "match": pipeMap{"modelIds": []any{"GW"}}, "action": "DECODE", "parserId": "reference_sensor", "parserVersion": "1.0.0", "schemaVersion": "reference-sensor-1", "targetVersions": []any{pipeMap{"targetId": "target_test", "version": "1", "mappingVersion": "1", "required": true}}}
	manifest := pipeMap{"rules": []any{rule}, "targets": pipeMap{"target_test@1": config}, "defaultAction": "QUARANTINE"}
	ctx := context.Background()
	if err = a.activatePipelineBundle(ctx, "bundle_mqtt", pipeJSON(manifest), pipeHash(manifest), 1); err != nil {
		t.Fatal(err)
	}
	_, _ = a.AcceptIngress(ctx, a.Config.NamespaceID, "gw_test", testIngress(a, "evt_mqtt"))
	if _, err = a.ProcessOne(ctx); err != nil {
		t.Fatal(err)
	}
	if _, err = a.DeliverOnce(ctx); err != nil {
		t.Fatal(err)
	}
	conn := <-connections
	defer conn.Close()
	defer a.closeDeliveryClients()
	batch := <-published
	time.Sleep(50 * time.Millisecond)
	var state string
	_ = a.Pipeline.QueryRow("SELECT state FROM delivery_outbox").Scan(&state)
	if state != "awaiting_ack" {
		t.Fatal("PUBACK/retained ACK transferred custody", state)
	}
	event := pl(batch, "events")[0].(map[string]any)
	ack := pipeMap{"batchId": batch["batchId"], "targetId": event["targetId"], "targetVersion": "1", "durability": "CUSTOMER_INBOX_PERSISTED", "items": []any{pipeMap{"deliveryId": event["deliveryId"], "status": "ACCEPTED"}}}
	writeMu.Lock()
	_, err = conn.Write(mqttApplicationPacket("customer/acks", ack, false))
	writeMu.Unlock()
	if err != nil {
		t.Fatal(err)
	}
	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		_ = a.Pipeline.QueryRow("SELECT state FROM delivery_outbox").Scan(&state)
		if state == "delivered" {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatal("application ACK did not transfer custody", state)
}
