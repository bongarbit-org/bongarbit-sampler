#pragma once
/*
 * Performer: everything between the Agbau's keys and its voices. Key edges
 * go in; note-ons and note-offs come out, on the audio task, at the top of
 * each block.
 *
 * Compiled into the firmware AND into the host test suite. The behaviour is
 * agbau/docs/looper.md, approved 11/10/2026.
 *
 * WHY IT RUNS ON THE AUDIO TASK
 *
 * Repeat and looper are timing, and the only clock that cannot drift against
 * the sound is the sound: the frame count of the blocks the audio task
 * renders. So loop() only reports key edges, through the same kind of queue
 * as NoteQueue, and this object - on the audio task - decides what sounds.
 * An edge takes effect at the start of the next block, and that frame is its
 * time, for the tap tempo and for the loop alike: every edge waits at most one
 * block (0.73ms), the same for all of them.
 *
 * Events the Performer generates from live keys - played notes, repeat hits,
 * the release when repeat is left - are recorded by the looper. What the
 * looper plays back is not.
 *
 * Out is a callable out(voice, on, target). `target` is the frame the event
 * belongs to: the block's own start for a live edge, the grid point for a
 * repeat hit, the position in the lap for the loop. The firmware ignores it
 * (everything is applied at the block start); the tests use it to show the
 * schedule does not depend on the block size.
 */
#include <stdint.h>
#include "note_queue.h"
#include "agbau_kit.h"
#include "tap_tempo.h"
#include "looper.h"

#define PERF_KEY_MODE  AGBAU_VOICES          // the mode button
#define PERF_KEY_PEDAL (AGBAU_VOICES + 1)    // the looper pedal
#define PERF_KEYS      (AGBAU_VOICES + 2)

// Holding the mode button leaves repeat, holding the pedal erases the loop:
// both after 1s, the .ino's currentDuration() > 1000.
#ifndef PERF_HOLD_SECONDS
  #define PERF_HOLD_SECONDS 1.0f
#endif

struct KeyEvent {
  uint8_t key;     // 0-15 a voice's key, PERF_KEY_MODE, PERF_KEY_PEDAL
  bool    down;
};

class Performer {
 public:
  void init(float sampleRate) {
    _tempo.init(sampleRate);
    _hold = (uint32_t)(sampleRate * PERF_HOLD_SECONDS + 0.5f);
  }

  /* loop() side. Out-of-range keys are refused at the door. */
  bool post(uint8_t key, bool down) {
    if (key >= PERF_KEYS) return false;
    return _keys.push(KeyEvent{key, down});
  }

  /* Audio task side, once per block of `frames`, before the voices render. */
  template <typename Out>
  void process(uint32_t frames, Out&& out) {
    const uint32_t now = _now;
    auto live = [&](uint8_t v, bool on, uint32_t target) {
      out(v, on, target);
      _loop.record(now, target, v, on);
    };

    _loop.advance(now);
    // Before this block's keys: a hit due now belongs to a frame at or before
    // the block's start, so it goes into the loop ahead of anything pressed
    // in this block, and every layer stays in position order.
    _repeat.tick(now, live);

    KeyEvent e;
    while (_keys.take(e)) {
      if (e.key < AGBAU_VOICES)        voiceKey(e.key, e.down, now, live);
      else if (e.key == PERF_KEY_MODE) modeKey(e.down, now);
      else                             pedalKey(e.down, now);
    }

    if (_modeDown && !_modeHoldDone && (uint32_t)(now - _modeAt) >= _hold) {
      _modeHoldDone = true;
      _repeat.leave(now, live);
    }
    if (_pedalDown && !_pedalHoldDone && (uint32_t)(now - _pedalAt) >= _hold) {
      _pedalHoldDone = true;
      if (_loop.state() != LOOPER_EMPTY) _loop.erase(now, out);
    }

    _loop.play(now, out);
    _now += frames;
  }

  // State for the log and the LEDs. Read from loop() without synchronisation,
  // as DEBUG_ENGINE reads its counters: a stale value costs one wrong line.
  bool          repeatOn() const { return _repeat.on(); }
  uint32_t      beat() const { return _tempo.beat(); }
  uint8_t       division() const { return _repeat.division(); }
  const Looper& looper() const { return _loop; }
  bool          modeLed() const { return _repeat.led(_now, _tempo.lastTap()); }
  bool          looperLed() const { return _loop.led(); }
  uint32_t      now() const { return _now; }
  uint32_t      dropped() const { return _keys.dropped(); }

 private:
  // How a held key was pressed, so its release is that mode's: a ganza held
  // across a switch into repeat still gets its note-off (bug 7).
  enum KeyMode : uint8_t { KEY_UP = 0, KEY_NOTE, KEY_REPEAT, KEY_DIVISION };

  template <typename Live>
  void voiceKey(uint8_t v, bool down, uint32_t now, Live& live) {
    if (down) {
      if (!_repeat.on()) {
        live(v, true, now);
        _mode[v] = KEY_NOTE;
      } else if (AGBAU_VOICE_DEFS[v].group == AGBAU_BASS) {
        _repeat.setDivision(v);            // bass voices are 0-7
        _mode[v] = KEY_DIVISION;
      } else {
        _repeat.press(v, now, live);
        _mode[v] = KEY_REPEAT;
      }
      return;
    }
    switch (_mode[v]) {
      case KEY_NOTE:     live(v, false, now); break;
      case KEY_REPEAT:   _repeat.release(v, now, live); break;
      case KEY_DIVISION:
      case KEY_UP:       break;
    }
    _mode[v] = KEY_UP;
  }

  void modeKey(bool down, uint32_t now) {
    _modeDown = down;
    if (!down) return;
    _modeAt = now;
    _modeHoldDone = false;
    _tempo.tap(now);
    _repeat.tap(now, _tempo.beat());
  }

  void pedalKey(bool down, uint32_t now) {
    _pedalDown = down;
    if (!down) return;
    _pedalAt = now;
    _pedalHoldDone = false;
    _loop.pedal(now);
  }

  SpscQueue<KeyEvent> _keys;
  TapTempo            _tempo;
  Repeat              _repeat;
  Looper              _loop;
  uint32_t            _now  = 0;
  uint32_t            _hold = 44100;
  KeyMode             _mode[AGBAU_VOICES] = {};
  bool                _modeDown = false, _modeHoldDone = false;
  bool                _pedalDown = false, _pedalHoldDone = false;
  uint32_t            _modeAt = 0, _pedalAt = 0;
};
