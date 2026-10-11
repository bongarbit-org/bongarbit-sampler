#pragma once
/*
 * The Agbau's tap tempo and repeat mode, counted in audio frames.
 *
 * Compiled into the firmware AND into the host test suite. The behaviour is
 * agbau/docs/looper.md, approved 11/10/2026, which is agbau.ino's design with
 * its bugs taken out. Read the spec before changing anything here: several
 * things below look like candidates for tidying and are the instrument's
 * feel, kept on purpose.
 *
 * TIME IS FRAMES
 *
 * Every time here is an absolute frame count, uint32_t, advanced by the audio
 * task one block at a time. Differences are unsigned, so the ~27 hour wrap is
 * a non-event, the same convention as micros() in trigger_input.h. Nothing
 * here reads a clock: the caller says what time it is, which is what lets the
 * tests run an hour of repeat in a few milliseconds.
 *
 * THE GRID DOES NOT DRIFT
 *
 * The .ino advanced its repeat with previousMillis = currentMillis on every
 * hit, so each hit inherited the lateness of the last. Here a tick is
 * anchor + floor(k * 4P / D), computed from the anchor every time, in 64
 * bits. The error stays under one frame forever, even at D = 3 where 4P/D is
 * not a whole number of frames.
 */
#include <stdint.h>

// The .ino's divisions of a 4-beat bar, chosen by the last bass struck in
// repeat mode (agbau.ino:36-40). Index = bass voice 0-7.
static const uint8_t REPEAT_DIVISIONS[8] = {2, 3, 4, 6, 8, 12, 16, 32};
#define REPEAT_DEFAULT_DIVISION 2   // the .ino's ultimoBaixo = 3: 4 per bar

/*
 * The tempo is the last interval between two taps, in frames. No average,
 * no range: whatever was tapped is the tempo, as on the .ino - including a
 * slow one after a lone tap following a pause, which players already know.
 * One exception, where the .ino breaks: an interval of 60s or more, where its
 * integer bpm truncates to 0 and the next line divides by it (a panic on the
 * ESP32). Such a tap only starts the count again.
 *
 * The first tap after boot also only starts the count. The .ino measured it
 * from boot (tempoBotaoAnterior = 0), which is a tempo nobody tapped.
 */
class TapTempo {
 public:
  void init(float sampleRate) {
    _beat = (uint32_t)(sampleRate * 0.5f + 0.5f);       // 120 bpm, the .ino's
    _maxInterval = (uint32_t)(sampleRate * 60.0f + 0.5f);
    _hasLast = false;
  }

  /* A tap at `now`. True if it changed the tempo. */
  bool tap(uint32_t now) {
    bool changed = false;
    if (_hasLast) {
      uint32_t d = now - _lastTap;
      // The lower bound only keeps 4P/D at a frame or more, so a tick can
      // always advance; no hand taps within 8 frames (0.2ms) of the last.
      if (d >= 8 && d < _maxInterval) {
        _beat = d;
        changed = true;
      }
    }
    _lastTap = now;
    _hasLast = true;
    return changed;
  }

  uint32_t beat() const { return _beat; }       // frames per beat
  uint32_t lastTap() const { return _lastTap; }

 private:
  uint32_t _beat = 22050;
  uint32_t _maxInterval = 2646000;
  uint32_t _lastTap = 0;
  bool     _hasLast = false;
};

/*
 * Repeat mode: while percussion keys pressed in repeat are held, the most
 * recently pressed of them is struck on a grid of D hits per 4-beat bar.
 *
 * The first hit, faithfully to the .ino: the grid has an origin, the last
 * hit or the last tap, whichever came later (previousMillis). A press more
 * than one subdivision after it sounds at once and restarts the grid there.
 * A press within one subdivision waits for the grid that was already running
 * - so a quick re-press, or a change of key mid-repeat, keeps the groove. Not
 * written on purpose in the .ino; it is how the Agbau answers under the hand.
 *
 * Emit is a callable emit(voice, on, target): `target` is the frame the event
 * belongs to, which the caller applies at the first block that starts at or
 * after it.
 */
