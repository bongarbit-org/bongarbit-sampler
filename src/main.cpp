/*
 * bongarbit-sampler
 *
 * One-shot sample triggers via 3 buttons (GPIO4/5/6), each playing its own
 * sample out to a PCM5102A I2S DAC via the M16 library. Pressing a button
 * again while its sample is playing restarts it from the beginning. All 3
 * buttons are equivalent - none is a special "trigger" button.
 *
 * A mode button (GPIO41, "BM") cycles through PAGE_COUNT pages; which page
 * is active decides which of the 3 buttons' samples gets played. This lets
 * 3 physical buttons reach PAGE_COUNT*3 samples. Its LED (GPIO42, "LM")
 * follows the same page indicator pattern as bongarbit/botoes-falantes
 * (github.com/bongarbit/botoes-falantes, Arduino/botoesFalantes/botoesFalantes.ino):
 * page 0 = LED off, page 1 = LED blinking (toggle every LED_BLINK_MS), page
 * 2 = LED solid on; the LED also lights up for as long as the mode button
 * itself is held (press feedback), overriding the page pattern, then the
 * pattern resumes on release.
 *
 * Hardware: ESP32-S3 N16R8 + PCM5102A (I2S) + microSD (SPI, optional - see
 * USE_SD_SAMPLER below). Full wiring plan: docs/circuit.md
 *
 * Two sample sources, selected at compile time. Both play through the same
 * Samp engine (M16's wavetable-style sample player) - the only difference is
 * where the raw audio buffers come from:
 *  - USE_SD_SAMPLER=1: only the CURRENT page's 3 files are in RAM at a time
 *    (M16's Wav::load() tries PSRAM first, so on this N16R8 board they land
 *    in the 8MB PSRAM) - named "<page>-<button>.wav", 1-indexed (page
 *    1/button 1 = "1-1.wav"). Changing page (mode button) re-reads that
 *    page's 3 files from SD into the same buffers, replacing the previous
 *    page's samples - so unlike a one-shot boot load, the **card must stay
 *    inserted** the whole time, not just at startup.
 *  - USE_SD_SAMPLER=0: no SD card needed, no pages. All 3 buttons share the
 *    same sample embedded directly in flash (src/kick_normalizado_data.h,
 *    generated from src/kick_normalizado.wav via tools/wav_to_header.py),
 *    each as its own independently retriggerable voice. The mode
 *    button/LED still work in this mode (useful to test that hardware
 *    without a card) but don't change what plays.
 */

#include "M16.h"
#include "Samp.h"
#include <Bounce2.h>

// --- Sample source: 1 = load one .wav per button from SD into RAM/PSRAM at
//                    boot, 0 = all buttons play the same embedded flash array ---
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

// --- microSD SPI pins (see docs/circuit.md) ---
#if USE_SD_SAMPLER
#define SD_CS   13
#define SD_SCLK 11
#define SD_MISO 10
#define SD_MOSI 12
// Was dropped to a conservative 4MHz early on, when a bad solder joint was
// making the card fail CMD0 (the very first command) at M16's 16MHz
// default. Wiring's since been reflowed and the card now mounts reliably,
// so back to the library default - each page switch re-reads 3 files from
// SD (see loadPage() below), and 4MHz made that pause noticeably long even
// after shrinking the samples. Drop back down (e.g. 8) if CMD0 failures
// come back - that means the wiring's margin doesn't actually cover 16MHz.
#define SD_SPI_MHZ 16

SdFs sd;
#endif

// --- The 3 buttons (see docs/circuit.md) ---
// One leg on the GPIO (internal pull-up, pressed = LOW), the other leg on a
// real GND pin on the dev kit. Each fires on the raw pin reading,
// immediately, without waiting for debounce to settle. Bounce2 (10ms
// interval) runs alongside purely to confirm a clean release before the
// button is armed again - that's what prevents it from re-firing every loop
// while held down, without adding any latency to the initial press.
#define BUTTON_COUNT 3
#define BUTTON_DEBOUNCE_MS 10
const uint8_t   buttonPins[BUTTON_COUNT]  = {4, 5, 6};
Bounce          buttonBounce[BUTTON_COUNT];
Samp            buttonSamp[BUTTON_COUNT];
uint8_t         buttonChannels[BUTTON_COUNT] = {1, 1, 1};
bool            buttonArmed[BUTTON_COUNT]    = {true, true, true};

// --- Pages (see docs/circuit.md) ---
// Only one page's worth of samples (3 files) lives in RAM at a time -
// buttonWav[i] gets reloaded from SD, in place, every time the page
// changes (see loadPage() below). Keeps PSRAM usage to 3 files instead of
// PAGE_COUNT*3, at the cost of a brief SD read (and a hard stop of
// whatever's currently playing) on every mode-button press.
#if USE_SD_SAMPLER
#define PAGE_COUNT 3
Wav  buttonWav[BUTTON_COUNT];
bool buttonLoaded[BUTTON_COUNT] = {false, false, false};
#else
#define PAGE_COUNT 1 // no real pages without an SD card - see USE_SD_SAMPLER=0 note above
#endif
uint8_t page = 0;

