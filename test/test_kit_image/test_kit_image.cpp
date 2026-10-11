/*
 * Host tests for the flash kit image parser: src/kit_image.h.
 *
 * The pointers this parser hands out are read by the audio task, every frame,
 * for the rest of the performance. So most of what is pinned here is refusal:
 * every way a hand-flashed partition can be wrong has to become an error at
 * boot, never an address past the end of the map.
 *
 * The images are built here byte by byte, to the format tools/agbau_kit.py
 * documents. That the tool's real output parses is checked separately, on the
 * real 9MB image, and recorded in the commit that added this file - a
 * fixture that size does not belong in the repository.
 */
#include <unity.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include "kit_image.h"

void setUp(void) {}
void tearDown(void) {}

// ---- A builder that writes what agbau_kit.py writes ------------------------

struct SrcTake {
  const char*          name;
  std::vector<int16_t> pcm;
};

static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = v & 0xFF; b[at + 1] = v >> 8;
}
static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (v >> (8 * i)) & 0xFF;
}

static std::vector<uint8_t> build(const std::vector<SrcTake>& takes) {
  size_t tableEnd = KIT_IMAGE_HEADER + takes.size() * KIT_IMAGE_ENTRY;
  std::vector<size_t> offs;
  size_t off = tableEnd;
  for (const SrcTake& t : takes) {
    off = (off + 3) & ~(size_t)3;
    offs.push_back(off);
    off += t.pcm.size() * 2;
  }
  std::vector<uint8_t> b(off, 0);
  memcpy(&b[0], "AGBK", 4);
  put16(b, 4, KIT_IMAGE_VERSION);
  put16(b, 6, (uint16_t)takes.size());
  put32(b, 12, (uint32_t)b.size());
  for (size_t i = 0; i < takes.size(); i++) {
    size_t e = KIT_IMAGE_HEADER + i * KIT_IMAGE_ENTRY;
    strncpy((char*)&b[e], takes[i].name, KIT_IMAGE_NAME_BYTES - 1);
    put32(b, e + KIT_IMAGE_NAME_BYTES, (uint32_t)offs[i]);
    put32(b, e + KIT_IMAGE_NAME_BYTES + 4, (uint32_t)takes[i].pcm.size());
    put32(b, e + KIT_IMAGE_NAME_BYTES + 8, 44100);
    for (size_t k = 0; k < takes[i].pcm.size(); k++)
      put16(b, offs[i] + 2 * k, (uint16_t)takes[i].pcm[k]);
  }
  put32(b, 8, kitCrc32(&b[tableEnd], b.size() - tableEnd));
  return b;
}

static std::vector<SrcTake> twoTakes() {
  return {{"b1", {1, -2, 3, -32768, 32767}}, {"p1.3.1", {100, 200, 300}}};
}

// Field offsets of take i's entry, for corrupting one thing at a time.
static size_t entry(size_t i) { return KIT_IMAGE_HEADER + i * KIT_IMAGE_ENTRY; }
static size_t offField(size_t i)    { return entry(i) + KIT_IMAGE_NAME_BYTES; }
static size_t framesField(size_t i) { return entry(i) + KIT_IMAGE_NAME_BYTES + 4; }

static KitImage gKit;

// ---- The CRC has to be zlib's, or the PC and the board disagree ----------

// The standard check value for CRC-32/ISO-HDLC, which is what Python's
// zlib.crc32() computes. If this fails, every image the tool writes is
// rejected as corrupt.
void test_crc_matches_zlib_check_value(void) {
  const uint8_t msg[] = "123456789";
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, kitCrc32(msg, 9));
}

// Chaining lets a CRC be computed in pieces - one mapped window at a time - and
// must give the same answer as one pass.
void test_crc_can_be_computed_in_pieces(void) {
  const uint8_t msg[] = "123456789";
  uint32_t part = kitCrc32(msg, 4);
  TEST_ASSERT_EQUAL_HEX32(0xCBF43926, kitCrc32(msg + 4, 5, part));
}

// ---- A good image ------------------------------------------------------

