// FPS analysis core — see fps-core.h.
//
// Moved out of fps-analyzer-filter.cpp unchanged. The arithmetic, the order of
// operations and every threshold are those of plugin v0.5.0; the substitutions
// are mechanical (OBS allocators -> std::vector, os_gettime_ns() -> the now_ns
// argument, VIDEO_FORMAT_* -> FPS_PIXFMT_*, the global published FPS -> a
// per-instance field). tools/fps-selftest.cpp proves the equality against a
// verbatim copy of v0.5.0 (tools/fps-ref-v050.cpp): the same goldens, plus a
// differential fuzz over random event streams.
//
// Do not include libobs headers here — the test tools build this file without
// OBS, against a headers-only SDK.

#include "fps-core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <new>
#include <vector>

#define FRAMETIME_HISTORY FPS_CORE_HISTORY

struct fps_core {
    struct fps_core_params params; // update_interval already normalised

    // frametime bookkeeping
    uint64_t last_unique_frame_time = 0;
    uint64_t last_write_time = 0;
    double frametime_history[FRAMETIME_HISTORY] = {};
    double smoothed_frametime[FRAMETIME_HISTORY] = {};
    double fps_per_frame[FRAMETIME_HISTORY] = {};
    bool tearing_per_frame[FRAMETIME_HISTORY] = {};
    int frametime_pos = 0;
    int frametime_count = 0;
    double ema_frametime = 0.0;

    // frame comparison. `*_allocated` mirrors v0.5.0's "is the pointer set"
    // test, which is not the same as "is the buffer non-empty" for a
    // zero-sized frame.
    std::vector<uint8_t> prev_frame;
    bool prev_frame_allocated = false;
    std::vector<uint8_t> luma_buffer;

    // tearing
    std::vector<uint8_t> prev_lines[3];
    bool prev_lines_allocated = false;
    size_t prev_lines_size = 0;
    int tearing_history[FPS_CORE_TEARING_HISTORY] = {};
    int tearing_history_pos = 0;
    bool tearing_detected = false;

    // v0.5.0 read the previously published FPS back out of the shared overlay
    // data, so two filters would feed each other's averaging window. Keeping it
    // per instance is identical for a single filter and drops that coupling.
    int last_published_fps = 0;

    struct fps_core_frame_debug dbg = {};

    // Capture-path beat detector (observes only; see fps-core.h)
    struct beat_state {
        int clean_frames = 0;          // consecutive unique frames
        int clean_ticks = 0;           // consecutive ticks with exactly one frame
        bool in_cluster = false;
        uint64_t cluster_start = 0;
        uint64_t cluster_last = 0;
        int cluster_events = 0;
        bool cluster_obs = false;      // the open cluster had an empty tick
        bool cluster_stall = false;    // FPS_BEAT_STALL_RUN events in a row
        double cluster_sum_s = 0.0;    // sum of event times relative to cluster_start
        int run_dup = 0;               // consecutive duplicate frames
        int run_empty = 0;             // consecutive empty ticks
        // closed clusters, oldest first (times are the cluster means)
        uint64_t times[FPS_BEAT_HISTORY] = {};
        bool obs[FPS_BEAT_HISTORY] = {};
        int n = 0;
        int clusters = 0;
        uint64_t obs_start = 0;        // when the current check started (0 = no frame yet)
        uint64_t last_activity = 0;    // last frame / tick that delivered one
    } beat;
};

// --- Small helpers ---

static void ensure_luma_buffer(struct fps_core *c, size_t needed)
{
    if (c->luma_buffer.size() >= needed)
        return;
    c->luma_buffer.assign(needed, 0);
}

static size_t count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t size)
{
    size_t diff = 0;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i])
            ++diff;
    }
    return diff;
}

static void init_prev_lines_buffers(struct fps_core *c, int roi_width)
{
    for (int i = 0; i < 3; ++i)
        c->prev_lines[i].assign((size_t)roi_width, 0);
    c->prev_lines_size = (size_t)roi_width;
    c->prev_lines_allocated = true;
}

