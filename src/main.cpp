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

// --- Debug logging over Serial (SD/DAC init, button presses, playback) ---
// Set to 0 once things are working to quiet the serial output.
#define DEBUG_SAMPLER 1
#if DEBUG_SAMPLER
  #define DBG(...) Serial.printf(__VA_ARGS__)
#else
  #define DBG(...)
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

// --- Extra pads: 1.wav / 2.wav / 3.wav on the SD card root ---
// Each pad fires on the raw pin reading - no debounce filter in the trigger
// path, so a strike costs PAD_CONFIRM_US of latency and nothing else.
//
// Two guards, both watching the RAW pin, deliberately independent of each
// other so neither can silently disable the other:
//  - PAD_CONFIRM_US: the LOW must persist this long before firing. Rejects EMI
//    pulses shorter than the window. Keep it well under one contact-bounce
//    segment (~300us on a typical tactile switch): if the window exceeds a
//    segment it cannot be satisfied until the whole bounce settles, and the
//    trigger latency becomes the bounce duration instead. Measured: 200us
//    costs 0.2ms, 1ms costs 3ms.
//  - PAD_RELEASE_MS: after firing, the pad rearms only once the pin has read
//    HIGH continuously for this long. This is what stops re-fire during
//    contact bounce.
//
// Both were previously derived from Bounce2's debounced state, and that was
// the bug: the debounced state lags the pin by its interval, so a rearm
// condition read from it fires immediately after the strike (level) or never
// fires at all when the contact time is shorter than the interval (edge). Any
// scheme built on that state couples correctness to the debounce constant.
// These read the pin directly, so the failure modes stay bounded and
// independent. All of this is measured in sim/trigger_sim.cpp.
#define PAD_COUNT 3
#define PAD_CONFIRM_US 200
#define PAD_RELEASE_MS 3
const uint8_t   padPins[PAD_COUNT]  = {5, 6, 7};
const char*     padFiles[PAD_COUNT] = {"/1.wav", "/2.wav", "/3.wav"};
Wav             padWav[PAD_COUNT];
Samp            padSamp[PAD_COUNT];
uint8_t         padChannels[PAD_COUNT] = {1, 1, 1};
bool            padArmed[PAD_COUNT]    = {true, true, true};
// micros() timestamps; all comparisons are unsigned differences so the ~71min
// micros() wraparound is a non-event.
bool            padPending[PAD_COUNT]     = {false, false, false};
uint32_t        padPendingSince[PAD_COUNT] = {0, 0, 0};
bool            padHighValid[PAD_COUNT]   = {false, false, false};
uint32_t        padHighSince[PAD_COUNT]   = {0, 0, 0};
#endif

// --- Trigger button (see docs/circuit.md) ---
// One leg on TRIGGER_PIN (internal pull-up, pressed = LOW), the other leg
// on a real GND pin on the dev kit.
#define TRIGGER_PIN 4
#define DEBOUNCE_MS 25

bool     lastRawReading    = HIGH; // raw pin reading, only used to reset the debounce timer on change
bool     buttonState       = HIGH; // debounced, stable state
uint32_t lastDebounceTime  = 0;

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
  Serial.println("Ready. Press the trigger button to play the sample.");
}

void loop() {
  bool reading = digitalRead(TRIGGER_PIN);

  // Raw, unfiltered pin state, printed regardless of debounce/edge logic -
  // lets you watch GPIO4 live while physically pressing the button.
  static uint32_t lastRawPrint = 0;
  if (millis() - lastRawPrint > 500) {
    lastRawPrint = millis();
    DBG("Button raw: GPIO%d = %s\n", TRIGGER_PIN, reading == LOW ? "LOW" : "HIGH");
  }

  if (reading != lastRawReading) {
    lastDebounceTime = millis();
  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        DBG("Button: pressed (GPIO%d LOW)\n", TRIGGER_PIN);
        samp.start();
      } else {
        DBG("Button: released\n");
      }
    }
  }
  lastRawReading = reading;

#if USE_SD_SAMPLER
  uint32_t padNow = micros();
  for (uint8_t i = 0; i < PAD_COUNT; i++) {
    bool low = (digitalRead(padPins[i]) == LOW); // raw pin, never a filtered state
    if (padArmed[i]) {
      if (!low) {
        padPending[i] = false; // bounced back HIGH before confirming: a glitch, not a strike
      } else if (!padPending[i]) {
        padPending[i] = true;
        padPendingSince[i] = padNow;
      } else if ((uint32_t)(padNow - padPendingSince[i]) >= PAD_CONFIRM_US) {
        DBG("Pad %u: triggered\n", i + 1);
        padSamp[i].start();
        padArmed[i]    = false; // locked out until a clean release, below
        padPending[i]  = false;
        padHighValid[i] = false;
      }
    } else if (low) {
      padHighValid[i] = false; // still held (or bouncing): restart the release window
    } else if (!padHighValid[i]) {
      padHighValid[i] = true;
      padHighSince[i] = padNow;
    } else if ((uint32_t)(padNow - padHighSince[i]) >= PAD_RELEASE_MS * 1000UL) {
      padArmed[i] = true; // pin has read HIGH continuously - safe to fire again
    }
  }
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
