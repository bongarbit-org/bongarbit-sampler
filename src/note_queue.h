#pragma once
/*
 * Note requests from loop() to the audio task: a single-producer,
 * single-consumer queue, lock-free and allocation-free.
 *
 * Compiled into the firmware AND into the host test suite, same contract as
 * every header here.
 *
 * WHY A QUEUE AND NOT A CALL
 *
 * In the sampler, loop() calls samp.start() directly, and the race that has
 * with the audio task costs at most one block - start() only rewinds a read
 * position on a buffer that never changes. An Agbau note-on is different: it
 * picks a new take, which is a new pointer AND a new length. If loop() wrote
 * those while the audio task was mid-block, the task could pair a long take's
 * length with a short take's pointer and read past the end of it. So loop()
 * never touches a voice. It posts here, and the audio task applies everything
 * posted at the top of its next block - the same rule as
 * PitchShift::setEngaged(), for the same reason, generalised from one flag to
 * an ordered stream of events.
 *
 * Ordered matters: a note-on and its note-off posted between two blocks must
 * arrive as a trigger followed by a release, not as whichever a pair of flags
 * happened to be checked first.
 *
 * std::atomic rather than volatile, unlike PitchShift's single bool: a queue
 * publishes a slot and THEN an index, and that ordering is what the
 * release/acquire pair guarantees across cores. volatile does not.
 */
#include <stdint.h>
#include <atomic>

// Power of two, for the mask. 64 events between two blocks (0.73ms) is more
// than sixteen keys can produce; overflow is counted, never silent.
#ifndef NOTE_QUEUE_SIZE
  #define NOTE_QUEUE_SIZE 64
#endif
static_assert((NOTE_QUEUE_SIZE & (NOTE_QUEUE_SIZE - 1)) == 0,
              "NOTE_QUEUE_SIZE must be a power of two");

struct NoteEvent {
  uint8_t voice;
  bool    on;      // true = note-on, false = note-off
};

class NoteQueue {
 public:
  /* Producer side: loop() only. False if the queue is full; the event is
   * dropped and counted rather than blocking the caller. */
  bool post(uint8_t voice, bool on) {
    uint32_t h = _head.load(std::memory_order_relaxed);
    uint32_t t = _tail.load(std::memory_order_acquire);
    if (h - t >= NOTE_QUEUE_SIZE) {
      _dropped++;
      return false;
    }
    _buf[h & (NOTE_QUEUE_SIZE - 1)] = NoteEvent{voice, on};
    _head.store(h + 1, std::memory_order_release);
    return true;
  }

  /* Consumer side: the audio task only. False when there is nothing left. */
  bool take(NoteEvent& e) {
    uint32_t t = _tail.load(std::memory_order_relaxed);
    uint32_t h = _head.load(std::memory_order_acquire);
    if (t == h) return false;
    e = _buf[t & (NOTE_QUEUE_SIZE - 1)];
    _tail.store(t + 1, std::memory_order_release);
    return true;
  }

  // Written by the producer only; for a diagnostic line from loop().
  uint32_t dropped() const { return _dropped; }

 private:
  NoteEvent             _buf[NOTE_QUEUE_SIZE];
  std::atomic<uint32_t> _head{0};
  std::atomic<uint32_t> _tail{0};
  uint32_t              _dropped = 0;
};
