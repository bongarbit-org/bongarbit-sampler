/*
 * bongarbit-sampler
 *
 * One-shot sample triggers on GPIO4/5/6, plus a held-button pitch shifter on
 * GPIO7, out to a PCM5102A I2S DAC via the M16 library. Pressing a sample
 * button again while its sample is playing restarts it from the beginning.
 *
 * GPIO7 used to be a fourth pad. It now shifts the whole mix down an octave
 * for as long as it is held and triggers nothing - the effect replaced the
 * pad. It is also the first processor in a chain meant to grow: the audio
 * engine renders blocks of normalised float (src/audio_block.h) specifically
 * so that a live audio-in stream can be read in at the top of audioUpdate()
 * later without the DSP below it changing.
 *
 * Hardware: ESP32-S3 N16R8 + PCM5102A (I2S) + microSD (SPI, optional - see
 * USE_SD_SAMPLER below). Full wiring plan: docs/circuit.md
 *
 * Two sample sources, selected at compile time. Both play through the same
 * Samp engine (M16's wavetable-style sample player) - the only difference is
 * where the raw audio buffers come from:
 *  - USE_SD_SAMPLER=1: at boot, mounts the SD card and loads whole .wav
 *    files into RAM (M16's Wav::load() tries PSRAM first, so on this N16R8
 *    board samples land in the 8MB PSRAM). SD is only touched once at
 *    startup, not during playback - the card can be removed after boot.
 *    GPIO4 plays FILE_INDEX (whichever file the card lists first); GPIO5/6
 *    play 1.wav/2.wav by name. 3.wav is no longer loaded - GPIO7 is the
 *    effect pin now.
 *  - USE_SD_SAMPLER=0: GPIO4 plays a sample embedded directly in flash
 *    (src/kick_normalizado_data.h, generated from src/kick_normalizado.wav via
 *    tools/wav_to_header.py) - no SD card needed at all. The GPIO5/6 pads are
 *    not available in this mode; GPIO7's pitch shifter still is, because it
 *    processes the mix rather than a sample of its own.
 */

// --- The audio engine: block-based, float (see src/audio_block.h) ---
// audioUpdate() renders AUDIO_BLOCK_FRAMES frames per call into a normalised
// float block, instead of returning one frame at a time. The three reasons -
// driver calls, PSRAM locality, and the fact that DSP with memory needs a
// window - are documented in that header. The third is the one that forced it.
//
// It also brings the I2S DMA ring, 4 x 64 = 5.8ms instead of M16's 46ms
// (src/engine_config.h: why that depth, how it was validated, the tuning
// ladder). M16 reads the ring with #ifndef, so this include has to stay ABOVE
// M16.h; the build fails if it drifts below.
#include "audio_block.h"

#include "M16.h"
#include "Samp.h"

// --- Sample source: 1 = load one .wav from SD into RAM/PSRAM at boot,
//                    0 = play from embedded flash array ---
#define USE_SD_SAMPLER 1

#if USE_SD_SAMPLER
  #include "Wav.h"
#else
  #include "kick_normalizado_data.h"
#endif

// --- Debug logging over Serial ---
// Split in two, because the two kinds of logging have very different costs.
//
// DEBUG_BOOT covers setup() only - SD mount, WAV load, DAC start. It runs
// before any audio and costs nothing at runtime. It is also what tells you
// PSRAM and the card actually came up, so leave it on.
//
// DEBUG_TRIGGERS prints one line per strike, from inside the polling loop.
// Serial over USB CDC blocks when the host is not draining the buffer, which
// puts an unbounded stall directly in the trigger path. Off by default: turn
// it on to diagnose an input problem, never while judging latency.
// The two diagnostics are #ifndef-guarded, same convention as every other knob
// in this file, so they can be flipped for one build without editing (and then
// forgetting to revert) a tracked file:
//
//   PLATFORMIO_BUILD_FLAGS='-DDEBUG_ENGINE=1' pio run -e esp32-s3-devkitc-1 -t upload
#define DEBUG_BOOT     1
#ifndef DEBUG_TRIGGERS
  #define DEBUG_TRIGGERS 0
#endif

