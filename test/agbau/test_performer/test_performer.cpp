/*
 * Host tests for the Agbau's repeat and looper: agbau/src/tap_tempo.h,
 * agbau/src/looper.h and agbau/src/performer.h, against agbau/docs/looper.md.
 *
 * Three groups, by reason:
 *  - Still the Agbau. The spec keeps agbau.ino's feel on purpose, including
 *    things nobody wrote on purpose (a re-press within a subdivision keeps
 *    the grid). These pin that feel.
 *  - The .ino's bugs, one test each, named after what broke. Reintroducing
 *    one fails a test that says which.
 *  - Time. Everything is counted in frames so it cannot drift against the
 *    sound; an hour of repeat and a hundred laps of loop say so, and the
 *    schedule is shown not to depend on the block size.
 */
#include <unity.h>
#include <algorithm>
#include <memory>
#include <vector>
#include "performer.h"

#define SR 44100.0f
#define SEC 44100u

void setUp(void) {}
void tearDown(void) {}

// Voices used below, from agbau_kit.h: b1 bass, p1/p2 percussion, p3 ganza.
#define B1 0
#define B3 2
#define P1 8
#define P2 9
#define P3 10
#define MODE  PERF_KEY_MODE
#define PEDAL PERF_KEY_PEDAL

struct Ev {
  uint32_t at;       // start of the block it was applied in
  uint8_t  v;
  bool     on;
  uint32_t target;   // the frame it belongs to
};

// The firmware's loop, on the host: blocks of a given size (or a ragged
// cycle of sizes), and key edges that land at the start of a given block.
struct Rig {
  Performer             p;
  std::vector<Ev>       out;
  std::vector<uint32_t> chunks;
  size_t                ci = 0;
  uint32_t              t = 0;

  explicit Rig(std::vector<uint32_t> c = {32}) : chunks(c) { p.init(SR); }

  void block(uint32_t n) {
    uint32_t at = t;
    p.process(n, [&](uint8_t v, bool on, uint32_t target) {
      out.push_back(Ev{at, v, on, target});
    });
    t += n;
  }
  // Run until the next block would start at `frame`. A ragged cycle is cut
  // short there, so an edge posted next lands at exactly `frame` whatever
  // the block size.
  void runTo(uint32_t frame) {
    while (t < frame) {
      uint32_t n = chunks[ci++ % chunks.size()];
      if (t + n > frame) n = frame - t;
      block(n);
    }
  }
  void key(uint32_t frame, uint8_t k, bool down) {
    runTo(frame);
    p.post(k, down);
  }
  void tap(uint32_t frame) {
    key(frame, MODE, true);
    key(frame + 3000, MODE, false);
  }
  void press(uint32_t frame, uint8_t k) { key(frame, k, true); }
  void lift(uint32_t frame, uint8_t k) { key(frame, k, false); }

  std::vector<Ev> of(uint8_t v, bool on, uint32_t from = 0) const {
    std::vector<Ev> r;
    for (const Ev& e : out)
      if (e.v == v && e.on == on && e.at >= from) r.push_back(e);
    return r;
  }
};

static std::unique_ptr<Rig> rig(std::vector<uint32_t> c = {32}) {
  return std::unique_ptr<Rig>(new Rig(c));
}

// Two taps half a second apart: repeat on at 120 bpm, D = 4, so one hit per
// beat (22050 frames). Returns the frame of the second tap.
static uint32_t enterRepeat(Rig& r, uint32_t at) {
  r.tap(at);
  r.tap(at + SEC / 2);
  return at + SEC / 2;
}

// ---- Still the Agbau: tap tempo and repeat ---------------------------------

void test_first_tap_after_boot_keeps_120(void) {
  auto r = rig();
  r->tap(10 * SEC);
  r->runTo(10 * SEC + 5000);
  TEST_ASSERT_TRUE(r->p.repeatOn());
  TEST_ASSERT_EQUAL_UINT32(22050, r->p.beat());
}

