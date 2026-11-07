"""Update the checked-in API contract with the additive GW32 1.1 profile.

No code generation or network dependencies are required at runtime.
"""
from pathlib import Path
import copy
import json

assets = Path(__file__).resolve().parents[1] / "internal/platform/assets"
api = json.loads((assets / "openapi.json").read_text(encoding="utf-8"))
s = api["components"]["schemas"]
ref = lambda n: {"$ref": "#/components/schemas/" + n}
def obj(p, required=(), **kw):
    return {"type": "object", "properties": p, "required": list(required), "additionalProperties": False, **kw}
def string(**kw): return {"type": "string", **kw}
def integer(lo, hi): return {"type": "integer", "minimum": lo, "maximum": hi}
def enum(*values): return string(enum=list(values))
def array(item, lo=0, hi=128): return {"type":"array", "items":item,"minItems":lo,"maxItems":hi,"uniqueItems":True}
boolean = {"type":"boolean"}
ident = copy.deepcopy(s["RawEvent"]["properties"]["eventId"]) if "properties" in s["RawEvent"] else copy.deepcopy(s["RawEventV10"]["properties"]["eventId"])
radio = enum("NRF54", "ESP32")
conn = string(pattern="^[0-9a-f]{32}$")
address = string(pattern="^[0-9A-F]{12}$")
uuid = string(pattern="^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$")
flex_uuid = string(pattern="^(?:[0-9a-fA-F]{4}|[0-9a-fA-F]{8}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12})$")
timeout = integer(100,30000)
adv = obj({"radio":radio,"kind":enum("ADV","SCAN_RSP"),"addressType":enum("PUBLIC","RANDOM"),"address":address,"legacy":boolean,"connectable":boolean,"observedBy":{"const":["NRF54","ESP32"]},"rssiDbm":integer(-127,126),"txPowerDbm":integer(-127,126),"primaryPhy":enum("LE_1M","LE_2M","LE_CODED"),"secondaryPhy":enum("LE_1M","LE_2M","LE_CODED"),"sid":integer(0,15)},["radio","kind","legacy"])
adv["dependentRequired"]={"address":["addressType"],"addressType":["address"]}
adv["properties"]["primaryPhy"]=enum("LE_1M","LE_CODED")
adv["allOf"]=[
    {"if":{"properties":{"kind":{"const":"SCAN_RSP"}}},"then":{"not":{"required":["connectable"]}}},
    {"if":{"properties":{"kind":{"const":"ADV"},"radio":{"const":"NRF54"}}},"then":{"required":["connectable"]}},
    {"if":{"required":["observedBy"]},"then":{"properties":{"radio":{"const":"NRF54"}},"required":["address","addressType"],"allOf":[{"if":{"properties":{"legacy":{"const":False}}},"then":{"required":["sid"]}}]}},
    {"if":{"properties":{"legacy":{"const":True}}},"then":{"not":{"anyOf":[{"required":["sid"]},{"required":["secondaryPhy"]}]}}},
]
gatt=obj({"radio":radio,"kind":enum("GATT_NOTIFY","GATT_INDICATE"),"addressType":enum("PUBLIC","RANDOM"),"address":address,"connectionRef":conn,"characteristicHandle":integer(1,65535)},["radio","kind","connectionRef","characteristicHandle"])
gatt["dependentRequired"]={"address":["addressType"],"addressType":["address"]}
link=obj({"radio":radio,"kind":{"const":"GATT_LINK_DOWN"},"connectionRef":conn,"linkReason":enum("DISCONNECTED","NRF_RESTART","ESP_RESTART")},["radio","kind","connectionRef","linkReason"])
s["CaptureContext"]={"oneOf":[adv,gatt,link]}
if "RawEventV10" not in s:s["RawEventV10"]=copy.deepcopy(s["RawEvent"])
s["RawEventV11"]=copy.deepcopy(s["RawEventV10"]);s["RawEventV11"]["properties"]["capture"]=ref("CaptureContext");s["RawEventV11"]["required"].append("capture")
s["RawEvent"]={"oneOf":[ref("RawEventV10"),ref("RawEventV11")]}
s["IngressBatch"]["properties"]["schemaVersion"]=enum("1.0","1.1")
s["IngressBatch"]["allOf"]=[{"if":{"properties":{"schemaVersion":{"const":"1.0"}}},"then":{"properties":{"events":{"items":ref("RawEventV10")}}},"else":{"properties":{"events":{"items":ref("RawEventV11")}}}}]
s["IngressAck"]["properties"]["schemaVersion"]=enum("1.0","1.1")
s["CustomerEvent"]["properties"].update(sourceSchemaVersion={"const":"1.1"},capture=ref("CaptureContext"))
s["CustomerEvent"]["dependentRequired"]={"sourceSchemaVersion":["capture"],"capture":["sourceSchemaVersion"]}
s["ScanConfiguration"]=obj({"enabled":boolean,"active":boolean,"intervalMs":integer(10,10240),"windowMs":integer(10,10240)})
s["ScanFilters"]=obj({"minRssi":integer(-127,20),"serviceUuids":array(flex_uuid),"manufacturerIds":array(integer(0,65535))})
s["RadioScanFilters"]=copy.deepcopy(s["ScanFilters"]);s["RadioScanFilters"]["properties"]["minRssi"]={"type":["integer","null"],"minimum":-127,"maximum":20}
s["BeaconRule"]=obj({"action":enum("ALLOW","DENY"),"reportKinds":array(enum("LEGACY_ADV","LEGACY_SCAN_RSP","EXT_ADV","EXT_SCAN_RSP"),1,4),"adType":integer(1,255),"offset":integer(0,253),"valueBase64":string(minLength=4,maxLength=32),"maskBase64":string(minLength=4,maxLength=32)},["action","reportKinds","adType","offset","valueBase64","maskBase64"])
s["AdvertisingConfiguration"]=obj({"enabled":boolean,"mode":enum("NONCONNECTABLE","SCANNABLE_NONCONNECTABLE"),"intervalMs":integer(100,10240),"advDataBase64":string(maxLength=44),"scanResponseBase64":string(maxLength=44)},minProperties=1)
s["RadioConfiguration"]={"oneOf":[obj({"scan":ref("ScanConfiguration"),"filters":ref("RadioScanFilters"),"beaconRules":array(ref("BeaconRule"),0,8)},minProperties=1),obj({"advertising":ref("AdvertisingConfiguration")},["advertising"])]}
ipv4=string(format="ipv4")
s["IPv4Configuration"]={"oneOf":[obj({"mode":{"const":"DHCP"}},["mode"]),obj({"mode":{"const":"STATIC"},"address":ipv4,"prefixLength":integer(1,30),"gateway":ipv4,"dns":array(ipv4,1,2)},["mode","address","prefixLength","gateway","dns"])]}
s["NetworkConfiguration"]=obj({"uplinkOrder":array(enum("ETHERNET","WIFI_STA"),1,2),"ethernet":obj({"enabled":boolean,"ipv4":ref("IPv4Configuration")},minProperties=1),"wifiSta":obj({"enabled":boolean,"ssid":string(maxLength=32),"security":enum("WPA2_PSK","WPA3_SAE"),"credentialRef":ident,"ipv4":ref("IPv4Configuration")},minProperties=1)},minProperties=1)
s["GatewayConfiguration"]={"oneOf":[obj({"scan":ref("ScanConfiguration"),"filters":ref("ScanFilters"),"upload":obj({"batchMaxRecords":integer(1,1000),"flushIntervalMs":integer(100,60000)})}),obj({"network":ref("NetworkConfiguration")},["network"]),obj({"radios":obj({"nrf54":ref("RadioConfiguration"),"esp32":ref("RadioConfiguration")},minProperties=1,maxProperties=1)},["radios"])]}
for n in ["TaskParamsConfigure","CommandParamsConfigure"]:s[n]["properties"]["configuration"]=ref("GatewayConfiguration")
# Also constrain the direct desired configuration endpoint.
for n,v in s.items():
    if n not in ["TaskParamsConfigure","CommandParamsConfigure"] and n.lower().startswith("configuration") and "configuration" in v.get("properties",{}):
        v["properties"]["configuration"]=ref("GatewayConfiguration")
