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

    printf(r.fails ? "RESULT: %d FAILURES\n" : "RESULT: ALL OK\n", r.fails);
    return r.fails;
}
