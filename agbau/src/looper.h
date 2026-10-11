#pragma once
/*
 * The Agbau's looper: record what is played live, play it back in a loop,
 * overdub on top, hold the pedal to erase. Counted in audio frames.
 *
 * Compiled into the firmware AND into the host test suite. The behaviour is
 * agbau/docs/looper.md, approved 11/10/2026: agbau.ino's looper with its bugs
 * taken out. Read the spec before changing anything here.
 *
 * WHAT A LAYER HOLDS
 *
 * Voice events (voice, on/off) at a position in the loop, in frames. Only
 * what the Performer generated from live keys is recorded - the loop's own
 * playback never is. The .ino recorded through the same noteOn() it played
 * back with, so every overdub re-recorded the loop under it (bug 4).
 *
 * An event is packed into 4 bytes: position in the top 24 bits, voice and
 * on/off below. 24 bits of position is ~380s at 44.1kHz, so a first take is
 * closed at LOOPER_MAX_FRAMES, as a full layer is. Four layers of 1024 events
 * are 16KB of internal RAM.
 *
 * Every layer is in position order by construction: the first take records
 * forward in time, and an overdub closes when the loop passes its start, so
 * it cannot wrap onto itself. Playback walks each layer with a cursor instead
 * of searching it.
 *
 * NO STUCK VOICES
 *
 * Three ways the .ino leaves a ganza looping forever, each closed here:
 *  - a key still held when a layer closes has its note-on recorded and not
 *    its note-off: closing writes the missing note-offs (bug 8). Room for
 *    them is reserved, so a full layer can still close cleanly;
 *  - erasing stops recording but not what the loop had turned on: erase
 *    releases every voice the playback started (bug 9);
 *  - a fourth overdub writing past the array (bug 3): there are exactly
 *    LOOPER_LAYERS layers, and the pedal does nothing once they are used.
 */
#include <stdint.h>

#ifndef LOOPER_LAYERS
  #define LOOPER_LAYERS 4             // the base and three overdubs
#endif
#ifndef LOOPER_CAPACITY
  #define LOOPER_CAPACITY 1024        // events per layer
#endif
// Kept free at the end of every layer for the note-offs a close may write.
#define LOOPER_RESERVE 32
// A first take is closed before its positions outgrow 24 bits, with room for
// one block of up to 4096 frames past the check.
#define LOOPER_MAX_FRAMES ((1u << 24) - 4096u)

static_assert(LOOPER_CAPACITY > LOOPER_RESERVE, "no room left to record");

enum LooperState : uint8_t {
  LOOPER_EMPTY = 0,
  LOOPER_RECORDING,
  LOOPER_PLAYING,
  LOOPER_OVERDUB,
};

static inline const char* looperStateName(LooperState s) {
  switch (s) {
    case LOOPER_EMPTY:     return "empty";
    case LOOPER_RECORDING: return "recording";
    case LOOPER_PLAYING:   return "playing";
    case LOOPER_OVERDUB:   return "overdub";
  }
  return "?";
}

class Looper {
 public:
  LooperState state() const { return _state; }
  uint8_t     layers() const { return _layers; }       // closed layers
  uint32_t    length() const { return _len; }          // frames; 0 until closed
  uint32_t    position() const { return _pos; }
  uint16_t    events(uint8_t layer) const { return layer < LOOPER_LAYERS ? _n[layer] : 0; }
  // Layers closed because they filled up - for DEBUG_ENGINE, never silent.
  uint32_t    fullCloses() const { return _fullCloses; }

