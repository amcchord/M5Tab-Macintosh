#include "../src/board/tab5/board_sd_format_tab5.cpp"

static Tab5SdMode mounted = Tab5SdMode::ArduinoSpi;
static bool usable=true;
bool Tab5SD_CanRetryMount(){return usable;}
void Tab5SD_BlockMounts(){usable=false;}
Tab5SdMode Tab5SD_MountedMode(){return mounted;}
bool Tab5SD_Unmount(){mounted=Tab5SdMode::None;return true;}
bool Tab5SD_Restore(Tab5SdMode m){mounted=m;restored=true;return true;}
int fakeClock(int,int *khz){*khz=10000;return ESP_OK;}
int Tab5SD_InitSpiBus(sdmmc_host_t &h,sdspi_device_config_t &d){h={1,SDMMC_HOST_FLAG_SPI,fakeClock};d={1};return ESP_OK;}
int Tab5SD_NativeConfig(sdmmc_host_t &h,sdmmc_slot_config_t &){h={0,0,fakeClock};return ESP_OK;}

int main() {
    BoardSdCardInfo info = {}, next = {};
    assert(BoardSD_FormatCard(nullptr) && BoardSD_FormatCard("bogus"));
    assert(!BoardSD_InspectCard(&info) && info.format_supported && restored);
    assert(!strcmp(info.layout,"Unknown or blank") && info.capacity_bytes==uint64_t{media_sectors}*512);
    assert(!BoardSD_InspectCard(&next) && strcmp(info.token,next.token));
    assert(BoardSD_FormatCard(info.token)); // superseded confirmation
    ++serial_number;
    assert(BoardSD_FormatCard(next.token)); // swapped card of identical capacity
    --serial_number;
    assert(BoardSD_FormatCard(next.token)); // consumed on failed identity check
    assert(!BoardSD_InspectCard(&info));media_sectors/=2;
    assert(BoardSD_FormatCard(info.token));media_sectors*=2;
    assert(!BoardSD_InspectCard(&info));sectors[0][0]=0x31;
    assert(BoardSD_FormatCard(info.token)); // sector zero changed
    assert(!mkfs_calls && !write_calls);
    serial_number=0;
    assert(!BoardSD_InspectCard(&info) && !info.format_supported && BoardSD_FormatCard(info.token));
    serial_number=1; media_sectors=65536;
    assert(!BoardSD_InspectCard(&info) && !info.format_supported && BoardSD_FormatCard(info.token));
    media_sectors=8*1024*1024;
    fail_read=true; assert(BoardSD_InspectCard(&info)); fail_read=false;
    assert(!mkfs_calls && !write_calls);

    // An exFAT boot sector inside a small MBR partition on a much bigger card.
    auto &mbr=sectors[0];mbr={};mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0x07;
    mbr[450]=0x83;put32(&mbr[454],64);put32(&mbr[458],100);
    mbr[466]=0x07;put32(&mbr[470],8192);put32(&mbr[474],500000);
    auto &exfat=sectors[8192];exfat[510]=0x55;exfat[511]=0xaa;
    memcpy(&exfat[3],"EXFAT   ",8);exfat[108]=9;exfat[109]=6;put32(&exfat[72],450000);
    memcpy(sectors[1].data(),"EFI PART",8);
    memcpy(sectors[media_sectors-1].data(),"EFI PART",8);
    assert(!BoardSD_InspectCard(&info) && !strcmp(info.layout,"exFAT"));
    assert(info.partition_count==2 && info.partition_type==7 && info.partition_lba==8192 && info.volume_bytes==uint64_t{450000}*512);
    assert(!mkfs_calls && !write_calls);
    assert(!BoardSD_FormatCard(info.token) && mkfs_calls==1 && write_calls==2);
    assert(BoardSD_FormatCard(info.token) && mkfs_calls==1);
    assert(!BoardSD_InspectCard(&info) && !strcmp(info.layout,"FAT32"));
    assert(info.partition_type==0x0c && info.cluster_bytes==32768 && mounted==Tab5SdMode::ArduinoSpi);
    // A partition-specific FatFs mapping must never overwrite only part of the card.
    VolToPart[1].pt=1; assert(BoardSD_FormatCard(info.token) && mkfs_calls==1);
}
