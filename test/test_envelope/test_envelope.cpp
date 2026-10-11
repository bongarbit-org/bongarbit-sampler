/*
 * Host tests for the Agbau envelope: src/envelope.h.
 *
 * The envelope is a port, so the first thing pinned here is that it is still
 * the Bela's envelope: sim/bela_reference.h is a verbatim copy of the ADSR the
 * Agbau is played through today, and test_envelope_matches_the_bela_reference
 * holds the port to it frame by frame on all three presets - the same role
 * mixer.h plays for the soft knee. Everything the port changes on purpose is
 * then pinned by a test named after the change, so it cannot drift back.
 */
#include <unity.h>
#include <math.h>
#include <string.h>
#include "envelope.h"
#include "bela_reference.h"

#define SR 44100.0f

void setUp(void) {}
void tearDown(void) {}

// The three ways the Agbau uses one envelope class (render.cpp:84-104).
struct Preset { float a, d, s, r; };
static const Preset BASS  = {0.01f, 0.0f, 1.0f, 1.0f};
static const Preset PERC  = {0.01f, 3.0f, 0.0f, 3.0f};
static const Preset GANZA = {0.01f, 0.0f, 1.0f, 0.1f};

static void setPort(Envelope& e, const Preset& p) { e.set(p.a, p.d, p.s, p.r, SR); }
static void setBela(BelaADSR& b, const Preset& p) {
  b.setSampleRate(SR);
  b.setAttackTime(p.a);
  b.setDecayTime(p.d);
  b.setSustainLevel(p.s);
  b.setReleaseTime(p.r);
}

// A note's life as a script: trigger or release, each just before a frame.
enum Ev { TRIG, REL };
struct Event { uint32_t frame; Ev ev; };

static void apply(Envelope& e, Ev ev) { ev == TRIG ? e.trigger() : e.release(); }

// Worst |port - bela| over a script, and in which frames.
static float worstAgainstBela(const Preset& p, const Event* evs, int nev,
                              uint32_t frames, uint32_t from = 0, uint32_t to = 0xFFFFFFFF) {
  Envelope e;  setPort(e, p);
  BelaADSR b;  setBela(b, p);
  int k = 0;
  float worst = 0.0f;
  for (uint32_t n = 0; n < frames; n++) {
    while (k < nev && evs[k].frame == n) {
      apply(e, evs[k].ev);
      evs[k].ev == TRIG ? b.trigger() : b.release();
      k++;
    }
    float d = fabsf(e.next() - b.process());
    if (n >= from && n < to && d > worst) worst = d;
  }
  return worst;
}

// ---- Still the Bela's envelope -------------------------------------------

// Every preset, through holds, releases and retriggers at awkward moments.
// The bound is Bela's own drift, not the port's: Bela adds its step every
// frame and the rounding accumulates - 1.1e-3 by the end of a 3s decay,
// measured - while the port computes each value directly. 2e-3 is that drift
// with headroom, and still -54dB: inaudible on any sample.
void test_envelope_matches_the_bela_reference(void) {
  const float bound = 2e-3f;

  Event bassHold[] = {{0, TRIG}, {22050, REL}};
  TEST_ASSERT_TRUE(worstAgainstBela(BASS, bassHold, 2, 22050 + 44100 + 500) < bound);
  Event bassRetrig[] = {{0, TRIG}, {22050, REL}, {42050, TRIG}, {60000, REL}};
  TEST_ASSERT_TRUE(worstAgainstBela(BASS, bassRetrig, 4, 60000 + 44100 + 500) < bound);
  Event bassEarlyOff[] = {{0, TRIG}, {200, REL}};             // off mid-attack
  TEST_ASSERT_TRUE(worstAgainstBela(BASS, bassEarlyOff, 2, 44100 + 1000) < bound);

  Event percHit[] = {{0, TRIG}};
  TEST_ASSERT_TRUE(worstAgainstBela(PERC, percHit, 1, 140000) < bound);
  Event percRoll[] = {{0, TRIG}, {60000, TRIG}};              // re-hit mid-decay
  TEST_ASSERT_TRUE(worstAgainstBela(PERC, percRoll, 2, 200000) < bound);
  Event percShort[] = {{0, TRIG}, {70000, REL}};              // sample ran out
  TEST_ASSERT_TRUE(worstAgainstBela(PERC, percShort, 2, 70000 + 132300 + 500) < bound);

  Event ganza[] = {{0, TRIG}, {100000, REL}, {102000, TRIG}, {150000, REL}};
  TEST_ASSERT_TRUE(worstAgainstBela(GANZA, ganza, 4, 150000 + 5000) < bound);
}

// The bound above is loose enough to hide a one-frame shift on a long decay,
// so timing is pinned separately, where a shift cannot hide: across an attack
// one frame is 1/441 of full scale. Here the two agree to float rounding.
void test_attack_timing_matches_the_bela_to_the_frame(void) {
  Event hit[] = {{0, TRIG}};
  TEST_ASSERT_TRUE(worstAgainstBela(PERC, hit, 1, 441, 0, 441) < 1e-5f);
}

