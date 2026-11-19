#include "gw_rules.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    uint64_t n=0;uint32_t ip=0;
    assert(gw_rules_u64("18446744073709551615",&n)&&n==UINT64_MAX);
    assert(gw_rules_u64("9007199254740993",&n)&&n==9007199254740993ULL);
    assert(gw_rules_u64("0",&n)&&n==0);
    const char *bad[]={"","00","01","-1","+1"," 1","1 ","1.0","1e3","18446744073709551616","99999999999999999999999999999999"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++)assert(!gw_rules_u64(bad[i],&n));
    assert(gw_rules_utf8("gateway\xe4\xb8\xad\xe6\x96\x87"));
    const char *invalid_utf8[]={"\xc0\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xe2\x82","\x80","\xf0\x80\x80\x80","\n"};
    for(unsigned i=0;i<sizeof(invalid_utf8)/sizeof(invalid_utf8[0]);i++)assert(!gw_rules_utf8(invalid_utf8[i]));
    assert(gw_rules_ipv4("192.168.4.1",&ip)&&ip==0xc0a80401u);
    const char *invalid_ip[]={"","1.2.3","1.2.3.4.5","1.2.3.256","192.168.004.1","127.0.0.1","0.0.0.0","224.1.2.3","255.255.255.255","1.-2.3.4"};
    for(unsigned i=0;i<sizeof(invalid_ip)/sizeof(invalid_ip[0]);i++)assert(!gw_rules_ipv4(invalid_ip[i],&ip));
    assert(gw_record_crc32("123456789",9)==0xcbf43926u);
    uint8_t record[64]={0},pointer[24]={0};const char payload[]="{\"networkGeneration\":\"9007199254740993\"}";
    const size_t length=24+strlen(payload);memcpy(record,"GWSS",4);gw_record_put32(record+4,(uint32_t)strlen(payload));gw_record_put64(record+8,UINT64_C(0x123456789abcdef0));memcpy(record+24,payload,strlen(payload));
    uint32_t crc=gw_record_crc32(record+24,strlen(payload));gw_record_put32(record+16,crc);gw_record_put32(record+20,gw_record_crc32(record,20));
    assert(gw_record_valid_snapshot(record,length,UINT64_C(0x123456789abcdef0),crc));
    for(size_t cut=0;cut<length;cut++)assert(!gw_record_valid_snapshot(record,cut,UINT64_C(0x123456789abcdef0),crc));
    for(size_t i=0;i<length;i++){record[i]^=0x40;assert(!gw_record_valid_snapshot(record,length,UINT64_C(0x123456789abcdef0),crc));record[i]^=0x40;}
    assert(!gw_record_valid_snapshot(record,length,1,crc));assert(!gw_record_valid_snapshot(record,length,UINT64_C(0x123456789abcdef0),crc^1));
    memcpy(pointer,"GWSA",4);gw_record_put32(pointer+4,1);gw_record_put64(pointer+8,9);gw_record_put32(pointer+16,crc);gw_record_put32(pointer+20,gw_record_crc32(pointer,20));
    assert(gw_record_valid_pointer(pointer,sizeof(pointer)));
    for(size_t cut=0;cut<sizeof(pointer);cut++)assert(!gw_record_valid_pointer(pointer,cut));
    for(size_t i=0;i<sizeof(pointer);i++){pointer[i]^=0x80;assert(!gw_record_valid_pointer(pointer,sizeof(pointer)));pointer[i]^=0x80;}
    puts("PASS: uint64/UTF-8/IPv4 rules; snapshot/pointer truncation, corruption and generation binding");return 0;
}
