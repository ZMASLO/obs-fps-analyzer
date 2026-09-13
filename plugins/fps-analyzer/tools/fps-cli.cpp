// fps-cli — replays recorded material through the FPS analysis core.
//
// Today it replays traces recorded by the plugin's "Record FPS trace" debug
// option (fpstrace v2). Every row of a trace is a decision the live plugin
// made, with the timestamp it made it at, so feeding those decisions into a
// freshly created core must reproduce the recorded frametimes, EMA values and
// published ticks exactly. That is the proof that the extracted core behaves
// like the plugin did on real capture timing, which no synthetic test can give.
//
//   fps-cli --trace <file.csv> [--verbose] [--tol N]
//   fps-cli --manifest <cases.txt> [--verbose] [--out DIR]
//
// Exit code = number of failed cases.

#include "fps-core.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// ------------------------------------------------------------- parsing ----

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == sep) {
            out.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    return out;
}

// A field that may be empty (the recorder leaves frametime columns blank when
// no sample was written).
static bool has(const std::vector<std::string> &f, size_t i)
{
    return i < f.size() && !trim(f[i]).empty();
}
static double dbl(const std::vector<std::string> &f, size_t i)
{
    return has(f, i) ? strtod(trim(f[i]).c_str(), nullptr) : 0.0;
}
static long long ll(const std::vector<std::string> &f, size_t i)
{
    return has(f, i) ? strtoll(trim(f[i]).c_str(), nullptr, 10) : 0;
}
static int ival(const std::vector<std::string> &f, size_t i)
{
    return (int)ll(f, i);
}

struct TraceHeader {
    int version = 0;
    std::string plugin, path, fmt;
    fps_core_params params = fps_core_params_defaults();
    uint32_t w = 0, h = 0;
};

// "# fpstrace v2 ; plugin=0.5.0 ; path=async ; method=0 ; sens=0.1 ; ..."
static bool parse_header(const std::string &line, TraceHeader &out, std::string &err)
{
    std::vector<std::string> parts = split(line, ';');
    if (parts.empty())
        return false;
    std::string first = trim(parts[0]);
    if (first.rfind("# fpstrace", 0) != 0)
        return false; // a different comment line; ignore
    size_t v = first.find(" v");
    if (v == std::string::npos) {
        err = "header without a version";
        return false;
    }
    out.version = atoi(first.c_str() + v + 2);
    for (size_t i = 1; i < parts.size(); i++) {
        std::string kv = trim(parts[i]);
        size_t eq = kv.find('=');
        if (eq == std::string::npos)
            continue;
        std::string k = trim(kv.substr(0, eq)), val = trim(kv.substr(eq + 1));
        if (k == "plugin") out.plugin = val;
        else if (k == "path") out.path = val;
        else if (k == "fmt") out.fmt = val;
        else if (k == "method") out.params.analyze_method = atoi(val.c_str());
        else if (k == "sens") out.params.sensitivity = strtod(val.c_str(), nullptr);
        else if (k == "tear") out.params.enable_tearing_detection = atoi(val.c_str()) != 0;
        else if (k == "tsens") out.params.tearing_sensitivity = strtod(val.c_str(), nullptr);
        else if (k == "interval") out.params.update_interval = strtod(val.c_str(), nullptr);
        else if (k == "w") out.w = (uint32_t)atoi(val.c_str());
        else if (k == "h") out.h = (uint32_t)atoi(val.c_str());
    }
    return true;
}

// -------------------------------------------------------------- replay ----

struct Mismatch {
    int line;
    std::string what;
    std::string expected, got;
};

struct ReplayResult {
    bool ok = false;
    std::string error;
    int frames = 0, uniques = 0, ticks = 0;
    double span_s = 0.0;
    int fps_min = 0, fps_max = 0;
    double fps_sum = 0.0;
    TraceHeader header;
    std::vector<Mismatch> mismatches;
};

