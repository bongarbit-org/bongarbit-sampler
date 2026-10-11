/*
 * Host tests for the Agbau voices, the request path into them, and the kit
 * table: agbau/src/voice.h, agbau/src/note_queue.h, agbau/src/agbau_kit.h.
 *
 * Three groups, by reason:
 *  - Still the Agbau. A voice is a port of the Bela's Sampler, held frame by
 *    frame to the verbatim copy in test/agbau/bela_reference.h; the kit table
 *    is held to transcriptions of render.cpp and agbau.ino. If these fail, the
 *    instrument on this board is not the one that was being played.
 *  - The engine's contracts: block-size invariance, voices that sum, a
 *    sustained note that is bit-exact.
 *  - The request path: loop() never touches a voice, so the queue between
 *    them has to deliver every event, in order, across two threads.
 */
#include <unity.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <thread>
#include "voice.h"
#include "agbau_kit.h"
#include "bela_reference.h"

#define SR 44100.0f

void setUp(void) {}
void tearDown(void) {}

typedef std::vector<int16_t> Take;

// A take that looks like audio and is never constant: a decaying two-tone
// waveform. Deterministic, so both sides of a comparison read the same thing.
static Take makeTake(uint32_t frames, float freq) {
  Take t(frames);
  for (uint32_t i = 0; i < frames; i++) {
    float env = expf(-(float)i / (float)frames * 3.0f);
    float s = 0.7f * sinf(6.2831853f * freq * i / SR) +
              0.2f * sinf(6.2831853f * freq * 2.7f * i / SR);
    t[i] = (int16_t)lrintf(s * env * 32767.0f);
  }
  return t;
}

// A take whose every frame is the same value: the voice's output is then the
// envelope itself, scaled, and can be read back exactly.
static Take flatTake(uint32_t frames, int16_t v) { return Take(frames, v); }

static std::vector<float> asBela(const Take& t) {
  std::vector<float> f(t.size());
  for (size_t i = 0; i < t.size(); i++) f[i] = audioFromI16(t[i]);
  return f;
}

static float renderOne(Voice& v) {
  float l = 0.0f, r = 0.0f;
  v.render(&l, &r, 1);
  return l;
}

// ---- Still the Agbau: the voice -----------------------------------------

struct Ev { uint32_t frame; bool on; };

static float worstAgainstBela(const VoiceConfig& c, const std::vector<Take>& takes,
                              const Ev* evs, int nev, uint32_t frames) {
  Voice v;
  v.configure(c, SR);
  v.seed(1234);
  BelaSampler b;
  b.setAdsrParameters(c.attack, c.decay, c.sustain, c.release);
  b.loopMode = c.loop;
  b.releaseOnNoteOff = c.releaseOnNoteOff;
  for (const Take& t : takes) {
    v.addTake(t.data(), (uint32_t)t.size());
    b.buffers.push_back(asBela(t));
  }
  int k = 0;
  float worst = 0.0f;
  for (uint32_t n = 0; n < frames; n++) {
    while (k < nev && evs[k].frame == n) {
      if (evs[k].on) {
        v.noteOn();
        b.trigger(v.take());           // same take on both sides
      } else {
        v.noteOff();
        b.noteOff();
      }
      k++;
    }
    float d = fabsf(renderOne(v) - b.process());
    if (d > worst) worst = d;
  }
  return worst;
}