// And the end of the decay: Bela's level stops moving on the same frame the
// port's lands on zero.
void test_decay_ends_on_the_same_frame_as_the_bela(void) {
  Envelope e;  setPort(e, PERC);
  BelaADSR b;  setBela(b, PERC);
  e.trigger();
  b.trigger();
  float bPrev = 0.0f, ePrev = 0.0f;
  int32_t bStill = -1, eStill = -1;
  for (int32_t n = 0; n < 140000; n++) {
    float bv = b.process(), ev = e.next();
    if (bStill < 0 && n > 441 && bv == bPrev) bStill = n - 1;
    if (eStill < 0 && n > 441 && ev == ePrev) eStill = n - 1;
    bPrev = bv;
    ePrev = ev;
  }
  TEST_ASSERT_EQUAL_INT32(441 + 132300 - 1, eStill);
  TEST_ASSERT_EQUAL_INT32(eStill, bStill);
}

// ---- What the port changes on purpose ------------------------------------

// A held bass multiplies its sample by 1.0f, not by 1.0000049f as Bela does,
// so the sustained part of a note comes out bit-exact.
void test_sustain_at_unity_is_bit_exact(void) {
  Envelope e;  setPort(e, BASS);
  e.trigger();
  for (int n = 0; n < 441; n++) e.next();
  const float unity = 1.0f;
  for (int n = 0; n < 10000; n++) {
    float g = e.next();
    TEST_ASSERT_EQUAL_MEMORY(&unity, &g, sizeof(float));
  }
  TEST_ASSERT_EQUAL_INT(Envelope::Sustain, e.state());

}

// A finished percussion hit multiplies by exactly zero from frame 132740 on.
// Bela leaves -0.00108 there; the port's zero is what makes
// tools/agbau_kit.py --trim-envelope lossless, and the frame count is the
// tool's own arithmetic, pinned.
void test_percussion_is_silent_after_attack_plus_decay(void) {
  uint32_t end = Envelope::framesFor(PERC.a, SR) + Envelope::framesFor(PERC.d, SR);
  TEST_ASSERT_EQUAL_UINT32(132741, end);   // agbau_kit.py: 441 + 132300

  Envelope e;  setPort(e, PERC);
  e.trigger();
  for (uint32_t n = 0; n < end - 1; n++) {
    float g = e.next();
    TEST_ASSERT_TRUE_MESSAGE(g > 0.0f, "percussion gain hit zero before A+D");
  }
  const float zero = 0.0f;
  for (uint32_t n = end - 1; n < end + 50000; n++) {
    float g = e.next();
    TEST_ASSERT_EQUAL_MEMORY(&zero, &g, sizeof(float));
  }
  TEST_ASSERT_TRUE(e.silent());
}

// A zero-time segment arrives on the next frame. Bela's rampTo(x, 0) never
// moves at all. No Agbau preset can tell - its only zero-time segments decay
// to the level the attack ended on - so this is pinned on a made-up one.
void test_zero_time_segment_jumps_to_its_target(void) {
  Envelope e;
  e.set(0.01f, 0.0f, 0.5f, 0.0f, SR);
  e.trigger();
  for (int n = 0; n < 440; n++) e.next();
  TEST_ASSERT_EQUAL_FLOAT(1.0f, e.next());      // the attack still peaks
  TEST_ASSERT_EQUAL_FLOAT(0.5f, e.next());      // then the decay is just done
  e.release();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, e.next());      // zero release: next frame
  TEST_ASSERT_EQUAL_INT(Envelope::Off, e.state());
}

// Every release must land on 0.0f exactly, from any level. silent() is what
// lets a voice stop - and a looping ganza only ever stops through it. With
// the step merely accumulated (or computed without setting the last frame to
// the target), a release from an arbitrary level ends a few 1e-9 off zero,
// silent() never fires, and a released ganza loops at inaudible gain forever,
// burning a voice's worth of CPU. That is exactly what it did on the Bela.
void test_every_release_ends_on_exact_zero(void) {
  const float zero = 0.0f;
  Envelope e;  setPort(e, GANZA);
  e.trigger();
  for (int n = 0; n < 1000; n++) e.next();
  for (int hold = 1; hold < 4410; hold += 37) {
    e.release();
    for (int n = 0; n < hold; n++) e.next();       // part-way down
    e.trigger();
    for (int n = 0; n < hold % 441; n++) e.next(); // part-way back up
    e.release();
    for (int n = 0; n < 4410; n++) e.next();
    float level = e.level();
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&zero, &level, sizeof(float),
        "release ended near zero, not on it");
    TEST_ASSERT_TRUE(e.silent());
  }
}

// ---- Bela semantics the port keeps -----------------------------------------

