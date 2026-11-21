#if TAB5_SD_BENCHMARK
#include <Arduino.h>
#include "board.h"
#include "board_sd_format.h"

void setup() {
    Serial.begin(115200);
    Board_Init();
    Serial.println("Ready. Send INSPECT.");
}

void loop() {
    if (!Serial.available()) { delay(10); return; }
    String line = Serial.readStringUntil('\n');
    line.trim();
    if (line == "INSPECT") {
        BoardSdCardInfo info = {};
        const char *err = BoardSD_InspectCard(&info);
        if (err) { Serial.printf("ERR %s\n", err); return; }
        Serial.printf("OK driver=%s width=%u clock_khz=%u bytes=%llu id=%s layout=%s "
                      "partitions=%u partition_type=%02x partition_lba=%u volume_bytes=%llu cluster_bytes=%u format_supported=%d token=%s\n",
                      info.driver, info.bus_width, info.clock_khz, (unsigned long long)info.capacity_bytes,
                      info.identity, info.layout, info.partition_count, info.partition_type, info.partition_lba,
                      (unsigned long long)info.volume_bytes, info.cluster_bytes,
                      info.format_supported, info.token);
    } else if (line.startsWith("ERASE FAT32 ")) {
        const char *err = BoardSD_FormatCard(line.c_str() + strlen("ERASE FAT32 "));
        Serial.println(err ? String("ERR ") + err : "OK formatted and mounted");
    } else {
        Serial.println("ERR expected INSPECT or ERASE FAT32 <confirmed-card-token>");
    }
}
#endif
