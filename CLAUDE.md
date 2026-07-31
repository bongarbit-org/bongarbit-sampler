# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Firmware for a button-triggered one-shot sampler: ESP32-S3 N16R8 + PCM5102A I2S DAC + microSD (SPI), built on the [M16](https://github.com/algomusic/M16) audio library under Arduino/PlatformIO. Four pads, samples resident in PSRAM, no OS scheduler in the audio path beyond M16's own I2S task.

## Build / flash / debug

```bash
pio run                      # build default env (sampler firmware)
pio run -t upload            # flash
pio device monitor           # serial log, 115200 baud
pio run -e sd_test -t upload # flash the standalone SD diagnostic instead
```

PlatformIO lives in a project-local venv (gitignored), so the commands above are `.venv/bin/pio ...` unless you activate it. Create it with `python3 -m venv .venv && .venv/bin/pip install platformio` — Debian's system Python is PEP 668 externally-managed, so a bare `pip install` is refused.

```bash
pio test -e native            # host unit suite, no board
```

Trigger logic is verified on the host; everything else — audio, SD, PSRAM residency, the DMA ring depth — is verified only by flashing. Neither substitutes for the other: a green build and a green suite say nothing about whether PSRAM initialised.

- [src/trigger_input.h](src/trigger_input.h) — `TriggerInput`, the state machine every button runs. **Compiled into the firmware AND into the tests.** It lived in `main.cpp` with a hand-written copy in the simulator until the two drifted: the copy grew a `confirm_us == 0` fast path the firmware never had, on the exact configuration the bench procedure in [docs/circuit-rc.md](docs/circuit-rc.md) uses. Tests were green while measuring something the board would not do. Keep it shared — divergence should be impossible by construction, not by vigilance.
- [sim/trigger_model.h](sim/trigger_model.h) — header-only scaffolding: pin-trace generator, RC filter model, scoring runner, plus models of the two **rejected** designs. Those are copies on purpose; they no longer exist in the firmware and exist only to keep failing in the documented way. **Timestamps are `uint32_t`**, matching `micros()`, so unsigned-difference arithmetic wraps in simulation exactly as on the board; `run()` takes a time base so the wrap boundary itself is testable.
- [sim/trigger_sim.cpp](sim/trigger_sim.cpp) — the sweep/report CLI (`--sweep=all`).
- [test/test_trigger](test/test_trigger) — the Unity suite.

The two windows are instance members with defaults from `TRIG_CONFIRM_US` / `TRIG_RELEASE_MS` (both `#ifndef`-guarded) rather than macros read inline, so sweeps and tests can vary them per instance. Costs 8 bytes per button.

Always pass `-e native`. A bare `pio test` also targets the ESP32 environments, which means trying to upload.

The suite is organised by *reason*, not code shape: each rejected design has a test named after how it failed (`test_rejected_level_rearm_machine_guns`, `test_rejected_edge_rearm_latches_on_short_contact`), so reintroducing one fails a test that explains itself. `test_harness_reproduces_historical_machine_gun` pins the model to the originally measured 112/3 fire counts — if it fails, the model drifted and no other assertion in the file means anything.

`platform` is pinned to an exact **pioarduino** release, not to `espressif32`. M16 needs Arduino core 3.x (ESP-IDF 5) for `driver/i2s_std.h`, and the official PlatformIO platform does not ship it — as of 7.0.1 it still pins core 2.0.17, and the build fails outright. pioarduino is the community fork that tracks core 3.x. Do not "modernise" this back to `platform = espressif32`.

The two environments share `src/` and are separated by `build_src_filter` because each has its own `setup()`/`loop()` pair. Adding a third sketch file means adding a filter exclusion to **every** env, not just the new one.

- `esp32-s3-devkitc-1` (default) — [src/main.cpp](src/main.cpp), the sampler. Uses SdFat via M16.
- `sd_test` — [src/test_sd.cpp](src/test_sd.cpp), the stock Arduino `SD.h` example, kept to isolate SD hardware faults from SdFat/M16-specific ones.

## Real-time design (the load-bearing decisions)

**Samples are fully resident in RAM before any audio plays.** `Wav::load*()` runs once in `setup()` and prefers PSRAM (8MB on this board). The SD card is never touched after boot — it can be physically removed. Any change that reads from SD, allocates, or blocks during playback breaks the latency guarantee this project exists for. Streaming-from-SD would reintroduce FAT seek jitter that PSRAM residency was chosen to eliminate.

**`audioUpdate()` ([src/main.cpp:206](src/main.cpp:206)) is M16's per-sample callback**, invoked from its I2S feed task, not from `loop()`. It must stay allocation-free, lock-free, and free of `Serial`/SD/`delay()`. It ends in `i2s_write_samples()`, which is the backpressure point. All voices are summed into `int32_t` and clipped once at the end — voices play concurrently, so never rewrite this to output a single "last triggered" voice.

**Two deliberately different debounce strategies**, and this asymmetry is intentional, not an oversight:

All four buttons run one shared `TriggerInput` state machine. Fires on the **raw** pin read after `TRIG_CONFIRM_US` (200 µs, the only defense against phantom hits), then disarms until the raw pin reads HIGH continuously for `TRIG_RELEASE_MS` (3 ms). No debounce library is involved.

