// srate-selftest — synthetic regression suite for source-rate.cpp (no OBS deps).
// Frames get driver timestamps (nominal period, crystal offset, jitter); a
// simplified OBS clock then picks the newest frame at each tick, as
// ready_async_frame does, and only the picked frames reach the estimator.
#include "source-rate.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

struct sim_source {
    double hz;          // nominal source rate
    double ppm = 0.0;   // crystal offset of the source clock
    double jitter_ms = 1.0;
};

// Frame timestamps over [t_start, t_start + seconds)
static std::vector<uint64_t> make_frames(const sim_source &src, double seconds,
                                         double t_start_s, unsigned seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> jit(-src.jitter_ms, src.jitter_ms);
    double period_ns = 1e9 / (src.hz * (1.0 + src.ppm * 1e-6));
    std::vector<uint64_t> ts;
    double t_end = (t_start_s + seconds) * 1e9;
    for (double t = t_start_s * 1e9 + 1e6; t < t_end; t += period_ns)
        ts.push_back((uint64_t)(t + jit(rng) * 1e6));
    return ts;
}

// OBS clock: at each tick deliver the newest frame not newer than the tick
static void obs_pick(struct srate *s, const std::vector<uint64_t> &frames, double obs_hz) {
    if (frames.empty())
        return;
    double tick_ns = 1e9 / obs_hz;
    size_t next = 0;
    uint64_t delivered = 0;
    for (double tick = (double)frames.front(); tick <= (double)frames.back() + tick_ns; tick += tick_ns) {
        uint64_t pick = 0;
        while (next < frames.size() && (double)frames[next] <= tick)
            pick = frames[next++];
        if (pick && pick != delivered) {
            srate_push(s, pick);
            delivered = pick;
        }
    }
}

static int fails = 0;

