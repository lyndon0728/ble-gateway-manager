package platform

import (
	"bytes"
	"context"
	"database/sql"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"math"
	"regexp"
	"strconv"
	"strings"
)

const ProtocolDSLVersion = "1.0"
const ProtocolCompilerVersion = "protocol-dsl-go-1"
const ProtocolDefaultSchema = "protocol-dsl-1"

var protocolIdentifier = regexp.MustCompile("^[A-Za-z][A-Za-z0-9_]{0,63}$")
var protocolResourceID = regexp.MustCompile("^[A-Za-z][A-Za-z0-9_.-]{0,127}$")
var protocolVersionID = regexp.MustCompile("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$")

type protocolSpec struct {
	DSLVersion    string          `json:"dslVersion"`
	InputKind     string          `json:"inputKind"`
	SchemaVersion string          `json:"schemaVersion,omitempty"`
	Description   string          `json:"description,omitempty"`
	Length        *int            `json:"length,omitempty"`
	MinLength     *int            `json:"minLength,omitempty"`
	MaxLength     *int            `json:"maxLength,omitempty"`
	Source        *protocolSource `json:"source,omitempty"`
	Fields        []protocolField `json:"fields"`
	Checks        []protocolCheck `json:"checks,omitempty"`
}
type protocolSource struct {
	ADType      int    `json:"adType"`
	CompanyID   *int   `json:"companyId,omitempty"`
	ServiceUUID string `json:"serviceUuid,omitempty"`
	DataOffset  int    `json:"dataOffset,omitempty"`
}
type protocolField struct {
	Name        string            `json:"name"`
	Offset      int               `json:"offset"`
	Type        string            `json:"type"`
	Endian      string            `json:"endian,omitempty"`
	Length      int               `json:"length,omitempty"`
	BitOffset   *int              `json:"bitOffset,omitempty"`
	BitLength   *int              `json:"bitLength,omitempty"`
	Scale       *float64          `json:"scale,omitempty"`
	Bias        *float64          `json:"bias,omitempty"`
	Min         *float64          `json:"min,omitempty"`
	Max         *float64          `json:"max,omitempty"`
	Enum        map[string]string `json:"enum,omitempty"`
	Unit        string            `json:"unit,omitempty"`
	Evidence    string            `json:"evidence,omitempty"`
	Description string            `json:"description,omitempty"`
}
type protocolCheck struct {
	Type   string `json:"type"`
	Offset int    `json:"offset"`
	Hex    string `json:"hex,omitempty"`
	Start  int    `json:"start,omitempty"`
	End    int    `json:"end,omitempty"`
	Endian string `json:"endian,omitempty"`
}

