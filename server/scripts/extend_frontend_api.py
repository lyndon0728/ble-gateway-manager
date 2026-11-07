"""Add durable operator-console read contracts without altering device messages."""
from pathlib import Path
import copy,json

assets=Path(__file__).resolve().parents[1]/'internal/platform/assets'
path=assets/'openapi.json'
api=json.loads(path.read_text(encoding='utf8'));s=api['components']['schemas']
ref=lambda name:{'$ref':'#/components/schemas/'+name}
def obj(fields,required=()):return {'type':'object','properties':fields,'required':list(required),'additionalProperties':False}
def text(**kw):return {'type':'string',**kw}
integer={'type':'integer','minimum':0}
ident=text(minLength=1,maxLength=128,pattern='^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$')
stamp=text(format='date-time');json_object={'type':'object','additionalProperties':True}
def model(name,fields,optional=()):s[name]=obj(fields,[k for k in fields if k not in optional])
def page(name,item):s[name]=copy.deepcopy(s['GatewayPage']);s[name]['properties']['items']['items']=ref(item)

model('ConfigurationFieldSource',{'path':text(),'revision':integer,'source':{'const':'DEVICE_VERIFIED_PATCH'},'taskId':ident,'verifiedAt':stamp},['taskId','verifiedAt'])
model('EffectiveConfiguration',{'gatewayId':ident,'configuration':json_object,'knownPaths':{'type':'array','items':text()},'sources':{'type':'array','items':ref('ConfigurationFieldSource')},'reportedRevision':integer,'complete':{'const':False},'valueSemantics':{'const':'ACKNOWLEDGED_PATCH_NOT_RUNTIME_READBACK'}})
model('TaskRequestView',{'taskId':ident,'gatewayId':ident,'request':json_object,'requestDigest':text(),'redacted':{'const':True}})
model('TaskResultHistoryItem',{'resultId':ident,'taskId':ident,'sequence':integer,'receivedAt':stamp,'result':json_object,'payloadAvailable':{'type':'boolean'},'redacted':{'const':True}},['result'])
page('TaskResultHistoryPage','TaskResultHistoryItem')
model('OutputRecord',{'outputId':ident,'namespaceId':ident,'gatewayId':ident,'sourceEventId':ident,'processingRevision':integer,'recordKey':text(),'schemaVersion':text(),'parserId':text(),'parserVersion':text(),'contentDigest':text(),'createdAt':stamp,'payloadAvailable':{'type':'boolean'},'record':json_object},['record'])
model('DeliveryPayload',{'deliveryId':ident,'gatewayId':ident,'contentDigest':text(),'payloadAvailable':{'type':'boolean'},'payload':json_object},['payload'])
s['ArtifactListItem']=copy.deepcopy(s['Artifact']);s['ArtifactListItem']['properties'].update(name=text(),createdAt=stamp);s['ArtifactListItem']['required']+=['name','createdAt'];page('ArtifactPage','ArtifactListItem')
page('ReprocessJobPage','ReprocessJob');page('OperationPage','Operation')
model('GATTSubscription',{'subscriptionId':ident,'namespaceId':ident,'gatewayId':ident,'connectionRef':text(pattern='^[0-9a-f]{32}$'),'serviceUuid':text(),'characteristicUuid':text(),'characteristicHandle':{'type':'integer','minimum':1,'maximum':65535},'mode':text(enum=['NOTIFY','INDICATE']),'state':text(enum=['ACTIVE','CLOSED']),'updatedAt':stamp});page('GATTSubscriptionPage','GATTSubscription')
model('AutoConnectTrigger',{'triggerId':ident,'namespaceId':ident,'gatewayId':ident,'ruleId':ident,'ruleRevision':integer,'advEventId':ident,'uuidEventId':ident,'serviceUuid':text(),'taskId':ident,'createdAt':stamp});page('AutoConnectTriggerPage','AutoConnectTrigger')

