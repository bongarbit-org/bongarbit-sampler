/*
 * test_agbau_storage - bench sketch for docs/agbau-migration.md, step 0.
 *
 * The Agbau kit is 8,995 MB of int16; the N16R8 has 8 MiB of PSRAM. Two ways
 * out, and this sketch measures both on the real board so the choice is made
 * with numbers instead of argued:
 *
 *  (A) The kit as one image in a flash data partition, mapped in place with
 *      esp_partition_mmap(). Fits uncut. Questions only the board answers:
 *      does a ~9MB mapping succeed next to 8MB of mapped PSRAM, what does the
 *      boot-time CRC cost, and what do sixteen voices cost per block when
 *      every read is a flash cache miss?
 *  (B) The trimmed kit as 16-bit WAVs on SD, loaded into PSRAM, the way the
 *      sampler does it. Fits by ~100KB. Questions: how long the boot takes,
 *      and how much PSRAM is really left after.
 *
 * Both run in one boot, A first, if their inputs are there. Then the same
 * sixteen-stream render is timed against each source, and finally audio
 * starts and plays all sixteen voices at once, looping, for as long as it
 * runs - the heaviest load the instrument can produce, through the same 5.8ms
 * ring the sampler uses. Listen for clicks; read the per-second line.
 *
 * PREPARING THE INPUTS
 *
 *   (A) python3 tools/agbau_kit.py <agbau samples dir> --partition kit.bin
 *       pio run -e agbau_storage -t upload        (writes the partition table)
 *       .venv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py \
 *           --chip esp32s3 write_flash 0x310000 kit.bin
 *       The offset is the "samples" row of partitions_agbau.csv.
 *
 *   (B) python3 tools/agbau_kit.py <agbau samples dir> --trim-envelope \
 *           --trim-tail-db -60 --sd sd_kit
 *       Copy sd_kit/*.wav into /agbau/ on the card. The UNTRIMMED set does
 *       not fit, and this sketch will say so rather than truncate anything.
 *
 * This is a measurement, not the firmware: no envelope, no buttons, every
 * voice always on. Results go to docs/bancada.md, and the A/B choice is the
 * user's to make from them.
 */

// Same ring as the sampler (src/main.cpp explains the numbers). Duplicated,
// asserts and all, until the Agbau firmware factors them into one header -
// docs/agbau-migration.md, step 5.
#define DMA_BUFFERS       4
#define DMA_BUFFER_LENGTH 64

#include "M16.h"
#include "Wav.h"
#include <esp_partition.h>
#include <spi_flash_mmap.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

static_assert(DMA_BUFFERS * DMA_BUFFER_LENGTH <= 512,
              "I2S ring deeper than 512 frames (~12ms) - the DMA override has "
              "drifted below #include \"M16.h\" and M16's default came back");

#include "audio_block.h"
#include "kit_image.h"

static_assert(AUDIO_BLOCK_FRAMES * 4 <= DMA_BUFFERS * DMA_BUFFER_LENGTH,
              "audio block is more than a quarter of the DMA ring");

// --- Knobs, all #ifndef-guarded so they can be set per build ---------------

// Play from PSRAM even when the flash image is mapped. Default is flash,
// because that is the option nobody has heard yet.
#ifndef SPIKE_PLAY_PSRAM
  #define SPIKE_PLAY_PSRAM 0
#endif
// SD clock in MHz. 16 is what main.cpp uses; raise it to see how much of the
// boot time is the bus.
#ifndef SPIKE_SD_MHZ
  #define SPIKE_SD_MHZ 16
#endif
// Blocks per render benchmark: ~3s of audio, so each of the sixteen streams
// walks ~260KB - far past any cache, which is the point.
#define SPIKE_BENCH_BLOCKS 4134
// Sixteen voices at 1/8 each peak at 2.0 if everything lines up; the soft knee
// takes the rest. Loud enough to hear a click, not a performance level.
#define SPIKE_VOICE_GAIN 0.125f

// --- The kit -------------------------------------------------------------

#define TAKE_COUNT  24
#define VOICE_COUNT 16

static const char* const kTakes[TAKE_COUNT] = {
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8",
  "p1.3.1", "p1.3.2", "p1.3.3", "p1.3.4", "p1.3.5", "p2.2.1", "p3.1", "p4.3.1",
  "p5.3.1", "p5.3.2", "p5.3.3", "p6.3.1", "p6.3.2", "p6.3.3", "p7.1", "p8.3.1",
};

// One take per voice, so the render has all sixteen voices sounding.
static const char* const kVoiceTakes[VOICE_COUNT] = {
  "b1", "b2", "b3", "b4", "b5", "b6", "b7", "b8",
  "p1.3.1", "p2.2.1", "p3.1", "p4.3.1", "p5.3.1", "p6.3.1", "p7.1", "p8.3.1",
};

// --- SD (option B), same pins as the sampler (docs/circuit.md) ------------
#define SD_CS   10
#define SD_SCLK 12
#define SD_MISO 13
#define SD_MOSI 11