// trigger() ramps from where the level IS. Bela's trigger() looks like it
// resets to 0 first, but that rampTo is overwritten before a frame runs - so
// a re-hit mid-decay climbs from the decayed level, not from silence.
void test_retrigger_ramps_from_current_level(void) {
  Envelope e;  setPort(e, PERC);
  e.trigger();
  for (int n = 0; n < 20000; n++) e.next();
  float before = e.level();
  TEST_ASSERT_TRUE(before > 0.5f && before < 1.0f);
  e.trigger();
  float first = e.next();
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, before + (1.0f - before) / 441.0f, first);
}

// The two usage patterns the Agbau gets out of one class. Bass: holds at 1
// until released, then falls to zero over 1s. Ganza: same hold, 0.1s fall.
void test_release_falls_from_the_current_level_over_its_time(void) {
  Envelope bass;  setPort(bass, BASS);
  bass.trigger();
  for (int n = 0; n < 5000; n++) bass.next();
  bass.release();
  for (int n = 0; n < 44100 - 1; n++) TEST_ASSERT_TRUE(bass.next() > 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, bass.next());
  TEST_ASSERT_EQUAL_INT(Envelope::Off, bass.state());

  Envelope g;  setPort(g, GANZA);
  g.trigger();
  for (int n = 0; n < 5000; n++) g.next();
  g.release();
  for (int n = 0; n < 4410 - 1; n++) g.next();
  TEST_ASSERT_EQUAL_FLOAT(0.0f, g.next());
  TEST_ASSERT_TRUE(g.silent());
}

// Releasing before the attack finishes falls from the partial level, not from
// 1 - a very short tap on a bass does not jump up before fading.
void test_release_mid_attack_never_rises(void) {
  Envelope e;  setPort(e, BASS);
  e.trigger();
  for (int n = 0; n < 100; n++) e.next();
  float at = e.level();
  e.release();
  float prev = at;
  for (int n = 0; n < 44100; n++) {
    float g = e.next();
    TEST_ASSERT_TRUE(g <= prev);
    prev = g;
  }
}

// ---- The engine's contracts ----------------------------------------------

// Same promise as the pitch shifter's: the output is a function of the
// frames, not of the block size the host happened to use. Events land between
// chunks in both runs, at the same frames.
void test_block_size_does_not_change_the_output(void) {
  const uint32_t N = 60000;
  static float one[N], many[N];
  const Event script[] = {{0, TRIG}, {9000, TRIG}, {21000, REL}, {30000, TRIG}};
  const int nev = 4;

  Envelope a;  setPort(a, BASS);
  for (int k = 0; k < nev; k++) {
    uint32_t from = script[k].frame;
    uint32_t to = k + 1 < nev ? script[k + 1].frame : N;
    apply(a, script[k].ev);
    a.render(one + from, (int)(to - from));       // one call per stretch
  }

  Envelope b;  setPort(b, BASS);
  const int sizes[] = {1, 7, 32, 3, 64, 13, 128, 5};
  int s = 0;
  for (int k = 0; k < nev; k++) {
    uint32_t off = script[k].frame;
    uint32_t to = k + 1 < nev ? script[k + 1].frame : N;
    apply(b, script[k].ev);
    while (off < to) {
      uint32_t chunk = (uint32_t)sizes[s++ % 8];
      if (off + chunk > to) chunk = to - off;
      b.render(many + off, (int)chunk);
      off += chunk;
    }
  }

  for (uint32_t i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&one[i], &many[i], sizeof(float),
        "envelope output depends on the host block size");
  }
}

// silent() is what lets a voice stop reading its sample early. It must never
// be true while the envelope is still going to produce something.
void test_silent_only_when_it_will_stay_silent(void) {
  Envelope e;  setPort(e, PERC);
  TEST_ASSERT_TRUE(e.silent());                 // never triggered
  e.trigger();
  TEST_ASSERT_FALSE(e.silent());                // level 0, but about to rise
  for (int n = 0; n < 132740; n++) {
    e.next();
    TEST_ASSERT_FALSE(e.silent());
  }
  e.next();
  TEST_ASSERT_TRUE(e.silent());                 // sustain of 0
  e.release();
  TEST_ASSERT_TRUE(e.silent());                 // releasing from 0 stays 0
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_envelope_matches_the_bela_reference);
  RUN_TEST(test_attack_timing_matches_the_bela_to_the_frame);
  RUN_TEST(test_decay_ends_on_the_same_frame_as_the_bela);

  RUN_TEST(test_sustain_at_unity_is_bit_exact);
  RUN_TEST(test_percussion_is_silent_after_attack_plus_decay);
  RUN_TEST(test_zero_time_segment_jumps_to_its_target);
  RUN_TEST(test_every_release_ends_on_exact_zero);

  RUN_TEST(test_retrigger_ramps_from_current_level);
  RUN_TEST(test_release_falls_from_the_current_level_over_its_time);
  RUN_TEST(test_release_mid_attack_never_rises);

  RUN_TEST(test_block_size_does_not_change_the_output);
  RUN_TEST(test_silent_only_when_it_will_stay_silent);

  return UNITY_END();
}
