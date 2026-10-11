#pragma once
/*
 * The Agbau kit as one image in a flash data partition: parse it, bounds-check
 * it, and hand out pointers into it.
 *
 * Compiled into the firmware AND into the host test suite, same contract as
 * every other header here. Nothing in it touches Arduino or ESP-IDF: it takes
 * a byte pointer and a size, so the firmware can pass the address
 * esp_partition_mmap() returned and the tests can pass a buffer they built.
 *
 * WHY AN IMAGE AT ALL
 *
 * 8,995 MB of int16 does not fit 8 MiB of PSRAM; it fits a 16MB flash with
 * room to spare (agbau/docs/migration.md, step 0). Mapped through the cache,
 * the kit is read in place - no copy, no SD, nothing loaded at boot - the same
 * way the sampler's USE_SD_SAMPLER=0 already plays a kick out of .rodata.
 *
 * The image is written by agbau/tools/agbau_kit.py --partition. The format is
 * documented there, and this parser is the other half of that definition:
 *
 *   0   char[4]  magic "AGBK"
 *   4   u16      version
 *   6   u16      take_count
 *   8   u32      crc32 (zlib) of [end of take table, image_bytes)
 *   12  u32      image_bytes
 *   16  take_count x { char name[24]; u32 offset; u32 frames; u32 sample_rate }
 *
 * NOTHING IN IT IS TRUSTED
 *
 * The partition is written by a separate esptool command, by hand, and can be
 * stale, truncated, from another kit version, or never written at all (erased
 * flash reads 0xFF). Every offset is checked against the image and every
 * length against its offset before a pointer is handed out, because the
 * consumer of these pointers is the audio task, and a bad one there is a read
 * past the end of the map - a crash in the middle of a performance, not an
 * error at boot. The CRC catches a partial write; the bounds catch everything
 * a valid CRC would still let through.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define KIT_IMAGE_VERSION    1
#define KIT_IMAGE_NAME_BYTES 24
#define KIT_IMAGE_HEADER     16
#define KIT_IMAGE_ENTRY      (KIT_IMAGE_NAME_BYTES + 12)

// 24 takes today. Room to grow, and a fixed array so parsing never allocates.
#ifndef KIT_IMAGE_MAX_TAKES
  #define KIT_IMAGE_MAX_TAKES 48
#endif

enum KitImageError {
  KIT_OK = 0,
  KIT_TOO_SMALL,          // shorter than its own header or take table
  KIT_BAD_MAGIC,          // not an image at all - erased flash reads 0xFF
  KIT_BAD_VERSION,        // written by a different version of the tool
  KIT_TOO_MANY_TAKES,
  KIT_SIZE_MISMATCH,      // image_bytes larger than the partition holding it
  KIT_NAME_UNTERMINATED,
  KIT_TAKE_EMPTY,
  KIT_TAKE_MISALIGNED,    // int16 data on an odd address
  KIT_TAKE_OUT_OF_BOUNDS,
  KIT_BAD_CRC,            // partial or corrupted write
};

static inline const char* kitImageErrorName(KitImageError e) {
  switch (e) {
    case KIT_OK:                 return "ok";
    case KIT_TOO_SMALL:          return "image smaller than its own header";
    case KIT_BAD_MAGIC:          return "no AGBK magic (partition never written?)";
    case KIT_BAD_VERSION:        return "image version differs from firmware";
    case KIT_TOO_MANY_TAKES:     return "more takes than KIT_IMAGE_MAX_TAKES";
    case KIT_SIZE_MISMATCH:      return "image larger than its partition";
    case KIT_NAME_UNTERMINATED:  return "take name not NUL-terminated";
    case KIT_TAKE_EMPTY:         return "take with zero frames";
    case KIT_TAKE_MISALIGNED:    return "take data on an odd offset";
    case KIT_TAKE_OUT_OF_BOUNDS: return "take runs past the end of the image";
    case KIT_BAD_CRC:            return "CRC mismatch (partial or stale write)";
  }
  return "unknown";
}

/*
 * CRC-32 as zlib computes it (reflected 0xEDB88320, init and final ~0), so
 * Python's zlib.crc32() on the PC and this on the board agree.
 *
 * Half-byte table: 64 bytes instead of 1KB, two lookups per byte. Over ~9MB at
 * boot that is the difference between a fraction of a second and a fraction
 * of a second slightly smaller - not worth a kilobyte of internal RAM.
 * Chainable: pass the previous result as `crc` to continue a running sum.
 */