s["TaskParamsDiagnostics"]["properties"]["wifiScan"]=enum("OFF","PASSIVE","ACTIVE")
s["TaskParamsDiagnostics"]["allOf"]=[{"if":{"required":["wifiScan"]},"then":{"properties":{"profile":{"const":"NETWORK"}}}}]
for n in ["TaskParamsGattRead","TaskParamsGattWrite"]:s[n]["properties"].update(radio=radio,connectionRef=conn)
s["TaskParamsGattConnect"]=obj({"radio":radio,"deviceAddress":string(),"addressType":enum("PUBLIC","RANDOM"),"security":enum("UNENCRYPTED","BONDED_ENCRYPTED"),"timeoutMs":timeout},["radio","deviceAddress","addressType","security","timeoutMs"])
s["TaskParamsGattDisconnect"]=obj({"radio":radio,"connectionRef":conn,"timeoutMs":timeout},["radio","connectionRef","timeoutMs"])
s["TaskParamsGattSubscribe"]=obj({"radio":radio,"connectionRef":conn,"serviceUuid":flex_uuid,"characteristicUuid":flex_uuid,"mode":enum("NOTIFY","INDICATE"),"timeoutMs":timeout},["radio","connectionRef","serviceUuid","characteristicUuid","mode","timeoutMs"])
s["TaskParamsGattUnsubscribe"]=copy.deepcopy(s["TaskParamsGattSubscribe"]);del s["TaskParamsGattUnsubscribe"]["properties"]["mode"];s["TaskParamsGattUnsubscribe"]["required"].remove("mode")
s["AutoTrigger"]=obj({"advEventId":ident,"uuidEventId":ident,"ruleId":ident,"ruleRevision":integer(1,2147483647),"serviceUuid":uuid,"wireBits":{"type":"integer","enum":[16,32,128]},"characteristicUuid":uuid,"maxAgeMs":integer(1000,30000)},["advEventId","uuidEventId","ruleId","ruleRevision","serviceUuid","wireBits","maxAgeMs"])
s["CommandParamsGattConnect"]=copy.deepcopy(s["TaskParamsGattConnect"]);s["CommandParamsGattConnect"]["properties"]["autoTrigger"]=ref("AutoTrigger")
types={"GATT_CONNECT":"GattConnect","GATT_DISCONNECT":"GattDisconnect","GATT_SUBSCRIBE":"GattSubscribe","GATT_UNSUBSCRIBE":"GattUnsubscribe"}
for n,v in s.items():
    p=v.get("properties",{})
    if "type" in p and isinstance(p["type"],dict) and "GATT_READ" in p["type"].get("enum",[]):
        for value in types:
            if value not in p["type"]["enum"]:p["type"]["enum"].append(value)
        if "allOf" in v:
            v["allOf"]=[c for c in v["allOf"] if c.get("if",{}).get("properties",{}).get("type",{}).get("const") not in types]
            for typ,suffix in types.items():v["allOf"].append({"if":{"properties":{"type":{"const":typ}}},"then":{"properties":{"parameters":ref(("CommandParams" if n=="Command" and typ=="GATT_CONNECT" else "TaskParams")+suffix)}}})
