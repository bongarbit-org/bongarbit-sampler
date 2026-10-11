/*
 * agbau - the Agbau's voices on the ESP32-S3, first bring-up.
 *
 * docs/agbau-migration.md, step 5, cut down on purpose: no 4x4 matrix yet,
 * no repeat mode, no looper. Four buttons on the pins the sampler bench is
 * already wired with (GPIO 4, 5, 6, 7, each to GND), each playing one Agbau
 * voice, so the three ways an Agbau voice behaves can be heard and checked
 * before anything about the instrument's layout is:
 *
 *   bass        sounds while held, falls away over 1s when released
 *   percussion  the 3s decay is the whole hit; releasing changes nothing;
 *               every strike picks one of the voice's takes at random
 *   ganza       loops for as long as it is held, gone 0.1s after release
 *
 * kButtonVoice below says which voice each button plays. Change the numbers,
 * not the wiring: voice i is the Bela's samplers[i] (src/agbau_kit.h).
 *
 * The kit is read in place from the flash "samples" partition (option A of
 * step 0, docs/bancada.md section 13): written once with
 *   python3 tools/agbau_kit.py <agbau samples dir> --partition kit.bin
 *   esptool.py --chip esp32s3 write_flash 0x310000 kit.bin
 * and never copied. Uploading this firmware does not touch it.
 *
 * Everything that makes sound is a host-tested header: the envelope
 * (envelope.h), the voices and the queue that is the only path from loop()
 * to them (voice.h, note_queue.h), the kit table (agbau_kit.h), the image
 * parser (kit_image.h) and the button conditioning (key_matrix.h over
 * trigger_input.h). This file only wires them to pins, flash and M16.
 */

// First, because it also sets the I2S ring (5.8ms, the sampler's) and M16
// reads that with #ifndef - see src/engine_config.h. The build fails if this
// drifts below M16.h.
#include "audio_block.h"

#include "M16.h"
#include <esp_partition.h>
#include <esp_random.h>

#include "kit_image.h"
#include "voice.h"
#include "agbau_kit.h"
#include "key_matrix.h"

// --- Diagnostics -------------------------------------------------------------
// Both ON by default here, unlike the sampler, and on purpose: this firmware
// exists to watch triggers happen, and its Serial is UART0 out of the COM
// connector, which buffers instead of stalling the way USB CDC does when the
// host is not reading. Still turn them off before judging latency.
#ifndef DEBUG_TRIGGERS
  #define DEBUG_TRIGGERS 1
#endif
#ifndef DEBUG_ENGINE
  #define DEBUG_ENGINE 1
#endif
#if DEBUG_TRIGGERS
  #define TRIGDBG(...) Serial.printf(__VA_ARGS__)
#else
  #define TRIGDBG(...)
#endif

// --- Buttons ---------------------------------------------------------------
// Four buttons wired straight to GND are a key matrix with one column, and
// that column is always "driven": ground. So they run through KeyMatrix,
// whose strike and release edges are already tested, rather than a second
// copy of that logic - the one-debounce rule from trigger_input.h, kept.
#define BUTTONS 4
static const uint8_t kButtonPins[BUTTONS]  = {4, 5, 6, 7};
static const uint8_t kButtonVoice[BUTTONS] = {
  0,    // b1      bass
  8,    // p1      percussion, five takes
  10,   // p3      ganza
  12,   // p5      percussion, three takes
};

struct DirectButtons {
  void driveColumn(uint8_t) {}
  void releaseColumn(uint8_t) {}
  void settle() {}
  bool rowLow(uint8_t row) { return digitalRead(kButtonPins[row]) == LOW; }
};

static DirectButtons             gPins;
static KeyMatrix<BUTTONS, 1>     gButtons;
static VoiceBank<AGBAU_VOICES>   gBank;
static KitImage                  gKit;

// 1.0 = unity, after the voices' own 0.5 each (Sampler.cpp:113). The Bela had
// no limiter; here the soft knee in audio_block.h follows.
static float masterGain = 1.0f;