routes=[
('/api/v1/gateways/{gatewayId}/configuration/effective','EffectiveConfiguration',['gateway:read'],[],'Verified reported patches only; absent paths are unknown. Values are acknowledged configuration, not a claim of current hardware readback.'),
('/api/v1/tasks/{taskId}/request','TaskRequestView',['task:read'],[],'Durable command envelope with secret and byte-value fields redacted.'),
('/api/v1/tasks/{taskId}/results','TaskResultHistoryPage',['task:read'],[],'Durably accepted result history ordered by device sequence; sensitive fields are redacted.'),
('/api/v1/outputs/{outputId}','OutputRecord',['data:read','data:raw'],[],'Persisted result from the original processing revision; absent record with payloadAvailable=false means reclaimed payload.'),
('/api/v1/deliveries/{deliveryId}/payload','DeliveryPayload',['delivery:read','data:raw'],[],'Frozen customer event for this delivery; never reads destination credentials. payloadAvailable=false after retention.'),
('/api/v1/gateways/{gatewayId}/events/{eventId}/deliveries','DeliveryPage',['delivery:read'],[],'Delivery identities for every processing revision of the scoped source event.'),
('/api/v1/artifacts','ArtifactPage',['artifact:read'],['kind'],'Instance-scoped signed upload artifacts only: DEVICE_MANIFEST, FIRMWARE and PLATFORM_PACKAGE; excludes internal rule artifacts.'),
('/api/v1/reprocess-jobs','ReprocessJobPage',['data:reprocess'],['state'],'Only non-preview jobs created by the current principal, including for administrators.'),
('/api/v1/operations','OperationPage',['operation:read'],['kind','state'],'Maintenance and pipeline operations; current owner or instance system:maintain permission required.'),
('/api/v1/ble-peers/{peerId}','BLEPeer',['gatt:read'],[],'Object-scoped peer detail; ETag is its revision.'),
('/api/v1/auto-connect-rules/{ruleId}','AutoConnectRule',['gatt:read'],[],'Rule peer and every configured gateway must be readable; ETag is its revision.'),
('/api/v1/gatt-subscriptions','GATTSubscriptionPage',['gatt:read'],['gatewayId','connectionRef'],'Durable current/closed subscriptions visible within gateway scope.'),
('/api/v1/auto-connect-triggers','AutoConnectTriggerPage',['gatt:read'],['gatewayId','ruleId'],'Durable trigger provenance; both peer/rule scope and all configured gateways must be readable.'),
]
import re
for route,model_name,permissions,filters,description in routes:
 op=copy.deepcopy(api['paths']['/api/v1/gateways']['get'])
 op.update(operationId='get_'+re.sub('[^a-zA-Z0-9]','_',route),tags=['ConsoleReadModels'],summary=description,description=description)
 op['x-required-permissions']=permissions
 op['responses']['200']['content']={'application/json':{'schema':ref(model_name)}}
 op['parameters']=[]
 for key in re.findall(r'{([^}]+)}',route):op['parameters'].append({'name':key,'in':'path','required':True,'schema':ident})
 if model_name.endswith('Page'):op['parameters'] += [{'$ref':'#/components/parameters/'+key} for key in ['Cursor','Limit']]
 for key in filters:op['parameters'].append({'name':key,'in':'query','schema':text(maxLength=128)})
 if model_name in ['BLEPeer','AutoConnectRule']:op['responses']['200'].setdefault('headers',{})['ETag']={'schema':text(),'description':'Quoted current resource revision.'}
 api['paths'].setdefault(route,{})['get']=op

api['info']['version']='1.9.0'
api['info']['description']='Server 0.3.0: operator console and durable scoped read models; GW32 1.0/1.1 device contracts remain compatible.'
api['paths']['/api/v1/auth/me']['get']['responses']['200'].setdefault('headers',{})['X-CSRF-Token']={'description':'Returned only for an authenticated browser session cookie; restores the same-origin CSRF proof without exposing the HttpOnly session token.','schema':text()}
path.write_text(json.dumps(api,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
messages=json.loads((assets/'message-contracts.schema.json').read_text(encoding='utf8'));messages['$id']='https://schemas.example.invalid/ble-gateway/1.9.0/contracts.json';messages['$defs']=copy.deepcopy(s)
def refs(v):
 if isinstance(v,dict):
  for k,value in v.items():
   if k=='$ref' and isinstance(value,str):v[k]=value.replace('#/components/schemas/','#/$defs/')
   else:refs(value)
 elif isinstance(v,list):
  for value in v:refs(value)
refs(messages)
(assets/'message-contracts.schema.json').write_text(json.dumps(messages,ensure_ascii=False,indent=2)+'\n',encoding='utf8')
print('Added frontend read routes:',len(routes),'schemas:',len(s))
