package platform

import "net/http"

// Resolve an image already measured by the device; this lookup does not certify
// a running image and never upgrades VERSION_ONLY inventory by itself.
func (a *App) componentArtifact(w http.ResponseWriter, r *http.Request) {
	id, ok := a.networkDevice(w, r)
	if !ok {
		return
	}
	component, hash := r.URL.Query().Get("component"), r.URL.Query().Get("sha256")
	if component != "GATEWAY" && component != "NRF54" || !artifactHashPattern.MatchString(hash) {
		Fail(w, r, 422, "INVALID_COMPONENT_IMAGE", "component and lowercase SHA256 required")
		return
	}
	g, e := a.gatewayView(r.Context(), a.Management, id)
	if e == nil {
		e = gw32Require(g, "COMPONENT_INVENTORY_1")
	}
	if e != nil {
		RespondError(w, r, e)
		return
	}
	rows, e := a.Management.QueryContext(r.Context(), "SELECT artifact_id,version,size_bytes,compatibility_json FROM artifacts WHERE kind='firmware' AND state='verified' AND sha256=? ORDER BY artifact_id", hash)
	if e != nil {
		RespondError(w, r, e)
		return
	}
	defer rows.Close()
	for rows.Next() {
		var artifact, version, raw string
		var size int64
		if e = rows.Scan(&artifact, &version, &size, &raw); e != nil {
			RespondError(w, r, e)
			return
		}
		m := pm(raw)
		target, err := firmwareComponent(m)
		if err != nil || target != component || Str(m, "sha256") != hash {
			continue
		}
		match := func(key, value string) bool {
			list := List(m, key)
			if len(list) == 0 {
				return key != "compatibleModels"
			}
			for _, x := range list {
				if x == value {
					return true
				}
			}
			return false
		}
		if !match("compatibleModels", Str(g, "model")) || !match("hardwareVersions", Str(g, "hardwareVersion")) {
			continue
		}
		JSON(w, 200, map[string]any{"artifactId": artifact, "component": component, "firmwareVersion": version, "imageSha256": hash, "sizeBytes": size})
		return
	}
	if e = rows.Err(); e != nil {
		RespondError(w, r, e)
		return
	}
	Fail(w, r, 404, "COMPONENT_ARTIFACT_NOT_FOUND", "matching verified firmware artifact has not been imported")
}