void test_good_image_parses_and_points_at_the_samples(void) {
  std::vector<uint8_t> img = build(twoTakes());
  TEST_ASSERT_EQUAL_INT(KIT_OK, gKit.parse(img.data(), img.size(), true));
  TEST_ASSERT_EQUAL_UINT16(2, gKit.count);

  const KitTake* b1 = gKit.find("b1");
  TEST_ASSERT_NOT_NULL(b1);
  TEST_ASSERT_EQUAL_UINT32(5, b1->frames);
  TEST_ASSERT_EQUAL_UINT32(44100, b1->sampleRate);
  TEST_ASSERT_EQUAL_INT16(-32768, b1->data[3]);
  TEST_ASSERT_EQUAL_INT16(32767, b1->data[4]);

  const KitTake* p = gKit.find("p1.3.1");
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_INT16(300, p->data[2]);
  // Points INTO the image, never a copy: in flash that is the whole point.
  TEST_ASSERT_TRUE((const uint8_t*)p->data > img.data());
  TEST_ASSERT_TRUE((const uint8_t*)(p->data + p->frames) <= img.data() + img.size());
}

void test_unknown_take_is_not_found(void) {
  std::vector<uint8_t> img = build(twoTakes());
  gKit.parse(img.data(), img.size(), true);
  TEST_ASSERT_NULL(gKit.find("b9"));
  TEST_ASSERT_NULL(gKit.find("p1.3"));   // a prefix of a real name is not it
}

// A partition is larger than the image in it; the slack is never read.
void test_image_smaller_than_its_partition_is_fine(void) {
  std::vector<uint8_t> img = build(twoTakes());
  size_t real = img.size();
  img.resize(real + 4096, 0xFF);           // erased flash past the image
  TEST_ASSERT_EQUAL_INT(KIT_OK, gKit.parse(img.data(), img.size(), true));
  TEST_ASSERT_EQUAL_UINT32(real, gKit.bytes);
}

void test_peek_size_reads_only_the_header(void) {
  std::vector<uint8_t> img = build(twoTakes());
  uint32_t bytes = 0;
  TEST_ASSERT_EQUAL_INT(KIT_OK, KitImage::peekSize(img.data(), KIT_IMAGE_HEADER, &bytes));
  TEST_ASSERT_EQUAL_UINT32(img.size(), bytes);
}

// ---- Everything a hand-flashed partition can be ------------------------

// Never written: erased NOR flash reads 0xFF everywhere.
void test_erased_partition_is_rejected(void) {
  std::vector<uint8_t> img(65536, 0xFF);
  TEST_ASSERT_EQUAL_INT(KIT_BAD_MAGIC, gKit.parse(img.data(), img.size(), true));
  TEST_ASSERT_EQUAL_UINT16(0, gKit.count);
}

void test_other_version_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put16(img, 4, KIT_IMAGE_VERSION + 1);
  TEST_ASSERT_EQUAL_INT(KIT_BAD_VERSION, gKit.parse(img.data(), img.size(), true));
}

void test_truncated_header_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  TEST_ASSERT_EQUAL_INT(KIT_TOO_SMALL, gKit.parse(img.data(), 10, true));
}

// The image claims more bytes than the partition it sits in - flashed into a
// partition table it was not built for.
void test_image_larger_than_partition_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  TEST_ASSERT_EQUAL_INT(KIT_SIZE_MISMATCH,
                        gKit.parse(img.data(), img.size() - 1, true));
}

// A partial write: the header and table landed, the end of the data did not.
// Bounds alone cannot see this - only the CRC can.
void test_partial_write_is_caught_by_the_crc(void) {
  std::vector<uint8_t> img = build(twoTakes());
  img[img.size() - 1] ^= 0x01;
  TEST_ASSERT_EQUAL_INT(KIT_BAD_CRC, gKit.parse(img.data(), img.size(), true));
  TEST_ASSERT_EQUAL_UINT16(0, gKit.count);   // no half-checked takes left behind
  // ...and the bounds were fine, which is why the CRC exists at all.
  TEST_ASSERT_EQUAL_INT(KIT_OK, gKit.parse(img.data(), img.size(), false));
}