// DEBUG_ENGINE prints one line per second from loop() with the state of the
// whole audio path: blocks rendered, peak sample produced, which voices think
// they are playing, and the raw level of every input pin.
//
// It exists because "no sound" has at least four distinct causes in this
// firmware and they are indistinguishable by ear: the audio task not running,
// the voices never starting, the mix coming out as zeroes, or the DAC not
// converting. One line separates all four. It costs a blocking USB CDC write
// in loop() once a second, so it does belong off while judging latency - but
// unlike DEBUG_TRIGGERS it is not in the per-strike path.
//
// It earned its place on 11/10/2026. The instrument went silent right after
// three commits that rewrote the whole audio engine, which made the firmware
// the obvious suspect. One line of this ruled it out in a single capture:
//
//   ENG blocks/s 1378  peak 5327  playing[trig 1 p1 0 p2 0]  pins[...]  fx 1
//
// 1378 blocks/s is exactly 44100/32, so the I2S clock - not our code - was
// setting the pace, and the driver was consuming every block (a timing-out
// write would have shown ~10/s). A non-zero peak meant real signal reached the
// last instruction before the DMA. The fault was a connection, the same class
// of fault that had already cost this project a full bench session while a
// dead GPIO was blamed instead.
#ifndef DEBUG_ENGINE
  #define DEBUG_ENGINE 0
#endif
#if DEBUG_BOOT
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
#endif
#if DEBUG_TRIGGERS
  #define TRIGDBG(...) Serial.printf(__VA_ARGS__)
#else
  #define TRIGDBG(...)
#endif

// --- Trigger conditioning, shared by all four buttons ---
// The state machine and the rationale for both its windows live in
// trigger_input.h, which is compiled into this firmware AND into the host test
// suite. Keeping one definition is what makes test/test_trigger able to make
// claims about this binary rather than about a transcription of it.
//
// To change TRIG_CONFIRM_US or TRIG_RELEASE_MS, define them before this
// include - the header guards its defaults with #ifndef, same as the DMA
// ring in engine_config.h.
#include "trigger_input.h"

// --- Mixer gain staging and output limiting ---
// Two independent switches, both on, both reversible, because they change how
// the instrument sounds and that is not a decision to make silently.
//
// MIXER_GAIN_STAGING gives every voice its own level plus a master level - the
// MPC model. It defaults to unity everywhere, so turning it on changes nothing
// audible until a kit is actually dialled in; it exists so that the control is
// there at all. Deliberately NOT a divide-by-voice-count, which would cost
// 12dB on one voice to protect a worst case that rarely happens.
//
// MIXER_SOFT_CLIP replaces the hard clamp with a knee that is the identity
// below 75% of full scale and asymptotic to full scale above it. This is the
// part that removes the clicks: with a normalised kit, two voices sum past
// full scale and hard-clamping a transient is what produced them.
//
// Set MIXER_SOFT_CLIP to 0 to get the hard clamp back for comparison. That
// needs no replacement curve: audioToI16() clamps on the way out regardless,
// because an out-of-range float cannot be converted to int safely, so turning
// the knee off leaves exactly the clamp that was there before it existed.
#define MIXER_GAIN_STAGING 1
#define MIXER_SOFT_CLIP    1

#if MIXER_GAIN_STAGING
  #define MIXGAIN(x, g) ((x) * (g))
#else
  #define MIXGAIN(x, g) (x)
#endif

#if MIXER_SOFT_CLIP
  #define MIXCLIP(x) audioSoftClip(x)
#else
  #define MIXCLIP(x) (x)
#endif

#if MIXER_GAIN_STAGING
// Per-voice and master levels, now plain float with 1.0 = unity (they were Q8
// fixed point before the engine moved to float). These are the knobs a kit is
// balanced with: lower a loud sample here rather than re-normalising the .wav.
// Unity by default - the headroom convention is that kit material carries
// ~6dB of its own, same as on an MPC.
float trigGain   = 1.0f;
float masterGain = 1.0f;
#endif

#if USE_SD_SAMPLER
// --- Which WAV file to load (0 = first file found on the card) ---
#define FILE_INDEX 0

// --- microSD SPI pins (see docs/circuit.md) ---
#define SD_CS   10
#define SD_SCLK 12
#define SD_MISO 13
#define SD_MOSI 11

SdFs sd;
Wav  wav;

