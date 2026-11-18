// fps-bench: how long the plugin's per-frame and per-render work takes.
//
//   fps-bench                 full table (1080p / 1440p / 4K, every format and method)
//   fps-bench --quick         the cases the budget check needs (a few seconds)
//   fps-bench --check         --quick, and exit 1 when a case exceeds its budget
//   fps-bench --json FILE     also write the results as JSON (run-perf.ps1 compares
//                             them with tests/perf-baseline.json)
//
// Each case is timed as the fastest of several rounds after a warm-up, in
// microseconds per call. No OBS: the same OBS-free code the plugin runs
// (fps-core, plot-layout, source-rate) on synthetic frames, half of them new
// and half repeats so both branches are in the measurement.

#include "fps-core.h"
#include "plot-layout.h"
#include "source-rate.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace {

struct Result {
    std::string name;
    double us;      // microseconds per call (fastest round)
    double budget;  // microseconds, 0 = none
    double ref_us;  // the reference loop timed right after it (CPU speed at that moment)
};

double now_s()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// Time per call: rounds of enough calls to take ~min_round_s each
double time_per_call(const std::function<void()> &fn, double min_round_s, int rounds)
{
    for (int i = 0; i < 3; i++) // warm-up
        fn();
    int calls = 1;
    for (;;) { // calibrate the calls per round
        double t0 = now_s();
        for (int i = 0; i < calls; i++)
            fn();
        double dt = now_s() - t0;
        if (dt >= min_round_s || calls >= (1 << 24))
            break;
        calls *= 2;
    }
    // The fastest round: other processes (OBS analysing frames itself, a
    // browser) can only slow a round down, never speed it up, so the minimum
    // is the stable estimate of the code's own cost; a median moved by 2x
    // between two runs with OBS open.
    double best = 1e300;
    for (int r = 0; r < rounds; r++) {
        double t0 = now_s();
        for (int i = 0; i < calls; i++)
            fn();
        double us = (now_s() - t0) / calls * 1e6;
        if (us < best)
            best = us;
    }
    return best;
}

// A fixed amount of pure arithmetic (no memory traffic): how fast the core
// runs right now, recorded next to each case ("normalised" in the JSON, for
// information: on the development machine it did not turn out steadier than
// the plain microseconds, which run-perf.ps1 compares).
volatile uint64_t g_sink;
double reference_us()
{
    return time_per_call(
        [] {
            uint64_t x = 88172645463325252ULL;
            for (int i = 0; i < 20000; i++) {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
            }
            g_sink = x;
        },
        0.02, 5);
}

struct Frame {
    int fmt;
    uint32_t w, h, linesize;
    std::vector<uint8_t> buf;
    fps_frame_view view() const
    {
        fps_frame_view v{};
        v.format = fmt;
        v.width = w;
        v.height = h;
        v.data = buf.data();
        v.linesize = linesize;
        v.src_ts_ns = 0;
        return v;
    }
};

uint32_t bytes_per_px(int fmt)
{
    switch (fmt) {
    case FPS_PIXFMT_BGRA:
    case FPS_PIXFMT_RGBA: return 4;
    case FPS_PIXFMT_YUY2:
    case FPS_PIXFMT_UYVY: return 2;
    default: return 1;
    }
}

Frame make_frame(int fmt, uint32_t w, uint32_t h, uint64_t seed)
{
    Frame f;
    f.fmt = fmt;
    f.w = w;
    f.h = h;
    f.linesize = w * bytes_per_px(fmt);
    f.buf.resize((size_t)f.linesize * h);
    uint64_t s = seed * 0x9E3779B97F4A7C15ULL + 1;
    for (auto &b : f.buf) {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        b = (uint8_t)(s >> 56);
    }
    return f;
}

const char *fmt_name(int fmt)
{
    switch (fmt) {
    case FPS_PIXFMT_NV12: return "NV12";
    case FPS_PIXFMT_YUY2: return "YUY2";
    case FPS_PIXFMT_BGRA: return "BGRA";
    default: return "?";
    }
}

// fps_core_feed on alternating new / repeated frames
Result bench_feed(int fmt, uint32_t w, uint32_t h, int method, bool deltas, bool tearing, double round_s, int rounds)
{
    fps_core_params p = fps_core_params_defaults();
    p.analyze_method = method;
    p.measure_deltas = deltas;
    p.enable_tearing_detection = tearing;
    fps_core *c = fps_core_create(&p);
    Frame a = make_frame(fmt, w, h, 1), b = make_frame(fmt, w, h, 2);
    fps_frame_view va = a.view(), vb = b.view();
    uint64_t t = 1000000000000ULL;
    uint64_t k = 0;
    double us = time_per_call(
        [&] {
            fps_frame_view v = ((k >> 1) & 1) ? vb : va; // A A B B ...: new, repeat
            v.src_ts_ns = t;                             // device timestamps: pacing and dropped frames run
            fps_core_feed(c, &v, t);
            t += 16683333;
            k++;
        },
        round_s, rounds);
    fps_core_destroy(c);
    char name[96];
    snprintf(name, sizeof(name), "feed %ux%u %s %s%s%s", w, h, fmt_name(fmt),
             method == FPS_ANALYZE_DIFF ? "full" : "lastline", deltas ? " +deltas" : "", tearing ? "" : " -tear");
    return {name, us, 0.0, 0.0};
}

