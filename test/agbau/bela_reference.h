#pragma once
/*
 * The Agbau's Bela voice, copied on purpose, as the REFERENCE the ESP32 port
 * is tested against.
 *
 * Same role mixer.h plays for the soft knee: this is the code whose sound was
 * approved by playing the instrument live, so the port is held to it frame by
 * frame rather than to a description of it. It is a copy - Ramp, ADSR and
 * Sampler::process() from agbau/Bela/agbau-sampler-poly/, logic unchanged,
 * Bela headers and file loading stripped, classes renamed so they cannot
 * collide with the port. It is never compiled into the firmware.
 *
 * Original: "C++ Real-Time Audio Programming with Bela", Lectures 2 and 16
 * (bela.io), as adapted in the Agbau repository.
 *
 * Kept exactly as found, including the parts the port deliberately does not
 * reproduce (see agbau/src/envelope.h):
 *  - rampTo(x, 0) sets counter 0 and divides by zero for the increment, which
 *    is never applied: a zero-time segment does not move at all.
 *  - Segments accumulate an increment per frame, so they end within float
 *    rounding of their target rather than on it. The sustain of a "1.0"
 *    envelope is 1 +- a few ulp; the end of a decay to "0" is a small residual.
 *  - Sampler::process() returns before advancing the envelope once the sample
 *    has ended, so the envelope freezes wherever it was. This one the port
 *    DOES reproduce - it decides where the next attack starts from.
 */
#include <vector>

class BelaRamp {
 public:
  void setSampleRate(float rate) { sampleRate_ = rate; }
  void rampTo(float value, float time) {
    increment_ = (value - currentValue_) / (sampleRate_ * time);
    counter_ = (int)(sampleRate_ * time);
  }
  float process() {
    if (counter_ > 0) {
      counter_--;
      currentValue_ += increment_;
    }
    return currentValue_;
  }
  bool finished() { return counter_ == 0; }
  float currentLevel() const { return currentValue_; }

 private:
  float sampleRate_   = 1;
  float currentValue_ = 0;
  float increment_    = 0;
  int   counter_      = 0;
};

class BelaADSR {
 public:
  enum State { StateOff = 0, StateAttack, StateDecay, StateSustain, StateRelease };

  void setSampleRate(float rate) { ramp_.setSampleRate(rate); }

  void trigger() {
    ramp_.rampTo(0.0, 0.0001);      // overwritten on the next line - no effect
    state_ = StateAttack;
    ramp_.rampTo(1.0, attackTime_);
  }
  void release() {
    state_ = StateRelease;
    ramp_.rampTo(0.0, releaseTime_);
  }
  float process() {
    if (state_ == StateOff) {
    } else if (state_ == StateAttack) {
      if (ramp_.finished()) {
        state_ = StateDecay;
        ramp_.rampTo(sustainLevel_, decayTime_);
      }
    } else if (state_ == StateDecay) {
      if (ramp_.finished()) state_ = StateSustain;
    } else if (state_ == StateSustain) {
    } else if (state_ == StateRelease) {
      if (ramp_.finished()) state_ = StateOff;
    }
    return ramp_.process();
  }
  bool  isActive() const { return state_ != StateOff; }
  State state() const { return state_; }

  void setAttackTime(float t)   { attackTime_ = t >= 0 ? t : 0; }
  void setDecayTime(float t)    { decayTime_ = t >= 0 ? t : 0; }
  void setSustainLevel(float s) { sustainLevel_ = s < 0 ? 0 : (s > 1 ? 1 : s); }
  void setReleaseTime(float t)  { releaseTime_ = t >= 0 ? t : 0; }

 private:
  float attackTime_   = 0.001f;
  float decayTime_    = 0.001f;
  float sustainLevel_ = 1;
  float releaseTime_  = 0.001f;
  State state_        = StateOff;
  BelaRamp ramp_;
};

// Sampler: the parts that make sound. Take selection is passed in instead of
// std::rand() so a test can make both sides pick the same take.
struct BelaSampler {
  std::vector<std::vector<float>> buffers;
  int  readPointer      = -1;
  int  sampleSelector   = 0;
  bool releaseOnNoteOff = true;
  bool loopMode         = false;
  BelaADSR env;

  BelaSampler() { env.setSampleRate(44100.0f); }

  void setAdsrParameters(float a, float d, float s, float r) {
    env.setAttackTime(a);
    env.setDecayTime(d);
    env.setSustainLevel(s);
    env.setReleaseTime(r);
  }
  void trigger(int take) {             // Sampler.cpp:36-40
    sampleSelector = take;
    readPointer = 0;
    env.trigger();
  }
  void noteOff() {                     // render.cpp: noteOff() consults the flag
    if (releaseOnNoteOff) env.release();
  }
  float process() {                    // Sampler.cpp:108-126
    if (readPointer == -1) return 0.f;
    float out = 0.5 * buffers[sampleSelector][readPointer] * env.process();
    readPointer++;
    if (readPointer >= static_cast<int>(buffers[sampleSelector].size())) {
      if (loopMode) {
        readPointer = 0;
      } else {
        env.release();
        readPointer = -1;
      }
    }
    return out;
  }
};