// --- M16's edge fades (docs/bancada.md, section 12) ---
// Wav::load() ends by multiplying the first and last 10 ms of every buffer by
// a cosine ramp, unconditionally (M16 aef89b2, Wav.h:199, ramp at 209-249).
// M16 means it to remove clicks at loop points and file edges. This sampler
// never loops, and at the start of a percussion hit the "click" is the
// attack: the ramp starts every sample from exactly zero and is still at -6dB
// at 5 ms. The repo's own kick peaks at 6 ms, inside it.
//
// 1 = undo the ramp: re-read the faded edges from the card with readFrames().
// It converts to int16 with the same arithmetic as load() at this pin, so the
// buffer comes out as if the ramp had never run - and identical to what
// tools/wav_to_header.py embeds in flash, where there is no ramp at all.
// 0 = M16's behaviour, untouched. It stays the default until the two have
// been compared by ear: whether the ramp is audible on this material is not
// known, only that it is there. Flip it for one build without editing:
//
//   PLATFORMIO_BUILD_FLAGS='-DWAV_RESTORE_EDGES=1' pio run -e esp32-s3-devkitc-1 -t upload
#ifndef WAV_RESTORE_EDGES
  #define WAV_RESTORE_EDGES 0
#endif

static void restoreEdges(Wav& w) {
#if WAV_RESTORE_EDGES
  const unsigned long total = w.getFrameCount();
  // 20 ms, twice M16's ramp. Re-reading frames the ramp never touched is
  // harmless - they come back identical - while re-reading too few would leave
  // the end of the ramp in place with no symptom at all. The margin absorbs a
  // modest change on an M16 bump; it does not replace re-reading Wav.h then.
  unsigned long n = w.getSampleRate() / 50;
  if (n > total) n = total;
  if (n == 0) return;
  bool ok = w.readFrames(0, n, w.getBuffer());
  // A truncated load keeps its fade-out. That end is M16's cut, not the
  // file's, and a cut mid-signal is the click the ramp exists to prevent.
  const bool whole = (total == w.getFileFrameCount());
  if (whole) {
    ok = w.readFrames(total - n, n, w.getBuffer() + (total - n) * w.getChannels()) && ok;
  }
  // A failed read leaves those frames as load() faded them - the M16 sound,
  // never garbage, since readFrames() only writes what it actually read. It
  // reports only whether it read anything, though, so a short read passes as
  // success with part of the ramp still in place. Unlikely for frames load()
  // has just read; not detectable from here.
  if (!ok) {
    Serial.printf("WAV: '%s' edge re-read FAILED - plays with M16's fades\n",
                  w.getFilename());
    return;
  }
  DBG("WAV: '%s' fades undone (%lu frames re-read at %s)\n",
      w.getFilename(), n, whole ? "each end" : "the start; truncated, fade-out kept");
#else
  (void)w;
#endif
}

// --- Extra pads: 1.wav / 2.wav on the SD card root ---
// Two, not three: GPIO7 stopped being a pad and became the pitch shifter's
// hold control (see FX_PIN below). /3.wav is no longer loaded, and the card
// does not need to carry it.
#define PAD_COUNT 2
const uint8_t   padPins[PAD_COUNT]  = {5, 6};
const char*     padFiles[PAD_COUNT] = {"/1.wav", "/2.wav"};
Wav             padWav[PAD_COUNT];
Samp            padSamp[PAD_COUNT];
uint8_t         padChannels[PAD_COUNT] = {1, 1};
TriggerInput    padInput[PAD_COUNT];
#if MIXER_GAIN_STAGING
// Per-pad level, 1.0 = unity. Balance the kit here.
float           padGain[PAD_COUNT] = {1.0f, 1.0f};
#endif

// Wav::load() returns true even when it did not load the whole file. With
// PSRAM present it caps each buffer at 90% of the PSRAM still free - 75% if
// the allocation is then refused - and cuts the file to fit. M16 says so once,
// in the middle of its own load output, and getFrameCount() becomes the cut
// length, which the DBG "loaded" line then reports as if it were the file. A
// sample that stops short reads as a bad file instead of a full memory. Loads
// run in order, so it is the last files loaded that get cut.
// Warning only: what plays is exactly what was loaded, unchanged.
static void warnIfTruncated(const Wav& w) {
  if (w.getFrameCount() < w.getFileFrameCount()) {
    Serial.printf("WAV: '%s' TRUNCATED to %lu of %lu frames - out of PSRAM, "
                  "the tail will not play\n",
                  w.getFilename(), w.getFrameCount(), w.getFileFrameCount());
  }
}
#endif