func protocolBad(code, detail string) error { return &ParserFailure{Code: code, Detail: detail} }
func protocolFieldBytes(f protocolField) int {
	switch f.Type {
	case "uint8", "int8":
		return 1
	case "uint16", "int16":
		return 2
	case "uint32", "int32":
		return 4
	case "bytes":
		return f.Length
	}
	return 0
}
func protocolLimits(s protocolSpec) (int, int) {
	if s.Length != nil {
		return *s.Length, *s.Length
	}
	if s.MinLength == nil || s.MaxLength == nil {
		return 0, 0
	}
	return *s.MinLength, *s.MaxLength
}
func decodeProtocolSpec(value map[string]any) (protocolSpec, error) {
	var s protocolSpec
	raw, e := json.Marshal(value)
	if e != nil || len(raw) > 256*1024 {
		return s, protocolBad("DSL_INVALID", "invalid or oversized specification")
	}
	d := json.NewDecoder(bytes.NewReader(raw))
	d.DisallowUnknownFields()
	if e = d.Decode(&s); e != nil {
		return s, protocolBad("DSL_INVALID", e.Error())
	}
	normalized := pm(string(raw))
	if protocolHasNull(normalized) {
		return s, protocolBad("DSL_INVALID", "unknown values must remain outside an executable specification; null is not a protocol value")
	}
	for _, fv := range pl(normalized, "fields") {
		f, _ := fv.(map[string]any)
		for _, key := range []string{"name", "offset", "type"} {
			if _, exists := f[key]; !exists {
				return s, protocolBad("DSL_INVALID", "field requires explicit "+key)
			}
		}
	}
	if s.Source != nil {
		if _, exists := po(normalized, "source")["adType"]; !exists {
			return s, protocolBad("DSL_INVALID", "source requires explicit adType")
		}
	}
	for _, cv := range pl(normalized, "checks") {
		c, _ := cv.(map[string]any)
		if _, exists := c["offset"]; !exists {
			return s, protocolBad("DSL_INVALID", "check requires explicit offset")
		}
		if ps(c, "type") == "crc16-ccitt-false" {
			if _, exists := c["start"]; !exists {
				return s, protocolBad("DSL_INVALID", "CRC check requires explicit start")
			}
		}
	}
	if s.DSLVersion != ProtocolDSLVersion {
		return s, protocolBad("DSL_VERSION_UNSUPPORTED", "dslVersion must be 1.0")
	}
	if s.InputKind != "RAW" && s.InputKind != "BLE_AD" && s.InputKind != "GATT_VALUE" {
		return s, protocolBad("DSL_INVALID", "inputKind must be RAW, BLE_AD or GATT_VALUE")
	}
	if s.SchemaVersion != "" && !protocolVersionID.MatchString(s.SchemaVersion) {
		return s, protocolBad("DSL_INVALID", "invalid schemaVersion")
	}
	if len(s.Description) > 4096 || len(s.Fields) < 1 || len(s.Fields) > 128 || len(s.Checks) > 16 {
		return s, protocolBad("DSL_LIMIT", "description, fields or checks exceeds limit")
	}
	if s.Length != nil && (s.MinLength != nil || s.MaxLength != nil) {
		return s, protocolBad("DSL_INVALID", "use length or minLength/maxLength, not both")
	}
	min, max := protocolLimits(s)
	if min < 1 || max < min || max > 65536 {
		return s, protocolBad("DSL_INVALID", "length bounds must be 1..65536")
	}
	if (s.InputKind == "BLE_AD" && max > 1650) || (s.InputKind == "GATT_VALUE" && max > 512) {
		return s, protocolBad("DSL_INVALID", "length exceeds current BLE capture capability")
	}
	if s.Source != nil {
		x := s.Source
		if s.InputKind != "BLE_AD" || x.ADType < 0 || x.ADType > 255 || x.DataOffset < 0 || x.DataOffset > 1650 {
			return s, protocolBad("DSL_INVALID", "invalid BLE AD source")
		}
		if x.CompanyID != nil && (x.ADType != 255 || *x.CompanyID < 0 || *x.CompanyID > 65535) {
			return s, protocolBad("DSL_INVALID", "companyId requires manufacturer AD type 255")
		}
		if x.ServiceUUID != "" {
			if (x.ADType != 0x16 && x.ADType != 0x20 && x.ADType != 0x21) || gw32CanonicalUUID(x.ServiceUUID) == "" {
				return s, protocolBad("DSL_INVALID", "serviceUuid requires service data AD type")
			}
		}
	}
	names := map[string]bool{}
	for _, f := range s.Fields {
		n := protocolFieldBytes(f)
		if !protocolIdentifier.MatchString(f.Name) || names[f.Name] || n < 1 || n > 4096 || f.Offset < 0 || f.Offset > max-n {
			return s, protocolBad("DSL_INVALID", "invalid, duplicated or out-of-bounds field "+f.Name)
		}
		names[f.Name] = true
		if len(f.Unit) > 32 || len(f.Evidence) > 1024 || len(f.Description) > 2048 {
			return s, protocolBad("DSL_LIMIT", "field metadata too long")
		}
		if f.Type != "bytes" && f.Length != 0 {
			return s, protocolBad("DSL_INVALID", "length is only valid for bytes")
		}
		if n > 1 && f.Type != "bytes" && f.Endian != "little" && f.Endian != "big" {
			return s, protocolBad("DSL_INVALID", "numeric multi-byte field requires endian")
		}
		if f.Endian != "" && f.Endian != "little" && f.Endian != "big" {
			return s, protocolBad("DSL_INVALID", "invalid endian")
		}
		if f.Type == "bytes" && (f.BitOffset != nil || f.BitLength != nil || f.Scale != nil || f.Bias != nil || f.Min != nil || f.Max != nil || len(f.Enum) > 0 || f.Unit != "" || f.Endian != "") {
			return s, protocolBad("DSL_INVALID", "bytes cannot have numeric options")
		}
		if (f.BitOffset == nil) != (f.BitLength == nil) {
			return s, protocolBad("DSL_INVALID", "bitOffset and bitLength must be paired")
		}
		if f.BitLength != nil && (strings.HasPrefix(f.Type, "int") || *f.BitLength < 1 || *f.BitLength > n*8 || *f.BitOffset < 0 || *f.BitOffset > n*8-*f.BitLength) {
			return s, protocolBad("DSL_INVALID", "invalid unsigned bit field")
		}
		for _, v := range []*float64{f.Scale, f.Bias, f.Min, f.Max} {
			if v != nil && (math.IsNaN(*v) || math.IsInf(*v, 0) || math.Abs(*v) > 1e15) {
				return s, protocolBad("DSL_INVALID", "numeric options must be finite and bounded")
			}
		}
		if f.Min != nil && f.Max != nil && *f.Min > *f.Max {
			return s, protocolBad("DSL_INVALID", "field min exceeds max")
		}
		if len(f.Enum) > 256 {
			return s, protocolBad("DSL_LIMIT", "too many enum values")
		}
		if len(f.Enum) > 0 && (f.Scale != nil || f.Bias != nil) {
			return s, protocolBad("DSL_INVALID", "enum cannot use scale or bias")
		}
		for k, v := range f.Enum {
			if n, err := strconv.ParseInt(k, 10, 64); err != nil || strconv.FormatInt(n, 10) != k || len(v) > 128 {
				return s, protocolBad("DSL_INVALID", "enum keys must be integer strings and labels <=128 bytes")
			}
		}
	}
	for _, f := range s.Fields {
		if f.Unit != "" {
			if names[f.Name+"Unit"] {
				return s, protocolBad("DSL_INVALID", "unit output collides with field")
			}
			names[f.Name+"Unit"] = true
		}
	}
	for _, c := range s.Checks {
		switch c.Type {
		case "equals":
			b, err := hex.DecodeString(c.Hex)
			if err != nil || len(b) < 1 || len(b) > 256 || c.Offset < 0 || c.Offset > max-len(b) || c.Start != 0 || c.End != 0 || c.Endian != "" {
				return s, protocolBad("DSL_INVALID", "invalid equals check")
			}
		case "crc16-ccitt-false":
			if c.Hex != "" || c.Start < 0 || c.End <= c.Start || c.End > max || c.Offset < 0 || c.Offset > max-2 || (c.Endian != "little" && c.Endian != "big") || (c.Offset < c.End && c.Offset+2 > c.Start) {
				return s, protocolBad("DSL_INVALID", "invalid CRC16 range or checksum offset")
			}
		default:
			return s, protocolBad("DSL_UNSUPPORTED", "unsupported check algorithm")
		}
	}
	return s, nil
}

