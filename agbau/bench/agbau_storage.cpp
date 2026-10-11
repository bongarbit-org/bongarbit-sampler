/*
 * agbau_storage - bench sketch for agbau/docs/migration.md, step 0.
 *
 * The Agbau kit is 8,995 MB of int16; the N16R8 has 8 MiB of PSRAM. Two ways
 * out, and this sketch measures both on the real board so the choice is made
 * with numbers instead of argued:
 *
 *  (A) The kit as one image in a flash data partition, mapped in place with
 *      esp_partition_mmap(). Fits uncut. Questions only the board answers:
 *      does a ~9MB mapping succeed next to 8MB of mapped PSRAM, what does the
 *      boot-time CRC cost, and what do voices cost per block when every read
 *      is a flash cache miss?
 *  (B) The trimmed kit as 16-bit WAVs on SD, loaded into PSRAM, the way the
 *      sampler does it. Fits by ~100KB. Questions: how long the boot takes,
 *      and how much PSRAM is really left after.
 *
 * Both run in one boot, A first, if their inputs are there. When both are,
 * every take present uncut in both is compared sample by sample - the claim
 * that the two options hold the same bits, measured instead of asserted.
 *
 * Then the VOICE LADDER, which needs nobody to press anything. The real
 * VoiceBank (agbau/src/voice.h) is stepped through 1, 2, 4, 8, 12, 16 voices -
 * the Agbau's maximum - and on past it to 24, 32, 48 and 64, to show the
 * margin. Every voice loops its take with the real envelope code, so each step
 * costs what that many sounding voices cost. Each step plays twice:
 *
 *   tone     6s of a pure 440Hz tone, with all N voices rendering MUTED. A
 *            pure tone is the most unforgiving signal there is for a dropout:
 *            a single lost block is a sharp click. If the render ever starves
 *            the 5.8ms DMA ring, this is where it is heard.
 *   voices   6s of the N voices audible, levelled so every step sounds about
 *            as loud as the last.
 *
 * Serial prints the step as it starts and one line per second: blocks/s
 * (1378 = the I2S clock is in charge), render time against the block period,
 * and the longest gap between two renders. A gap past the ring's 5.8ms is an
 * underrun by arithmetic, flagged whether or not it was heard. After the
 * ladder a table summarises every step; then it starts again.
 *
 * SERIAL GOES OUT THE COM CONNECTOR in this env, not the native one: audio is
 * only reliable with the board on COM (shared/docs/bancada.md, section 11), and
 * a listening test needs the log on the same cable as the sound.
 *
 * PREPARING THE INPUTS
 *
 *   (A) python3 agbau/tools/agbau_kit.py <agbau samples dir> --partition kit.bin
 *       pio run -e agbau_storage -t upload        (writes the partition table)
 *       ~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
 *           --chip esp32s3 --baud 921600 write_flash 0x310000 kit.bin
 *       The offset is the "samples" row of agbau/partitions.csv. PlatformIO's
 *       own python, because the project venv lacks esptool's dependencies.
 *
 *   (B) python3 agbau/tools/agbau_kit.py <agbau samples dir> --trim-envelope \
 *           --trim-tail-db -60 --sd sd_kit
 *       Copy sd_kit/*.wav into /agbau/ on the card. The UNTRIMMED set does
 *       not fit, and this sketch will say so rather than truncate anything.
 *
 * This is a measurement, not the firmware: no buttons, no kit presets, every
 * ladder voice looping. Results go to shared/docs/bancada.md, and the A/B
 * choice is the user's to make from them.
 */

// First, because it also sets the I2S ring (5.8ms, the sampler's) and M16
// reads that with #ifndef - see shared/src/engine_config.h. The build fails if
// this drifts below M16.h.
#include "audio_block.h"

#include "M16.h"
#include "Wav.h"
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <math.h>

#include "kit_image.h"
#include "voice.h"

// --- Knobs, all #ifndef-guarded so they can be set per build ---------------

// Play from PSRAM even when the flash image is mapped. Default is flash,
// because that is the option nobody has heard yet.
#ifndef SPIKE_PLAY_PSRAM
  #define SPIKE_PLAY_PSRAM 0