// --- Latency probe (see docs/circuit-rc.md, "Medir de verdade") ---
// Measures real button-to-sound by watching the DAC's own analog output, which
// is the only way to capture the part that happens after the CPU writes: the
// DMA ring draining and the DAC's digital filter. A GPIO marker cannot see it.
//
// Needs a biased divider feeding an ADC pin - two 10k from 3V3/GND to make a
// 1.65V midpoint, DAC output AC-coupled in through 100nF. Silence then sits
// mid-scale and the signal swings around it without ever going negative, which
// the ADC could not survive.
//
// Off by default: it puts a blocking analogRead() in loop() and is a bench
// instrument, not a runtime feature.
#define LATENCY_PROBE 0

#if LATENCY_PROBE
  #define PROBE_ADC_PIN     1       // GPIO1 = ADC1_CH0, free in this design
  #define PROBE_TIMEOUT_US  50000
  #define PROBE_BASELINE_N  256

  uint16_t    probeBaseline  = 0;
  uint16_t    probeThreshold = 0;
  bool        probeArmed     = false;
  uint32_t    probeEdgeUs    = 0;   // raw pin went LOW
  uint32_t    probeFireUs    = 0;   // samp.start() was called
  const char* probeSource    = "";

  // Sample the idle DAC output to learn where silence sits and how noisy it is,
  // so the detection threshold is derived from the actual rig rather than
  // guessed. Must run after audioStart(), with nothing playing.
  void probeCalibrate() {
    uint32_t sum = 0, lo = 4095, hi = 0;
    for (int i = 0; i < PROBE_BASELINE_N; i++) {
      uint16_t v = analogRead(PROBE_ADC_PIN);
      sum += v;
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    probeBaseline  = sum / PROBE_BASELINE_N;
    uint16_t noise = hi - lo;
    probeThreshold = noise * 4 + 40;  // 4x the observed idle spread, plus floor
    Serial.printf("PROBE: baseline=%u noise=%u threshold=%u (GPIO%d)\n",
                  probeBaseline, noise, probeThreshold, PROBE_ADC_PIN);
    if (noise > 200)
      Serial.println("PROBE: idle noise is high - check the divider and grounding");
  }

  void probeArm(const char* source, uint32_t edgeUs, uint32_t fireUs) {
    if (probeArmed) return;         // a measurement is already in flight
    probeArmed  = true;
    probeSource = source;
    probeEdgeUs = edgeUs;
    probeFireUs = fireUs;
  }

  // Poll the DAC output until it leaves the silence band. Reports both halves
  // of the chain: what the firmware spent deciding, and what the audio path
  // added after start() - the second is the number the DMA ring controls.
  void probePoll() {
    if (!probeArmed) return;
    uint32_t now = micros();
    int      v   = analogRead(PROBE_ADC_PIN);
    if (abs(v - (int)probeBaseline) > (int)probeThreshold) {
      Serial.printf("PROBE %s: total %lu us  (trigger %lu + audio path %lu)\n",
                    probeSource,
                    (unsigned long)(now - probeEdgeUs),
                    (unsigned long)(probeFireUs - probeEdgeUs),
                    (unsigned long)(now - probeFireUs));
      probeArmed = false;
    } else if ((uint32_t)(now - probeFireUs) > PROBE_TIMEOUT_US) {
      Serial.printf("PROBE %s: no audio within %d us - sample silent at its "
                    "start, or the divider is not connected\n",
                    probeSource, PROBE_TIMEOUT_US);
      probeArmed = false;
    }
  }
#endif

// --- Trigger button (see docs/circuit.md) ---
// One leg on TRIGGER_PIN (internal pull-up, pressed = LOW), the other leg
// on a real GND pin on the dev kit. Runs the same TriggerInput as the pads -
// it used to have its own wait-for-stable debounce costing 25ms, which was
// most of this button's trigger-to-sound time.
#define TRIGGER_PIN 4
TriggerInput trigInput;

// --- GPIO7: hold to pitch-shift (see src/pitch_shift.h) ---
// This pin used to trigger /3.wav. It now runs the pitch shifter on the whole
// mix for as long as it is held, and triggers nothing - the effect replaced
// the pad rather than being added next to it.
//
// A hold, not a strike, so it reads TriggerInput::held() instead of the fire
// edge from update(). Same state machine and the same two windows as every
// other button: confirm for 200us before engaging, require 3ms of continuous
// HIGH before believing the release. A second debounce for the hold case is
// exactly what trigger_input.h exists to prevent.
//
// The shift itself: negative is down. -12 is an octave down, which on
// percussion is the obviously useful direction and the easiest to confirm by
// ear. Change the one number; pitchRatioFromSemitones() handles the rest, and
// the ratio can be moved at runtime without a discontinuity if this ever grows
// a knob.
#define FX_PIN 7
#ifndef PITCH_SHIFT_SEMITONES
  #define PITCH_SHIFT_SEMITONES (-12.0f)
#endif
#include "pitch_shift.h"
TriggerInput fxInput;
PitchShift   pitchShift;

Samp    samp;
uint8_t sampleChannels = 1; // set at boot from whichever source is used below

void setup() {
  Serial.begin(115200);
  delay(500);

  // PSRAM is the single most important thing this log can confirm, because
  // losing it does not stop the sampler - it shrinks it, quietly.
  //
  // This comment used to say the opposite: that M16's psramAllocSafe() has no
  // internal-RAM fallback, so without PSRAM every Wav::load() below fails and
  // the only symptom is "WAV: failed to load". The premise is true of
  // psramAllocSafe() itself; the conclusion was wrong, because Wav::load()
  // does not stop there. When PSRAM is missing, or refuses the allocation, it
  // falls back to a plain malloc() from internal RAM and prints "Wav:
  // Allocated N bytes in RAM". Recorded rather than deleted because the wrong
  // version predicted a loud failure, and the real behaviour is the opposite.
  //
  // What actually happens without PSRAM: samples load into internal RAM until
  // it runs out - a few hundred KB in total on the S3, less in one piece. A
  // kit of short hits can fit entirely and nothing sounds wrong at all; a
  // bigger one loads what fits and fails the rest. GPIO4's file loads first,
  // and if it fails boot halts at "WAV: failed to load"; a pad that fails
  // stays silent while the rest plays. Either way a half-working sampler
  // sends you debugging SD files or wiring for what is a board/build
  // configuration problem. So report it before anything can depend on it,
  // with the internal-RAM budget left (the real figures, rather than the
  // estimate above), and shout when it is missing - same convention as the
  // SD failure below: errors are unconditional, successes go through DBG.
  size_t psramTotal = ESP.getPsramSize();
  if (psramTotal == 0) {
    Serial.printf("PSRAM: NOT AVAILABLE - samples fall back to internal RAM "
                  "(%u bytes free, largest block %u): small ones load, bigger "
                  "ones fail. This is a board/build configuration problem, not "
                  "wiring: check that platformio.ini still has "
                  "board = esp32-s3-devkitc1-n16r8\n",
                  (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  } else {
    DBG("PSRAM: %u bytes free of %u\n",
        (unsigned)ESP.getFreePsram(), (unsigned)psramTotal);
  }

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  pinMode(FX_PIN, INPUT_PULLUP);

  // Must happen before audioStart(), so the audio task never reads
  // uninitialised history. Its diagnostic line is further down, with the rest
  // of the audio summary.
  pitchShift.init();
  pitchShift.setRatio(pitchRatioFromSemitones(PITCH_SHIFT_SEMITONES));

#if USE_SD_SAMPLER
  // initSD() already lists the .wav files it finds on the card (Serial).
  if (!wav.initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, 16)) {
    Serial.print("SD: init FAILED - ");
    sd.printSdError(&Serial); // prints the specific SdFat error (e.g. CMD0 = card not responding at all)
    Serial.println("Check wiring (docs/circuit.md)");
    while (1) delay(1000);
  }
  DBG("SD: init OK\n");

  // Loads the whole file into RAM (PSRAM preferred, tried first by the
  // library) - a one-time blocking read at boot, not during playback.
  if (!wav.loadNumber(FILE_INDEX)) {
    Serial.println("WAV: failed to load file at index " + String(FILE_INDEX));
    while (1) delay(1000);
  }
  warnIfTruncated(wav);

  sampleChannels = wav.getChannels();
  DBG("WAV: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
      wav.getFilename(), sampleChannels, (unsigned long)wav.getSampleRate(),
      wav.getFrameCount(),
      (unsigned long)(1000ULL * wav.getFrameCount() / wav.getSampleRate()));
  restoreEdges(wav);

  // Samp::setTable() computes the buffer-rate -> output-rate ratio itself,
  // so the sample plays at its original speed regardless of the file's
  // native sample rate - no need to reconfigure the I2S clock here.
  samp.setTable(wav.getBuffer(), wav.getFrameCount(), wav.getSampleRate(), sampleChannels);

  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    pinMode(padPins[i], INPUT_PULLUP);

    if (!padWav[i].initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, 16) ||
        !padWav[i].load(padFiles[i])) {
      Serial.printf("Pad %u: failed to load %s\n", i + 1, padFiles[i]);
      continue;
    }
    warnIfTruncated(padWav[i]);
    padChannels[i] = padWav[i].getChannels();
    padSamp[i].setTable(padWav[i].getBuffer(), padWav[i].getFrameCount(),
                         padWav[i].getSampleRate(), padChannels[i]);
    DBG("Pad %u: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
        i + 1, padWav[i].getFilename(), padChannels[i],
        (unsigned long)padWav[i].getSampleRate(), padWav[i].getFrameCount(),
        (unsigned long)(1000ULL * padWav[i].getFrameCount() / padWav[i].getSampleRate()));
    restoreEdges(padWav[i]); // setTable() keeps the pointer, so order is free
  }
#else
  sampleChannels = KICK_NORMALIZADO_CHANNELS;
  // KICK_NORMALIZADO_DATA is const (lives in flash/.rodata) - setTable() only
  // reads from it during playback, so the cast away from const is safe.
  samp.setTable(const_cast<int16_t*>(KICK_NORMALIZADO_DATA), KICK_NORMALIZADO_FRAME_COUNT,
                KICK_NORMALIZADO_SAMPLE_RATE, sampleChannels);
  DBG("Flash sample: %lu frames, %u ch, %u Hz (~%lu ms)\n",
      KICK_NORMALIZADO_FRAME_COUNT, sampleChannels, KICK_NORMALIZADO_SAMPLE_RATE,
      (unsigned long)(1000ULL * KICK_NORMALIZADO_FRAME_COUNT / KICK_NORMALIZADO_SAMPLE_RATE));
#endif

  // BCK/WS/DOUT = 16/17/18, which is M16's own default set. Stated explicitly
  // anyway, for the din argument below.
  //
  // This comment used to claim GPIO18 "does not drive" and that DOUT had to be
  // moved to GPIO8, on the evidence of 0V measured at the DAC's DIN. That was
  // wrong: the 0V was an intermittent breadboard contact, and the pad has
  // worked on 18 ever since the connections were soldered directly. Recorded
  // rather than deleted because the measurement really was 0V - a dead pin and
  // a bad contact look identical from one multimeter reading, and this cost a
  // bench session. See docs/bancada.md.
  //
  // din = -1 also disables the I2S RX channel. M16's default set is
  // {16, 17, 18, 21}, and any din other than -1 makes audioStart() allocate an
  // RX channel and re-run i2s_channel_init_std_mode() with the same std_cfg.
  // This project has no I2S input (docs/circuit.md lists GPIO21 as unused).
  seti2sPins(16, 17, 18, -1);

  // One audio task, stated rather than inherited.
  //
  // This is M16's current default on ESP32, but it is a default, and it is the
  // one default whose flipping would break this firmware in two ways at once:
  // audioUpdate() would run on both cores against a single static block
  // buffer, and M16's block-split finaliser interleaves two per-core
  // partitions that this engine does not produce. Dual-core rendering here
  // would mean partitioning voices with audioPartitionOffset()/Stride(), which
  // is a real option for later - four voices is not why this board is busy -
  // but it is a design, not a flag.
  setIsDualCore(false);

  audioStart();
  DBG("DAC: I2S started - BCK=GPIO16 WS=GPIO17 DOUT=GPIO18, RX disabled\n");
  DBG("DAC: PCM5102A has no status feedback pin - the 'M16 is running' line "
      "above is the only software confirmation; audible output still "
      "depends on correct wiring (docs/circuit.md)\n");
  DBG("ENGINE: %d-frame float blocks (%.2f ms), ring %d x %d (%.2f ms)\n",
      AUDIO_BLOCK_FRAMES, (double)(1000.0f * AUDIO_BLOCK_FRAMES / SAMPLE_RATE),
      DMA_BUFFERS, DMA_BUFFER_LENGTH,
      (double)(1000.0f * DMA_BUFFERS * DMA_BUFFER_LENGTH / SAMPLE_RATE));
  DBG("FX: hold GPIO%d to pitch shift %+.1f semitones (ratio %.4f); adds "
      "%.1f ms when engaged, bit-exact bypass when not\n",
      FX_PIN, (double)PITCH_SHIFT_SEMITONES, (double)pitchShift.getRatio(),
      (double)(1000.0f * PitchShift::latencyFrames() / SAMPLE_RATE));
#if LATENCY_PROBE
  probeCalibrate(); // after audioStart(), while nothing is playing
#endif
#if USE_SD_SAMPLER
  Serial.printf("Ready. GPIO%d/%d/%d trigger samples; hold GPIO%d to pitch "
                "shift the mix.\n", TRIGGER_PIN, padPins[0], padPins[1], FX_PIN);
#else
  Serial.printf("Ready. GPIO%d triggers the flash sample; hold GPIO%d to pitch "
                "shift the mix.\n", TRIGGER_PIN, FX_PIN);
#endif
}