// Ring of samples like 136 s of a 60 FPS game
void fill_ring(plot_ring *r, double ft_ms, int n, double end_ms)
{
    plot_ring_clear(r);
    double t = end_ms - ft_ms * (n - 1);
    for (int i = 0; i < n; i++) {
        plot_sample s{};
        s.t_ms = t;
        s.ft_ms = ft_ms;
        s.ema_ms = ft_ms;
        s.fps_pf = 1000.0 / ft_ms;
        s.pacing_ms = ft_ms;
        plot_ring_push(r, &s);
        t += ft_ms;
    }
}

plot_ring g_ring;
plot_span g_spans[FPS_PLOT_CAP];
float g_xs[4096];
double g_vs[4096];

// Budgets (microseconds per call) for --check: generous, they only catch a
// gross regression on any machine - ~4x what a desktop CPU measured on
// 2026-11-18, at least a few us for the cheapest cases (timer noise).
struct Budget {
    const char *name;
    double us;
};
const Budget k_budgets[] = {
    {"feed 3840x2160 NV12 lastline +deltas", 40.0},  // measured 9
    {"feed 3840x2160 NV12 full +deltas", 43000.0},   // measured 10650
    {"feed 3840x2160 YUY2 full +deltas", 48000.0},   // measured 12030
    {"feed 3840x2160 BGRA full +deltas", 63000.0},   // measured 15720
    {"beat obs_tick + get_beat", 5.0},               // measured 0.06
    {"core_tick (960 graph samples)", 5.0},          // measured 0.01
    {"plot_window 60 s 1800 px", 32.0},              // measured 7.8
    {"plot_fps_curve 60 s 1800 px @60fps", 500.0},   // measured 124
    {"srate push + get", 20.0},                      // measured 5.0
};

// On a CI runner (GitHub sets CI=true) shared, slower machines: 3x more room,
// the check is only after gross regressions there
double budget_scale()
{
    const char *ci = getenv("CI");
    return (ci && *ci && strcmp(ci, "0") != 0 && strcmp(ci, "false") != 0) ? 3.0 : 1.0;
}

double budget_of(const std::string &name)
{
    for (const Budget &b : k_budgets)
        if (name == b.name)
            return b.us * budget_scale();
    return 0.0;
}

} // namespace

void add(std::vector<Result> &res, Result r)
{
    r.ref_us = reference_us();
    res.push_back(r);
}

