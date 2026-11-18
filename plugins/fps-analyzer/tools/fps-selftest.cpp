// fps-selftest — characterization tests for the FPS-analysis core.
//
// Synthetic frame/tick streams with a synthetic clock are driven through one
// or two implementations (the v0.5.0 reference copy and, when linked, the
// extracted fps-core). Every scenario writes a log in the fps-trace row
// layout (F rows per frame, T rows per publish); hand-written asserts check
// the values derivable from the algorithm, golden files under
// tests/golden/ pin the exact output, and when both implementations are
// present their logs must be identical.
//
//   fps-selftest [--impl ref|core|both] [--update-goldens] [--data-dir DIR] [--verbose] [--only NAME]
//
// Exit code = number of failures.

#include "fps-core.h"
#include "plot-layout.h"
#include "fps-ref-v050.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ---------------------------------------------------------------- Impl ----

struct Impl {
    virtual ~Impl() {}
    virtual const char *name() const = 0;
    virtual void reset(const fps_core_params &p) = 0;
    virtual void set_params(const fps_core_params &p) = 0;
    virtual int feed(const fps_frame_view &f, uint64_t now) = 0;
    virtual bool tick(uint64_t now, fps_core_output &out) = 0;
    virtual void last_frame(fps_core_frame_debug &d) const = 0;
    virtual void feed_decision(bool unique, bool tearing, uint64_t now) = 0;
    virtual int last_fps() const = 0;
    virtual bool extract(const fps_frame_view &f, uint8_t *luma, uint32_t first, uint32_t lines) = 0;
    virtual int csv_line(char *buf, size_t n, long long t, int fps, double ft) = 0;
    virtual void csv_keep(const char *path, int n) = 0;
};

struct RefImpl : Impl {
    fpsref_core *c = nullptr;
    ~RefImpl() { fpsref_destroy(c); }
    const char *name() const override { return "ref"; }
    void reset(const fps_core_params &p) override { fpsref_destroy(c); c = fpsref_create(&p); }
    void set_params(const fps_core_params &p) override { fpsref_set_params(c, &p); }
    int feed(const fps_frame_view &f, uint64_t now) override { return fpsref_feed(c, &f, now); }
    bool tick(uint64_t now, fps_core_output &out) override { return fpsref_tick(c, now, &out); }
    void last_frame(fps_core_frame_debug &d) const override { fpsref_debug_last_frame(c, &d); }
    void feed_decision(bool u, bool t, uint64_t now) override { fpsref_debug_feed_decision(c, u, t, now); }
    int last_fps() const override { return fpsref_debug_last_published_fps(c); }
    bool extract(const fps_frame_view &f, uint8_t *l, uint32_t a, uint32_t b) override { return fpsref_extract_luma(&f, l, a, b); }
    int csv_line(char *buf, size_t n, long long t, int fps, double ft) override { return fpsref_format_csv_line(buf, n, t, fps, ft); }
    void csv_keep(const char *path, int n) override { fpsref_csv_keep_last_n_lines(path, n); }
};

#ifdef FPS_SELFTEST_HAVE_CORE
struct CoreImpl : Impl {
    fps_core *c = nullptr;
    ~CoreImpl() { fps_core_destroy(c); }
    const char *name() const override { return "core"; }
    void reset(const fps_core_params &p) override { fps_core_destroy(c); c = fps_core_create(&p); }
    void set_params(const fps_core_params &p) override { fps_core_set_params(c, &p); }
    int feed(const fps_frame_view &f, uint64_t now) override { return fps_core_feed(c, &f, now); }
    bool tick(uint64_t now, fps_core_output &out) override { return fps_core_tick(c, now, &out); }
    void last_frame(fps_core_frame_debug &d) const override { fps_core_debug_last_frame(c, &d); }
    void feed_decision(bool u, bool t, uint64_t now) override { fps_core_debug_feed_decision(c, u, t, now); }
    int last_fps() const override { return fps_core_debug_last_published_fps(c); }
    bool extract(const fps_frame_view &f, uint8_t *l, uint32_t a, uint32_t b) override { return fps_core_extract_luma(&f, l, a, b); }
    int csv_line(char *buf, size_t n, long long t, int fps, double ft) override { return fps_core_format_csv_line(buf, n, t, fps, ft); }
    void csv_keep(const char *path, int n) override { fps_core_csv_keep_last_n_lines(path, n); }
};
#endif

// -------------------------------------------------------------- frames ----

// Synthetic clock: OBS-like large values so the very first tick publishes
// (last_write_time starts at 0), integer nanoseconds.
static const uint64_t T0 = 1000000000000ULL;
static uint64_t at(uint64_t i, uint64_t hz) { return T0 + i * 1000000000ULL / hz; }
static double secs(uint64_t now) { return (double)(now - T0) / 1e9; }

static uint32_t bytes_per_px(int fmt)
{
    switch (fmt) {
    case FPS_PIXFMT_BGRA: case FPS_PIXFMT_RGBA: return 4;
    case FPS_PIXFMT_YUY2: case FPS_PIXFMT_UYVY: return 2;
    default: return 1;
    }
}

struct Frame {
    int fmt = FPS_PIXFMT_NV12;
    uint32_t w = 0, h = 0, linesize = 0;
    std::vector<uint8_t> buf;
    fps_frame_view view() const { return {fmt, w, h, buf.data(), linesize}; }
};

static Frame make_frame(int fmt, uint32_t w, uint32_t h, uint32_t pad = 0)
{
    Frame f;
    f.fmt = fmt;
    f.w = w;
    f.h = h;
    f.linesize = w * bytes_per_px(fmt) + pad;
    f.buf.assign((size_t)f.linesize * h, 0);
    return f;
}

static uint64_t lcg_next(uint64_t &s)
{
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    return s >> 56;
}

// Rewrites rows [y0, y1) with pseudo-random bytes derived from `seed`.
static void fill_rows(Frame &f, uint32_t y0, uint32_t y1, uint64_t seed)
{
    uint64_t s = seed * 0x9E3779B97F4A7C15ULL + 12345;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = 0; x < f.linesize; x++)
            f.buf[(size_t)y * f.linesize + x] = (uint8_t)lcg_next(s);
}
static void fill_all(Frame &f, uint64_t seed) { fill_rows(f, 0, f.h, seed); }

// ------------------------------------------------------------- runner ----

struct Pub {
    uint64_t now;
    int fps;
    double ft;
    int window;
    int graph_count;
    bool tearing;
};

// The four graph arrays are what the overlay draws, and they are far too big to
// print per tick. Hashing the entries actually in use puts them in the log, so
// the goldens pin them and the cross-implementation comparison covers them.
static uint32_t graph_hash(const fps_core_output &o)
{
    uint32_t h = 2166136261u;
    auto mix = [&h](const void *p, size_t n) {
        const uint8_t *b = (const uint8_t *)p;
        for (size_t i = 0; i < n; i++) {
            h ^= b[i];
            h *= 16777619u;
        }
    };
    int n = o.graph_count;
    if (n < 0) n = 0;
    if (n > FPS_CORE_HISTORY) n = FPS_CORE_HISTORY;
    mix(o.graph_frametimes, sizeof(double) * (size_t)n);
    mix(o.graph_frametimes_raw, sizeof(double) * (size_t)n);
    mix(o.graph_fps, sizeof(double) * (size_t)n);
    mix(o.graph_tearing, sizeof(bool) * (size_t)n);
    return h;
}

struct Runner {
    std::vector<std::unique_ptr<Impl>> impls;
    std::vector<std::string> logs;
    std::string scenario;
    fps_core_params params;
    fps_core_frame_debug dbg{};       // impl[0]'s last frame
    fps_core_output out{};            // impl[0]'s last publish
    std::vector<Pub> pubs;
    int fails = 0;
    bool verbose = false;
    bool update_goldens = false;
    fs::path data_dir;
    std::string only;

    bool active() const { return only.empty() || only == scenario; }

    void begin(const char *name, const fps_core_params &p)
    {
        scenario = name;
        params = p;
        pubs.clear();
        logs.assign(impls.size(), std::string());
        for (auto &i : impls)
            i->reset(p);
        if (verbose)
            printf("--- %s\n", name);
    }

    void set_params(const fps_core_params &p)
    {
        params = p;
        for (auto &i : impls)
            i->set_params(p);
        char line[128];
        snprintf(line, sizeof(line), "# params method=%d sens=%.17g tear=%d tsens=%.17g interval=%.17g\n",
                 p.analyze_method, p.sensitivity, p.enable_tearing_detection ? 1 : 0, p.tearing_sensitivity,
                 p.update_interval);
        for (auto &l : logs)
            l += line;
    }

    static void fmt_frame(std::string &log, uint64_t now, int status, const fps_core_frame_debug &d)
    {
        char line[256];
        if (status != FPS_FEED_OK) {
            snprintf(line, sizeof(line), "F,%llu,status=%d\n", (unsigned long long)now, status);
            log += line;
            return;
        }
        int n = snprintf(line, sizeof(line), "F,%llu,%zu,", (unsigned long long)now, d.luma_size);
        if (d.compared)
            n += snprintf(line + n, sizeof(line) - n, "%zu,%.17g,", d.diff, d.percent);
        else
            n += snprintf(line + n, sizeof(line) - n, ",,");
        n += snprintf(line + n, sizeof(line) - n, "%d,%d,", d.unique ? 1 : 0, d.tearing_flag ? 1 : 0);
        if (d.sample_written)
            n += snprintf(line + n, sizeof(line) - n, "%.17g,%.17g,%.17g,", d.ft_ms, d.ema, d.fps_pf);
        else
            n += snprintf(line + n, sizeof(line) - n, ",,,");
        snprintf(line + n, sizeof(line) - n, "%d,%d\n", d.pos, d.count);
        log += line;
    }

    static void fmt_tick(std::string &log, uint64_t now, const fps_core_output &o)
    {
        char line[256];
        snprintf(line, sizeof(line), "T,%llu,%d,%.17g,%d,%.17g,%d,%d,%08x\n", (unsigned long long)now, o.window,
                 o.frametime_ms, o.fps, o.frametime_ms, o.tearing_detected ? 1 : 0, o.graph_count, graph_hash(o));
        log += line;
    }

    int feed(const Frame &f, uint64_t now)
    {
        int st0 = 0;
        for (size_t k = 0; k < impls.size(); k++) {
            int st = impls[k]->feed(f.view(), now);
            fps_core_frame_debug d{};
            impls[k]->last_frame(d);
            fmt_frame(logs[k], now, st, d);
            if (k == 0) {
                st0 = st;
                dbg = d;
            }
        }
        return st0;
    }

    void feed_decision(bool unique, bool tearing, uint64_t now)
    {
        for (size_t k = 0; k < impls.size(); k++) {
            impls[k]->feed_decision(unique, tearing, now);
            fps_core_frame_debug d{};
            impls[k]->last_frame(d);
            fmt_frame(logs[k], now, FPS_FEED_OK, d);
            if (k == 0)
                dbg = d;
        }
    }

    bool tick(uint64_t now)
    {
        bool p0 = false;
        for (size_t k = 0; k < impls.size(); k++) {
            fps_core_output o{};
            bool p = impls[k]->tick(now, o);
            if (p)
                fmt_tick(logs[k], now, o);
            if (k == 0) {
                p0 = p;
                if (p) {
                    out = o;
                    pubs.push_back({now, o.fps, o.frametime_ms, o.window, o.graph_count, o.tearing_detected});
                }
            }
        }
        return p0;
    }