#endif
// SD clock in MHz. 16 is what the sampler uses; raise it to see how much of
// the boot time is the bus.
#ifndef SPIKE_SD_MHZ
  #define SPIKE_SD_MHZ 16
#endif
// Undo M16's 10ms edge fades after each SD load (CLAUDE.md, "Embedding a
// sample in flash"). ON here, unlike the sampler's opt-in WAV_RESTORE_EDGES:
// this sketch compares two ways of STORING the kit, and a fade that only one
// of them applies would be a difference in sound blamed on storage - on a
// ganza loop it is a ~20ms hole at every wrap.
#ifndef SPIKE_RESTORE_EDGES
  #define SPIKE_RESTORE_EDGES 1
#endif
// Blocks per storage benchmark: ~3s of audio, so each of the sixteen streams
// walks ~260KB - far past any cache, which is the point.
#define SPIKE_BENCH_BLOCKS 4134
// Raw-stream gain for the storage benchmark; it is never heard.
#define SPIKE_VOICE_GAIN 0.125f

// --- The ladder -------------------------------------------------------------

#define LADDER_MAX_VOICES 64
static const uint8_t kLadder[] = {1, 2, 4, 8, 12, 16, 24, 32, 48, 64};
#define LADDER_STEPS (sizeof(kLadder) / sizeof(kLadder[0]))
#define LADDER_PHASE_MS 6000
// The ring holds 4 x 64 frames = 5.8ms. A gap between two renders longer than
// that has emptied it: an underrun by arithmetic. Past 4ms it is flagged as
// close.
#define RING_US   ((uint32_t)(1000000ULL * DMA_BUFFERS * DMA_BUFFER_LENGTH / SAMPLE_RATE))
#define CLOSE_US  4000
// Canary tone: 440Hz at -12dBFS, from a table so the audio task does no sinf().
#define TONE_HZ   440.0f
#define TONE_AMP  0.25f
#define TONE_LEN  1024

// --- The kit -------------------------------------------------------------

#define TAKE_COUNT  24
#define VOICE_COUNT 16

static const char* const kTakes[TAKE_COUNT] = {
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8",
  "p1.3.1", "p1.3.2", "p1.3.3", "p1.3.4", "p1.3.5", "p2.2.1", "p3.1", "p4.3.1",
  "p5.3.1", "p5.3.2", "p5.3.3", "p6.3.1", "p6.3.2", "p6.3.3", "p7.1", "p8.3.1",
};

// One take per Agbau voice; the ladder cycles through these.
static const char* const kVoiceTakes[VOICE_COUNT] = {
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8",
  "p1.3.1", "p2.2.1", "p3.1", "p4.3.1", "p5.3.1", "p6.3.1", "p7.1", "p8.3.1",
};

// --- SD (option B), same pins as the sampler (shared/docs/circuit.md) --------
#define SD_CS   10
#define SD_SCLK 12
#define SD_MISO 13
#define SD_MOSI 11

struct TakeStream {
  const int16_t* data   = nullptr;
  uint32_t       frames = 0;
  uint32_t       pos    = 0;
};

static KitImage   gKit;                       // option A: pointers into flash
static TakeStream gFlash[VOICE_COUNT];
static TakeStream gPsram[VOICE_COUNT];
static bool       gHaveFlash = false, gHavePsram = false;
static const char* gPlayName = "none";

static SdFs sd;
static Wav  gWav;
struct Loaded { const int16_t* data; uint32_t frames; };
static Loaded gLoaded[TAKE_COUNT];

// The ladder's voices. Audio task renders them; loop() only posts to them.
static VoiceBank<LADDER_MAX_VOICES> gBank;
static float gTone[TONE_LEN];
static uint32_t gTonePhase = 0;
static const uint32_t kToneStep =
    (uint32_t)(TONE_HZ / SAMPLE_RATE * 4294967296.0);

// Set by loop(), read by the audio task once per block.
static volatile float gVoiceMix = 0.0f;      // 0 = voices rendered but muted
static volatile float gToneMix  = 0.0f;