static void init_prev_frame_buffer(struct fps_core *c, size_t roi_size, const uint8_t *roi_ptr)
{
    c->prev_frame.assign(roi_size, 0);
    c->prev_frame_allocated = true;
    memcpy(c->prev_frame.data(), roi_ptr, roi_size);
}

// --- Luma conversion (ITU-R BT.601, studio range [16, 235]) ---

static void bgra_to_luma(const uint8_t *bgra, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = bgra + y * linesize;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            luma[y * width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
        }
    }
}

static void rgba_to_luma(const uint8_t *rgba, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height)
{
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = rgba + y * linesize;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t r = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t b = row[x * 4 + 2];
            luma[y * width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
        }
    }
}

// `lines` rows starting at `first_line`, packed width bytes per row.
static bool extract_luma_rows(const struct fps_frame_view *frame, uint8_t *luma, uint32_t first_line, uint32_t lines)
{
    const uint32_t width = frame->width;
    switch (frame->format) {
    case FPS_PIXFMT_NV12:
    case FPS_PIXFMT_I420:
    case FPS_PIXFMT_I444:
    case FPS_PIXFMT_I422:
        for (uint32_t y = 0; y < lines; ++y)
            memcpy(luma + y * width, frame->data + (first_line + y) * frame->linesize, width);
        return true;
    case FPS_PIXFMT_YUY2:
        for (uint32_t y = 0; y < lines; ++y) {
            const uint8_t *src = frame->data + (first_line + y) * frame->linesize;
            for (uint32_t x = 0; x < width; ++x)
                luma[y * width + x] = src[x * 2];
        }
        return true;
    case FPS_PIXFMT_UYVY:
        for (uint32_t y = 0; y < lines; ++y) {
            const uint8_t *src = frame->data + (first_line + y) * frame->linesize;
            for (uint32_t x = 0; x < width; ++x)
                luma[y * width + x] = src[x * 2 + 1];
        }
        return true;
    case FPS_PIXFMT_BGRA:
        bgra_to_luma(frame->data + first_line * frame->linesize, frame->linesize, luma, width, lines);
        return true;
    case FPS_PIXFMT_RGBA:
        rgba_to_luma(frame->data + first_line * frame->linesize, frame->linesize, luma, width, lines);
        return true;
    default:
        return false;
    }
}

// --- Capture-path beat detector ---

static void beat_close_cluster(struct fps_core *c)
{
    auto &b = c->beat;
    b.in_cluster = false;
    if (b.cluster_stall || b.cluster_events > FPS_BEAT_MAX_EVENTS)
        return; // a stall (static scene, loading screen, signal loss), not a beat
    b.clusters++;
    if (b.n == FPS_BEAT_HISTORY) {
        memmove(b.times, b.times + 1, sizeof(b.times[0]) * (FPS_BEAT_HISTORY - 1));
        memmove(b.obs, b.obs + 1, sizeof(b.obs[0]) * (FPS_BEAT_HISTORY - 1));
        b.n--;
    }
    b.times[b.n] = b.cluster_start + (uint64_t)(b.cluster_sum_s / b.cluster_events * 1e9);
    b.obs[b.n] = b.cluster_obs;
    b.n++;
}

static const double k_std_rates[] = {
    24000.0 / 1001.0, 24.0, 25.0, 30000.0 / 1001.0, 30.0, 48.0, 50.0,
    60000.0 / 1001.0, 60.0, 100.0, 120000.0 / 1001.0, 120.0,
};

static double beat_snap(double hz)
{
    for (double r : k_std_rates)
        if (fabs(hz / r - 1.0) <= FPS_BEAT_SNAP_TOL)
            return r;
    return 0.0;
}

static void beat_reset(struct fps_core *c, uint64_t now)
{
    int clusters = c->beat.clusters;
    c->beat = fps_core::beat_state();
    c->beat.clusters = clusters; // diagnostic total survives
    c->beat.obs_start = now;
    c->beat.last_activity = now;
}

void fps_core_beat_reset(struct fps_core *c, uint64_t now_ns)
{
    if (c)
        beat_reset(c, now_ns);
}