// Every preset through the cases that make the Bela's behaviour visible: a
// take that runs out while the key is still held, a percussion take that
// ends mid-decay and freezes the envelope, a re-hit from that frozen level,
// a roll across random takes, a ganza looping through release and re-hit.
// The bound is the envelope's (2e-3, Bela's accumulated drift) at gain 0.5.
void test_voice_matches_the_bela_sampler(void) {
  const float bound = 1e-3f;

  std::vector<Take> bass = {makeTake(30000, 110.0f)};
  Ev heldPastTheEnd[] = {{0, true}, {40000, true}, {50000, false}};
  TEST_ASSERT_TRUE(worstAgainstBela(AGBAU_PRESETS[AGBAU_BASS], bass,
                                    heldPastTheEnd, 3, 50000 + 44100 + 100) < bound);
  std::vector<Take> longBass = {makeTake(100000, 82.0f)};
  Ev released[] = {{0, true}, {20000, false}};
  TEST_ASSERT_TRUE(worstAgainstBela(AGBAU_PRESETS[AGBAU_BASS], longBass,
                                    released, 2, 100100) < bound);

  std::vector<Take> shortHit = {makeTake(30000, 300.0f)};
  Ev frozen[] = {{0, true}, {35000, true}, {36000, false}};
  TEST_ASSERT_TRUE(worstAgainstBela(AGBAU_PRESETS[AGBAU_PERC], shortHit,
                                    frozen, 3, 35000 + 30000 + 100) < bound);
  std::vector<Take> five;
  for (int i = 0; i < 5; i++) five.push_back(makeTake(20000 + 3000 * i, 200.0f + 40 * i));
  Ev roll[] = {{0, true}, {3000, true}, {6000, true}, {9000, true}, {9500, false}};
  TEST_ASSERT_TRUE(worstAgainstBela(AGBAU_PRESETS[AGBAU_PERC], five,
                                    roll, 5, 9000 + 40000) < bound);

  std::vector<Take> ganza = {makeTake(5000, 900.0f)};
  Ev shake[] = {{0, true}, {22050, false}, {30000, true}, {40000, false}};
  TEST_ASSERT_TRUE(worstAgainstBela(AGBAU_PRESETS[AGBAU_GANZA], ganza,
                                    shake, 4, 40000 + 4410 + 100) < bound);
}

// The Bela returns before advancing the envelope once a take has run out, so
// the envelope freezes there, and the next attack climbs from that level
// instead of from zero. A port that kept advancing it would soften every
// re-hit of a short percussion take.
void test_short_take_freezes_the_envelope_for_the_next_attack(void) {
  Take t = flatTake(20000, 16384);                 // audioFromI16 = 0.5
  Voice v;
  v.configure(AGBAU_PRESETS[AGBAU_PERC], SR);
  v.addTake(t.data(), (uint32_t)t.size());
  v.noteOn();
  for (int n = 0; n < 20000; n++) renderOne(v);
  TEST_ASSERT_FALSE(v.playing());
  float frozen = v.envelope().level();
  TEST_ASSERT_TRUE(frozen > 0.8f && frozen < 0.9f); // mid-decay, not zero
  for (int n = 0; n < 5000; n++) renderOne(v);       // time passes; nothing moves
  TEST_ASSERT_EQUAL_FLOAT(frozen, v.envelope().level());

  v.noteOn();
  float first = renderOne(v) / (VOICE_GAIN * 0.5f);
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, frozen + (1.0f - frozen) / 441.0f, first);
}

// ---- Behaviour of each kind of voice -------------------------------------

// A zero-attack config makes the gain 1 from the first frame, so the output
// reads back the take's own frames: they must run 0..99, 0..99, ... with no
// frame skipped or repeated at the wrap.
void test_loop_wraps_without_dropping_a_frame(void) {
  Take t(100);
  for (int i = 0; i < 100; i++) t[i] = (int16_t)(100 * i);
  Voice v;
  v.configure(VoiceConfig{0.0f, 0.0f, 1.0f, 0.1f, true, true}, SR);
  v.addTake(t.data(), 100);
  v.noteOn();
  for (int n = 0; n < 350; n++) {
    float out = renderOne(v);
    TEST_ASSERT_EQUAL_FLOAT(VOICE_GAIN * audioFromI16(t[n % 100]), out);
  }
  TEST_ASSERT_TRUE(v.playing());
}

void test_one_shot_stops_at_the_end_of_its_take(void) {
  Take t = flatTake(1000, 8000);
  Voice v;
  v.configure(AGBAU_PRESETS[AGBAU_PERC], SR);
  v.addTake(t.data(), 1000);
  v.noteOn();
  for (int n = 0; n < 1000; n++) TEST_ASSERT_TRUE(renderOne(v) > 0.0f);
  TEST_ASSERT_FALSE(v.playing());
  for (int n = 0; n < 100; n++) TEST_ASSERT_EQUAL_FLOAT(0.0f, renderOne(v));
}

// The percussion's decay is the whole hit: lifting the key changes nothing.
void test_percussion_ignores_note_off(void) {
  Take t = makeTake(60000, 250.0f);
  Voice held, tapped;
  held.configure(AGBAU_PRESETS[AGBAU_PERC], SR);
  tapped.configure(AGBAU_PRESETS[AGBAU_PERC], SR);
  held.addTake(t.data(), 60000);
  tapped.addTake(t.data(), 60000);
  held.noteOn();
  tapped.noteOn();
  for (int n = 0; n < 60000; n++) {
    if (n == 300) tapped.noteOff();
    float a = renderOne(held), b = renderOne(tapped);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(float));
  }
}