// Written by the audio task, read by loop(). Same reasoning as DEBUG_ENGINE in
// the sampler: a torn read costs one wrong line, a lock would cost the audio.
static volatile uint32_t gBlocks = 0, gRenderSum = 0, gRenderWorst = 0;
static volatile uint32_t gGapWorst = 0;
static volatile int32_t  gPeak = 0;

// Every boot result line is also kept and reprinted at the start of each
// ladder pass: on whichever connector, whatever is printed before the host
// opens the port is lost, and the boot is when the numbers that matter print.
static char   gSummary[3072];
static size_t gSummaryLen = 0;

static void note(const char* fmt, ...) {
  char line[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.print(line);
  size_t n = strlen(line);
  if (gSummaryLen + n < sizeof(gSummary)) {
    memcpy(gSummary + gSummaryLen, line, n + 1);
    gSummaryLen += n;
  }
}

static uint32_t blockPeriodCycles() {
  return (uint32_t)((uint64_t)ESP.getCpuFreqMHz() * 1000000ULL *
                    AUDIO_BLOCK_FRAMES / SAMPLE_RATE);
}

// --- (A) Flash partition --------------------------------------------------

static bool mapFlashKit() {
  const esp_partition_t* part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "samples");
  if (!part) {
    note("A: no 'samples' partition - this env must build with "
         "agbau/partitions.csv\n");
    return false;
  }
  note("A: partition 'samples' at 0x%06lx, %lu bytes\n",
       (unsigned long)part->address, (unsigned long)part->size);
  unsigned long pagesBefore = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);

  // Map one page first: image_bytes says how much of the partition to map.
  const void* p = nullptr;
  esp_partition_mmap_handle_t h;
  size_t headLen = part->size < 65536 ? part->size : 65536;
  esp_err_t err = esp_partition_mmap(part, 0, headLen, ESP_PARTITION_MMAP_DATA, &p, &h);
  if (err != ESP_OK) {
    note("A: mapping the header page FAILED: %s\n", esp_err_to_name(err));
    return false;
  }
  uint32_t imageBytes = 0;
  KitImageError ke = KitImage::peekSize((const uint8_t*)p, headLen, &imageBytes);
  esp_partition_munmap(h);
  if (ke != KIT_OK) {
    note("A: %s\n", kitImageErrorName(ke));
    return false;
  }
  if (imageBytes > part->size) {
    note("A: image claims %lu bytes, partition holds %lu\n",
         (unsigned long)imageBytes, (unsigned long)part->size);
    return false;
  }

  uint32_t t0 = micros();
  err = esp_partition_mmap(part, 0, imageBytes, ESP_PARTITION_MMAP_DATA, &p, &h);
  uint32_t tMap = micros() - t0;
  if (err != ESP_OK) {
    note("A: mapping %lu bytes FAILED: %s (free data MMU pages before: %lu)\n",
         (unsigned long)imageBytes, esp_err_to_name(err), pagesBefore);
    return false;
  }
  // Never unmapped: the audio task reads it for as long as the board is on.
  unsigned long pagesAfter = spi_flash_mmap_get_free_pages(SPI_FLASH_MMAP_DATA);
  note("A: mapped %lu bytes in %lu us; free data MMU pages %lu -> %lu (64KB each)\n",
       (unsigned long)imageBytes, (unsigned long)tMap, pagesBefore, pagesAfter);

  t0 = micros();
  ke = gKit.parse((const uint8_t*)p, imageBytes, false);
  uint32_t tBounds = micros() - t0;
  if (ke != KIT_OK) {
    note("A: image rejected: %s\n", kitImageErrorName(ke));
    return false;
  }
  t0 = micros();
  ke = gKit.parse((const uint8_t*)p, imageBytes, true);
  uint32_t tCrc = micros() - t0;
  note("A: %u takes; bounds check %lu us, full CRC %lu ms -> %s\n",
       gKit.count, (unsigned long)tBounds, (unsigned long)(tCrc / 1000),
       kitImageErrorName(ke));
  if (ke != KIT_OK) return false;

  for (int v = 0; v < VOICE_COUNT; v++) {
    const KitTake* t = gKit.find(kVoiceTakes[v]);
    if (!t) {
      note("A: take '%s' missing from the image\n", kVoiceTakes[v]);
      return false;
    }
    gFlash[v].data   = t->data;
    gFlash[v].frames = t->frames;
  }
  return true;
}