struct TakeStream {
  const int16_t* data   = nullptr;
  uint32_t       frames = 0;
  uint32_t       pos    = 0;
};

static KitImage gKit;                       // option A: pointers into flash
static TakeStream   gFlash[VOICE_COUNT];
static TakeStream   gPsram[VOICE_COUNT];
static bool     gHaveFlash = false, gHavePsram = false;
static TakeStream*  gPlay = nullptr;            // which set audioUpdate() reads
static const char* gPlayName = "none";

static SdFs sd;
static Wav  gWav;
struct Loaded { const int16_t* data; uint32_t frames; };
static Loaded gLoaded[TAKE_COUNT];

// Written by the audio task, read by loop(). Same reasoning as DEBUG_ENGINE in
// main.cpp: a torn read costs one wrong line, a lock would cost the audio.
static volatile uint32_t gBlocks = 0, gRenderSum = 0, gRenderWorst = 0;
static volatile int32_t  gPeak = 0;

// Every result line is also kept, and reprinted every 15s from loop(): USB CDC
// drops whatever is printed before the host opens the port, and the boot is
// exactly when the numbers that matter are printed.
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
         "partitions_agbau.csv\n");
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

static bool loadSdKit() {
  size_t freeBefore    = ESP.getFreePsram();
  size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  note("B: PSRAM free %u, largest block %u\n",
       (unsigned)freeBefore, (unsigned)largestBefore);

  uint32_t t0 = millis();
  if (!gWav.initSD(sd, SD_CS, SD_SCLK, SD_MISO, SD_MOSI, SPIKE_SD_MHZ)) {
    note("B: SD mount failed - no card, or not wired (docs/circuit.md)\n");
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
    gLoaded[i] = {slice, frames};
    off += (bytes[i] + 3) & ~3u;
  }
  uint32_t tLoad = millis() - t0;
  note("B: %d takes, %u bytes: mount %lu ms + load %lu ms at %d MHz (%.2f MB/s)\n",
       TAKE_COUNT, (unsigned)total, (unsigned long)tMount, (unsigned long)tLoad,
       SPIKE_SD_MHZ, tLoad ? (double)total / 1000.0 / tLoad : 0.0);
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

// --- The render, same shape as the firmware's: voice-outer, frame-inner ----

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

void setup() {
  Serial.begin(115200);
  // A measurement nobody sees is not one. Wait for the host, but not forever.
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

  if (gHaveFlash) benchRender("A flash", gFlash);
  if (gHavePsram) benchRender("B psram", gPsram);

  if (gHaveFlash && !(SPIKE_PLAY_PSRAM && gHavePsram)) {
    gPlay = gFlash;  gPlayName = "flash";
  } else if (gHavePsram) {
    gPlay = gPsram;  gPlayName = "psram";
  }
  note("Playing: %s. Internal heap free %u.\n", gPlayName,
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

  seti2sPins(16, 17, 18, -1);
  setIsDualCore(false);       // one task, one static block - see main.cpp
  audioStart();
}

void loop() {
  static uint32_t lastReport = 0, lastSummary = 0, lastBlocks = 0;
  uint32_t now = millis();
  if (now - lastReport >= 1000) {
    uint32_t blocks = gBlocks, sum = gRenderSum, worst = gRenderWorst;
    int32_t  peak = gPeak;
    gRenderSum = 0;
    gRenderWorst = 0;
    gPeak = 0;
    uint32_t n = blocks - lastBlocks;
    uint32_t period = blockPeriodCycles();
    Serial.printf("LIVE %s  blocks/s %lu  render avg %lu worst %lu cycles "
                  "(%.1f%% worst)  peak %ld\n", gPlayName, (unsigned long)n,
                  (unsigned long)(n ? sum / n : 0), (unsigned long)worst,
                  100.0 * worst / period, (long)peak);
    lastBlocks = blocks;
    lastReport = now;
  }
  if (now - lastSummary >= 15000) {
    Serial.print("--- boot summary ---\n");
    Serial.print(gSummary);
    lastSummary = now;
  }
  delay(5);
}

void audioUpdate() {
  static AudioBlock blk;   // one task only - setIsDualCore(false) above

  uint32_t c0 = ESP.getCycleCount();
  if (gPlay) renderStreams(gPlay, blk);
  else       audioBlockSilence(blk);
  uint32_t c = ESP.getCycleCount() - c0;
  gRenderSum = gRenderSum + c;
  if (c > gRenderWorst) gRenderWorst = c;

  for (int n = 0; n < AUDIO_BLOCK_FRAMES; n++) {
    int16_t outL = audioToI16(audioSoftClip(blk.l[n]));
    int16_t outR = audioToI16(audioSoftClip(blk.r[n]));
    int32_t mag = outL < 0 ? -(int32_t)outL : (int32_t)outL;
    if (mag > gPeak) gPeak = mag;
    audioBlockWrite(outL, outR);
  }
  gBlocks = gBlocks + 1;
}
