/*
 * Host tests for the Agbau key matrix: src/key_matrix.h.
 *
 * The scan code runs here against a simulated 4x4 matrix that models the
 * three things that make a real one go wrong: the diodes (or their absence),
 * the direction of the scan, and how long a row line takes to climb back to
 * HIGH through a weak pull-up. Each is a test named after the failure it
 * would cause. Key presses come from sim/trigger_model.h's bouncing pin
 * traces, so the matrix is held to the same contact model as the sampler's
 * buttons - and to the same harness, which is how the sweep period is pinned.
 */
#include <unity.h>
#include <math.h>
#include <string.h>
#include "trigger_model.h"
#include "key_matrix.h"

#define R 4
#define C 4

void setUp(void) {}
void tearDown(void) {}

/*
 * The simulated board. Keys close either from a pin trace (bouncing, timed)
 * or from a fixed `held` set. Electrically: a pressed key connects its row to
 * its column through a diode that only conducts row -> column; the driven
 * column is LOW, the others float, rows have pull-ups.
 */
struct SimMatrix {
  const Trace* trace[R][C] = {};
  bool         held[R][C]  = {};
  bool         diodes      = true;
  bool         transposed  = false;   // wired the wrong way round
  double       tauRise_us  = 0.0;     // row line: pull-up x capacitance
  uint32_t     settle_us   = KEY_MATRIX_SETTLE_US;

  uint64_t clock_us = 0;
  int      driven   = -1;
  bool     recovering[R] = {};
  uint64_t releasedAt[R] = {};

  bool closed(int r, int c) const {
    return held[r][c] || (trace[r][c] && levelAt(*trace[r][c], clock_us) == PIN_LOW);
  }

  // Is row r pulled to the driven column's LOW right now?
  bool pulledLow(int r) const {
    if (driven < 0) return false;
    if (transposed) {
      // The code drives what are really rows and reads what are really
      // columns: every diode is reversed and nothing conducts. Without
      // diodes the matrix is symmetric and the transposed scan would work.
      return diodes ? false : connected(driven, r);
    }
    if (diodes) return closed(r, driven);
    return connected(r, driven);
  }

  // Without diodes current flows both ways through any closed key: a row
  // reaches a column through any chain of closed keys.
  bool connected(int row, int col) const {
    bool rowSeen[R] = {}, colSeen[C] = {};
    int  rq[R * C], n = 0;
    rq[n++] = row;
    rowSeen[row] = true;
    for (int i = 0; i < n; i++) {
      for (int c = 0; c < C; c++) {
        if (!closed(rq[i], c) || colSeen[c]) continue;
        if (c == col) return true;
        colSeen[c] = true;
        for (int r = 0; r < R; r++)
          if (!rowSeen[r] && closed(r, c)) { rowSeen[r] = true; rq[n++] = r; }
      }
    }
    return false;
  }

  void driveColumn(uint8_t c) { driven = c; }
  void releaseColumn(uint8_t) {
    for (int r = 0; r < R; r++) {
      if (pulledLow(r)) { recovering[r] = true; releasedAt[r] = clock_us; }
    }
    driven = -1;
  }
  void settle() { clock_us += settle_us; }
  bool rowLow(uint8_t r) {
    if (pulledLow(r)) return true;
    // Still climbing back from the column before: below the input threshold
    // (taken as half the supply) until tau * ln 2 has passed.
    return recovering[r] && tauRise_us > 0.0 &&
           (double)(clock_us - releasedAt[r]) < tauRise_us * 0.6931;
  }
};

// Sweep the matrix every `period_us` for `duration_us`, collecting edges.
struct Tally {
  int      struck[R * C]   = {};
  int      released[R * C] = {};
  uint32_t everHeld        = 0;
  uint64_t firstStrike_us[R * C] = {};
};

static Tally sweepFor(SimMatrix& m, KeyMatrix<R, C>& km, uint64_t duration_us,
                      uint64_t period_us = 45) {
  Tally t;
  for (uint64_t now = 0; now < duration_us; now += period_us) {
    m.clock_us = now;
    KeyMatrix<R, C>::Sweep s = km.scan(m, (uint32_t)now);
    for (int b = 0; b < R * C; b++) {
      if (s.struck & (1u << b)) {
        if (t.struck[b]++ == 0) t.firstStrike_us[b] = now;
      }
      if (s.released & (1u << b)) t.released[b]++;
    }
    t.everHeld |= km.heldMask();
  }
  return t;
}

// ---- One key, one strike -------------------------------------------------

// The roll test_trigger uses: eight strikes, 2ms of bounce, 6 transitions.
// Read through the matrix, each is one strike and one release, on its own
// bit and nowhere else.
void test_each_bouncing_strike_fires_once_on_its_own_bit(void) {
  Trace roll = makeRoll("roll", 8, 80000, 2000, 40000, 6);
  for (int r = 0; r < R; r++) {
    for (int c = 0; c < C; c++) {
      SimMatrix m;
      m.trace[r][c] = &roll;
      KeyMatrix<R, C> km;
      Tally t = sweepFor(m, km, roll.duration_us);
      int bit = r * C + c;                       // the Keypad's kcode
      TEST_ASSERT_EQUAL_INT(8, t.struck[bit]);
      TEST_ASSERT_EQUAL_INT(8, t.released[bit]);
      TEST_ASSERT_EQUAL_HEX32(1u << bit, t.everHeld);
    }
  }
}

// ---- The diodes ----------------------------------------------------------