func protocolHasNull(v any) bool {
	if v == nil {
		return true
	}
	switch x := v.(type) {
	case map[string]any:
		for _, value := range x {
			if protocolHasNull(value) {
				return true
			}
		}
	case []any:
		for _, value := range x {
			if protocolHasNull(value) {
				return true
			}
		}
	}
	return false
}

// ValidateProtocolSpec accepts a bounded declarative grammar, never executable code.
func ValidateProtocolSpec(spec map[string]any) error { _, e := decodeProtocolSpec(spec); return e }

func protocolSourceBytes(s protocolSpec, raw []byte) ([]byte, error) {
	if len(raw) > 65536 {
		return nil, protocolBad("PAYLOAD_TOO_LARGE", "payload exceeds 64 KiB")
	}
	if s.InputKind == "BLE_AD" && len(raw) > 1650 {
		return nil, protocolBad("PAYLOAD_TOO_LARGE", "BLE advertising input exceeds 1650 bytes")
	}
	if s.Source == nil {
		return raw, nil
	}
	var matched []byte
	count := 0
	selector := s.Source
	for pos := 0; pos < len(raw); {
		size := int(raw[pos])
		pos++
		if size == 0 {
			for _, b := range raw[pos:] {
				if b != 0 {
					return nil, protocolBad("AD_TRAILING_DATA", "nonzero trailing AD bytes")
				}
			}
			break
		}
		if pos+size > len(raw) {
			return nil, protocolBad("AD_TRUNCATED", "AD length exceeds payload")
		}
		adType := int(raw[pos])
		value := raw[pos+1 : pos+size]
		pos += size
		if adType != selector.ADType {
			continue
		}
		if selector.CompanyID != nil && (len(value) < 2 || int(binary.LittleEndian.Uint16(value[:2])) != *selector.CompanyID) {
			continue
		}
		if selector.ServiceUUID != "" {
			width := map[int]int{0x16: 2, 0x20: 4, 0x21: 16}[adType]
			if len(value) < width {
				return nil, protocolBad("AD_TRUNCATED", "service data UUID truncated")
			}
			if normalizedServiceUUID(value[:width]) != gw32CanonicalUUID(selector.ServiceUUID) {
				continue
			}
		}
		if len(value) < selector.DataOffset {
			return nil, protocolBad("INVALID_LENGTH", "AD dataOffset exceeds value")
		}
		matched = value[selector.DataOffset:]
		count++
	}
	if count == 0 {
		return nil, protocolBad("SOURCE_UNMATCHED", "no matching AD structure")
	}
	if count > 1 {
		return nil, protocolBad("SOURCE_AMBIGUOUS", "multiple matching AD structures")
	}
	return matched, nil
}