s["BLEPeerCreate"]=obj({"scopeId":ident,"displayName":string(minLength=1,maxLength=128),"deviceAddress":address,"addressType":enum("PUBLIC","RANDOM")},["scopeId","displayName","deviceAddress","addressType"])
s["BLEPeerPatch"]=obj({"displayName":string(minLength=1,maxLength=128),"state":enum("ACTIVE","DISABLED")},minProperties=1)
s["BLEPeer"]=obj({**s["BLEPeerCreate"]["properties"],"peerId":ident,"namespaceId":ident,"state":enum("ACTIVE","DISABLED"),"revision":integer(1,2147483647),"createdAt":string(format="date-time")},["scopeId","displayName","deviceAddress","addressType","peerId","namespaceId","state","revision","createdAt"])
rule_fields={"name":string(minLength=1,maxLength=128),"peerId":ident,"gatewayIds":array(ident,1,100),"enabled":boolean,"priority":integer(0,10000),"serviceUuid":uuid,"security":enum("UNENCRYPTED","BONDED_ENCRYPTED"),"wireBits":{"type":"integer","enum":[16,32,128]},"characteristicUuid":uuid,"minRssi":integer(-127,20),"maxAgeMs":integer(1000,30000),"cooldownMs":integer(30000,86400000)}
s["AutoConnectRuleCreate"]=obj(rule_fields,["name","peerId","gatewayIds","enabled","priority","serviceUuid","security"])
s["AutoConnectRulePatch"]=obj({k:v for k,v in rule_fields.items() if k!="peerId"},minProperties=1)
s["AutoConnectRule"]=obj({**rule_fields,"ruleId":ident,"revision":integer(1,2147483647),"createdAt":string(format="date-time")},s["AutoConnectRuleCreate"]["required"]+["maxAgeMs","cooldownMs","ruleId","revision","createdAt"])
s["GATTConnection"]=obj({"namespaceId":ident,"gatewayId":ident,"connectionRef":conn,"radio":radio,"addressType":enum("","PUBLIC","RANDOM"),"deviceAddress":string(pattern="^(?:[0-9A-F]{12})?$"),"securityState":enum("","UNENCRYPTED","ENCRYPTED","AUTHENTICATED_ENCRYPTED"),"attMtu":integer(0,517),"state":enum("ACTIVE","CLOSED","UNKNOWN"),"createdAt":string(format="date-time"),"updatedAt":string(format="date-time")},["namespaceId","gatewayId","connectionRef","radio","addressType","deviceAddress","securityState","attMtu","state","createdAt","updatedAt"])
for model in ["BLEPeer","AutoConnectRule","GATTConnection"]:
    s[model+"Page"]=copy.deepcopy(s["GatewayPage"])
    s[model+"Page"]["properties"]["items"]["items"]=ref(model)
