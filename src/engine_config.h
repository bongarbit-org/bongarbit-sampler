#pragma once
/*
 * The I2S DMA ring every block engine in this repo runs on: the sampler
 * (main.cpp), the Agbau (agbau_main.cpp) and the Agbau storage bench
 * (test_agbau_storage.cpp). One definition, so the three cannot drift apart -
 * they used to carry a copy each, asserts and all.
 *
 * Not included directly. audio_block.h includes it, right after defining
 * AUDIO_BLOCK_FRAMES, because the two numbers are only meaningful together and
 * because every block engine has to include audio_block.h anyway: a sketch
 * cannot render an AudioBlock and forget its ring. The one thing a sketch can
 * still get wrong is the order, and that fails the build below.
 *
 * Arduino-free, like everything it rides in with: on the host the ring is two
 * unused macros, and the asserts at the bottom compile in the test suite too.
 */

#ifndef AUDIO_BLOCK_FRAMES
  #error "engine_config.h comes in through audio_block.h - include that instead"
#endif

// M16 sizes its ring with #ifndef and expands the two macros inside M16.h
// itself, so the override only works if it is seen BEFORE #include "M16.h".
// After it, the ring is already M16's 4 x 512 = 46ms and redefining the
// macros changes nothing but a warning. This used to be a static_assert
// after the include in each sketch, which caught a dropped override but not
// one moved below the include together with its assert.
#ifdef M16_H_
  #error "audio_block.h (which sets the I2S ring, see engine_config.h) must be included BEFORE M16.h - after it, M16's 46ms ring is already in place"
#endif

// --- Audio output latency ---
// The I2S DMA ring is where the trigger-to-sound time is spent. The library
// default is 4 x 512 = 2048 frames = 46ms at 44.1kHz, sized to survive a flash
// sector erase on the host (~30-40ms).
//
// M16 drains it in M16_BLOCK_SIZE (32) frame bursts: audioBlockWrite()
// accumulates a block and issues one i2s_channel_write() per 32 frames, which
// is what paces the audio task. It fed the ring one frame per driver call
// until the library changed under this project - see the pin note in
// platformio.ini. That is why the pin exists, and why the ring depth is the
// figure to tune rather than anything about how it gets filled.
//
// That worst case does not exist in these firmwares: samples are resident
// before the first sound (in PSRAM for the sampler, in mapped flash for the
// Agbau), the SD card is never touched after boot, there is no WiFi, and
// nothing writes to flash at runtime. The only producer stall left is a cache
// miss inside audioUpdate(), measured at ~110 cycles per frame for four PSRAM
// voices (docs/bancada.md section 9). M16's own comment names this exact case
// as the reason the values are overridable ("percussive/Beat Machine style").
//
// 4 x 64 = 256 frames = 5.8ms. Validated on hardware 13/08/2026: clean on a
// fast roll with all four sampler voices sounding at once, the sampler's
// heaviest audioUpdate() path. See docs/bancada.md, "Fase E".
//
// That was measured against an M16 checkout that no longer exists, from
// before the transport changed; the ring is now drained in 32-frame bursts
// rather than one frame per driver call. Re-confirmed on the current pin on
// 11/10/2026 by the agbau_storage voice ladder: 1 to 64 voices, the longest
// gap between renders 1.4ms against this 5.8ms ring, and no click on a pure
// sine at any step (docs/bancada.md section 13). That was the Agbau's engine
// rather than the sampler's, but the same ring, block size and transport,
// under a far heavier load than four voices.
//
// Getting the first answer required removing two confounds that both sound
// exactly like an underrun, and which made 5.8ms appear to fail:
//   - 8-bit source samples. A ~48dB quantisation floor crackles on decay tails.
//   - Clipping. audioUpdate() sums voices and clips once at the end, so two
//     normalised samples (89% of full scale) sum to 178% and clip hard.
// The ladder only became meaningful with 16-bit/44.1kHz samples attenuated to
// ~22% each, where four voices sum to 89% and nothing clips.
//
// Tuning ladder, if it ever needs walking again:
//   clicks/dropouts  -> raise DMA_BUFFER_LENGTH (128 = 11.6ms, 256 = 23ms)
//   clean, want less -> 4 x 32 = 2.9ms, then 4 x 16 = 1.5ms
// Listen on a pure tone, or on samples that neither clip nor carry their own
// noise floor - otherwise the ladder measures those.
//
// #ifndef-guarded like M16's own, so a bench env can walk the ladder from
// build_flags without editing this file.
#ifndef DMA_BUFFERS
  #define DMA_BUFFERS       4
#endif
#ifndef DMA_BUFFER_LENGTH
  #define DMA_BUFFER_LENGTH 64
#endif

// The bound only has to be well below M16's default of 4 x 512 = 2048 frames.
// 512 leaves room for the two rungs above the validated 256 without being
// anywhere near the default. It was briefly raised to 1024 on the bench of
// 13/08/2026 to try 4 x 256, back when clipping was being mistaken for an
// underrun - that reason did not survive. With the order now enforced above,
// what this still catches is an override that asks for that much latency.
static_assert(DMA_BUFFERS * DMA_BUFFER_LENGTH <= 512,
              "I2S ring deeper than 512 frames (~12ms) - more latency than any "
              "of these firmwares was built for; see the tuning ladder");

// Rendering one block must not take a meaningful bite out of the ring that is
// draining while we render it. A quarter of the ring is the bound: it leaves
// three block periods of slack for the worst-case render, which at four
// sampler voices measured ~3,500 cycles against a 174,000-cycle block period.
static_assert(AUDIO_BLOCK_FRAMES * 4 <= DMA_BUFFERS * DMA_BUFFER_LENGTH,
              "audio block is more than a quarter of the DMA ring - either "
              "shrink AUDIO_BLOCK_FRAMES or deepen the ring");
