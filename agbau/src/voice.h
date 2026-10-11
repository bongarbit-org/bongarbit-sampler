#pragma once
/*
 * One Agbau voice: a set of takes, a read position, an envelope - and the
 * bank of sixteen that the audio task renders.
 *
 * Compiled into the firmware AND into the host test suite, same contract as
 * every header here. A take is a `const int16_t*` and a frame count, nothing
 * else, so the same code plays a kit mapped from flash or loaded into PSRAM -
 * agbau/docs/migration.md step 0 has not chosen yet, and nothing here cares.
 *
 * A PORT OF THE BELA SAMPLER
 *
 * agbau/Bela/agbau-sampler-poly/Sampler.cpp, held frame by frame to the copy
 * in test/agbau/bela_reference.h. What it keeps, deliberately, including the
 * parts that look like accidents:
 *
 *  - Gain 0.5 per voice, no limiter of its own (Sampler.cpp:113). The engine's
 *    soft knee sits after the sum, which the Bela never had.
 *  - A note-on picks a random take and restarts from frame 0 even while the
 *    voice is sounding, with the envelope climbing from wherever it was. On a
 *    roll that is a jump in the waveform at high gain and may click. The
 *    instrument is played live like that today; whether to change it is a
 *    decision for after it has been heard on this board.
 *  - When a one-shot take runs out, the envelope is released and then FROZEN:
 *    Bela's process() returns before advancing it once the sample has ended.
 *    So the next attack starts from wherever it froze - on a short percussion
 *    take that is mid-decay, not zero. A voice that is not playing simply is
 *    not rendered here, which freezes it the same way by construction.
 *  - Whether note-off releases the envelope is a per-voice flag: the Agbau's
 *    percussion ignores note-off and lets its own decay be the whole hit.
 *
 * And one thing it does not keep, which changes no output sample: a voice
 * stops being rendered once its envelope is silent(). The Bela left a
 * released ganza looping at zero gain forever, one voice of CPU per ganza.
 *
 * Every method here is for the audio task. loop() talks to a VoiceBank only
 * through its NoteQueue - see note_queue.h for why.
 */
#include <stdint.h>
#include "audio_block.h"
#include "envelope.h"
#include "note_queue.h"

#ifndef VOICE_MAX_TAKES
  #define VOICE_MAX_TAKES 8
#endif

// Sampler.cpp:113 - every voice at half gain, summed with no other scaling.
#define VOICE_GAIN 0.5f

struct VoiceTake {
  const int16_t* data;
  uint32_t       frames;
};

// What a voice is, as opposed to what it plays. The Agbau has three of these
// (agbau/src/agbau_kit.h); all sixteen voices are one of them.
struct VoiceConfig {
  float attack, decay, sustain, release;   // seconds, seconds, level, seconds
  bool  loop;                              // loop the take while held (ganza)
  bool  releaseOnNoteOff;                  // false: note-off is ignored
};

/* xorshift32 (Marsaglia). Bela used std::rand() % takes; this is the same
 * idea without libc state: header-only, seedable, identical on host and
 * board. Never seeded with 0 - xorshift has a fixed point there. */
struct XorShift32 {
  uint32_t s = 0x9E3779B9u;
  void     seed(uint32_t v) { s = v ? v : 0x9E3779B9u; }
  uint32_t next() {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
  }
};

class Voice {
 public:
  void configure(const VoiceConfig& c, float sampleRate) {
    _env.set(c.attack, c.decay, c.sustain, c.release, sampleRate);
    _loop = c.loop;
    _releaseOnNoteOff = c.releaseOnNoteOff;
  }

  /* Add one take. Done at boot, before audio starts; false when full or when
   * the take is empty, which would make the read position meaningless. */
  bool addTake(const int16_t* data, uint32_t frames) {
    if (_count >= VOICE_MAX_TAKES || data == nullptr || frames == 0) return false;
    _takes[_count++] = VoiceTake{data, frames};
    return true;
  }

  void seed(uint32_t s) { _rng.seed(s); }

  /* Bela's Sampler::trigger(): pick a take, rewind, attack from here. */
  void noteOn() {
    if (_count == 0) return;
    _take = _count > 1 ? (uint8_t)(_rng.next() % _count) : 0;
    _pos = 0;
    _playing = true;
    _env.trigger();
  }

  /* Bela's noteOff(): only voices that obey note-off are released. Applied
   * even to a voice that has stopped, as Bela does - it moves the frozen
   * envelope into its release state without advancing it. */
  void noteOff() {
    if (_releaseOnNoteOff) _env.release();
  }

  /* Add this voice's next `frames` frames into l and r (mono, both sides).
   * Voice-outer, frame-inner: the caller loops voices, this loops frames, so
   * each voice walks consecutive memory of its own take. */
  void render(float* l, float* r, int frames) {
    if (!_playing) return;
    const int16_t* data = _takes[_take].data;
    const uint32_t len  = _takes[_take].frames;
    for (int n = 0; n < frames; n++) {
      float x = VOICE_GAIN * audioFromI16(data[_pos]) * _env.next();
      l[n] += x;
      r[n] += x;
      if (++_pos >= len) {
        if (_loop) {
          _pos = 0;
        } else {
          _env.release();          // and freeze: nothing advances it from here
          _playing = false;
          return;
        }
      }
    }
    // Once per block, not per frame: an envelope that went silent mid-block
    // only multiplies the rest of that block by zero.
    if (_env.silent()) _playing = false;
  }
  void render(AudioBlock& b) { render(b.l, b.r, AUDIO_BLOCK_FRAMES); }

  bool            playing() const { return _playing; }
  uint8_t         take() const { return _take; }
  uint8_t         takeCount() const { return _count; }
  const Envelope& envelope() const { return _env; }

 private:
  VoiceTake  _takes[VOICE_MAX_TAKES];
  uint8_t    _count            = 0;
  uint8_t    _take             = 0;
  uint32_t   _pos              = 0;
  bool       _playing          = false;
  bool       _loop             = false;
  bool       _releaseOnNoteOff = true;
  Envelope   _env;
  XorShift32 _rng;
};

/*
 * N voices and the queue that feeds them: the whole of the Agbau's sound
 * generation, short of the mix bus.
 *
 * loop() calls post(); the audio task calls render() once per block, which
 * first applies every request posted since the last block, in order, and
 * then renders every voice into the block. Nothing else crosses the two.
 */
template <int N>
class VoiceBank {
 public:
  Voice voices[N];

  /* loop() side. Out-of-range voices are dropped here, before they can
   * index anything on the audio side. */
  bool post(uint8_t voice, bool on) {
    if (voice >= N) return false;
    return _queue.post(voice, on);
  }

  /* Audio task side. Adds into l and r - the caller silences the block. */
  void render(float* l, float* r, int frames) {
    NoteEvent e;
    while (_queue.take(e)) {
      if (e.on) voices[e.voice].noteOn();
      else      voices[e.voice].noteOff();
    }
    for (int v = 0; v < N; v++) voices[v].render(l, r, frames);
  }
  void render(AudioBlock& b) { render(b.l, b.r, AUDIO_BLOCK_FRAMES); }

  /* Which voices are sounding, one bit each - for DEBUG_ENGINE. Read from
   * loop() without synchronisation: a stale bit costs one wrong line. */
  uint32_t playingMask() const {
    uint32_t m = 0;
    for (int v = 0; v < N; v++)
      if (voices[v].playing()) m |= 1u << v;
    return m;
  }

  uint32_t dropped() const { return _queue.dropped(); }

 private:
  NoteQueue _queue;
};
