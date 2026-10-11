#pragma once
/*
 * The Agbau's amplitude envelope: attack, decay, sustain, release, as linear
 * segments, one gain value per frame.
 *
 * Compiled into the firmware AND into the host test suite, same contract as
 * every header here. No Arduino, no M16, no clock: the sample rate is a
 * parameter, so the curve can be checked frame by frame on a desktop.
 *
 * NOT A CHAIN PROCESSOR
 *
 * Unlike PitchShift, this does not have the shape process(AudioBlock&). The
 * Agbau has sixteen of these, one per voice, each scaling that voice's own
 * mono stream before the voices are summed - a summed block has nothing left
 * to envelope. A voice asks it for one gain per frame (next()) as it walks
 * its sample, voice-outer and frame-inner like everything else in the engine.
 *
 * Its contracts are the ones any stateful DSP here has: the output depends on
 * the frames, never on how they were chunked (test_envelope pins it with the
 * same ragged sizes as the pitch shifter), and at a sustain of 1.0 the gain is
 * exactly 1.0f, so a held bass comes out bit-exact.
 *
 * WHERE IT COMES FROM
 *
 * A port of the Bela ADSR and Ramp that the Agbau plays through today
 * (agbau/Bela/agbau-sampler-poly/ADSR.cpp, Ramp.cpp), held to a verbatim copy
 * of that code in test/agbau/bela_reference.h. Same timing to the frame: a
 * segment of N frames yields N values, the last one on the target, and the next
 * segment's first step comes on the very next frame - no hold at the peak.
 * trigger() ramps from wherever the level IS to 1, not from 0: Bela's
 * ADSR::trigger() looks like it resets to zero first, but that rampTo() is
 * overwritten on the following line before a single frame runs.
 *
 * Two places where the port differs on purpose:
 *
 *  - Each value is computed as start + step * k, and the last frame of a
 *    segment is set to the target itself. Bela adds the step every frame and
 *    the rounding accumulates: measured against test/agbau/bela_reference.h,
 *    its sustain at "1.0" is 1.0000049, its release to "0" stops at +0.0005
 *    (-66dB), and its 3s percussion decay to "0" stops at -0.00108 (-59dB,
 *    phase-inverted) and stays there until the sample runs out. Here a held
 *    bass multiplies by exactly 1.0f and a finished hit by exactly 0.0f - which
 *    is what lets agbau/tools/agbau_kit.py drop everything after it. On the
 *    real kit that residual put Bela's post-decay output at -92dBFS at the
 *    loudest (p6.3.2) and -114 to -118 elsewhere: under one 16-bit LSB.
 *  - A zero-time segment jumps to its target on the next frame. In Bela,
 *    rampTo(x, 0) never moves at all. No preset the Agbau uses can tell the
 *    difference: its only zero-time segments are decays to the level the
 *    attack already ended on.
 *
 * Segment lengths are rounded to the nearest frame; Bela truncates. For every
 * preset in use they agree (441, 4410, 44100, 132300 frames), and rounding
 * does not depend on whether 0.01 happens to land a hair under 441.
 */
#include <stdint.h>

class Envelope {
 public:
  enum State : uint8_t { Off = 0, Attack, Decay, Sustain, Release };

  // Seconds -> frames, rounded to nearest. Public so agbau/tools/agbau_kit.py's
  // trimming arithmetic can be pinned against it by a test.
  static uint32_t framesFor(float seconds, float sampleRate) {
    if (!(seconds > 0.0f)) return 0;
    return (uint32_t)(seconds * sampleRate + 0.5f);
  }

  /* Times in seconds, sustain as a level in [0, 1]. Out-of-range values are
   * clamped the way Bela's setters clamp them. Takes effect from the next
   * segment started; a segment already running keeps its length. */
  void set(float attack, float decay, float sustain, float release, float sampleRate) {
    _attackLen  = framesFor(attack, sampleRate);
    _decayLen   = framesFor(decay, sampleRate);
    _releaseLen = framesFor(release, sampleRate);
    _sustain    = sustain < 0.0f ? 0.0f : (sustain > 1.0f ? 1.0f : sustain);
  }

  /* Start a note: ramp from the current level to 1 over the attack. */
  void trigger() { begin(Attack, 1.0f, _attackLen); }

  /* End a note: ramp from the current level to 0 over the release. Whether a
   * note-off should call this at all is the voice's decision, not this
   * class's - the Agbau's percussion ignores note-off entirely. */
  void release() { begin(Release, 0.0f, _releaseLen); }

  /* The gain for the next frame. */
  float next() {
    if (_state == Off || _state == Sustain) return _level;
    float out;
    if (++_pos >= _len) {
      out = _target;                 // on the target, not near it
      _level = _target;
      finishSegment();               // the next segment steps from the next frame
    } else {
      out = _start + _step * (float)_pos;
      _level = out;
    }
    return out;
  }

  /* next() for a run of frames. The block-size tests use it; a voice calls
   * next() inside its own frame loop instead, to keep one pass per voice. */
  void render(float* gain, int frames) {
    for (int n = 0; n < frames; n++) gain[n] = next();
  }

  /* Zero now, and zero for every frame until the next trigger(): Off, or a
   * sustain of 0 (a finished percussion hit). A voice can stop reading its
   * sample here without changing a single output sample. */
  bool silent() const {
    return _level == 0.0f && (_state == Off || _state == Sustain || _state == Release);
  }

  State state() const { return _state; }
  float level() const { return _level; }

 private:
  void begin(State s, float target, uint32_t len) {
    _state  = s;
    _start  = _level;
    _target = target;
    _len    = len;
    _pos    = 0;
    if (len == 0) {
      _level = target;               // zero-time: there on the next frame
      finishSegment();
    } else {
      _step = (target - _start) / (float)len;
    }
  }

  void finishSegment() {
    switch (_state) {
      case Attack:  begin(Decay, _sustain, _decayLen); break;
      case Decay:   _state = Sustain; break;
      case Release: _state = Off; break;
      default: break;
    }
  }

  State    _state      = Off;
  float    _level      = 0.0f;
  float    _start      = 0.0f;
  float    _target     = 0.0f;
  float    _step       = 0.0f;
  uint32_t _len        = 0;
  uint32_t _pos        = 0;

  uint32_t _attackLen  = 0;
  uint32_t _decayLen   = 0;
  uint32_t _releaseLen = 0;
  float    _sustain    = 1.0f;
};