#if DEBUG_ENGINE
// Written by the audio task, read by loop(). Plain counters rather than
// anything synchronised: a torn read costs one wrong diagnostic line, and
// adding a lock to the audio path to protect a debug print would be a worse
// bug than the one being diagnosed.
volatile uint32_t engineBlocks = 0;
volatile int32_t  enginePeak   = 0;   // |sample| as written to the DAC
#endif

void loop() {
  // One timestamp for the whole sweep, so every input is conditioned against
  // the same instant. Nothing blocking belongs in this function: with
  // DEBUG_TRIGGERS off it is four digitalRead()s and some arithmetic, which
  // keeps the poll period in the low tens of microseconds.
  uint32_t now_us = micros();

  if (trigInput.update(now_us, digitalRead(TRIGGER_PIN) == LOW)) {
    TRIGDBG("Trigger: GPIO%d\n", TRIGGER_PIN);
    samp.start();
#if LATENCY_PROBE
    probeArm("trigger", trigInput.pendingSince, micros());
#endif
  }

#if USE_SD_SAMPLER
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    if (padInput[i].update(now_us, digitalRead(padPins[i]) == LOW)) {
      TRIGDBG("Pad %u: triggered\n", i + 1);
      padSamp[i].start();
#if LATENCY_PROBE
      probeArm(padFiles[i], padInput[i].pendingSince, micros());
#endif
    }
  }