  /* First thing in every block. Moves the playhead to `now`, and closes an
   * overdub whose block crosses the loop's start, or a first take that has
   * reached LOOPER_MAX_FRAMES. */
  void advance(uint32_t now) {
    if (_state == LOOPER_RECORDING) {
      uint32_t r = now - _start;
      if (r >= LOOPER_MAX_FRAMES) {
        _fullCloses++;
        closeFirst(now);
      }
      return;
    }
    if (_state != LOOPER_PLAYING && _state != LOOPER_OVERDUB) return;
    uint64_t sum = (uint64_t)_pos + (uint32_t)(now - _last);
    _last = now;
    _wrapped = sum >= _len;
    _pos = (uint32_t)(sum % _len);
    // The .ino meant this (agbau.ino:326) but tested for an exact
    // millisecond, which it usually hit and sometimes skipped. This line IS
    // the overdub rule - rule (a) of agbau/docs/looper.md, kept on purpose.
    // The two alternatives, and what each breaks here (layers in position
    // order; a layer that does not play while it records), are written down
    // there under "Se um dia for preciso trocar a regra do overdub".
    if (_state == LOOPER_OVERDUB && _wrapped) closeOverdub(_len - 1);
  }

  /* A press of the pedal. Its hold is the Performer's (erase()). */
  void pedal(uint32_t now) {
    switch (_state) {
      case LOOPER_EMPTY:
        _state = LOOPER_RECORDING;
        _start = now;
        _layers = 0;
        _n[0] = 0;
        _open = 0;
        break;
      case LOOPER_RECORDING:
        closeFirst(now);
        break;
      case LOOPER_PLAYING:
        if (_layers < LOOPER_LAYERS) {
          _state = LOOPER_OVERDUB;
          _n[_layers] = 0;
          _open = 0;
        }
        break;
      case LOOPER_OVERDUB:
        closeOverdub(_pos);
        break;
    }
  }

  /* A live voice event, applied in the block starting at `now` and
   * belonging to frame `target` (a repeat hit's grid point, up to one block
   * earlier). It is recorded where it belongs, not where it was applied, or
   * the loop would carry the block size into every lap. */
  void record(uint32_t now, uint32_t target, uint8_t voice, bool on) {
    if (voice >= 32) return;
    uint32_t pos;
    uint8_t layer;
    if (_state == LOOPER_RECORDING) {
      int32_t r = (int32_t)(target - _start);
      pos = r < 0 ? 0 : (uint32_t)r;
      layer = 0;
    } else if (_state == LOOPER_OVERDUB) {
      // An overdub closes in any block that crosses the loop's start, so a
      // target this block is never behind the start of the lap.
      uint32_t behind = now - target;
      pos = behind <= _pos ? _pos - behind : 0;
      layer = _layers;
    } else {
      return;
    }
    append(layer, pos, voice, on);
    if (on) _open |= 1u << voice;
    else    _open &= ~(1u << voice);
    if (_n[layer] >= LOOPER_CAPACITY - LOOPER_RESERVE) {
      _fullCloses++;
      if (_state == LOOPER_RECORDING) closeFirst(now);
      else                            closeOverdub(_pos);
    }
  }

  /* Hold on the pedal: everything goes, and every voice the playback turned
   * on is turned off first. */
  template <typename Emit>
  void erase(uint32_t now, Emit&& emit) {
    for (uint8_t v = 0; v < 32; v++)
      if (_loopOn & (1u << v)) emit(v, false, now);
    _state = LOOPER_EMPTY;
    _layers = 0;
    _len = 0;
    _pos = 0;
    _open = 0;
    _loopOn = 0;
    for (int l = 0; l < LOOPER_LAYERS; l++) _n[l] = _cursor[l] = 0;
  }

  /* After the live events of the block. Plays every closed layer's events
   * in (previous playhead, playhead]: in position order, and at the same
   * position in layer order. The layer being overdubbed does not play until
   * it closes - its events are all behind the playhead anyway. */
  template <typename Emit>
  void play(uint32_t now, Emit&& emit) {
    if (_state != LOOPER_PLAYING && _state != LOOPER_OVERDUB) return;
    if (_wrapped) {
      fireUpTo(_len - 1, now, emit);
      for (int l = 0; l < _layers; l++) _cursor[l] = 0;
    }
    fireUpTo(_pos, now, emit);
  }

  /* The looper LED: off empty, lit recording and overdubbing, and while
   * playing lit in the first and third quarter of every lap, counted from
   * the loop's start - the .ino's two blinks per lap, without its drift. */
  bool led() const {
    switch (_state) {
      case LOOPER_EMPTY:     return false;
      case LOOPER_RECORDING:
      case LOOPER_OVERDUB:   return true;
      case LOOPER_PLAYING: {
        uint32_t q = (uint32_t)((uint64_t)_pos * 4 / _len);
        return q == 0 || q == 2;
      }
    }
    return false;
  }

