"""Add AI onboarding management contracts; preserve gateway wire schemas."""
from pathlib import Path
import copy
import json
import re

path = Path(__file__).resolve().parents[1] / "internal/platform/assets/openapi.json"
api = json.loads(path.read_text(encoding="utf-8"))
s = api["components"]["schemas"]
ref = lambda name: {"$ref": "#/components/schemas/" + name}
def string(**kw): return {"type": "string", **kw}
def obj(fields, required=(), extra=False):
    result = {"type": "object", "properties": fields, "additionalProperties": extra}
    if required: result["required"] = list(required)
    return result
def arr(item, maximum=256): return {"type": "array", "items": item, "maxItems": maximum}
ident = string(minLength=1, maxLength=160)
text = string(maxLength=4096)
integer = {"type": "integer", "minimum": 0}
boolean = {"type": "boolean"}
any_object = {"type": "object"}
timestamp = string(format="date-time")
for key in ["peerIds","protocolBindingIds","captureKinds","serviceUuids","characteristicUuids"]:
    s["Rule"]["properties"]["match"]["properties"][key] = {**arr(ident), "uniqueItems": True}
for name, schema in s.items():
    if name == "Parser":
        schema["properties"].update(kind=string(enum=["DECLARATIVE"]), compilerVersion=string(), spec=any_object, scopeId=ident)
def page(name):
    return obj({"items": arr(ref(name), 200), "snapshotAt": timestamp, "nextCursor": string()}, ["items", "snapshotAt"])

provider = {
    "name": string(minLength=1, maxLength=128), "kind": string(enum=["OPENAI_COMPATIBLE", "OLLAMA"]),
    "mode": string(enum=["LOCAL", "CLOUD"]), "baseUrl": string(maxLength=2048),
    "model": string(minLength=1, maxLength=128), "enabled": boolean,
    "allowExternal": boolean, "allowInsecureLocal": boolean, "vision": boolean,
    "schemaMode": string(enum=["JSON_OBJECT", "JSON_SCHEMA"]),
    "tokenParameter": string(enum=["max_tokens","max_completion_tokens"]),
    "timeoutSeconds": {"type": "integer", "minimum": 5, "maximum": 180},
    "maxOutputTokens": {"type": "integer", "minimum": 256, "maximum": 16384},
    "dailyRequestLimit": {"type": "integer", "minimum": 1, "maximum": 10000},
    "apiKey": string(maxLength=8192),
}
s["AIProviderCreate"] = obj(provider, ["name", "kind", "mode", "baseUrl", "model"])
s["AIProviderPatch"] = obj(provider)
s["AIProvider"] = obj({**{k:v for k,v in provider.items() if k != "apiKey"},
    "providerId":ident, "revision": integer, "hasCredential":boolean,
    "lastCheck":any_object, "createdAt":timestamp, "updatedAt":timestamp},
    ["providerId", "revision", "name", "kind", "mode", "model", "hasCredential"], extra=True)
s["AIProviderPage"] = page("AIProvider")
s["AIStatus"] = obj({"enabled":boolean, "configured":boolean, "providers":arr(any_object), "limits":any_object,
    "extraction":any_object, "todayRequests":integer}, ["enabled", "configured", "providers"], extra=True)
s["AIProviderCheck"] = obj({"passed":boolean, "checkedAt":timestamp, "message":text, "usage":any_object}, ["passed"], extra=True)
s["SourceDocument"] = obj({"documentId":ident,"scopeId":ident,"name":text,"mimeType":string(),"sizeBytes":integer,
    "sha256":string(),"extractionState":string(enum=["READY","NEEDS_VISION","UNSUPPORTED"]),"warnings":arr(text),
    "evidenceCount":integer,"createdAt":timestamp}, ["documentId","scopeId","name","extractionState"], extra=True)