void test_a_slow_tap_is_still_a_tempo(void) {
  // No range, as on the .ino: a lone tap 30s after the last sets a 30s beat.
  auto r = rig();
  r->tap(0);
  r->tap(30 * SEC);
  r->runTo(30 * SEC + 5000);
  TEST_ASSERT_EQUAL_UINT32(30 * SEC, r->p.beat());
}

void test_repeat_hits_at_once_after_a_pause(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0) + 2 * SEC;
  r->press(t, P1);
  r->runTo(t + 4 * 22050 + 1);
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_EQUAL(5, (int)on.size());
  for (int k = 0; k < 5; k++) TEST_ASSERT_EQUAL_UINT32(t + k * 22050, on[k].target);
}

void test_a_repress_within_a_subdivision_keeps_the_grid(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0) + 2 * SEC;
  r->press(t, P1);                      // hits at t, t+22050
  r->lift(t + 30000, P1);
  r->press(t + 35000, P1);              // within 22050 of the last hit
  r->runTo(t + 44100 + 1);
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_EQUAL(3, (int)on.size());
  TEST_ASSERT_EQUAL_UINT32(t + 44100, on[2].target);   // the old grid, not t+35000
}

void test_first_hit_after_a_tap_waits_for_the_tap_grid(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0);
  r->press(t + 1000, P1);
  r->runTo(t + 22050 + 1);
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_EQUAL(1, (int)on.size());
  TEST_ASSERT_EQUAL_UINT32(t + 22050, on[0].target);
}

void test_bass_in_repeat_sets_the_division_and_is_silent(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0) + 2 * SEC;
  TEST_ASSERT_EQUAL_UINT8(4, r->p.division());
  r->press(t, B1);
  r->lift(t + 1000, B1);
  TEST_ASSERT_EQUAL_UINT8(2, r->p.division());
  r->press(t + 2 * SEC, P1);
  r->runTo(t + 2 * SEC + 2 * 44100 + 1);
  TEST_ASSERT_EQUAL(0, (int)r->of(B1, true).size());
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_EQUAL(3, (int)on.size());
  TEST_ASSERT_EQUAL_UINT32(on[0].target + 44100, on[1].target);   // half notes
}

void test_bass_in_note_mode_sounds(void) {
  auto r = rig();
  r->press(3200, B3);
  r->lift(6400, B3);
  r->runTo(9600);
  TEST_ASSERT_EQUAL(1, (int)r->of(B3, true).size());
  TEST_ASSERT_EQUAL(1, (int)r->of(B3, false).size());
}

void test_holding_mode_one_second_leaves_repeat(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0) + SEC;
  r->key(t, MODE, true);
  r->runTo(t + SEC - 1);
  r->block(1);
  TEST_ASSERT_TRUE(r->p.repeatOn());
  r->block(1);
  TEST_ASSERT_FALSE(r->p.repeatOn());
}

void test_mode_led_lights_the_first_half_of_each_beat(void) {
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0);
  r->runTo(t + 100);
  TEST_ASSERT_TRUE(r->p.modeLed());
  r->runTo(t + 11025 + 100);
  TEST_ASSERT_FALSE(r->p.modeLed());
  r->runTo(t + 22050 + 100);
  TEST_ASSERT_TRUE(r->p.modeLed());
}

// ---- The .ino's bugs -------------------------------------------------------

void test_tempo_at_120_is_22050_frames(void) {
  // Bug 1: (1000 / bpm) * 60 in integers made 120 bpm a 480ms beat, and put
  // every tempo from 84 to 90 bpm on 660ms. Here the tap is the tempo.
  auto r = rig({1});
  r->tap(SEC);
  r->tap(SEC + 22050);
  r->runTo(SEC + 22050 + 1);
  TEST_ASSERT_EQUAL_UINT32(22050, r->p.beat());
  r->tap(SEC + 22050 + 30120);           // 683ms, from the bench (bancada section 15)
  r->runTo(SEC + 22050 + 30120 + 1);
  TEST_ASSERT_EQUAL_UINT32(30120, r->p.beat());
}