#endif

  // The effect pin runs the same machine as every other button, but its return
  // value is discarded: what matters here is held(), the level, not the strike
  // edge. update() still has to be called every poll - held() is derived from
  // the state update() maintains, so skipping a poll freezes the control.
  //
  // setEngaged() only records a request; the audio task picks it up at its
  // next block boundary. Nothing blocking or audio-rate happens in loop().
  fxInput.update(now_us, digitalRead(FX_PIN) == LOW);
  pitchShift.setEngaged(fxInput.held());

#if LATENCY_PROBE
  probePoll();
#endif

#if DEBUG_ENGINE
  // Once a second, the whole audio path in one line.
  //
  //   blocks/s ~1378 -> the audio task is running at the expected rate
  //            0     -> it is not running at all
  //   peak     0     -> the mix really is silence, so look upstream
  //            >0    -> the firmware IS producing signal, so look at the DAC
  //   playing  which voices believe they are sounding
  //   pins     raw levels; 0 = pressed (INPUT_PULLUP), 1 = released
  static uint32_t lastReport = 0;
  static uint32_t lastBlocks = 0;
  if ((uint32_t)(now_us - lastReport) >= 1000000UL) {
    uint32_t blocks = engineBlocks;
    int32_t  peak   = enginePeak;
    enginePeak = 0;
    Serial.printf("ENG blocks/s %lu  peak %ld  playing[trig %d",
                  (unsigned long)(blocks - lastBlocks), (long)peak,
                  samp.isPlaying() ? 1 : 0);
#if USE_SD_SAMPLER
    for (uint8_t i = 0; i < PAD_COUNT; i++)
      Serial.printf(" p%u %d", i + 1, padSamp[i].isPlaying() ? 1 : 0);
#endif
    Serial.printf("]  pins[4:%d 5:%d 6:%d 7:%d]  fx %d\n",
                  digitalRead(4), digitalRead(5), digitalRead(6), digitalRead(7),
                  pitchShift.isEngaged() ? 1 : 0);
    lastReport = now_us;
    lastBlocks = blocks;
  }
