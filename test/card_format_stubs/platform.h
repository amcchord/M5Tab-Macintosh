// Host card transport for the production format state machine. The destructive
// calls record every sector write and require the simulated card's actual drive.
#pragma once
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <array>

using BYTE = uint8_t;
using sdspi_dev_handle_t = int;
using esp_err_t = int;
using FRESULT = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=2, SDMMC_HOST_FLAG_SPI=4;
constexpr int SDMMC_HOST_SLOT_0=0, SDSPI_DEFAULT_HOST=1, CTRL_SYNC=0;
constexpr int FR_OK=0, RES_OK=0, FM_FAT32=2, MALLOC_CAP_DMA=1, MALLOC_CAP_INTERNAL=2;
struct sdmmc_host_t { int slot, flags; int (*get_real_freq)(int,int*); };
struct sdspi_device_config_t { int host_id; };
struct sdmmc_slot_config_t {};
struct sdmmc_card_t {
    sdmmc_host_t host;
    struct { int mfg_id, oem_id; char name[8]; int revision, serial, date; } cid;
    struct { int capacity, sector_size; } csd;
    unsigned log_bus_width, is_mem, is_mmc;
};
struct MKFS_PARM { unsigned fmt, n_fat, align, au_size; };
struct PARTITION { unsigned char pd, pt; };
inline PARTITION VolToPart[2]={{0,0},{1,0}};
inline struct { template<class... A> void printf(const char *, A...) {} } Serial;
inline const char *esp_err_to_name(int){return "fixture";}
inline unsigned serial_number=1234, media_sectors=8*1024*1024, write_calls, mkfs_calls;
inline bool fail_read=false, restored=false;
inline std::map<unsigned,std::array<BYTE,512>> sectors;
inline void put16(BYTE *p,uint16_t v){p[0]=v;p[1]=v>>8;}
inline void put32(BYTE *p,uint32_t v){put16(p,v);put16(p+2,v>>16);}
inline void esp_fill_random(void *p,size_t n){static uint8_t nonce=0;memset(p,++nonce,n);}
inline uint32_t esp_rom_crc32_le(uint32_t x,const uint8_t *b,size_t n){
    while(n--)x=x*33+*b++;
    return x;
}
inline void *heap_caps_aligned_alloc(size_t a,size_t n,int){void *p;return posix_memalign(&p,a,n)?nullptr:p;}
inline int sdmmc_host_init(){return ESP_OK;}
inline int sdmmc_host_init_slot(int,const sdmmc_slot_config_t *){return ESP_OK;}
inline int sdmmc_host_deinit_slot(int){return ESP_OK;}
inline int sdspi_host_init(){return ESP_OK;}
inline int sdspi_host_init_device(const sdspi_device_config_t *,int *slot){*slot=5;return ESP_OK;}
inline int sdspi_host_remove_device(int slot){assert(slot==5);return ESP_OK;}
inline int spi_bus_free(int){return ESP_OK;}
inline int sdmmc_card_init(const sdmmc_host_t *host,sdmmc_card_t *c){
    c->host=*host;c->csd={(int)media_sectors,512};c->cid={3,127,"SD32G",1,(int)serial_number,1};
    c->is_mem=1;c->log_bus_width=c->host.flags&SDMMC_HOST_FLAG_SPI?0:2;return ESP_OK;
}
inline int sdmmc_read_sectors(sdmmc_card_t *,BYTE *p,unsigned n,unsigned count){
    assert(count==1&&n<media_sectors);if(fail_read)return 1;
    auto it=sectors.find(n);
    if(it!=sectors.end())memcpy(p,it->second.data(),512);else memset(p,0,512);
    return ESP_OK;
}
inline int ff_diskio_get_drive(BYTE *n){*n=1;return ESP_OK;}
inline void ff_diskio_register_sdmmc(BYTE d,sdmmc_card_t*){assert(d==1);}
inline void ff_diskio_unregister(BYTE d){assert(d==1);}
inline int disk_read(BYTE d,BYTE *p,unsigned n,unsigned c){assert(d==1);return sdmmc_read_sectors(nullptr,p,n,c);}
inline int disk_write(BYTE d,BYTE *p,unsigned n,unsigned c){
    assert(d==1&&c==1&&n<media_sectors);memcpy(sectors[n].data(),p,512);++write_calls;return RES_OK;
}
inline int disk_ioctl(BYTE d,int cmd,void*){assert(d==1&&cmd==CTRL_SYNC);return RES_OK;}
inline int f_mkfs(const char *p,const MKFS_PARM *opts,BYTE *,unsigned){
    assert(!strcmp(p,"1:")&&opts->fmt==FM_FAT32&&opts->n_fat==2&&opts->align==2048);++mkfs_calls;
    auto &m=sectors[0];m={};m[510]=0x55;m[511]=0xaa;m[450]=0x0c;
    put32(&m[454],63);put32(&m[458],media_sectors-63);
    auto &b=sectors[63];b={};b[510]=0x55;b[511]=0xaa;put16(&b[11],512);
    b[13]=opts->au_size/512;put16(&b[14],32);b[16]=2;put32(&b[32],media_sectors-63);
    put32(&b[36],2048);put32(&b[44],2);return FR_OK;
}