static inline uint32_t kitCrc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static const uint32_t t[16] = {
    0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC,
    0x76DC4190, 0x6B6B51F4, 0x4DB26158, 0x5005713C,
    0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
    0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C,
  };
  crc = ~crc;
  for (size_t i = 0; i < n; i++) {
    crc = t[(crc ^ p[i]) & 0xF] ^ (crc >> 4);
    crc = t[(crc ^ (p[i] >> 4)) & 0xF] ^ (crc >> 4);
  }
  return ~crc;
}

// Little-endian reads by bytes: correct on any host, and no alignment demand
// on the mapped flash.
static inline uint16_t kitRd16(const uint8_t* p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t kitRd32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

struct KitTake {
  char           name[KIT_IMAGE_NAME_BYTES];
  const int16_t* data;
  uint32_t       frames;
  uint32_t       sampleRate;
};

struct KitImage {
  KitTake  takes[KIT_IMAGE_MAX_TAKES];
  uint16_t count = 0;
  uint32_t bytes = 0;     // image_bytes from the header
  uint32_t crc   = 0;     // as stored in the header

  /*
   * Fill `takes` from an image of at most `size` bytes. On any error the
   * object is left empty (count = 0), so a caller that ignores the return
   * value gets no takes rather than half-checked ones.
   *
   * checkCrc is optional because it reads every byte: the spike measures what
   * that costs at boot before anyone decides it is free. Bounds are always
   * checked - they read only the header and table.
   */
  KitImageError parse(const uint8_t* img, size_t size, bool checkCrc) {
    count = 0;
    KitImageError e = parseInto(img, size, checkCrc);
    if (e != KIT_OK) count = 0;
    return e;
  }

  // Header fields only, for a caller that has mapped just the first page and
  // needs image_bytes to know how much more to map.
  static KitImageError peekSize(const uint8_t* img, size_t size, uint32_t* bytes) {
    if (size < KIT_IMAGE_HEADER) return KIT_TOO_SMALL;
    if (memcmp(img, "AGBK", 4) != 0) return KIT_BAD_MAGIC;
    if (kitRd16(img + 4) != KIT_IMAGE_VERSION) return KIT_BAD_VERSION;
    *bytes = kitRd32(img + 12);
    return KIT_OK;
  }

  const KitTake* find(const char* name) const {
    for (uint16_t i = 0; i < count; i++)
      if (strncmp(takes[i].name, name, KIT_IMAGE_NAME_BYTES) == 0) return &takes[i];
    return nullptr;
  }

private:
  KitImageError parseInto(const uint8_t* img, size_t size, bool checkCrc) {
    uint32_t imageBytes = 0;
    KitImageError e = peekSize(img, size, &imageBytes);
    if (e != KIT_OK) return e;

    uint16_t n = kitRd16(img + 6);
    if (n > KIT_IMAGE_MAX_TAKES) return KIT_TOO_MANY_TAKES;
    if (imageBytes > size) return KIT_SIZE_MISMATCH;
    uint64_t tableEnd = KIT_IMAGE_HEADER + (uint64_t)n * KIT_IMAGE_ENTRY;
    if (tableEnd > imageBytes) return KIT_TOO_SMALL;

    for (uint16_t i = 0; i < n; i++) {
      const uint8_t* ent = img + KIT_IMAGE_HEADER + (size_t)i * KIT_IMAGE_ENTRY;
      if (memchr(ent, 0, KIT_IMAGE_NAME_BYTES) == nullptr) return KIT_NAME_UNTERMINATED;
      uint32_t off    = kitRd32(ent + KIT_IMAGE_NAME_BYTES);
      uint32_t frames = kitRd32(ent + KIT_IMAGE_NAME_BYTES + 4);
      uint32_t rate   = kitRd32(ent + KIT_IMAGE_NAME_BYTES + 8);
      if (frames == 0) return KIT_TAKE_EMPTY;
      if (off & 1) return KIT_TAKE_MISALIGNED;
      // 64-bit so a hostile frames value cannot wrap the sum back in bounds.
      if (off < tableEnd || (uint64_t)off + (uint64_t)frames * 2 > imageBytes)
        return KIT_TAKE_OUT_OF_BOUNDS;

      KitTake& t = takes[i];
      memcpy(t.name, ent, KIT_IMAGE_NAME_BYTES);
      t.data       = (const int16_t*)(img + off);
      t.frames     = frames;
      t.sampleRate = rate;
    }

    uint32_t stored = kitRd32(img + 8);
    if (checkCrc &&
        kitCrc32(img + tableEnd, (size_t)(imageBytes - tableEnd)) != stored)
      return KIT_BAD_CRC;

    count = n;
    bytes = imageBytes;
    crc   = stored;
    return KIT_OK;
  }
};