static bool same(double a, double b, double tol)
{
    if (tol <= 0.0)
        return a == b;
    return fabs(a - b) <= tol;
}

static std::string num(double v)
{
    char b[64];
    snprintf(b, sizeof(b), "%.17g", v);
    return b;
}

static ReplayResult replay_trace(const fs::path &path, double tol, bool verbose)
{
    ReplayResult r;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        r.error = "cannot open " + path.string();
        return r;
    }

    struct fps_core *core = nullptr;
    bool have_header = false;
    uint64_t first_ns = 0, last_ns = 0;
    std::string line;
    int lineno = 0;
    const int max_report = 10;

    while (std::getline(in, line)) {
        lineno++;
        std::string t = trim(line);
        if (t.empty())
            continue;

        if (t[0] == '#') {
            TraceHeader h = r.header;
            std::string err;
            if (parse_header(t, h, err)) {
                if (!err.empty()) {
                    r.error = err;
                    break;
                }
                r.header = h;
                if (!have_header) {
                    if (h.version < 2) {
                        r.error = "fpstrace v" + std::to_string(h.version) +
                                  " starts from a warm analysis state and cannot be replayed exactly; "
                                  "re-record with a current plugin build";
                        break;
                    }
                    core = fps_core_create(&h.params);
                    have_header = true;
                } else {
                    // parameters changed mid-session
                    fps_core_set_params(core, &h.params);
                }
            }
            continue;
        }

        if (!have_header) {
            r.error = "data before the fpstrace header";
            break;
        }

        std::vector<std::string> f = split(t, ',');
        if (f.empty())
            continue;

        if (f[0] == "F") {
            // F,now_ns,luma_size,diff,percent,unique,tear,ft_ms,ema,fps_pf,pos,count
            if (f.size() < 12) {
                r.error = "short F row at line " + std::to_string(lineno);
                break;
            }
            uint64_t now = (uint64_t)ll(f, 1);
            bool unique = ival(f, 5) != 0;
            bool tear = ival(f, 6) != 0;
            bool want_sample = has(f, 7);
            double want_ft = dbl(f, 7), want_ema = dbl(f, 8), want_pf = dbl(f, 9);
            int want_pos = ival(f, 10), want_count = ival(f, 11);

            fps_core_debug_feed_decision(core, unique, tear, now);
            struct fps_core_frame_debug d;
            fps_core_debug_last_frame(core, &d);

            r.frames++;
            if (unique)
                r.uniques++;
            if (!first_ns)
                first_ns = now;
            last_ns = now;

            auto bad = [&](const char *what, const std::string &e, const std::string &g) {
                if ((int)r.mismatches.size() < max_report)
                    r.mismatches.push_back({lineno, what, e, g});
                else if ((int)r.mismatches.size() == max_report)
                    r.mismatches.push_back({lineno, "...", "", "(further mismatches not listed)"});
            };
            if (d.sample_written != want_sample)
                bad("frame sample written", want_sample ? "yes" : "no", d.sample_written ? "yes" : "no");
            else if (want_sample) {
                if (!same(d.ft_ms, want_ft, tol))
                    bad("frametime", num(want_ft), num(d.ft_ms));
                if (!same(d.ema, want_ema, tol))
                    bad("EMA", num(want_ema), num(d.ema));
                if (!same(d.fps_pf, want_pf, tol))
                    bad("per-frame FPS", num(want_pf), num(d.fps_pf));
            }
            if (d.pos != want_pos)
                bad("history position", std::to_string(want_pos), std::to_string(d.pos));
            if (d.count != want_count)
                bad("sample count", std::to_string(want_count), std::to_string(d.count));
        } else if (f[0] == "T") {
            // T,now_ns,window,avg_ft,fps,frametime_ms,tearing,graph_count
            if (f.size() < 8) {
                r.error = "short T row at line " + std::to_string(lineno);
                break;
            }
            uint64_t now = (uint64_t)ll(f, 1);
            int want_window = ival(f, 2);
            double want_avg = dbl(f, 3);
            int want_fps = ival(f, 4);
            double want_ft = dbl(f, 5);
            bool want_tear = ival(f, 6) != 0;
            int want_graph = ival(f, 7);

            struct fps_core_output out;
            bool published = fps_core_tick(core, now, &out);
            r.ticks++;
            last_ns = now;

            auto bad = [&](const char *what, const std::string &e, const std::string &g) {
                if ((int)r.mismatches.size() < max_report)
                    r.mismatches.push_back({lineno, what, e, g});
                else if ((int)r.mismatches.size() == max_report)
                    r.mismatches.push_back({lineno, "...", "", "(further mismatches not listed)"});
            };
            if (!published) {
                bad("tick published", "yes", "no (the update interval gate stayed shut)");
                continue;
            }
            if (out.window != want_window)
                bad("averaging window", std::to_string(want_window), std::to_string(out.window));
            if (!same(out.frametime_ms, want_avg, tol))
                bad("average frametime", num(want_avg), num(out.frametime_ms));
            if (!same(out.frametime_ms, want_ft, tol))
                bad("published frametime", num(want_ft), num(out.frametime_ms));
            if (out.fps != want_fps)
                bad("published FPS", std::to_string(want_fps), std::to_string(out.fps));
            if (out.tearing_detected != want_tear)
                bad("tearing flag", want_tear ? "1" : "0", out.tearing_detected ? "1" : "0");
            if (out.graph_count != want_graph)
                bad("graph sample count", std::to_string(want_graph), std::to_string(out.graph_count));

            if (r.ticks == 1 || out.fps < r.fps_min)
                r.fps_min = out.fps;
            if (out.fps > r.fps_max)
                r.fps_max = out.fps;
            r.fps_sum += out.fps;
            if (verbose)
                printf("    T line %d: fps %d, frametime %.3f ms, window %d, graph %d\n", lineno, out.fps,
                       out.frametime_ms, out.window, out.graph_count);
        }
    }

    if (core)
        fps_core_destroy(core);
    if (r.error.empty() && !have_header)
        r.error = "no fpstrace header found";
    r.span_s = last_ns > first_ns ? (double)(last_ns - first_ns) / 1e9 : 0.0;
    r.ok = r.error.empty() && r.mismatches.empty();
    return r;
}