The sharing is the point. GPIO4 used to have its own wait-for-stable debounce at 25 ms; having two divergent implementations in one trigger path is what let the pad retrigger bug survive review. Do not reintroduce a second one.

  Both guards read the pin directly, and that is load-bearing. They were originally derived from Bounce2's debounced state, which lags the pin by its interval — rearming on the debounced *level* re-fired ~112 times per strike, and rearming on the debounced *edge* (`rose()`) latched the pad off permanently whenever contact time was under `bounce + interval`. Any rearm condition read from a filtered state couples correctness to the filter's constant. Do not reintroduce one. [sim/trigger_sim.cpp](sim/trigger_sim.cpp) measures all of this on the host.

  `PAD_CONFIRM_US` must stay well under one contact-bounce segment (~300 µs). Above that it cannot be satisfied until the whole bounce settles and the trigger latency becomes the bounce duration: 200 µs costs 0.2 ms, 1 ms costs 3 ms.

The pad scheme is the reference pattern for any new trigger input. GPIO4 is the legacy path.

**The I2S DMA ring is the latency budget.** M16 feeds it one sample at a time and blocks when full, so ring depth ≈ trigger-to-sound time. Its default of 4 × 512 frames is 46 ms, sized to survive a host flash-sector erase — a stall that cannot happen here, since samples are PSRAM-resident, SD is untouched after boot, and nothing writes flash at runtime. [src/main.cpp](src/main.cpp) overrides it to 4 × 64 = 5.8 ms **before** `#include "M16.h"`, guarded by a `static_assert`: M16 uses `#ifndef`, so moving the defines below the include silently restores 46 ms with no other symptom. This value is not hardware-validated — tune it against a four-voice roll, the heaviest `audioUpdate()` path.

Serial logging is split: `DEBUG_BOOT` (setup only, free, leave on) and `DEBUG_TRIGGERS` (one line per strike, from inside the polling loop, **off by default**). USB CDC blocks when the host is not draining, which puts an unbounded stall in the trigger path — never leave it on while judging latency, and don't add unconditional Serial output to `loop()`.

## Compile-time sample source

`USE_SD_SAMPLER` ([src/main.cpp:32](src/main.cpp:32)) selects where audio buffers come from, and it gates more than it looks like:

- `1` (default): GPIO4 plays `FILE_INDEX` (first `.wav` the card enumerates); GPIO5/6/7 play `/1.wav`, `/2.wav`, `/3.wav`. All the pad code — pins, `Wav`/`Samp` arrays, the trigger state arrays, and the mixing loop in `audioUpdate()` — lives inside `#if USE_SD_SAMPLER`.
- `0`: single voice from an array in flash (`.rodata`), no SD card, **no pads at all**. Changing the embedded sample means editing both the `#include` and the `samp.setTable()` call.

`Samp::setTable()` takes the source sample rate and resamples internally, so a file's native rate does not require reconfiguring the I2S clock.

## Embedding a sample in flash

```bash
python3 tools/wav_to_header.py <input.wav> src/<name>_data.h <ARRAY_NAME>
```

[tools/wav_to_header.py](tools/wav_to_header.py) downconverts 8/24/32-bit PCM to 16-bit using the same arithmetic as M16's `Wav::readFramesFast()`, so a sample is bit-identical whether it plays from SD or from flash. If M16's conversion changes, this script has to follow it.

Only `src/kick_normalizado_data.h` is included by the firmware. `src/kick_data.h` (from the un-normalized `kick.wav`) is a leftover and is referenced nowhere.

## Pin constraints — N16R8 specific

Full wiring rationale, including the PCM5102A control pins (`SCK`→GND for internal clock, `XSMT`→3V3 or output stays muted) and the common-ground requirement, is in [docs/circuit.md](docs/circuit.md). Read it before changing any pin assignment.

The trap: **GPIO 33–37 are consumed by the Octal PSRAM** on R8 modules. Generic ESP32-S3 pinout guides list them as free and suggest them for VSPI; using them here bricks PSRAM. SD therefore sits on HSPI (10–13). Also unusable: 0/3/45/46 (strapping), 26–32 (flash), 43/44 (UART0), 19/20 (native USB).

`board = esp32-s3-devkitc1-n16r8`, **not** the generic `esp32-s3-devkitc-1` — that one is the N8 (8MB flash, no PSRAM). The N16R8 definition carries `default_16MB.csv`, `memory_type = qio_opi`, `psram_type = opi`, `-DBOARD_HAS_PSRAM` and the correct 16MB size limits together. This used to be a stack of `board_build`/`board_upload` overrides on top of the generic board; they mostly worked, but left `maximum_size` at the N8's 8MB while writing a partition table running to 0xff0000, so firmware size was checked against the wrong ceiling.

Two `build_flags` are load-bearing, both about serial. `-DARDUINO_USB_CDC_ON_BOOT=1` because the board has no UART bridge chip — without it Serial goes to UART0 (GPIO 43/44), wired to nothing, and the monitor stays empty while the firmware runs fine. `-DARDUINO_USB_MODE=1` because the generic board definition provided it via `extra_flags` and the n16r8 one does not; drop it and serial goes quiet the same way.

## Conventions

Prose docs (`README.md`, `docs/circuit.md`) are written in Brazilian Portuguese; source comments are in English. Match whichever file you are editing. Comments in `main.cpp` explain *why* a timing or memory choice was made rather than what a line does — preserve that when refactoring.
