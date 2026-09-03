// resdet-cli — replays dumped frame sequences (from the filter's "Debug
// options" frame dump) through the resolution detector and compares the
// result with the expected source resolution. No OBS dependency.
//
//   resdet-cli <dumpdir> [--expect WxH|native] [--tol PX] [--verbose] [--out DIR]
//   resdet-cli --manifest cases.txt [--verbose] [--out DIR]
//
// Manifest lines:  dir ; expected (WxH | native) ; tolerance px ; note
// ('#' starts a comment; dirs are relative to the manifest's folder).
// On failure (or with --verbose) prints the per-frame sign/knee picks, the
// top candidates per axis and writes the spectrum thumbnail as BMP with the
// detected (green) and expected (yellow) positions marked.

#include "resolution-detector.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

struct frame_img {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
};

static bool load_pgm(const fs::path &path, frame_img &img)
{
    FILE *f = fopen(path.string().c_str(), "rb");
    if (!f)
        return false;
    char magic[3] = {};
    int maxval = 0;
    if (fscanf(f, "%2s %d %d %d", magic, &img.w, &img.h, &maxval) != 4 || strcmp(magic, "P5") != 0 ||
        img.w <= 0 || img.h <= 0 || maxval != 255) {
        fclose(f);
        return false;
    }
    fgetc(f); // single whitespace after maxval
    img.px.resize((size_t)img.w * img.h);
    size_t got = fread(img.px.data(), 1, img.px.size(), f);
    fclose(f);
    return got == img.px.size();
}

// Plugin-reported result per frame index from frames.csv (src_w, src_h)
static std::vector<std::pair<int, int>> load_plugin_csv(const fs::path &dir)
{
    std::vector<std::pair<int, int>> out;
    std::ifstream in(dir / "frames.csv");
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) {
            header = false;
            continue;
        }
        // index,t_ms,width,height,video_format,res_valid,src_w,src_h,conf_w,conf_h
        std::vector<std::string> cols;
        std::stringstream ss(line);
        std::string c;
        while (std::getline(ss, c, ','))
            cols.push_back(c);
        if (cols.size() >= 8)
            out.emplace_back(atoi(cols[6].c_str()), atoi(cols[7].c_str()));
    }
    return out;
}

