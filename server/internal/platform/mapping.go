package platform

import (
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"regexp"
)

var mappingKey = regexp.MustCompile(`^[A-Za-z][A-Za-z0-9_]{0,63}$`)

// Mapping changes only the customer data object. Event identities, revisions and
// delivery identifiers remain server-owned and cannot be rewritten by a mapping.
func ValidateOutputMapping(config map[string]any) error {
	raw, present := config["mapping"]
	if !present {
		if Str(config, "mappingVersion") != "1" && Str(config, "mappingVersion") != "identity-1" {
			return errors.New("custom mappingVersion requires a frozen mapping definition")
		}
		return nil
	}
	m, ok := raw.(map[string]any)
	if !ok {
		return errors.New("mapping must be an object")
	}
	if Str(config, "mappingVersion") == "1" || Str(config, "mappingVersion") == "identity-1" {
		return errors.New("identity versions cannot contain a custom mapping")
	}
	if len(List(m, "fields")) < 1 || len(List(m, "fields")) > 64 {
		return errors.New("mapping requires 1..64 fields")
	}
	if len(Obj(m, "constants")) > 32 {
		return errors.New("mapping permits at most 32 constants")
	}
	if k := Str(m, "wrapKey"); k != "" && !mappingKey.MatchString(k) {
		return errors.New("invalid mapping wrapKey")
	}
	seen := map[string]bool{}
	claim := func(k string) error {
		if !mappingKey.MatchString(k) || seen[k] {
			return fmt.Errorf("invalid or duplicate mapping output field %q", k)
		}
		seen[k] = true
		return nil
	}
	for _, item := range List(m, "fields") {
		f, ok := item.(map[string]any)
		if !ok {
			return errors.New("mapping field must be an object")
		}
		if !mappingKey.MatchString(Str(f, "source")) {
			return errors.New("mapping source must be a top-level field name")
		}
		if e := claim(Str(f, "target")); e != nil {
			return e
		}
		for _, k := range []string{"scale", "offset"} {
			if v, ok := f[k]; ok {
				n, valid := mappingNumber(v)
				if !valid || math.Abs(n) > 1000000 {
					return errors.New("mapping scale and offset must be finite numbers within +/-1000000")
				}
			}
		}
		if unit := Str(f, "unit"); unit != "" {
			if len(unit) > 32 {
				return errors.New("unit label too long")
			}
			if e := claim(Str(f, "target") + "Unit"); e != nil {
				return e
			}
		}
	}
	for k, v := range Obj(m, "constants") {
		if e := claim(k); e != nil {
			return e
		}
		switch x := v.(type) {
		case nil, bool:
		case string:
			if len(x) > 256 {
				return errors.New("mapping constant string too long")
			}
		default:
			if _, ok := mappingNumber(x); !ok {
				return errors.New("mapping constants must be scalar JSON values")
			}
		}
	}
	return nil
}
func mappingNumber(v any) (float64, bool) {
	var n float64
	switch x := v.(type) {
	case float64:
		n = x
	case float32:
		n = float64(x)
	case int:
		n = float64(x)
	case int64:
		n = float64(x)
	case int32:
		n = float64(x)
	case uint64:
		n = float64(x)
	case uint32:
		n = float64(x)
	case json.Number:
		y, e := x.Float64()
		if e != nil {
			return 0, false
		}
		n = y
	default:
		return 0, false
	}
	return n, !math.IsNaN(n) && !math.IsInf(n, 0)
}
func MapOutputData(data map[string]any, config map[string]any) (map[string]any, error) {
	if e := ValidateOutputMapping(config); e != nil {
		return nil, &ParserFailure{"MAPPING_INVALID", e.Error()}
	}
	b, e := json.Marshal(data)
	if e != nil {
		return nil, e
	}
	var source map[string]any
	if e = json.Unmarshal(b, &source); e != nil {
		return nil, e
	}
	m := Obj(config, "mapping")
	if len(m) == 0 {
		return source, nil
	}
	out := map[string]any{}
	for _, item := range List(m, "fields") {
		f := item.(map[string]any)
		key, target := Str(f, "source"), Str(f, "target")
		v, found := source[key]
		if !found {
			return nil, &ParserFailure{"MAPPING_FIELD_MISSING", "required source field " + key + " is absent"}
		}
		_, scaleSet := f["scale"]
		_, offsetSet := f["offset"]
		if scaleSet || offsetSet {
			n, ok := mappingNumber(v)
			if !ok {
				return nil, &ParserFailure{"MAPPING_TYPE_MISMATCH", "numeric source required for " + key}
			}
			scale, offset := 1.0, 0.0
			if scaleSet {
				scale, _ = mappingNumber(f["scale"])
			}
			if offsetSet {
				offset, _ = mappingNumber(f["offset"])
			}
			v = n*scale + offset
			if n, ok = mappingNumber(v); !ok {
				return nil, &ParserFailure{"MAPPING_OVERFLOW", "non-finite mapped result"}
			}
		}
		out[target] = v
		if unit := Str(f, "unit"); unit != "" {
			out[target+"Unit"] = unit
		}
	}
	for k, v := range Obj(m, "constants") {
		out[k] = v
	}
	if key := Str(m, "wrapKey"); key != "" {
		out = map[string]any{key: out}
	}
	return out, nil
}