// Called before every new observation: starts the check over after a lost
// signal, closes a cluster that has gone quiet. `delivered` is true for a
// frame or a tick that brought one.
static void beat_advance(struct fps_core *c, uint64_t now, bool delivered)
{
    auto &b = c->beat;
    if (delivered) {
        if (!b.obs_start || now - b.last_activity > FPS_BEAT_RESET_GAP_NS)
            beat_reset(c, now);
        b.last_activity = now;
    }
    if (b.in_cluster && now - b.cluster_last > FPS_BEAT_CLUSTER_NS)
        beat_close_cluster(c);
}

// `run` is how many events in a row this one completes (1 = isolated).
static void beat_event(struct fps_core *c, uint64_t now, bool from_obs, bool clean_before, int run)
{
    auto &b = c->beat;
    if (!b.in_cluster) {
        if (!clean_before)
            return; // not preceded by a steady stream: the game, not the capture path
        b.in_cluster = true;
        b.cluster_start = now;
        b.cluster_events = 0;
        b.cluster_sum_s = 0.0;
        b.cluster_obs = false;
        b.cluster_stall = false;
    }
    b.cluster_events++;
    b.cluster_last = now;
    b.cluster_sum_s += (double)(now - b.cluster_start) / 1e9;
    b.cluster_obs = b.cluster_obs || from_obs;
    if (run >= FPS_BEAT_STALL_RUN)
        b.cluster_stall = true;
}

static void beat_on_frame(struct fps_core *c, bool unique, uint64_t now)
{
    auto &b = c->beat;
    beat_advance(c, now, true);
    if (unique) {
        b.clean_frames++;
        b.run_dup = 0;
        return;
    }
    b.run_dup++;
    beat_event(c, now, false, b.clean_frames >= FPS_BEAT_CLEAN_RUN, b.run_dup);
    b.clean_frames = 0;
}

void fps_core_obs_tick(struct fps_core *c, uint64_t now_ns, int frames)
{
    if (!c)
        return;
    auto &b = c->beat;
    beat_advance(c, now_ns, frames > 0);
    if (frames == 1) {
        b.clean_ticks++;
        b.run_empty = 0;
        return;
    }
    if (frames == 0) {
        b.run_empty++;
        beat_event(c, now_ns, true, b.clean_ticks >= FPS_BEAT_CLEAN_RUN, b.run_empty);
    } else {
        b.run_empty = 0;
    }
    b.clean_ticks = 0;
}

// Index of the closed cluster of the given kind within tol of t, searching
// below index `below`; -1 if none. Clusters are in time order.
static int beat_find(const struct fps_core::beat_state &b, bool obs, double t, double tol, int below)
{
    for (int i = below - 1; i >= 0; i--) {
        double d = (double)b.times[i] / 1e9 - t;
        if (d < -tol)
            break;
        if (b.obs[i] == obs && fabs(d) <= tol)
            return i;
    }
    return -1;
}