void test_a_long_gap_starts_over_instead_of_dividing_by_zero(void) {
  // Bug 2: 60s or more between taps made the .ino's bpm 0, then divided by it.
  auto r = rig();
  r->tap(0);
  r->tap(26432);
  r->runTo(30000);
  TEST_ASSERT_EQUAL_UINT32(26432, r->p.beat());
  r->tap(26432 + 61 * SEC);
  r->runTo(26432 + 61 * SEC + 5000);
  TEST_ASSERT_EQUAL_UINT32(26432, r->p.beat());
  r->tap(26432 + 61 * SEC + 17632);      // and the next tap measures from it
  r->runTo(26432 + 61 * SEC + 17632 + 5000);
  TEST_ASSERT_EQUAL_UINT32(17632, r->p.beat());
}

void test_repeats_the_most_recent_key_still_held(void) {
  // Bug 6: the .ino kept repeating the last key pressed after it was let go.
  auto r = rig({1});
  uint32_t t = enterRepeat(*r, 0) + 2 * SEC;
  r->press(t, P1);
  r->press(t + 1000, P2);
  r->runTo(t + 22050 + 1);
  TEST_ASSERT_EQUAL(1, (int)r->of(P2, true).size());   // the newer one
  r->lift(t + 30000, P2);
  r->runTo(t + 44100 + 1);
  std::vector<Ev> p1 = r->of(P1, true);
  TEST_ASSERT_EQUAL(2, (int)p1.size());
  TEST_ASSERT_EQUAL_UINT32(t + 44100, p1[1].target);   // back to the one still held
}

void test_a_key_is_released_in_the_mode_it_was_pressed(void) {
  // Bugs 5 and 7: a ganza held across the switch into repeat never got its
  // note-off, and the .ino's counter of held keys went negative.
  auto r = rig();
  r->press(3200, P3);
  enterRepeat(*r, 6400);
  r->lift(SEC * 2, P3);
  r->runTo(SEC * 2 + 64);
  TEST_ASSERT_EQUAL(1, (int)r->of(P3, false).size());

  // The other way: a ganza repeating when repeat is left is released then,
  // and its key's own release later changes nothing.
  auto s = rig();
  uint32_t t = enterRepeat(*s, 0) + 2 * SEC;
  s->press(t, P3);
  s->key(t + SEC, MODE, true);           // hold to leave
  s->runTo(t + 2 * SEC + 64);
  TEST_ASSERT_FALSE(s->p.repeatOn());
  size_t offs = s->of(P3, false).size();
  TEST_ASSERT_TRUE(offs >= 1);
  s->lift(t + 3 * SEC, P3);
  s->runTo(t + 3 * SEC + 64);
  TEST_ASSERT_EQUAL(offs, s->of(P3, false).size());
  TEST_ASSERT_TRUE(s->out.back().v == P3 && !s->out.back().on);  // silent since
}

// A one-note loop of one second: P1 at `pos` frames into it.
static uint32_t recordOneNote(Rig& r, uint32_t start, uint32_t pos, uint32_t len) {
  r.key(start, PEDAL, true);
  r.key(start + 3200, PEDAL, false);
  r.press(start + pos, P1);
  r.lift(start + pos + 3200, P1);
  r.key(start + len, PEDAL, true);
  r.key(start + len + 3200, PEDAL, false);
  return start + len;                    // where the loop comes round
}

void test_overdub_does_not_record_the_playback(void) {
  // Bug 4: the .ino recorded through the noteOn() it played back with.
  auto r = rig();
  uint32_t round = recordOneNote(*r, 32000, 6400, SEC);
  r->key(round + 640, PEDAL, true);      // overdub, a whole lap of playback
  r->key(round + 3840, PEDAL, false);
  r->runTo(round + SEC + 3200);
  TEST_ASSERT_EQUAL(LOOPER_PLAYING, r->p.looper().state());
  TEST_ASSERT_EQUAL(2, r->p.looper().layers());
  TEST_ASSERT_EQUAL(0, r->p.looper().events(1));
}