def endpoint(path,method,model,permission,body=None,item_id=None):
    template=api["paths"]["/api/v1/gateways"]["get"] if method=="get" else api["paths"]["/api/v1/rule-bundles"]["post"]
    op=copy.deepcopy(template)
    op.update(operationId=method+"_"+path.replace("/","_").replace("-","_").replace("{","").replace("}",""),tags=["GATT"],summary=method.upper()+" "+path,description="GW32 protocol 1.1; resource-scoped permissions and durable state.")
    op["x-required-permissions"]=[permission]
    status="201" if method=="post" else "200"
    response=copy.deepcopy(op["responses"].pop("201" if method!="get" else "200"))
    response["content"]={"application/json":{"schema":ref(model)}}
    if method!="get":response["headers"]["ETag"]={"schema":string(),"description":"Quoted resource revision, required for PATCH."}
    op["responses"][status]=response
    op["parameters"]=[{"$ref":"#/components/parameters/"+x} for x in (["Cursor","Limit"] if method=="get" else ["IdempotencyKey"] if method=="post" else ["IfMatch"])]
    if item_id:op["parameters"].append({"name":item_id,"in":"path","required":True,"schema":ident})
    if body:op["requestBody"]={"required":True,"content":{"application/json":{"schema":ref(body)}}}
    else:op.pop("requestBody",None)
    if method=="patch":
        for code in ["412","428"]:op["responses"][code]={"description":"Revision precondition failed or missing.","content":{"application/json":{"schema":ref("ErrorResponse")}}}
    api["paths"].setdefault(path,{})[method]=op
for path,model,key in [("ble-peers","BLEPeer","peerId"),("auto-connect-rules","AutoConnectRule","ruleId")]:
    endpoint("/api/v1/"+path,"get",model+"Page","gatt:read")
    endpoint("/api/v1/"+path,"post",model,"gatt:manage",model+"Create")
    endpoint("/api/v1/"+path+"/{"+key+"}","patch",model,"gatt:manage",model+"Patch",key)
endpoint("/api/v1/gatt-connections","get","GATTConnectionPage","gatt:read")
api["paths"]["/api/v1/gatt-connections"]["get"]["parameters"].append({"name":"gatewayId","in":"query","schema":ident})
api["info"].update(version="1.8.0",description="Server 0.2.0: additive GW32 protocol 1.1 with backward-compatible 1.0 ingress, explicit capabilities, durable connection ownership and automatic connection rules. UART execution and dual-radio pairing are gateway responsibilities.")
(assets/"openapi.json").write_text(json.dumps(api,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
messages=json.loads((assets/"message-contracts.schema.json").read_text(encoding="utf-8"));messages["$id"]="https://schemas.example.invalid/ble-gateway/1.8.0/contracts.json";messages["title"]="Gateway 1.0/1.1 message contracts";messages["$defs"]=copy.deepcopy(s)
def fix_refs(value):
    if isinstance(value,dict):
        for k,v in value.items():
            if k=="$ref" and isinstance(v,str):value[k]=v.replace("#/components/schemas/","#/$defs/")
            else:fix_refs(v)
    elif isinstance(value,list):
        for v in value:fix_refs(v)
fix_refs(messages)
(assets/"message-contracts.schema.json").write_text(json.dumps(messages,ensure_ascii=False,indent=2)+"\n",encoding="utf-8")
print("Updated GW32 1.1 schemas",len(s))
