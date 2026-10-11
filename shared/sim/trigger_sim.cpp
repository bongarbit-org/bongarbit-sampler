/*
 * trigger_sim.cpp - Host-side simulation of the sampler's trigger schemes.
 * Builds and runs on a normal PC, no board and no PlatformIO required:
 *
 *     g++ -O2 -std=c++17 -I shared/src -o /tmp/trigger_sim shared/sim/trigger_sim.cpp
 *     /tmp/trigger_sim                  # 6-trace report (regression baseline)
 *     /tmp/trigger_sim 3000             # same, with a 3000us loop() period
 *     /tmp/trigger_sim --sweep=all      # parameter sweeps
 *
 * The state machines are transcribed from sampler/src/main.cpp, not reinvented:
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

#include "trigger_model.h"

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
        printRow("raw+release 3ms",    tr, run(tr, trig(0, c.release_ms), c.loop_us));
        printRow("release+confirm",    tr, run(tr, trig(c.confirm_us, c.release_ms), c.loop_us));
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
                if (run(t, trig(0, rel), c.loop_us, iv * 1000).clean(t.hits.size())) {
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
        printf(" | %11d\n", run(t, trig(0, c.release_ms), c.loop_us, 300000).missed);
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
                   run(t, trig(0, c.release_ms), lp).fires, mark);
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

// --- Sweep E: does the hardware RC actually deliver? --------------------

static void sweepRC(Config c) {
    RCParams p;
    RCDerived d = rcDerive(p);

    printf("\n########## SWEEP E - hardware RC debounce ##########\n");
    printf("R1=%.0f (|| %.0f internal = %.0f), R2=%.0f, C=%.0fnF\n",
           p.r_pullup, p.r_internal, d.rup, p.r_series, p.c_farad * 1e9);
    printf("  tau strike (discharge)  = %6.1f us   -> target %.0f mV\n",
           d.tau_closed_us, d.v_closed * 1000);
    printf("  tau release (charge)    = %6.1f us   -> target %.0f mV\n",
           d.tau_open_us, p.vdd * 1000);
    printf("  Schmitt: VIL=%.2fV  VIH=%.2fV\n", d.vil, d.vih);

    // 1. How many transitions does the pin actually see?
    printf("\n--- edges the GPIO sees, per press (bounce 2ms / 6 segments) ---\n");
    printf("  %-14s %14s %14s\n", "trace", "raw edges", "through RC");
    for (double seg_ms : {0.1, 0.333, 1.0, 3.0}) {
        Trace t{"", {}, {}, 400000};
        addPress(t, 10000, (uint64_t)(seg_ms * 6000), 150000, 6);
        Trace f = applyRC(t, p);
        char lbl[32]; snprintf(lbl, sizeof lbl, "seg %.2fms", seg_ms);
        printf("  %-14s %14d %14d%s\n", lbl, (int)t.edges.size(),
               (int)f.edges.size(), f.edges.size() == 2 ? "   <- clean" : "");
    }

    // 2. What does the filter cost on the strike?
    printf("\n--- trigger latency, raw pin vs through RC (ms) ---\n");
    printf("  %-22s %12s %12s %10s\n", "scheme", "raw", "with RC", "delta");
    {
        Trace t{"", {}, {}, 400000};
        addPress(t, 10000, 2000, 150000, 6);
        Trace f = applyRC(t, p);
        struct { const char* n; double raw, rc; } rows[] = {
            {"release+confirm 200us",
             run(t, trig(c.confirm_us, c.release_ms), c.loop_us).meanLat_ms(),
             run(f, trig(c.confirm_us, c.release_ms), c.loop_us).meanLat_ms()},
            {"release only (no confirm)",
             run(t, trig(0, c.release_ms), c.loop_us).meanLat_ms(),
             run(f, trig(0, c.release_ms), c.loop_us).meanLat_ms()},
        };
        for (auto& r : rows)
            printf("  %-22s %12.3f %12.3f %10.3f\n", r.n, r.raw, r.rc, r.rc - r.raw);
    }

    // 3. The verification procedure written into sampler/docs/circuit-rc.md:
    //    with the RC in place, deliberately weakened firmware settings should
    //    still behave. If this table does not hold, that procedure is wrong.
    printf("\n--- sampler/docs/circuit-rc.md test: weaken the firmware, does RC cover? ---\n");
    printf("  fires/expected, single press, bounce 2ms/6seg\n");
    printf("  %-26s %12s %12s\n", "firmware setting", "raw pin", "with RC");
    {
        Trace t{"", {}, {}, 400000};
        addPress(t, 10000, 2000, 150000, 6);
        Trace f = applyRC(t, p);
        struct { const char* n; uint32_t rel; uint32_t conf; } cfgs[] = {
            {"confirm 200us, release 3ms", 3, 200},
            {"confirm 0,     release 3ms", 3, 0},
            {"confirm 0,     release 1ms", 1, 0},
            {"confirm 0,     release 0ms", 0, 0},
        };
        for (auto& cf : cfgs) {
            Result a = run(t, trig(cf.conf, cf.rel), c.loop_us);
            Result b = run(f, trig(cf.conf, cf.rel), c.loop_us);
            printf("  %-26s %8d/1    %8d/1  %s\n", cf.n, a.fires, b.fires,
                   b.fires == 1 ? "ok" : "FAIL");
        }
    }

    // 4. Glitch absorption. Modelled as a momentary contact closure, which is
    //    pessimistic: real coupled EMI arrives through a much higher impedance
    //    than a closed switch, so the cap shunts it harder than this shows.
    printf("\n--- EMI glitch absorbed? (fires; 0 = rejected) ---\n");
    printf("  %10s %14s %14s\n", "width us", "raw pin", "with RC");
    for (uint64_t w : {10ULL, 50ULL, 100ULL, 200ULL, 500ULL, 1000ULL, 2000ULL,
                       5000ULL, 10000ULL}) {
        Trace t = makeGlitch("", w);
        Trace f = applyRC(t, p);
        int a = run(t, trig(0, c.release_ms), c.loop_us).fires;              // no confirm
        int b = run(f, trig(0, c.release_ms), c.loop_us).fires;
        printf("  %10llu %14d %14d %s\n", (unsigned long long)w, a, b,
               b == 0 ? "rejected" : "PHANTOM");
    }

    // 5. I claimed the RC would rescue rose() if PAD_DEBOUNCE_MS came down.
    //    Check it, because it was asserted from theory.
    printf("\n--- does RC rescue the rose() scheme? (missed of %d, 300ms apart) ---\n",
           c.hits);
    printf("  %9s | %11s %11s | %11s %11s\n", "hold ms",
           "rose db=10 raw", "  +RC", "rose db=1 raw", "  +RC");
    for (uint64_t hold : {2ULL, 5ULL, 8ULL, 12ULL, 20ULL, 40ULL}) {
        Trace t = makeRoll("", c.hits, 300000, 2000, hold * 1000, 6);
        Trace f = applyRC(t, p);
        printf("  %9llu | %11d %11d | %11d %11d\n", (unsigned long long)hold,
               run(t, RawFireRose{{10}}, c.loop_us, 300000).missed,
               run(f, RawFireRose{{10}}, c.loop_us, 300000).missed,
               run(t, RawFireRose{{1}},  c.loop_us, 300000).missed,
               run(f, RawFireRose{{1}},  c.loop_us, 300000).missed);
    }
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
    if (sweep == "rc"        || sweep == "all") sweepRC(c);
    return 0;
}
