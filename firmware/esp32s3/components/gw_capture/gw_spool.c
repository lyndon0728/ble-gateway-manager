#include "gw_spool.h"
#include "gw_wire.h"
#include <stdlib.h>
#include <string.h>

#define ENTRY 128u
#define E_SIZE 32u
#define FOOT (GW_SPOOL_ROOT_BYTES-32u)
#define DATA (2u*GW_SPOOL_ROOT_BYTES)
#define REC_DATA 96u
static const uint8_t root_magic[8]={'G','W','S','P','0','0','0','3'};
static const uint8_t seal_magic[8]={'C','O','M','M','I','T','0','3'};
static const uint8_t rec_magic[8]={'G','W','R','E','C','0','0','3'};
static uint8_t *entry(uint8_t *root,unsigned i){return root+ENTRY+i*E_SIZE;}
static const uint8_t *centry(const gw_spool_t *s,unsigned i){return s->root+ENTRY+i*E_SIZE;}
static uint32_t slot_offset(unsigned i){return DATA+i*GW_SPOOL_SLOT_BYTES;}
static int failed(gw_spool_t *s){s->fault=true;return GW_SP_FAULT;}
unsigned gw_spool_state(const gw_spool_t *s,unsigned i){return s&&s->root&&i<GW_SPOOL_SLOTS?centry(s,i)[0]:GW_SP_FREE;}
uint64_t gw_spool_sequence(const gw_spool_t *s,unsigned i){return gw_get_u64(centry(s,i)+8);}
const uint8_t *gw_spool_event_boot(const gw_spool_t *s,unsigned i){return centry(s,i)+16;}
bool gw_spool_merged(const gw_spool_t *s,unsigned i){return centry(s,i)[1]!=0;}
size_t gw_spool_used(const gw_spool_t *s){size_t n=0;for(unsigned i=0;i<GW_SPOOL_SLOTS;i++)if(gw_spool_state(s,i))n+=GW_SPOOL_SLOT_BYTES;return n;}
static bool valid_root(const uint8_t *r){
    return !memcmp(r,root_magic,8)&&gw_get_u32(r+8)==3&&
        !memcmp(r+FOOT+24,seal_magic,8)&&gw_get_u64(r+FOOT+8)==gw_get_u64(r+16)&&
        gw_get_u32(r+FOOT)==gw_crc32c(r,FOOT)&&gw_get_u64(r+16)>0&&
        gw_get_u64(r+24)>0&&gw_get_u64(r+112)>0&&gw_get_u32(r+32)<GW_SPOOL_SLOTS;
}
static int prepare(gw_spool_t *s,uint8_t **copy){
    if(s->fault||!s->root)return GW_SP_FAULT;
    *copy=malloc(GW_SPOOL_ROOT_BYTES);if(!*copy)return GW_SP_FULL;
    memcpy(*copy,s->root,GW_SPOOL_ROOT_BYTES);
    if(gw_get_u64(*copy+16)==UINT64_MAX){free(*copy);*copy=NULL;return failed(s);}
    /* Invalidate the obsolete root BEFORE reusing any of its obsolete slots.
     * The current root remains intact throughout the complete transaction. */
    if(s->io.erase(s->io.ctx,(1u-s->active)*GW_SPOOL_ROOT_BYTES,GW_SPOOL_ROOT_BYTES)){
        free(*copy);*copy=NULL;return failed(s);
    }
    return GW_SP_OK;
}
static int publish(gw_spool_t *s,uint8_t *r){
    uint64_t generation=gw_get_u64(s->root+16)+1;
    gw_put_u64(r+16,generation);memset(r+FOOT,0,32);memcpy(r+FOOT+24,seal_magic,8);
    gw_put_u64(r+FOOT+8,generation);gw_put_u32(r+FOOT,gw_crc32c(r,FOOT));
    uint32_t at=(1u-s->active)*GW_SPOOL_ROOT_BYTES;
    int result=GW_SP_OK;
    if(s->io.write(s->io.ctx,at,r,FOOT)||s->io.write(s->io.ctx,at+FOOT,r+FOOT,32))result=failed(s);
    uint8_t *verify=malloc(GW_SPOOL_ROOT_BYTES);
    if(!verify)result=failed(s); /* Outcome UNKNOWN: do not acknowledge custody. */
    else if(result==GW_SP_OK&&(s->io.read(s->io.ctx,at,verify,GW_SPOOL_ROOT_BYTES)||memcmp(verify,r,GW_SPOOL_ROOT_BYTES)||!valid_root(verify)))result=failed(s);
    free(verify);
    if(result==GW_SP_OK){memcpy(s->root,r,GW_SPOOL_ROOT_BYTES);s->active=1u-s->active;}
    free(r);return result;
}
static bool valid_record(const uint8_t *p){return !memcmp(p,rec_magic,8)&&
    gw_get_u16(p+10)>=24&&gw_get_u16(p+10)<=GW_SPOOL_RECORD_MAX&&p[9]<=1&&
    (p[8]==0x20||p[8]==0x21||p[8]==0x22||p[8]==0x23)&&gw_get_u64(p+64)>0&&
    gw_get_u32(p+GW_SPOOL_SLOT_BYTES-4)==gw_crc32c(p,GW_SPOOL_SLOT_BYTES-4);}
