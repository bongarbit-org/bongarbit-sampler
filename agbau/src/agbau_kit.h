#pragma once
/*
 * The Agbau kit: which takes each of the sixteen voices plays, how each kind
 * of voice behaves, and which key of the 4x4 matrix plays which voice.
 *
 * Compiled into the firmware AND into the host test suite. Transcribed from
 * the two programs it replaces, and tested against transcriptions of them
 * (test_voice): the Bela's render.cpp for the voices, the Arduino's agbau.ino
 * for the keys. agbau/tools/agbau_kit.py carries its own copy of the voice list
 * and of the percussion preset, to know where the envelope ends - change one,
 * change the other.
 */
#include <stdint.h>
#include "voice.h"

#define AGBAU_VOICES    16
#define AGBAU_ROWS      4
#define AGBAU_COLS      4
#define AGBAU_MAX_TAKES 5

enum AgbauGroup : uint8_t { AGBAU_BASS = 0, AGBAU_PERC, AGBAU_GANZA };

// render.cpp:84-104. One envelope class, two opposite ways of using it - see
// agbau/docs/migration.md, step 1.
static const VoiceConfig AGBAU_PRESETS[3] = {
  // attack decay sustain release  loop   releaseOnNoteOff
  {  0.01f, 0.0f, 1.0f,   1.0f,    false, true  },   // bass: held note, 1s release
  {  0.01f, 3.0f, 0.0f,   3.0f,    false, false },   // percussion: the decay IS the hit
  {  0.01f, 0.0f, 1.0f,   0.1f,    true,  true  },   // ganza: loops while held
};

struct AgbauVoiceDef {
  AgbauGroup  group;
  uint8_t     takeCount;
  const char* takes[AGBAU_MAX_TAKES];
};

// Voice i is Bela's samplers[i], played by MIDI note 60 + i: bass b1-b8, then
// percussion p1-p8, with p3 and p7 overridden into ganzas. Take names are file
// stems - the SD path appends ".wav", the flash image stores them as is.
static const AgbauVoiceDef AGBAU_VOICE_DEFS[AGBAU_VOICES] = {
  {AGBAU_BASS,  1, {"b1"}},
  {AGBAU_BASS,  1, {"b2"}},
  {AGBAU_BASS,  1, {"b3"}},
  {AGBAU_BASS,  1, {"b4"}},
  {AGBAU_BASS,  1, {"b5"}},
  {AGBAU_BASS,  1, {"b6"}},
  {AGBAU_BASS,  1, {"b7"}},
  {AGBAU_BASS,  1, {"b8"}},
  {AGBAU_PERC,  5, {"p1.3.1", "p1.3.2", "p1.3.3", "p1.3.4", "p1.3.5"}},
  {AGBAU_PERC,  1, {"p2.2.1"}},
  {AGBAU_GANZA, 1, {"p3.1"}},
  {AGBAU_PERC,  1, {"p4.3.1"}},
  {AGBAU_PERC,  3, {"p5.3.1", "p5.3.2", "p5.3.3"}},
  {AGBAU_PERC,  3, {"p6.3.1", "p6.3.2", "p6.3.3"}},
  {AGBAU_GANZA, 1, {"p7.1"}},
  {AGBAU_PERC,  1, {"p8.3.1"}},
};

/*
 * Key -> voice. The Arduino's Keypad numbers a key kcode = row * 4 + col
 * (Keypad.cpp:120-121) and agbau.ino sends it as MIDI note
 * notaMidi[kcode] = 60 + 4 * col + row; the Bela plays note 60 + i on voice
 * i. So columns 0-1 are the basses, columns 2-3 the percussion, and the two
 * ganzas sit on row 2 of columns 2 and 3. Row and column are the Keypad's:
 * rows are the inputs read with pull-ups, columns the outputs strobed low.
 */
static inline uint8_t agbauVoiceForKey(uint8_t row, uint8_t col) {
  return (uint8_t)(4 * col + row);
}

/*
 * Give every voice its preset and its takes. `find(name, &take)` is the
 * storage: a lookup in the mapped KitImage, or in whatever was loaded from SD.
 * Returns how many takes could not be found; a voice with none stays silent
 * rather than failing the boot, and the caller says which.
 */
template <typename Find>
int agbauConfigure(VoiceBank<AGBAU_VOICES>& bank, Find find, float sampleRate,
                   uint32_t seed) {
  int missing = 0;
  for (int v = 0; v < AGBAU_VOICES; v++) {
    const AgbauVoiceDef& d = AGBAU_VOICE_DEFS[v];
    Voice& voice = bank.voices[v];
    voice.configure(AGBAU_PRESETS[d.group], sampleRate);
    // Distinct, never-zero seeds, so no two voices walk the same sequence.
    voice.seed(seed + 0x9E3779B9u * (uint32_t)(v + 1));
    for (int t = 0; t < d.takeCount; t++) {
      VoiceTake take{nullptr, 0};
      if (!find(d.takes[t], &take) || !voice.addTake(take.data, take.frames))
        missing++;
    }
  }
  return missing;
}