#endif
}

// Add one voice's next AUDIO_BLOCK_FRAMES frames into the block.
//
// Voice-outer, frame-inner, and that order is the point: each voice walks 32
// consecutive frames of its own PSRAM buffer before the next voice starts.
// The old frame-outer loop interleaved four PSRAM streams and bounced the
// cache between them on every single frame.
//
// Skipping a silent voice saves the whole inner loop, which is most of them
// most of the time. Samp::next() already returns 0 when stopped, so the check
// is an optimisation and not a behaviour change - and the race it has with
// start() in loop() can only cost one block, which is a granularity the
// trigger path already has.
static inline void addVoice(AudioBlock& b, Samp& voice, uint8_t channels, float gain) {
  if (!voice.isPlaying()) return;
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    int16_t l, r;
    if (channels == 2) {
      voice.nextStereo(l, r);
    } else {
      l = r = voice.next(); // nextStereo() only works for stereo buffers (num_channels==2)
    }
    b.l[n] += MIXGAIN(audioFromI16(l), gain);
    b.r[n] += MIXGAIN(audioFromI16(r), gain);
  }
}

void audioUpdate() {
  // One block, reused. Static rather than stack-local so there is exactly one,
  // which is only safe because there is exactly one audio task - see the
  // setIsDualCore(false) call in setup(). If that ever becomes two, this
  // buffer is the first thing that breaks, silently.
  static AudioBlock blk;

  audioBlockSilence(blk);

  // Voices accumulate. Pads play concurrently with the main button and with
  // each other, so this sums them all rather than outputting the last one
  // triggered.
  addVoice(blk, samp, sampleChannels, trigGain);
#if USE_SD_SAMPLER
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    addVoice(blk, padSamp[i], padChannels[i], padGain[i]);
  }