// --- Mode button + its LED (see docs/circuit.md) ---
// Cycles `page` on each press. The LED mirrors the current page using the
// same pattern as bongarbit/botoes-falantes (see file header comment).
#define MODE_BUTTON_PIN 41
#define MODE_LED_PIN    42
#define LED_BLINK_MS    13
Bounce   modeBounce;
uint32_t lastLedBlinkTime = 0;
bool     ledBlinkState     = false;

// --- I2S pins for the PCM5102A (see docs/circuit.md) ---
// Not the M16 library defaults (BCK/WS/DOUT = 16/17/18) - WS and DOUT are
// swapped from default here to match the perfboard wiring, set explicitly
// via seti2sPins() in setup() before audioStart().
#define I2S_BCK  16
#define I2S_WS   18
#define I2S_DOUT 17
#define I2S_DIN  21 // not wired - I2S input unused in this project

#if USE_SD_SAMPLER
// Loads page `p`'s 3 files ("/<p+1>-<i+1>.wav") into buttonWav[i], in
// place - Wav::load() frees each Wav's previous buffer before allocating
// the new one, so any button whose voice might still be mid-playback from
// the old page is stopped first (Samp::next()/nextStereo() bail out
// immediately once stopped, so this is safe even though audioUpdate() runs
// independently of loop()). A missing/bad file just leaves that slot
// unloaded (buttonLoaded false) instead of blocking the rest.
void loadPage(uint8_t p) {
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    buttonSamp[i].stop();

    char filename[16];
    snprintf(filename, sizeof(filename), "/%u-%u.wav", p + 1, i + 1);

    buttonLoaded[i] = buttonWav[i].load(filename);
    if (!buttonLoaded[i]) {
      Serial.printf("Page %u button %u: failed to load %s\n", p + 1, i + 1, filename);
      continue;
    }
    buttonChannels[i] = buttonWav[i].getChannels();
    buttonSamp[i].setTable(buttonWav[i].getBuffer(), buttonWav[i].getFrameCount(),
                            buttonWav[i].getSampleRate(), buttonChannels[i]);
    DBG("Page %u button %u: loaded '%s' - %u ch, %lu Hz, %lu frames (~%lu ms)\n",
        p + 1, i + 1, buttonWav[i].getFilename(), buttonChannels[i],
        (unsigned long)buttonWav[i].getSampleRate(), buttonWav[i].getFrameCount(),
        (unsigned long)(1000ULL * buttonWav[i].getFrameCount() / buttonWav[i].getSampleRate()));
  }
}
#endif

void setup() {
  Serial.begin(115200);
  delay(500);

#if USE_SD_SAMPLER
  // initSD() on the first button also mounts the card and lists the .wav
  // files it finds (Serial) - the other two just reuse the already-mounted
  // `sd` instance via their own initSD() call right after.
  if (!buttonWav[0].initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, SD_SPI_MHZ)) {
    Serial.print("SD: init FAILED - ");
    sd.printSdError(&Serial); // prints the specific SdFat error (e.g. CMD0 = card not responding at all)
    Serial.println("Check wiring (docs/circuit.md)");
    while (1) delay(1000);
  }
  DBG("SD: init OK\n");
  for (uint8_t i = 1; i < BUTTON_COUNT; i++) {
    buttonWav[i].initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, SD_SPI_MHZ);
  }

  loadPage(page);
#else
  DBG("Flash sample: %lu frames, %u ch, %u Hz (~%lu ms) - shared by all %u buttons, no pages\n",
      KICK_NORMALIZADO_FRAME_COUNT, KICK_NORMALIZADO_CHANNELS, KICK_NORMALIZADO_SAMPLE_RATE,
      (unsigned long)(1000ULL * KICK_NORMALIZADO_FRAME_COUNT / KICK_NORMALIZADO_SAMPLE_RATE),
      BUTTON_COUNT);
#endif

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    pinMode(buttonPins[i], INPUT_PULLUP);
    buttonBounce[i].attach(buttonPins[i], INPUT_PULLUP);
    buttonBounce[i].interval(BUTTON_DEBOUNCE_MS);

#if !USE_SD_SAMPLER
    // All 3 buttons share the same embedded clip, each as its own
    // independently retriggerable voice. KICK_NORMALIZADO_DATA is const
    // (lives in flash/.rodata) - setTable() only reads from it during
    // playback, so the cast away from const is safe.
    buttonChannels[i] = KICK_NORMALIZADO_CHANNELS;
    buttonSamp[i].setTable(const_cast<int16_t*>(KICK_NORMALIZADO_DATA), KICK_NORMALIZADO_FRAME_COUNT,
                            KICK_NORMALIZADO_SAMPLE_RATE, buttonChannels[i]);
