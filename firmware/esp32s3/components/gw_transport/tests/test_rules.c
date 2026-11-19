#include "gw_transport_rules.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 const char *suffix;assert(!strcmp(gw_transport_topic("/api/v1/gateway-ingress/results",&suffix),"command-results"));assert(!strcmp(suffix,"result-ack"));
 assert(!gw_transport_topic("/api/v1/gateway-network-state",&suffix));
 cJSON *r=cJSON_Parse("{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"batchId\":\"b\"}"),*a=cJSON_Parse("{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"batchId\":\"b\",\"durability\":\"RAW_PERSISTED\"}");
 assert(gw_transport_ack_matches(a,r,"/api/v1/gateway-ingress/batches"));
 assert(!gw_transport_ack_matches(a,r,"/api/v1/gateway-peripheral/batches"));
 cJSON_ReplaceItemInObject(a,"gatewayId",cJSON_CreateString("other"));assert(!gw_transport_ack_matches(a,r,"/api/v1/gateway-ingress/batches"));cJSON_Delete(a);cJSON_Delete(r);
 r=cJSON_Parse("{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"results\":[{\"taskId\":\"t\",\"resultId\":\"r\"}]}");
 a=cJSON_Parse("{\"namespaceId\":\"n\",\"gatewayId\":\"g\",\"durability\":\"RESULT_PERSISTED\",\"items\":[{\"taskId\":\"t\",\"resultId\":\"old\"}]}");
 assert(!gw_transport_ack_matches(a,r,"/api/v1/gateway-ingress/results"));
 cJSON_ReplaceItemInObject(cJSON_GetArrayItem(cJSON_GetObjectItem(a,"items"),0),"resultId",cJSON_CreateString("r"));
 assert(gw_transport_ack_matches(a,r,"/api/v1/gateway-ingress/results"));cJSON_Delete(a);cJSON_Delete(r);
 puts("PASS: MQTT topics, identity, durability, late ACK correlation");
}