static void print_result(const fs::path &path, const ReplayResult &r)
{
    printf("%s\n", path.filename().string().c_str());
    if (!r.error.empty()) {
        printf("  [FAIL] %s\n", r.error.c_str());
        return;
    }
    printf("  plugin %s, %s path, %ux%u %s, method %d, interval %.4f s\n", r.header.plugin.c_str(),
           r.header.path.c_str(), r.header.w, r.header.h, r.header.fmt.c_str(), r.header.params.analyze_method,
           r.header.params.update_interval);
    printf("  %d frames (%d unique, %d duplicate), %d published ticks, %.2f s\n", r.frames, r.uniques,
           r.frames - r.uniques, r.ticks, r.span_s);
    if (r.span_s > 0.0)
        printf("  unique frames/s %.2f, published FPS min %d max %d avg %.1f\n", r.uniques / r.span_s, r.fps_min,
               r.fps_max, r.ticks ? r.fps_sum / r.ticks : 0.0);
    if (r.mismatches.empty()) {
        printf("  [PASS] the core reproduced every recorded frame and tick exactly\n");
        return;
    }
    printf("  [FAIL] %zu mismatches\n", r.mismatches.size());
    for (const Mismatch &m : r.mismatches) {
        if (m.what == "...")
            printf("    %s\n", m.got.c_str());
        else
            printf("    line %d: %s - recorded %s, replayed %s\n", m.line, m.what.c_str(), m.expected.c_str(),
                   m.got.c_str());
    }
}