// Three keys held in a rectangle, in every rectangle the matrix has: with the
// board's diodes the fourth corner never appears.
void test_diodes_stop_ghosts_in_every_rectangle(void) {
  int cases = 0;
  for (int r1 = 0; r1 < R; r1++) for (int r2 = r1 + 1; r2 < R; r2++)
  for (int c1 = 0; c1 < C; c1++) for (int c2 = c1 + 1; c2 < C; c2++)
  for (int miss = 0; miss < 4; miss++) {
    SimMatrix m;
    int rs[4] = {r1, r1, r2, r2}, cs[4] = {c1, c2, c1, c2};
    uint32_t want = 0;
    for (int k = 0; k < 4; k++) {
      if (k == miss) continue;
      m.held[rs[k]][cs[k]] = true;
      want |= 1u << (rs[k] * C + cs[k]);
    }
    KeyMatrix<R, C> km;
    Tally t = sweepFor(m, km, 5000);
    TEST_ASSERT_EQUAL_HEX32(want, t.everHeld);
    cases++;
  }
  TEST_ASSERT_EQUAL_INT(144, cases);
}

// The same rectangles without diodes: the model must see the ghost, or the
// test above proves nothing about the diodes.
void test_without_diodes_the_fourth_corner_appears(void) {
  SimMatrix m;
  m.diodes = false;
  m.held[0][0] = m.held[0][1] = m.held[1][0] = true;
  KeyMatrix<R, C> km;
  Tally t = sweepFor(m, km, 5000);
  TEST_ASSERT_TRUE(t.everHeld & (1u << (1 * C + 1)));   // (1,1) never pressed
}

// Rows read, columns driven - the Keypad's direction. Wired or coded the other
// way round, every diode is reversed and the instrument is silent.
void test_scanning_the_wrong_way_reads_nothing(void) {
  SimMatrix m;
  m.transposed = true;
  for (int r = 0; r < R; r++) for (int c = 0; c < C; c++) m.held[r][c] = true;
  KeyMatrix<R, C> km;
  Tally t = sweepFor(m, km, 5000);
  TEST_ASSERT_EQUAL_HEX32(0, t.everHeld);
}

// ---- Settling --------------------------------------------------------------

// A row held low by one column climbs back through a ~45k pull-up. At 100pF
// of cable and diodes that is tau = 4.5us. If the next column is read before
// the row crosses the input threshold, the key in the same row of the next
// column appears pressed - a ghost that diodes cannot stop, because it is
// made of time, not current.
void test_short_settle_ghosts_into_the_next_column(void) {
  for (uint32_t settle : {2u, (uint32_t)KEY_MATRIX_SETTLE_US}) {
    SimMatrix m;
    m.tauRise_us = 4.5;
    m.settle_us = settle;
    m.held[2][1] = true;
    KeyMatrix<R, C> km;
    Tally t = sweepFor(m, km, 5000);
    bool ghost = (t.everHeld & (1u << (2 * C + 2))) != 0;
    if (settle == 2) TEST_ASSERT_TRUE_MESSAGE(ghost, "the model missed the stale read");
    else             TEST_ASSERT_FALSE_MESSAGE(ghost, "default settle ghosts at 100pF");
  }
}

// ---- The sweep period is TriggerInput's sampling period --------------------

// Same roll and same harness as test_trigger, sampled at the sweep period
// instead of the loop period. At the default sweep the strike still lands in
// the first ~333us contact segment. Sampled at 150us it no longer can: some
// strikes wait for the bounce to end - the too-long-confirm trap from
// trigger_input.h, reached by sampling too slowly instead.
void test_sweep_rate_keeps_the_strike_in_the_first_contact_segment(void) {
  Trace roll = makeRoll("roll", 8, 80000, 2000, 40000, 6);
  const uint64_t segment = 2000 / 6;
  const uint64_t sweep = C * (KEY_MATRIX_SETTLE_US + 1);   // ~1us of reads per column

  Result fast = run(roll, trig(TRIG_CONFIRM_US, TRIG_RELEASE_MS), sweep);
  TEST_ASSERT_TRUE(fast.clean(roll.hits.size()));
  TEST_ASSERT_TRUE(fast.worstLat_us < segment);

  Result slow = run(roll, trig(TRIG_CONFIRM_US, TRIG_RELEASE_MS), 150);
  TEST_ASSERT_TRUE(slow.worstLat_us > segment);
}

// The firmware has to call scan() continuously: held() only moves inside
// update(), so a key released while nobody scans stays held - for a bass or a
// ganza that is a note that never ends.
void test_release_is_only_seen_by_scanning(void) {
  SimMatrix m;
  m.held[0][2] = true;
  KeyMatrix<R, C> km;
  sweepFor(m, km, 2000);
  TEST_ASSERT_TRUE(km.heldMask() & (1u << 2));
  m.held[0][2] = false;
  m.clock_us = 100000;                         // time passes, no scan
  TEST_ASSERT_TRUE(km.heldMask() & (1u << 2));
  uint32_t released = 0;
  for (uint64_t now = 100000; now < 110000; now += 45) {
    m.clock_us = now;
    released |= km.scan(m, (uint32_t)now).released;
  }
  TEST_ASSERT_EQUAL_HEX32(1u << 2, released);
}

int main(int, char**) {
  UNITY_BEGIN();

  RUN_TEST(test_each_bouncing_strike_fires_once_on_its_own_bit);

  RUN_TEST(test_diodes_stop_ghosts_in_every_rectangle);
  RUN_TEST(test_without_diodes_the_fourth_corner_appears);
  RUN_TEST(test_scanning_the_wrong_way_reads_nothing);

  RUN_TEST(test_short_settle_ghosts_into_the_next_column);

  RUN_TEST(test_sweep_rate_keeps_the_strike_in_the_first_contact_segment);
  RUN_TEST(test_release_is_only_seen_by_scanning);

  return UNITY_END();
}
