#include <dfu_mock.h>
#include "../src/gw_dfu.c"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

int64_t dfu_mock_time;
static const struct flash_area areas[] = {{0,0x10000,128*1024},{1,0x30000,128*1024}};
static struct device storage;
static uint8_t slot[128*1024], image[1500], running_image[1500], hash[32], output[GW_DFU_STATUS_SIZE];
static size_t output_length;
static unsigned int erases, writes, checks, requests, reboots, confirmations, reservations, releases;
static bool boot_confirmed = true;
static int radio_error, write_error, hash_error, pending_error, confirm_error, erase_error;
static uint8_t upload_slot = 1;

int flash_area_open(uint8_t id, const struct flash_area **area)
{ assert(id <= 1); *area = &areas[id]; return 0; }
void flash_area_close(const struct flash_area *area) { (void)area; }
const struct device *flash_area_get_device(const struct flash_area *area)
{ (void)area; return &storage; }
int flash_area_sectors(const struct flash_area *area, uint32_t *count, struct flash_sector *sector)
{ assert(area->fa_id == 1 && *count == 1); sector->fs_size=4096; sector->fs_off=0; return -ENOMEM; }
int flash_area_flatten(const struct flash_area *area, uint32_t offset, size_t length)
{ assert(area->fa_id==1 && offset==0 && length==sizeof(slot)); ++erases;
  if (erase_error) { return erase_error; } memset(slot,0xff,sizeof(slot)); return 0; }
int flash_img_init_id(struct flash_img_context *ctx, uint8_t id)
{ assert(id==1); ctx->flash_area=&areas[1]; ctx->accepted=0; return 0; }
int flash_img_buffered_write(struct flash_img_context *ctx,const uint8_t *data,size_t length,bool flush)
{
    assert(ctx->flash_area==&areas[1]);
    if (write_error) { return write_error; }
    if (length) { ++writes; assert(4096+ctx->accepted+length<=sizeof(slot));
        memcpy(slot+4096+ctx->accepted,data,length); ctx->accepted+=(uint32_t)length; }
    if (flush) { ctx->flash_area=NULL; }
    return 0;
}
int flash_img_check(struct flash_img_context *ctx,const struct flash_img_check *check,uint8_t id)
{
    (void)ctx; ++checks; assert(id<=1 && check->clen==sizeof(image));
    if (hash_error) { return hash_error; }
    const uint8_t *stored = id ? slot+4096 : running_image;
    return memcmp(check->match,hash,32) || memcmp(stored,image,sizeof(image)) ? -EILSEQ : 0;
}
uint8_t flash_img_get_upload_slot(void) { return upload_slot; }
uint8_t boot_fetch_active_slot(void) { return 0; }
bool boot_is_img_confirmed(void) { return boot_confirmed; }
ssize_t boot_get_trailer_status_offset(size_t size) { return (ssize_t)size-32; }
int boot_read_bank_header(uint8_t id,struct mcuboot_img_header *header,size_t length)
{ assert(id==1 && length==sizeof(*header)); header->mcuboot_version=1; header->h.v1.image_size=1400; return 0; }
int boot_request_upgrade(int permanent)
{ assert(permanent==BOOT_UPGRADE_TEST); ++requests; return pending_error; }
int boot_write_img_confirmed(void)
{ ++confirmations; if (!confirm_error) { boot_confirmed=true; } return confirm_error; }
void sys_reboot(int type) { assert(type==SYS_REBOOT_COLD); ++reboots; }
int gw_radio_prepare_dfu(void) { ++reservations; return radio_error; }
void gw_radio_finish_dfu(void) { ++releases; }

static uint8_t command(uint8_t op,const uint8_t *args,size_t length)
{ return gw_dfu_command(op,args,length,output,&output_length); }
static void reset(void)
{
    dfu_mock_time=100;
    erases=writes=checks=requests=reboots=confirmations=reservations=releases=0;
    radio_error=write_error=hash_error=pending_error=confirm_error=erase_error=0;
    boot_confirmed=true; upload_slot=1;
    for (size_t i=0;i<sizeof(image);++i) { image[i]=(uint8_t)i; }
    memset(image,0,32); gw_put_u32(image,0x96f3b83d); gw_put_u16(image+8,32);
    gw_put_u32(image+12,1400);
    memcpy(running_image,image,sizeof(image));
    memset(hash,0x7a,sizeof(hash));
    assert(gw_dfu_init()==0 && gw_dfu_available() && !gw_dfu_active());
}
static uint8_t begin_request(uint64_t session,uint32_t size)
{
    uint8_t args[44]; gw_put_u64(args,session);gw_put_u32(args+8,size);memcpy(args+12,hash,32);
    return command(0x30,args,sizeof(args));
}
static uint8_t send_chunk(uint64_t session,uint32_t offset,uint16_t count)
{
    uint8_t args[14+GW_DFU_CHUNK_MAX];
    assert(offset+count<=sizeof(image) && count<=GW_DFU_CHUNK_MAX);
    gw_put_u64(args,session);gw_put_u32(args+8,offset);gw_put_u16(args+12,count);
    memcpy(args+14,image+offset,count);
    return command(0x31,args,14+count);
}
static uint8_t session_command(uint8_t opcode,uint64_t session)
{ uint8_t args[8];gw_put_u64(args,session);return command(opcode,args,sizeof(args)); }
static void full_upload(void)
{ assert(begin_request(7,sizeof(image))==GW_OK);assert(send_chunk(7,0,1024)==GW_OK);assert(send_chunk(7,1024,476)==GW_OK); }
static uint8_t verify_running(void)
{ uint8_t args[36];gw_put_u32(args,sizeof(image));memcpy(args+4,hash,32);return command(0x37,args,sizeof(args)); }