void test_a_fourth_overdub_is_refused(void) {
  // Bug 3: the .ino let a fourth layer open and wrote past its array. Here
  // there are four - the base and three overdubs - and no fifth.
  auto r = rig();
  uint32_t round = recordOneNote(*r, 0, 6400, SEC);
  for (int i = 0; i < 4; i++) {
    uint32_t lap = round + i * SEC;
    r->key(lap + 3200, PEDAL, true);
    r->key(lap + 6400, PEDAL, false);
  }
  r->runTo(round + 5 * SEC);
  TEST_ASSERT_EQUAL(LOOPER_PLAYING, r->p.looper().state());
  TEST_ASSERT_EQUAL(LOOPER_LAYERS, r->p.looper().layers());
}

void test_closing_a_layer_closes_its_held_notes(void) {
  // Bug 8: a ganza held while the take closed looped forever.
  auto r = rig();
  r->key(0, PEDAL, true);
  r->key(3200, PEDAL, false);
  r->press(6400, P3);
  r->key(SEC, PEDAL, true);              // closed with the ganza still held
  r->key(SEC + 3200, PEDAL, false);
  r->lift(SEC + 32000, P3);              // after the close: not recorded
  r->runTo(3 * SEC + 64);
  // Each lap: on at 6400, off at the lap's last frame.
  std::vector<Ev> on = r->of(P3, true, SEC + 1), off = r->of(P3, false, SEC + 32001);
  TEST_ASSERT_EQUAL(2, (int)on.size());
  TEST_ASSERT_EQUAL(2, (int)off.size());
  TEST_ASSERT_EQUAL_UINT32(SEC + 6400, on[0].target);
  TEST_ASSERT_EQUAL_UINT32(2 * SEC - 1, off[0].target);
}

void test_erase_releases_every_voice_the_loop_started(void) {
  // Bug 9: erasing left on whatever the loop had turned on.
  auto r = rig();
  r->key(0, PEDAL, true);
  r->key(3200, PEDAL, false);
  r->press(6400, P3);
  r->lift(40000, P3);
  r->key(SEC, PEDAL, true);
  r->key(SEC + 3200, PEDAL, false);
  uint32_t t = 2 * SEC + 10048;          // the loop's ganza is on here
  r->key(t, PEDAL, true);                // press (opens an overdub), then hold
  r->runTo(t + SEC + 64);
  r->lift(t + SEC + 64, PEDAL);
  TEST_ASSERT_EQUAL(LOOPER_EMPTY, r->p.looper().state());
  const Ev& last = r->out.back();
  TEST_ASSERT_EQUAL_UINT8(P3, last.v);
  TEST_ASSERT_FALSE(last.on);
  TEST_ASSERT_TRUE(last.at >= t + SEC && last.at - (t + SEC) < 32);  // the hold's block
  size_t n = r->out.size();
  r->runTo(t + 4 * SEC);
  TEST_ASSERT_EQUAL(n, r->out.size());   // and nothing plays after
}

void test_a_full_layer_closes_with_room_for_its_note_offs(void) {
  auto r = rig();
  r->key(0, PEDAL, true);
  r->key(3200, PEDAL, false);
  r->press(6400, P3);                    // held through the close
  uint32_t t = 9600;
  int presses = 0;
  while (r->p.looper().state() == LOOPER_RECORDING) {
    r->press(t, P1);
    r->lift(t + 32, P1);
    t += 64;
    presses++;
  }
  TEST_ASSERT_EQUAL_UINT32(1, r->p.looper().fullCloses());
  TEST_ASSERT_TRUE(r->p.looper().events(0) <= LOOPER_CAPACITY);
  // The ganza's note-off was written: the playback turns it off each lap.
  r->runTo(t + 2 * r->p.looper().length());
  TEST_ASSERT_TRUE(r->of(P3, false).size() >= 2);
  TEST_ASSERT_TRUE(presses > LOOPER_CAPACITY / 2 - LOOPER_RESERVE);
}

// ---- Still the Agbau: the looper -------------------------------------------

