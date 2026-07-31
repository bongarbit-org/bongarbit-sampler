/*
 * bongarbit-sampler
 *
 * One-shot sample triggers via 4 buttons (GPIO4/5/6/7), each playing a
 * sample out to a PCM5102A I2S DAC via the M16 library. Pressing a button
 * again while its sample is playing restarts it from the beginning.
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
 *    GPIO4 plays FILE_INDEX (whichever file the card lists first); GPIO5/6/7
 *    play 1.wav/2.wav/3.wav by name.
 *  - USE_SD_SAMPLER=0: GPIO4 plays a sample embedded directly in flash
 *    (src/kick_normalizado_data.h, generated from src/kick_normalizado.wav via
 *    tools/wav_to_header.py) - no SD card needed at all. GPIO5/6/7 pads are
 *    not available in this mode.
 */

// --- Audio output latency (MUST be set before including M16.h) ---
// M16 feeds the I2S DMA ring one sample at a time and blocks when it is full,
// so the ring depth IS the trigger-to-sound latency, essentially all of it.
// The library default is 4 x 512 = 2048 frames = 46ms at 44.1kHz, sized to
// survive a flash sector erase on the host (~30-40ms).
//
// That worst case does not exist in this firmware: samples are resident in
// PSRAM, the SD card is never touched after boot, there is no WiFi, and
// nothing writes to flash at runtime. The only producer stall left is a PSRAM
// cache miss inside audioUpdate(), which is sub-microsecond. M16's own comment
// names this exact case as the reason the values are overridable
// ("percussive/Beat Machine style").
//
// 4 x 64 = 256 frames = 5.8ms. Tuning ladder, to be walked on hardware:
//   clicks/dropouts  -> raise DMA_BUFFER_LENGTH (128 = 11.6ms, 256 = 23ms)
//   clean, want less -> 4 x 32 = 2.9ms, then 4 x 16 = 1.5ms
// Test on a fast roll with all four voices sounding at once - that is the
// heaviest audioUpdate() path and the first place an underrun will show.
#define DMA_BUFFERS       4
#define DMA_BUFFER_LENGTH 64

#include "M16.h"
#include "Samp.h"

// The override above only works while it sits BEFORE the include - M16 guards
// its own values with #ifndef. Moving it below, or dropping it, silently
// restores the 2048-frame default and puts 46ms back into the trigger path
// with no other symptom. Fail the build instead.
static_assert(DMA_BUFFERS * DMA_BUFFER_LENGTH <= 512,
              "I2S ring deeper than 512 frames (~12ms) - the DMA override has "
              "drifted below #include \"M16.h\" and M16's default came back");

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
#define DEBUG_BOOT     1
#define DEBUG_TRIGGERS 0
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
// GPIO4 and the three pads run this same state machine. The sharing is
// deliberate: the retrigger bug this replaced survived because the trigger
// path had drifted into two divergent debounce implementations, and only one
// of them ever got scrutinised.
//
// Both guards read the RAW pin. Nothing here derives a decision from a
// filtered or debounced state - the lag of such a state couples correctness to
// the filter's constant, which is precisely how the old bug worked. Measured
// in sim/trigger_sim.cpp.
//
//  TRIG_CONFIRM_US: the LOW must persist this long before firing. Rejects
//    glitches shorter than the window, and is the ONLY defense against phantom
//    hits - the RC filter in docs/circuit-rc.md does not cover them. Keep it
//    under one contact-bounce segment (~300us): above that it cannot be
//    satisfied until the whole bounce settles and the latency becomes the
//    bounce duration instead. 200us costs 0.2ms; 1ms costs 3ms.
//  TRIG_RELEASE_MS: rearm only after the raw pin reads HIGH continuously.
//    Guards re-fire during contact bounce. With the RC filter fitted, 1 is
//    enough, and lowering it raises the maximum sustainable roll rate.
#define TRIG_CONFIRM_US 200
#define TRIG_RELEASE_MS 3