    void check(bool cond, const char *fmt, ...)
    {
        if (cond)
            return;
        fails++;
        char msg[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        printf("  FAIL [%s] %s\n", scenario.c_str(), msg);
    }

    // Cross-implementation equality + golden comparison/update.
    void end(bool golden = true)
    {
        for (size_t k = 1; k < impls.size(); k++) {
            if (logs[k] != logs[0]) {
                fails++;
                printf("  FAIL [%s] %s and %s logs differ at line %d\n", scenario.c_str(), impls[0]->name(),
                       impls[k]->name(), first_diff_line(logs[0], logs[k]));
                print_diff_context(logs[0], logs[k]);
            }
        }
        if (!golden)
            return;
        fs::path path = data_dir / "golden" / (scenario + ".csv");
        if (update_goldens) {
            fs::create_directories(path.parent_path());
            std::ofstream o(path, std::ios::binary);
            o << logs[0];
            printf("  golden written: %s (%zu lines)\n", path.string().c_str(), count_lines(logs[0]));
            return;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            fails++;
            printf("  FAIL [%s] golden missing: %s (run with --update-goldens)\n", scenario.c_str(),
                   path.string().c_str());
            return;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string g = ss.str();
        if (g != logs[0]) {
            fails++;
            printf("  FAIL [%s] output differs from golden %s at line %d\n", scenario.c_str(),
                   path.string().c_str(), first_diff_line(g, logs[0]));
            print_diff_context(g, logs[0]);
        }
    }

    static size_t count_lines(const std::string &s)
    {
        size_t n = 0;
        for (char c : s)
            if (c == '\n')
                n++;
        return n;
    }

    static int first_diff_line(const std::string &a, const std::string &b)
    {
        int line = 1;
        size_t n = a.size() < b.size() ? a.size() : b.size();
        for (size_t i = 0; i < n; i++) {
            if (a[i] != b[i])
                return line;
            if (a[i] == '\n')
                line++;
        }
        return a.size() == b.size() ? 0 : line;
    }

    static void print_diff_context(const std::string &a, const std::string &b)
    {
        int line = first_diff_line(a, b);
        if (!line)
            return;
        auto get_line = [](const std::string &s, int ln) {
            int cur = 1;
            size_t start = 0;
            for (size_t i = 0; i < s.size(); i++) {
                if (cur == ln) {
                    size_t e = s.find('\n', i);
                    return s.substr(i, e == std::string::npos ? std::string::npos : e - i);
                }
                if (s[i] == '\n') {
                    cur++;
                    start = i + 1;
                }
            }
            (void)start;
            return std::string("<eof>");
        };
        printf("    expected: %s\n    got:      %s\n", get_line(a, line).c_str(), get_line(b, line).c_str());
    }
};


// ----------------------------------------------------------- helpers ----

// Feeds one frame per tick at `hz` for `nticks`; `changes(k)` says whether
// frame k has new content. Ticks come right after the feed (async_tick order).
static void stream(Runner &r, Frame &f, uint64_t hz, int nticks, const std::function<bool(int)> &changes,
                   uint64_t seed = 1, const std::function<void(int)> &hook = nullptr)
{
    for (int k = 0; k < nticks; k++) {
        if (hook)
            hook(k);
        if (changes(k))
            fill_all(f, seed * 100000 + k);
        uint64_t now = at(k, hz);
        r.feed(f, now);
        r.tick(now);
    }
}

static bool all_pubs(const Runner &r, double from_s, double to_s, const std::function<bool(const Pub &)> &pred,
                     int *checked = nullptr)
{
    int n = 0;
    for (auto &p : r.pubs) {
        double t = secs(p.now);
        if (t < from_s || t >= to_s)
            continue;
        n++;
        if (!pred(p))
            return false;
    }
    if (checked)
        *checked = n;
    return n > 0;
}

static const Pub *first_pub_after(const Runner &r, double t_s)
{
    for (auto &p : r.pubs)
        if (secs(p.now) > t_s)
            return &p;
    return nullptr;
}

// -------------------------------------------------------- scenarios ----

static void s01_60fps(Runner &r)
{
    r.begin("s01_60fps", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    stream(r, f, 60, 300, [](int) { return true; });
    int n = 0;
    r.check(all_pubs(r, 0.05, 5.0, [](const Pub &p) { return p.fps == 60 && p.ft > 16.66 && p.ft < 16.67; }, &n),
            "fps must be 60 with frametime ~16.67 ms on every publish after 0.05 s (checked %d)", n);
    // Publish gate: two 60 Hz ticks measured in whole nanoseconds (33333333 ns)
    // are a hair short of 1/30 s, so the gate opens on every third tick — the
    // overlay refreshes at 20 Hz, not 30 Hz, on a perfectly regular clock.
    r.check(r.pubs.size() == 100, "publishes on ticks 0, 3, 6 ... 297 (got %zu)", r.pubs.size());
    r.check(r.out.graph_count == 297, "last publish (tick 297) carries 297 samples (got %d)", r.out.graph_count);
    r.check(r.out.graph_fps[r.out.graph_count - 1] == 60.0, "per-frame fps of the last sample is 60");
    r.end();
}

static void s02_30in60(Runner &r)
{
    r.begin("s02_30in60", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    stream(r, f, 60, 300, [](int k) { return k % 2 == 0; });
    r.check(all_pubs(r, 0.1, 5.0, [](const Pub &p) { return p.fps == 30; }), "fps must be 30 after 0.1 s");
    r.check(r.dbg.sample_written == false && r.dbg.unique == false, "last (duplicate) frame is not unique");
    r.check(r.out.graph_fps[r.out.graph_count - 1] == 30.0, "per-frame fps == 30");
    r.end();
}

static void s03_24in60(Runner &r)
{
    r.begin("s03_24in60", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    // 3:2 pulldown: new frames at k % 5 in {0, 2} -> durations 2,3,2,3 ticks
    stream(r, f, 60, 300, [](int k) { return k % 5 == 0 || k % 5 == 2; });
    r.check(all_pubs(r, 1.0, 5.0, [](const Pub &p) { return p.fps == 24; }), "fps must be 24 after 1 s");
    r.end();
}

static void s04_120fps(Runner &r)
{
    r.begin("s04_120fps", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    stream(r, f, 120, 600, [](int) { return true; });
    r.check(all_pubs(r, 0.5, 5.0, [](const Pub &p) { return p.fps == 120; }), "fps must be 120 after 0.5 s");
    r.check(all_pubs(r, 1.5, 5.0, [](const Pub &p) { return p.window == 120; }),
            "window is capped at 120 once the history holds 120+ samples");
    r.end();
}

static void s05_45fps(Runner &r)
{
    r.begin("s05_45fps", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    // frames at 45 Hz, ticks at 60 Hz, merged by time (feed before tick on ties)
    int nf = 225, nt = 300, i = 0, j = 0;
    while (i < nf || j < nt) {
        uint64_t tf = i < nf ? at(i, 45) : UINT64_MAX;
        uint64_t tt = j < nt ? at(j, 60) : UINT64_MAX;
        if (tf <= tt) {
            fill_all(f, 500000 + i);
            r.feed(f, tf);
            i++;
        } else {
            r.tick(tt);
            j++;
        }
    }
    r.check(all_pubs(r, 0.5, 5.0, [](const Pub &p) { return p.fps == 45; }), "fps must be 45 after 0.5 s");
    r.end();
}

static void s06_stutter(Runner &r)
{
    r.begin("s06_stutter", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    // a 100 ms freeze: ticks 121..125 re-feed the frame of tick 120
    stream(r, f, 60, 300, [](int k) { return !(k >= 121 && k <= 125); }, 6, [&](int k) {
        if (k == 126) {
            // nothing yet: assertions after the feed below
        }
    });
    // Recreate the state at k = 126 to inspect the sample: rerun a copy? Simpler:
    // the sample of the frame at 2.1 s is the one with ft == 100.
    bool found = false;
    for (int i = 0; i < r.out.graph_count; i++)
        if (r.out.graph_frametimes_raw[i] == 100.0) {
            found = true;
            r.check(r.out.graph_fps[i] == 40.0, "per-frame fps at the 100 ms gap == 40 (window 10) got %g",
                    r.out.graph_fps[i]);
            double ema = r.out.graph_frametimes[i];
            r.check(fabs(ema - (16.666666666666668 * 0.85 + 100.0 * 0.15)) < 0.01,
                    "EMA right after the gap ~29.17 (got %.4f)", ema);
        }
    r.check(found, "a 100 ms frametime sample exists");
    const Pub *p = first_pub_after(r, 2.1);
    r.check(p && p->fps == 55, "first publish after the gap reports 55 (got %d)", p ? p->fps : -1);
    r.check(all_pubs(r, 3.2, 5.0, [](const Pub &q) { return q.fps == 60; }), "fps back to 60 after 3.2 s");
    r.end();
}

static void s07_rate_change(Runner &r)
{
    r.begin("s07_rate_change", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    stream(r, f, 60, 420, [](int k) { return k < 180 ? true : (k % 2 == 0); }, 7);
    r.check(all_pubs(r, 1.0, 3.0, [](const Pub &p) { return p.fps == 60; }), "60 fps before the change");
    r.check(all_pubs(r, 4.5, 7.0, [](const Pub &p) { return p.fps == 30; }), "30 fps from 4.5 s on");
    // never below 30 or above 60 during the transition
    r.check(all_pubs(r, 3.0, 4.5, [](const Pub &p) { return p.fps >= 30 && p.fps <= 60; }), "transition stays in [30,60]");
    r.end();
}

static void s08_stale(Runner &r)
{
    r.begin("s08_stale", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    double ema_before = 0.0;
    stream(r, f, 60, 300, [](int k) { return k < 60 || k >= 240; }, 8, [&](int k) {
        if (k == 60)
            ema_before = r.dbg.ema; // EMA after the last unique frame (k = 59)
    });
    r.check(all_pubs(r, 1.0, 2.99, [](const Pub &p) { return p.fps == 60; }), "fps holds at 60 while frames repeat (<2 s)");
    int n = 0;
    r.check(all_pubs(r, 3.0, 3.99, [](const Pub &p) { return p.fps == 0 && p.ft == 0.0 && p.graph_count == 0; }, &n),
            "stale after 2 s without unique frames: fps 0, frametime 0, empty graph (checked %d)", n);
    // resume: first sample is the whole gap; EMA continues from the old value (never reset)
    bool found = false;
    for (int i = 0; i < r.out.graph_count && !found; i++)
        if (r.out.graph_frametimes_raw[i] > 2900.0) {
            found = true;
            double expect = ema_before * 0.85 + r.out.graph_frametimes_raw[i] * 0.15;
            r.check(fabs(r.out.graph_frametimes[i] - expect) < 0.01,
                    "EMA after resume continues from the pre-stale value (got %.3f, expected %.3f)",
                    r.out.graph_frametimes[i], expect);
        }
    r.check(found, "the resume frame records the ~3 s gap as a frametime sample");
    r.end();
}

static void s09_sensitivity_edge(Runner &r)
{
    r.begin("s09_sensitivity_edge", fps_core_params_defaults());
    if (!r.active()) return;
    // LAST_LINE, width 1000: one changed byte == exactly 0.1 % -> unique (>=)
    Frame f = make_frame(FPS_PIXFMT_NV12, 1000, 2);
    fill_all(f, 9);
    r.feed(f, at(0, 60));
    r.check(r.dbg.unique && !r.dbg.compared, "first frame is unique without a comparison");
    f.buf[(size_t)1 * f.linesize + 500] ^= 0x55;
    r.feed(f, at(1, 60));
    r.check(r.dbg.compared && r.dbg.diff == 1 && r.dbg.unique, "1/1000 bytes = 0.1 %% -> unique (got diff %zu unique %d)",
            r.dbg.diff, r.dbg.unique);
    r.feed(f, at(2, 60));
    r.check(r.dbg.compared && r.dbg.diff == 0 && !r.dbg.unique, "identical frame -> not unique");
    Frame g = make_frame(FPS_PIXFMT_NV12, 1001, 2);
    fill_all(g, 10);
    r.feed(g, at(3, 60)); // size change -> unique
    g.buf[(size_t)1 * g.linesize + 500] ^= 0x55;
    r.feed(g, at(4, 60));
    r.check(r.dbg.compared && r.dbg.diff == 1 && !r.dbg.unique, "1/1001 bytes < 0.1 %% -> not unique");
    r.tick(at(5, 60));
    r.end();
}

static void s10_method_divergence(Runner &r)
{
    for (int method = 0; method < 2; method++) {
        fps_core_params p = fps_core_params_defaults();
        p.analyze_method = method;
        r.begin(method == FPS_ANALYZE_DIFF ? "s10_method_diff" : "s10_method_lastline", p);
        if (!r.active()) continue;
        Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
        fill_all(f, 10);
        int uniques = 0;
        for (int k = 0; k < 20; k++) {
            if (k > 0)
                fill_rows(f, 0, 18, 1000 + k); // top half changes, bottom rows (incl. last line) static
            r.feed(f, at(k, 60));
            if (r.dbg.unique)
                uniques++;
            r.tick(at(k, 60));
        }
        if (method == FPS_ANALYZE_DIFF)
            r.check(uniques == 20, "DIFF sees every frame as unique (got %d)", uniques);
        else
            r.check(uniques == 1, "LAST_LINE sees only the first frame as unique (got %d)", uniques);
        r.end();
    }
}

static void s11_first_frame_and_size_change(Runner &r)
{
    r.begin("s11_size_change", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    for (int k = 0; k < 10; k++) {
        fill_all(f, 1100 + k);
        r.feed(f, at(k, 60));
        if (k == 0)
            r.check(r.dbg.unique && !r.dbg.sample_written && r.dbg.count == 0, "first frame: unique, no sample");
        r.tick(at(k, 60));
    }
    Frame g = make_frame(FPS_PIXFMT_NV12, 80, 36);
    fill_all(g, 1200);
    r.feed(g, at(10, 60));
    r.check(!r.dbg.compared && r.dbg.unique && r.dbg.sample_written && r.dbg.luma_size == 80,
            "size change: unique without comparison, sample written from the previous unique frame");
    r.check(fabs(r.dbg.ft_ms - (double)(at(10, 60) - at(9, 60)) / 1e6) < 1e-9, "ft spans from the last unique frame");
    fill_all(g, 1201);
    r.feed(g, at(11, 60));
    r.check(r.dbg.compared && r.dbg.luma_size == 80, "after the change the new size is compared");
    r.tick(at(11, 60));
    r.end();
}

// tearing helper: change only the top line, or every line, or nothing
static void feed_tear(Runner &r, Frame &f, int k, const char *mode, uint64_t seed)
{
    if (!strcmp(mode, "all"))
        fill_all(f, seed + k);
    else if (!strcmp(mode, "partial"))
        fill_rows(f, 0, 1, seed + k);
    r.feed(f, at(k, 60));
}

static void s12_tearing(Runner &r)
{
    fps_core_params p = fps_core_params_defaults();
    p.analyze_method = FPS_ANALYZE_DIFF;
    r.begin("s12_tearing", p);
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    fill_all(f, 12);
    int k = 0;
    const char *seq[] = {"all", "all", "none", "all", "partial", "partial", "all", "all", "all", "all"};
    bool expect[] = {false, false, false, false, false, true, true, true, true, false};
    for (int i = 0; i < 10; i++, k++) {
        feed_tear(r, f, k, seq[i], 1200);
        r.check(r.dbg.tearing_flag == expect[i], "step %d (%s): tearing %d expected %d", i, seq[i], r.dbg.tearing_flag,
                expect[i]);
    }
    // disabled: history untouched
    p.enable_tearing_detection = false;
    r.set_params(p);
    for (int i = 0; i < 3; i++, k++) {
        feed_tear(r, f, k, "partial", 1300);
        r.check(!r.dbg.tearing_flag, "disabled -> false");
    }
    p.enable_tearing_detection = true;
    r.set_params(p);
    feed_tear(r, f, k++, "partial", 1400);
    r.check(!r.dbg.tearing_flag, "re-enabled: history still holds one old hit -> 1 recent -> false");
    feed_tear(r, f, k++, "partial", 1400);
    r.check(r.dbg.tearing_flag, "second partial after re-enable -> true");
    // width change re-initialises the line buffers (one false), then works again
    Frame g = make_frame(FPS_PIXFMT_NV12, 80, 36);
    fill_all(g, 1500);
    r.feed(g, at(k++, 60));
    r.check(!r.dbg.tearing_flag, "width change -> re-init -> false");
    fill_rows(g, 0, 1, 1501);
    r.feed(g, at(k++, 60));
    r.check(r.dbg.tearing_flag, "history survives the re-init (2 recent hits) -> true");
    // Exactly at the limit the probe lines still fit, so detection runs: two
    // partial frames in a row must still be reported.
    Frame edge = make_frame(FPS_PIXFMT_NV12, 4096, 3);
    fill_all(edge, 1550);
    r.feed(edge, at(k++, 60)); // width change -> re-init of the probe lines
    fill_rows(edge, 0, 1, 1551);
    r.feed(edge, at(k++, 60));
    fill_rows(edge, 0, 1, 1552);
    r.feed(edge, at(k++, 60));
    r.check(r.dbg.tearing_flag, "width exactly 4096 is still analysed");
    // One pixel wider than the probe buffers -> skipped, history untouched
    Frame wide = make_frame(FPS_PIXFMT_NV12, 4097, 3);
    fill_all(wide, 1600);
    r.feed(wide, at(k++, 60));
    r.check(!r.dbg.tearing_flag, "width > 4096 -> tearing skipped");
    // height 1: the three probed lines coincide, so they always change together.
    // Six frames also flush the 5-slot history, which the re-init above leaves
    // untouched (the verdict right after a size change still sees old hits).
    Frame one = make_frame(FPS_PIXFMT_NV12, 64, 1);
    for (int i = 0; i < 6; i++) {
        fill_all(one, 1700 + i);
        r.feed(one, at(k++, 60));
    }
    r.check(!r.dbg.tearing_flag, "height 1 -> the three probed lines coincide -> no tearing");
    r.tick(at(k, 60));
    r.end();
}

static void s13_tearing_alignment(Runner &r)
{
    fps_core_params p = fps_core_params_defaults();
    p.analyze_method = FPS_ANALYZE_DIFF;
    r.begin("s13_tearing_alignment", p);
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    fill_all(f, 13);
    for (int k = 0; k < 8; k++) {
        if (k > 0)
            fill_rows(f, 0, 1, 1300 + k); // top line only: unique in DIFF (2.8 %), partial tear
        r.feed(f, at(k, 60));
        r.tick(at(k, 60));
    }
    r.tick(at(20, 60)); // the 1/30 s gate only opens every third tick; force a publish
    // samples: frame1 (1 recent hit -> false), frame2.. (>=2 -> true)
    r.check(r.out.graph_count == 7, "7 samples (got %d)", r.out.graph_count);
    r.check(r.out.graph_tearing[0] == false && r.out.graph_tearing[1] == true && r.out.graph_tearing[6] == true,
            "tearing flag of frame k is stored with frame k's own sample");
    r.end();
}

static void s14_luma(Runner &r)
{
    r.begin("s14_luma", fps_core_params_defaults());
    if (!r.active()) return;
    Impl &impl = *r.impls[0];
    // The helpers are pure, so run them on impls[0] and log the values into
    // every implementation log: the golden pins the numbers, and the
    // cross-implementation comparison still covers the feed rows below.
    auto logv = [&](const char *tag, const uint8_t *v, size_t n) {
        std::string row = tag;
        for (size_t i = 0; i < n; i++) {
            char b[8];
            snprintf(b, sizeof(b), " %u", v[i]);
            row += b;
        }
        row += "\n";
        for (auto &l : r.logs)
            l += row;
    };
    // BGRA / RGBA: white, black, pure R, G, B
    {
        Frame f = make_frame(FPS_PIXFMT_BGRA, 5, 1);
        uint8_t px[5][4] = {{255, 255, 255, 255}, {0, 0, 0, 255}, {0, 0, 255, 255}, {0, 255, 0, 255}, {255, 0, 0, 255}};
        memcpy(f.buf.data(), px, sizeof(px));
        uint8_t luma[5];
        r.check(impl.extract(f.view(), luma, 0, 1), "BGRA extract ok");
        uint8_t expect[5] = {235, 16, 82, 144, 41};
        r.check(memcmp(luma, expect, 5) == 0, "BGRA luma: white 235, black 16, R 82, G 144, B 41 (got %u %u %u %u %u)",
                luma[0], luma[1], luma[2], luma[3], luma[4]);
        logv("BGRA", luma, 5);
        Frame g = make_frame(FPS_PIXFMT_RGBA, 5, 1);
        uint8_t qx[5][4] = {{255, 255, 255, 255}, {0, 0, 0, 255}, {255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}};
        memcpy(g.buf.data(), qx, sizeof(qx));
        r.check(impl.extract(g.view(), luma, 0, 1) && memcmp(luma, expect, 5) == 0, "RGBA luma matches");
        logv("RGBA", luma, 5);
        // BGRX (XRGB capture) goes through the BGRA path: the 4th byte is
        // padding, often garbage, and must not change the luma
        Frame x = make_frame(FPS_PIXFMT_BGRA, 5, 1);
        uint8_t xx[5][4] = {{255, 255, 255, 0}, {0, 0, 0, 17}, {0, 0, 255, 128}, {0, 255, 0, 200}, {255, 0, 0, 77}};
        memcpy(x.buf.data(), xx, sizeof(xx));
        r.check(impl.extract(x.view(), luma, 0, 1) && memcmp(luma, expect, 5) == 0,
                "BGRX luma ignores the 4th byte (got %u %u %u %u %u)", luma[0], luma[1], luma[2], luma[3], luma[4]);
    }
    // grey ramp through BGRA: ((i*220+128)>>8)+16, range [16,235], no wrap
    {
        Frame f = make_frame(FPS_PIXFMT_BGRA, 256, 1);
        for (int i = 0; i < 256; i++) {
            f.buf[i * 4 + 0] = f.buf[i * 4 + 1] = f.buf[i * 4 + 2] = (uint8_t)i;
            f.buf[i * 4 + 3] = 255;
        }
        uint8_t luma[256];
        impl.extract(f.view(), luma, 0, 1);
        bool ok = luma[0] == 16 && luma[255] == 235;
        for (int i = 1; i < 256; i++)
            ok = ok && luma[i] >= luma[i - 1];
        r.check(ok, "grey ramp is monotonic from 16 to 235");
        logv("RAMP", luma, 256);
    }
    // packed YUV: YUY2 takes even bytes, UYVY odd bytes
    {
        Frame f = make_frame(FPS_PIXFMT_YUY2, 2, 1);
        uint8_t px[4] = {10, 200, 20, 210};
        memcpy(f.buf.data(), px, 4);
        uint8_t luma[2];
        impl.extract(f.view(), luma, 0, 1);
        r.check(luma[0] == 10 && luma[1] == 20, "YUY2 luma = bytes 0,2");
        Frame g = make_frame(FPS_PIXFMT_UYVY, 2, 1);
        uint8_t qx[4] = {200, 10, 210, 20};
        memcpy(g.buf.data(), qx, 4);
        impl.extract(g.view(), luma, 0, 1);
        r.check(luma[0] == 10 && luma[1] == 20, "UYVY luma = bytes 1,3");
        logv("YUY2/UYVY", luma, 2);
    }
    // planar with padding: rows come from linesize, not width
    {
        Frame f = make_frame(FPS_PIXFMT_NV12, 4, 2, 4);
        for (uint32_t y = 0; y < 2; y++)
            for (uint32_t x = 0; x < 8; x++)
                f.buf[y * 8 + x] = (uint8_t)(y * 16 + x);
        uint8_t luma[8];
        impl.extract(f.view(), luma, 0, 2);
        uint8_t expect[8] = {0, 1, 2, 3, 16, 17, 18, 19};
        r.check(memcmp(luma, expect, 8) == 0, "planar extraction honours linesize");
        uint8_t last[4];
        impl.extract(f.view(), last, 1, 1);
        r.check(memcmp(last, expect + 4, 4) == 0, "single-line extraction of the last line");
        logv("NV12pad", luma, 8);
    }
    // unsupported format: nothing analysed, state untouched
    {
        Frame ok = make_frame(FPS_PIXFMT_NV12, 64, 36);
        fill_all(ok, 14);
        r.feed(ok, at(0, 60));
        Frame bad = make_frame(FPS_PIXFMT_NV12, 64, 36);
        bad.fmt = 99;
        fill_all(bad, 15);
        int st = r.feed(bad, at(1, 60));
        r.check(st == FPS_FEED_UNSUPPORTED_FORMAT, "unsupported format status (got %d)", st);
        fill_all(ok, 16);
        r.feed(ok, at(2, 60));
        r.check(r.dbg.compared && r.dbg.sample_written, "state continued from the last supported frame");
        r.tick(at(2, 60));
        r.check(r.out.graph_count == 1, "exactly one sample (got %d)", r.out.graph_count);
    }
    r.end();
}

static void s15_csv(Runner &r)
{
    r.begin("s15_csv", fps_core_params_defaults());
    if (!r.active()) return;
    Impl &impl = *r.impls[0];
    char buf[64];
    impl.csv_line(buf, sizeof(buf), 1700000000LL, 60, 16.666);
    r.check(!strcmp(buf, "1700000000,60,16.67\n"), "csv line format (got %s)", buf);
    fs::path tmp = r.data_dir / "_out" / "keep_last_n.csv";
    fs::create_directories(tmp.parent_path());
    {
        std::ofstream o(tmp, std::ios::binary);
        for (int i = 0; i < 305; i++)
            o << "line" << i << "\n";
    }
    impl.csv_keep(tmp.string().c_str(), 300);
    {
        std::ifstream in(tmp, std::ios::binary);
        std::string first, line;
        int n = 0;
        while (std::getline(in, line)) {
            if (n == 0)
                first = line;
            n++;
        }
        // the rewrite goes through a text-mode FILE*, so every line ends CRLF
        r.check(n == 300 && first == "line5", "305 -> last 300 lines kept, first is line5, CRLF endings (n=%d first=%s)",
                n, first.c_str());
    }
    {
        std::ofstream o(tmp, std::ios::binary);
        o << std::string(600, 'x') << "\n";
    }
    impl.csv_keep(tmp.string().c_str(), 2);
    {
        std::ifstream in(tmp, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        // A 600-char line is read as 255 + 255 + 90 chunks, so it counts as three
        // "lines"; keeping 2 drops the first chunk. +1 for the CR of the CRLF.
        r.check(ss.str().size() == 600 + 1 - 255 + 1, "256-byte fgets granularity is preserved (size %zu)",
                ss.str().size());
    }
    impl.csv_keep((r.data_dir / "_out" / "does_not_exist.csv").string().c_str(), 10);
    r.check(true, "missing file is a no-op");
    r.end(false);
}

static void s16_graph_linearization(Runner &r)
{
    r.begin("s16_graph", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    // varying cadence so samples are distinguishable: gap_k = 16 + (k % 7) ms
    std::vector<double> fts;
    uint64_t now = T0;
    for (int k = 0; k < 966; k++) {
        if (k > 0) {
            uint64_t gap = (16 + (k % 7)) * 1000000ULL;
            now += gap;
            fts.push_back((double)gap / 1e6);
        }
        fill_all(f, 1600 + k);
        r.feed(f, now);
        if (k == 5) {
            r.tick(now);
            r.check(r.out.graph_count == 5, "5 samples after 6 frames (got %d)", r.out.graph_count);
            bool ok = true;
            for (int i = 0; i < 5; i++)
                ok = ok && r.out.graph_frametimes_raw[i] == fts[i];
            r.check(ok, "graph is oldest -> newest");
        }
    }
    r.tick(now);
    r.check(r.out.graph_count == 960, "graph holds the last 960 of 965 samples (got %d)", r.out.graph_count);
    r.check(r.out.graph_frametimes_raw[0] == fts[5], "first graph entry is the 6th sample");
    r.check(r.out.graph_frametimes[959] == r.dbg.ema, "smoothed graph ends with the current EMA");
    r.end();
}

static void s17_params_mid_run(Runner &r)
{
    r.begin("s17_params", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    for (int k = 0; k < 5; k++) {
        fill_all(f, 1700 + k);
        r.feed(f, at(k, 60));
        r.tick(at(k, 60));
    }
    fps_core_params p = fps_core_params_defaults();
    p.analyze_method = FPS_ANALYZE_DIFF;
    r.set_params(p);
    fill_all(f, 1705);
    r.feed(f, at(5, 60));
    r.check(!r.dbg.compared && r.dbg.unique && r.dbg.luma_size == 64 * 36,
            "method switch changes the ROI size -> unique without comparison");
    // interval change does not reset the publish clock
    uint64_t last_pub = r.pubs.back().now;
    p.update_interval = 1.0;
    r.set_params(p);
    size_t before = r.pubs.size();
    for (int k = 6; k < 80; k++) {
        fill_all(f, 1700 + k);
        r.feed(f, at(k, 60));
        r.tick(at(k, 60));
    }
    r.check(r.pubs.size() == before + 1, "exactly one publish in the next ~1.2 s (got %zu)", r.pubs.size() - before);
    r.check(r.pubs.back().now - last_pub >= 1000000000ULL, "the publish is >= 1 s after the previous one");
    r.end();
}

static void s18_fuzz(Runner &r)
{
    if (r.impls.size() < 2) {
        printf("  (s18_fuzz skipped: needs two implementations)\n");
        return;
    }
    for (int seed = 1; seed <= 20; seed++) {
        char name[32];
        snprintf(name, sizeof(name), "s18_fuzz_%02d", seed);
        fps_core_params p = fps_core_params_defaults();
        r.begin(name, p);
        if (!r.active()) continue;
        uint64_t s = 0xF00D + seed;
        auto rnd = [&](uint32_t n) { return (uint32_t)(lcg_next(s) % n); };
        int fmts[] = {FPS_PIXFMT_NV12, FPS_PIXFMT_YUY2, FPS_PIXFMT_UYVY, FPS_PIXFMT_BGRA, FPS_PIXFMT_RGBA, FPS_PIXFMT_I420};
        Frame f = make_frame(fmts[rnd(6)], 16 + rnd(113), 16 + rnd(113));
        fill_all(f, seed);
        uint64_t now = T0;
        for (int e = 0; e < 2000; e++) {
            now += 1000000ULL * (1 + rnd(40)) + (rnd(50) == 0 ? 2500000000ULL : 0);
            switch (rnd(10)) {
            case 0: break;                                        // duplicate frame
            case 1: fill_rows(f, f.h - 1, f.h, now); break;       // last line only
            case 2: fill_rows(f, 0, f.h / 2, now); break;         // top half
            case 3: f.buf[rnd((uint32_t)f.buf.size())] ^= 1; break; // one byte
            case 4: fill_rows(f, 0, 1, now); break;               // top line (tearing)
            case 5: {
                fps_core_params q = p;
                q.analyze_method = rnd(2);
                q.sensitivity = rnd(3) * 0.1;
                q.enable_tearing_detection = rnd(4) != 0;
                q.update_interval = rnd(2) ? 1.0 / 30.0 : 0.5;
                r.set_params(q);
                p = q;
                break;
            }
            case 6:
                f = make_frame(fmts[rnd(6)], 16 + rnd(113), 16 + rnd(113));
                fill_all(f, now);
                break;
            default: fill_all(f, now); break;
            }
            if (rnd(3))
                r.feed(f, now);
            if (rnd(2))
                r.tick(now);
        }
        r.end(false);
    }
}

static void s19_zero_ft(Runner &r)
{
    r.begin("s19_zero_ft", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
    fill_all(f, 19);
    r.feed(f, at(0, 60));
    fill_all(f, 20);
    r.feed(f, at(0, 60)); // same timestamp
    r.check(r.dbg.sample_written && r.dbg.ft_ms == 0.0 && r.dbg.fps_pf == 0.0,
            "ft 0 -> inst_fps 30 -> window 1 -> avg 0 -> per-frame fps 0");
    r.tick(at(0, 60));
    r.check(r.out.fps == 0, "publish of a zero frametime gives fps 0");
    fill_all(f, 21);
    r.feed(f, at(1, 60));
    // the averaging window is min(inst_fps, count BEFORE this sample) == 1, and
    // the EMA re-initialises because the stored value is still 0 (0 <= 0)
    r.check(r.dbg.sample_written && r.dbg.fps_pf == 60.0, "next sample: window 1 -> 60 (got %g)", r.dbg.fps_pf);
    r.check(fabs(r.dbg.ema - r.dbg.ft_ms) < 1e-12, "EMA re-initialises from the zero frametime");
    r.tick(at(1, 60));
    r.end();
}

// The per-frame averaging window is sized from an instantaneous estimate that
// is clamped to [10, 120]. With every frametime equal the window size cannot
// change the average, so both scenarios below put one very different sample
// exactly at the clamp boundary: whether it falls inside the window or just
// outside it is then visible in the per-frame FPS.

// Content slower than 10 fps: the estimate would be 8, the clamp raises it to
// 10, so the tenth sample back is included in the average.
static void s21_below_clamp(Runner &r)
{
    r.begin("s21_below_clamp", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);

    std::vector<uint64_t> gaps;
    for (int i = 0; i < 15; i++) gaps.push_back(20000000ULL);  // build up history
    gaps.push_back(500000000ULL);                              // the outlier, 500 ms
    for (int i = 0; i < 8; i++) gaps.push_back(20000000ULL);
    gaps.push_back(125000000ULL);                              // 8 fps -> clamped to 10

    uint64_t now = T0;
    fill_all(f, 2100);
    r.feed(f, now);
    r.tick(now);
    for (size_t i = 0; i < gaps.size(); i++) {
        now += gaps[i];
        fill_all(f, 2101 + (uint64_t)i);
        r.feed(f, now);
        r.tick(now);
    }
    // window 10: (125 + 8*20 + 500) / 10 = 78.5 ms -> 13
    // window  9: (125 + 8*20) / 9       = 31.7 ms -> 32
    r.check(r.dbg.ft_ms == 125.0, "the last frame took 125 ms (got %g)", r.dbg.ft_ms);
    r.check(r.dbg.fps_pf == 13.0, "the clamp pulls the 500 ms sample into the window (got %g)", r.dbg.fps_pf);
    // The tick window has a floor of 10 that can never fire: it is only tested
    // when there are at least 10 samples, and then the window is at least 10
    // already. Recorded here so the dead branch is not mistaken for coverage.
    r.check(r.out.window == r.dbg.count, "the tick averages every sample it has (%d of %d)", r.out.window,
            r.dbg.count);
    r.end();
}

// Content faster than 120 fps: the estimate would be 200, the cap lowers it to
// 120, so the sample just beyond that stays out of the average.
static void s22_above_clamp(Runner &r)
{
    r.begin("s22_above_clamp", fps_core_params_defaults());
    if (!r.active()) return;
    Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);

    std::vector<uint64_t> gaps;
    for (int i = 0; i < 130; i++) gaps.push_back(5000000ULL);
    gaps.push_back(500000000ULL);                              // the outlier
    for (int i = 0; i < 120; i++) gaps.push_back(5000000ULL);  // exactly fills the window
    const size_t boundary = gaps.size() - 1;
    for (int i = 0; i < 200; i++) gaps.push_back(5000000ULL);  // let the outlier scroll out

    uint64_t now = T0;
    fill_all(f, 2200);
    r.feed(f, now);
    r.tick(now);
    double pf_at_boundary = -1.0;
    for (size_t i = 0; i < gaps.size(); i++) {
        now += gaps[i];
        fill_all(f, 2201 + (uint64_t)i);
        r.feed(f, now);
        if (i == boundary)
            pf_at_boundary = r.dbg.fps_pf;
        r.tick(now);
    }
    // At the boundary frame the outlier sits exactly one sample beyond the cap:
    //   window 120: 120 * 5 ms / 120 = 5 ms          -> 200
    //   window 121: (120 * 5 + 500) / 121 = 9.09 ms  -> 110
    r.check(pf_at_boundary == 200.0, "the cap keeps the 500 ms sample out of the window (got %g)", pf_at_boundary);
    r.check(r.dbg.ft_ms == 5.0, "the last frame took 5 ms (got %g)", r.dbg.ft_ms);
    r.check(r.dbg.count == 451, "all 451 samples are kept (got %d)", r.dbg.count);
    // Once the reading is steady at 200 the window feedback cannot shrink it
    // (it only applies below the window size), so the cap itself is visible.
    r.check(r.out.fps == 200, "the published reading settles at 200 (got %d)", r.out.fps);
    r.check(r.out.window == 120, "the tick window caps at 120 despite 451 samples (got %d)", r.out.window);
    r.end();
}

// Everything a replay has to reproduce from a recorded row.
struct Rec {
    bool unique, tear, sample;
    double ft, ema, pf;
    int pos, count;
    bool published;
    int fps, window, graph;
    double ft_ms;
    bool tear_pub;
    uint32_t ghash;
};

static bool rec_equal(const Rec &a, const Rec &b)
{
    return a.unique == b.unique && a.tear == b.tear && a.sample == b.sample && a.ft == b.ft && a.ema == b.ema &&
           a.pf == b.pf && a.pos == b.pos && a.count == b.count && a.published == b.published && a.fps == b.fps &&
           a.window == b.window && a.graph == b.graph && a.ft_ms == b.ft_ms && a.tear_pub == b.tear_pub &&
           a.ghash == b.ghash;
}

// The trace replay model, checked against the pixel path.
//
// fps-cli replays a recorded session by feeding back only the decisions the
// live plugin made (new frame or duplicate, tearing or not, and when), never
// the pixels. That only proves anything if replaying a decision leaves the
// core in exactly the state the pixels would have. This scenario runs a stream
// through the full pixel path, records what it decided, replays those
// decisions into a fresh core and requires the two to agree on every frame and
// every publish.
static void s20_decision_replay(Runner &r)
{
    r.begin("s20_decision_replay", fps_core_params_defaults());
    if (!r.active()) return;

    struct Event {
        uint64_t now;
        bool is_frame;
        bool unique, tear;
    };

    for (size_t k = 0; k < r.impls.size(); k++) {
        Impl &impl = *r.impls[k];
        fps_core_params p = fps_core_params_defaults();
        p.analyze_method = FPS_ANALYZE_DIFF; // exercise tearing and full-frame diffs together
        impl.reset(p);

        Frame f = make_frame(FPS_PIXFMT_NV12, 64, 36);
        std::vector<Event> events;
        std::vector<Rec> live;
        uint64_t now = T0;

        for (int i = 0; i < 400; i++) {
            // A mixed stream: steady motion, duplicates, a partial-frame update
            // that trips tearing, a 100 ms stutter and a 3 s stale gap.
            if (i == 200)
                now += 100000000ULL; // stutter
            else if (i == 300)
                now += 3000000000ULL; // stale
            else
                now += 16666666ULL;

            if (i % 7 == 3)
                ; // duplicate: leave the frame untouched
            else if (i % 6 == 0 || i % 6 == 1)
                // Top line only, on two consecutive frames: one partial frame
                // is not enough, the verdict needs two hits within five.
                fill_rows(f, 0, 1, 2000 + i);
            else
                fill_all(f, 2000 + i);

            impl.feed(f.view(), now);
            fps_core_frame_debug d{};
            impl.last_frame(d);
            events.push_back({now, true, d.unique, d.tearing_flag});

            Rec rec{};
            rec.unique = d.unique;
            rec.tear = d.tearing_flag;
            rec.sample = d.sample_written;
            rec.ft = d.ft_ms;
            rec.ema = d.ema;
            rec.pf = d.fps_pf;
            rec.pos = d.pos;
            rec.count = d.count;
            live.push_back(rec);

            fps_core_output o{};
            if (impl.tick(now, o)) {
                events.push_back({now, false, false, false});
                Rec t{};
                t.published = true;
                t.fps = o.fps;
                t.window = o.window;
                t.graph = o.graph_count;
                t.ft_ms = o.frametime_ms;
                t.tear_pub = o.tearing_detected;
                t.ghash = graph_hash(o);
                live.push_back(t);
            }
        }

        // Replay: same events, same times, decisions only.
        impl.reset(p);
        std::vector<Rec> replayed;
        for (const Event &e : events) {
            if (e.is_frame) {
                impl.feed_decision(e.unique, e.tear, e.now);
                fps_core_frame_debug d{};
                impl.last_frame(d);
                Rec rec{};
                rec.unique = d.unique;
                rec.tear = d.tearing_flag;
                rec.sample = d.sample_written;
                rec.ft = d.ft_ms;
                rec.ema = d.ema;
                rec.pf = d.fps_pf;
                rec.pos = d.pos;
                rec.count = d.count;
                replayed.push_back(rec);
            } else {
                fps_core_output o{};
                bool pub = impl.tick(e.now, o);
                Rec t{};
                t.published = pub;
                t.fps = o.fps;
                t.window = o.window;
                t.graph = o.graph_count;
                t.ft_ms = o.frametime_ms;
                t.tear_pub = o.tearing_detected;
                t.ghash = pub ? graph_hash(o) : 0;
                replayed.push_back(t);
            }
        }

        r.check(live.size() == replayed.size(), "%s: replay produced %zu records for %zu recorded",
                impl.name(), replayed.size(), live.size());
        size_t n = live.size() < replayed.size() ? live.size() : replayed.size();
        size_t first_bad = n;
        for (size_t i = 0; i < n; i++)
            if (!rec_equal(live[i], replayed[i])) {
                first_bad = i;
                break;
            }
        r.check(first_bad == n, "%s: replaying the decisions reproduces the pixel run (first difference at record %zu)",
                impl.name(), first_bad);

        // The stream has to be interesting enough for the check to mean
        // something: duplicates, tearing, a stutter and a stale reset.
        int uniques = 0, tears = 0, pubs = 0, zero_pubs = 0;
        for (const Rec &rec : live) {
            if (rec.published) {
                pubs++;
                if (rec.fps == 0)
                    zero_pubs++;
            } else {
                if (rec.unique) uniques++;
                if (rec.tear) tears++;
            }
        }
        r.check(uniques > 300 && uniques < 400, "%s: the stream mixes unique and duplicate frames (%d unique)",
                impl.name(), uniques);
        r.check(tears > 20, "%s: tearing fires during the run (%d frames)", impl.name(), tears);
        r.check(zero_pubs > 0, "%s: the stale reset is exercised (%d zero publishes)", impl.name(), zero_pubs);
        r.check(pubs > 50, "%s: enough publishes to compare (%d)", impl.name(), pubs);
    }
    r.end(false);
}

#ifdef FPS_SELFTEST_HAVE_CORE
// ------------------------------------------------ capture-path beat (core) ----
//
// The beat detector only exists in fps-core (the v0.5.0 oracle has no
// counterpart), so these scenarios drive the core directly, at the decision
// level: an OBS tick loop at `obs_hz`, a source clock at `src_hz`, and a game
// whose frames the source carries.
//
//   card mode: the capture device resamples the source to the OBS rate itself.
//              Every tick delivers one frame; a frame repeats when the source
//              did not advance (Elgato 4K X at "60" with a 59.94 Hz console).
//   obs mode:  source frames arrive at src_hz with +-jitter; OBS picks the
//              newest frame per tick (unbuffered). No arrival = empty tick,
//              two arrivals = the older one is dropped.

struct BeatSim {
    double obs_hz = 60.0;
    double src_hz = 60000.0 / 1001.0;
    bool card = true;
    double jitter_ms = 0.0;
    double phase = 0.5;          // source phase offset, in source periods
    int game_div = 1;            // game shows a new image every game_div source frames
    double drop_p = 0.0;         // per game frame: chance the game repeats its last image
    double switch_s = -1.0;      // from here on the source runs at switch_hz
    double switch_hz = 60.0;
    double hitch_every_s = 0.0;  // the game repeats one image every N s exactly (timer-driven stutter)
    double freeze_every_s = 0.0; // the game freezes for freeze_len_s every N s exactly
    double freeze_len_s = 0.0;
    double empty_p = 0.0;        // per tick: a stray empty tick (obs-side hiccup), card mode
    double reset_at_s = -1.0;    // fps_core_beat_reset at this time (settings changed)
    int mask_repeat = 0;         // card mode: the Nth capture repeat goes unseen (delivered as new)
    uint64_t seed = 1;
};

struct BeatRun {
    fps_beat beat{};             // at the end
    double first_detect_s = -1;  // first whole second at which a beat was reported
    double first_clear_s = -1;   // first whole second at which CLEAR was reported
    bool clear_before_detect = false;
    bool uneven_seen = false;
    bool uneven_before_detect = false; // the card's own beat blamed on the game
    std::vector<int> status;     // status at each whole second (index = second)
    std::vector<double> observed;
};

static BeatRun run_beat(const BeatSim &s, double seconds)
{
    BeatRun br;
    fps_core_params p = fps_core_params_defaults();
    fps_core *c = fps_core_create(&p);
    uint64_t rng = s.seed * 0x9E3779B97F4A7C15ull + 1;
    auto rnd = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (double)(rng >> 11) / 9007199254740992.0;
    };

    // Source frame k: when it is captured and which game image it shows.
    std::vector<double> src_t;      // arrival time (s)
    std::vector<long long> gid;     // game image id
    long long game_img = 0;
    auto ensure_src = [&](double until) {
        while (src_t.empty() || src_t.back() < until + 1.0) {
            size_t k = src_t.size();
            double t;
            if (k == 0) {
                t = s.phase / s.src_hz;
            } else {
                double base = src_t.size() >= 1 ? src_t[k - 1] : 0.0;
                double hz = (s.switch_s >= 0.0 && base >= s.switch_s) ? s.switch_hz : s.src_hz;
                t = base + 1.0 / hz;
            }
            src_t.push_back(t);
            bool hold = s.drop_p > 0.0 && rnd() < s.drop_p;
            if (s.hitch_every_s > 0.0 && k > 0) {
                double prev = src_t[k - 1];
                hold = hold || floor(t / s.hitch_every_s) != floor(prev / s.hitch_every_s);
            }
            if (s.freeze_every_s > 0.0 && fmod(t, s.freeze_every_s) < s.freeze_len_s && t > s.freeze_every_s)
                hold = true;
            if (k % (size_t)s.game_div == 0 && !hold)
                game_img++;
            gid.push_back(game_img);
        }
    };
    auto arrival = [&](size_t k) { return src_t[k] + (s.jitter_ms > 0.0 ? (rnd() - 0.5) * 2e-3 * s.jitter_ms : 0.0); };

    std::vector<double> arr; // jittered arrival per source frame (obs mode)
    long long prev_gid = -1;
    int card_repeats = 0;
    size_t next_src = 0;
    int last_reported = -1;
    const uint64_t base_ns = 1000000000ull;
    const long long nticks = (long long)(seconds * s.obs_hz);
    for (long long n = 1; n <= nticks; n++) {
        double t = n / s.obs_hz;
        uint64_t now = base_ns + (uint64_t)(t * 1e9);
        ensure_src(t);
        int frames = 1;
        long long shown;
        if (s.card) {
            // newest source frame captured by now
            while (next_src + 1 < src_t.size() && src_t[next_src + 1] <= t)
                next_src++;
            shown = gid[next_src];
            if (s.empty_p > 0.0 && rnd() < s.empty_p)
                frames = 0;
        } else {
            while (arr.size() < src_t.size())
                arr.push_back(arrival(arr.size()));
            int count = 0;
            size_t newest = next_src;
            while (next_src < arr.size() && arr[next_src] <= t) {
                newest = next_src;
                next_src++;
                count++;
            }
            frames = count ? 1 : 0;
            shown = count ? gid[newest] : prev_gid;
        }
        if (frames) {
            bool unique = shown != prev_gid;
            if (s.card && !unique && ++card_repeats == s.mask_repeat)
                unique = true; // this repeat is missed (e.g. hidden behind a game hitch)
            fps_core_debug_feed_decision(c, unique, false, now);
            prev_gid = shown;
        }
        fps_core_obs_tick(c, now, frames);
        if (s.reset_at_s >= 0.0 && t >= s.reset_at_s && t - 1.0 / s.obs_hz < s.reset_at_s)
            fps_core_beat_reset(c, now);
        int sec = (int)t;
        if (sec != last_reported) {
            last_reported = sec;
            fps_beat b{};
            fps_core_get_beat(c, now, s.obs_hz, &b);
            if (b.detected && br.first_detect_s < 0)
                br.first_detect_s = t;
            if (b.status == FPS_BEAT_CLEAR) {
                if (br.first_clear_s < 0)
                    br.first_clear_s = t;
                if (br.first_detect_s < 0)
                    br.clear_before_detect = true;
            }
            br.uneven_seen = br.uneven_seen || b.uneven;
            if (b.uneven && br.first_detect_s < 0)
                br.uneven_before_detect = true;
            br.status.resize((size_t)sec + 1, -1);
            br.observed.resize((size_t)sec + 1, 0.0);
            br.status[(size_t)sec] = b.status;
            br.observed[(size_t)sec] = b.observed_s;
        }
    }
    fps_core_get_beat(c, base_ns + (uint64_t)(seconds * 1e9), s.obs_hz, &br.beat);
    fps_core_destroy(c);
    return br;
}

static void b_report(Runner &r, const char *name, const BeatRun &br)
{
    if (r.verbose)
        printf("    %s: detected=%d period %.3f s signal %.4f nominal %.4f origin %d chain %d clusters %d first %.1f s\n",
               name, br.beat.detected, br.beat.period_s, br.beat.signal_hz, br.beat.nominal_hz, br.beat.origin,
               br.beat.chain, br.beat.clusters, br.first_detect_s);
}

// --- s24: altered-frame check (capture devices that dither every frame) ---
struct AltSim {
    int game_div = 2;          // new content every game_div source frames (60 Hz source)
    bool still = false;        // content never changes after the first frame
    int fade_frames = 0;       // the first N new-content frames are a +1 brightness step (tiny change)
    double noise_p = 0.0;      // the card: fraction of pixels moved by 1..noise_max on every frame
    int noise_max = 1;
    double clean_after_s = -1; // the card stops adding noise from this second on
    bool measure = true;       // params.measure_deltas
    double sensitivity = -1;   // params.sensitivity, -1 = default
    bool histogram = false;    // params.measure_histogram (the trace's full |d| histogram)
    uint64_t seed = 3;
};
struct AltRun {
    double first_altered_s = -1;
    double cleared_s = -1;     // first second it was not altered after having been
    bool altered_end = false;
    int fps_end = 0;
    bool hist_ok = true;       // every compared frame: sum(deltas) == size, diff == size - d0
};

static AltRun run_altered(const AltSim &s, double seconds)
{
    AltRun ar;
    fps_core_params p = fps_core_params_defaults();
    p.measure_deltas = s.measure;
    p.measure_histogram = s.measure && s.histogram;
    if (s.sensitivity >= 0)
        p.sensitivity = s.sensitivity;
    fps_core *c = fps_core_create(&p);
    const uint32_t W = 512, H = 4;
    Frame content = make_frame(FPS_PIXFMT_NV12, W, H);
    Frame out = make_frame(FPS_PIXFMT_NV12, W, H);
    uint64_t rng = s.seed * 0x9E3779B97F4A7C15ull + 7;
    auto rnd = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    int new_frames = 0;
    fps_core_output o{};
    const uint64_t n = (uint64_t)(seconds * 60);
    for (uint64_t k = 0; k < n; k++) {
        uint64_t now = at(k, 60);
        double t = secs(now);
        bool fresh = k == 0 || (!s.still && k % (uint64_t)s.game_div == 0);
        if (fresh) {
            if (k > 0 && new_frames <= s.fade_frames) {
                for (auto &v : content.buf)
                    v = (uint8_t)(v < 235 ? v + 1 : v); // a slow fade: one level per new frame
            } else {
                for (auto &v : content.buf)
                    v = (uint8_t)(32 + rnd() % 160);
            }
            new_frames++;
        }
        out.buf = content.buf;
        bool noisy = s.noise_p > 0.0 && !(s.clean_after_s >= 0 && t >= s.clean_after_s);
        if (noisy) {
            for (auto &v : out.buf) {
                if ((double)(rnd() % 10000) / 10000.0 < s.noise_p) {
                    int d = 1 + (int)(rnd() % (uint64_t)s.noise_max);
                    v = (uint8_t)((rnd() & 1) ? v + d : v - d);
                }
            }
        }
        fps_frame_view view = out.view();
        fps_core_feed(c, &view, now);
        fps_core_frame_debug d;
        fps_core_debug_last_frame(c, &d);
        if (d.deltas_valid) {
            uint64_t sum = 0;
            for (int i = 0; i < FPS_CORE_DELTA_BUCKETS; i++)
                sum += d.deltas[i];
            ar.hist_ok = ar.hist_ok && sum == d.luma_size && d.diff == d.luma_size - d.deltas[0];
        }
        fps_core_tick(c, now, &o);
        bool alt = fps_core_frames_altered(c);
        if (alt && ar.first_altered_s < 0)
            ar.first_altered_s = t;
        if (!alt && ar.first_altered_s >= 0 && ar.cleared_s < 0)
            ar.cleared_s = t;
    }
    ar.altered_end = fps_core_frames_altered(c);
    ar.fps_end = o.fps;
    fps_core_destroy(c);
    return ar;
}

static void s24_altered_frames(Runner &r)
{
    r.begin("s24_altered_frames", fps_core_params_defaults());
    {   // a good card, game at 30: repeats are identical, never altered, reads 30
        AltSim s;
        s.histogram = true; // the trace's histogram path, checked below
        AltRun a = run_altered(s, 20);
        r.check(a.first_altered_s < 0 && a.fps_end == 30, "good card, game 30: not altered, %d FPS", a.fps_end);
        r.check(a.hist_ok, "difference histogram adds up to the compared bytes and matches diff");
    }
    {   // a dithering card (4K X): +-1 on 28% of pixels, game at 30 -> altered in ~3 s
        AltSim s;
        s.noise_p = 0.28;
        AltRun a = run_altered(s, 20);
        r.check(a.first_altered_s > 0 && a.first_altered_s <= 3.5 && a.altered_end,
                "dithering card, game 30: altered after %.2f s (<= 3.5), still at the end", a.first_altered_s);
    }
    {   // the same card on a still picture: every frame is a tiny-only "new" frame
        // (+-1 on both frames compared: differences up to 2, as on the 4K X)
        AltSim s;
        s.noise_p = 0.10;
        s.still = true;
        AltRun a = run_altered(s, 10);
        r.check(a.first_altered_s > 0 && a.first_altered_s <= 2.0,
                "dithering card, still picture: altered after %.2f s (<= 2)", a.first_altered_s);
    }
    {   // a dithering card with a game at a locked 60: no repeats, nothing to tell (README limitation)
        AltSim s;
        s.noise_p = 0.28;
        s.game_div = 1;
        AltRun a = run_altered(s, 20);
        r.check(a.first_altered_s < 0, "dithering card, game 60: not decidable, not altered");
    }
    {   // differences above 2 levels are content, not dither: the rule stops at |d| <= 2
        // (+-2 on both frames compared reaches 3 and 4)
        AltSim s;
        s.noise_p = 0.5;
        s.noise_max = 2;
        s.still = true;
        AltRun a = run_altered(s, 10);
        r.check(a.first_altered_s < 0, "changes of up to 3 levels count as content: not altered");
    }
    {   // a good card, a 1 s fade at 60 (tiny changes only): below the trigger, not altered
        AltSim s;
        s.game_div = 1;
        s.fade_frames = 60;
        AltRun a = run_altered(s, 1.2);
        r.check(a.first_altered_s < 0, "good card, 1 s fade at 60: not altered");
    }
    {   // a good card, a 3 s fade in a 30 FPS game: the repeats are identical and keep it clear
        AltSim s;
        s.fade_frames = 90;
        AltRun a = run_altered(s, 10);
        r.check(a.first_altered_s < 0, "good card, 3 s fade in a 30 FPS game: identical repeats keep it clear");
    }
    {   // a good card, a 3 s fade at 60 followed by a 30 FPS game: the fade can trip the
        // check (no repeat to prove the card), the first identical repeats clear it
        AltSim s;
        s.game_div = 1;
        s.fade_frames = 180;
        AltRun a = run_altered(s, 3.0);
        r.check(a.first_altered_s > 0, "good card, 3 s fade at 60: trips the check (documented limitation)");
    }
    {   // the card stops dithering (format switched): identical frames clear it quickly
        AltSim s;
        s.noise_p = 0.28;
        s.clean_after_s = 6.0;
        AltRun a = run_altered(s, 12);
        r.check(a.first_altered_s > 0 && a.cleared_s > 6.0 && a.cleared_s < 6.5 && !a.altered_end,
                "dithering stops at 6 s: altered at %.2f s, cleared at %.2f s", a.first_altered_s, a.cleared_s);
    }
    {   // noise below the sensitivity threshold: the repeats are not counted as new, the
        // reading is right (30), so there is nothing to hide
        AltSim s;
        s.noise_p = 0.01;
        s.sensitivity = 5.0;
        AltRun a = run_altered(s, 20);
        r.check(a.first_altered_s < 0 && a.fps_end == 30,
                "noise below the sensitivity threshold: reads %d, not altered", a.fps_end);
    }
    {   // the check needs the histogram: with measure_deltas off it never runs
        AltSim s;
        s.noise_p = 0.28;
        s.measure = false;
        AltRun a = run_altered(s, 10);
        r.check(a.first_altered_s < 0, "measure_deltas off: the check does not run");
    }
    {   // the beat detector can be switched off: no clusters, no verdict
        fps_core_params p = fps_core_params_defaults();
        p.beat_enabled = false;
        fps_core *c = fps_core_create(&p);
        for (int k = 0; k < 60 * 120; k++)
            fps_core_obs_tick(c, at((uint64_t)k, 60), (k % 1000 == 999) ? 0 : 1); // an OBS repeat every ~16.7 s
        fps_beat b;
        fps_core_get_beat(c, at(60 * 120, 60), 60.0, &b);
        r.check(b.clusters == 0 && !b.detected, "beat_enabled off: no clusters (%d), no verdict", b.clusters);
        fps_core_destroy(c);
    }
}

// --- s25: frame pacing (per game frame, unsmoothed, from device timestamps) ---
struct PacingSim {
    std::vector<int> pattern = {2};  // game frame durations in refresh periods, repeated
    std::vector<int> lost;           // source frame indices the capture device loses
    bool sync = false;               // no device timestamps: analysis clock + OBS period
    int jitter_us = 0;               // analysis clock jitter (+-), sync path
    uint64_t period_ns = 16683333;   // 59.94 Hz
};
struct PacingRun {
    std::vector<double> ms;
    std::vector<bool> gap;
    int fps = 0;
};

static PacingRun run_pacing(const PacingSim &s, int frames)
{
    PacingRun pr;
    fps_core_params p = fps_core_params_defaults();
    if (s.sync)
        p.pacing_period_ns = s.period_ns;
    fps_core *c = fps_core_create(&p);
    Frame f = make_frame(FPS_PIXFMT_NV12, 512, 4);
    uint64_t rng = 99;
    auto rnd = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    size_t pi = 0;
    int left = 0;
    fps_core_output o{};
    for (int k = 0; k < frames; k++) {
        if (left == 0) { // a new game frame
            for (auto &v : f.buf)
                v = (uint8_t)(32 + rnd() % 160);
            left = s.pattern[pi++ % s.pattern.size()];
        }
        left--;
        bool lost = false;
        for (int l : s.lost)
            lost = lost || l == k;
        uint64_t ts = T0 + (uint64_t)k * s.period_ns;
        if (lost)
            continue;
        int64_t jit = s.jitter_us ? ((int64_t)(rnd() % (2 * s.jitter_us + 1)) - s.jitter_us) * 1000 : 0;
        uint64_t now = ts + 3000000 + (uint64_t)(jit + 2000000); // the filter sees it a bit later
        fps_frame_view v = f.view();
        v.src_ts_ns = s.sync ? 0 : ts;
        fps_core_feed(c, &v, now);
        fps_core_tick(c, now, &o);
    }
    for (int i = 0; i < o.graph_count; i++) {
        pr.ms.push_back(o.graph_pacing[i]);
        pr.gap.push_back(o.graph_pacing_gap[i]);
    }
    pr.fps = o.fps;
    fps_core_destroy(c);
    return pr;
}

static bool all_near(const std::vector<double> &v, size_t from, double want, double tol)
{
    for (size_t i = from; i < v.size(); i++)
        if (fabs(v[i] - want) > tol)
            return false;
    return from < v.size();
}

static void s25_frame_pacing(Runner &r)
{
    r.begin("s25_frame_pacing", fps_core_params_defaults());
    const double P = 16.683333;
    {   // a 30 FPS game at 59.94 Hz, even pacing: every frame 2 periods - exactly, although
        // OBS's clock jitters by +-2 ms (the value comes from the device's timestamps)
        PacingSim s;
        s.jitter_us = 2000;
        PacingRun pr = run_pacing(s, 600);
        bool nogap = true;
        for (bool g : pr.gap)
            nogap = nogap && !g;
        r.check(all_near(pr.ms, 0, 2 * P, 0.001) && nogap && pr.fps == 30,
                "30 FPS, even pacing: all %zu samples 33.37 ms, no gaps, reads %d", pr.ms.size(), pr.fps);
    }
    {   // the same average with bad pacing: 1 and 3 periods alternate, the FPS still reads 30
        PacingSim s;
        s.pattern = {1, 3};
        PacingRun pr = run_pacing(s, 600);
        bool alt = pr.ms.size() > 10;
        for (size_t i = 1; i < pr.ms.size(); i++) {
            double want = (fabs(pr.ms[i - 1] - P) < 0.01) ? 3 * P : P;
            alt = alt && fabs(pr.ms[i] - want) < 0.001;
        }
        r.check(alt && pr.fps == 30, "30 FPS, judder 16.7/50: the samples alternate exactly, FPS still %d", pr.fps);
    }
    {   // a 60 FPS game: one period each
        PacingSim s;
        s.pattern = {1};
        PacingRun pr = run_pacing(s, 600);
        r.check(all_near(pr.ms, 0, P, 0.001), "60 FPS: every sample 16.68 ms");
    }
    {   // the capture device loses a repeat (no effect on the game's frames) and a new frame
        PacingSim s;
        s.lost = {301, 400}; // 301: a repeat of the frame shown at 300; 400: a new frame
        PacingRun pr = run_pacing(s, 600);
        int gaps = 0;
        bool others_even = true;
        for (size_t i = 0; i < pr.ms.size(); i++) {
            if (pr.gap[i])
                gaps++;
            else
                others_even = others_even && fabs(pr.ms[i] - 2 * P) < 0.001;
        }
        // each loss flags the sample it falls in and the next one (its start is the late frame)
        r.check(gaps == 4 && others_even,
                "frames lost by the device: %d samples flagged (4), every other sample still 33.37 ms", gaps);
    }
    {   // sync path (Game Capture): analysis clock with +-2 ms jitter, rounded to OBS frames
        PacingSim s;
        s.sync = true;
        s.jitter_us = 2000;
        s.pattern = {2, 2, 3};
        PacingRun pr = run_pacing(s, 600);
        bool whole = pr.ms.size() > 10;
        for (double v : pr.ms) {
            double k = v / P;
            whole = whole && fabs(k - round(k)) < 1e-6 && (round(k) == 2 || round(k) == 3);
        }
        r.check(whole, "sync path with clock jitter: every sample a whole 2 or 3 periods");
    }
    {   // reset: the graphs empty, the capture-path verdicts stay
        fps_core_params p = fps_core_params_defaults();
        p.measure_deltas = true;
        fps_core *c = fps_core_create(&p);
        Frame f = make_frame(FPS_PIXFMT_NV12, 512, 4);
        fill_all(f, 5);
        fps_core_output o{};
        uint64_t rng = 7;
        for (int k = 0; k < 300; k++) { // a dithering card on a still picture
            uint64_t now = at((uint64_t)k, 60);
            Frame g = f;
            for (auto &v : g.buf) {
                rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
                if ((rng >> 60) == 0)
                    v = (uint8_t)(v + 1);
            }
            fps_frame_view view = g.view();
            fps_core_feed(c, &view, now);
            fps_core_tick(c, now, &o);
        }
        bool altered_before = fps_core_frames_altered(c);
        int count_before = o.graph_count;
        fps_core_clear_history(c);
        fps_core_tick(c, at(400, 60), &o);
        r.check(altered_before && count_before > 0 && o.graph_count == 0 && fps_core_frames_altered(c),
                "clear history: graphs empty (%d -> %d), the altered-frame verdict stays", count_before,
                o.graph_count);
        fps_core_destroy(c);
    }
}

// --- s26: graph geometry on a time axis (plot-layout) ---
static struct plot_ring g_s26_ring; // large: kept off the stack

// Frames of the given durations (ms), repeated, ending exactly at end_ms
static void s26_fill(struct plot_ring *r, const std::vector<double> &pattern, int frames, double end_ms)
{
    plot_ring_clear(r);
    std::vector<double> ft;
    for (int i = 0; i < frames; i++)
        ft.push_back(pattern[(size_t)i % pattern.size()]);
    double t = end_ms;
    std::vector<double> ts(frames);
    for (int i = frames - 1; i >= 0; i--) {
        ts[(size_t)i] = t;
        t -= ft[(size_t)i];
    }
    for (int i = 0; i < frames; i++) {
        struct plot_sample s = {};
        s.t_ms = ts[(size_t)i];
        s.ft_ms = ft[(size_t)i];
        s.pacing_ms = ft[(size_t)i];
        plot_ring_push(r, &s);
    }
}

static void s26_plot_layout(Runner &r)
{
    r.begin("s26_plot_layout", fps_core_params_defaults());
    static struct plot_span spans[FPS_PLOT_CAP];
    const double now = 1000000.0;
    const double P = 1000.0 / 59.94;
    {   // fixed pixels per second whatever the frame rate: 30 and 60 FPS, 10 s in 1000 px
        bool ok = true;
        for (double ft : {2 * P, P}) {
            s26_fill(&g_s26_ring, {ft}, 2000, now);
            int n = plot_window(&g_s26_ring, now, 10.0, 1000.0f, spans, FPS_PLOT_CAP);
            for (int i = 1; i < n; i++) // the first one may be clipped at the left edge
                ok = ok && fabs((spans[i].x1 - spans[i].x0) - ft * 0.1) < 0.01;
        }
        r.check(ok, "30 and 60 FPS: every frame is as wide as its time (0.1 px per ms)");
    }
    {   // judder: a 50 ms frame is three times as wide as a 16.7 ms one
        s26_fill(&g_s26_ring, {P, 3 * P}, 600, now);
        int n = plot_window(&g_s26_ring, now, 5.0, 900.0f, spans, FPS_PLOT_CAP);
        bool ok = n > 10;
        for (int i = 2; i + 1 < n; i += 2) {
            double a = spans[i].x1 - spans[i].x0, b = spans[i + 1].x1 - spans[i + 1].x0;
            double small = a < b ? a : b, big = a < b ? b : a;
            ok = ok && fabs(big / small - 3.0) < 0.01;
        }
        r.check(ok, "judder 16.7 / 50 ms: the long frames are exactly 3x as wide");
    }
    {   // the window: only the last 5 s, the right edge is now, older samples in the ring stay off
        s26_fill(&g_s26_ring, {2 * P}, 1200, now); // 40 s in the ring
        int n = plot_window(&g_s26_ring, now, 5.0, 500.0f, spans, FPS_PLOT_CAP);
        const struct plot_sample *first = plot_ring_at(&g_s26_ring, spans[0].idx);
        bool in = first->t_ms > now - 5000.0;
        const struct plot_sample *before = plot_ring_at(&g_s26_ring, spans[0].idx - 1);
        bool prev_out = before && before->t_ms <= now - 5000.0;
        r.check(in && prev_out && fabs(spans[n - 1].x1 - 500.0f) < 0.01 && spans[0].x0 >= 0.0f &&
                    n >= 149 && n <= 151,
                "5 s window: %d frames of 1200, right edge at now (%.2f), nothing older", n, spans[n - 1].x1);
    }
    {   // a 20 s window shows twice the time in the same width
        s26_fill(&g_s26_ring, {2 * P}, 1200, now);
        int n10 = plot_window(&g_s26_ring, now, 10.0, 1000.0f, spans, FPS_PLOT_CAP);
        float w10 = spans[n10 - 1].x1 - spans[n10 - 1].x0;
        int n20 = plot_window(&g_s26_ring, now, 20.0, 1000.0f, spans, FPS_PLOT_CAP);
        float w20 = spans[n20 - 1].x1 - spans[n20 - 1].x0;
        r.check(abs(n20 - 2 * n10) <= 2 && fabs(w10 / w20 - 2.0) < 0.01,
                "20 s window: twice the frames (%d vs %d), half as wide", n20, n10);
    }
    {   // the axis: normal 0 at the bottom, inverted (Digital Foundry) 0 at the top
        float n16 = plot_value_y(16.7, 100.0, 100.0f, false), n50 = plot_value_y(50.0, 100.0, 100.0f, false);
        float i16 = plot_value_y(16.7, 100.0, 100.0f, true), i50 = plot_value_y(50.0, 100.0, 100.0f, true);
        r.check(n16 > n50 && i16 < i50 && fabs(i16 - 16.7f) < 0.01 && fabs(n16 - 83.3f) < 0.01,
                "axis: normal puts 50 ms above 16.7, inverted below (%.1f / %.1f)", i16, i50);
        r.check(plot_value_y(500.0, 100.0, 100.0f, true) == 100.0f && plot_value_y(-5.0, 100.0, 100.0f, false) == 100.0f,
                "values beyond the scale are clipped at the edge");
    }
    {   // the frame on screen now: frames keep coming (judder included) -> no tail
        s26_fill(&g_s26_ring, {P, 3 * P}, 600, now);
        float a = 0, b = 0;
        double el = 0;
        bool none = !plot_tail(&g_s26_ring, now + 40.0, 10.0, 1000.0f, &a, &b, &el) &&
                    !plot_tail(&g_s26_ring, now + 99.0, 10.0, 1000.0f, &a, &b, &el);
        r.check(none, "frames still coming (a 50 ms frame, 99 ms since the last): no tail");
    }
    {   // a 3 s pause in a 10 s window: the tail runs from 70 % to the edge, showing 3000 ms
        s26_fill(&g_s26_ring, {2 * P}, 600, now);
        float a = 0, b = 0;
        double el = 0;
        bool got = plot_tail(&g_s26_ring, now + 3000.0, 10.0, 1000.0f, &a, &b, &el);
        r.check(got && fabs(a - 700.0f) < 0.01 && b == 1000.0f && fabs(el - 3000.0) < 0.001,
                "3 s pause: tail %.1f..%.1f px, current frame shown %.0f ms", a, b, el);
        // a pause longer than the window: the tail covers all of it, the line never vanishes
        got = plot_tail(&g_s26_ring, now + 15000.0, 10.0, 1000.0f, &a, &b, &el);
        r.check(got && a == 0.0f && b == 1000.0f && fabs(el - 15000.0) < 0.001,
                "15 s pause in a 10 s window: tail over the whole width");
    }
    {   // FPS "now": new frames in the last second, 0 while the picture stands still
        s26_fill(&g_s26_ring, {2 * P}, 600, now);
        int during = plot_recent_frames(&g_s26_ring, now, 1000.0);
        int paused = plot_recent_frames(&g_s26_ring, now + 1500.0, 1000.0);
        int half = plot_recent_frames(&g_s26_ring, now + 500.0, 1000.0);
        r.check(during >= 29 && during <= 31 && paused == 0 && half >= 14 && half <= 16,
                "recent frames: %d while playing at 30, %d half a second into a pause, %d after 1.5 s", during,
                half, paused);
    }
    {   // framerate in time: steady 30 and 60 FPS read 29.97 / 59.94 at every column, no 29/30 flicker
        static float xs[4096];
        static double vs[4096];
        bool ok = true;
        double worst = 0;
        for (double ft : {2 * P, P}) {
            s26_fill(&g_s26_ring, {ft}, 2400, now);
            int n = plot_fps_curve(&g_s26_ring, now, 20.0, 1000.0f, 2.0f, xs, vs, 4096);
            double want = 1000.0 / ft;
            ok = ok && n > 400;
            for (int i = 0; i < n; i++) {
                if (fabs(vs[i] - want) > fabs(worst))
                    worst = vs[i] - want;
                ok = ok && fabs(vs[i] - want) < 0.01;
            }
        }
        r.check(ok, "framerate curve: steady 30 and 60 FPS flat at 29.97 / 59.94 (worst off by %.4f)", worst);
        // judder 16.7 / 50 ms still averages 29.97 - the judder belongs to the pacing graph
        s26_fill(&g_s26_ring, {P, 3 * P}, 1200, now);
        int n = plot_fps_curve(&g_s26_ring, now, 10.0, 1000.0f, 2.0f, xs, vs, 4096);
        bool flat = n > 100;
        for (int i = 0; i < n; i++)
            flat = flat && fabs(vs[i] - 1000.0 / (2 * P)) < 0.05;
        r.check(flat, "framerate curve: judder 16.7 / 50 ms reads a steady 29.97");
    }
    {   // a 3 s pause after a 30 FPS game: falls within a second, never a slope across the pause
        s26_fill(&g_s26_ring, {2 * P}, 600, now);
        const double pause_end = now + 3000.0;
        bool falling = true;
        double prev = 1e9, peak = 0;
        for (double t = now; t <= pause_end; t += 50.0) {
            double v = plot_fps_at(&g_s26_ring, t, pause_end);
            falling = falling && v <= prev + 1e-9;
            prev = v;
            if (v > peak)
                peak = v;
        }
        double one_s = plot_fps_at(&g_s26_ring, now + 1000.0, now + 1000.0);
        double mid = plot_fps_at(&g_s26_ring, now + 1500.0, pause_end);
        double end = plot_fps_at(&g_s26_ring, pause_end, pause_end);
        r.check(falling && peak <= 1000.0 / (2 * P) + 1e-6 && one_s < 1.1 && mid <= 1.1 && fabs(end - 1000.0 / 3000.0) < 0.01,
                "pause: only falls (peak %.2f), %.2f a second in, %.2f mid-pause (a slope would give ~15), %.2f at 3 s",
                peak, one_s, mid, end);
        // the game resumes: back to 29.97 a second later
        plot_ring_clear(&g_s26_ring);
        s26_fill(&g_s26_ring, {2 * P}, 600, now);
        struct plot_sample after = {};
        after.t_ms = pause_end;
        after.ft_ms = 3000.0;
        plot_ring_push(&g_s26_ring, &after);
        for (int k = 1; k <= 60; k++) {
            struct plot_sample s = {};
            s.t_ms = pause_end + k * 2 * P;
            s.ft_ms = 2 * P;
            plot_ring_push(&g_s26_ring, &s);
        }
        double back = plot_fps_at(&g_s26_ring, pause_end + 1100.0, pause_end + 60 * 2 * P);
        r.check(fabs(back - 1000.0 / (2 * P)) < 0.05, "resumed: %.2f FPS a second after the pause", back);
        // the frametime graph is the same curve as an average frame time: back to 33.37 a
        // second after the pause (an EMA would still be hundreds of ms up), and before the
        // pause, steady at 33.37
        double ft_back = plot_frametime_at(&g_s26_ring, pause_end + 1100.0, pause_end + 60 * 2 * P);
        double ft_before = plot_frametime_at(&g_s26_ring, now, pause_end + 60 * 2 * P);
        r.check(fabs(ft_back - 2 * P) < 0.1 && fabs(ft_before - 2 * P) < 0.01,
                "frametime curve: %.2f ms before the pause, %.2f ms a second after it", ft_before, ft_back);
    }
    {   // frametime through the pause: only rises, never a slope (mid-pause it is the frame's time)
        s26_fill(&g_s26_ring, {2 * P}, 600, now);
        const double pause_end = now + 3000.0;
        bool rising = true;
        double prev = 0;
        for (double t = now; t <= pause_end; t += 50.0) {
            double v = plot_frametime_at(&g_s26_ring, t, pause_end);
            rising = rising && v >= prev - 1e-9;
            prev = v;
        }
        double mid = plot_frametime_at(&g_s26_ring, now + 1500.0, pause_end);
        r.check(rising && mid >= 900.0 && plot_fps_to_frametime(0.0) >= 1e9,
                "frametime curve in a pause: only rises, %.0f ms mid-pause (a slope would give ~500)", mid);
    }
    {   // the ring wraps and keeps the order
        plot_ring_clear(&g_s26_ring);
        for (int i = 0; i < FPS_PLOT_CAP + 1808; i++) {
            struct plot_sample s = {};
            s.t_ms = i;
            s.ft_ms = 1.0;
            plot_ring_push(&g_s26_ring, &s);
        }
        bool ordered = g_s26_ring.count == FPS_PLOT_CAP && plot_ring_at(&g_s26_ring, 0)->t_ms == 1808.0;
        for (int i = 1; i < g_s26_ring.count && ordered; i++)
            ordered = plot_ring_at(&g_s26_ring, i)->t_ms > plot_ring_at(&g_s26_ring, i - 1)->t_ms;
        r.check(ordered && plot_ring_at(&g_s26_ring, FPS_PLOT_CAP) == nullptr,
                "ring: full at %d, oldest dropped first, order kept", FPS_PLOT_CAP);
    }
}

// --- s27: dropped frames (capture device vs OBS) ---
struct DropSim {
    double seconds = 20.0;
    uint64_t period_ns = 16683333; // 59.94 Hz source and OBS
    int card_lose_every = 0;       // the device loses every Nth frame (an empty OBS tick)
    int obs_lose_every = 0;        // OBS takes the newer of two frames every Nth (no empty tick)
    int beat_every = 0;            // an empty tick without a gap (59.94 into 60) every N
    bool variable = false;         // random intervals (camera / VRR)
    bool detect = true;
    double stop_at_s = -1;         // losses stop here
    bool obs_buffered = false;     // OBS buffers a frame: the empty tick comes one frame before the gap
    bool empty_after = false;      // ... or two ticks after it
    bool lose_two = false;         // each loss is two frames in a row
};
struct DropRun {
    fps_drops at_end{};
    double card_rate_mid = 0, obs_rate_mid = 0; // at 10 s
    bool active_mid = false;
    double inactive_after_s = -1;               // first second inactive after losses stopped
};

static DropRun run_drops(const DropSim &s)
{
    DropRun dr;
    fps_core_params p = fps_core_params_defaults();
    p.detect_drops = s.detect;
    fps_core *c = fps_core_create(&p);
    Frame a = make_frame(FPS_PIXFMT_NV12, 64, 4), b = make_frame(FPS_PIXFMT_NV12, 64, 4);
    fill_all(a, 1);
    fill_all(b, 2);
    uint64_t rng = 5;
    const uint64_t n = (uint64_t)(s.seconds * 1e9 / s.period_ns);
    uint64_t ts = T0;
    int late_gap = 0; // obs_buffered: the gap to deliver on the next frame
    uint64_t empty_at = 0; // empty_after: the tick that gets no frame
    for (uint64_t k = 0; k < n; k++) {
        uint64_t now = T0 + k * s.period_ns + 2000000;
        double t = (double)(now - T0) / 1e9;
        bool losing = !(s.stop_at_s >= 0 && t >= s.stop_at_s);
        bool card_loss = losing && s.card_lose_every && k % (uint64_t)s.card_lose_every == 7 % (uint64_t)s.card_lose_every;
        bool obs_loss = losing && s.obs_lose_every && k % (uint64_t)s.obs_lose_every == 7 % (uint64_t)s.obs_lose_every;
        bool beat = s.beat_every && k % (uint64_t)s.beat_every == 3;
        uint64_t step = s.period_ns;
        if (s.variable) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            step = s.period_ns / 2 + (rng >> 40) % (2 * s.period_ns); // 0.5 .. 2.5 periods
        }
        if (s.empty_after && empty_at && k == empty_at) {
            fps_core_obs_tick(c, now, 0); // the empty tick, two ticks after the gap
            empty_at = 0;
            continue;
        }
        if (card_loss && s.empty_after) {
            ts += 2 * step; // the gap arrives first ...
            empty_at = k + 2;
            Frame &f = (k & 1) ? b : a;
            fps_frame_view v = f.view();
            v.src_ts_ns = ts;
            fps_core_feed(c, &v, now);
            fps_core_obs_tick(c, now, 1);
            continue;
        }
        if (card_loss && s.obs_buffered) {
            // as in the 17.11.2026 Mk2 traces: an empty tick, then one frame at the
            // usual interval, then the one after the lost frame
            fps_core_obs_tick(c, now, 0);
            late_gap = 1;
            continue;
        }
        if (late_gap == 1) {
            late_gap = 2;
        } else if (late_gap == 2) {
            ts += step; // the lost frame's slot
            late_gap = 0;
        }
        if (card_loss || beat) {
            // nothing delivered on this tick (a lost frame, or OBS repeating one)
            if (card_loss)
                ts += step * (s.lose_two ? 2 : 1); // the device's clock moves on: the next frame comes late
            fps_core_obs_tick(c, now, 0);
            continue;
        }
        if (obs_loss)
            ts += step; // OBS skipped the older of two frames: a gap, but this tick has a frame
        ts += step;
        Frame &f = (k & 1) ? b : a;
        fps_frame_view v = f.view();
        v.src_ts_ns = ts;
        fps_core_feed(c, &v, now);
        fps_core_obs_tick(c, now, 1);
        if (t >= 10.0 && t < 10.0 + s.period_ns / 1e9) {
            fps_drops d;
            fps_core_get_drops(c, now, &d);
            dr.card_rate_mid = d.card_per_s;
            dr.obs_rate_mid = d.obs_per_s;
            dr.active_mid = d.active;
        }
        if (s.stop_at_s >= 0 && t > s.stop_at_s && dr.inactive_after_s < 0) {
            fps_drops d;
            fps_core_get_drops(c, now, &d);
            if (!d.active)
                dr.inactive_after_s = t - s.stop_at_s;
        }
    }
    fps_core_get_drops(c, T0 + n * s.period_ns, &dr.at_end);
    fps_core_destroy(c);
    return dr;
}

static void s27_dropped_frames(Runner &r)
{
    r.begin("s27_dropped_frames", fps_core_params_defaults());
    {   // the card loses one frame in 12 (~5/s at 59.94): attributed to the card
        DropSim s;
        s.card_lose_every = 12;
        DropRun d = run_drops(s);
        r.check(d.active_mid && fabs(d.card_rate_mid - 59.94 / 12) < 0.5 && d.obs_rate_mid == 0.0,
                "card loses 1 in 12: %.2f/s by the card (want ~5.0), %.2f/s by OBS", d.card_rate_mid, d.obs_rate_mid);
    }
    {   // the same with OBS's one-frame buffer: the empty tick a frame ahead of the gap
        DropSim s;
        s.card_lose_every = 12;
        s.obs_buffered = true;
        DropRun d = run_drops(s);
        r.check(d.active_mid && fabs(d.card_rate_mid - 59.94 / 12) < 0.5 && d.obs_rate_mid == 0.0,
                "card loses 1 in 12 behind OBS's buffer: %.2f/s by the card, %.2f/s by OBS", d.card_rate_mid,
                d.obs_rate_mid);
    }
    {   // ... or with the empty tick two ticks after the gap (seen in a third trace)
        DropSim s;
        s.card_lose_every = 12;
        s.empty_after = true;
        DropRun d = run_drops(s);
        r.check(d.active_mid && fabs(d.card_rate_mid - 59.94 / 12) < 0.5 && d.obs_rate_mid == 0.0,
                "card loses 1 in 12, empty tick after the gap: %.2f/s by the card, %.2f/s by OBS", d.card_rate_mid,
                d.obs_rate_mid);
    }
    {   // two frames lost in a row count as two
        DropSim s;
        s.card_lose_every = 12;
        s.lose_two = true;
        DropRun d = run_drops(s);
        r.check(fabs(d.card_rate_mid - 2 * 59.94 / 12) < 1.0, "card loses 2 in a row, 1 in 12: %.2f/s (want ~10)",
                d.card_rate_mid);
    }
    {   // the Mk2 state seen live: 13 lost a second - still counted, not "variable"
        DropSim s;
        s.card_lose_every = 5;
        DropRun d = run_drops(s);
        r.check(d.active_mid && d.card_rate_mid > 10.0 && !d.at_end.variable_source,
                "card loses 1 in 5: %.1f/s, not taken for a variable source", d.card_rate_mid);
    }
    {   // OBS takes the newer of two frames: a gap without an empty tick
        DropSim s;
        s.obs_lose_every = 20;
        DropRun d = run_drops(s);
        r.check(d.active_mid && d.card_rate_mid == 0.0 && fabs(d.obs_rate_mid - 59.94 / 20) < 0.5,
                "OBS loses 1 in 20: %.2f/s by OBS (want ~3.0), %.2f/s by the card", d.obs_rate_mid, d.card_rate_mid);
    }
    {   // a 59.94-into-60 beat: an empty tick every ~16.7 s, no gap - no loss
        DropSim s;
        s.beat_every = 1000;
        DropRun d = run_drops(s);
        r.check(!d.at_end.active && d.at_end.card_total == 0 && d.at_end.obs_total == 0,
                "beat (empty ticks without a gap): nothing counted");
    }
    {   // a camera / VRR: random intervals - variable, nothing counted
        DropSim s;
        s.variable = true;
        DropRun d = run_drops(s);
        r.check(d.at_end.variable_source && d.at_end.card_total == 0 && d.at_end.obs_total == 0,
                "variable source: flagged, nothing counted (%llu / %llu)",
                (unsigned long long)d.at_end.card_total, (unsigned long long)d.at_end.obs_total);
    }
    {   // the stream interrupted for 3 s (filter off, source hidden): not lost frames
        fps_core_params p = fps_core_params_defaults();
        fps_core *c = fps_core_create(&p);
        Frame f = make_frame(FPS_PIXFMT_NV12, 64, 4);
        uint64_t ts = T0;
        for (int k = 0; k < 600; k++) {
            if (k == 300)
                ts += 3000000000ULL;
            ts += 16683333;
            fill_all(f, (uint64_t)k);
            fps_frame_view v = f.view();
            v.src_ts_ns = ts;
            fps_core_feed(c, &v, ts);
            fps_core_obs_tick(c, ts, 1);
        }
        fps_drops d;
        fps_core_get_drops(c, ts, &d);
        r.check(d.card_total == 0 && d.obs_total == 0, "3 s interruption: nothing counted (%llu / %llu)",
                (unsigned long long)d.card_total, (unsigned long long)d.obs_total);
        fps_core_destroy(c);
    }
    {   // switched off: nothing at all
        DropSim s;
        s.card_lose_every = 12;
        s.detect = false;
        DropRun d = run_drops(s);
        r.check(!d.active_mid && d.at_end.card_total == 0, "detect_drops off: nothing measured");
    }
    {   // the warning holds 5 s after the last loss, then goes
        DropSim s;
        s.card_lose_every = 12;
        s.stop_at_s = 8.0;
        DropRun d = run_drops(s);
        r.check(d.inactive_after_s > 4.5 && d.inactive_after_s < 5.6,
                "losses stop at 8 s: warning gone %.2f s later (want ~5)", d.inactive_after_s);
    }
}

// --- s28: the fast frame comparison gives the plain loop's result, bit for bit ---
static void s28_diff_equivalence(Runner &r)
{
    r.begin("s28_diff_equivalence", fps_core_params_defaults());
    uint64_t rng = 12345;
    auto rnd = [&rng]() {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return rng;
    };
    // lengths around the 16-byte vectors and the 255-block counters, a line, a 4K plane
    const size_t lens[] = {0, 1, 15, 16, 17, 31, 255, 256, 257, 4079, 4080, 4081, 4096, 3840, 65536, 8294400};
    std::vector<uint8_t> a, b;
    int cases = 0, bad = 0;
    char first_bad[160] = "";
    auto check = [&](const char *what, size_t n) {
        uint8_t m_fast = 0, m_ref = 0;
        size_t d_fast = fps_core_count_diff_max(a.data(), b.data(), n, &m_fast);
        size_t d_ref = fps_core_count_diff_max_scalar(a.data(), b.data(), n, &m_ref);
        size_t d_bytes = fps_core_count_diff_bytes(a.data(), b.data(), n);
        // the reference is checked against the definition too
        size_t d_def = 0;
        int m_def = 0;
        for (size_t i = 0; i < n; i++) {
            int d = abs((int)a[i] - (int)b[i]);
            d_def += d != 0;
            m_def = d > m_def ? d : m_def;
        }
        cases++;
        if (d_fast != d_ref || m_fast != m_ref || d_bytes != d_ref || d_ref != d_def || m_ref != m_def) {
            if (!bad)
                snprintf(first_bad, sizeof(first_bad), "%s n=%zu: fast %zu/%u ref %zu/%u def %zu/%d", what, n,
                         d_fast, m_fast, d_ref, m_ref, d_def, m_def);
            bad++;
        }
    };
    for (size_t n : lens) {
        a.assign(n + 1, 0);
        b.assign(n + 1, 0);
        for (int pass = 0; pass < 8; pass++) {
            // random bytes
            for (size_t i = 0; i < n; i++) {
                a[i] = (uint8_t)rnd();
                b[i] = (uint8_t)rnd();
            }
            check("random", n);
            // identical
            b = a;
            check("identical", n);
            // dither: +-1, +-2, +-3 on part of the bytes (the <= 2 boundary)
            for (int amp = 1; amp <= 3; amp++) {
                for (size_t i = 0; i < n; i++) {
                    int v = a[i] + (int)(rnd() % (2 * amp + 1)) - amp;
                    b[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
                }
                check("dither", n);
            }
            // one difference at the very end (the scalar tail) and at the very start
            b = a;
            if (n) {
                b[n - 1] = (uint8_t)(a[n - 1] ^ 0x80);
                check("last byte", n);
                b[n - 1] = a[n - 1];
                b[0] = (uint8_t)(a[0] + 1);
                check("first byte", n);
            }
            // extremes: 0 against 255 everywhere (|d| = 255, both directions)
            for (size_t i = 0; i < n; i++) {
                a[i] = (i & 1) ? 0 : 255;
                b[i] = (i & 1) ? 255 : 0;
            }
            check("0 vs 255", n);
            // every byte different by exactly 1 (the per-lane counters at their limit)
            for (size_t i = 0; i < n; i++) {
                a[i] = (uint8_t)(i * 7);
                b[i] = (uint8_t)(a[i] + 1);
            }
            check("all differ", n);
        }
    }
    r.check(bad == 0, "fast comparison == plain loop == definition in all %d cases (%d differ%s%s)", cases, bad,
            bad ? ": " : "", first_bad);

    // the whole core: the altered-frame check from the largest |d| decides exactly as
    // from the full histogram, scenario by scenario
    struct Sc {
        const char *name;
        AltSim s;
    };
    std::vector<Sc> scs;
    {
        Sc c{"good card 30", {}};
        scs.push_back(c);
    }
    {
        Sc c{"dither 30", {}};
        c.s.noise_p = 0.28;
        scs.push_back(c);
    }
    {
        Sc c{"dither still", {}};
        c.s.noise_p = 0.10;
        c.s.still = true;
        scs.push_back(c);
    }
    {
        Sc c{"changes of 3", {}};
        c.s.noise_p = 0.5;
        c.s.noise_max = 2;
        c.s.still = true;
        scs.push_back(c);
    }
    {
        Sc c{"fade 60", {}};
        c.s.game_div = 1;
        c.s.fade_frames = 180;
        scs.push_back(c);
    }
    {
        Sc c{"dither stops", {}};
        c.s.noise_p = 0.28;
        c.s.clean_after_s = 6.0;
        scs.push_back(c);
    }
    bool same = true;
    char diff_txt[160] = "";
    for (Sc &c : scs) {
        AltSim h = c.s, m = c.s;
        h.histogram = true;
        m.histogram = false;
        AltRun rh = run_altered(h, 12), rm = run_altered(m, 12);
        if (rh.first_altered_s != rm.first_altered_s || rh.cleared_s != rm.cleared_s ||
            rh.altered_end != rm.altered_end || rh.fps_end != rm.fps_end) {
            if (same)
                snprintf(diff_txt, sizeof(diff_txt), "%s: histogram %.2f/%.2f/%d vs max %.2f/%.2f/%d", c.name,
                         rh.first_altered_s, rh.cleared_s, rh.fps_end, rm.first_altered_s, rm.cleared_s, rm.fps_end);
            same = false;
        }
    }
    r.check(same, "altered-frame check: largest |d| and full histogram decide alike in %zu scenarios%s%s",
            scs.size(), same ? "" : " - ", diff_txt);
}

static void s23_capture_beat(Runner &r)
{
    r.begin("s23_capture_beat", fps_core_params_defaults());
    if (!r.active()) return;
    const double ntsc60 = 60000.0 / 1001.0;

    {   // 59.94 console through a card that outputs 60 (Elgato 4K X at "60")
        BeatSim s;
        BeatRun br = run_beat(s, 120);
        b_report(r, "card 59.94->60", br);
        r.check(br.beat.detected, "card 59.94 into 60: beat detected");
        r.check(fabs(br.beat.signal_hz - ntsc60) < 0.005, "card 59.94 into 60: signal %.4f Hz", br.beat.signal_hz);
        r.check(br.beat.nominal_hz == ntsc60, "card 59.94 into 60: snapped to 59.94 (%.4f)", br.beat.nominal_hz);
        r.check(br.beat.origin == FPS_BEAT_ORIGIN_CAPTURE, "card 59.94 into 60: origin is the capture device");
        r.check(fabs(br.beat.period_s - 1001.0 / 60.0) < 0.1, "card 59.94 into 60: period %.3f s", br.beat.period_s);
        r.check(br.first_detect_s > 0 && br.first_detect_s < 70, "card 59.94 into 60: detected after %.1f s",
                br.first_detect_s);
    }
    // 59.94 source frames into OBS at 60 (unbuffered pick). The 4K X traces
    // show bursts of ~0.4 s, i.e. about +-0.2 ms of arrival jitter; 1 ms
    // spreads each burst over ~2 s.
    for (double jit : {0.2, 1.0}) {
        BeatSim s;
        s.card = false;
        s.jitter_ms = jit;
        BeatRun br = run_beat(s, 120);
        b_report(r, "obs 59.94->60", br);
        r.check(br.beat.detected, "OBS 59.94 into 60, jitter %.1f ms: beat detected", jit);
        r.check(br.beat.nominal_hz == ntsc60, "OBS 59.94 into 60, jitter %.1f ms: snapped to 59.94 (%.4f)", jit,
                br.beat.signal_hz);
        r.check(br.beat.origin == FPS_BEAT_ORIGIN_OBS, "OBS 59.94 into 60, jitter %.1f ms: origin is OBS", jit);
        // four bursts (~50-67 s) at 0.2 ms; wide bursts fragment and take longer
        double limit = jit < 0.5 ? 70.0 : 100.0;
        r.check(br.first_detect_s > 0 && br.first_detect_s < limit,
                "OBS 59.94 into 60, jitter %.1f ms: detected after %.1f s", jit, br.first_detect_s);
        // the burst is timed at its centre, so the period stays sharp even when
        // jitter spreads the burst over a second or two
        r.check(fabs(br.beat.period_s - 1001.0 / 60.0) < 0.03, "OBS 59.94 into 60, jitter %.1f ms: period %.3f s", jit,
                br.beat.period_s);
    }
    {   // matched 60 into 60, jitter away from the tick boundary: nothing
        BeatSim s;
        s.card = false;
        s.src_hz = 60.0;
        s.jitter_ms = 1.0;
        BeatRun br = run_beat(s, 300);
        b_report(r, "obs 60->60", br);
        r.check(!br.beat.detected && br.beat.clusters == 0, "matched 60 into 60: no beat (%d clusters)",
                br.beat.clusters);
    }
    {   // card at exactly 60: nothing
        BeatSim s;
        s.src_hz = 60.0;
        BeatRun br = run_beat(s, 300);
        r.check(!br.beat.detected && br.beat.clusters == 0, "card 60 into 60: no beat");
    }
    {   // 60 Hz source 50 ppm slow (two crystals): a beat every ~333 s, nominal 60
        BeatSim s;
        s.card = false;
        s.src_hz = 60.0 * (1.0 - 50e-6);
        s.jitter_ms = 0.05;
        BeatRun br = run_beat(s, 2400); // five crossings, ~333 s apart
        b_report(r, "obs 60-50ppm->60", br);
        r.check(br.beat.detected, "clock drift 50 ppm: beat detected");
        r.check(br.beat.nominal_hz == 60.0, "clock drift 50 ppm: nominal 60 (%.4f)", br.beat.nominal_hz);
        r.check(fabs(br.beat.period_s - 1.0 / (60.0 * 50e-6)) < 5.0, "clock drift 50 ppm: period %.1f s",
                br.beat.period_s);
        // drift cannot be fixed in settings and is the easiest to fake, so it
        // needs one crossing more than a 59.94/60 beat (four are at ~1170 s)
        r.check(br.first_detect_s > 1300, "clock drift 50 ppm: not reported before a fifth crossing (%.0f s)",
                br.first_detect_s);
    }
    {   // a game that drops the odd frame (one per ~8 s) on a matched pipeline
        for (uint64_t seed = 1; seed <= 5; seed++) {
            BeatSim s;
            s.src_hz = 60.0;
            s.drop_p = 1.0 / 480.0;
            s.seed = seed;
            BeatRun br = run_beat(s, 900);
            b_report(r, "random drops", br);
            r.check(!br.beat.detected && br.first_detect_s < 0,
                    "random game drops (seed %llu): no beat (%d clusters, false alarm at %.1f s)",
                    (unsigned long long)seed, br.beat.clusters, br.first_detect_s);
        }
    }
    {   // game locked to 30 on a 60 pipeline: every other frame repeats
        BeatSim s;
        s.src_hz = 60.0;
        s.game_div = 2;
        BeatRun br = run_beat(s, 300);
        r.check(!br.beat.detected && br.beat.clusters == 0, "game at 30 in 60: no beat (%d clusters)",
                br.beat.clusters);
        r.check(br.first_clear_s > 0 && br.first_clear_s < 45 && !br.uneven_seen,
                "game at 30 in 60: OK at %.0f s, not called uneven (%d)", br.first_clear_s, (int)br.uneven_seen);
    }
    {   // 59.94 through the card plus a game that drops the odd frame
        BeatSim s;
        s.drop_p = 1.0 / 480.0;
        s.seed = 7;
        BeatRun br = run_beat(s, 300);
        b_report(r, "card 59.94 + drops", br);
        r.check(br.beat.detected && br.beat.nominal_hz == ntsc60,
                "59.94 into 60 with random game drops: beat still detected at 59.94 (%.4f)", br.beat.signal_hz);
    }
    {   // a game that hitches on a 5 s timer: perfectly regular, but 60 - 1/5 =
        // 59.8 Hz is no standard rate, so it is the game, not two clocks
        BeatSim s;
        s.src_hz = 60.0;
        s.hitch_every_s = 5.0;
        BeatRun br = run_beat(s, 300);
        b_report(r, "5 s game hitch", br);
        r.check(br.beat.clusters > 40, "5 s game hitch: the hitches do form clusters (%d)", br.beat.clusters);
        r.check(!br.beat.detected && br.first_detect_s < 0, "5 s game hitch: no beat (not a standard-rate difference)");
    }
    {   // stray empty ticks (~1 per 20 s) plus a game dropping the odd frame:
        // neither kind is regular on its own and they must not be chained together
        for (uint64_t seed = 1; seed <= 4; seed++) {
            BeatSim s;
            s.src_hz = 60.0;
            s.drop_p = 1.0 / 300.0;
            s.empty_p = 1.0 / 1200.0;
            s.seed = 100 + seed;
            BeatRun br = run_beat(s, 900);
            b_report(r, "stray empties + drops", br);
            r.check(!br.beat.detected && br.first_detect_s < 0,
                    "stray empty ticks + random drops (seed %llu): no beat (false alarm at %.1f s)",
                    (unsigned long long)seed, br.first_detect_s);
        }
    }
    {   // the game freezes for half a second every 16.68 s (a loading hitch on a
        // timer that happens to look like 59.94 vs 60): a freeze is a stall,
        // not a repeated frame, and must not count
        BeatSim s;
        s.src_hz = 60.0;
        s.freeze_every_s = 1001.0 / 60.0;
        s.freeze_len_s = 0.5;
        BeatRun br = run_beat(s, 200);
        b_report(r, "periodic freeze", br);
        r.check(!br.beat.detected && br.first_detect_s < 0, "periodic 0.5 s freeze: no beat (%d clusters)",
                br.beat.clusters);
    }
    {   // fixed after 90 s (source now matches): the verdict expires
        BeatSim s;
        s.switch_s = 90.0;
        s.switch_hz = 60.0;
        BeatRun at90 = run_beat(s, 89);
        BeatRun later = run_beat(s, 200);
        r.check(at90.beat.detected, "before the fix: beat detected");
        r.check(!later.beat.detected, "after the fix: the verdict has expired");
        r.check(later.beat.status == FPS_BEAT_CLEAR, "after the fix: the check ends CLEAR (status %d)",
                later.beat.status);
    }

    // --- The check: never "OK" before the warning, OK once it is ruled out ---
    for (int ph = 0; ph < 10; ph++) {
        for (int mode = 0; mode < 2; mode++) {
            BeatSim s;
            s.card = mode == 0;
            s.jitter_ms = s.card ? 0.0 : 0.2;
            s.phase = ph / 10.0;
            BeatRun br = run_beat(s, 120);
            r.check(!br.clear_before_detect && br.beat.status == FPS_BEAT_DETECTED,
                    "%s 59.94 into 60, phase %.1f: never CLEAR before the warning (first CLEAR %.0f s, "
                    "detected %.0f s, final status %d)",
                    s.card ? "card" : "OBS", s.phase, br.first_clear_s, br.first_detect_s, br.beat.status);
            r.check(!br.uneven_before_detect, "%s 59.94 into 60, phase %.1f: the beat is not called uneven game pacing",
                    s.card ? "card" : "OBS", s.phase);
        }
    }
    // One of the card's repeats goes unseen: the remaining ones are 2P apart
    // for a while, which must still hold the check back.
    for (int ph = 0; ph < 10; ph++) {
        BeatSim s;
        s.phase = ph / 10.0;
        s.mask_repeat = 2;
        BeatRun br = run_beat(s, 150);
        r.check(!br.clear_before_detect && br.beat.status == FPS_BEAT_DETECTED,
                "card 59.94 into 60, 2nd repeat missed, phase %.1f: never CLEAR before the warning "
                "(first CLEAR %.0f s, detected %.0f s)",
                s.phase, br.first_clear_s, br.first_detect_s);
        r.check(!br.uneven_before_detect,
                "card 59.94 into 60, 2nd repeat missed, phase %.1f: the beat is not called uneven game pacing", s.phase);
    }
    {   // matched 60 into 60: CHECKING with a running clock, CLEAR at 40 s
        BeatSim s;
        s.src_hz = 60.0;
        BeatRun br = run_beat(s, 60);
        r.check(br.status.size() > 45 && br.status[20] == FPS_BEAT_CHECKING && br.status[39] == FPS_BEAT_CHECKING,
                "matched 60: CHECKING through 39 s");
        r.check(br.observed.size() > 20 && fabs(br.observed[20] - 20.0) < 1.1, "matched 60: the clock runs (%.1f s at 20)",
                br.observed.size() > 20 ? br.observed[20] : -1.0);
        r.check(br.first_clear_s >= 40.0 && br.first_clear_s < 42.0, "matched 60: CLEAR at %.0f s",
                br.first_clear_s);
        // 2.4 periods of 1001/60 s = 40.04 s
        r.check(fabs(br.beat.verify_s - 2.4 * 1001.0 / 60.0) < 1e-9, "matched 60: verify window %.2f s",
                br.beat.verify_s);
    }
    {   // matched 30 into 30: the 1000/1001 period is 33.4 s, so the check is 80 s
        BeatSim s;
        s.obs_hz = 30.0;
        s.src_hz = 30.0;
        BeatRun br = run_beat(s, 100);
        r.check(br.first_clear_s >= 80.0 && br.first_clear_s < 82.5, "matched 30: CLEAR at %.0f s (verify %.1f)",
                br.first_clear_s, br.beat.verify_s);
    }
    {   // 29.97 console into 30: never CLEAR before the warning either
        BeatSim s;
        s.obs_hz = 30.0;
        s.src_hz = 30000.0 / 1001.0;
        BeatRun br = run_beat(s, 200);
        b_report(r, "card 29.97->30", br);
        r.check(!br.clear_before_detect && br.beat.detected, "29.97 into 30: warned, never CLEAR before (%.0f / %.0f)",
                br.first_clear_s, br.first_detect_s);
    }
    {   // a game that drops a frame now and then (~every 45 s): the check waits
        // out each one and ends CLEAR, never warning
        for (uint64_t seed = 1; seed <= 5; seed++) {
            BeatSim s;
            s.src_hz = 60.0;
            s.drop_p = 1.0 / 2700.0;
            s.seed = seed;
            BeatRun br = run_beat(s, 400);
            r.check(br.first_clear_s > 0 && br.first_clear_s < 300 && br.first_detect_s < 0,
                    "occasional game drops (seed %llu): CLEAR at %.0f s, no warning", (unsigned long long)seed,
                    br.first_clear_s);
        }
    }
    {   // a game dropping a frame every ~8 s: flagged uneven, never a warning
        for (uint64_t seed = 1; seed <= 5; seed++) {
            BeatSim s;
            s.src_hz = 60.0;
            s.drop_p = 1.0 / 480.0;
            s.seed = seed;
            BeatRun br = run_beat(s, 300);
            r.check(br.uneven_seen && br.first_detect_s < 0,
                    "frequent game drops (seed %llu): flagged uneven, no warning", (unsigned long long)seed);
        }
    }
    {   // settings changed at 50 s: the check starts over
        BeatSim s;
        s.src_hz = 60.0;
        s.reset_at_s = 50.0;
        BeatRun br = run_beat(s, 100);
        r.check(br.status.size() > 92 && br.status[45] == FPS_BEAT_CLEAR && br.status[60] == FPS_BEAT_CHECKING &&
                    br.status[92] == FPS_BEAT_CLEAR,
                "reset at 50 s: CLEAR -> CHECKING -> CLEAR");
        r.check(br.observed.size() > 52 && br.observed[52] < 3.0, "reset at 50 s: clock restarted (%.1f s at 52)",
                br.observed.size() > 52 ? br.observed[52] : -1.0);
    }
    {   // the signal drops out for 3 s at 60 s: the check starts over
        BeatSim s;
        s.card = false;
        s.src_hz = 60.0;
        s.jitter_ms = 0.2;
        s.switch_s = 60.0;
        s.switch_hz = 1.0 / 3.0; // frames 3 s apart from here on (next one at ~63.01 s)
        BeatRun br = run_beat(s, 65);
        r.check(br.status.size() > 64 && br.observed[64] < 1.5,
                "3 s signal loss: the check restarts (%.1f s observed at 64 s)",
                br.observed.size() > 64 ? br.observed[64] : -1.0);
    }
    r.end(false);
}
#endif

// --------------------------------------------------------------- main ----

int main(int argc, char **argv)
{
    Runner r;
    std::string impl_sel = "both";
#ifdef FPS_TEST_DATA_DIR
    r.data_dir = FPS_TEST_DATA_DIR;
#else
    r.data_dir = "tests";
#endif
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--update-goldens") r.update_goldens = true;
        else if (a == "--verbose" || a == "-v") r.verbose = true;
        else if (a == "--impl" && i + 1 < argc) impl_sel = argv[++i];
        else if (a == "--data-dir" && i + 1 < argc) r.data_dir = argv[++i];
        else if (a == "--only" && i + 1 < argc) r.only = argv[++i];
        else {
            fprintf(stderr, "usage: fps-selftest [--impl ref|core|both] [--update-goldens] [--data-dir DIR] [--verbose] [--only NAME]\n");
            return 2;
        }
    }
#ifdef FPS_SELFTEST_HAVE_CORE
    if (impl_sel == "core" || impl_sel == "both")
        r.impls.emplace_back(new CoreImpl());
    if (impl_sel == "ref" || impl_sel == "both")
        r.impls.emplace_back(new RefImpl());
    // goldens are written from the reference while it exists
    if (r.update_goldens && impl_sel == "both")
        std::swap(r.impls[0], r.impls[1]);
#else
    if (impl_sel == "core") {
        fprintf(stderr, "fps-core is not linked into this build\n");
        return 2;
    }
    r.impls.emplace_back(new RefImpl());
#endif
    printf("fps-selftest: impls =");
    for (auto &i : r.impls)
        printf(" %s", i->name());
    printf(", data dir = %s%s\n", r.data_dir.string().c_str(), r.update_goldens ? " (updating goldens)" : "");

    s01_60fps(r);
    s02_30in60(r);
    s03_24in60(r);
    s04_120fps(r);
    s05_45fps(r);
    s06_stutter(r);
    s07_rate_change(r);
    s08_stale(r);
    s09_sensitivity_edge(r);
    s10_method_divergence(r);
    s11_first_frame_and_size_change(r);
    s12_tearing(r);
    s13_tearing_alignment(r);
    s14_luma(r);
    s15_csv(r);
    s16_graph_linearization(r);
    s17_params_mid_run(r);
    s18_fuzz(r);
    s19_zero_ft(r);
    s20_decision_replay(r);
    s21_below_clamp(r);
    s22_above_clamp(r);
#ifdef FPS_SELFTEST_HAVE_CORE
    s23_capture_beat(r);
    s24_altered_frames(r);
    s25_frame_pacing(r);
    s26_plot_layout(r);
    s27_dropped_frames(r);
    s28_diff_equivalence(r);
#endif

    printf(r.fails ? "RESULT: %d FAILURES\n" : "RESULT: ALL OK\n", r.fails);
    return r.fails;
}