// A ganza loops for as long as it is held, then falls silent over exactly
// 0.1s and stops being rendered - where the Bela kept looping it at zero
// gain forever.
void test_ganza_stops_a_tenth_of_a_second_after_note_off(void) {
  Take t = flatTake(3000, 16384);
  Voice v;
  v.configure(AGBAU_PRESETS[AGBAU_GANZA], SR);
  v.addTake(t.data(), 3000);
  v.noteOn();
  for (int n = 0; n < 20000; n++) TEST_ASSERT_TRUE(renderOne(v) > 0.0f);
  v.noteOff();
  for (int n = 0; n < 4410 - 1; n++) TEST_ASSERT_TRUE(renderOne(v) > 0.0f);
  TEST_ASSERT_EQUAL_FLOAT(0.0f, renderOne(v));
  TEST_ASSERT_FALSE(v.playing());
}

// A held bass multiplies by exactly 1.0f: the sustained note is the take at
// half gain, bit for bit.
void test_sustained_bass_is_bit_exact(void) {
  Take t = makeTake(30000, 98.0f);
  Voice v;
  v.configure(AGBAU_PRESETS[AGBAU_BASS], SR);
  v.addTake(t.data(), 30000);
  v.noteOn();
  for (int n = 0; n < 441; n++) renderOne(v);
  for (int n = 441; n < 30000; n++) {
    float out = renderOne(v);
    float want = VOICE_GAIN * audioFromI16(t[n]);
    TEST_ASSERT_EQUAL_MEMORY(&want, &out, sizeof(float));
  }
}

// Seeded, every take reachable, and the same seed gives the same choices.
void test_take_choice_is_seeded_and_reaches_every_take(void) {
  Take t = flatTake(10, 1);
  Voice a, b;
  for (Voice* v : {&a, &b}) {
    v->configure(AGBAU_PRESETS[AGBAU_PERC], SR);
    for (int i = 0; i < 5; i++) v->addTake(t.data(), 10);
    v->seed(42);
  }
  int seen[5] = {0};
  for (int i = 0; i < 1000; i++) {
    a.noteOn();
    b.noteOn();
    TEST_ASSERT_EQUAL_UINT8(a.take(), b.take());
    seen[a.take()]++;
  }
  for (int i = 0; i < 5; i++) TEST_ASSERT_TRUE(seen[i] > 100);   // ~200 each
}

void test_voice_without_takes_stays_silent(void) {
  Voice v;
  v.configure(AGBAU_PRESETS[AGBAU_BASS], SR);
  v.noteOn();
  TEST_ASSERT_FALSE(v.playing());
  TEST_ASSERT_EQUAL_FLOAT(0.0f, renderOne(v));
}

// ---- The engine's contracts ----------------------------------------------

// Pads play concurrently: two voices add into the same block.
void test_voices_sum_rather_than_replace(void) {
  Take t = flatTake(1000, 8192);
  VoiceBank<2> bank;
  for (Voice& v : bank.voices) {
    v.configure(VoiceConfig{0.0f, 0.0f, 1.0f, 0.1f, false, true}, SR);
    v.addTake(t.data(), 1000);
  }
  bank.post(0, true);
  bank.post(1, true);
  AudioBlock b;
  audioBlockSilence(b);
  bank.render(b);
  TEST_ASSERT_EQUAL_FLOAT(2.0f * VOICE_GAIN * audioFromI16(8192), b.l[0]);
  TEST_ASSERT_EQUAL_FLOAT(b.l[5], b.r[5]);            // mono, both sides
}