struct TriggerInput {
  bool     armed        = true;
  bool     pending      = false;
  // pendingSince is deliberately NOT cleared when a strike fires: it then holds
  // the instant the raw pin first went LOW, which is what LATENCY_PROBE needs
  // to measure true button-to-sound rather than just start()-to-sound.
  uint32_t pendingSince = 0;      // micros() stamps; every comparison below is
  bool     highValid    = false;  // an unsigned difference, so the ~71 minute
  uint32_t highSince    = 0;      // micros() wraparound is a non-event

  // True on the single loop() iteration where a strike is confirmed.
  bool update(uint32_t now_us, bool low) {
    if (armed) {
      if (!low) {
        pending = false;            // bounced back HIGH: a glitch, not a strike
      } else if (!pending) {
        pending = true; pendingSince = now_us;
      } else if ((uint32_t)(now_us - pendingSince) >= TRIG_CONFIRM_US) {
        armed = false; pending = false; highValid = false;
        return true;
      }
    } else if (low) {
      highValid = false;            // still held or bouncing: restart the window
    } else if (!highValid) {
      highValid = true; highSince = now_us;
    } else if ((uint32_t)(now_us - highSince) >= TRIG_RELEASE_MS * 1000UL) {
      armed = true;
    }
    return false;
  }
};

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

// --- Extra pads: 1.wav / 2.wav / 3.wav on the SD card root ---
#define PAD_COUNT 3
const uint8_t   padPins[PAD_COUNT]  = {5, 6, 7};
const char*     padFiles[PAD_COUNT] = {"/1.wav", "/2.wav", "/3.wav"};
Wav             padWav[PAD_COUNT];
Samp            padSamp[PAD_COUNT];
uint8_t         padChannels[PAD_COUNT] = {1, 1, 1};
TriggerInput    padInput[PAD_COUNT];
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

Samp    samp;
uint8_t sampleChannels = 1; // set at boot from whichever source is used below

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);

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

  sampleChannels = wav.getChannels();
  DBG("WAV: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
      wav.getFilename(), sampleChannels, (unsigned long)wav.getSampleRate(),
      wav.getFrameCount(),
      (unsigned long)(1000ULL * wav.getFrameCount() / wav.getSampleRate()));

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
    padChannels[i] = padWav[i].getChannels();
    padSamp[i].setTable(padWav[i].getBuffer(), padWav[i].getFrameCount(),
                         padWav[i].getSampleRate(), padChannels[i]);
    DBG("Pad %u: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
        i + 1, padWav[i].getFilename(), padChannels[i],
        (unsigned long)padWav[i].getSampleRate(), padWav[i].getFrameCount(),
        (unsigned long)(1000ULL * padWav[i].getFrameCount() / padWav[i].getSampleRate()));
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

  audioStart();
  DBG("DAC: I2S started - BCK=GPIO16 WS=GPIO17 DOUT=GPIO18 (M16 defaults)\n");
  DBG("DAC: PCM5102A has no status feedback pin - the 'M16 is running' line "
      "above is the only software confirmation; audible output still "
      "depends on correct wiring (docs/circuit.md)\n");
#if LATENCY_PROBE
  probeCalibrate(); // after audioStart(), while nothing is playing
#endif
  Serial.println("Ready. Press the trigger button to play the sample.");
}

void loop() {
  // One timestamp for the whole sweep, so all four inputs are conditioned
  // against the same instant. Nothing blocking belongs in this function: with
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

#if LATENCY_PROBE
  probePoll();
#endif
}

void audioUpdate() {
  int16_t L, R;
  if (sampleChannels == 2) {
    samp.nextStereo(L, R);
  } else {
    L = R = samp.next(); // nextStereo() only works for stereo buffers (num_channels==2)
  }
  int32_t mixL = L, mixR = R;

#if USE_SD_SAMPLER
  // Pads can play concurrently with the main button and with each other -
  // sum all voices and clip, instead of just outputting the last one.
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    int16_t pl, pr;
    if (padChannels[i] == 2) {
      padSamp[i].nextStereo(pl, pr);
    } else {
      pl = pr = padSamp[i].next();
    }
    mixL += pl;
    mixR += pr;
  }
#endif

  int16_t outL = (int16_t)max((int32_t)MIN_16, min((int32_t)MAX_16, mixL));
  int16_t outR = (int16_t)max((int32_t)MIN_16, min((int32_t)MAX_16, mixR));
  i2s_write_samples(outL, outR);
}