int main(int argc, char **argv)
{
    bool quick = false, check = false;
    const char *json = nullptr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--quick")
            quick = true;
        else if (a == "--check")
            quick = check = true;
        else if (a == "--json" && i + 1 < argc)
            json = argv[++i];
        else {
            fprintf(stderr, "usage: fps-bench [--quick | --check] [--json FILE]\n");
            return 2;
        }
    }
    const double round_s = quick ? 0.05 : 0.15;
    const int rounds = quick ? 5 : 7;
    std::vector<Result> res;

    // Per frame: fps_core_feed
    struct Res {
        uint32_t w, h;
    };
    std::vector<Res> sizes = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
    std::vector<int> fmts = {FPS_PIXFMT_NV12, FPS_PIXFMT_YUY2, FPS_PIXFMT_BGRA};
    if (quick) {
        add(res, bench_feed(FPS_PIXFMT_NV12, 3840, 2160, FPS_ANALYZE_LAST_LINE, true, true, round_s, rounds));
        for (int fmt : fmts)
            add(res, bench_feed(fmt, 3840, 2160, FPS_ANALYZE_DIFF, true, true, round_s, rounds));
    } else {
        for (const Res &s : sizes)
            for (int fmt : fmts)
                for (int method : {FPS_ANALYZE_LAST_LINE, FPS_ANALYZE_DIFF})
                    for (bool deltas : {false, true})
                        add(res, bench_feed(fmt, s.w, s.h, method, deltas, true, round_s, rounds));
        add(res, bench_feed(FPS_PIXFMT_NV12, 3840, 2160, FPS_ANALYZE_DIFF, true, false, round_s, rounds));
    }

    // Per OBS tick: beat detector, publish
    {
        fps_core_params p = fps_core_params_defaults();
        fps_core *c = fps_core_create(&p);
        uint64_t t = 1000000000000ULL;
        int k = 0;
        double us = time_per_call(
            [&] {
                fps_core_obs_tick(c, t, (k++ % 1000 == 999) ? 0 : 1);
                fps_beat b;
                fps_core_get_beat(c, t, 60.0, &b);
                t += 16666667;
            },
            round_s, rounds);
        add(res, {"beat obs_tick + get_beat", us, 0.0, 0.0});
        fps_core_destroy(c);
    }
    {
        fps_core_params p = fps_core_params_defaults();
        fps_core *c = fps_core_create(&p);
        Frame a = make_frame(FPS_PIXFMT_NV12, 64, 4, 1), b = make_frame(FPS_PIXFMT_NV12, 64, 4, 2);
        uint64_t t = 1000000000000ULL;
        for (int i = 0; i < 2000; i++) { // a full history
            fps_frame_view v = (i & 1) ? b.view() : a.view();
            fps_core_feed(c, &v, t);
            t += 16666667;
        }
        static fps_core_output o;
        double us = time_per_call(
            [&] {
                t += 1000000000ULL; // past the update gate every call
                fps_core_tick(c, t, &o);
            },
            round_s, rounds);
        add(res, {"core_tick (960 graph samples)", us, 0.0, 0.0});
        fps_core_destroy(c);
    }

    // Per overlay render: the time-axis graphs
    const double end_ms = 1e9;
    fill_ring(&g_ring, 1000.0 / 59.94, FPS_PLOT_CAP, end_ms);
    for (double win : {5.0, 20.0, 60.0}) {
        if (quick && win != 60.0)
            continue;
        double us = time_per_call([&] { plot_window(&g_ring, end_ms, win, 1800.0f, g_spans, FPS_PLOT_CAP); },
                                  round_s, rounds);
        char name[64];
        snprintf(name, sizeof(name), "plot_window %.0f s 1800 px", win);
        add(res, {name, us, 0.0, 0.0});
    }
    for (double fps : {30.0, 60.0}) {
        fill_ring(&g_ring, 1000.0 / (fps * 0.999), FPS_PLOT_CAP, end_ms);
        for (double win : {10.0, 20.0, 60.0}) {
            if (quick && (win != 60.0 || fps != 60.0))
                continue;
            double us = time_per_call(
                [&] { plot_fps_curve(&g_ring, end_ms, win, 1800.0f, 2.0f, g_xs, g_vs, 4096); }, round_s, rounds);
            char name[64];
            snprintf(name, sizeof(name), "plot_fps_curve %.0f s 1800 px @%.0ffps", win, fps);
            add(res, {name, us, 0.0, 0.0});
        }
    }

    // Per frame: the source refresh rate estimator
    {
        srate *s = srate_create();
        uint64_t ts = 1000000000ULL, now = 5000000000ULL;
        double us = time_per_call(
            [&] {
                srate_push(s, ts, now);
                srate_result r;
                srate_get(s, 59.94, &r);
                ts += 16683333;
                now += 16683333;
            },
            round_s, rounds);
        add(res, {"srate push + get", us, 0.0, 0.0});
        srate_destroy(s);
    }

    // Report
    bool over = false;
    printf("%-46s %12s %12s\n", "case", "us/call", "budget");
    for (Result &r : res) {
        r.budget = budget_of(r.name);
        bool bad = r.budget > 0 && r.us > r.budget;
        over = over || bad;
        if (r.budget > 0)
            printf("%-46s %12.2f %12.0f%s\n", r.name.c_str(), r.us, r.budget, bad ? "  OVER BUDGET" : "");
        else
            printf("%-46s %12.2f %12s\n", r.name.c_str(), r.us, "-");
    }

    if (json) {
        FILE *f = fopen(json, "w");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", json);
            return 2;
        }
        const char *cpu = getenv("PROCESSOR_IDENTIFIER");
        char date[32];
        time_t tt = time(nullptr);
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M", localtime(&tt));
        fprintf(f, "{\n  \"cpu\": \"%s\",\n  \"date\": \"%s\",\n  \"mode\": \"%s\",\n  \"results\": {\n",
                cpu ? cpu : "unknown", date, quick ? "quick" : "full");
        for (size_t i = 0; i < res.size(); i++)
            fprintf(f, "    \"%s\": %.3f%s\n", res[i].name.c_str(), res[i].us, i + 1 < res.size() ? "," : "");
        // case time in reference loops (informational)
        fprintf(f, "  },\n  \"normalised\": {\n");
        for (size_t i = 0; i < res.size(); i++)
            fprintf(f, "    \"%s\": %.6f%s\n", res[i].name.c_str(), res[i].ref_us > 0 ? res[i].us / res[i].ref_us : 0.0,
                    i + 1 < res.size() ? "," : "");
        fprintf(f, "  }\n}\n");
        fclose(f);
    }

    if (check) {
        if (budget_scale() > 1.0)
            printf("(CI: budgets x%.0f)\n", budget_scale());
        printf(over ? "RESULT: OVER BUDGET\n" : "RESULT: ALL WITHIN BUDGET\n");
        return over ? 1 : 0;
    }
    return 0;
}