void test_take_past_the_end_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put32(img, framesField(1), 4);           // one frame more than is there
  TEST_ASSERT_EQUAL_INT(KIT_TAKE_OUT_OF_BOUNDS,
                        gKit.parse(img.data(), img.size(), false));
}

// A frame count chosen so offset + 2*frames wraps 32 bits back into range.
// Done in 32-bit arithmetic this would pass, and hand the audio task a take
// two gigabytes long.
void test_frame_count_cannot_wrap_back_in_bounds(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put32(img, framesField(0), 0x80000000u);
  TEST_ASSERT_EQUAL_INT(KIT_TAKE_OUT_OF_BOUNDS,
                        gKit.parse(img.data(), img.size(), false));
}

void test_take_inside_the_table_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put32(img, offField(0), KIT_IMAGE_HEADER);
  TEST_ASSERT_EQUAL_INT(KIT_TAKE_OUT_OF_BOUNDS,
                        gKit.parse(img.data(), img.size(), false));
}

void test_odd_offset_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  uint32_t off = img[offField(0)] | (img[offField(0) + 1] << 8);
  put32(img, offField(0), off + 1);
  TEST_ASSERT_EQUAL_INT(KIT_TAKE_MISALIGNED,
                        gKit.parse(img.data(), img.size(), false));
}

void test_empty_take_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put32(img, framesField(0), 0);
  TEST_ASSERT_EQUAL_INT(KIT_TAKE_EMPTY, gKit.parse(img.data(), img.size(), false));
}

void test_unterminated_name_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  memset(&img[entry(0)], 'x', KIT_IMAGE_NAME_BYTES);
  TEST_ASSERT_EQUAL_INT(KIT_NAME_UNTERMINATED,
                        gKit.parse(img.data(), img.size(), false));
}

void test_too_many_takes_is_rejected(void) {
  std::vector<uint8_t> img = build(twoTakes());
  put16(img, 6, KIT_IMAGE_MAX_TAKES + 1);
  TEST_ASSERT_EQUAL_INT(KIT_TOO_MANY_TAKES,
                        gKit.parse(img.data(), img.size(), false));
}

// A failed parse after a good one must not leave the good one's takes visible.
void test_failed_parse_clears_a_previous_good_one(void) {
  std::vector<uint8_t> good = build(twoTakes());
  TEST_ASSERT_EQUAL_INT(KIT_OK, gKit.parse(good.data(), good.size(), true));
  std::vector<uint8_t> bad(1024, 0xFF);
  gKit.parse(bad.data(), bad.size(), true);
  TEST_ASSERT_EQUAL_UINT16(0, gKit.count);
  TEST_ASSERT_NULL(gKit.find("b1"));
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_crc_matches_zlib_check_value);
  RUN_TEST(test_crc_can_be_computed_in_pieces);

  RUN_TEST(test_good_image_parses_and_points_at_the_samples);
  RUN_TEST(test_unknown_take_is_not_found);
  RUN_TEST(test_image_smaller_than_its_partition_is_fine);
  RUN_TEST(test_peek_size_reads_only_the_header);

  RUN_TEST(test_erased_partition_is_rejected);
  RUN_TEST(test_other_version_is_rejected);
  RUN_TEST(test_truncated_header_is_rejected);
  RUN_TEST(test_image_larger_than_partition_is_rejected);
  RUN_TEST(test_partial_write_is_caught_by_the_crc);
  RUN_TEST(test_take_past_the_end_is_rejected);
  RUN_TEST(test_frame_count_cannot_wrap_back_in_bounds);
  RUN_TEST(test_take_inside_the_table_is_rejected);
  RUN_TEST(test_odd_offset_is_rejected);
  RUN_TEST(test_empty_take_is_rejected);
  RUN_TEST(test_unterminated_name_is_rejected);
  RUN_TEST(test_too_many_takes_is_rejected);
  RUN_TEST(test_failed_parse_clears_a_previous_good_one);

  return UNITY_END();
}