// --- (B) SD -> PSRAM --------------------------------------------------------

// Re-read the 20ms at each end that Wav::load() just faded - the same repair
// as restoreEdges() in the sampler's main.cpp, which explains the margin.
// Every take here is loaded whole into a buffer of exactly its size, so both
// ends are the file's own and both are restored.
static bool restoreEdges(int16_t* buf, uint32_t frames) {
  uint32_t n = SAMPLE_RATE / 50;
  if (n > frames) n = frames;
  bool ok = gWav.readFrames(0, n, buf);
  ok = gWav.readFrames(frames - n, n, buf + (frames - n)) && ok;
  return ok;
}

static bool loadSdKit() {
  size_t freeBefore    = ESP.getFreePsram();
  size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  note("B: PSRAM free %u, largest block %u\n",
       (unsigned)freeBefore, (unsigned)largestBefore);

  uint32_t t0 = millis();
  if (!gWav.initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, SPIKE_SD_MHZ)) {
    note("B: SD mount failed - no card, or not wired (shared/docs/circuit.md)\n");
    return false;
  }
  uint32_t tMount = millis() - t0;

  // Sizes first, so the whole kit is ONE allocation of exactly its size,
  // carved into takes. Wav::load() on its own would allocate per file and
  // silently truncate any file larger than 90% of the PSRAM still free -
  // which, with ~100KB of margin, is the last few takes. External buffers
  // bypass that rule, and the frame count is checked after each load anyway.
  char path[48];
  uint32_t bytes[TAKE_COUNT];
  size_t total = 0;
  for (int i = 0; i < TAKE_COUNT; i++) {
    snprintf(path, sizeof(path), "/agbau/%s.wav", kTakes[i]);
    FsFile f = sd.open(path, O_RDONLY);
    if (!f) {
      note("B: %s not on the card\n", path);
      return false;
    }
    uint32_t size = (uint32_t)f.size();
    f.close();
    if (size <= 44) {
      note("B: %s is empty\n", path);
      return false;
    }
    bytes[i] = size - 44;                   // agbau_kit.py writes 44-byte headers
    total += (bytes[i] + 3) & ~3u;
  }
  uint8_t* block = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!block) {
    note("B: the kit needs %u bytes in one block; largest free PSRAM block "
         "is %u - it does not fit\n", (unsigned)total, (unsigned)largestBefore);
    return false;
  }

  t0 = millis();
  uint32_t tEdges = 0;
  int edgeFailures = 0;
  size_t off = 0;
  for (int i = 0; i < TAKE_COUNT; i++) {
    snprintf(path, sizeof(path), "/agbau/%s.wav", kTakes[i]);
    int16_t* slice = (int16_t*)(block + off);
    gWav.setExternalBuffer(slice, bytes[i]);
    bool ok = gWav.load(path);
    uint32_t frames = (uint32_t)gWav.getFrameCount();
    if (!ok || frames != bytes[i] / 2 || gWav.getChannels() != 1 ||
        gWav.getSampleRate() != SAMPLE_RATE) {
      note("B: %s loaded wrong: ok=%d, %lu frames (expected %lu), %u ch, %lu Hz\n",
           path, ok, (unsigned long)frames, (unsigned long)(bytes[i] / 2),
           gWav.getChannels(), (unsigned long)gWav.getSampleRate());
      return false;
    }
#if SPIKE_RESTORE_EDGES
    uint32_t te = millis();
    if (!restoreEdges(slice, frames)) edgeFailures++;
    tEdges += millis() - te;
#endif
    gLoaded[i] = {slice, frames};
    off += (bytes[i] + 3) & ~3u;
  }
  uint32_t tLoad = millis() - t0;
  note("B: %d takes, %u bytes: mount %lu ms + load %lu ms at %d MHz (%.2f MB/s)\n",
       TAKE_COUNT, (unsigned)total, (unsigned long)tMount, (unsigned long)tLoad,
       SPIKE_SD_MHZ, tLoad ? (double)total / 1000.0 / tLoad : 0.0);