// ParseProtocolSpec deterministically executes the exact reviewed specification.
func ParseProtocolSpec(spec map[string]any, raw []byte) ([]ParsedRecord, error) {
	s, e := decodeProtocolSpec(spec)
	if e != nil {
		return nil, e
	}
	data, e := protocolSourceBytes(s, raw)
	if e != nil {
		return nil, e
	}
	min, max := protocolLimits(s)
	if len(data) < min || len(data) > max {
		return nil, protocolBad("INVALID_LENGTH", "selected payload does not meet protocol length")
	}
	for _, c := range s.Checks {
		if c.Type == "equals" {
			want, _ := hex.DecodeString(c.Hex)
			if c.Offset+len(want) > len(data) || !bytes.Equal(data[c.Offset:c.Offset+len(want)], want) {
				return nil, protocolBad("MAGIC_MISMATCH", "constant field did not match")
			}
		}
		if c.Type == "crc16-ccitt-false" {
			if c.End > len(data) || c.Offset+2 > len(data) {
				return nil, protocolBad("INVALID_LENGTH", "checksum range exceeds actual payload")
			}
			var order binary.ByteOrder = binary.BigEndian
			if c.Endian == "little" {
				order = binary.LittleEndian
			}
			if order.Uint16(data[c.Offset:c.Offset+2]) != ReferenceCRC16(data[c.Start:c.End]) {
				return nil, protocolBad("CRC_MISMATCH", "CRC16-CCITT-FALSE did not match")
			}
		}
	}
	out := map[string]any{}
	for _, f := range s.Fields {
		n := protocolFieldBytes(f)
		if f.Offset+n > len(data) {
			return nil, protocolBad("INVALID_LENGTH", "field "+f.Name+" exceeds actual payload")
		}
		b := data[f.Offset : f.Offset+n]
		if f.Type == "bytes" {
			out[f.Name] = hex.EncodeToString(b)
			continue
		}
		var order binary.ByteOrder = binary.BigEndian
		if f.Endian == "little" {
			order = binary.LittleEndian
		}
		var u uint64
		switch n {
		case 1:
			u = uint64(b[0])
		case 2:
			u = uint64(order.Uint16(b))
		case 4:
			u = uint64(order.Uint32(b))
		}
		if f.BitLength != nil {
			u = (u >> *f.BitOffset) & ((uint64(1) << *f.BitLength) - 1)
		}
		value := float64(u)
		switch f.Type {
		case "int8":
			value = float64(int8(u))
		case "int16":
			value = float64(int16(u))
		case "int32":
			value = float64(int32(u))
		}
		rawValue := value
		if f.Scale != nil {
			value *= *f.Scale
		}
		if f.Bias != nil {
			value += *f.Bias
		}
		if math.IsNaN(value) || math.IsInf(value, 0) || (f.Min != nil && value < *f.Min) || (f.Max != nil && value > *f.Max) {
			return nil, protocolBad("VALUE_OUT_OF_RANGE", "field "+f.Name+" outside protocol bounds")
		}
		if len(f.Enum) > 0 {
			label, ok := f.Enum[strconv.FormatInt(int64(rawValue), 10)]
			if !ok {
				return nil, protocolBad("ENUM_UNKNOWN", "field "+f.Name+" has unknown enum value")
			}
			out[f.Name] = label
		} else {
			out[f.Name] = value
		}
		if f.Unit != "" {
			out[f.Name+"Unit"] = f.Unit
		}
	}
	return []ParsedRecord{{Key: "0", Data: out, Quality: []string{}}}, nil
}