// ------------------------------------------------------------ y4m clip ----

// A YUV4MPEG2 reader for the mono (gray) clips generated by tests/gen-fixtures.ps1.
// Only plane 0 is ever analysed, so a mono clip is all the analysis needs.
struct Y4mReader {
    std::ifstream in;
    uint32_t w = 0, h = 0;
    int rate_num = 0, rate_den = 1;
    std::string colour = "420jpeg";
    std::vector<uint8_t> frame;
    size_t frame_bytes = 0;
    long index = -1; // index of the frame currently in `frame`
    std::string error;

    bool open(const fs::path &path)
    {
        in.open(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + path.string();
            return false;
        }
        std::string header;
        if (!std::getline(in, header)) {
            error = "empty file";
            return false;
        }
        if (header.rfind("YUV4MPEG2", 0) != 0) {
            error = "not a YUV4MPEG2 file";
            return false;
        }
        for (const std::string &tag : split(header, ' ')) {
            if (tag.size() < 2)
                continue;
            switch (tag[0]) {
            case 'W': w = (uint32_t)atoi(tag.c_str() + 1); break;
            case 'H': h = (uint32_t)atoi(tag.c_str() + 1); break;
            case 'F': {
                std::vector<std::string> fr = split(tag.substr(1), ':');
                if (fr.size() == 2) {
                    rate_num = atoi(fr[0].c_str());
                    rate_den = atoi(fr[1].c_str());
                }
                break;
            }
            case 'C': colour = tag.substr(1); break;
            default: break;
            }
        }
        if (!w || !h || rate_num <= 0 || rate_den <= 0) {
            error = "header without a usable size or frame rate";
            return false;
        }
        if (colour.rfind("mono", 0) != 0) {
            error = "expected a mono (gray) clip, got C" + colour +
                    " - regenerate with -pix_fmt gray";
            return false;
        }
        frame_bytes = (size_t)w * h;
        frame.resize(frame_bytes);
        return true;
    }

    // Presentation time of frame i, in nanoseconds from the clip start.
    uint64_t pts_ns(long i) const
    {
        return (uint64_t)((double)i * 1e9 * rate_den / rate_num);
    }

    // Reads the next frame into `frame`. Returns false at end of stream.
    bool next()
    {
        std::string marker;
        if (!std::getline(in, marker))
            return false;
        if (marker.rfind("FRAME", 0) != 0) {
            error = "expected a FRAME marker";
            return false;
        }
        in.read((char *)frame.data(), (std::streamsize)frame_bytes);
        if ((size_t)in.gcount() != frame_bytes)
            return false;
        index++;
        return true;
    }
};

// ---------------------------------------------------------- expectations ----

struct Expect {
    std::string kind; // fps | ft | tear
    double from_s = 0.0, to_s = 0.0;
    double value = 0.0, tol = 0.0;
};

// The overlay graph arrays are too big to print per tick; hashing the entries
// in use puts them into the golden, so a change in what the graph would draw
// shows up as a failing case.
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

struct Publish {
    double t_s;
    int fps;
    double ft;
    bool tearing;
    int window, graph_count;
};

// "fps@0.5-5=60+-0", "ft@1-2=16.67+-0.1", "tear@1-3=1"
static bool parse_expect(const std::string &spec, Expect &e, std::string &err)
{
    size_t at = spec.find('@');
    size_t eq = spec.find('=');
    if (at == std::string::npos || eq == std::string::npos || eq < at) {
        err = "cannot parse expectation '" + spec + "'";
        return false;
    }
    e.kind = trim(spec.substr(0, at));
    std::string range = spec.substr(at + 1, eq - at - 1);
    size_t dash = range.find('-', 1);
    if (dash == std::string::npos) {
        err = "expectation '" + spec + "' has no time range";
        return false;
    }
    e.from_s = strtod(range.substr(0, dash).c_str(), nullptr);
    e.to_s = strtod(range.substr(dash + 1).c_str(), nullptr);
    std::string val = spec.substr(eq + 1);
    size_t pm = val.find("+-");
    if (pm == std::string::npos) {
        e.value = strtod(val.c_str(), nullptr);
        e.tol = 0.0;
    } else {
        e.value = strtod(val.substr(0, pm).c_str(), nullptr);
        e.tol = strtod(val.substr(pm + 2).c_str(), nullptr);
    }
    if (e.kind != "fps" && e.kind != "ft" && e.kind != "tear") {
        err = "unknown expectation kind '" + e.kind + "'";
        return false;
    }
    return true;
}

