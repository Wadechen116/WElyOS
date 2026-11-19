/* 真實快取、摘取的 WFS 函式;只替換磁碟、頁配置及未測試的 checksum bookkeeping。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "bcache.h"
#include "blk.h"
#include "mmu.h"
#include "uapi/errno.h"
#define S_PERM_MASK 0777u
struct inode { uint64_t size, mtime; unsigned mode, uid, gid; };
#include "wfs_types.h"
#define BLOCKS 160u
static uint8_t image[BLOCKS][BSIZE], before_image[BLOCKS][BSIZE];
static struct wsuper sb;
static uint32_t free_blocks, balloc_hint;
static int fs_failed, fail_read=-1, fail_verify=-1, fail_write=-1;
static unsigned reads,writes,rejected,csum_updates;
static void *allocation;
static int covered(uint32_t no) { return no>=sb.data_start && no<sb.nblocks; }
static void csum_set(uint32_t no,uint32_t crc) { (void)no;(void)crc;csum_updates++; }
void *pmm_alloc_pages(size_t pages) { allocation=calloc(pages,PAGE_SIZE);return allocation; }
void pmm_free_pages(void *p,size_t pages) { (void)pages;free(p);allocation=NULL; }
_Noreturn void panic(const char *fmt,...) {
    va_list ap;va_start(ap,fmt);vfprintf(stderr,fmt,ap);va_end(ap);abort();
}
static int ram_rw(struct blkdev *dev,uint64_t lba,unsigned count,void *buf,int write) {
    (void)dev;
    if (lba% BSECT || count!=BSECT || lba/BSECT>=BLOCKS) abort();
    unsigned no=(unsigned)(lba/BSECT);
    if (write) {
        writes++;
        if ((int)no==fail_write) return -E_IO;
        memcpy(image[no],buf,BSIZE);
    } else {
        reads++;
        if ((int)no==fail_read) return -E_IO;
        memcpy(buf,image[no],BSIZE);
    }
    return 0;
}
static int verify(uint32_t no,const uint8_t *data) {
    (void)data;
    if ((int)no==fail_verify) { rejected++;return -E_IO; }
    return 0;
}
static void write_failed(uint32_t no) { (void)no;fs_failed=1; }
static const struct bcache_ops callbacks={.verify=verify,.write_failed=write_failed};
static struct blkdev device={.name="ram",.sectors=BLOCKS*BSECT,.rw=ram_rw};
#include "wfs_functions.c"
static void allocated(unsigned no) { image[sb.bitmap_start][no/8]|=(uint8_t)(1u<<(no%8)); }
static int used(unsigned no) { return (image[sb.bitmap_start][no/8]>>(no%8))&1; }
static struct wnode setup(int indirect) {
    sb=(struct wsuper){.nblocks=BLOCKS,.ninodes=64,.bitmap_start=1,.inode_start=2,.data_start=8};
    free_blocks=100;
    struct wnode n={.inum=1,.in={.mode=0644,.size=(indirect?12u:3u)*BSIZE}};
    n.d.size=(uint32_t)n.in.size;
    for (unsigned i=0;i<(indirect?NDIRECT:3u);i++) {
        n.d.addr[i]=20+i;allocated(20+i);memset(image[20+i],0xA5,BSIZE);
    }
    if (indirect) {
        n.d.indirect=40;allocated(40);
        uint32_t tab[NINDIRECT]={41,42};
        memcpy(image[40],tab,sizeof(tab));
        allocated(41);allocated(42);memset(image[41],0xA5,BSIZE);memset(image[42],0xA5,BSIZE);
    }
    if (bcache_init(&device,&sb.nblocks,&callbacks)) abort();
    return n;
}
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr,"line %d: %s\n",__LINE__,#expr);errors++; } } while (0)
int main(int argc,char **argv) {
    if (argc!=2) return 90;
    const char *kind=argv[1];int errors=0,rc=0;
    int indirect=strstr(kind,"indirect")!=NULL;
    struct wnode n=setup(indirect),old=n;
    memcpy(before_image,image,sizeof(image));
    if (!strncmp(kind,"bitmap-",7)) {
        if (strstr(kind,"verify")) fail_verify=1;else fail_read=1;
        bfree(20);
        CHECK(fs_failed==1);CHECK(free_blocks==100);CHECK(bcache_sync()==0);
        CHECK(memcmp(image,before_image,sizeof(image))==0);CHECK(writes==0);
    } else if (!strcmp(kind,"free-ok") || !strcmp(kind,"free-invalid")) {
        int invalid=strstr(kind,"invalid")!=NULL;
        bfree(invalid?BLOCKS:20);CHECK(bcache_sync()==0);
        CHECK(free_blocks==(invalid?100u:101u));CHECK(used(20)==invalid);CHECK(fs_failed==0);
    } else if (!strcmp(kind,"sync-write") || !strcmp(kind,"evict-write")) {
        unsigned count=!strcmp(kind,"sync-write")?1:BCACHE_NBUF;
        for (unsigned i=0;i<count;i++) {
            struct buf *b=bget(60+i,0);CHECK(b!=NULL);
            if (b) { b->dirty=1;bput(b); }
        }
        fail_write=60;
        if (count==1) { rc=bcache_sync();CHECK(rc==-E_IO); }
        else CHECK(bget(140,1)==NULL);
        CHECK(fs_failed==1);CHECK(writes==1);CHECK(memcmp(image,before_image,sizeof(image))==0);
    } else if (!strncmp(kind,"read-",5)) {
        uint8_t data[16];memset(data,0xCC,sizeof(data));
        int hole=strstr(kind,"hole")!=NULL,normal=strstr(kind,"-ok")!=NULL;
        if (hole) {
            if (indirect) memset(image[40],0,BSIZE);else n.d.addr[0]=0;
        } else if (!normal) {
            if (strstr(kind,"verify")) fail_verify=40;else fail_read=indirect?40:20;
        }
        old=n;
        rc=(int)file_read(&n,data,sizeof(data),indirect?(uint64_t)NDIRECT*BSIZE:0);
        CHECK(rc==(hole || normal ? (int)sizeof(data):-E_IO));
        if (hole || normal) for (unsigned i=0;i<sizeof(data);i++) CHECK(data[i]==(hole?0:0xA5));
        CHECK(memcmp(&n,&old,sizeof(n))==0);CHECK(writes==0);
        printf("read_first_byte=%u ",data[0]);
    } else {
        int success=strstr(kind,"-ok")!=NULL;
        uint64_t size=indirect?(uint64_t)NDIRECT*BSIZE+17:17;
        if (!strcmp(kind,"indirect-read") || !strcmp(kind,"indirect-verify")) {
            size=0;
            if (strstr(kind,"verify")) fail_verify=40;else fail_read=40;
        } else if (!success) fail_read=indirect?41:20;
        rc=file_truncate(&n,size);
        if (!success) {
            CHECK(rc==-E_IO);CHECK(memcmp(&n,&old,sizeof(n))==0);
            CHECK(free_blocks==100);CHECK(bcache_sync()==0);CHECK(writes==0);
            CHECK(memcmp(image,before_image,sizeof(image))==0);
            /* 成功的 bget 都必須 bput:佔滿全部槽位能抓到前置讀取失敗時的 pin 洩漏。 */
            struct buf *held[BCACHE_NBUF];unsigned got=0;
            for (;got<BCACHE_NBUF;got++) { held[got]=bget(60+got,0);if (!held[got]) break; }
            CHECK(got==BCACHE_NBUF);for (unsigned i=0;i<got;i++) bput(held[i]);
        } else {
            CHECK(rc==0);CHECK(n.in.size==size);CHECK(n.d.size==size);CHECK(bcache_sync()==0);
            unsigned tail=indirect?41:20;
            for (unsigned i=0;i<BSIZE;i++) CHECK(image[tail][i]==(i<17?0xA5:0));
            CHECK(free_blocks==(indirect?101u:102u));CHECK(fs_failed==0);
            if (indirect) { CHECK(used(42)==0);CHECK(n.d.indirect==40); }
            else { CHECK(n.d.addr[1]==0);CHECK(n.d.addr[2]==0); }
        }
    }
    printf("{\"rc\":%d,\"errors\":%d,\"reads\":%u,\"writes\":%u,\"fs_failed\":%d,\"free_blocks\":%u,\"verify_rejected\":%u}\n",
           rc,errors,reads,writes,fs_failed,free_blocks,rejected);
    bcache_free();return errors!=0;
}