void test_the_first_frame_plays_as_the_loop_comes_round(void) {
  auto r = rig();
  r->key(6400, PEDAL, true);
  r->press(6400, P1);                    // same block as the pedal
  r->lift(9600, P1);
  r->key(9600, PEDAL, false);
  r->key(6400 + SEC, PEDAL, true);
  r->key(9600 + SEC, PEDAL, false);
  r->runTo(6400 + 2 * SEC + 64);
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_EQUAL(3, (int)on.size());  // live, then each time round
  TEST_ASSERT_EQUAL_UINT32(6400 + SEC, on[1].target);
  TEST_ASSERT_EQUAL_UINT32(6400 + SEC, on[1].at);
}

void test_overdub_closes_at_the_loop_start(void) {
  // As the .ino meant: opened at 3/4 of the loop, it records the last
  // quarter and closes by itself.
  auto r = rig();
  uint32_t round = recordOneNote(*r, 0, 6400, SEC);
  uint32_t open = round + 3 * SEC / 4;
  r->key(open, PEDAL, true);
  r->key(open + 3200, PEDAL, false);
  r->press(open + 6400, P2);
  r->lift(open + 9600, P2);
  r->runTo(round + SEC - 64);
  TEST_ASSERT_EQUAL(LOOPER_OVERDUB, r->p.looper().state());
  r->runTo(round + SEC + 64);
  TEST_ASSERT_EQUAL(LOOPER_PLAYING, r->p.looper().state());
  TEST_ASSERT_EQUAL(2, r->p.looper().layers());
  r->runTo(round + 2 * SEC + 64);
  std::vector<Ev> p2 = r->of(P2, true);
  TEST_ASSERT_EQUAL(2, (int)p2.size());
  TEST_ASSERT_EQUAL_UINT32(p2[0].target + SEC, p2[1].target);
}

void test_looper_led_blinks_twice_a_lap_from_the_start(void) {
  auto r = rig();
  uint32_t round = recordOneNote(*r, 0, 6400, 4 * 32000);
  bool lit[4];
  for (int q = 0; q < 4; q++) {
    r->runTo(round + q * 32000 + 1000);
    lit[q] = r->p.looperLed();
  }
  TEST_ASSERT_TRUE(lit[0]);
  TEST_ASSERT_FALSE(lit[1]);
  TEST_ASSERT_TRUE(lit[2]);
  TEST_ASSERT_FALSE(lit[3]);
}

// ---- Time ------------------------------------------------------------------

void test_repeat_grid_does_not_drift(void) {
  // An hour at D = 3 on an odd beat: 4P/3 is not a whole number of frames.
  auto r = rig();
  r->tap(0);
  r->tap(22051);
  uint32_t t = 22051 + 4 * SEC;
  r->press(t - 64, B1 + 1);              // b2: D = 3
  r->lift(t - 32, B1 + 1);
  r->press(t, P1);
  r->runTo(t + 3600 * SEC);
  std::vector<Ev> on = r->of(P1, true);
  TEST_ASSERT_TRUE(on.size() > 5000);
  for (size_t k = 0; k < on.size(); k++) {
    uint32_t want = t + (uint32_t)((uint64_t)k * 4 * 22051 / 3);
    TEST_ASSERT_EQUAL_UINT32(want, on[k].target);
    TEST_ASSERT_TRUE(on[k].at >= want && on[k].at - want < 32);
  }
}

void test_loop_does_not_drift(void) {
  auto r = rig();
  const uint32_t len = 3 * SEC + 17;     // not a multiple of the block
  uint32_t round = recordOneNote(*r, 0, 7000, len);
  r->runTo(round + 100 * len + 64);
  std::vector<Ev> on = r->of(P1, true, round);
  TEST_ASSERT_EQUAL(100, (int)on.size());
  for (int lap = 0; lap < 100; lap++) {
    uint32_t want = round + lap * len + 7000;
    TEST_ASSERT_EQUAL_UINT32(want, on[lap].target);
    TEST_ASSERT_TRUE(on[lap].at - want < 32);
  }
}

