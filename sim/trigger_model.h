/*
 * trigger_model.h - The model under test: trigger state machines transcribed
 * from src/main.cpp, a virtual pin-trace generator, an RC filter model, and
 * the runner that scores a scheme against a trace.
 *
 * Header-only and free of I/O so it has exactly one definition shared by both
 * consumers: sim/trigger_sim.cpp (the sweep/report CLI) and
 * test/test_trigger (the unit suite). Duplicating any of this into the tests
 * would mean the tests stop testing what the tool reports.
 *
 * TIME TYPES MATTER HERE. Scheme timestamps are uint32_t, matching the
 * firmware's micros(), so that unsigned-difference arithmetic wraps in the
 * simulation exactly as it does on the board at ~71 minutes. The runner keeps
 * a separate uint64_t clock for scoring, and can start a trace at an arbitrary
 * uint32_t base so the wrap boundary itself is testable.
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The firmware's own trigger state machine, compiled into this binary.
// Not a model of it - the same translation unit the board runs.
#include "trigger_input.h"

static const int PIN_HIGH = 1; // idle, INPUT_PULLUP
static const int PIN_LOW  = 0; // pressed

// --- Virtual pin trace --------------------------------------------------

struct Edge { uint64_t t_us; int level; };

struct Trace {
    std::string           name;
    std::vector<Edge>     edges;  // chronological; pin idles HIGH before the first
    std::vector<uint64_t> hits;   // when a human actually pressed - ground truth
    uint64_t              duration_us = 0;
};

inline int levelAt(const Trace& tr, uint64_t t_us) {
    int lvl = PIN_HIGH;
    for (const Edge& e : tr.edges) {
        if (e.t_us > t_us) break;
        lvl = e.level;
    }
    return lvl;
}

// Append a press whose contact bounces for `bounce_us` before settling LOW,
// then a release that bounces the same way before settling HIGH. `bounces` is
// the number of transitions, so one contact-open segment lasts
// bounce_us/bounces - that segment length, not the total, is what decides
// whether Bounce2 can latch onto a spurious stable state mid-bounce.
inline void addPress(Trace& tr, uint64_t at_us, uint64_t bounce_us,
                     uint64_t hold_us, int bounces) {
    tr.hits.push_back(at_us);
    for (int i = 0; i < bounces; i++) {
        uint64_t t = at_us + (bounce_us * i) / bounces;
        tr.edges.push_back({t, (i % 2 == 0) ? PIN_LOW : PIN_HIGH});
    }
    tr.edges.push_back({at_us + bounce_us, PIN_LOW});

    uint64_t rel = at_us + hold_us;
    for (int i = 0; i < bounces; i++) {
        uint64_t t = rel + (bounce_us * i) / bounces;
        tr.edges.push_back({t, (i % 2 == 0) ? PIN_HIGH : PIN_LOW});
    }
    tr.edges.push_back({rel + bounce_us, PIN_HIGH});
}

inline Trace makeRoll(const std::string& name, int count, uint64_t interval_us,
                      uint64_t bounce_us, uint64_t hold_us, int bounces) {
    Trace t; t.name = name;
    const uint64_t start = 10000;
    for (int i = 0; i < count; i++)
        addPress(t, start + interval_us * i, bounce_us, hold_us, bounces);
    t.duration_us = start + interval_us * count + hold_us + bounce_us + 200000;
    return t;
}

inline Trace makeGlitch(const std::string& name, uint64_t width_us) {
    Trace t; t.name = name; t.duration_us = 200000;
    t.edges.push_back({50000, PIN_LOW});
    t.edges.push_back({50000 + width_us, PIN_HIGH});
    return t; // no hits: every fire is spurious by construction
}

// --- Hardware RC debounce (docs/circuit-rc.md) --------------------------
//
//        3V3 --[R1]--+-- GPIO          R1 in parallel with the ESP32's
//                    |                 internal INPUT_PULLUP (~45k)
//                    +--[R2]-- switch -- GND
//                    |
//                   [C]
//                    |
//                   GND
//
// Switch open:   node charges toward VDD through R1,        tau = R1*C
// Switch closed: node decays toward VDD*R2/(R1+R2),         tau = (R1||R2)*C
//
// The two time constants land on opposite edges, which is the whole point:
// the strike is fast, the release is filtered. Integrated numerically and
// passed through a Schmitt trigger, then handed to the same schemes as any
// other trace - so the comparison is like for like.

struct RCParams {
    double r_pullup   = 10000;   // R1, external
    double r_series   = 220;     // R2, in series with the switch
    double c_farad    = 100e-9;  // C
    double r_internal = 45000;   // ESP32 INPUT_PULLUP, in parallel with R1; 0 disables
    double vdd        = 3.3;
    double vih_frac   = 0.75;    // ESP32 datasheet logic levels
    double vil_frac   = 0.25;
};

struct RCDerived { double rup, tau_open_us, tau_closed_us, v_closed, vih, vil; };

inline RCDerived rcDerive(const RCParams& p) {
    RCDerived d;
    d.rup = (p.r_internal > 0) ? (p.r_pullup * p.r_internal) / (p.r_pullup + p.r_internal)
                               : p.r_pullup;
    double rth_closed = (d.rup * p.r_series) / (d.rup + p.r_series);
    d.v_closed      = p.vdd * p.r_series / (d.rup + p.r_series);
    d.tau_open_us   = d.rup * p.c_farad * 1e6;
    d.tau_closed_us = rth_closed * p.c_farad * 1e6;
    d.vih = p.vdd * p.vih_frac;
    d.vil = p.vdd * p.vil_frac;
    return d;
}

// Turn an ideal switch trace into what the GPIO actually sees through the RC.
// Integrated at step_us (1us default; must be small next to tau_closed).
inline Trace applyRC(const Trace& in, const RCParams& p, uint64_t step_us = 1) {
    RCDerived d = rcDerive(p);
    Trace out;
    out.name        = in.name;
    out.hits        = in.hits;
    out.duration_us = in.duration_us;

    double dt       = (double)step_us * 1e-6;
    double a_open   = 1.0 - std::exp(-dt / (d.tau_open_us   * 1e-6));
    double a_closed = 1.0 - std::exp(-dt / (d.tau_closed_us * 1e-6));

    double v     = p.vdd;
    int    logic = PIN_HIGH;
    size_t ei    = 0;      // walk input edges forward instead of rescanning
    int    sw    = PIN_HIGH;

    for (uint64_t t = 0; t < in.duration_us; t += step_us) {
        while (ei < in.edges.size() && in.edges[ei].t_us <= t) sw = in.edges[ei++].level;
        if (sw == PIN_LOW) v += (d.v_closed - v) * a_closed;
        else               v += (p.vdd      - v) * a_open;

        int nl = logic;                       // Schmitt: hysteresis between the two
        if      (v >= d.vih) nl = PIN_HIGH;
        else if (v <= d.vil) nl = PIN_LOW;
        if (nl != logic) { logic = nl; out.edges.push_back({t, logic}); }
    }
    return out;
}

// --- Scheme A: GPIO4, wait-for-stable (src/main.cpp:174) ----------------

struct WaitStable {
    uint32_t debounce_ms;
    int      lastRaw          = PIN_HIGH;
    int      buttonState      = PIN_HIGH;
    uint32_t lastDebounceTime = 0;

    bool update(uint32_t now_us, int reading) {
        uint32_t now_ms = (uint32_t)(now_us / 1000);
        bool fired = false;
        if (reading != lastRaw) lastDebounceTime = now_ms;
        if ((now_ms - lastDebounceTime) > debounce_ms) {
            if (reading != buttonState) {
                buttonState = reading;
                if (buttonState == PIN_LOW) fired = true; // samp.start()
            }
        }
        lastRaw = reading;
        return fired;
    }
};

// --- Bounce2, faithful to the library ------------------------------------

struct Bounce2Model {
    uint32_t interval_ms;
    int      lastRead   = PIN_HIGH;
    int      debounced  = PIN_HIGH;
    uint32_t lastChange = 0;
    bool     changed    = false;

    void update(uint32_t now_ms, int reading) {
        changed = false; // Debouncer::update() unsets CHANGED_STATE on line 1
        if (reading != lastRead) { lastChange = now_ms; lastRead = reading; }
        if ((now_ms - lastChange) >= interval_ms && debounced != reading) {
            debounced = reading;
            changed   = true;
        }
    }
    int  read() const { return debounced; }
    bool rose() const { return changed && debounced == PIN_HIGH; }
};

// --- Scheme B variants: pads (src/main.cpp:191) --------------------------

// The pad logic as written BEFORE the fix. Rearms on the debounced LEVEL,
// which still reads HIGH for interval_ms after the press -> machine-gun.
struct RawFire {
    Bounce2Model bounce;
    bool         armed = true;

    bool update(uint32_t now_us, int reading) {
        bounce.update((uint32_t)(now_us / 1000), reading);
        bool fired = false;
        if (armed) {
            if (reading == PIN_LOW) { fired = true; armed = false; }
        } else if (bounce.read() == PIN_HIGH) {
            armed = true;
        }
        return fired;
    }
};

// The fix: rearm on the debounced rising EDGE. Costs a latch - rearming needs
// a clean-HIGH window of at least interval_ms.
struct RawFireRose {
    Bounce2Model bounce;
    bool         armed = true;

    bool update(uint32_t now_us, int reading) {
        bounce.update((uint32_t)(now_us / 1000), reading);
        bool fired = false;
        if (armed) {
            if (reading == PIN_LOW) { fired = true; armed = false; }
        } else if (bounce.rose()) {
            armed = true;
        }
        return fired;
    }
};

// Candidate: require the raw LOW to persist confirm_us before firing. Buys
// glitch immunity for a sub-millisecond latency cost.
struct RawFireConfirm {
    Bounce2Model bounce;
    uint32_t     confirm_us;
    bool         armed        = true;
    bool         pending      = false;
    uint32_t     pendingSince = 0;

    bool update(uint32_t now_us, int reading) {
        bounce.update((uint32_t)(now_us / 1000), reading);
        bool fired = false;
        if (armed) {
            if (reading == PIN_LOW) {
                if (!pending) { pending = true; pendingSince = now_us; }
                else if (now_us - pendingSince >= confirm_us) {
                    fired = true; armed = false; pending = false;
                }
            } else {
                pending = false; // went back HIGH - was a glitch, not a press
            }
        } else if (bounce.rose()) {
            armed = true; pending = false;
        }
        return fired;
    }
};

// --- The shipped scheme -------------------------------------------------
// This is not a transcription. TriggerInput comes straight from
// src/trigger_input.h, so a sweep or a test exercising it is exercising the
// code that goes on the board. The wrapper exists only to bridge conventions:
// the sim describes a pin as int/PIN_LOW, the firmware as bool low.
//
// It replaced a hand-written RawFireRelease that had already drifted - it grew
// a confirm_us == 0 fast path the firmware never had, on the exact
// configuration docs/circuit-rc.md tells you to use at the bench.
struct TriggerInputScheme {
    TriggerInput t;
    bool update(uint32_t now_us, int reading) {
        return t.update(now_us, reading == PIN_LOW);
    }
};

// Named construction, because {a, b} on two same-typed windows is exactly the
// sort of thing that gets silently transposed.
inline TriggerInputScheme trig(uint32_t confirm_us, uint32_t release_ms) {
    TriggerInputScheme s;
    s.t.confirm_us = confirm_us;
    s.t.release_ms = release_ms;
    return s;
}

// --- Runner -------------------------------------------------------------

struct Result {
    int      fires       = 0;
    int      spurious    = 0;  // fires not attributable to a real press
    int      missed      = 0;  // real presses that produced no fire
    int      matchedHits = 0;
    uint64_t worstLat_us = 0;
    uint64_t totalLat_us = 0;

    double meanLat_ms() const {
        return matchedHits ? (double)totalLat_us / matchedHits / 1000.0 : 0.0;
    }
    bool clean(size_t expected) const {
        return spurious == 0 && missed == 0 && fires == (int)expected;
    }
};

template <typename Scheme>
static Result run(const Trace& tr, Scheme scheme, uint64_t loopPeriod_us,
                  uint64_t latWindow_us = 60000, uint32_t timeBase_us = 0) {
    Result r;
    std::vector<bool> credited(tr.hits.size(), false);

    for (uint64_t t = 0; t < tr.duration_us; t += loopPeriod_us) {
        int reading = levelAt(tr, t);
        if (!scheme.update((uint32_t)(timeBase_us + t), reading)) continue;

        r.fires++;
        int owner = -1;
        for (size_t i = 0; i < tr.hits.size(); i++)
            if (tr.hits[i] <= t && t - tr.hits[i] <= latWindow_us) owner = (int)i;

        if (owner < 0 || credited[owner]) { r.spurious++; continue; }
        credited[owner] = true;
        uint64_t lat = t - tr.hits[owner];
        r.matchedHits++;
        r.totalLat_us += lat;
        if (lat > r.worstLat_us) r.worstLat_us = lat;
    }
    r.missed = (int)tr.hits.size() - r.matchedHits;
    return r;
}

// --- Config -------------------------------------------------------------

struct Config {
    uint64_t loop_us     = 50;
    uint32_t debounce_ms = 10;    // PAD_DEBOUNCE_MS
    uint64_t hold_ms     = 40;
    uint64_t segment_us  = 333;   // one contact-open segment
    int      bounces     = 6;
    uint32_t confirm_us  = 200;
    uint32_t release_ms  = 3;     // release window for RawFireRelease
    int      hits        = 12;
};

struct Row { Result rawFire, rose, confirm, release; };

inline Row runAll(const Trace& tr, const Config& c, uint64_t latWindow) {
    Row row;
    row.rawFire = run(tr, RawFire{{c.debounce_ms}},                   c.loop_us, latWindow);
    row.rose    = run(tr, RawFireRose{{c.debounce_ms}},               c.loop_us, latWindow);
    row.confirm = run(tr, RawFireConfirm{{c.debounce_ms}, c.confirm_us}, c.loop_us, latWindow);
    row.release = run(tr, trig(0, c.release_ms),                       c.loop_us, latWindow);
    return row;
}