// The output is a function of the frames and the events, not of the size of
// the blocks the host asked for. Events land between chunks in both runs.
void test_block_size_does_not_change_the_output(void) {
  const uint32_t N = 120000;
  static float oneL[N], oneR[N], manyL[N], manyR[N];
  static Take takes[3] = {makeTake(30000, 110.0f), makeTake(40000, 300.0f),
                          makeTake(7000, 900.0f)};
  const AgbauGroup groups[3] = {AGBAU_BASS, AGBAU_PERC, AGBAU_GANZA};
  struct E { uint32_t frame; uint8_t voice; bool on; };
  const E script[] = {{0, 0, true}, {0, 2, true}, {5000, 1, true}, {20000, 0, false},
                      {26000, 1, true}, {50000, 2, false}, {60000, 0, true}};
  const int nev = 7;

  VoiceBank<3> a, b;
  for (int v = 0; v < 3; v++) {
    for (VoiceBank<3>* bank : {&a, &b}) {
      bank->voices[v].configure(AGBAU_PRESETS[groups[v]], SR);
      bank->voices[v].addTake(takes[v].data(), (uint32_t)takes[v].size());
      bank->voices[v].seed(7);
    }
  }
  memset(oneL, 0, sizeof(oneL)); memset(oneR, 0, sizeof(oneR));
  memset(manyL, 0, sizeof(manyL)); memset(manyR, 0, sizeof(manyR));

  const int sizes[] = {1, 7, 32, 3, 64, 13, 128, 5};
  int s = 0, k = 0;
  uint32_t off = 0;
  while (off < N) {
    uint32_t next = N;
    while (k < nev && script[k].frame == off) {
      a.post(script[k].voice, script[k].on);
      b.post(script[k].voice, script[k].on);
      k++;
    }
    if (k < nev) next = script[k].frame;
    a.render(oneL + off, oneR + off, (int)(next - off));     // one call per stretch
    uint32_t o = off;
    while (o < next) {
      uint32_t chunk = (uint32_t)sizes[s++ % 8];
      if (o + chunk > next) chunk = next - o;
      b.render(manyL + o, manyR + o, (int)chunk);
      o += chunk;
    }
    off = next;
  }
  for (uint32_t i = 0; i < N; i++) {
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&oneL[i], &manyL[i], sizeof(float),
        "voice output depends on the host block size");
  }
}

// ---- The request path ------------------------------------------------------

void test_requests_wait_for_the_next_block(void) {
  Take t = flatTake(1000, 8192);
  VoiceBank<1> bank;
  bank.voices[0].configure(VoiceConfig{0.0f, 0.0f, 1.0f, 0.1f, false, true}, SR);
  bank.voices[0].addTake(t.data(), 1000);
  bank.post(0, true);
  TEST_ASSERT_FALSE(bank.voices[0].playing());    // posted, not yet applied
  AudioBlock b;
  audioBlockSilence(b);
  bank.render(b);
  TEST_ASSERT_TRUE(bank.voices[0].playing());
  TEST_ASSERT_TRUE(b.l[0] > 0.0f);                // from the block's first frame
}

// A note-on and its note-off posted between the same two blocks must apply
// as a trigger and then a release. Reversed, a ganza would be left looping
// with no note-off ever coming. (The matrix cannot produce this - a release
// needs 3ms of continuous HIGH - but the looper will post events too.)
void test_on_then_off_in_one_block_is_applied_in_order(void) {
  Take t = flatTake(3000, 16384);
  VoiceBank<1> bank;
  bank.voices[0].configure(AGBAU_PRESETS[AGBAU_GANZA], SR);
  bank.voices[0].addTake(t.data(), 3000);
  bank.post(0, true);
  bank.post(0, false);
  AudioBlock b;
  for (int i = 0; i < 4; i++) {
    audioBlockSilence(b);
    bank.render(b);
  }
  TEST_ASSERT_FALSE(bank.voices[0].playing());
  TEST_ASSERT_TRUE(bank.voices[0].envelope().silent());

  // The same two events the other way round leave it looping: order is the
  // whole difference, which is why the queue keeps it.
  VoiceBank<1> reversed;
  reversed.voices[0].configure(AGBAU_PRESETS[AGBAU_GANZA], SR);
  reversed.voices[0].addTake(t.data(), 3000);
  reversed.post(0, false);
  reversed.post(0, true);
  for (int i = 0; i < 4; i++) {
    audioBlockSilence(b);
    reversed.render(b);
  }
  TEST_ASSERT_TRUE(reversed.voices[0].playing());
}

void test_out_of_range_voice_is_refused_at_the_door(void) {
  VoiceBank<4> bank;
  TEST_ASSERT_FALSE(bank.post(4, true));
  TEST_ASSERT_FALSE(bank.post(255, true));
  TEST_ASSERT_TRUE(bank.post(3, true));
}