class Repeat {
 public:
  bool    on() const { return _on; }
  uint8_t division() const { return REPEAT_DIVISIONS[_div]; }

  /* A tap: repeat on, and the grid (and the beat the LED shows) realigned to
   * the tap - tapping says where "one" is, not only how fast. */
  void tap(uint32_t now, uint32_t beat) {
    _on = true;
    _beat = beat;
    _anchor = now;
    _k = 0;
  }

  /* Hold on the mode button: repeat off. Unlike the .ino, which left the
   * repeating voice to itself - a ganza kept looping - it is released. */
  template <typename Emit>
  void leave(uint32_t now, Emit&& emit) {
    if (_fired >= 0) emit((uint8_t)_fired, false, now);
    _on = false;
    _held = 0;
    _fired = -1;
  }

  /* A bass struck in repeat: sets the division, and does not sound. The
   * grid re-anchors at its last hit, so the next one comes one NEW
   * subdivision after it - what the .ino's now - previousMillis > interval
   * did with the interval changed underneath it. */
  void setDivision(uint8_t bass) {
    if (bass >= 8) return;
    _anchor = lastTick();
    _k = 0;
    _div = bass;
  }

  template <typename Emit>
  void press(uint8_t voice, uint32_t now, Emit&& emit) {
    if (voice >= 32) return;
    bool idle = _held == 0;
    _held |= 1u << voice;
    _seq[voice] = ++_counter;
    if (idle && (uint32_t)(now - lastTick()) >= subdivision()) {
      _anchor = now;
      _k = 0;
      fire(voice, now, emit);
    }
    // Otherwise the next tick plays it: the most recent key held.
  }

  /* True if `voice` was a key of this repeat (pressed in repeat and not
   * cleared since), so the caller knows the release was this mode's. */
  template <typename Emit>
  bool release(uint8_t voice, uint32_t now, Emit&& emit) {
    if (voice >= 32 || !(_held & (1u << voice))) return false;
    _held &= ~(1u << voice);
    if (_held == 0 && _fired >= 0) {
      emit((uint8_t)_fired, false, now);
      _fired = -1;
    }
    return true;
  }

  /* Once per block, at its start. At most one hit per block however short
   * the subdivision, as the .ino played at most one per pass of loop(). */
  template <typename Emit>
  void tick(uint32_t now, Emit&& emit) {
    if (!_on || _held == 0) return;          // the grid stands still, as previousMillis did
    bool due = false;
    while ((int32_t)(now - tickAt(_k + 1)) >= 0) {
      _k++;
      due = true;
    }
    if (due) fire(mostRecent(), tickAt(_k), emit);
  }

  /* The mode LED: lit for the first half of every beat, counted from the tap. */
  bool led(uint32_t now, uint32_t tapAt) const {
    if (!_on || _beat == 0) return false;
    return (now - tapAt) % _beat < _beat / 2;
  }

  uint32_t subdivision() const { return (uint32_t)(4ull * _beat / division()); }

 private:
  uint32_t tickAt(uint32_t k) const {
    return _anchor + (uint32_t)((uint64_t)k * 4u * _beat / division());
  }
  uint32_t lastTick() const { return tickAt(_k); }

  uint8_t mostRecent() const {
    uint8_t best = 0;
    uint32_t seq = 0;
    for (uint8_t v = 0; v < 32; v++)
      if ((_held & (1u << v)) && _seq[v] >= seq) {
        seq = _seq[v];
        best = v;
      }
    return best;
  }

  template <typename Emit>
  void fire(uint8_t voice, uint32_t target, Emit&& emit) {
    // A change of voice releases the last one first: nothing for percussion,
    // but a ganza that was repeating stops instead of looping on.
    if (_fired >= 0 && _fired != voice) emit((uint8_t)_fired, false, target);
    emit(voice, true, target);
    _fired = voice;
  }

  bool     _on      = false;
  uint8_t  _div     = REPEAT_DEFAULT_DIVISION;
  uint32_t _beat    = 22050;
  uint32_t _anchor  = 0;
  uint32_t _k       = 0;
  uint32_t _held    = 0;
  int      _fired   = -1;
  uint32_t _counter = 0;
  uint32_t _seq[32] = {};
};