#if SPIKE_RESTORE_EDGES
  note("B: M16's edge fades undone: %lu ms of the load, %d failed re-reads\n",
       (unsigned long)tEdges, edgeFailures);
#else
  note("B: M16's edge fades LEFT IN (SPIKE_RESTORE_EDGES=0)\n");
#endif
  note("B: PSRAM free after %u, largest block %u\n",
       (unsigned)ESP.getFreePsram(),
       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));

  for (int v = 0; v < VOICE_COUNT; v++) {
    for (int i = 0; i < TAKE_COUNT; i++) {
      if (strcmp(kVoiceTakes[v], kTakes[i]) == 0) {
        gPsram[v].data   = gLoaded[i].data;
        gPsram[v].frames = gLoaded[i].frames;
      }
    }
  }
  return true;
}

// Every take present in both, at the same length, compared sample by sample.
// Takes the SD set trimmed have different lengths and are skipped - their
// content differs by design.
static void compareSources() {
  int same = 0, differ = 0, trimmed = 0;
  for (int i = 0; i < TAKE_COUNT; i++) {
    const KitTake* a = gKit.find(kTakes[i]);
    if (!a) continue;
    if (a->frames != gLoaded[i].frames) { trimmed++; continue; }
    uint32_t diffs = 0, first = 0;
    for (uint32_t k = 0; k < a->frames; k++) {
      if (a->data[k] != gLoaded[i].data[k]) {
        if (diffs++ == 0) first = k;
      }
    }
    if (diffs == 0) {
      same++;
    } else {
      differ++;
      note("A/B: %s differs in %lu of %lu samples, first at frame %lu\n",
           kTakes[i], (unsigned long)diffs, (unsigned long)a->frames,
           (unsigned long)first);
    }
  }
  note("A/B: %d takes compared sample by sample: %d identical, %d different; "
       "%d trimmed on SD, not comparable\n", same + differ, same, differ, trimmed);
}

// --- Storage benchmark: raw streams, no envelope -----------------------------

static void renderStreams(TakeStream* s, AudioBlock& b) {
  audioBlockSilence(b);
  for (int v = 0; v < VOICE_COUNT; v++) {
    TakeStream& st = s[v];
    for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
      float x = audioFromI16(st.data[st.pos]) * SPIKE_VOICE_GAIN;
      b.l[n] += x;
      b.r[n] += x;
      if (++st.pos >= st.frames) st.pos = 0;
    }
  }
}

static void benchRender(const char* label, TakeStream* s) {
  AudioBlock b;
  uint64_t sum = 0;
  uint32_t worst = 0;
  for (int i = 0; i < SPIKE_BENCH_BLOCKS; i++) {
    uint32_t c0 = ESP.getCycleCount();
    renderStreams(s, b);
    uint32_t c = ESP.getCycleCount() - c0;
    sum += c;
    if (c > worst) worst = c;
  }
  for (int v = 0; v < VOICE_COUNT; v++) s[v].pos = 0;
  uint32_t period = blockPeriodCycles();
  uint32_t avg = (uint32_t)(sum / SPIKE_BENCH_BLOCKS);
  note("%s: 16 streams, avg %lu cycles/block (%.1f%%), worst %lu (%.1f%%) "
       "of a %lu-cycle block\n", label, (unsigned long)avg,
       100.0 * avg / period, (unsigned long)worst, 100.0 * worst / period,
       (unsigned long)period);
}

// --- Ladder bookkeeping (loop() only) ---------------------------------------

struct PhaseResult {
  float    renderAvgPct, renderWorstPct;
  uint32_t gapWorstUs, blocksMin;
};
static PhaseResult gResults[LADDER_STEPS][2];
static uint8_t  gStep = 0, gPhase = 0;          // phase 0 = tone, 1 = voices
static uint8_t  gActive = 0;                    // voices note-on'd so far
static uint32_t gPhaseStart = 0;
static PhaseResult gAcc;