// A full queue drops the newest event and counts it - never blocks loop(),
// never overwrites an event the audio task has not read.
void test_full_queue_drops_and_counts(void) {
  NoteQueue q;
  for (int i = 0; i < NOTE_QUEUE_SIZE; i++) TEST_ASSERT_TRUE(q.post((uint8_t)i, true));
  TEST_ASSERT_FALSE(q.post(99, true));
  TEST_ASSERT_EQUAL_UINT32(1, q.dropped());
  NoteEvent e;
  for (int i = 0; i < NOTE_QUEUE_SIZE; i++) {
    TEST_ASSERT_TRUE(q.take(e));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)i, e.voice);
  }
  TEST_ASSERT_FALSE(q.take(e));
}

// Two real threads, as on the board's two cores: every event arrives, once,
// in order, across many wraps of the ring. On x86 this cannot prove the
// memory ordering an Xtensa needs - the release/acquire pair is what does
// that - but it does catch any slip in the index arithmetic under contention.
void test_queue_delivers_everything_in_order_across_threads(void) {
  static NoteQueue q;
  const uint32_t total = 200000;
  std::thread producer([&] {
    for (uint32_t i = 0; i < total; i++) {
      while (!q.post((uint8_t)(i & 0x7F), (i & 1) != 0)) std::this_thread::yield();
    }
  });
  uint32_t got = 0;
  bool inOrder = true;
  NoteEvent e;
  while (got < total) {
    if (q.take(e)) {
      if (e.voice != (uint8_t)(got & 0x7F) || e.on != ((got & 1) != 0)) inOrder = false;
      got++;
    } else {
      std::this_thread::yield();
    }
  }
  producer.join();
  TEST_ASSERT_TRUE(inOrder);
  TEST_ASSERT_FALSE(q.take(e));
}

// ---- Still the Agbau: the kit ----------------------------------------------

// agbau.ino:24-35, verbatim: the Keypad kcode (row * 4 + col) of each key, as
// the button number the Arduino printed and the MIDI note it sent.
static const int     ARDUINO_KEY_ORDER[16] = {1, 5, 9, 13, 2, 6, 10, 14,
                                              3, 7, 11, 15, 4, 8, 12, 16};
static const uint8_t ARDUINO_NOTA_MIDI[16] = {60, 64, 68, 72, 61, 65, 69, 73,
                                              62, 66, 70, 74, 63, 67, 71, 75};

void test_kit_matches_the_arduino_note_map(void) {
  int hits[AGBAU_VOICES] = {0};
  for (uint8_t row = 0; row < AGBAU_ROWS; row++) {
    for (uint8_t col = 0; col < AGBAU_COLS; col++) {
      int kcode = row * 4 + col;
      uint8_t voice = agbauVoiceForKey(row, col);
      // The Bela played note 60 + i on samplers[i].
      TEST_ASSERT_EQUAL_UINT8(ARDUINO_NOTA_MIDI[kcode] - 60, voice);
      // The Arduino's own split: button numbers above 8 were percussion.
      bool arduinoPerc = ARDUINO_KEY_ORDER[kcode] > 8;
      TEST_ASSERT_EQUAL(arduinoPerc, AGBAU_VOICE_DEFS[voice].group != AGBAU_BASS);
      hits[voice]++;
    }
  }
  for (int v = 0; v < AGBAU_VOICES; v++) TEST_ASSERT_EQUAL_INT(1, hits[v]);
  // The ganzas, where agbau/docs/migration.md says they are.
  TEST_ASSERT_EQUAL(AGBAU_GANZA, AGBAU_VOICE_DEFS[agbauVoiceForKey(2, 2)].group);
  TEST_ASSERT_EQUAL(AGBAU_GANZA, AGBAU_VOICE_DEFS[agbauVoiceForKey(2, 3)].group);
}

