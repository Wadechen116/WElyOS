/* 真實黑盒子與 WFS probe，僅以記憶體磁碟替換裝置／配置器／鎖。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include "blk.h"
#include "pmm.h"
static unsigned char image[1024*4096];
static int writes, fail_primary, fail_backup, fail_alloc, outstanding, bad_access;
static int fake_rw(struct blkdev *d,uint64_t lba,unsigned count,void *buf,int write) {
    if (lba>d->sectors || count>d->sectors-lba) { bad_access=1; return -5; }
    if (!write && ((fail_primary && lba==0) || (fail_backup && lba==d->sectors-24))) {
        /* 回報讀取失敗之前可能已寫入部分緩衝區；caller 不可使用它。 */
        memset(buf,0,512); return -5;
    }
    if (write) { writes++; memcpy(image+lba*512,buf,count*512); }
    else memcpy(buf,image+lba*512,count*512);
    return 0;
}
void *pmm_alloc(void) { if (fail_alloc) return NULL; outstanding++; return malloc(4096); }
void pmm_free(void *p) { outstanding--; free(p); }
#include "blackbox.c"
#include "wfs_probe.c"
static void seed_record(unsigned slot,uint32_t seq,uint32_t boot,int type,const char *text) {
    struct bb_record r={0};
    r.magic=BB_MAGIC; r.seq=seq; r.boot=boot; r.type=(uint16_t)type; r.len=(uint16_t)strlen(text);
    memcpy(r.text,text,r.len); r.crc=crc32(&r,sizeof(r)-sizeof(r.crc));
    memcpy(image+sizeof(image)-8192+slot*sizeof(r),&r,sizeof(r));
}
int main(int argc,char **argv) {
    if (argc!=2) return 90;
    struct blkdev dev={.name="fake",.sectors=sizeof(image)/512,.rw=fake_rw};
    const char *kind=argv[1];
    int expected_enabled=1, expected_failed=-1, expected_seq=-1;
    int64_t expected_boot=-1;
    uint32_t result;
    if (!strcmp(kind,"seq-wrap")) {
        seed_record(0,UINT32_MAX-1,10,BB_BOOT,"old");
        seed_record(1,UINT32_MAX,11,BB_BOOT,"old");
        seed_record(2,0,12,BB_BOOT,"new");
        blackbox_init(&dev,1022); expected_seq=0; result=1022;
    } else if (!strcmp(kind,"boot-wrap")) {
        seed_record(0,1,UINT32_MAX,BB_BOOT,"max boot");
        blackbox_init(&dev,1022); expected_boot=UINT32_MAX; result=1022;
    } else if (!strcmp(kind,"bad-crc-boot")) {
        seed_record(0,1,9,BB_BOOT,"ok");
        seed_record(1,2,UINT32_MAX,BB_BOOT,"corrupt");
        image[sizeof(image)-8192+128+124]^=1;
        blackbox_init(&dev,1022); expected_boot=10; result=1022;
    } else if (!strcmp(kind,"failed-with-test")) {
        seed_record(0,1,1,BB_HEALTHY,"ok");
        seed_record(1,2,2,BB_BOOT,"failed");
        seed_record(2,3,3,BB_BOOT,"boot (test)");
        seed_record(3,4,4,BB_BOOT,"failed");
        seed_record(4,5,5,BB_BOOT,"failed");
        blackbox_init(&dev,1022); expected_failed=3; result=1022;
    } else if (!strcmp(kind,"ring")) {
        blackbox_init(&dev,1022);
        for (int i=0;i<70;i++) blackbox_log(BB_BOOT,"failed");
        expected_failed=64; result=1022;
    } else {
        if (!strncmp(kind,"tiny",4)) dev.sectors=(uint64_t)atoi(kind+4)*8;
        if (!strcmp(kind,"unknown")) { memset(image,0xa5,sizeof(image)); expected_enabled=0; }
        if (!strcmp(kind,"primary-io")) { fail_primary=1; expected_enabled=0; }
        if (!strcmp(kind,"backup-io")) { fail_backup=1; expected_enabled=0; }
        if (!strcmp(kind,"oom")) { fail_alloc=1; expected_enabled=0; }
        if (!strncmp(kind,"tiny",4) && atoi(kind+4)<66) expected_enabled=0;
        if (!strcmp(kind,"valid") || !strcmp(kind,"backup") || !strcmp(kind,"old-full")) {
            struct wsuper s;
            layout(&s,!strcmp(kind,"old-full") ? 1024:1022,64,strcmp(kind,"old-full")!=0);
            s.crc=sb_crc(&s);
            memcpy(image+(!strcmp(kind,"backup") ? (1022-1)*4096:0),&s,sizeof(s));
            if (!strcmp(kind,"old-full")) expected_enabled=0;
        }
        result=wfs_probe(&dev);
        blackbox_init(&dev,result);
        if (result && result<64) expected_enabled=0;
    }
    int actual_enabled=blackbox_enabled();
    unsigned failed=blackbox_failed_boots();
    struct bb_record latest={0}; blackbox_read(0,&latest);
    uint32_t boot=blackbox_boot();
    int before=writes;
    blackbox_log(BB_NOTE,"probe");
    int errors=actual_enabled!=expected_enabled || outstanding || bad_access || (!expected_enabled && writes);
    if (expected_failed>=0 && failed!=(unsigned)expected_failed) errors=1;
    if (expected_seq>=0 && latest.seq!=(uint32_t)expected_seq) errors=1;
    if (expected_boot>=0 && boot!=(uint32_t)expected_boot) errors=1;
    printf("{\"probe\":%u,\"enabled\":%d,\"expected_enabled\":%d,\"writes_after_probe\":%d,"
           "\"failed_boots\":%u,\"latest_seq\":%u,\"boot\":%u,\"alloc_leaks\":%d,\"bad_access\":%d}\n",
           result,actual_enabled,expected_enabled,writes-before,failed,latest.seq,boot,outstanding,bad_access);
    return errors;
}
