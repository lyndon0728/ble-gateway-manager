// SPDX-License-Identifier: Apache-2.0
#include "gw_provision_guard.h"

static bool utf8_valid(const uint8_t *s,size_t size) {
    for(size_t i=0;i<size;){
        uint32_t cp=s[i++];unsigned continuation=0;uint32_t minimum=0;
        if(cp<0x80){if(!cp)return false;continue;}
        if(cp>=0xc2&&cp<=0xdf){cp&=0x1f;continuation=1;minimum=0x80;}
        else if(cp>=0xe0&&cp<=0xef){cp&=0x0f;continuation=2;minimum=0x800;}
        else if(cp>=0xf0&&cp<=0xf4){cp&=0x07;continuation=3;minimum=0x10000;}
        else return false;
        if(continuation>size-i)return false;
        while(continuation--){uint8_t c=s[i++];if((c&0xc0)!=0x80)return false;cp=(cp<<6)|(c&0x3f);}
        if(cp<minimum||cp>0x10ffff||(cp>=0xd800&&cp<=0xdfff))return false;
    }
    return true;
}
bool gw_prov_payload_precheck(const uint8_t *s,size_t size) {
    if(!s||size<2||size>16384||!utf8_valid(s,size))return false;
    unsigned depth=0;bool quoted=false,escape=false;
    for(size_t i=0;i<size;i++){
        uint8_t c=s[i];
        if(quoted){
            if(escape){
                /* cJSON strings use NUL termination: do not allow a decoded
                 * NUL to truncate keys, secrets, identifiers or digests. */
                if(c=='u'&&i+4<size&&s[i+1]=='0'&&s[i+2]=='0'&&s[i+3]=='0'&&s[i+4]=='0')return false;
                escape=false;
            }else if(c=='\\')escape=true;else if(c=='"')quoted=false;
            continue;
        }
        if(c=='"')quoted=true;
        else if(c=='{'||c=='['){if(++depth>8)return false;}
        else if(c=='}'||c==']'){if(!depth)return false;--depth;}
    }
    return !quoted&&!depth;
}