// Applies one expectation to the publishes of a run.
static bool check_expect(const Expect &e, const std::vector<Publish> &pubs, std::string &detail)
{
    int checked = 0, bad = 0;
    double worst = 0.0;
    double worst_t = 0.0;
    for (const Publish &p : pubs) {
        if (p.t_s < e.from_s || p.t_s >= e.to_s)
            continue;
        checked++;
        double got = e.kind == "fps" ? p.fps : (e.kind == "ft" ? p.ft : (p.tearing ? 1.0 : 0.0));
        if (fabs(got - e.value) > e.tol + 1e-9) {
            bad++;
            if (fabs(got - e.value) > fabs(worst - e.value) || bad == 1) {
                worst = got;
                worst_t = p.t_s;
            }
        }
    }
    char buf[256];
    if (!checked) {
        snprintf(buf, sizeof(buf), "no published ticks in %.3f-%.3f s", e.from_s, e.to_s);
        detail = buf;
        return false;
    }
    if (bad) {
        snprintf(buf, sizeof(buf), "%d of %d ticks off, worst %g at %.3f s (expected %g +- %g)", bad, checked, worst,
                 worst_t, e.value, e.tol);
        detail = buf;
        return false;
    }
    snprintf(buf, sizeof(buf), "%d ticks in %.3f-%.3f s", checked, e.from_s, e.to_s);
    detail = buf;
    return true;
}

// --------------------------------------------------------- clip replay ----

struct ClipResult {
    bool ok = false;
    std::string error;
    uint32_t w = 0, h = 0;
    double clip_fps = 0.0;
    long frames = 0;
    int uniques = 0;
    std::vector<Publish> pubs;
    std::string log; // trace-shaped rows, for golden comparison
};