static void write_bmp(const fs::path &path, const uint8_t *spec, int frame_w, int frame_h,
                      int det_w, int det_h, int exp_w, int exp_h)
{
    const int W = RESDET_SPEC_W, H = RESDET_SPEC_H;
    auto xpos = [&](int v) { return (v > 0 && frame_w > 0) ? v * W / frame_w : -1; };
    auto ypos = [&](int v) { return (v > 0 && frame_h > 0) ? v * H / frame_h : -1; };
    int dx = xpos(det_w), dy = ypos(det_h), ex = xpos(exp_w), ey = ypos(exp_h);
    std::vector<uint8_t> px((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = &px[((size_t)(H - 1 - y) * W + x) * 3];
            resdet_spectrum_color(spec[(size_t)y * W + x], &p[0], &p[1], &p[2]);
            if (x == ex || y == ey) { p[0] = 0; p[1] = 255; p[2] = 255; } // yellow = expected
            if (x == dx || y == dy) { p[0] = 0; p[1] = 255; p[2] = 0; }   // green = detected
        }
    uint32_t imgsize = (uint32_t)px.size(), filesize = 54 + imgsize;
    uint8_t hdr[54] = {0x42, 0x4D};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (uint8_t)(v >> (8 * i)); };
    auto put16 = [&](int off, uint16_t v) { hdr[off] = (uint8_t)v; hdr[off + 1] = (uint8_t)(v >> 8); };
    put32(2, filesize); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
    put16(26, 1); put16(28, 24); put32(34, imgsize);
    FILE *f = fopen(path.string().c_str(), "wb");
    if (!f)
        return;
    fwrite(hdr, 1, 54, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

struct case_spec {
    fs::path dir;
    int exp_w = 0, exp_h = 0; // 0x0 = native expected
    int tol = 16;
    bool xfail = false;       // known limitation: a failure is expected and not counted as a regression
    std::string note;
};

// Parameter overrides from the command line (NaN/-1 = keep default)
struct param_overrides {
    float alpha = -1.0f, sign_thr = -1.0f, knee_thr = -1.0f;
    float cand_min = -1.0f, joint_min = -1.0f, flank_min = -1.0f;
    int warmup = -1, min_votes = -1, weighted = -1;
    bool dump_votes = false;
};

static void apply_params(resolution_detector *rd, const param_overrides &o)
{
    resdet_params p;
    resdet_debug_get_params(rd, &p);
    if (o.alpha >= 0.0f) p.accum_alpha = o.alpha;
    if (o.sign_thr >= 0.0f) p.sign_threshold = o.sign_thr;
    if (o.knee_thr >= 0.0f) p.knee_threshold = o.knee_thr;
    if (o.cand_min >= 0.0f) p.sign_cand_min = o.cand_min;
    if (o.joint_min >= 0.0f) p.sign_joint_min = o.joint_min;
    if (o.flank_min >= 0.0f) p.sign_flank_min = o.flank_min;
    if (o.warmup >= 0) p.warmup_frames = o.warmup;
    if (o.min_votes >= 0) p.min_votes = o.min_votes;
    if (o.weighted >= 0) p.sign_weighted = o.weighted;
    resdet_debug_set_params(rd, &p);
}

// Full per-position sign votes and knee scores of one axis -> CSV
static void dump_axis_csv(resolution_detector *rd, int axis, const fs::path &path)
{
    std::vector<float> votes(4096), profile(4096);
    int range = 0;
    size_t length = resdet_debug_axis(rd, axis, votes.data(), votes.size(), profile.data(), profile.size(), &range);
    if (!length)
        return;
    std::vector<double> scores(length);
    resdet_debug_knee_scores(profile.data(), length, scores.data());
    FILE *f = fopen(path.string().c_str(), "w");
    if (!f)
        return;
    fprintf(f, "pos,sign_vote,knee_score,log_profile\n");
    for (size_t k = 0; k < length; k++) {
        double vote = (k >= (size_t)range && k + range < length) ? votes[k - range] : 0.0;
        fprintf(f, "%zu,%.4f,%.4f,%.4f\n", k, vote, scores[k], profile[k]);
    }
    fclose(f);
}

struct trace_row {
    int idx;
    resdet_debug_frame d;
    resdet_result r;
    int plugin_w, plugin_h;
};

static void print_top(const char *label, const std::vector<std::pair<int, double>> &items)
{
    printf("      %-12s", label);
    int n = 0;
    for (auto &it : items) {
        if (n++ >= 5)
            break;
        printf(" %d(%.2f)", it.first, it.second);
    }
    printf("\n");
}

// Top-5 sign votes / knee scores in the searchable range of one axis
static void axis_diagnostics(resolution_detector *rd, int axis, const char *name)
{
    std::vector<float> votes(4096), profile(4096);
    int range = 0;
    size_t length = resdet_debug_axis(rd, axis, votes.data(), votes.size(), profile.data(), profile.size(), &range);
    if (!length)
        return;
    size_t lo = (size_t)(length * 0.25), hi = (size_t)(length * 0.97);

    std::vector<std::pair<int, double>> sign, knee;
    for (size_t i = 0; i + 2 * range < length; i++) {
        size_t pos = i + range;
        if (pos >= lo && pos < hi)
            sign.emplace_back((int)pos, (double)votes[i]);
    }
    std::vector<double> scores(length);
    resdet_debug_knee_scores(profile.data(), length, scores.data());
    for (size_t k = lo; k < hi; k++)
        if (scores[k] > 0.0)
            knee.emplace_back((int)k, scores[k]);

    auto by_score = [](const std::pair<int, double> &a, const std::pair<int, double> &b) { return a.second > b.second; };
    std::sort(sign.begin(), sign.end(), by_score);
    std::sort(knee.begin(), knee.end(), by_score);
    // de-duplicate neighbouring positions so the top-5 shows distinct features
    auto thin = [](std::vector<std::pair<int, double>> &v) {
        std::vector<std::pair<int, double>> out;
        for (auto &e : v) {
            bool near = false;
            for (auto &o : out)
                if (abs(o.first - e.first) < 24)
                    near = true;
            if (!near)
                out.push_back(e);
            if (out.size() >= 5)
                break;
        }
        v.swap(out);
    };
    thin(sign);
    thin(knee);
    char l1[32], l2[32];
    snprintf(l1, sizeof(l1), "sign %s:", name);
    snprintf(l2, sizeof(l2), "knee %s:", name);
    print_top(l1, sign);
    print_top(l2, knee);
}

static bool run_case(const case_spec &cs, bool verbose, const fs::path &outdir, const param_overrides &po)
{
    std::vector<fs::path> frames;
    if (fs::is_directory(cs.dir))
        for (auto &e : fs::directory_iterator(cs.dir))
            if (e.path().extension() == ".pgm")
                frames.push_back(e.path());
    std::sort(frames.begin(), frames.end());
    std::string name = cs.dir.filename().string();
    if (frames.empty()) {
        printf("[SKIP] %-52s no .pgm frames in %s\n", name.c_str(), cs.dir.string().c_str());
        return false;
    }
    auto plugin = load_plugin_csv(cs.dir);

    resolution_detector *rd = resdet_create();
    apply_params(rd, po);
    std::vector<trace_row> trace;
    resdet_result r = {};
    frame_img img;
    int fw = 0, fh = 0;
    for (size_t i = 0; i < frames.size(); i++) {
        if (!load_pgm(frames[i], img)) {
            printf("  ! cannot read %s\n", frames[i].string().c_str());
            continue;
        }
        fw = img.w;
        fh = img.h;
        while (!resdet_submit(rd, img.px.data(), img.w, img.h))
            std::this_thread::yield();
        int target = (int)trace.size() + 1;
        while (resdet_debug_analysis_count(rd) < target)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        trace_row row = {};
        row.idx = (int)i;
        resdet_debug_last_frame(rd, &row.d);
        resdet_result tmp;
        if (resdet_get_result(rd, &tmp))
            r = tmp;
        row.r = r;
        if (i < plugin.size()) {
            row.plugin_w = plugin[i].first;
            row.plugin_h = plugin[i].second;
        }
        trace.push_back(row);
    }

    bool native_expected = cs.exp_w == 0 && cs.exp_h == 0;
    bool pass = native_expected ? (r.src_w == 0 && r.src_h == 0)
                                : (abs(r.src_w - cs.exp_w) <= cs.tol && abs(r.src_h - cs.exp_h) <= cs.tol);

    char expect[32], got[64], plug[32] = "-";
    if (native_expected) snprintf(expect, sizeof(expect), "native");
    else snprintf(expect, sizeof(expect), "%dx%d", cs.exp_w, cs.exp_h);
    if (r.src_w || r.src_h) snprintf(got, sizeof(got), "%dx%d (%.2f/%.2f)", r.src_w, r.src_h, r.conf_w, r.conf_h);
    else snprintf(got, sizeof(got), "native");
    if (!trace.empty()) {
        auto &last = trace.back();
        if (last.plugin_w || last.plugin_h) snprintf(plug, sizeof(plug), "%dx%d", last.plugin_w, last.plugin_h);
        else snprintf(plug, sizeof(plug), "native");
    }
    const char *tag = pass ? (cs.xfail ? "XPASS" : "PASS") : (cs.xfail ? "XFAIL" : "FAIL");
    printf("[%s] %-52s expected %-10s got %-24s plugin(live) %s\n",
           tag, name.c_str(), expect, got, plug);
    if (!cs.note.empty())
        printf("       %s\n", cs.note.c_str());

    if ((!pass && !cs.xfail) || verbose) {
        printf("       frame  sign W/H (J=joint,S=solo)  knee W/H            consensus     plugin\n");
        for (auto &t : trace) {
            char mode = t.d.sign_mode == 2 ? 'J' : (t.d.sign_mode == 1 ? 'S' : ' ');
            printf("       %3d    %4d(%.2f) %4d(%.2f) %c     %4d(%.2f) %4d(%.2f)  %4dx%-4d     %dx%d\n",
                   t.idx, t.d.sign_w, t.d.sign_conf_w, t.d.sign_h, t.d.sign_conf_h, mode,
                   t.d.knee_w, t.d.knee_conf_w, t.d.knee_h, t.d.knee_conf_h,
                   t.r.src_w, t.r.src_h, t.plugin_w, t.plugin_h);
        }
        axis_diagnostics(rd, 0, "W");
        axis_diagnostics(rd, 1, "H");
        static uint8_t spec[RESDET_SPEC_W * RESDET_SPEC_H];
        if (resdet_get_spectrum(rd, spec)) {
            fs::create_directories(outdir);
            fs::path bmp = outdir / (name + ".bmp");
            write_bmp(bmp, spec, fw, fh, r.src_w, r.src_h, cs.exp_w, cs.exp_h);
            printf("       spectrum: %s (green = detected, yellow = expected)\n", bmp.string().c_str());
        }
    }
    if (po.dump_votes) {
        fs::create_directories(outdir);
        dump_axis_csv(rd, 0, outdir / (name + "_W.csv"));
        dump_axis_csv(rd, 1, outdir / (name + "_H.csv"));
        printf("       votes: %s\n", (outdir / (name + "_{W,H}.csv")).string().c_str());
    }
    resdet_destroy(rd);
    return pass;
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// "WxH", "native", optionally prefixed with "xfail:" for known limitations
static bool parse_expect(std::string s, int &w, int &h, bool &xfail)
{
    xfail = false;
    if (s.rfind("xfail:", 0) == 0) {
        xfail = true;
        s = trim(s.substr(6));
    }
    if (s == "native" || s == "0" || s == "0x0") {
        w = h = 0;
        return true;
    }
    return sscanf(s.c_str(), "%dx%d", &w, &h) == 2;
}

int main(int argc, char **argv)
{
    std::vector<case_spec> cases;
    bool verbose = false;
    fs::path outdir;
    fs::path single;
    std::string expect_str = "native";
    int tol = 16;
    fs::path manifest;
    param_overrides po;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs a value\n", what); exit(2); }
            return argv[++i];
        };
        if (a == "--manifest") manifest = next("--manifest");
        else if (a == "--expect") expect_str = next("--expect");
        else if (a == "--tol") tol = atoi(next("--tol"));
        else if (a == "--verbose" || a == "-v") verbose = true;
        else if (a == "--out") outdir = next("--out");
        else if (a == "--alpha") po.alpha = (float)atof(next("--alpha"));
        else if (a == "--sign-thr") po.sign_thr = (float)atof(next("--sign-thr"));
        else if (a == "--knee-thr") po.knee_thr = (float)atof(next("--knee-thr"));
        else if (a == "--cand-min") po.cand_min = (float)atof(next("--cand-min"));
        else if (a == "--joint-min") po.joint_min = (float)atof(next("--joint-min"));
        else if (a == "--flank-min") po.flank_min = (float)atof(next("--flank-min"));
        else if (a == "--warmup") po.warmup = atoi(next("--warmup"));
        else if (a == "--sign-weighted") po.weighted = atoi(next("--sign-weighted"));
        else if (a == "--min-votes") po.min_votes = atoi(next("--min-votes"));
        else if (a == "--dump-votes") po.dump_votes = true;
        else if (a[0] == '-') { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else single = a;
    }
    if (po.alpha >= 0 || po.sign_thr >= 0 || po.knee_thr >= 0 || po.warmup >= 0 || po.min_votes >= 0 ||
        po.cand_min >= 0 || po.joint_min >= 0 || po.flank_min >= 0)
        printf("params: alpha=%.3f sign-thr=%.3f knee-thr=%.3f cand-min=%.3f joint-min=%.3f flank-min=%.3f "
               "warmup=%d min-votes=%d  (-1 = default)\n",
               po.alpha, po.sign_thr, po.knee_thr, po.cand_min, po.joint_min, po.flank_min,
               po.warmup, po.min_votes);

    if (!manifest.empty()) {
        std::ifstream in(manifest);
        if (!in) { fprintf(stderr, "cannot open manifest %s\n", manifest.string().c_str()); return 2; }
        fs::path base = manifest.parent_path();
        if (outdir.empty()) outdir = base / "_resdet_out";
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> cols;
            std::stringstream ss(line);
            std::string c;
            while (std::getline(ss, c, ';')) cols.push_back(trim(c));
            if (cols.size() < 2) { fprintf(stderr, "bad manifest line: %s\n", line.c_str()); continue; }
            case_spec cs;
            cs.dir = base / cols[0];
            if (!parse_expect(cols[1], cs.exp_w, cs.exp_h, cs.xfail)) { fprintf(stderr, "bad expectation: %s\n", cols[1].c_str()); continue; }
            if (cols.size() >= 3 && !cols[2].empty()) cs.tol = atoi(cols[2].c_str());
            if (cols.size() >= 4) cs.note = cols[3];
            cases.push_back(cs);
        }
    } else if (!single.empty()) {
        case_spec cs;
        cs.dir = single;
        if (!parse_expect(expect_str, cs.exp_w, cs.exp_h, cs.xfail)) { fprintf(stderr, "bad --expect %s\n", expect_str.c_str()); return 2; }
        cs.tol = tol;
        cases.push_back(cs);
        if (outdir.empty()) outdir = single.parent_path() / "_resdet_out";
    } else {
        fprintf(stderr, "usage: resdet-cli <dumpdir> [--expect WxH|native] [--tol PX] [--verbose] [--out DIR]\n"
                        "       resdet-cli --manifest cases.txt [--verbose] [--out DIR]\n"
                        "tuning: --alpha A --sign-thr T --knee-thr T --cand-min T --joint-min S --flank-min T\n"
                        "        --warmup N --min-votes N --sign-weighted 0|1 --dump-votes\n");
        return 2;
    }

    int passed = 0, xfailed = 0, xpassed = 0, failed = 0;
    for (auto &cs : cases) {
        bool ok = run_case(cs, verbose, outdir, po);
        if (cs.xfail) (ok ? xpassed : xfailed)++;
        else (ok ? passed : failed)++;
    }
    size_t regular = cases.size() - xfailed - xpassed;
    printf("\nRESULT: %d/%zu passed", passed, regular);
    if (xfailed || xpassed)
        printf(", %d known limitations (xfail)%s", xfailed,
               xpassed ? " — and some now PASS (xpass): consider un-marking them" : "");
    printf("\n");
    return failed == 0 ? 0 : 1;
}
