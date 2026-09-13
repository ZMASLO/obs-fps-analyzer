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

// ------------------------------------------------------------ manifest ----

// source ; params ; expectations ; note
//   source:       trace:<file>
//   expectations: self  (the trace's own T rows must be reproduced exactly)
//                 prefix xfail: marks a known limitation
struct Case {
    std::string source, params, expect, note;
    bool xfail = false;
};

static int run_manifest(const fs::path &manifest, double tol, bool verbose)
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
        if (c.source.rfind("trace:", 0) != 0) {
            printf("%s\n  [SKIP] only trace: sources are supported so far\n", c.source.c_str());
            skipped++;
            continue;
        }
        fs::path src = base / c.source.substr(6);
        if (!fs::exists(src)) {
            printf("%s\n  [SKIP] file not present\n", c.source.c_str());
            skipped++;
            continue;
        }
        ReplayResult r = replay_trace(src, tol, verbose);
        print_result(src, r);
        if (!c.note.empty())
            printf("  note: %s\n", c.note.c_str());
        if (r.ok) {
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

int main(int argc, char **argv)
{
    fs::path trace, manifest;
    double tol = 0.0;
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--trace" && i + 1 < argc) trace = argv[++i];
        else if (a == "--manifest" && i + 1 < argc) manifest = argv[++i];
        else if (a == "--tol" && i + 1 < argc) tol = strtod(argv[++i], nullptr);
        else if (a == "--verbose" || a == "-v") verbose = true;
        else {
            fprintf(stderr, "usage: fps-cli --trace <file.csv> | --manifest <cases.txt> [--verbose] [--tol N]\n");
            return 2;
        }
    }

    if (!manifest.empty())
        return run_manifest(manifest, tol, verbose);

    if (trace.empty()) {
        fprintf(stderr, "usage: fps-cli --trace <file.csv> | --manifest <cases.txt> [--verbose] [--tol N]\n");
        return 2;
    }

    ReplayResult r = replay_trace(trace, tol, verbose);
    print_result(trace, r);
    return r.ok ? 0 : 1;
}