s["DocumentEvidencePage"] = obj({"documentId":ident, "items":arr(any_object,2000),"warnings":arr(text)}, ["documentId","items"], extra=True)
context = obj({"pageId":string(maxLength=4),"targetId":ident,"peerId":ident,"returnTo":string(maxLength=1024)})
kind = string(enum=["DEVICE_PROFILE","PARSER_PROFILE","GATT_PROFILE","AUTO_CONNECT","ROUTE_PLAN","CUSTOMER_INTERFACE"])
s["AIImportJobCreate"] = obj({"scopeId":ident,"documentIds":arr(ident,5),"taskKind":kind,"providerId":ident,
    "useModel":boolean,"sourceContext":context}, ["scopeId","documentIds","taskKind"])
s["AIImportJob"] = obj({"jobId":ident,"scopeId":ident,"taskKind":kind,"documentIds":arr(ident,5),"documents":arr(ref("SourceDocument"),5),
    "sourceContext":context,"state":string(),"phase":string(),"providerId":string(),"draftId":string(),
    "errorCode":string(),"message":text,"usage":any_object,"createdAt":timestamp,"updatedAt":timestamp},
    ["jobId","scopeId","taskKind","state","phase"],extra=True)
s["AIImportJobPage"] = page("AIImportJob")
s["ProtocolDraftContent"] = obj({"manufacturer":string(maxLength=256),"modelName":string(maxLength=256),"firmwareRange":string(maxLength=256),
    "parserSpec":{"type":["object","null"]},"connectionSpec":{"type":["object","null"]},"mappingSpec":{"type":["object","null"]},
    "tests":arr(any_object,64),"sources":arr(any_object,256),"issues":arr(any_object,128)},
    ["manufacturer","modelName","firmwareRange","parserSpec","connectionSpec","mappingSpec","tests","sources","issues"], extra=False)
s["ProtocolDraftPatch"] = obj({"content":ref("ProtocolDraftContent")},["content"])
s["ProtocolDraft"] = obj({"draftId":ident,"jobId":ident,"scopeId":ident,"revision":integer,"state":string(),
    "content":ref("ProtocolDraftContent"),"validation":any_object,"publishedModelId":string()}, ["draftId","jobId","revision","state","content"], extra=True)
s["ProtocolValidationCreate"] = obj({"draftRevision":{"type":"integer","minimum":1}}, ["draftRevision"])
s["ProtocolValidation"] = obj({"validationId":ident,"draftRevision":integer,"passed":boolean,"errors":arr(any_object,1024),
    "testResults":arr(any_object,64),"verifiedAt":timestamp}, ["validationId","draftRevision","passed"], extra=True)
s["ProtocolPublicationCreate"] = obj({"draftRevision":{"type":"integer","minimum":1},"validationId":ident,
    "version":string(minLength=1,maxLength=64)}, ["draftRevision","validationId","version"])
s["DeviceModel"] = obj({"modelId":ident,"version":ident,"scopeId":ident,"manufacturer":text,"modelName":text,"firmwareRange":text,
    "content":ref("ProtocolDraftContent"),"parserRef":any_object,"verificationLevel":string(enum=["SAMPLE_VERIFIED","DOCUMENT_REVIEWED"]),
    "state":string(),"createdAt":timestamp}, ["modelId","version","scopeId","content","verificationLevel","state"], extra=True)
s["DeviceModelPage"] = page("DeviceModel")
s["AIApplyPlanCreate"] = obj({"modelId":ident,"sourceContext":context}, ["modelId"])
s["AIApplyPlanExecution"] = obj({"confirmed":{"const":True}},["confirmed"])
s["AIApplyPlan"] = obj({"planId":ident,"modelId":ident,"state":string(),"steps":arr(any_object),"createdAt":timestamp},
    ["planId","modelId","state","steps"],extra=True)
s["ProtocolBindingCreate"] = obj({"peerId":ident,"parserId":ident,"parserVersion":ident,"modelId":ident,"modelVersion":ident},
    ["peerId","parserId","parserVersion"])