static const char* groupName(uint8_t voice) {
  switch (AGBAU_VOICE_DEFS[voice].group) {
    case AGBAU_BASS:  return "bass";
    case AGBAU_PERC:  return "percussion";
    case AGBAU_GANZA: return "ganza";
  }
  return "?";
}

// The voice's name is its first take's: b1, p1.3.1 -> "p1".
static void voiceName(uint8_t voice, char* out, size_t len) {
  const char* t = AGBAU_VOICE_DEFS[voice].takes[0];
  size_t n = 0;
  while (t[n] && t[n] != '.' && n + 1 < len) { out[n] = t[n]; n++; }
  out[n] = '\0';
}

// --- The kit, mapped from flash ---------------------------------------------
// Errors are unconditional and name their likely cause, same convention as
// the sampler's SD and PSRAM failures.
static bool mapKit() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "samples");
  if (!part) {
    Serial.println("KIT: no 'samples' partition - this env must build with "
                   "partitions_agbau.csv");
    return false;
  }
  const void* p = nullptr;
  esp_partition_mmap_handle_t h;
  size_t headLen = part->size < 65536 ? part->size : 65536;
  if (esp_partition_mmap(part, 0, headLen, ESP_PARTITION_MMAP_DATA, &p, &h) != ESP_OK) {
    Serial.println("KIT: could not map the partition header");
    return false;
  }
  uint32_t imageBytes = 0;
  KitImageError e = KitImage::peekSize((const uint8_t*)p, headLen, &imageBytes);
  esp_partition_munmap(h);
  if (e != KIT_OK || imageBytes > part->size) {
    Serial.printf("KIT: %s - flash the kit image with esptool (see the top of "
                  "agbau_main.cpp)\n",
                  e != KIT_OK ? kitImageErrorName(e) : "image larger than partition");
    return false;
  }
  // Mapped for good: the audio task reads it as long as the board is on.
  esp_err_t err = esp_partition_mmap(part, 0, imageBytes, ESP_PARTITION_MMAP_DATA, &p, &h);
  if (err != ESP_OK) {
    Serial.printf("KIT: mapping %lu bytes failed: %s\n",
                  (unsigned long)imageBytes, esp_err_to_name(err));
    return false;
  }
  // The CRC costs ~1s of boot (docs/bancada.md section 13) and is what tells a
  // half-written partition from a good one. Worth it.
  e = gKit.parse((const uint8_t*)p, imageBytes, true);
  if (e != KIT_OK) {
    Serial.printf("KIT: image rejected: %s\n", kitImageErrorName(e));
    return false;
  }
  Serial.printf("KIT: %u takes, %lu bytes, mapped from flash, CRC ok\n",
                gKit.count, (unsigned long)imageBytes);
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== agbau: four-button bring-up ===");

  if (!mapKit()) {
    while (1) delay(1000);
  }
  auto find = [](const char* name, VoiceTake* out) {
    const KitTake* t = gKit.find(name);
    if (!t || t->sampleRate != SAMPLE_RATE) return false;
    *out = VoiceTake{t->data, t->frames};
    return true;
  };
  int missing = agbauConfigure(gBank, find, SAMPLE_RATE, esp_random());
  if (missing) {
    Serial.printf("KIT: %d takes missing or at the wrong rate - those voices "
                  "stay silent\n", missing);
  }

  for (int b = 0; b < BUTTONS; b++) {
    pinMode(kButtonPins[b], INPUT_PULLUP);
    char name[8];
    voiceName(kButtonVoice[b], name, sizeof(name));
    Serial.printf("GPIO%u -> %s (%s, %u take%s)\n", kButtonPins[b], name,
                  groupName(kButtonVoice[b]),
                  gBank.voices[kButtonVoice[b]].takeCount(),
                  gBank.voices[kButtonVoice[b]].takeCount() == 1 ? "" : "s");
  }

  seti2sPins(16, 17, 18, -1);
  // One audio task, one static block - see the same call in main.cpp.
  setIsDualCore(false);
  audioStart();
  Serial.printf("Ready. Ring %d x %d (%.1f ms).\n", DMA_BUFFERS, DMA_BUFFER_LENGTH,
                (double)(1000.0f * DMA_BUFFERS * DMA_BUFFER_LENGTH / SAMPLE_RATE));
}