static void check(bool ok, const char *what) {
    printf("  %s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        fails++;
}

static void print_result(const srate_result &r) {
    char src[32], rec[32];
    srate_format_hz(r.source_hz, src, sizeof(src));
    srate_format_hz(r.recommended_hz, rec, sizeof(rec));
    printf("  -> valid=%d status=%d source=%.4f (%s) nominal=%.4f ratio=%d beat=%.2fs rec=%s\n",
           r.valid, r.status, r.source_hz, src, r.nominal_hz, r.ratio, r.beat_period_s, rec);
}

static srate_result run(const sim_source &src, double obs_hz, double seconds, unsigned seed) {
    struct srate *s = srate_create();
    obs_pick(s, make_frames(src, seconds, 100.0, seed), obs_hz);
    srate_result r;
    srate_get(s, obs_hz, &r);
    srate_destroy(s);
    print_result(r);
    return r;
}

static bool near(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

int main() {
    const double NTSC60 = 60000.0 / 1001.0;
    const double NTSC30 = 30000.0 / 1001.0;
    const double NTSC120 = 120000.0 / 1001.0;

    printf("[1] PS5 59.94 (+73 ppm) into OBS 60\n");
    {
        srate_result r = run({NTSC60, 73.0}, 60.0, 20.0, 1);
        check(r.valid && r.status == SRATE_STATUS_MISMATCH, "mismatch");
        check(near(r.source_hz, NTSC60 * (1 + 73e-6), 0.01), "rate ~59.944");
        check(fabs(r.nominal_hz - NTSC60) < 1e-9, "nominal 59.94");
        check(near(r.beat_period_s, 18.0, 1.0), "beat ~18 s (as measured on the PS5)");
        check(fabs(r.recommended_hz - NTSC60) < 1e-9, "recommends 59.94");
    }

    printf("[2] PC 60 into OBS 59.94 (OBS skips frames)\n");
    {
        srate_result r = run({60.0}, NTSC60, 20.0, 2);
        check(r.valid && r.status == SRATE_STATUS_MISMATCH, "mismatch");
        check(near(r.source_hz, 60.0, 0.01), "rate 60");
        check(near(r.beat_period_s, 16.68, 0.7), "beat ~16.7 s");
        check(fabs(r.recommended_hz - 60.0) < 1e-9, "recommends 60");
    }

    printf("[3] 59.94 (+73 ppm) into OBS 59.94\n");
    {
        srate_result r = run({NTSC60, 73.0}, NTSC60, 20.0, 3);
        check(r.valid && r.status == SRATE_STATUS_MATCHED, "matched");
        check(r.beat_period_s == 0.0, "no beat reported");
    }

    printf("[4] 60 into OBS 60\n");
    {
        srate_result r = run({60.0, -40.0}, 60.0, 20.0, 4);
        check(r.valid && r.status == SRATE_STATUS_MATCHED, "matched");
    }

    printf("[5] 119.88 into OBS 60 (effective rate)\n");
    {
        srate_result r = run({NTSC120}, 60.0, 20.0, 5);
        check(r.valid && r.status == SRATE_STATUS_MISMATCH, "mismatch");
        check(near(r.source_hz, NTSC60, 0.01) || near(r.source_hz, NTSC120, 0.02),
              "rate 59.94 effective (or 119.88)");
    }

    printf("[6a] 29.97 into OBS 30\n");
    {
        srate_result r = run({NTSC30}, 30.0, 20.0, 6);
        check(r.valid && r.status == SRATE_STATUS_MISMATCH && r.ratio == 1, "mismatch, 1:1");
        check(near(r.beat_period_s, 33.4, 1.5), "beat ~33 s");
    }
    printf("[6b] 29.97 into OBS 60 (each frame shown twice)\n");
    {
        srate_result r = run({NTSC30}, 60.0, 20.0, 7);
        check(r.valid && r.status == SRATE_STATUS_MISMATCH && r.ratio == 2, "mismatch, k=2");
        check(fabs(r.recommended_hz - NTSC60) < 1e-9, "recommends 59.94");
    }
    printf("[6c] 30 into OBS 60\n");
    {
        srate_result r = run({30.0}, 60.0, 20.0, 8);
        check(r.valid && r.status == SRATE_STATUS_MATCHED && r.ratio == 2, "matched, k=2");
    }

    printf("[7] signal lost for 5 s, then back\n");
    {
        // One continuous signal with 5 s missing (cable pulled)
        std::vector<uint64_t> all = make_frames({NTSC60}, 26.0, 100.0, 9), before, after;
        for (uint64_t ts : all) {
            if (ts < 110000000000ULL)
                before.push_back(ts);
            else if (ts >= 115000000000ULL)
                after.push_back(ts);
        }
        std::vector<uint64_t> after_short(after.begin(), after.begin() + 180); // ~3 s
        std::vector<uint64_t> after_rest(after.begin() + 180, after.end());
        struct srate *s = srate_create();
        obs_pick(s, before, 60.0);
        obs_pick(s, after_short, 60.0);
        srate_result r;
        srate_get(s, 60.0, &r);
        print_result(r);
        check(!r.valid && r.status == SRATE_STATUS_MEASURING, "measuring after the gap");
        obs_pick(s, after_rest, 60.0);
        srate_get(s, 60.0, &r);
        print_result(r);
        check(r.valid && near(r.source_hz, NTSC60, 0.01), "valid again");
        srate_destroy(s);
    }

    printf("[8] repeated timestamps (deinterlacing)\n");
    {
        struct srate *s = srate_create();
        for (uint64_t ts : make_frames({NTSC60}, 20.0, 100.0, 12)) {
            srate_push(s, ts);
            srate_push(s, ts);
        }
        srate_result r;
        srate_get(s, 60.0, &r);
        print_result(r);
        check(r.valid && near(r.source_hz, NTSC60, 0.01), "unaffected");
    }

    printf("[9] variable frame rate\n");
    {
        struct srate *s = srate_create();
        std::mt19937 rng(13);
        std::uniform_real_distribution<double> d(10e6, 40e6);
        double t = 100e9;
        for (int i = 0; i < 800; i++, t += d(rng))
            srate_push(s, (uint64_t)t);
        srate_result r;
        srate_get(s, 60.0, &r);
        print_result(r);
        check(!r.valid && r.status == SRATE_STATUS_UNSTABLE, "unstable");
        srate_destroy(s);
    }

    printf("[10] source switches 60 -> 120 Hz\n");
    {
        struct srate *s = srate_create();
        for (uint64_t ts : make_frames({60.0}, 10.0, 100.0, 14))
            srate_push(s, ts);
        for (uint64_t ts : make_frames({120.0}, 12.0, 110.0, 15))
            srate_push(s, ts);
        srate_result r;
        srate_get(s, 120.0, &r);
        srate_get(s, 120.0, &r); // first call may only trim the window
        print_result(r);
        check(near(r.source_hz, 120.0, 0.05), "follows the new rate");
        srate_destroy(s);
    }

    printf("[11] short window\n");
    {
        srate_result r = run({NTSC60}, 60.0, 3.0, 16);
        check(!r.valid && r.status == SRATE_STATUS_MEASURING, "measuring");
    }

    printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: ALL OK\n", fails);
    return fails ? 1 : 0;
}