// The confirmed-beat search: fills the beat fields of `out` and sets
// out->detected when a chain qualifies.
static void beat_detect(const struct fps_core *c, uint64_t now_ns, double delivery_hz, struct fps_beat *out)
{
    const auto &b = c->beat;
    if (b.n < 2)
        return;

    // Chains ending at one of the latest clusters L (the newest may be a
    // stray: a game hitch, a fragment of a wide OBS burst), through clusters
    // of L's kind only: for each earlier cluster E of that kind as the first
    // step, walk back by the period refined from the chain so far. The
    // shortest period with a full chain wins (a 2P chain exists whenever a
    // P chain does).
    for (int last = b.n - 1; last >= 1 && last >= b.n - 1 - FPS_BEAT_END_SEARCH; last--) {
        const bool obs = b.obs[last];
        const double tol = obs ? FPS_BEAT_TOL_OBS_S : FPS_BEAT_TOL_CAPTURE_S;
        const int need = obs ? FPS_BEAT_CHAIN_OBS : FPS_BEAT_CHAIN_CAPTURE;
        const double tl = (double)b.times[last] / 1e9;
        for (int e = last - 1; e >= 0; e--) {
            if (b.obs[e] != obs)
                continue;
            double p = tl - (double)b.times[e] / 1e9;
            if (p < FPS_BEAT_MIN_PERIOD_S)
                continue;
            int chain = 2, oldest = e;
            for (;;) {
                double step = (tl - (double)b.times[oldest] / 1e9) / (chain - 1);
                int k = beat_find(b, obs, (double)b.times[oldest] / 1e9 - step, tol, oldest);
                if (k < 0)
                    break;
                oldest = k;
                chain++;
            }
            if (chain < need)
                continue;
            // The shortest regular period decides. If it is no standard-rate
            // difference (a game hitching on a 5 s timer), its multiples (15 s
            // -> 59.933 Hz, close to 59.94) are not considered either.
            double period = (tl - (double)b.times[oldest] / 1e9) / (chain - 1);
            double signal = delivery_hz > 0.0 ? delivery_hz - 1.0 / period : 0.0;
            double nominal = delivery_hz > 0.0 ? beat_snap(signal) : 0.0;
            if (delivery_hz > 0.0 && nominal == 0.0)
                return; // not the difference of two standard clocks
            if (nominal > 0.0 && fabs(nominal / delivery_hz - 1.0) < 1e-9 &&
                chain < need + FPS_BEAT_CHAIN_DRIFT_EXTRA)
                return; // same nominal rate (crystal drift): needs a longer chain
            // Gone quiet for three periods (settings fixed, source changed): expired.
            if (now_ns > b.times[last] && (double)(now_ns - b.times[last]) / 1e9 > 3.0 * period)
                return;
            out->detected = true;
            out->period_s = period;
            out->origin = obs ? FPS_BEAT_ORIGIN_OBS : FPS_BEAT_ORIGIN_CAPTURE;
            out->signal_hz = signal;
            out->nominal_hz = nominal;
            out->chain = chain;
            return;
        }
    }
}

// CHECKING vs CLEAR when no beat is confirmed: has the detector watched long
// enough, and has every repeat seen had two periods to show it is not part of
// a beat? A repeat inside the last 2 * 1001/D (plus tolerance) holds the check
// back: if it belongs to a beat, its successor arrives within that time (a
// missed one included), so the hold extends until the beat is confirmed.
static void beat_verify(const struct fps_core *c, uint64_t now_ns, double delivery_hz, struct fps_beat *out)
{
    const auto &b = c->beat;
    out->observed_s = b.obs_start && now_ns > b.obs_start ? (double)(now_ns - b.obs_start) / 1e9 : 0.0;
    out->status = FPS_BEAT_CHECKING;
    if (delivery_hz <= 0.0)
        return; // no rate to derive the period from
    const double p = 1001.0 / delivery_hz;
    out->verify_s = p * FPS_BEAT_VERIFY_PERIODS > FPS_BEAT_VERIFY_MIN_S ? p * FPS_BEAT_VERIFY_PERIODS
                                                                       : FPS_BEAT_VERIFY_MIN_S;

    // clusters within the window, the still open one included
    double t[FPS_BEAT_HISTORY + 1];
    bool k[FPS_BEAT_HISTORY + 1];
    int m = 0;
    const double now_s = (double)now_ns / 1e9;
    for (int i = 0; i < b.n; i++) {
        double ti = (double)b.times[i] / 1e9;
        if (now_s - ti <= out->verify_s) {
            t[m] = ti;
            k[m++] = b.obs[i];
        }
    }
    if (b.in_cluster && !b.cluster_stall) {
        t[m] = (double)b.cluster_start / 1e9 + b.cluster_sum_s / b.cluster_events;
        k[m++] = b.cluster_obs;
    }
    // uneven: repeats the capture beat does not explain. A repeat 1 or 2 periods
    // from another of the same kind is the card's own (not yet confirmed) beat,
    // not the game, so it does not count.
    int irregular = 0;
    for (int i = 0; i < m; i++) {
        const double tol = 2.0 * (k[i] ? FPS_BEAT_TOL_OBS_S : FPS_BEAT_TOL_CAPTURE_S);
        bool regular = false;
        for (int j = 0; j < m && !regular; j++) {
            if (j == i || k[j] != k[i])
                continue;
            const double d = fabs(t[j] - t[i]);
            regular = fabs(d - p) <= tol || fabs(d - 2.0 * p) <= tol;
        }
        irregular += regular ? 0 : 1;
    }
    out->uneven = irregular >= FPS_BEAT_UNEVEN_CLUSTERS;

    bool held = false;
    for (int i = 0; i < m && !held; i++) {
        double tol = k[i] ? FPS_BEAT_TOL_OBS_S : FPS_BEAT_TOL_CAPTURE_S;
        held = now_s - t[i] < 2.0 * p + tol;
    }
    if (!held && out->observed_s >= out->verify_s)
        out->status = FPS_BEAT_CLEAR;
}

