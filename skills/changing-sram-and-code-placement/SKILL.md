---
name: changing-sram-and-code-placement
description: Use when changing memory allocation, internal SRAM/PSRAM/RTC placement, IRAM attributes, linker fragments, sdkconfig options, task stacks or priorities, the NAM core split, or when internal heap runs short or the audio graph fails to load.
---

# SRAM and code placement

The ESP32-S3 has 512 KiB of internal SRAM. NAM's aligned coefficient banks, the reverb tank
and the USB path all compete for it with Gea's caches. Placement decides both whether the
graph loads and whether it meets its deadlines.

**Before any change, read** `docs/MEMORY.md` for the placement rules and what has already been
tried, and the pipeline section of `docs/ARCHITECTURE.md`. Don't reuse their numbers; measure
again with `measuring-pedal-audio`.

## Where placement is declared

| Concern                                 | File                                                               |
| --------------------------------------- | ------------------------------------------------------------------ |
| Gea framework state/caches → PSRAM      | `src/native/gea_memory.lf` (`gea.targets.esp32.ldFragments`)       |
| Gea cache sizes, UI task stack          | `gea.defines` in `package.json`                                    |
| IDF options                             | `src/native/sdkconfig.defaults`                                    |
| NAM bank arenas                         | `src/native/nam_banks/`, `src/audio/nam/runtime_allocation_plan.c` |
| Heap wraps (`tlsf_memalign_offs`, etc.) | `gea.targets.esp32.linkOptions`                                    |
| Moving Gea's own state out of SRAM      | `GEA_EMBEDDED_*_EXTERNAL` in `gea.defines`                         |

## Rules learned the hard way

- **Contiguity matters, not just total free memory.** Allocation order decides whether a bank
  fits. The boot heap census names each block's owner.
- **`MALLOC_CAP_INTERNAL` does not mean fast.** RTC fast memory (`0x600fe000`) is internal but
  sits on the peripheral bus. Request `MALLOC_CAP_DMA` for hot internal buffers. RTC memory is
  fine for cold stacks and TCBs.
- **Per-block code belongs in IRAM**: the USB HCD ISR, host callbacks, the client task and the
  NAM/Processor block entry points. Code fetched through the shared I-cache caused the misses
  during UI boot.
- **The global IRAM sdkconfig options broke OTA** while code ran from flash (`docs/MEMORY.md`).
  Place specific functions instead. `CONFIG_ESP_PHY_IRAM_OPT` does nothing on the S3.
- The reverb tank stays statically internal. The user requires it.
- Radio lifecycle work needs an internal stack. OTA metadata mapping asserts when run from a PSRAM
  stack. Never call `esp_bt_mem_release`.
- `CONFIG_ESP_IPC_TASK_STACK_SIZE` must be at least 1280.
- Weak hooks can quietly fail to link: a weak undefined reference does not pull its definition out
  of a static archive, so the hook stays null. Confirm your override actually runs.
- NAM split default is **8** (`src/audio/nam/CMakeLists.txt`). The USB client task (prio 20) must
  keep preempting stage A.

## Verifying a change

1. `npm run build:firmware` and the relevant host tests (`running-host-checks`).
2. After flashing, as authorised by the user: compare `HEAP MAP` against the audio-boot census in `logs`, and
   run an `AUDIO TRY` window with the user's preset mask. Check `miss=0/0`, `err=0` and silence
   only at stream start.
3. Test a maintenance round trip and an OTA whenever the change touches IRAM, sdkconfig or stacks.
   Those are what broke before.
