#include "../gw_provision_guard.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CHECK(s) gw_prov_payload_precheck((const uint8_t *)(s),sizeof(s)-1)
int main(void){
    assert(CHECK("{}"));
    assert(CHECK("{\"ssid\":\"\xe4\xb8\xad\xe6\x96\x87\"}"));
    assert(CHECK("{\"value\":\"\\\"[[[[[[[[[[[\"}"));
    assert(CHECK("[[[[[[[[]]]]]]]]"));
    assert(!CHECK("[[[[[[[[[]]]]]]]]]"));
    assert(!CHECK("{\"x\":\"\xc0\x80\"}")); /* overlong NUL */
    assert(!CHECK("{\"x\":\"\xed\xa0\x80\"}")); /* surrogate */
    assert(!CHECK("{\"x\":\"\xf4\x90\x80\x80\"}")); /* > U+10FFFF */
    assert(!CHECK("{\"x\":\"\xf0\x80\x80\x80\"}")); /* overlong */
    assert(!CHECK("{\"x\":\"\x80\"}"));
    assert(!CHECK("{\"x\":\"\xe4\xb8"));
    assert(!CHECK("{\"x\":\"\0\"}"));
    assert(!CHECK("{\"operation\\u0000x\":\"GET_STATE\"}"));
    assert(CHECK("{\"literal\":\"\\\\u0000\"}"));
    assert(!CHECK("{\"x\":\"unterminated}"));
    assert(!CHECK("}"));assert(!CHECK("{\"x\":[]"));assert(!gw_prov_payload_precheck(NULL,3));
    uint8_t large[16385];memset(large,' ',sizeof(large));assert(!gw_prov_payload_precheck(large,sizeof(large)));
    puts("PASS provisioning UTF-8, depth and size guards");return 0;
}
