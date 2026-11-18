# Ahead-of-time translation

These tools turn the 68k code a Mac spends its time in into C++ that is
compiled into the firmware. [EMULATOR_PERFORMANCE.md](../../EMULATOR_PERFORMANCE.md)
explains the design, its safety rules and the measurements. This page is the
procedure.

For a new hardware session, the [Tab5 hardware handoff](../../TAB5_HARDWARE_HANDOFF.md#4-profile-finder-and-an-application-then-time-an-independent-workload) gives a Finder and drawing workload, repeatable collection commands and a third application to time independently.

Nothing here runs on the device except the profiling and self-test builds.
No code is generated at run time.

## What is and is not in the repository

| In the repository | Local only |
|---|---|
| The tools, the runtime (`src/basilisk/aot`), the tests | `src/basilisk/aot/aot_rom_generated.cpp` |
| `hot_q650.txt`: ROM offsets and execution counts | RAM code profiles (`*-ram.txt`): they contain code from the System file and applications |
| | ROM images |

The generated file is a translation of Apple's ROM and system software. It is
git-ignored; do not commit or publish it, or firmware built with it, without
deciding that you may. A build without the file links an empty image and
interprets everything, exactly as 5.0.1 does.

## Steps

The commands below use the pre-v3 Tab5 environments. On a v3.1 or newer
Tab5, use `esp32p4_aot_profile_rev3`, `esp32p4_aot_selftest_rev3` and
`esp32p4_pioarduino_rev3` respectively, including the build directory names.

```sh
PIO=.build/release-tools/bin/pio
PY=.build/release-tools/bin/python
PORT=/dev/cu.usbmodemXXXX

# 1. Profile. The profiling build counts every instruction it dispatches.
$PIO run -e esp32p4_aot_profile          # flash .pio/build/esp32p4_aot_profile/firmware.bin at 0x10000
$PY tools/aot/aot_profile.py start --port $PORT
$PY tools/speedometer_benchmark.py --port $PORT --no-reset --suite rating --label profile
#    ...or use the Mac the way you want it to be fast
$PY tools/aot/aot_profile.py stop --port $PORT --out rom-counts.txt --ram ram.txt

# 2. The ROM as the emulator runs it (Basilisk patches it at start-up).
$PY tools/aot/dump_rom.py --port $PORT --out patched.rom

# 3. Translate.
tools/aot/run_translate.sh patched.rom --hot rom-counts.txt --ram-profile ram.txt \
    --min-count 1000 --ram-min-count 3000 --min-region-insns 2

# 4. Test on the host, then on the device.
bash test/aot_host/build.sh .pio/aot-host src/basilisk/aot/aot_rom_generated.cpp
.pio/aot-host/aot_diff patched.rom --trials 12
$PIO run -e esp32p4_aot_selftest         # flash it; it prints [AOT TEST] RESULT PASS or FAIL

# 5. Build the firmware as usual, and check that the translated code in it
#    can run from its copy in PSRAM.
$PIO run -e esp32p4_pioarduino_debug
python3 tools/aot/check_blob.py .pio/build/esp32p4_pioarduino_debug/firmware.elf \
    --object .pio/build/esp32p4_pioarduino_debug/src/basilisk/aot/aot_rom_generated.cpp.o
```

Do not flash a firmware that `check_blob.py` rejects; build it with
`-DAOT_PSRAM=0` instead, which runs the translated code from flash.

`aot_translate.py` prints how much of the profile it translated and what it
left to the interpreter, by reason. `--ram-exclude START:END` keeps an address
range out, for example an application's heap.

Translate again whenever `generated/cpuemu.cpp` changes: region bodies are
copies of the handler bodies at translation time.

## Reading a translation report

The report is at `.pio/aot-tools/report.json`, or in `AOT_BUILD_DIR` when
using a separate experiment directory. Each share weights a guest instruction
by its execution count. ROM and RAM have separate denominators.

| Report field | Interpretation |
| --- | --- |
| `rom_translated_share`, `ram_translated_share` | Share covered by decoded instructions kept for consideration before entry selection |
| `rom_installed_share`, `ram_installed_share` | Share inside emitted regions with a selected entry. Device installation also requires matching bytes at run time. |
| `rom_hot_untranslated`, `ram_hot_untranslated` | Up to 20 omitted guest PCs per address space with their counts and reasons, ordered by decreasing count |
| `*_not_translated_by_executions` | Aggregated decoder and flow-check failures by reason |

Use a frequent omitted PC to choose an experiment. For example, several
addresses rejected for the same instruction may justify examining that
handler. Addresses below the execution threshold suggest a different
experiment with the training input or entry budget. Confirm changes with
the interpreter comparison, the linked blob check and the independently
timed application.

## Files

| File | Purpose |
|---|---|
| `dump_optable.cpp` | Prints UAE's decoded opcode table for the translator |
| `aot_profile.py` | Starts and reads the firmware's execution counters |
| `dump_rom.py` | Saves the patched ROM from the running firmware |
| `aot_translate.py` | Profile and memory image in, `aot_rom_generated.cpp` out |
| `check_blob.py` | Checks a linked firmware: can its translated code be moved to PSRAM? |
| `run_translate.sh` | Builds `dump_optable` and runs the translator with the usual paths |
| `make_test_rom.py` | Generated 68k code for `test/test_aot.py` |
| `hot_q650.txt` | ROM execution counts of a Speedometer 4.02 Performance Rating on the Tab5 |

## Rules the translator keeps

- Only addresses the profile counted are instruction starts. A sampled PC can
  lie inside an instruction and is never used.
- An entry point is never placed inside the bytes of another executed
  instruction.
- An instruction whose handler does anything the translator does not
  recognize ends the region; the interpreter runs it.
- A RAM profile is read after the run. Where an instruction's successor ran
  far less often than it did, the memory no longer holds the code that was
  counted, and it is left out.