s["ProtocolBinding"] = obj({"bindingId":ident,"peerId":ident,"scopeId":ident,"parserId":ident,"parserVersion":ident,"schemaVersion":ident,
    "modelId":string(),"modelVersion":string(),"deviceAddress":string(),"addressType":string(),"createdAt":timestamp},
    ["bindingId","peerId","scopeId","parserId","parserVersion"],extra=True)
s["ProtocolBindingPage"] = page("ProtocolBinding")
s["GATTDiscoverySnapshotPage"] = obj({"items":arr(any_object,200),"snapshotAt":timestamp,"nextCursor":string()},["items","snapshotAt"])
s["AIEmptyRequest"] = obj({})

routes = [
("get","/api/v1/ai/status",None,"AIStatus",200,[],"AI readiness without credentials or provider endpoints."),
("get","/api/v1/ai/providers",None,"AIProviderPage",200,["identity:write"],"Administrator provider configurations, credentials redacted."),
("post","/api/v1/ai/providers","AIProviderCreate","AIProvider",201,["identity:write"],"Create an explicitly configured model service; no inference call."),
("patch","/api/v1/ai/providers/{providerId}","AIProviderPatch","AIProvider",200,["identity:write"],"Update provider policy with If-Match; empty key preserves current credential."),
("post","/api/v1/ai/providers/{providerId}/checks","AIEmptyRequest","AIProviderCheck",200,["identity:write"],"Minimal non-business capability check; may consume provider quota."),
("post","/api/v1/source-documents","multipart","SourceDocument",201,["document:write"],"Upload bounded protocol documentation in the authorized scope."),
("get","/api/v1/source-documents/{documentId}/evidence",None,"DocumentEvidencePage",200,["document:read"],"Document text and provenance; scope is checked."),
("get","/api/v1/source-documents/{documentId}/content",None,"binary",200,["document:read"],"Download the authorized original as attachment."),
("post","/api/v1/ai/import-jobs","AIImportJobCreate","AIImportJob",202,["profile:write"],"Create a persistent import job; useModel=false never invokes inference."),
("get","/api/v1/ai/import-jobs",None,"AIImportJobPage",200,["profile:write"],"Resume scoped imports; no raw document permission is implied."),
("get","/api/v1/ai/import-jobs/{jobId}",None,"AIImportJob",200,["profile:write"],"Current persisted import state and draft reference."),
("post","/api/v1/ai/import-jobs/{jobId}/cancellation","AIEmptyRequest","AIImportJob",200,["profile:write"],"Stop subsequent work; an already dispatched request may still be billed."),
("post","/api/v1/ai/import-jobs/{jobId}/retries","AIEmptyRequest","AIImportJob",202,["profile:write","ai:use"],"Retry a failed job explicitly; preserve human edits."),
("get","/api/v1/protocol-drafts/{draftId}",None,"ProtocolDraft",200,["profile:write"],"Draft and exact revision; not an active runtime configuration."),
("patch","/api/v1/protocol-drafts/{draftId}","ProtocolDraftPatch","ProtocolDraft",200,["profile:write"],"Replace editable draft content with If-Match; invalidate old validation."),
("post","/api/v1/protocol-drafts/{draftId}/validations","ProtocolValidationCreate","ProtocolValidation",200,["profile:write"],"Deterministic protocol validation and independent sample checks."),
("post","/api/v1/protocol-drafts/{draftId}/publications","ProtocolPublicationCreate","DeviceModel",201,["profile:publish"],"Publish exact validated revision and immutable parser; does not activate rules or create GATT tasks."),
("get","/api/v1/device-models",None,"DeviceModelPage",200,["profile:read"],"Authorized published peripheral templates."),
("get","/api/v1/device-models/{modelId}",None,"DeviceModel",200,["profile:read"],"Immutable model version; sample verification does not imply hardware verification."),
("post","/api/v1/ai/apply-plans","AIApplyPlanCreate","AIApplyPlan",201,["profile:read"],"Prepare fixed template parameters for explicit application."),
("get","/api/v1/ai/apply-plans/{planId}",None,"AIApplyPlan",200,["profile:read"],"Read the current user's application plan."),
("post","/api/v1/ai/apply-plans/{planId}/executions","AIApplyPlanExecution","AIApplyPlan",200,["profile:read"],"Confirm and return prepared parameters. Object writes and physical actions require their normal APIs and permissions."),
("get","/api/v1/protocol-bindings",None,"ProtocolBindingPage",200,["gatt:read"],"Immutable bindings visible within peer scopes."),
("post","/api/v1/protocol-bindings","ProtocolBindingCreate","ProtocolBinding",201,["gatt:manage","profile:read"],"Bind the real static peer to an installed parser version; no activation."),
("get","/api/v1/gatt-discovery-snapshots",None,"GATTDiscoverySnapshotPage",200,["gatt:read"],"Persisted historical GATT evidence; no live discovery is performed."),
]
for method,route,request,response,status,permissions,description in routes:
    op=copy.deepcopy(api["paths"]["/api/v1/gateways"]["get"])
    op.update(operationId=method+"_"+re.sub("[^a-zA-Z0-9]","_",route),tags=["AIOnboarding"],summary=description,
              description=description, **{"x-required-permissions":permissions})
    op["parameters"]=[{"name":key,"in":"path","required":True,"schema":ident} for key in re.findall(r"{([^}]+)}",route)]
    if response.endswith("Page") and response != "DocumentEvidencePage":
        op["parameters"] += [{"$ref":"#/components/parameters/"+key} for key in ["Cursor","Limit"]]
    if method == "patch":
        op["parameters"].append({"name":"If-Match","in":"header","required":True,"schema":string()})
    if method not in ["get","head"]:
        op["parameters"].append({"name":"Idempotency-Key","in":"header","required":True,"schema":string(minLength=1,maxLength=128)})
    if route == "/api/v1/protocol-bindings" and method == "get":
        op["parameters"].append({"name":"peerId","in":"query","schema":ident})
    if route.endswith("gatt-discovery-snapshots"):
        op["parameters"] += [{"name":k,"in":"query","schema":ident} for k in ["gatewayId","connectionRef"]]
    op["responses"].pop("200",None)
    content={"application/octet-stream":{"schema":{"type":"string","format":"binary"}}} if response=="binary" else {"application/json":{"schema":ref(response)}}
    op["responses"][str(status)]={"description":"Success","content":content}
    for code in ["400","403","404","409","412","413","422","429","503"]:
        op["responses"].setdefault(code,{"description":"Request rejected; see error code.","content":{"application/json":{"schema":ref("ErrorResponse")}}})
    if response in ["AIProvider","ProtocolDraft"]:
        op["responses"][str(status)]["headers"]={"ETag":{"schema":string(),"description":"Quoted immutable resource revision."}}
    if request:
        schema=obj({"file":{"type":"string","format":"binary"},"scopeId":ident},["file","scopeId"]) if request=="multipart" else ref(request)
        op["requestBody"]={"required":True,"content":{("multipart/form-data" if request=="multipart" else "application/json"):{"schema":schema}}}
    api["paths"].setdefault(route,{})[method]=op
api["info"]["version"]="1.10.0"
api["info"]["description"]="Server 0.4.0: AI-assisted document onboarding, immutable protocol templates and deterministic peripheral routing; GW32 1.0/1.1 wire contracts remain compatible."
# Existing ErrorEnvelope is named by the baseline; reuse its actual reference.
error_ref=api["paths"]["/api/v1/gateways"]["get"]["responses"]["401"]["content"]["application/json"]["schema"]
for _,route,_,_,_,_,_ in routes:
    for op in api["paths"][route].values():
        for response in op.get("responses",{}).values():
            schema=response.get("content",{}).get("application/json",{}).get("schema",{})
            if schema.get("$ref")=="#/components/schemas/ErrorResponse":
                schema.clear();schema.update(error_ref)
path.write_text(json.dumps(api,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print("Added AI management operations:",len(routes),"schemas:",len(s))
