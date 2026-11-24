# BasiliskII ESP32-P4 Port - Main Component CMakeLists.txt

set(BASILISK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/basilisk")
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../src/board")

# Collect BasiliskII core sources (aligned with platformio.ini build_src_filter_common)
set(BASILISK_SOURCES
    ${BASILISK_DIR}/main.cpp
    ${BASILISK_DIR}/main_esp32.cpp
    ${BASILISK_DIR}/video_esp32.cpp
    ${BASILISK_DIR}/sys_esp32.cpp
    ${BASILISK_DIR}/timer_esp32.cpp
    ${BASILISK_DIR}/prefs_esp32.cpp
    ${BASILISK_DIR}/xpram_esp32.cpp
    ${BASILISK_DIR}/user_strings_esp32.cpp
    ${BASILISK_DIR}/adb.cpp
    ${BASILISK_DIR}/audio.cpp
    ${BASILISK_DIR}/audio_esp32.cpp
    ${BASILISK_DIR}/automation.cpp
    ${BASILISK_DIR}/boot_gui.cpp
    ${BASILISK_DIR}/cdrom.cpp
    ${BASILISK_DIR}/cpu_profiler.cpp
    ${BASILISK_DIR}/disk.cpp
    ${BASILISK_DIR}/driver_stubs.cpp
    ${BASILISK_DIR}/emul_op.cpp
    ${BASILISK_DIR}/ether.cpp
    ${BASILISK_DIR}/ether_esp32.cpp
    ${BASILISK_DIR}/extfs.cpp
    ${BASILISK_DIR}/extfs_esp32.cpp
    ${BASILISK_DIR}/firmware_update.cpp
    ${BASILISK_DIR}/hfs_boot.cpp
    ${BASILISK_DIR}/hfs_starter7.cpp
    ${BASILISK_DIR}/hid_descriptor.cpp
    ${BASILISK_DIR}/input_esp32.cpp
    ${BASILISK_DIR}/mac_clock_esp32.cpp
    ${BASILISK_DIR}/mac_splash.cpp
    ${BASILISK_DIR}/macos_util.cpp
    ${BASILISK_DIR}/net_router.cpp
    ${BASILISK_DIR}/opcode_sampler.cpp
    ${BASILISK_DIR}/perf_sampler.cpp
    ${BASILISK_DIR}/power_manager.cpp
    ${BASILISK_DIR}/preboot_screens.cpp
    ${BASILISK_DIR}/preboot_ui.cpp
    ${BASILISK_DIR}/prefs.cpp
    ${BASILISK_DIR}/prefs_items.cpp
    ${BASILISK_DIR}/quickdraw_accel.cpp
    ${BASILISK_DIR}/rom_patches.cpp
    ${BASILISK_DIR}/rsrc_patches.cpp
    ${BASILISK_DIR}/slot_rom.cpp
    ${BASILISK_DIR}/sony.cpp
    ${BASILISK_DIR}/starter_download.cpp
    ${BASILISK_DIR}/timer.cpp
    ${BASILISK_DIR}/touch_overlay.cpp
    ${BASILISK_DIR}/usb_msc.cpp
    ${BASILISK_DIR}/user_strings.cpp
    ${BASILISK_DIR}/video.cpp
    ${BASILISK_DIR}/xpram.cpp
)

# UAE CPU and AOT runtime sources
set(UAE_CPU_SOURCES
    ${BASILISK_DIR}/uae_cpu/basilisk_glue.cpp
    ${BASILISK_DIR}/uae_cpu/memory.cpp
    ${BASILISK_DIR}/uae_cpu/newcpu.cpp
    ${BASILISK_DIR}/uae_cpu/readcpu.cpp
    ${BASILISK_DIR}/uae_cpu/fpu/fpu_ieee.cpp
    ${BASILISK_DIR}/uae_cpu/generated/cpudefs.cpp
    ${BASILISK_DIR}/uae_cpu/generated/cpuemu.cpp
    ${BASILISK_DIR}/uae_cpu/generated/cpustbl.cpp
    ${BASILISK_DIR}/aot/aot_profile.cpp
    ${BASILISK_DIR}/aot/aot_runtime.cpp
    ${BASILISK_DIR}/aot/aot_selftest.cpp
)

idf_component_register(
    SRCS 
        "main.c"
        ${BASILISK_SOURCES}
        ${UAE_CPU_SOURCES}
    INCLUDE_DIRS 
        "."
        "${BASILISK_DIR}"
        "${BASILISK_DIR}/include"
        "${BASILISK_DIR}/uae_cpu"
        "${BASILISK_DIR}/uae_cpu/fpu"
        "${BASILISK_DIR}/uae_cpu/generated"
        "${BOARD_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/generated"
    REQUIRES 
        driver
        esp_timer
        fatfs
        sdmmc
        esp_psram
        vfs
)

# Add BasiliskII-specific compile definitions
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EMULATED_68K=1
    REAL_ADDRESSING=0
    DIRECT_ADDRESSING=0
    ROM_IS_WRITE_PROTECTED=1
    SIZEOF_SHORT=2
    SIZEOF_INT=4
    SIZEOF_LONG=4
    SIZEOF_LONG_LONG=8
    SIZEOF_VOID_P=4
    FPU_IEEE=1
    FPU_UAE=0
    FPU_X86=0
    ENABLE_MON=0
    USE_JIT=0
)

# Suppress specific warnings for BasiliskII code
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
    -Wno-sign-compare
    -Wno-missing-field-initializers
    -Wno-deprecated-enum-enum-conversion
    -Wno-return-type
    -Wno-pointer-arith
)
