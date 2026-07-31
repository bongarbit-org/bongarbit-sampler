/*
 * trigger_sim.cpp - Host-side simulation of the trigger schemes in src/main.cpp.
 * Builds and runs on a normal PC, no board and no PlatformIO required:
 *
 *     g++ -O2 -std=c++17 -o /tmp/trigger_sim sim/trigger_sim.cpp
 *     /tmp/trigger_sim                  # 6-trace report (regression baseline)
 *     /tmp/trigger_sim 3000             # same, with a 3000us loop() period
 *     /tmp/trigger_sim --sweep=all      # parameter sweeps
 *
 * The state machines are transcribed from src/main.cpp, not reinvented:
 * WaitStable mirrors the GPIO4 path (main.cpp:174); RawFire mirrors the pad
 * path as it was written before the rose() fix; RawFireRose mirrors it after.
 * Bounce2Model reproduces the real Bounce2 library's "stable for interval"
 * algorithm, including the detail the bug hinged on - the CHANGED flag is
 * cleared at the top of every update(), so rose() is true for exactly one
 * update() per debounced transition.
 *
 * RawFireConfirm and RawFireLockout are candidate alternatives, here to be
 * measured against the other two rather than argued about.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

static int levelAt(const Trace& tr, uint64_t t_us) {
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
static void addPress(Trace& tr, uint64_t at_us, uint64_t bounce_us,
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

static Trace makeRoll(const std::string& name, int count, uint64_t interval_us,
                      uint64_t bounce_us, uint64_t hold_us, int bounces) {
    Trace t; t.name = name;
    const uint64_t start = 10000;
    for (int i = 0; i < count; i++)
        addPress(t, start + interval_us * i, bounce_us, hold_us, bounces);
    t.duration_us = start + interval_us * count + hold_us + bounce_us + 200000;
    return t;
}

static Trace makeGlitch(const std::string& name, uint64_t width_us) {
    Trace t; t.name = name; t.duration_us = 200000;
    t.edges.push_back({50000, PIN_LOW});
    t.edges.push_back({50000 + width_us, PIN_HIGH});
    return t; // no hits: every fire is spurious by construction
}

// --- Scheme A: GPIO4, wait-for-stable (src/main.cpp:174) ----------------

struct WaitStable {
    uint32_t debounce_ms;
    int      lastRaw          = PIN_HIGH;
    int      buttonState      = PIN_HIGH;
    uint32_t lastDebounceTime = 0;

    bool update(uint64_t now_us, int reading) {
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

    bool update(uint64_t now_us, int reading) {
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

    bool update(uint64_t now_us, int reading) {
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
    uint64_t     confirm_us;
    bool         armed        = true;
    bool         pending      = false;
    uint64_t     pendingSince = 0;

    bool update(uint64_t now_us, int reading) {
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

// Candidate: fire on raw LOW, then rearm only after the RAW pin has been
// continuously HIGH for release_ms.
//
// A plain time lockout measured from the fire instant does NOT work and was
// tried first: the release arrives `hold` milliseconds later, long after any
// sane lockout has expired, so the first HIGH of the release bounce rearms the
// pad and the next bounce edge fires a second sample. Confirmed in simulation
// (2 fires per press). A stable-release requirement is unavoidable.
//
// So this is the same SHAPE as the rose() fix - and it latches the same way -
// but the release window is its own constant instead of being welded to
// PAD_DEBOUNCE_MS, so it can be sized to the switch's actual bounce (0.1-1ms)
// rather than to a generic 10ms debounce default.
// confirm_us > 0 additionally requires the raw LOW to persist that long before
// firing, which is what buys glitch immunity. The two mechanisms are
// independent: release_ms guards re-fire, confirm_us guards phantom fire.
struct RawFireRelease {
    uint32_t release_ms;
    uint64_t confirm_us     = 0;
    bool     armed          = true;
    bool     highSinceValid = false;
    uint64_t highSince_us   = 0;
    bool     pending        = false;
    uint64_t pendingSince   = 0;

    bool update(uint64_t now_us, int reading) {
        bool fired = false;
        if (armed) {
            if (reading == PIN_LOW) {
                if (confirm_us == 0) {
                    fired = true; armed = false; highSinceValid = false;
                } else if (!pending) {
                    pending = true; pendingSince = now_us;
                } else if (now_us - pendingSince >= confirm_us) {
                    fired = true; armed = false;
                    highSinceValid = false; pending = false;
                }
            } else {
                pending = false; // bounced back HIGH - glitch, not a press
            }
        } else if (reading == PIN_LOW) {
            highSinceValid = false; // release not clean yet, restart the window
        } else if (!highSinceValid) {
            highSinceValid = true; highSince_us = now_us;
        } else if (now_us - highSince_us >= (uint64_t)release_ms * 1000) {
            armed = true;
        }
        return fired;
    }
};

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
                  uint64_t latWindow_us = 60000) {
    Result r;
    std::vector<bool> credited(tr.hits.size(), false);

    for (uint64_t t = 0; t < tr.duration_us; t += loopPeriod_us) {
        int reading = levelAt(tr, t);
        if (!scheme.update(t, reading)) continue;

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
    uint64_t confirm_us  = 200;
    uint32_t release_ms  = 3;     // release window for RawFireRelease
    int      hits        = 12;
};

struct Row { Result rawFire, rose, confirm, release; };

static Row runAll(const Trace& tr, const Config& c, uint64_t latWindow) {
    Row row;
    row.rawFire = run(tr, RawFire{{c.debounce_ms}},                   c.loop_us, latWindow);
    row.rose    = run(tr, RawFireRose{{c.debounce_ms}},               c.loop_us, latWindow);
    row.confirm = run(tr, RawFireConfirm{{c.debounce_ms}, c.confirm_us}, c.loop_us, latWindow);
    row.release = run(tr, RawFireRelease{c.release_ms},               c.loop_us, latWindow);
    return row;
}

// --- Threshold reporting -------------------------------------------------
// Turns a column of pass/fail into a stated numeric boundary, compared
// against the closed-form prediction. This is what makes the sweep a
// self-checking experiment instead of a wall of numbers.

struct Sample { double value; bool clean; };

static void printThreshold(const char* scheme, const char* unit,
                           const std::vector<Sample>& s, bool wantFirstClean,
                           double predicted, const char* predExpr) {
    double found = -1;
    for (const Sample& x : s) {
        if (wantFirstClean ? x.clean : !x.clean) { found = x.value; break; }
    }
    if (found < 0) {
        printf("  THRESHOLD  %-18s no %s value in range\n", scheme,
               wantFirstClean ? "clean" : "failing");
        return;
    }
    printf("  THRESHOLD  %-18s %s = %g %s", scheme,
           wantFirstClean ? "first clean" : "first failing", found, unit);
    if (predicted > 0) {
        // Allow +2ms: millis() truncation plus Bounce2's >= comparison mean the
        // measured boundary lands one or two whole ms above the ideal formula.
        bool match = found >= predicted - 1.0 && found <= predicted + 2.0;
        printf("   predicted %s = %g  [%s]", predExpr, predicted,
               match ? "MATCH" : "MISMATCH");
    }
    printf("\n");
}

// --- Legacy 6-trace report (regression baseline) -------------------------

static void printRow(const char* label, const Trace& tr, const Result& r) {
    char lat[32];
    if (r.matchedHits == 0) snprintf(lat, sizeof lat, "%9s", "-");
    else snprintf(lat, sizeof lat, "%4.1f/%4.1f", r.meanLat_ms(),
                  (double)r.worstLat_us / 1000.0);
    printf("  %-22s %5d %8d %8d %9d  %s  %s\n", label, r.fires,
           (int)tr.hits.size(), r.missed, r.spurious, lat,
           r.clean(tr.hits.size()) ? "ok" : "FAIL");
}

static void legacyReport(const Config& c) {
    std::vector<Trace> traces;
    { Trace t{"clean press (no bounce)", {}, {}, 400000};
      addPress(t, 10000, 0, 150000, 0); traces.push_back(t); }
    { Trace t{"typical bounce (2ms)", {}, {}, 400000};
      addPress(t, 10000, 2000, 150000, 6); traces.push_back(t); }
    { Trace t{"worn switch (15ms)", {}, {}, 400000};
      addPress(t, 10000, 15000, 150000, 12); traces.push_back(t); }
    traces.push_back(makeGlitch("EMI glitch, 50us", 50));
    { Trace t{"held down 500ms", {}, {}, 800000};
      addPress(t, 10000, 2000, 500000, 6); traces.push_back(t); }
    traces.push_back(makeRoll("fast roll (8 @ 80ms)", 8, 80000, 2000, 40000, 6));

    printf("loop() poll period: %llu us\n", (unsigned long long)c.loop_us);
    printf("latency column: mean/worst in ms, from physical press to samp.start()\n");

    for (const Trace& tr : traces) {
        printf("\n%s\n", tr.name.c_str());
        printf("  %-22s %5s %8s %8s %9s %9s\n",
               "scheme", "fires", "expected", "missed", "spurious", "latency");
        printRow("wait-stable 25ms",   tr, run(tr, WaitStable{25}, c.loop_us));
        printRow("wait-stable 5ms",    tr, run(tr, WaitStable{5},  c.loop_us));
        printRow("wait-stable 0ms",    tr, run(tr, WaitStable{0},  c.loop_us));
        printRow("raw-fire (as-is)",   tr, run(tr, RawFire{{c.debounce_ms}},     c.loop_us));
        printRow("raw-fire (rose fix)", tr, run(tr, RawFireRose{{c.debounce_ms}}, c.loop_us));
        printRow("raw+confirm 200us",  tr, run(tr, RawFireConfirm{{c.debounce_ms}, c.confirm_us}, c.loop_us));
        printRow("raw+release 3ms",    tr, run(tr, RawFireRelease{c.release_ms}, c.loop_us));
        printRow("release+confirm",    tr, run(tr, RawFireRelease{c.release_ms, c.confirm_us}, c.loop_us));
    }
}

// --- Sweep D: loop period (harness self-check) ---------------------------

static void sweepLoop(Config c) {
    printf("\n########## SWEEP D - loop() period (HARNESS SELF-CHECK) ##########\n");
    printf("single press, 2ms bounce / 6 bounces, hold 150ms, debounce %ums\n",
           c.debounce_ms);
    printf("Must reproduce the originally measured 112 fires @50us and 3 @3000us.\n");
    printf("If it does not, the harness changed semantics and every other sweep\n");
    printf("in this run is suspect.\n\n");
    printf("  %10s %14s %14s %14s %14s\n", "loop us",
           "raw-fire fires", "rose fires", "confirm fires", "release fires");

    for (uint64_t lp : {20ULL, 50ULL, 100ULL, 200ULL, 500ULL, 1000ULL, 2000ULL,
                        3000ULL, 5000ULL}) {
        Trace t{"", {}, {}, 400000};
        addPress(t, 10000, 2000, 150000, 6);
        Config cc = c; cc.loop_us = lp;
        Row r = runAll(t, cc, 60000);
        printf("  %10llu %14d %14d %14d %14d\n", (unsigned long long)lp,
               r.rawFire.fires, r.rose.fires, r.confirm.fires, r.release.fires);
    }
    printf("\n  expected: raw-fire ~112 @50us and ~3 @3000us; all others exactly 1\n");
}

// --- Sweep A: retrigger interval (the rose() latch) ----------------------

static void sweepRetriggerDetail(Config c) {
    printf("\n########## SWEEP A - retrigger interval ##########\n");
    printf("hold %llums, segment %lluus x %d bounces, loop %lluus, debounce %ums, %d hits\n",
           (unsigned long long)c.hold_ms, (unsigned long long)c.segment_us,
           c.bounces, (unsigned long long)c.loop_us, c.debounce_ms, c.hits);
    printf("cell = missed/spurious;  expected fires = %d\n\n", c.hits);
    printf("  %8s %7s | %11s | %11s | %11s | %11s\n", "interval", "rate",
           "raw-fire", "rose fix", "confirm", "lockout");
    printf("  %8s %7s | %11s | %11s | %11s | %11s\n", "ms", "Hz",
           "miss/spur", "miss/spur", "miss/spur", "miss/spur");

    std::vector<uint64_t> intervals;
    for (uint64_t v : {20ULL, 25ULL, 30ULL, 35ULL, 40ULL, 45ULL}) intervals.push_back(v);
    for (uint64_t v = 46; v <= 60; v++) intervals.push_back(v);
    for (uint64_t v : {65ULL, 70ULL, 80ULL, 100ULL, 150ULL, 200ULL}) intervals.push_back(v);

    uint64_t bounce_us = c.segment_us * c.bounces;
    std::vector<Sample> sRose, sRel, sRaw;

    for (uint64_t iv : intervals) {
        if (iv * 1000 <= c.hold_ms * 1000) continue; // release must precede next press
        Trace t = makeRoll("", c.hits, iv * 1000, bounce_us, c.hold_ms * 1000, c.bounces);
        Row r = runAll(t, c, iv * 1000);
        printf("  %8llu %7.1f | %5d/%-5d | %5d/%-5d | %5d/%-5d | %5d/%-5d\n",
               (unsigned long long)iv, 1000.0 / (double)iv,
               r.rawFire.missed, r.rawFire.spurious,
               r.rose.missed,    r.rose.spurious,
               r.confirm.missed, r.confirm.spurious,
               r.release.missed, r.release.spurious);
        size_t n = t.hits.size();
        sRaw.push_back ({(double)iv, r.rawFire.clean(n)});
        sRose.push_back({(double)iv, r.rose.clean(n)});
        sRel.push_back ({(double)iv, r.release.clean(n)});
    }

    double predRose = (double)c.hold_ms + (double)bounce_us / 1000.0 + c.debounce_ms;
    double predRel  = (double)c.hold_ms + (double)bounce_us / 1000.0 + c.release_ms;
    printf("\n");
    printThreshold("rose fix", "ms", sRose, true, predRose, "hold+bounce+debounce");
    printThreshold("release 3ms", "ms", sRel, true, predRel, "hold+bounce+release");
    printThreshold("raw-fire (as-is)", "ms", sRaw, true, 0, "");
}

static void sweepRetriggerMatrix(Config c) {
    printf("\n--- threshold matrix: does the cliff track PAD_DEBOUNCE_MS 1:1? ---\n");
    printf("cell = first retrigger interval (ms) with zero missed and zero spurious\n\n");
    printf("  %14s", "rose fix:");
    for (uint32_t db : {5u, 10u, 20u}) printf("  debounce=%2ums", db);
    printf("\n");

    uint64_t bounce_us = c.segment_us * c.bounces;
    for (uint64_t hold : {10ULL, 20ULL, 40ULL, 80ULL}) {
        printf("  hold=%3llums   ", (unsigned long long)hold);
        for (uint32_t db : {5u, 10u, 20u}) {
            Config cc = c; cc.hold_ms = hold; cc.debounce_ms = db;
            double found = -1;
            for (uint64_t iv = hold + 1; iv <= hold + 80; iv++) {
                Trace t = makeRoll("", cc.hits, iv * 1000, bounce_us, hold * 1000, cc.bounces);
                if (run(t, RawFireRose{{db}}, cc.loop_us, iv * 1000).clean(t.hits.size())) {
                    found = (double)iv; break;
                }
            }
            double pred = (double)hold + (double)bounce_us / 1000.0 + db;
            printf("   %5g (%4g)", found, pred);
        }
        printf("\n");
    }
    printf("\n  format: measured (predicted = hold+bounce+debounce)\n");

    printf("\n  %14s", "release window:");
    printf("   (its own constant, not tied to PAD_DEBOUNCE_MS)\n");
    printf("  %14s", "");
    for (uint32_t rel : {1u, 3u, 5u, 10u}) printf("   release=%2ums", rel);
    printf("\n");
    for (uint64_t hold : {10ULL, 20ULL, 40ULL, 80ULL}) {
        printf("  hold=%3llums   ", (unsigned long long)hold);
        for (uint32_t rel : {1u, 3u, 5u, 10u}) {
            double found = -1;
            for (uint64_t iv = hold + 1; iv <= hold + 80; iv++) {
                Trace t = makeRoll("", c.hits, iv * 1000, bounce_us, hold * 1000, c.bounces);
                if (run(t, RawFireRelease{rel}, c.loop_us, iv * 1000).clean(t.hits.size())) {
                    found = (double)iv; break;
                }
            }
            printf("   %5g (%4g)", found,
                   (double)hold + (double)bounce_us / 1000.0 + rel);
        }
        printf("\n");
    }
    printf("\n  format: measured (predicted = hold+bounce+release);  -1 = never clean\n");
    printf("  note: hold DOMINATES the cap, and hold is the player's finger, not a\n");
    printf("  constant you get to choose. Shrinking the debounce window helps, but\n");
    printf("  it cannot buy much.\n");

    // --- The second, much nastier constraint on the rose() fix -------------
    // rose() can only fire if the debounced state went LOW first. That needs
    // the press to be held for at least debounce_ms. A shorter tap never
    // registers as a debounced LOW, so nothing ever rises, and the pad latches
    // off PERMANENTLY - no retrigger interval, however generous, recovers it.
    printf("\n--- contact time sweep, at a deliberately generous 300ms interval ---\n");
    printf("cell = missed of %d hits.  Interval is far above every cliff above, so\n", c.hits);
    printf("anything nonzero here is the OTHER failure: a tap too short to register.\n\n");
    printf("  %9s |", "hold ms");
    for (uint32_t db : {5u, 10u, 20u}) printf("  rose db=%2ums", db);
    printf(" | release 3ms\n");

    for (uint64_t hold : {2ULL, 4ULL, 6ULL, 8ULL, 9ULL, 10ULL, 11ULL, 12ULL,
                          15ULL, 20ULL, 25ULL, 40ULL}) {
        Trace t = makeRoll("", c.hits, 300000, bounce_us, hold * 1000, c.bounces);
        printf("  %9llu |", (unsigned long long)hold);
        for (uint32_t db : {5u, 10u, 20u})
            printf("  %11d", run(t, RawFireRose{{db}}, c.loop_us, 300000).missed);
        printf(" | %11d\n", run(t, RawFireRelease{c.release_ms}, c.loop_us, 300000).missed);
    }
    printf("\n  rose() needs hold >= PAD_DEBOUNCE_MS or the pad dies after hit 1.\n");
    printf("  release-window rearm watches the RAW pin, so it has no such floor.\n");
}

// --- Sweep B: bounce segment length --------------------------------------

static void sweepBounce(Config c) {
    printf("\n########## SWEEP B - contact-open segment length ##########\n");
    printf("single press, hold 150ms, loop %lluus, debounce %ums\n",
           (unsigned long long)c.loop_us, c.debounce_ms);
    printf("cell = missed/spurious.  Segment = bounce_total / bounces - it is the\n");
    printf("segment, not the total, that lets Bounce2 latch a stable state mid-bounce.\n");

    for (int bounces : {2, 6, 12}) {
        printf("\n  bounces = %d\n", bounces);
        printf("  %9s %10s | %11s | %11s | %11s | %11s\n", "segment", "total",
               "raw-fire", "rose fix", "confirm", "release");
        printf("  %9s %10s | %11s | %11s | %11s | %11s\n", "ms", "ms",
               "miss/spur", "miss/spur", "miss/spur", "miss/spur");

        std::vector<Sample> sRose, sRaw;
        for (double seg_ms : {0.1, 0.25, 0.5, 1.0, 2.0, 4.0, 6.0, 8.0, 9.0, 10.0,
                              11.0, 12.0, 15.0, 20.0, 30.0}) {
            uint64_t seg_us    = (uint64_t)(seg_ms * 1000);
            uint64_t bounce_us = seg_us * bounces;
            Config cc = c; cc.bounces = bounces; cc.segment_us = seg_us;
            Trace t{"", {}, {}, 0};
            addPress(t, 10000, bounce_us, 150000, bounces);
            t.duration_us = 10000 + 150000 + bounce_us * 2 + 200000;
            Row r = runAll(t, cc, 60000);
            printf("  %9.2f %10.2f | %5d/%-5d | %5d/%-5d | %5d/%-5d | %5d/%-5d%s\n",
                   seg_ms, seg_ms * bounces,
                   r.rawFire.missed, r.rawFire.spurious,
                   r.rose.missed,    r.rose.spurious,
                   r.confirm.missed, r.confirm.spurious,
                   r.release.missed, r.release.spurious,
                   seg_ms <= 1.0 ? "   <- physically plausible" : "");
            sRaw.push_back ({seg_ms, r.rawFire.clean(t.hits.size())});
            sRose.push_back({seg_ms, r.rose.clean(t.hits.size())});
        }
        printf("\n");
        printThreshold("rose fix", "ms", sRose, false, (double)c.debounce_ms, "debounce");
        printThreshold("raw-fire (as-is)", "ms", sRaw, false, (double)c.debounce_ms, "debounce");
    }
}

// --- Sweep C: glitch width -----------------------------------------------

static void sweepGlitch(Config c) {
    printf("\n########## SWEEP C - EMI glitch width ##########\n");
    printf("no real press anywhere in the trace: every fire is a phantom hit.\n");
    printf("cell = fires.  '(al)' marks width < loop period, where a zero is\n");
    printf("aliasing luck, not immunity.\n");

    for (uint64_t lp : {50ULL, 500ULL, 3000ULL}) {
        printf("\n  loop period = %lluus\n", (unsigned long long)lp);
        printf("  %10s %8s %8s %9s %9s %9s %9s\n", "width us",
               "stable25", "stable5", "raw-fire", "rose fix", "confirm", "release");
        for (uint64_t w : {10ULL, 20ULL, 50ULL, 100ULL, 200ULL, 500ULL, 1000ULL,
                           2000ULL, 5000ULL, 10000ULL}) {
            Trace t = makeGlitch("", w);
            char mark[8]; snprintf(mark, sizeof mark, "%s", w < lp ? "(al)" : "");
            printf("  %10llu %8d %8d %9d %9d %9d %9d %s\n",
                   (unsigned long long)w,
                   run(t, WaitStable{25}, lp).fires,
                   run(t, WaitStable{5},  lp).fires,
                   run(t, RawFire{{c.debounce_ms}}, lp).fires,
                   run(t, RawFireRose{{c.debounce_ms}}, lp).fires,
                   run(t, RawFireConfirm{{c.debounce_ms}, c.confirm_us}, lp).fires,
                   run(t, RawFireRelease{c.release_ms}, lp).fires, mark);
        }
    }

    printf("\n  --- what glitch immunity costs, on a clean press ---\n");
    printf("  %22s %14s\n", "scheme", "latency ms");
    Trace t{"", {}, {}, 400000};
    addPress(t, 10000, 2000, 150000, 6);
    Result a = run(t, RawFireRose{{c.debounce_ms}}, c.loop_us);
    Result b = run(t, RawFireConfirm{{c.debounce_ms}, 200},   c.loop_us);
    Result d = run(t, RawFireConfirm{{c.debounce_ms}, 1000},  c.loop_us);
    Result e = run(t, RawFireConfirm{{c.debounce_ms}, 5000},  c.loop_us);
    Result f = run(t, WaitStable{25}, c.loop_us);
    printf("  %22s %14.3f\n", "rose fix (no confirm)", a.meanLat_ms());
    printf("  %22s %14.3f\n", "confirm 200us",         b.meanLat_ms());
    printf("  %22s %14.3f\n", "confirm 1ms",           d.meanLat_ms());
    printf("  %22s %14.3f\n", "confirm 5ms",           e.meanLat_ms());
    printf("  %22s %14.3f\n", "wait-stable 25ms",      f.meanLat_ms());
}

// --- main ---------------------------------------------------------------

static bool isNumeric(const char* s) {
    if (!*s) return false;
    for (const char* p = s; *p; p++) if (*p < '0' || *p > '9') return false;
    return true;
}

int main(int argc, char** argv) {
    Config c;
    std::string sweep;

    // Backward compatible: no args, or a bare integer, means the legacy report.
    if (argc == 2 && isNumeric(argv[1])) {
        c.loop_us = strtoull(argv[1], nullptr, 10);
        legacyReport(c);
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&](const char* k) -> const char* {
            size_t n = strlen(k);
            return (a.compare(0, n, k) == 0) ? a.c_str() + n : nullptr;
        };
        if (const char* v = val("--sweep="))       sweep       = v;
        else if (const char* v = val("--loop-us=")) c.loop_us   = strtoull(v, nullptr, 10);
        else if (const char* v = val("--debounce-ms=")) c.debounce_ms = (uint32_t)atoi(v);
        else if (const char* v = val("--hold-ms="))     c.hold_ms     = strtoull(v, nullptr, 10);
        else if (const char* v = val("--segment-us="))  c.segment_us  = strtoull(v, nullptr, 10);
        else if (const char* v = val("--bounces="))     c.bounces     = atoi(v);
        else if (const char* v = val("--confirm-us="))  c.confirm_us  = strtoull(v, nullptr, 10);
        else if (const char* v = val("--release-ms="))  c.release_ms  = (uint32_t)atoi(v);
        else if (const char* v = val("--hits="))        c.hits        = atoi(v);
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }

    if (sweep.empty())                        { legacyReport(c); return 0; }
    if (sweep == "loop"      || sweep == "all") sweepLoop(c);
    if (sweep == "retrigger" || sweep == "all") { sweepRetriggerDetail(c); sweepRetriggerMatrix(c); }
    if (sweep == "bounce"    || sweep == "all") sweepBounce(c);
    if (sweep == "glitch"    || sweep == "all") sweepGlitch(c);
    return 0;
}