 private:
  static uint32_t pack(uint32_t pos, uint8_t voice, bool on) {
    return (pos << 8) | ((uint32_t)voice << 1) | (on ? 1u : 0u);
  }
  static uint32_t posOf(uint32_t e) { return e >> 8; }
  static uint8_t  voiceOf(uint32_t e) { return (uint8_t)((e >> 1) & 0x7F); }
  static bool     onOf(uint32_t e) { return e & 1u; }

  void append(uint8_t layer, uint32_t pos, uint8_t voice, bool on) {
    if (_n[layer] < LOOPER_CAPACITY) _ev[layer][_n[layer]++] = pack(pos, voice, on);
  }

  // Write the note-offs of every note still open in the layer being recorded.
  void closeOpenNotes(uint8_t layer, uint32_t pos) {
    for (uint8_t v = 0; v < 32; v++)
      if (_open & (1u << v)) append(layer, pos, v, false);
    _open = 0;
  }

  void closeFirst(uint32_t now) {
    uint32_t len = now - _start;
    if (len == 0) return;            // two presses in one block: not a loop yet
    _len = len;
    // Events from the closing block itself sit at `len`, which is the next
    // lap's start. They were just played live; at len - 1 they come back one
    // frame early each lap, instead of twice on this one.
    for (uint16_t i = 0; i < _n[0]; i++)
      if (posOf(_ev[0][i]) > len - 1)
        _ev[0][i] = pack(len - 1, voiceOf(_ev[0][i]), onOf(_ev[0][i]));
    closeOpenNotes(0, len - 1);
    _layers = 1;
    _cursor[0] = _n[0];
    _state = LOOPER_PLAYING;
    // This block's window is (len - 1, 0]: whatever was recorded at the
    // first frame plays now, as the loop comes round for the first time.
    _last = now;
    _pos = 0;
    _wrapped = true;
  }

  void closeOverdub(uint32_t pos) {
    closeOpenNotes(_layers, pos);
    _cursor[_layers] = _n[_layers];  // all behind the playhead: next lap
    _layers++;
    _state = LOOPER_PLAYING;
  }

  template <typename Emit>
  void fireUpTo(uint32_t hi, uint32_t now, Emit&& emit) {
    for (;;) {
      int best = -1;
      uint32_t bestPos = 0;
      for (int l = 0; l < _layers; l++) {
        if (_cursor[l] >= _n[l]) continue;
        uint32_t p = posOf(_ev[l][_cursor[l]]);
        if (p > hi) continue;
        if (best < 0 || p < bestPos) {
          best = l;
          bestPos = p;
        }
      }
      if (best < 0) return;
      uint32_t e = _ev[best][_cursor[best]++];
      uint8_t v = voiceOf(e);
      // Where this event falls relative to the playhead: behind it by
      // (pos - p) on this lap, or by (pos + len - p) on the lap just ended.
      uint32_t behind = bestPos <= _pos ? _pos - bestPos : _pos + _len - bestPos;
      if (onOf(e)) _loopOn |= 1u << v;
      else         _loopOn &= ~(1u << v);
      emit(v, onOf(e), now - behind);
    }
  }

  LooperState _state      = LOOPER_EMPTY;
  uint8_t     _layers     = 0;
  uint32_t    _start      = 0;    // frame the first take began
  uint32_t    _len        = 0;
  uint32_t    _last       = 0;    // frame of the last advance()
  uint32_t    _pos        = 0;    // playhead now
  bool        _wrapped    = false;
  uint32_t    _open       = 0;    // voices on and not yet off, in the layer being recorded
  uint32_t    _loopOn     = 0;    // voices the playback has on
  uint32_t    _fullCloses = 0;
  uint16_t    _n[LOOPER_LAYERS]      = {};
  uint16_t    _cursor[LOOPER_LAYERS] = {};
  uint32_t    _ev[LOOPER_LAYERS][LOOPER_CAPACITY];
};