// Models the async source path: OBS ticks at `tick_hz`, and on each tick the
// analysis sees whichever clip frame is current at that moment. A clip slower
// than the tick rate therefore gets the same frame fed repeatedly, which is how
// libobs re-feeds cur_async_frame and how duplicate frames arise in practice.
static ClipResult replay_clip(const fs::path &path, const fps_core_params &params, double tick_hz, double max_s)
{
    ClipResult r;
    Y4mReader y;
    if (!y.open(path)) {
        r.error = y.error;
        return r;
    }
    r.w = y.w;
    r.h = y.h;
    r.clip_fps = (double)y.rate_num / y.rate_den;

    if (!y.next()) {
        r.error = "clip has no frames";
        return r;
    }

    struct fps_core *core = fps_core_create(&params);
    // The same offset the plugin sees: a large monotonic clock, so the very
    // first tick publishes (last_write_time starts at zero).
    const uint64_t T0 = 1000000000000ULL;
    char row[256];

    bool eof = false;
    for (long k = 0;; k++) {
        uint64_t rel = (uint64_t)((double)k * 1e9 / tick_hz);
        if (max_s > 0.0 && (double)rel / 1e9 > max_s)
            break;
        // Advance to the frame that is current at this tick.
        while (!eof && y.pts_ns(y.index + 1) <= rel) {
            if (!y.next())
                eof = true;
        }
        // Past the end of the clip: the last frame would just repeat forever.
        if (eof && rel > y.pts_ns(y.index))
            break;

        uint64_t now = T0 + rel;
        struct fps_frame_view view;
        view.format = FPS_PIXFMT_I420; // plane 0 of a mono clip is the luma plane
        view.width = y.w;
        view.height = y.h;
        view.data = y.frame.data();
        view.linesize = y.w;
        fps_core_feed(core, &view, now);
        r.frames++;

        struct fps_core_frame_debug d;
        fps_core_debug_last_frame(core, &d);
        if (d.unique)
            r.uniques++;
        int n = snprintf(row, sizeof(row), "F,%llu,%zu,", (unsigned long long)now, d.luma_size);
        if (d.compared)
            n += snprintf(row + n, sizeof(row) - n, "%zu,%.17g,", d.diff, d.percent);
        else
            n += snprintf(row + n, sizeof(row) - n, ",,");
        n += snprintf(row + n, sizeof(row) - n, "%d,%d,", d.unique ? 1 : 0, d.tearing_flag ? 1 : 0);
        if (d.sample_written)
            n += snprintf(row + n, sizeof(row) - n, "%.17g,%.17g,%.17g,", d.ft_ms, d.ema, d.fps_pf);
        else
            n += snprintf(row + n, sizeof(row) - n, ",,,");
        snprintf(row + n, sizeof(row) - n, "%d,%d\n", d.pos, d.count);
        r.log += row;

        struct fps_core_output out;
        if (fps_core_tick(core, now, &out)) {
            r.pubs.push_back({(double)rel / 1e9, out.fps, out.frametime_ms, out.tearing_detected, out.window,
                              out.graph_count});
            snprintf(row, sizeof(row), "T,%llu,%d,%.17g,%d,%.17g,%d,%d,%08x\n", (unsigned long long)now, out.window,
                     out.frametime_ms, out.fps, out.frametime_ms, out.tearing_detected ? 1 : 0, out.graph_count,
                     graph_hash(out));
            r.log += row;
        }
    }

    fps_core_destroy(core);
    r.ok = r.error.empty();
    return r;
}

// ------------------------------------------------------------ manifest ----

// source ; params ; expectations ; note
//   source:       trace:<file>
//   expectations: self  (the trace's own T rows must be reproduced exactly)
//                 prefix xfail: marks a known limitation
struct Case {
    std::string source, params, expect, note;
    bool xfail = false;
};

// "method=diff sens=0.1 tear=1 tsens=1.0 interval=1/30 tick_hz=60"
static bool parse_params(const std::string &spec, fps_core_params &p, double &tick_hz, std::string &err)
{
    for (const std::string &tok : split(spec, ' ')) {
        std::string kv = trim(tok);
        if (kv.empty())
            continue;
        size_t eq = kv.find('=');
        if (eq == std::string::npos) {
            err = "cannot parse parameter '" + kv + "'";
            return false;
        }
        std::string k = trim(kv.substr(0, eq)), v = trim(kv.substr(eq + 1));
        // interval accepts a fraction, so "1/30" reads like the UI shows it
        double num = 0.0;
        size_t slash = v.find('/');
        if (slash != std::string::npos)
            num = strtod(v.substr(0, slash).c_str(), nullptr) / strtod(v.substr(slash + 1).c_str(), nullptr);
        else
            num = strtod(v.c_str(), nullptr);

        if (k == "method") {
            if (v == "last_line") p.analyze_method = FPS_ANALYZE_LAST_LINE;
            else if (v == "diff") p.analyze_method = FPS_ANALYZE_DIFF;
            else { err = "unknown method '" + v + "'"; return false; }
        } else if (k == "sens") p.sensitivity = num;
        else if (k == "tear") p.enable_tearing_detection = num != 0.0;
        else if (k == "tsens") p.tearing_sensitivity = num;
        else if (k == "interval") p.update_interval = num;
        else if (k == "tick_hz") tick_hz = num;
        else { err = "unknown parameter '" + k + "'"; return false; }
    }
    return true;
}