int main(void)
{
    reset();
    assert(begin_request(0,sizeof(image))==GW_BAD_PARAM && erases==0);
    assert(begin_request(7,65537)==GW_BAD_PARAM && reservations==0);
    radio_error=-EBUSY; assert(begin_request(7,sizeof(image))==GW_BUSY && erases==0);
    radio_error=0; assert(begin_request(7,sizeof(image))==GW_OK && erases==1);
    assert(output_length==62 && gw_get_u32(output+18)==65536);
    assert(begin_request(7,sizeof(image))==GW_OK && erases==1);
    assert(begin_request(7,sizeof(image)-1)==GW_CONFLICT);
    assert(begin_request(8,sizeof(image))==GW_BUSY);
    assert(send_chunk(8,0,512)==GW_NOT_FOUND);
    assert(send_chunk(7,512,512)==GW_CONFLICT);
    assert(send_chunk(7,0,512)==GW_OK && writes==1);
    assert(send_chunk(7,0,512)==GW_OK && writes==1);
    image[40]^=1;assert(send_chunk(7,0,512)==GW_CONFLICT);image[40]^=1;
    assert(send_chunk(7,512,512)==GW_OK);
    assert(send_chunk(7,0,512)==GW_STALE);
    assert(session_command(0x32,7)==GW_CONFLICT);
    assert(send_chunk(7,1024,476)==GW_OK && gw_get_u32(output+10)==1500);
    assert(session_command(0x32,7)==GW_OK && checks==1 && requests==1);
    assert(session_command(0x32,7)==GW_OK && requests==1);
    assert(output[8]==DFU_READY && gw_dfu_active());
    assert(session_command(0x35,7)==GW_OK && output_length==0);
    dfu_mock_time=1099;gw_dfu_poll();assert(reboots==0);
    assert(session_command(0x35,7)==GW_OK); /* retry must not extend reset deadline */
    dfu_mock_time=1100;gw_dfu_poll();assert(reboots==1);

    reset();full_upload();hash_error=-EILSEQ;
    assert(session_command(0x32,7)==GW_APPLY_FAILED && requests==0);
    assert(command(0x33,NULL,0)==GW_OK && output[8]==DFU_FAULT);
    assert(session_command(0x35,7)==GW_CONFLICT);
    assert(session_command(0x34,7)==GW_OK && releases==1 && !gw_dfu_active());
    assert(session_command(0x34,7)==GW_OK && erases==2);
    assert(begin_request(7,sizeof(image))==GW_STALE);
    assert(begin_request(8,sizeof(image))==GW_OK);

    reset();full_upload();pending_error=-EIO;
    assert(session_command(0x32,7)==GW_APPLY_FAILED && checks==1 && requests==1);
    assert(session_command(0x34,7)==GW_OK && releases==1);
    reset();assert(begin_request(7,sizeof(image))==GW_OK);write_error=-EIO;
    assert(send_chunk(7,0,512)==GW_APPLY_FAILED && gw_dfu_active());
    assert(command(0x33,NULL,0)==GW_OK && gw_get_u32(output+10)==0);

    reset();boot_confirmed=false;assert(gw_dfu_init()==0);
    assert(begin_request(7,sizeof(image))==GW_BUSY && erases==0);
    assert(command(0x33,NULL,0)==GW_OK && output[9]==0 && gw_get_u32(output+58)==120000);
    assert(command(0x36,NULL,0)==GW_CONFLICT && confirmations==0);
    running_image[100]^=1;assert(verify_running()==GW_APPLY_FAILED);
    assert(command(0x36,NULL,0)==GW_CONFLICT && confirmations==0);
    running_image[100]^=1;assert(verify_running()==GW_OK && output_length==0);
    confirm_error=-EIO;assert(command(0x36,NULL,0)==GW_APPLY_FAILED);
    confirm_error=0;assert(command(0x36,NULL,0)==GW_OK && boot_confirmed);
    dfu_mock_time+=120001;gw_dfu_poll();assert(reboots==0);
    assert(command(0x36,NULL,0)==GW_OK && confirmations==2);
    hash_error=-EILSEQ;assert(verify_running()==GW_APPLY_FAILED);
    assert(command(0x36,NULL,0)==GW_CONFLICT); /* a failed recheck revokes permission */
    reset();assert(command(0x36,NULL,0)==GW_CONFLICT); /* even already-confirmed images */
    assert(verify_running()==GW_OK && command(0x36,NULL,0)==GW_OK && confirmations==0);
    reset();boot_confirmed=false;assert(gw_dfu_init()==0);
    assert(verify_running()==GW_OK);
    dfu_mock_time+=120000;
    assert(command(0x36,NULL,0)==GW_STALE && confirmations==0);
    gw_dfu_poll();assert(reboots==1);
    reset();upload_slot=0;assert(gw_dfu_init()==-ENOTSUP && !gw_dfu_available());
    assert(begin_request(7,sizeof(image))==GW_UNSUPPORTED && erases==0);
    boot_confirmed=false;assert(gw_dfu_init()==-ENOTSUP);
    dfu_mock_time+=120000;gw_dfu_poll();assert(reboots==1);
    puts("DFU: bounds, replay, offsets, SHA failure, upgrade/abort, confirmation/revert tests passed");
    return 0;
}