static void setVoices(uint8_t n) {
  for (uint8_t v = gActive; v < n; v++) gBank.post(v, true);
  for (uint8_t v = n; v < gActive; v++) gBank.post(v, false);
  gActive = n;
}

static void startPhase(uint32_t now) {
  uint8_t n = kLadder[gStep];
  if (gPhase == 0) {
    setVoices(n);
    gVoiceMix = 0.0f;
    gToneMix  = 1.0f;
    Serial.printf("\n=== %u voice%s: %d s of pure tone, the voices rendering muted ===\n",
                  n, n == 1 ? "" : "s", LADDER_PHASE_MS / 1000);
  } else {
    // Each voice is a take at half gain (VOICE_GAIN); N uncorrelated takes
    // add as sqrt(N). Keep every step at about one voice's loudness.
    gVoiceMix = 1.2f / sqrtf((float)n);
    gToneMix  = 0.0f;
    Serial.printf("=== %u voice%s: %d s of the voices themselves ===\n",
                  n, n == 1 ? "" : "s", LADDER_PHASE_MS / 1000);
  }
  gPhaseStart = now;
  gAcc = PhaseResult{0.0f, 0.0f, 0, 0xFFFFFFFF};
}

static void printLadderTable() {
  Serial.printf("\n--- ladder summary (%s; ring %lu us; block period %lu cycles) ---\n",
                gPlayName, (unsigned long)RING_US, (unsigned long)blockPeriodCycles());
  Serial.printf("voices | phase  | render avg  worst | worst gap | min blocks/s | verdict\n");
  for (uint8_t s = 0; s < LADDER_STEPS; s++) {
    for (uint8_t ph = 0; ph < 2; ph++) {
      const PhaseResult& r = gResults[s][ph];
      const char* v = r.gapWorstUs > RING_US ? "UNDERRUN" :
                      (r.gapWorstUs > CLOSE_US || r.blocksMin < 1370) ? "close" : "ok";
      Serial.printf("%6u | %-6s | %8.1f%% %5.1f%% | %6lu us | %12lu | %s\n",
                    kLadder[s], ph == 0 ? "tone" : "voices", r.renderAvgPct,
                    r.renderWorstPct, (unsigned long)r.gapWorstUs,
                    (unsigned long)r.blocksMin, v);
    }
  }
  Serial.printf("(the gaps include the first second of each phase, when voices\n"
                " were just switched on - a spike only there is a start-up cost)\n");
}

void setup() {
  Serial.begin(115200);
  uint32_t t = millis();
  while (!Serial && millis() - t < 3000) delay(10);
  delay(200);

  note("\n=== agbau storage spike ===\n");
  if (ESP.getPsramSize() == 0) {
    note("PSRAM: NOT AVAILABLE - check board = esp32-s3-devkitc1-n16r8\n");
  }
  note("CPU %lu MHz, PSRAM %u bytes, internal heap free %u\n",
       (unsigned long)ESP.getCpuFreqMHz(), (unsigned)ESP.getPsramSize(),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  gHaveFlash = mapFlashKit();
  gHavePsram = loadSdKit();
  if (gHaveFlash && gHavePsram) compareSources();

  if (gHaveFlash) benchRender("A flash", gFlash);
  if (gHavePsram) benchRender("B psram", gPsram);

  TakeStream* src = nullptr;
  if (gHaveFlash && !(SPIKE_PLAY_PSRAM && gHavePsram)) {
    src = gFlash;  gPlayName = "flash";
  } else if (gHavePsram) {
    src = gPsram;  gPlayName = "psram";
  }
  note("Ladder plays from: %s%s\n", gPlayName,
       src ? "" : " - no kit, so only the tone is real; voice steps are silent");

  // Ladder voices: every one loops its take with the real envelope, so each
  // costs what a held ganza costs - the most expensive kind of Agbau voice.
  const VoiceConfig looping{0.01f, 0.0f, 1.0f, 0.1f, true, true};
  for (int v = 0; v < LADDER_MAX_VOICES; v++) {
    gBank.voices[v].configure(looping, SAMPLE_RATE);
    if (src) {
      const TakeStream& s = src[v % VOICE_COUNT];
      gBank.voices[v].addTake(s.data, s.frames);
    }
  }
  for (int i = 0; i < TONE_LEN; i++)
    gTone[i] = TONE_AMP * sinf(6.28318530718f * i / TONE_LEN);

  note("Internal heap free %u. Ring %lu us.\n",
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned long)RING_US);

  seti2sPins(16, 17, 18, -1);
  setIsDualCore(false);       // one task, one static block - see the sampler
  audioStart();

  Serial.print("\n--- boot summary ---\n");
  Serial.print(gSummary);
  startPhase(millis());
}