// render.cpp:36-56 (file lists) and 84-104 (presets), transcribed.
void test_kit_matches_the_bela_presets(void) {
  const char* bela[16][5] = {
    {"b1"}, {"b2"}, {"b3"}, {"b4"}, {"b5"}, {"b6"}, {"b7"}, {"b8"},
    {"p1.3.1", "p1.3.2", "p1.3.3", "p1.3.4", "p1.3.5"},
    {"p2.2.1"}, {"p3.1"}, {"p4.3.1"},
    {"p5.3.1", "p5.3.2", "p5.3.3"},
    {"p6.3.1", "p6.3.2", "p6.3.3"},
    {"p7.1"}, {"p8.3.1"},
  };
  for (int v = 0; v < 16; v++) {
    const AgbauVoiceDef& d = AGBAU_VOICE_DEFS[v];
    int n = 0;
    while (n < 5 && bela[v][n]) n++;
    TEST_ASSERT_EQUAL_INT(n, d.takeCount);
    for (int t = 0; t < n; t++) TEST_ASSERT_EQUAL_STRING(bela[v][t], d.takes[t]);

    // samplers[0..7]: (0.01, 0.0, 1.0, 1.0), release on note-off
    // samplers[8..15]: (0.01, 3.0, 0.0, 3.0), note-off ignored
    // samplers[8+2], [8+6]: (0.01, 0.0, 1.0, 0.1), loop, release on note-off
    bool ganza = v == 8 + 3 - 1 || v == 8 + 7 - 1;
    const VoiceConfig& c = AGBAU_PRESETS[d.group];
    if (v < 8) {
      TEST_ASSERT_EQUAL(AGBAU_BASS, d.group);
      TEST_ASSERT_EQUAL_FLOAT(1.0f, c.release);
      TEST_ASSERT_TRUE(c.releaseOnNoteOff && !c.loop);
    } else if (ganza) {
      TEST_ASSERT_EQUAL(AGBAU_GANZA, d.group);
      TEST_ASSERT_EQUAL_FLOAT(0.1f, c.release);
      TEST_ASSERT_TRUE(c.releaseOnNoteOff && c.loop);
    } else {
      TEST_ASSERT_EQUAL(AGBAU_PERC, d.group);
      TEST_ASSERT_EQUAL_FLOAT(3.0f, c.decay);
      TEST_ASSERT_EQUAL_FLOAT(0.0f, c.sustain);
      TEST_ASSERT_TRUE(!c.releaseOnNoteOff && !c.loop);
    }
    TEST_ASSERT_EQUAL_FLOAT(0.01f, c.attack);
  }
}

// Takes are bound by name from whatever storage was chosen; a missing one is
// counted, leaves its voice silent, and does not take the others down.
void test_configure_binds_takes_by_name_and_counts_missing(void) {
  static int16_t pcm[64];
  static VoiceBank<AGBAU_VOICES> bank;
  auto find = [](const char* name, VoiceTake* out) {
    if (strcmp(name, "p3.1") == 0) return false;          // pretend it is absent
    *out = VoiceTake{pcm, 64};
    return true;
  };
  TEST_ASSERT_EQUAL_INT(1, agbauConfigure(bank, find, SR, 99));
  TEST_ASSERT_EQUAL_UINT8(5, bank.voices[8].takeCount());
  TEST_ASSERT_EQUAL_UINT8(0, bank.voices[10].takeCount());
  bank.post(10, true);
  bank.post(0, true);
  AudioBlock b;
  audioBlockSilence(b);
  bank.render(b);
  TEST_ASSERT_FALSE(bank.voices[10].playing());
  TEST_ASSERT_TRUE(bank.voices[0].playing());
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_voice_matches_the_bela_sampler);
  RUN_TEST(test_short_take_freezes_the_envelope_for_the_next_attack);

  RUN_TEST(test_loop_wraps_without_dropping_a_frame);
  RUN_TEST(test_one_shot_stops_at_the_end_of_its_take);
  RUN_TEST(test_percussion_ignores_note_off);
  RUN_TEST(test_ganza_stops_a_tenth_of_a_second_after_note_off);
  RUN_TEST(test_sustained_bass_is_bit_exact);
  RUN_TEST(test_take_choice_is_seeded_and_reaches_every_take);
  RUN_TEST(test_voice_without_takes_stays_silent);

  RUN_TEST(test_voices_sum_rather_than_replace);
  RUN_TEST(test_block_size_does_not_change_the_output);

  RUN_TEST(test_requests_wait_for_the_next_block);
  RUN_TEST(test_on_then_off_in_one_block_is_applied_in_order);
  RUN_TEST(test_out_of_range_voice_is_refused_at_the_door);
  RUN_TEST(test_full_queue_drops_and_counts);
  RUN_TEST(test_queue_delivers_everything_in_order_across_threads);

  RUN_TEST(test_kit_matches_the_arduino_note_map);
  RUN_TEST(test_kit_matches_the_bela_presets);
  RUN_TEST(test_configure_binds_takes_by_name_and_counts_missing);

  return UNITY_END();
}