// The same performance - taps, a division change, a note-mode ganza, a take,
// an overdub, repeat hits recorded into it - through ragged blocks and
// through blocks of 32. Edges land on the same frames in both (Rig::runTo),
// so every event must belong to the same frame; only how late it is applied,
// under one block, may differ.
static std::vector<Ev> perform(std::vector<uint32_t> chunks) {
  auto r = rig(chunks);
  r->key(1000, PEDAL, true);
  r->key(4000, PEDAL, false);
  r->press(9000, P3);
  r->lift(70011, P3);
  r->tap(80000);
  r->tap(80000 + 23111);
  r->press(150000, B1 + 3);              // D = 6
  r->lift(152000, B1 + 3);
  r->press(160003, P1);
  r->press(190000, P2);
  r->lift(230000, P2);
  r->lift(260000, P1);
  r->key(300000, PEDAL, true);
  r->key(303000, PEDAL, false);
  r->key(320000, PEDAL, true);           // overdub
  r->key(323000, PEDAL, false);
  r->press(330000, P1);
  r->runTo(1200000);
  return r->out;
}

void test_block_size_does_not_change_the_schedule(void) {
  std::vector<Ev> ref = perform({32});
  std::vector<Ev> rag = perform({1, 7, 32, 3, 64, 13, 128, 5});
  // Two sources (the repeat, the loop) can share a block in one run and not
  // in the other; ordered by the frame they belong to, the lists must agree.
  auto byTarget = [](const Ev& a, const Ev& b) { return a.target < b.target; };
  std::stable_sort(ref.begin(), ref.end(), byTarget);
  std::stable_sort(rag.begin(), rag.end(), byTarget);
  TEST_ASSERT_TRUE(ref.size() > 20);
  TEST_ASSERT_EQUAL(ref.size(), rag.size());
  for (size_t i = 0; i < ref.size(); i++) {
    TEST_ASSERT_EQUAL_UINT8(ref[i].v, rag[i].v);
    TEST_ASSERT_EQUAL(ref[i].on, rag[i].on);
    TEST_ASSERT_EQUAL_UINT32(ref[i].target, rag[i].target);
    TEST_ASSERT_TRUE(rag[i].at - rag[i].target < 128);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_tap_after_boot_keeps_120);
  RUN_TEST(test_a_slow_tap_is_still_a_tempo);
  RUN_TEST(test_repeat_hits_at_once_after_a_pause);
  RUN_TEST(test_a_repress_within_a_subdivision_keeps_the_grid);
  RUN_TEST(test_first_hit_after_a_tap_waits_for_the_tap_grid);
  RUN_TEST(test_bass_in_repeat_sets_the_division_and_is_silent);
  RUN_TEST(test_bass_in_note_mode_sounds);
  RUN_TEST(test_holding_mode_one_second_leaves_repeat);
  RUN_TEST(test_mode_led_lights_the_first_half_of_each_beat);

  RUN_TEST(test_tempo_at_120_is_22050_frames);
  RUN_TEST(test_a_long_gap_starts_over_instead_of_dividing_by_zero);
  RUN_TEST(test_repeats_the_most_recent_key_still_held);
  RUN_TEST(test_a_key_is_released_in_the_mode_it_was_pressed);
  RUN_TEST(test_overdub_does_not_record_the_playback);
  RUN_TEST(test_a_fourth_overdub_is_refused);
  RUN_TEST(test_closing_a_layer_closes_its_held_notes);
  RUN_TEST(test_erase_releases_every_voice_the_loop_started);
  RUN_TEST(test_a_full_layer_closes_with_room_for_its_note_offs);

  RUN_TEST(test_the_first_frame_plays_as_the_loop_comes_round);
  RUN_TEST(test_overdub_closes_at_the_loop_start);
  RUN_TEST(test_looper_led_blinks_twice_a_lap_from_the_start);

  RUN_TEST(test_repeat_grid_does_not_drift);
  RUN_TEST(test_loop_does_not_drift);
  RUN_TEST(test_block_size_does_not_change_the_schedule);
  return UNITY_END();
}