int gw_spool_open(gw_spool_t *s,const gw_spool_io_t *io,const uint8_t boot[16]){
    memset(s,0,sizeof(*s));s->io=*io;memcpy(s->session_boot,boot,16);
    uint8_t *a=malloc(GW_SPOOL_ROOT_BYTES),*b=malloc(GW_SPOOL_ROOT_BYTES);
    if(!a||!b){free(a);free(b);return GW_SP_FULL;}
    if(io->read(io->ctx,0,a,GW_SPOOL_ROOT_BYTES)||io->read(io->ctx,GW_SPOOL_ROOT_BYTES,b,GW_SPOOL_ROOT_BYTES)){free(a);free(b);return failed(s);}
    bool va=valid_root(a),vb=valid_root(b);
    /* A newer complete seal with damaged contents is media corruption, never
     * permission to roll back. An OLDER seal can survive halfway through the
     * erase of the obsolete root; a newer valid root proves it superseded. */
    if((!va&&!memcmp(a+FOOT+24,seal_magic,8)&&(!vb||gw_get_u64(a+FOOT+8)>=gw_get_u64(b+16)))||
       (!vb&&!memcmp(b+FOOT+24,seal_magic,8)&&(!va||gw_get_u64(b+FOOT+8)>=gw_get_u64(a+16)))){free(a);free(b);return failed(s);}
    if(!va&&!vb){
        bool empty=false;
        if(io->erased(io->ctx,0,GW_SPOOL_BYTES,&empty)||!empty){free(a);free(b);return failed(s);}
        memset(a,0,GW_SPOOL_ROOT_BYTES);memcpy(a,root_magic,8);gw_put_u32(a+8,3);gw_put_u64(a+24,1);gw_put_u64(a+112,1);
        s->root=a;s->active=1;memcpy(b,a,GW_SPOOL_ROOT_BYTES);
        if(publish(s,b)!=GW_SP_OK)return GW_SP_FAULT;
    }else{
        if(va&&vb&&gw_get_u64(a+16)==gw_get_u64(b+16)&&memcmp(a,b,GW_SPOOL_ROOT_BYTES)){free(a);free(b);return failed(s);}
        if(vb&&(!va||gw_get_u64(b+16)>gw_get_u64(a+16))){s->root=b;s->active=1;free(a);}else{s->root=a;s->active=0;free(b);}
    }
    uint8_t *p=malloc(GW_SPOOL_SLOT_BYTES);if(!p)return failed(s);
    for(unsigned i=0;i<GW_SPOOL_SLOTS;i++){
        unsigned st=gw_spool_state(s,i);if(st>GW_SP_INBOX){free(p);return failed(s);}
        if(st&&(io->read(io->ctx,slot_offset(i),p,GW_SPOOL_SLOT_BYTES)||!valid_record(p)||
            ((st==GW_SP_INBOX)!=(p[8]==0x22))||((st==GW_SP_READY||st==GW_SP_QUARANTINE)&&!gw_spool_sequence(s,i)))){free(p);return failed(s);}
    }
    free(p);return GW_SP_OK;
}
void gw_spool_close(gw_spool_t *s){free(s->root);s->root=NULL;}
int gw_spool_check(gw_spool_t *s,const gw_spool_watermark_t *w){
    if(s->fault||!s->root)return GW_SP_FAULT;
    if(!w||!w->batch||!w->first||!w->count||w->count>32||w->first>UINT64_MAX-w->count)return failed(s);
    bool same=!memcmp(w->boot,s->root+40,16);uint32_t last=gw_get_u32(s->root+56);
    if(!same||!last)return w->batch==1&&w->first==1?GW_SP_OK:failed(s);
    if(w->batch<last)return GW_SP_OLD;
    if(w->batch==last)return w->first==gw_get_u64(s->root+64)&&w->count==gw_get_u16(s->root+72)&&w->type==s->root[74]&&!memcmp(w->hash,s->root+80,32)?GW_SP_REPLAY:failed(s);
    return last<UINT32_MAX&&w->batch==last+1&&w->first==gw_get_u64(s->root+64)+gw_get_u16(s->root+72)?GW_SP_OK:failed(s);
}
int gw_spool_add(gw_spool_t *s,const gw_spool_record_t *records,size_t n,const gw_spool_watermark_t *w){
    if(!n||n>32||!records)return failed(s);
    if(w){int check=gw_spool_check(s,w);if(check!=GW_SP_OK)return check;if(w->count!=n)return failed(s);}
    if(s->fault||!s->root)return GW_SP_FAULT;
    if(gw_get_u64(s->root+112)>UINT64_MAX-n)return failed(s);
    unsigned free_slots=0;for(unsigned i=0;i<GW_SPOOL_SLOTS;i++)if(!gw_spool_state(s,i))free_slots++;
    if(free_slots<n)return GW_SP_FULL;
    for(size_t i=0;i<n;i++)if(records[i].length<24||records[i].length>GW_SPOOL_RECORD_MAX||records[i].radio>1||
        (records[i].type!=0x20&&records[i].type!=0x21&&records[i].type!=0x22&&records[i].type!=0x23))return failed(s);
    uint8_t *p=malloc(GW_SPOOL_SLOT_BYTES),*verify=malloc(GW_SPOOL_SLOT_BYTES),*r=NULL;
    if(!p||!verify){free(p);free(verify);return GW_SP_FULL;}
    int result=prepare(s,&r);if(result)goto done;
    unsigned cursor=gw_get_u32(r+32);uint64_t order=gw_get_u64(r+112);
    for(size_t j=0;j<n;j++){
        while(entry(r,cursor)[0])cursor=(cursor+1)%GW_SPOOL_SLOTS;
        const gw_spool_record_t *v=records+j;
        memset(p,0,GW_SPOOL_SLOT_BYTES);memcpy(p,rec_magic,8);p[8]=v->type;p[9]=v->radio;gw_put_u16(p+10,v->length);
        memcpy(p+16,v->source_boot,16);gw_put_u64(p+32,v->arrived_ms);gw_put_u64(p+40,v->mapped_ms);gw_put_u64(p+48,v->utc_ms);
        gw_put_u32(p+56,v->error_ms);p[60]=v->mapped;gw_put_u64(p+64,order++);memcpy(p+REC_DATA,v->bytes,v->length);
        gw_put_u32(p+GW_SPOOL_SLOT_BYTES-4,gw_crc32c(p,GW_SPOOL_SLOT_BYTES-4));
        uint32_t at=slot_offset(cursor);
        if(s->io.erase(s->io.ctx,at,GW_SPOOL_SLOT_BYTES)||s->io.write(s->io.ctx,at,p,GW_SPOOL_SLOT_BYTES)||
            s->io.read(s->io.ctx,at,verify,GW_SPOOL_SLOT_BYTES)||memcmp(p,verify,GW_SPOOL_SLOT_BYTES)){result=failed(s);goto done;}
        memset(entry(r,cursor),0,E_SIZE);entry(r,cursor)[0]=v->type==0x22?GW_SP_INBOX:GW_SP_PENDING;
        cursor=(cursor+1)%GW_SPOOL_SLOTS;
    }
    gw_put_u32(r+32,cursor);gw_put_u64(r+112,order);
    if(w){memcpy(r+40,w->boot,16);gw_put_u32(r+56,w->batch);gw_put_u64(r+64,w->first);gw_put_u16(r+72,w->count);r[74]=w->type;memcpy(r+80,w->hash,32);}
    result=publish(s,r);r=NULL;
done:free(p);free(verify);free(r);return result;
}
int gw_spool_read(gw_spool_t *s,unsigned i,gw_spool_record_t *v){
    if(!v||i>=GW_SPOOL_SLOTS||!gw_spool_state(s,i)||s->fault)return GW_SP_FAULT;
    uint8_t *p=malloc(GW_SPOOL_SLOT_BYTES);if(!p)return GW_SP_FULL;
    if(s->io.read(s->io.ctx,slot_offset(i),p,GW_SPOOL_SLOT_BYTES)||!valid_record(p)){free(p);return failed(s);}
    memset(v,0,sizeof(*v));v->type=p[8];v->radio=p[9];v->length=gw_get_u16(p+10);memcpy(v->source_boot,p+16,16);
    v->arrived_ms=gw_get_u64(p+32);v->mapped_ms=gw_get_u64(p+40);v->utc_ms=gw_get_u64(p+48);v->error_ms=gw_get_u32(p+56);v->mapped=p[60]!=0;v->order=gw_get_u64(p+64);memcpy(v->bytes,p+REC_DATA,v->length);free(p);return GW_SP_OK;
}
int gw_spool_finish(gw_spool_t *s,const gw_spool_pair_t *pairs,size_t n){
    if(!n)return GW_SP_OK;if(!pairs||n>GW_SPOOL_SLOTS)return failed(s);
    uint8_t *r=NULL;int result=prepare(s,&r);if(result)return result;
    uint64_t sequence=gw_get_u64(r+24);
    for(size_t j=0;j<n;j++){
        unsigned i=pairs[j].primary,k=pairs[j].peer;
        if(i>=GW_SPOOL_SLOTS||entry(r,i)[0]!=GW_SP_PENDING||sequence>9007199254740991ULL||
            (k!=GW_SPOOL_NONE&&(k>=GW_SPOOL_SLOTS||k==i||entry(r,k)[0]!=GW_SP_PENDING))){free(r);return failed(s);}
        entry(r,i)[0]=GW_SP_READY;entry(r,i)[1]=k!=GW_SPOOL_NONE;gw_put_u64(entry(r,i)+8,sequence++);memcpy(entry(r,i)+16,s->session_boot,16);
        if(k!=GW_SPOOL_NONE)memset(entry(r,k),0,E_SIZE);
    }
    gw_put_u64(r+24,sequence);return publish(s,r);
}
int gw_spool_mark(gw_spool_t *s,const uint16_t *slots,const uint8_t *states,size_t n){
    if(!n)return GW_SP_OK;if(!slots||!states||n>GW_SPOOL_SLOTS)return failed(s);
    uint8_t *r=NULL;int result=prepare(s,&r);if(result)return result;
    for(size_t j=0;j<n;j++){
        unsigned i=slots[j];if(i>=GW_SPOOL_SLOTS||!entry(r,i)[0]||entry(r,i)[0]==GW_SP_PENDING||
            (states[j]!=GW_SP_FREE&&states[j]!=GW_SP_QUARANTINE)||(states[j]==GW_SP_QUARANTINE&&entry(r,i)[0]!=GW_SP_READY)){free(r);return failed(s);}
        if(states[j]==GW_SP_FREE)memset(entry(r,i),0,E_SIZE);else entry(r,i)[0]=states[j];
    }
    return publish(s,r);
}