#endif
  }

  pinMode(MODE_BUTTON_PIN, INPUT_PULLUP);
  modeBounce.attach(MODE_BUTTON_PIN, INPUT_PULLUP);
  modeBounce.interval(BUTTON_DEBOUNCE_MS);
  pinMode(MODE_LED_PIN, OUTPUT);
  digitalWrite(MODE_LED_PIN, LOW);

  seti2sPins(I2S_BCK, I2S_WS, I2S_DOUT, I2S_DIN);
  audioStart();
  DBG("DAC: I2S started - BCK=GPIO%d WS=GPIO%d DOUT=GPIO%d\n", I2S_BCK, I2S_WS, I2S_DOUT);
  DBG("DAC: PCM5102A has no status feedback pin - the 'M16 is running' line "
      "above is the only software confirmation; audible output still "
      "depends on correct wiring (docs/circuit.md)\n");
  Serial.printf("Ready. Press a button to play its sample - page %u/%u.\n", page + 1, PAGE_COUNT);
}

void loop() {
  // Raw, unfiltered pin state, printed regardless of debounce/edge logic -
  // lets you watch the 3 GPIOs live while physically pressing each button.
  static uint32_t lastRawPrint = 0;
  if (millis() - lastRawPrint > 500) {
    lastRawPrint = millis();
    for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
      DBG("Button raw: GPIO%d = %s\n", buttonPins[i],
          digitalRead(buttonPins[i]) == LOW ? "LOW" : "HIGH");
    }
  }

  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    buttonBounce[i].update(); // must run every loop for Bounce2's own interval tracking to stay accurate
    if (buttonArmed[i]) {
      if (digitalRead(buttonPins[i]) == LOW) { // raw, immediate - fires before/without waiting for debounce
#if USE_SD_SAMPLER
        if (buttonLoaded[i]) {
          DBG("Button %u: pressed (GPIO%d, page %u)\n", i + 1, buttonPins[i], page + 1);
          buttonSamp[i].start();
        } else {
          DBG("Button %u: pressed (GPIO%d, page %u) - no sample loaded for this slot\n",
              i + 1, buttonPins[i], page + 1);
        }
#else
        DBG("Button %u: pressed (GPIO%d)\n", i + 1, buttonPins[i]);
        buttonSamp[i].start();
#endif
        buttonArmed[i] = false; // locked out until a debounced release is confirmed below
      }
    } else if (buttonBounce[i].read() == HIGH) {
      buttonArmed[i] = true; // debounce confirms a clean release - safe to fire again
    }
  }

  // --- Mode LED: same page indicator pattern as bongarbit/botoes-falantes
  // (see file header) - off on page 0, blinking on page PAGE_COUNT-1... for
  // just 3 pages: off / blinking / solid. Runs every loop using whatever
  // `page` currently is.
  if (page == 0) {
    digitalWrite(MODE_LED_PIN, LOW);
  } else if (page == PAGE_COUNT - 1) {
    digitalWrite(MODE_LED_PIN, HIGH);
  } else {
    uint32_t now = millis();
    if (now - lastLedBlinkTime >= LED_BLINK_MS) {
      lastLedBlinkTime = now;
      ledBlinkState = !ledBlinkState;
      digitalWrite(MODE_LED_PIN, ledBlinkState);
    }
  }

  // --- Mode button: advances the page. Same as the reference, pressing it
  // also forces the LED on and releasing forces it off - on top of the
  // page pattern above, same ordering as the original sketch.
  modeBounce.update();
  if (modeBounce.fell()) {
    digitalWrite(MODE_LED_PIN, HIGH);
    page = (page + 1) % PAGE_COUNT;
    DBG("Mode: page %u/%u\n", page + 1, PAGE_COUNT);
#if USE_SD_SAMPLER
    loadPage(page); // blocking SD read of 3 files - audio glitches/pauses briefly here, by design
#endif
  }
  if (modeBounce.rose()) {
    digitalWrite(MODE_LED_PIN, LOW);
  }
}

void audioUpdate() {
  int32_t mixL = 0, mixR = 0;

  // All 3 buttons can play concurrently with each other - sum all voices
  // and clip, instead of just outputting the last one.
  for (uint8_t i = 0; i < BUTTON_COUNT; i++) {
    int16_t l, r;
    if (buttonChannels[i] == 2) {
      buttonSamp[i].nextStereo(l, r);
    } else {
      l = r = buttonSamp[i].next(); // nextStereo() only works for stereo buffers (num_channels==2)
    }
    mixL += l;
    mixR += r;
  }

  int16_t outL = (int16_t)max((int32_t)MIN_16, min((int32_t)MAX_16, mixL));
  int16_t outR = (int16_t)max((int32_t)MIN_16, min((int32_t)MAX_16, mixR));
  i2s_write_samples(outL, outR);
}