#endif

  // --- Processing chain --------------------------------------------------
  // Every processor has the shape process(AudioBlock&): it sees a whole block
  // of normalised float and may keep history across calls. This is the window
  // the block engine exists to provide, and the slot a live audio-in stream
  // will eventually be read into at the top of rather than bolted onto.
  //
  // The shifter runs unconditionally, and that is on purpose: it needs to keep
  // writing its delay line even while disengaged, or pressing the button would
  // start the effect from silence. While bypassed it costs 32 stores per
  // channel and returns the block bit-exact - pinned by
  // test_bypass_is_bit_exact, which is what makes leaving it here free.
  pitchShift.process(blk);

  // Master level, limiter, and the one conversion back to the wire's units.
  //
  // audioBlockWrite(), not the older i2s_write_samples(): that is now a
  // [[deprecated]] shim over it which also returns false without writing
  // anything if M16's dual-core block-split mode is ever active. It clips to
  // 16 bits itself, which is a no-op after audioToI16().
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    int16_t outL = audioToI16(MIXCLIP(MIXGAIN(blk.l[n], masterGain)));
    int16_t outR = audioToI16(MIXCLIP(MIXGAIN(blk.r[n], masterGain)));
#if DEBUG_ENGINE
    int32_t mag = outL < 0 ? -(int32_t)outL : (int32_t)outL;
    if (mag > enginePeak) enginePeak = mag;
#endif
    audioBlockWrite(outL, outR);
  }

#if DEBUG_ENGINE
  engineBlocks++;
#endif
}