void loop() {
  static uint32_t lastReport = 0, lastBlocks = 0;
  uint32_t now = millis();

  if (now - lastReport >= 1000) {
    uint32_t blocks = gBlocks, sum = gRenderSum, worst = gRenderWorst;
    uint32_t gap = gGapWorst;
    int32_t  peak = gPeak;
    gRenderSum = 0;
    gRenderWorst = 0;
    gGapWorst = 0;
    gPeak = 0;
    uint32_t n = blocks - lastBlocks;
    uint32_t period = blockPeriodCycles();
    float avgPct = n ? 100.0f * (sum / n) / period : 0.0f;
    float worstPct = 100.0f * worst / period;
    const char* flag = gap > RING_US ? "  <-- UNDERRUN" :
                       (gap > CLOSE_US || n < 1370) ? "  <-- close" : "";
    Serial.printf("  %2u voices %-6s  blocks/s %lu  render avg %.1f%% worst %.1f%%"
                  "  gap %lu us  peak %ld%s\n", kLadder[gStep],
                  gPhase == 0 ? "tone" : "voices", (unsigned long)n, avgPct,
                  worstPct, (unsigned long)gap, (long)peak, flag);
    if (lastReport != 0) {
      if (avgPct > gAcc.renderAvgPct) gAcc.renderAvgPct = avgPct;
      if (worstPct > gAcc.renderWorstPct) gAcc.renderWorstPct = worstPct;
      if (gap > gAcc.gapWorstUs) gAcc.gapWorstUs = gap;
      if (n < gAcc.blocksMin) gAcc.blocksMin = n;
    }
    lastBlocks = blocks;
    lastReport = now;
  }

  if (now - gPhaseStart >= LADDER_PHASE_MS) {
    gResults[gStep][gPhase] = gAcc;
    if (++gPhase == 2) {
      gPhase = 0;
      if (++gStep == LADDER_STEPS) {
        printLadderTable();
        gStep = 0;
        Serial.print("\n--- boot summary ---\n");
        Serial.print(gSummary);
      }
    }
    startPhase(now);
  }
  delay(5);
}

void audioUpdate() {
  static AudioBlock blk;   // one task only - setIsDualCore(false) above
  static int64_t lastStart = 0;

  int64_t start = esp_timer_get_time();
  if (lastStart != 0) {
    uint32_t gap = (uint32_t)(start - lastStart);
    if (gap > gGapWorst) gGapWorst = gap;
  }
  lastStart = start;

  uint32_t c0 = ESP.getCycleCount();
  audioBlockSilence(blk);
  gBank.render(blk);
  uint32_t c = ESP.getCycleCount() - c0;
  gRenderSum = gRenderSum + c;
  if (c > gRenderWorst) gRenderWorst = c;

  // The voices are always rendered; in a tone phase they are multiplied by
  // zero here, after the cost has been paid and measured.
  const float vmix = gVoiceMix, tmix = gToneMix;
  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    float tone = tmix * gTone[gTonePhase >> (32 - 10)];
    gTonePhase += kToneStep;
    int16_t outL = audioToI16(audioSoftClip(blk.l[n] * vmix + tone));
    int16_t outR = audioToI16(audioSoftClip(blk.r[n] * vmix + tone));
    int32_t mag = outL < 0 ? -(int32_t)outL : (int32_t)outL;
    if (mag > gPeak) gPeak = mag;
    audioBlockWrite(outL, outR);
  }
  gBlocks = gBlocks + 1;
}