#if DEBUG_ENGINE
// Written by the audio task, read by loop(), unsynchronised on purpose - see
// DEBUG_ENGINE in main.cpp.
static volatile uint32_t engineBlocks = 0, engineWorst = 0;
static volatile int32_t  enginePeak = 0;
#endif

void loop() {
  // One timestamp for the sweep, same as main.cpp. Nothing here blocks except
  // the debug prints, which are what this bring-up is for.
  uint32_t now_us = micros();
  KeyMatrix<BUTTONS, 1>::Sweep s = gButtons.scan(gPins, now_us);

  for (int b = 0; b < BUTTONS; b++) {
    const uint8_t v = kButtonVoice[b];
    if (s.struck & (1u << b)) {
      gBank.post(v, true);
#if DEBUG_TRIGGERS
      char name[8];
      voiceName(v, name, sizeof(name));
      TRIGDBG("GPIO%u down: %s on\n", kButtonPins[b], name);
#endif
    }
    if (s.released & (1u << b)) {
      gBank.post(v, false);
#if DEBUG_TRIGGERS
      char name[8];
      voiceName(v, name, sizeof(name));
      TRIGDBG("GPIO%u up:   %s off%s\n", kButtonPins[b], name,
              AGBAU_PRESETS[AGBAU_VOICE_DEFS[v].group].releaseOnNoteOff
                  ? "" : " (ignored: percussion decays on its own)");
#endif
    }
  }

#if DEBUG_ENGINE
  // Once a second: is the audio task running (1378 blocks/s), is anything
  // reaching the DAC (peak), and which voices think they are sounding, with
  // the take each one picked - the random choice, visible.
  static uint32_t lastReport = 0, lastBlocks = 0;
  if ((uint32_t)(now_us - lastReport) >= 1000000UL) {
    uint32_t blocks = engineBlocks;
    int32_t  peak = enginePeak;
    uint32_t worst = engineWorst;
    enginePeak = 0;
    engineWorst = 0;
    uint32_t period = (uint32_t)((uint64_t)ESP.getCpuFreqMHz() * 1000000ULL *
                                 AUDIO_BLOCK_FRAMES / SAMPLE_RATE);
    Serial.printf("ENG blocks/s %lu  peak %ld  render worst %.1f%%  dropped %lu  playing[",
                  (unsigned long)(blocks - lastBlocks), (long)peak,
                  100.0 * worst / period, (unsigned long)gBank.dropped());
    bool first = true;
    for (int v = 0; v < AGBAU_VOICES; v++) {
      const Voice& voice = gBank.voices[v];
      if (!voice.playing()) continue;
      Serial.printf("%s%s", first ? "" : " ", AGBAU_VOICE_DEFS[v].takes[voice.take()]);
      first = false;
    }
    Serial.println("]");
    lastReport = now_us;
    lastBlocks = blocks;
  }
#endif
}

void audioUpdate() {
  // One block, reused; one audio task only (setIsDualCore(false) above).
  static AudioBlock blk;

#if DEBUG_ENGINE
  uint32_t c0 = ESP.getCycleCount();
#endif
  audioBlockSilence(blk);
  gBank.render(blk);     // applies the queued note-ons/offs, then the voices
#if DEBUG_ENGINE
  uint32_t c = ESP.getCycleCount() - c0;
  if (c > engineWorst) engineWorst = c;
#endif

  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    int16_t outL = audioToI16(audioSoftClip(blk.l[n] * masterGain));
    int16_t outR = audioToI16(audioSoftClip(blk.r[n] * masterGain));
#if DEBUG_ENGINE
    int32_t mag = outL < 0 ? -(int32_t)outL : (int32_t)outL;
    if (mag > enginePeak) enginePeak = mag;
#endif
    audioBlockWrite(outL, outR);
  }
#if DEBUG_ENGINE
  engineBlocks = engineBlocks + 1;
#endif
}
