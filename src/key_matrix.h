#pragma once
/*
 * The Agbau's 4x4 key matrix: strobe a column, read the rows, condition every
 * key with its own TriggerInput.
 *
 * Compiled into the firmware AND into the host test suite. The pins are not
 * touched here: a scan takes an IO object, so the firmware passes one that
 * drives GPIOs and the tests pass a simulated matrix - with diodes, without
 * them, with slow row lines - and the same scan code runs against both.
 *
 * NO NEW DEBOUNCE
 *
 * Every key is a TriggerInput, the same state machine as every button on the
 * sampler, reading the raw level. The matrix only changes how that raw level
 * is obtained: one key per row per column strobe, instead of one pin per key.
 * A strike is TriggerInput's fire edge; a release is held() falling - the
 * same two readings of the same machine GPIO7 already uses.
 *
 * THE SCAN DIRECTION IS NOT A CHOICE
 *
 * The Agbau board has a diode in series with every key, conducting from the
 * row side to the column side: on Agbaixo_PCB.kicad_pcb each key connector
 * (J8-J23) sits between a row and a diode's anode, and the cathodes meet on
 * the columns (docs/agbau-circuit.md). The Arduino's
 * Keypad library (Keypad.cpp:83-100) scans accordingly: rows are inputs with
 * pull-ups, one column at a time is driven LOW, the rest float. A pressed
 * key then pulls its row low through a forward-biased diode. Scanned the
 * other way - rows driven, columns read - every diode is reversed and the
 * matrix reads nothing at all; test_scanning_the_wrong_way_reads_nothing pins
 * this. The same diodes are what stop ghost keys: three keys held in a
 * rectangle cannot fake the fourth, because the path would need one diode
 * to conduct backwards.
 *
 * Idle columns FLOAT rather than being driven HIGH, as Keypad does. With
 * diodes either works; floating is what the board was built and played with.
 * On the ESP32 that is an open-drain output: LOW drives, HIGH lets go.
 *
 * SETTLING, AND WHY IT SETS THE SCAN RATE
 *
 * When a column lets go, any row it was holding low has to climb back to HIGH
 * through the pin's internal pull-up (~45k on the S3) and the line's own
 * capacitance before the NEXT column is read - or that row still reads LOW,
 * and the key above it in the next column appears pressed. A falling edge is
 * fast (the key is a short); the rising edge is the slow one, ~45k x the
 * cable and diode capacitance. KEY_MATRIX_SETTLE_US is the wait after each
 * column is driven, and it has to cover that rise: 10us is ~4 time constants
 * at 50pF. test_short_settle_ghosts_into_the_next_column models it.
 *
 * And it is not free: every key is sampled once per sweep, so the sweep
 * period is TriggerInput's sampling period. Its 200us confirm window has to
 * fit inside the first contact segment of a bounce (~300us) with room for
 * a sample after it, or the strike waits for the bounce to end - the same
 * trap trigger_input.h describes for a too-long confirm window, reached by
 * sampling too slowly instead. The static_assert below holds four samples per
 * confirm window, which with the read overhead is a ~45us sweep.
 */
#include <stdint.h>
#include "trigger_input.h"

#ifndef KEY_MATRIX_SETTLE_US
  #define KEY_MATRIX_SETTLE_US 10
#endif

// Minimum samples of each key inside one TRIG_CONFIRM_US window.
#define KEY_MATRIX_SAMPLES_PER_CONFIRM 4

/*
 * IO is any type with:
 *   void driveColumn(uint8_t col);    // column to OUTPUT LOW
 *   void releaseColumn(uint8_t col);  // column back to floating
 *   void settle();                    // wait KEY_MATRIX_SETTLE_US
 *   bool rowLow(uint8_t row);         // raw read: true = LOW = closed
 */
template <int ROWS, int COLS>
class KeyMatrix {
  static_assert(ROWS * COLS <= 32, "one bit per key in a uint32_t");
  static_assert(COLS * KEY_MATRIX_SETTLE_US * KEY_MATRIX_SAMPLES_PER_CONFIRM
                    <= TRIG_CONFIRM_US,
                "a sweep is too slow for TRIG_CONFIRM_US: each key would be "
                "sampled fewer than KEY_MATRIX_SAMPLES_PER_CONFIRM times per "
                "confirm window - shorten the settle or use stronger pull-ups");

 public:
  TriggerInput keys[ROWS][COLS];

  // Bit (row * COLS + col) - the Keypad library's kcode - per key that was
  // struck or released during this sweep.
  struct Sweep {
    uint32_t struck;
    uint32_t released;
  };

  /* One sweep of every key against one timestamp, the same convention as
   * main.cpp's loop(): every input conditioned against the same instant. */
  template <typename IO>
  Sweep scan(IO& io, uint32_t now_us) {
    Sweep s{0, 0};
    for (uint8_t c = 0; c < COLS; c++) {
      io.driveColumn(c);
      io.settle();
      for (uint8_t r = 0; r < ROWS; r++) {
        TriggerInput& k = keys[r][c];
        const uint32_t bit = 1u << (r * COLS + c);
        if (k.update(now_us, io.rowLow(r))) s.struck |= bit;
        if (k.held()) {
          _held |= bit;
        } else if (_held & bit) {
          _held &= ~bit;
          s.released |= bit;
        }
      }
      io.releaseColumn(c);
    }
    return s;
  }

  uint32_t heldMask() const { return _held; }

 private:
  uint32_t _held = 0;
};
