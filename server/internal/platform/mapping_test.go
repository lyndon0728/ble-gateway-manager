package platform

import (
	"encoding/json"
	"math"
	"testing"
)

func mappingFixture() map[string]any {
	return map[string]any{"mappingVersion": "customer-v1", "mapping": map[string]any{"fields": []any{map[string]any{"source": "temperature", "target": "tempF", "scale": 1.8, "offset": 32.0, "unit": "degF"}, map[string]any{"source": "batteryMillivolts", "target": "battery", "scale": 0.001, "unit": "V"}}, "constants": map[string]any{"source": "gateway"}, "wrapKey": "measurements"}}
}
func TestMappingRenameUnitsAndImmutability(t *testing.T) {
	input := map[string]any{"temperature": 25.0, "batteryMillivolts": int64(3300), "sampleSequence": uint64(10)}
	c := mappingFixture()
	v, e := MapOutputData(input, c)
	if e != nil {
		t.Fatal(e)
	}
	m := Obj(v, "measurements")
	if m["tempF"] != 77.0 || math.Abs(m["battery"].(float64)-3.3) > 1e-12 || Str(m, "tempFUnit") != "degF" || Str(m, "source") != "gateway" {
		t.Fatalf("unexpected mapped value %v", v)
	}
	if len(input) != 3 || input["temperature"] != 25.0 {
		t.Fatal("source mutated")
	}
	first := encode(v)
	v2, e := MapOutputData(input, c)
	if e != nil || encode(v2) != first {
		t.Fatal("mapping not deterministic")
	}
	identity, e := MapOutputData(input, map[string]any{"mappingVersion": "identity-1"})
	if e != nil {
		t.Fatal(e)
	}
	identity["temperature"] = 0
	if input["temperature"] != 25.0 {
		t.Fatal("identity mapping aliases source")
	}
}
func TestMappingRejectsAmbiguityAndMissingFields(t *testing.T) {
	c := mappingFixture()
	Obj(c, "mapping")["fields"] = append(List(Obj(c, "mapping"), "fields"), map[string]any{"source": "temperature", "target": "tempFUnit"})
	if ValidateOutputMapping(c) == nil {
		t.Fatal("unit collision accepted")
	}
	if _, e := MapOutputData(map[string]any{"temperature": "25"}, mappingFixture()); e == nil {
		t.Fatal("numeric string silently coerced")
	}
	if _, e := MapOutputData(map[string]any{}, mappingFixture()); e == nil {
		t.Fatal("missing required field silently dropped")
	}
	c = mappingFixture()
	c["mappingVersion"] = "identity-1"
	if ValidateOutputMapping(c) == nil {
		t.Fatal("identity definition overridden")
	}
	c = mappingFixture()
	Obj(c, "mapping")["constants"] = map[string]any{"injected": map[string]any{"arbitrary": "object"}}
	if ValidateOutputMapping(c) == nil {
		t.Fatal("nested arbitrary constant accepted")
	}
}
func TestMappingTargetContract(t *testing.T) {
	a, e := New(Config{DataDir: t.TempDir(), AllowInsecureDev: true})
	if e != nil {
		t.Fatal(e)
	}
	defer a.Close()
	c := mappingFixture()
	for k, v := range map[string]any{"protocol": "HTTP", "endpoint": "https://customer.example.invalid/data", "ackMode": "HTTP_DURABLE_RESPONSE", "maxBatchEvents": 10, "maxInflightEvents": 10, "maxQueueBytes": 1048576} {
		c[k] = v
	}
	if e = a.Validate("TargetVersionCreate", c); e != nil {
		t.Fatal(e)
	}
	b, _ := json.Marshal(c)
	var stored map[string]any
	_ = json.Unmarshal(b, &stored)
	if e = ValidateOutputMapping(stored); e != nil {
		t.Fatal(e)
	}
}