// Compares a run's rows with a stored golden, or writes it.
static bool check_golden(const fs::path &path, const std::string &log, bool update, std::string &detail)
{
    if (update) {
        fs::create_directories(path.parent_path());
        std::ofstream o(path, std::ios::binary);
        o << log;
        detail = "written";
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        detail = "golden missing: " + path.string() + " (run with --update-goldens)";
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string want = ss.str();
    if (want == log) {
        detail = "matches " + path.filename().string();
        return true;
    }
    int line = 1;
    size_t n = want.size() < log.size() ? want.size() : log.size();
    for (size_t i = 0; i < n && want[i] == log[i]; i++)
        if (want[i] == '\n')
            line++;
    detail = "differs from " + path.filename().string() + " at line " + std::to_string(line);
    return false;
}

// One y4m clip case: replay it, then apply every expectation in the case line.
static bool run_clip_case(const fs::path &src, const Case &c, const fs::path &base, double tol, bool verbose,
                          bool update_goldens, const fs::path &out_dir)
{
    printf("%s\n", src.filename().string().c_str());
    fps_core_params params = fps_core_params_defaults();
    double tick_hz = 60.0;
    std::string err;
    if (!parse_params(c.params, params, tick_hz, err)) {
        printf("  [FAIL] %s\n", err.c_str());
        return false;
    }

    ClipResult r = replay_clip(src, params, tick_hz, 0.0);
    if (!r.ok) {
        printf("  [FAIL] %s\n", r.error.c_str());
        return false;
    }
    printf("  %ux%u clip at %.3f fps, ticks at %.0f Hz, method %d, interval %.4f s\n", r.w, r.h, r.clip_fps, tick_hz,
           params.analyze_method, params.update_interval);
    printf("  %ld feeds (%d unique, %ld duplicate), %zu published ticks\n", r.frames, r.uniques,
           r.frames - r.uniques, r.pubs.size());
    if (verbose)
        for (const Publish &p : r.pubs)
            printf("    t=%.3f s: fps %d, frametime %.3f ms, window %d, graph %d\n", p.t_s, p.fps, p.ft, p.window,
                   p.graph_count);

    if (!out_dir.empty()) {
        fs::create_directories(out_dir);
        std::ofstream o(out_dir / (src.stem().string() + ".csv"), std::ios::binary);
        o << r.log;
    }

    bool all_ok = true;
    for (const std::string &raw : split(c.expect, ',')) {
        std::string spec = trim(raw);
        if (spec.empty())
            continue;
        if (spec.rfind("golden=", 0) == 0) {
            std::string detail;
            bool ok = check_golden(base / "golden" / trim(spec.substr(7)), r.log, update_goldens, detail);
            printf("  %s golden: %s\n", ok ? "[ok]" : "[FAIL]", detail.c_str());
            all_ok = all_ok && ok;
            continue;
        }
        if (spec == "self") {
            printf("  [FAIL] 'self' only applies to trace: sources\n");
            all_ok = false;
            continue;
        }
        Expect e;
        if (!parse_expect(spec, e, err)) {
            printf("  [FAIL] %s\n", err.c_str());
            all_ok = false;
            continue;
        }
        std::string detail;
        bool ok = check_expect(e, r.pubs, detail);
        printf("  %s %s: %s\n", ok ? "[ok]" : "[FAIL]", spec.c_str(), detail.c_str());
        all_ok = all_ok && ok;
    }
    (void)tol;
    if (all_ok)
        printf("  [PASS]\n");
    return all_ok;
}

static int run_manifest(const fs::path &manifest, double tol, bool verbose, bool update_goldens, const fs::path &out_dir)
{
    std::ifstream in(manifest, std::ios::binary);
    if (!in) {
        fprintf(stderr, "cannot open manifest %s\n", manifest.string().c_str());
        return 1;
    }
    fs::path base = manifest.parent_path();
    std::string line;
    int failures = 0, pass = 0, xfail = 0, xpass = 0, skipped = 0;

    while (std::getline(in, line)) {
        std::string t = trim(line);
        if (t.empty() || t[0] == '#')
            continue;
        std::vector<std::string> f = split(t, ';');
        Case c;
        c.source = trim(f.size() > 0 ? f[0] : "");
        c.params = trim(f.size() > 1 ? f[1] : "");
        c.expect = trim(f.size() > 2 ? f[2] : "");
        c.note = trim(f.size() > 3 ? f[3] : "");
        if (c.expect.rfind("xfail:", 0) == 0) {
            c.xfail = true;
            c.expect = trim(c.expect.substr(6));
        }
        const bool is_trace = c.source.rfind("trace:", 0) == 0;
        fs::path src = base / (is_trace ? c.source.substr(6) : c.source);
        if (!fs::exists(src)) {
            printf("%s\n  [SKIP] file not present\n", c.source.c_str());
            skipped++;
            continue;
        }

        bool ok;
        if (is_trace) {
            ReplayResult r = replay_trace(src, tol, verbose);
            print_result(src, r);
            ok = r.ok;
        } else {
            ok = run_clip_case(src, c, base, tol, verbose, update_goldens, out_dir);
        }
        if (!c.note.empty())
            printf("  note: %s\n", c.note.c_str());
        if (ok) {
            if (c.xfail) {
                printf("  [XPASS] expected to fail but passed - drop the xfail marker\n");
                xpass++;
                failures++;
            } else {
                pass++;
            }
        } else if (c.xfail) {
            printf("  [XFAIL] known limitation\n");
            xfail++;
        } else {
            failures++;
        }
    }
    printf("\n%d passed, %d failed, %d expected failures, %d unexpected passes, %d skipped\n", pass, failures - xpass,
           xfail, xpass, skipped);
    return failures;
}

// ---------------------------------------------------------------- main ----

static void usage(void)
{
    fprintf(stderr,
            "usage:\n"
            "  fps-cli --trace <file.csv>            replay a recorded plugin trace\n"
            "  fps-cli --y4m <clip.y4m> [params]     replay a reference clip\n"
            "  fps-cli --manifest <cases.txt>        run every case in a manifest\n"
            "options: --verbose --tol N --out DIR --update-goldens\n"
            "         --params \"method=diff sens=0.1 interval=1/30 tick_hz=60\"\n");
}

int main(int argc, char **argv)
{
    fs::path trace, manifest, clip, out_dir;
    std::string params_spec;
    double tol = 0.0;
    bool verbose = false, update_goldens = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace" && i + 1 < argc) trace = argv[++i];
        else if (a == "--y4m" && i + 1 < argc) clip = argv[++i];
        else if (a == "--manifest" && i + 1 < argc) manifest = argv[++i];
        else if (a == "--params" && i + 1 < argc) params_spec = argv[++i];
        else if (a == "--out" && i + 1 < argc) out_dir = argv[++i];
        else if (a == "--tol" && i + 1 < argc) tol = strtod(argv[++i], nullptr);
        else if (a == "--update-goldens") update_goldens = true;
        else if (a == "--verbose" || a == "-v") verbose = true;
        else {
            usage();
            return 2;
        }
    }

    if (!manifest.empty())
        return run_manifest(manifest, tol, verbose, update_goldens, out_dir);

    if (!clip.empty()) {
        Case c;
        c.params = params_spec;
        c.expect = "";
        return run_clip_case(clip, c, clip.parent_path(), tol, true, update_goldens, out_dir) ? 0 : 1;
    }

    if (trace.empty()) {
        usage();
        return 2;
    }

    ReplayResult r = replay_trace(trace, tol, verbose);
    print_result(trace, r);
    return r.ok ? 0 : 1;
}