func protocolSchema(spec map[string]any) string {
	if v := ps(spec, "schemaVersion"); v != "" {
		return v
	}
	return ProtocolDefaultSchema
}
func protocolDigest(spec map[string]any) string {
	return pipeHash(pipeMap{"compiler": ProtocolCompilerVersion, "spec": spec})
}

// RegisterAIParser is called inside the same management transaction as publication.
func RegisterAIParser(ctx context.Context, tx *sql.Tx, id, version string, spec map[string]any, scopeIDs ...string) error {
	scope := "scope_instance"
	if len(scopeIDs) > 1 {
		return errors.New("one parser scope required")
	}
	if len(scopeIDs) == 1 {
		scope = scopeIDs[0]
	}
	if !protocolResourceID.MatchString(scope) {
		return errors.New("invalid parser scope")
	}
	if !protocolResourceID.MatchString(id) || !protocolVersionID.MatchString(version) {
		return errors.New("invalid parser identity")
	}
	for _, p := range parserCatalog() {
		if ps(p, "parserId") == id {
			return errors.New("cannot replace a built-in parser")
		}
	}
	if e := ValidateProtocolSpec(spec); e != nil {
		return e
	}
	var digest, savedScope string
	e := tx.QueryRowContext(ctx, "SELECT implementation_digest,scope_id FROM protocol_parsers WHERE parser_id=? AND version=?", id, version).Scan(&digest, &savedScope)
	if e == nil {
		if digest == protocolDigest(spec) && savedScope == scope {
			return nil
		}
		return errors.New("parser version is immutable")
	}
	if !errors.Is(e, sql.ErrNoRows) {
		return e
	}
	_, e = tx.ExecContext(ctx, "INSERT INTO protocol_parsers(parser_id,version,schema_version,scope_id,spec_json,implementation_digest,compiler_version,created_at_ms) VALUES(?,?,?,?,?,?,?,?)", id, version, protocolSchema(spec), scope, pipeJSON(spec), protocolDigest(spec), ProtocolCompilerVersion, Now())
	return e
}