void fps_core_get_beat(const struct fps_core *c, uint64_t now_ns, double delivery_hz, struct fps_beat *out)
{
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    if (!c)
        return;
    out->clusters = c->beat.clusters;
    beat_detect(c, now_ns, delivery_hz, out);
    beat_verify(c, now_ns, delivery_hz, out);
    if (out->detected)
        out->status = FPS_BEAT_DETECTED;
}

// --- Unique-frame bookkeeping ---

// Everything v0.5.0 did under `if (is_unique)`. Split out so a recorded trace
// can replay the decision without pixels.
static void analyze_unique(struct fps_core *c, uint64_t now)
{
    if (c->last_unique_frame_time != 0) {
        double ft = (now - c->last_unique_frame_time) / 1000000.0;
        c->frametime_history[c->frametime_pos] = ft;
        c->tearing_per_frame[c->frametime_pos] = c->tearing_detected;

        // Smoothed frametime: EMA, alpha 0.15 — responsive enough to show
        // stutters, smooth enough to hide sampling noise. It initialises from
        // the first value and is deliberately never reset, so the curve
        // survives a stale gap.
        const double alpha = 0.15;
        if (c->ema_frametime <= 0.0)
            c->ema_frametime = ft;
        else
            c->ema_frametime = c->ema_frametime * (1.0 - alpha) + ft * alpha;
        c->smoothed_frametime[c->frametime_pos] = c->ema_frametime;

        // Per-frame FPS: average the frametimes over roughly the last second,
        // sized from the instantaneous estimate and clamped to what we have.
        int inst_fps = (ft > 0.0) ? (int)round(1000.0 / ft) : 30;
        if (inst_fps < 10) inst_fps = 10;
        if (inst_fps > 120) inst_fps = 120;
        int window = inst_fps;
        if (window > c->frametime_count) window = c->frametime_count;
        if (window < 1) window = 1;
        double sum = 0.0;
        for (int w = 0; w < window; w++) {
            int idx = (c->frametime_pos - w + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
            sum += c->frametime_history[idx];
        }
        double avg_ft = sum / window;
        c->fps_per_frame[c->frametime_pos] = (avg_ft > 0.0) ? round(1000.0 / avg_ft) : 0.0;

        c->dbg.sample_written = true;
        c->dbg.ft_ms = ft;
        c->dbg.ema = c->ema_frametime;
        c->dbg.fps_pf = c->fps_per_frame[c->frametime_pos];

        c->frametime_pos = (c->frametime_pos + 1) % FRAMETIME_HISTORY;
        if (c->frametime_count < FRAMETIME_HISTORY)
            c->frametime_count++;
    }
    c->last_unique_frame_time = now;
}

static void analyze_luma_frame(struct fps_core *c, const uint8_t *luma_ptr, size_t luma_size, uint64_t now)
{
    bool is_unique = false;
    c->dbg.luma_size = luma_size;
    c->dbg.compared = false;
    c->dbg.diff = 0;
    c->dbg.percent = 0.0;
    c->dbg.sample_written = false;
    c->dbg.ft_ms = c->dbg.ema = c->dbg.fps_pf = 0.0;

    if (!c->prev_frame_allocated || c->prev_frame.size() != luma_size) {
        // First frame, or the analysed region changed size: nothing to compare
        // against, so treat it as new.
        init_prev_frame_buffer(c, luma_size, luma_ptr);
        is_unique = true;
    } else {
        size_t diff = count_diff_bytes(luma_ptr, c->prev_frame.data(), luma_size);
        double percent = (luma_size > 0) ? (100.0 * diff / luma_size) : 0.0;
        if (percent >= c->params.sensitivity)
            is_unique = true;
        memcpy(c->prev_frame.data(), luma_ptr, luma_size);
        c->dbg.compared = true;
        c->dbg.diff = diff;
        c->dbg.percent = percent;
    }

    c->dbg.unique = is_unique;
    if (is_unique)
        analyze_unique(c, now);
    beat_on_frame(c, is_unique, now);
    c->dbg.pos = c->frametime_pos;
    c->dbg.count = c->frametime_count;
}

// --- Tearing detection ---

// Takes the three probe lines (top, middle, bottom) concatenated, roi_width
// bytes each. A frame tears when some of them changed and others did not.
static bool detect_tearing_core(struct fps_core *c, const uint8_t *lines_luma, int roi_width)
{
    if (!c->params.enable_tearing_detection)
        return false;

    if (!c->prev_lines_allocated || c->prev_lines_size != (size_t)roi_width) {
        init_prev_lines_buffers(c, roi_width);
        for (int i = 0; i < 3; ++i)
            memcpy(c->prev_lines[i].data(), lines_luma + (size_t)i * roi_width, roi_width);
        return false;
    }

    double change_percent[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        size_t diff = count_diff_bytes(lines_luma + (size_t)i * roi_width, c->prev_lines[i].data(), roi_width);
        change_percent[i] = (roi_width > 0) ? (100.0 * diff / roi_width) : 0.0;
    }
    for (int i = 0; i < 3; ++i)
        memcpy(c->prev_lines[i].data(), lines_luma + (size_t)i * roi_width, roi_width);

    bool significant_change[3];
    for (int i = 0; i < 3; ++i)
        significant_change[i] = (change_percent[i] >= c->params.tearing_sensitivity);

    bool all_changed = significant_change[0] && significant_change[1] && significant_change[2];
    bool none_changed = !significant_change[0] && !significant_change[1] && !significant_change[2];
    bool tearing = !(all_changed || none_changed);

    c->tearing_history[c->tearing_history_pos] = tearing ? 1 : 0;
    c->tearing_history_pos = (c->tearing_history_pos + 1) % FPS_CORE_TEARING_HISTORY;

    // Report only when the last few frames agree, so one odd frame does not
    // light up the indicator.
    int recent_tears = 0;
    for (int i = 0; i < FPS_CORE_TEARING_HISTORY; ++i)
        recent_tears += c->tearing_history[i];
    return recent_tears >= 2;
}

static bool detect_tearing_frame(struct fps_core *c, const struct fps_frame_view *frame)
{
    if (!c->params.enable_tearing_detection)
        return false;

    const int roi_width = (int)frame->width;
    if (roi_width > FPS_CORE_MAX_TEARING_WIDTH)
        return false;

    uint8_t lines_luma[3 * FPS_CORE_MAX_TEARING_WIDTH];
    int line_ys[3] = {0, (int)frame->height / 2, (int)frame->height - 1};

    switch (frame->format) {
    case FPS_PIXFMT_NV12:
    case FPS_PIXFMT_I420:
    case FPS_PIXFMT_I444:
    case FPS_PIXFMT_I422:
        for (int i = 0; i < 3; ++i)
            memcpy(lines_luma + (size_t)i * roi_width, frame->data + (size_t)line_ys[i] * frame->linesize, roi_width);
        break;
    case FPS_PIXFMT_YUY2:
        for (int i = 0; i < 3; ++i) {
            const uint8_t *src = frame->data + (size_t)line_ys[i] * frame->linesize;
            for (int x = 0; x < roi_width; ++x)
                lines_luma[(size_t)i * roi_width + x] = src[x * 2];
        }
        break;
    case FPS_PIXFMT_UYVY:
        for (int i = 0; i < 3; ++i) {
            const uint8_t *src = frame->data + (size_t)line_ys[i] * frame->linesize;
            for (int x = 0; x < roi_width; ++x)
                lines_luma[(size_t)i * roi_width + x] = src[x * 2 + 1];
        }
        break;
    case FPS_PIXFMT_BGRA:
        for (int i = 0; i < 3; ++i)
            bgra_to_luma(frame->data + (size_t)line_ys[i] * frame->linesize, frame->linesize,
                         lines_luma + (size_t)i * roi_width, (uint32_t)roi_width, 1);
        break;
    case FPS_PIXFMT_RGBA:
        for (int i = 0; i < 3; ++i)
            rgba_to_luma(frame->data + (size_t)line_ys[i] * frame->linesize, frame->linesize,
                         lines_luma + (size_t)i * roi_width, (uint32_t)roi_width, 1);
        break;
    default:
        return false;
    }

    return detect_tearing_core(c, lines_luma, roi_width);
}

// --- Public API ---

static void apply_params(struct fps_core *c, const struct fps_core_params *p)
{
    c->params = *p;
    if (c->params.update_interval <= 0.0)
        c->params.update_interval = 1.0;
}

struct fps_core *fps_core_create(const struct fps_core_params *p)
{
    struct fps_core *c = new (std::nothrow) fps_core();
    if (!c)
        return NULL;
    struct fps_core_params defaults = fps_core_params_defaults();
    apply_params(c, p ? p : &defaults);
    return c;
}

void fps_core_destroy(struct fps_core *c)
{
    delete c;
}

void fps_core_set_params(struct fps_core *c, const struct fps_core_params *p)
{
    if (c && p)
        apply_params(c, p);
}

void fps_core_get_params(const struct fps_core *c, struct fps_core_params *out)
{
    if (c && out)
        *out = c->params;
}

int fps_core_feed(struct fps_core *c, const struct fps_frame_view *frame, uint64_t now_ns)
{
    if (!frame || !frame->data)
        return FPS_FEED_NO_DATA;

    const uint32_t width = frame->width;
    const uint32_t height = frame->height;
    uint32_t roi_line, roi_lines;

    if (c->params.analyze_method == FPS_ANALYZE_DIFF) {
        roi_line = 0;
        roi_lines = height;
    } else {
        roi_line = height - 1;
        roi_lines = 1;
    }

    const size_t luma_size = (size_t)width * roi_lines;
    // Room for a full frame: tearing detection needs three lines even when the
    // analysis only looks at the last one.
    ensure_luma_buffer(c, (size_t)width * height);
    uint8_t *luma = c->luma_buffer.data();

    // An unsupported format leaves the state completely untouched.
    if (!extract_luma_rows(frame, luma, roi_line, roi_lines))
        return FPS_FEED_UNSUPPORTED_FORMAT;

    // Tearing is judged on every frame, including duplicates.
    c->tearing_detected = detect_tearing_frame(c, frame);
    c->dbg.tearing_flag = c->tearing_detected;

    analyze_luma_frame(c, luma, luma_size, now_ns);
    return FPS_FEED_OK;
}

bool fps_core_tick(struct fps_core *c, uint64_t now_ns, struct fps_core_output *out)
{
    if (!c || !out)
        return false;

    double elapsed = (now_ns - c->last_write_time) / 1000000000.0;
    if (elapsed < c->params.update_interval)
        return false;
    c->last_write_time = now_ns;

    // No unique frame for over 2 s: drop the samples so the overlay reads 0
    // instead of a stale average. The EMA and the history contents stay, which
    // is why a resumed source picks its curve back up.
    if (c->last_unique_frame_time != 0 && now_ns - c->last_unique_frame_time > 2000000000ULL) {
        c->frametime_count = 0;
        c->frametime_pos = 0;
    }

    // Average roughly the last second of frametimes: at most 120 samples,
    // shrunk towards the previously published FPS, at least 10 once we have 10.
    int window = c->frametime_count;
    if (window > 120) window = 120;
    if (c->last_published_fps > 10 && c->last_published_fps < window)
        window = c->last_published_fps;
    if (window < 10 && c->frametime_count >= 10) window = 10;
    if (window > c->frametime_count) window = c->frametime_count;

    double avg_frametime = 0.0;
    for (int i = 0; i < window; ++i) {
        int idx = (c->frametime_pos - window + i + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
        avg_frametime += c->frametime_history[idx];
    }
    if (window > 0)
        avg_frametime /= window;

    double fps = (avg_frametime > 0.0) ? (1000.0 / avg_frametime) : 0.0;
    int fps_smooth = (int)round(fps);

    c->last_published_fps = fps_smooth;
    out->fps = fps_smooth;
    out->frametime_ms = avg_frametime;
    out->tearing_detected = c->tearing_detected;
    out->window = window;

    // Linearise the ring buffer for the graph: oldest first.
    int count = c->frametime_count;
    if (count > FPS_CORE_HISTORY)
        count = FPS_CORE_HISTORY;
    for (int i = 0; i < count; i++) {
        int idx = (c->frametime_pos - count + i + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
        out->graph_frametimes[i] = c->smoothed_frametime[idx];
        out->graph_frametimes_raw[i] = c->frametime_history[idx];
        out->graph_fps[i] = c->fps_per_frame[idx];
        out->graph_tearing[i] = c->tearing_per_frame[idx];
    }
    out->graph_count = count;
    return true;
}

// --- Pure helpers ---

size_t fps_core_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n)
{
    return count_diff_bytes(a, b, n);
}

void fps_core_bgra_to_luma(const uint8_t *bgra, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height)
{
    bgra_to_luma(bgra, linesize, luma, width, height);
}

void fps_core_rgba_to_luma(const uint8_t *rgba, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height)
{
    rgba_to_luma(rgba, linesize, luma, width, height);
}

bool fps_core_extract_luma(const struct fps_frame_view *f, uint8_t *luma, uint32_t first_line, uint32_t lines)
{
    return extract_luma_rows(f, luma, first_line, lines);
}

int fps_core_format_csv_line(char *buf, size_t size, long long time_s, int fps, double frametime_ms)
{
    return snprintf(buf, size, "%lld,%d,%.2f\n", time_s, fps, frametime_ms);
}

// Trims the log to its last n lines. Reads in 256-byte chunks, so a longer line
// counts as several — kept as it was to avoid changing existing log files.
void fps_core_csv_keep_last_n_lines(const char *csv_path, int n)
{
    if (n < 0 || n > FPS_CORE_CSV_HISTORY_LIMIT)
        return;
    FILE *f = fopen(csv_path, "r");
    if (!f)
        return;
    std::vector<char *> lines((size_t)FPS_CORE_CSV_HISTORY_LIMIT + 1, nullptr);
    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        lines[(size_t)count] = _strdup(buf);
        count++;
        if (count > n) {
            free(lines[0]);
            memmove(lines.data(), lines.data() + 1, sizeof(char *) * (size_t)n);
            count = n;
        }
    }
    fclose(f);
    f = fopen(csv_path, "w");
    if (f) {
        for (int i = 0; i < count; ++i) {
            fputs(lines[(size_t)i], f);
            free(lines[(size_t)i]);
        }
        fclose(f);
    } else {
        for (int i = 0; i < count; ++i)
            free(lines[(size_t)i]);
    }
}

// --- Debug / replay ---

void fps_core_debug_last_frame(const struct fps_core *c, struct fps_core_frame_debug *out)
{
    if (c && out)
        *out = c->dbg;
}

void fps_core_debug_feed_decision(struct fps_core *c, bool unique, bool tearing, uint64_t now_ns)
{
    if (!c)
        return;
    c->tearing_detected = tearing;
    c->dbg.tearing_flag = tearing;
    c->dbg.luma_size = 0;
    c->dbg.compared = false;
    c->dbg.diff = 0;
    c->dbg.percent = 0.0;
    c->dbg.sample_written = false;
    c->dbg.ft_ms = c->dbg.ema = c->dbg.fps_pf = 0.0;
    c->dbg.unique = unique;
    if (unique)
        analyze_unique(c, now_ns);
    beat_on_frame(c, unique, now_ns);
    c->dbg.pos = c->frametime_pos;
    c->dbg.count = c->frametime_count;
}

int fps_core_debug_last_published_fps(const struct fps_core *c)
{
    return c ? c->last_published_fps : 0;
}
