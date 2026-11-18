#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#include "fps-core.h"
#include "fps-shared-data.h"
#include "resolution-detector.h"
#include "source-rate.h"

// Global shared data — read by fps-analyzer-overlay.cpp
struct fps_shared_data g_fps_shared = {};

// Declare overlay info for registration in overlay file
extern struct obs_source_info fps_overlay_source_info;

#define FPS_CSV_HISTORY_LIMIT 300
// Version stamped into FPS trace headers (see trace_note_frame)
#define FPS_TRACE_PLUGIN_VERSION "0.6.0"

// Dodaj enum do wyboru metody analizy
typedef enum {
    ANALYZE_LAST_LINE = 0,
    ANALYZE_DIFF = 1
} analyze_method_t;

// Prototypes
static void build_csv_path(const char *output_path, char *csv_path, size_t csv_path_size);

struct fps_analyzer_filter {
    obs_source_t *context;
    char output_path[512];
    bool clear_csv_on_start;
    bool enable_csv;
    // Analysis settings; the analysis itself (unique frames, frametimes, EMA,
    // tearing, per-tick averaging) runs in fps-core, the same code the test
    // tools pin with goldens and replay traces through.
    double update_interval;
    analyze_method_t analyze_method;
    double sensitivity;
    bool enable_tearing_detection;
    double tearing_sensitivity;
    struct fps_core *core;
    struct fps_core_output core_out; // last publish (large: kept off the stack)
    // Full-frame luma for the resolution detector and the frame dump
    uint8_t *luma_buffer;
    size_t luma_buffer_size;
    // GPU staging resources for sync sources (video_render path)
    gs_texrender_t *texrender;
    gs_stagesurf_t *stagesurface;
    uint32_t staged_width;
    uint32_t staged_height;
    // Debug: log format once
    int last_logged_format;
    // Upscale source resolution detection
    bool enable_resolution_detection;
    struct resolution_detector *res_detector;
    uint64_t last_resdet_submit_ns;
    int spectrum_fps;             // 0 = follow analysis rate (~2/s), else 30/60
    uint64_t last_spec_submit_ns;
    uint8_t *crop_buffer;         // RESDET_FAST_CROP_W*H luma for the fast spectrum path
    // Debug: frame dump (the exact luma planes fed to the detector)
    bool debug_options;
    char dump_dir[512];
    char dump_label[128];
    int dump_count;
    int dump_delay;               // seconds between the button click and the first frame
    long dump_remaining;          // >0 while a dump session is active (set from the UI thread);
                                  // accessed through std::atomic_ref (see dump_remaining_* helpers)
    uint64_t dump_not_before_ns;  // frames are written only once os_gettime_ns() passes this
    int dump_done_frames;         // frames written by the last finished session (0 = none)
    bool dump_status_dirty;       // status label needs a properties refresh
    uint64_t dump_status_refresh_ns;
    int dump_index;
    char dump_session_dir[640];
    FILE *dump_csv;
    uint64_t dump_start_ns;
    // Debug: FPS trace recorder — one CSV row per analysed frame (the
    // unique/tearing decision with its timestamp) and per published tick.
    // Replayed offline by the test tools to pin the FPS algorithm's output.
    bool trace_enabled;
    FILE *trace_file;
    char trace_path[640];
    bool trace_header_pending;    // write a '#' header before the next row
    int trace_kind;               // 1 = async (capture card), 2 = sync (GPU)
    uint32_t trace_w, trace_h;
    int trace_fmt;
    uint64_t trace_not_before_ns; // nothing is recorded until os_gettime_ns() passes this
    int trace_rows;               // frame rows written in the current session
    int trace_done_rows;          // and in the last finished one, for the status label
    char trace_done_path[640];
    long trace_toggle_req;        // hotkey -> video thread, via std::atomic_ref
    uint64_t trace_src_ts;        // frame->timestamp of the frame being analysed (0 = sync path)
    int frames_since_tick;        // filter_video calls since the last video_tick (K rows)
    // Source refresh rate (async sources: driver timestamps of each frame)
    struct srate *srate;
    int srate_logged_status;      // last status written to the OBS log
    uint64_t srate_diag_log_ns;   // last diagnostic line while measuring
    bool beat_logged;             // a beat verdict is in the OBS log
    bool beat_clear_logged;       // the signal check's OK is in the OBS log
    bool beat_seen_rates;         // beat_obs_hz / beat_device_hz are set
    double beat_obs_hz;           // rates the current signal check runs under
    double beat_device_hz;
    // Reliability: no FPS where it cannot be measured exactly
    bool block_unreliable;        // "block_unreliable" setting (default on)
    int unsupported_fmt;          // VIDEO_FORMAT_* of the last async frame the core cannot read, -1 = ok
    int blocked_reason;           // FPS_BLOCK_* last published (for the log)
    bool trace_altered_start;     // the trace's fresh core inherited the altered verdict
    // What the core is asked to compute, kept in step with the settings and
    // with the overlay's demand (core_sync)
    bool core_measure_deltas;
    bool core_measure_histogram;    // the full |d| histogram: only while a trace is recorded
    bool core_beat_enabled;
    uint64_t core_pacing_period_ns; // OBS frame interval given to the core (frame pacing, sync path)
    uint32_t history_gen_seen;      // last g_history_reset_gen this filter acted on
    // Dropped frames ("detect_drops" setting, default on)
    bool detect_drops;
    uint32_t lagged_seen;           // obs_get_lagged_frames() at the previous tick
    uint64_t lag_ev_t[64];          // OBS render-lag frames: when (ns) ...
    int lag_ev_n[64];               // ... and how many
    int lag_head, lag_count;
    uint64_t lag_last_ns;
    bool drops_logged;              // a dropping episode is in the log
    uint64_t drops_since_ns;
    uint64_t drops_card_at_start, drops_obs_at_start;
};

// --- Utility functions ---

// Cross-thread counter for the debug frame dump. std::atomic_ref keeps the
// struct plain C-style (bzalloc'd, no constructors) and avoids libobs'
// util/threading.h, which on Windows pulls in pthread.h that the minimal
// CI SDK does not ship.
static inline long dump_remaining_load(struct fps_analyzer_filter *f) {
    return std::atomic_ref<long>(f->dump_remaining).load();
}
static inline void dump_remaining_store(struct fps_analyzer_filter *f, long v) {
    std::atomic_ref<long>(f->dump_remaining).store(v);
}
static inline long dump_remaining_dec(struct fps_analyzer_filter *f) {
    return std::atomic_ref<long>(f->dump_remaining).fetch_sub(1) - 1;
}

// The trace hotkey fires on the UI thread; the video thread picks the request up.
static inline void trace_toggle_post(struct fps_analyzer_filter *f) {
    std::atomic_ref<long>(f->trace_toggle_req).store(1);
}
static inline bool trace_toggle_take(struct fps_analyzer_filter *f) {
    return std::atomic_ref<long>(f->trace_toggle_req).exchange(0) != 0;
}

static void ensure_luma_buffer(struct fps_analyzer_filter *filter, size_t needed) {
    if (filter->luma_buffer_size >= needed)
        return;
    if (filter->luma_buffer) bfree(filter->luma_buffer);
    filter->luma_buffer = (uint8_t *)bzalloc(needed);
    filter->luma_buffer_size = needed;
}

// --- fps-core adapter ---

static int core_pixfmt(enum video_format f) {
    switch (f) {
    case VIDEO_FORMAT_NV12: return FPS_PIXFMT_NV12;
    case VIDEO_FORMAT_I420: return FPS_PIXFMT_I420;
    case VIDEO_FORMAT_I444: return FPS_PIXFMT_I444;
    case VIDEO_FORMAT_I422: return FPS_PIXFMT_I422;
    case VIDEO_FORMAT_YUY2: return FPS_PIXFMT_YUY2;
    case VIDEO_FORMAT_UYVY: return FPS_PIXFMT_UYVY;
    case VIDEO_FORMAT_BGRA:
    case VIDEO_FORMAT_BGRX: // XRGB capture: same bytes as BGRA, the 4th one is ignored
        return FPS_PIXFMT_BGRA;
    case VIDEO_FORMAT_RGBA: return FPS_PIXFMT_RGBA;
    default: return FPS_PIXFMT_UNSUPPORTED;
    }
}

static struct fps_core_params core_params(const struct fps_analyzer_filter *filter) {
    struct fps_core_params p;
    p.analyze_method = filter->analyze_method == ANALYZE_DIFF ? FPS_ANALYZE_DIFF : FPS_ANALYZE_LAST_LINE;
    p.sensitivity = filter->sensitivity;
    p.enable_tearing_detection = filter->enable_tearing_detection;
    p.tearing_sensitivity = filter->tearing_sensitivity;
    p.update_interval = filter->update_interval;
    p.measure_deltas = filter->core_measure_deltas;
    p.measure_histogram = filter->core_measure_histogram;
    p.beat_enabled = filter->core_beat_enabled;
    p.pacing_period_ns = filter->core_pacing_period_ns;
    p.detect_drops = filter->detect_drops;
    return p;
}

// A fresh core is the state a freshly created filter has: no reference frame,
// empty history, cold EMA, no previously published FPS.
static void core_recreate(struct fps_analyzer_filter *filter) {
    if (filter->core)
        fps_core_destroy(filter->core);
    struct fps_core_params p = core_params(filter);
    filter->core = fps_core_create(&p);
}

// --- Luma conversion helpers (ITU-R BT.601) ---

static void bgra_to_luma(const uint8_t *bgra, uint32_t linesize,
                          uint8_t *luma, uint32_t width, uint32_t height) {
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

static void rgba_to_luma(const uint8_t *rgba, uint32_t linesize,
                          uint8_t *luma, uint32_t width, uint32_t height) {
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

// Extract the full luma plane from an async frame. Returns false on
// unsupported format. `luma` must hold width*height bytes.
static bool extract_full_luma_async(struct obs_source_frame *frame, uint8_t *luma) {
    const uint32_t width = frame->width;
    const uint32_t height = frame->height;

    switch (frame->format) {
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
    case VIDEO_FORMAT_I444:
    case VIDEO_FORMAT_I422:
        for (uint32_t y = 0; y < height; ++y) {
            memcpy(luma + y * width,
                   frame->data[0] + y * frame->linesize[0], width);
        }
        return true;
    case VIDEO_FORMAT_YUY2:
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *src = frame->data[0] + y * frame->linesize[0];
            for (uint32_t x = 0; x < width; ++x) {
                luma[y * width + x] = src[x * 2];
            }
        }
        return true;
    case VIDEO_FORMAT_UYVY:
        for (uint32_t y = 0; y < height; ++y) {
            uint8_t *src = frame->data[0] + y * frame->linesize[0];
            for (uint32_t x = 0; x < width; ++x) {
                luma[y * width + x] = src[x * 2 + 1];
            }
        }
        return true;
    case VIDEO_FORMAT_BGRA:
    case VIDEO_FORMAT_BGRX:
        bgra_to_luma(frame->data[0], frame->linesize[0], luma, width, height);
        return true;
    case VIDEO_FORMAT_RGBA:
        rgba_to_luma(frame->data[0], frame->linesize[0], luma, width, height);
        return true;
    default:
        return false;
    }
}

// Extract a luma crop (x0, y0, cw x ch) from an async frame into `luma`
// (cw*ch bytes). Returns false on unsupported format.
static bool extract_crop_luma_async(struct obs_source_frame *frame, uint8_t *luma,
                                    uint32_t x0, uint32_t y0, uint32_t cw, uint32_t ch) {
    const uint32_t ls = frame->linesize[0];
    switch (frame->format) {
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
    case VIDEO_FORMAT_I444:
    case VIDEO_FORMAT_I422:
        for (uint32_t y = 0; y < ch; ++y)
            memcpy(luma + y * cw, frame->data[0] + (y0 + y) * ls + x0, cw);
        return true;
    case VIDEO_FORMAT_YUY2:
        for (uint32_t y = 0; y < ch; ++y) {
            const uint8_t *src = frame->data[0] + (y0 + y) * ls + x0 * 2;
            for (uint32_t x = 0; x < cw; ++x)
                luma[y * cw + x] = src[x * 2];
        }
        return true;
    case VIDEO_FORMAT_UYVY:
        for (uint32_t y = 0; y < ch; ++y) {
            const uint8_t *src = frame->data[0] + (y0 + y) * ls + x0 * 2;
            for (uint32_t x = 0; x < cw; ++x)
                luma[y * cw + x] = src[x * 2 + 1];
        }
        return true;
    case VIDEO_FORMAT_BGRA:
    case VIDEO_FORMAT_BGRX:
        bgra_to_luma(frame->data[0] + y0 * ls + x0 * 4, ls, luma, cw, ch);
        return true;
    case VIDEO_FORMAT_RGBA:
        rgba_to_luma(frame->data[0] + y0 * ls + x0 * 4, ls, luma, cw, ch);
        return true;
    default:
        return false;
    }
}

// --- Fast spectrum path helpers (30/60 FPS center crop) ---

static bool fast_spectrum_due(struct fps_analyzer_filter *filter, uint64_t now) {
    if (!filter->enable_resolution_detection || !filter->res_detector ||
        filter->spectrum_fps <= 0 || !filter->crop_buffer)
        return false;
    uint64_t interval = 1000000000ULL / (uint64_t)filter->spectrum_fps;
    // Accept slightly early so frame-timing jitter doesn't halve the rate
    return now - filter->last_spec_submit_ns >= interval - interval / 4;
}

static void fast_spectrum_crop(uint32_t w, uint32_t h,
                               uint32_t *x0, uint32_t *y0, uint32_t *cw, uint32_t *ch) {
    *cw = w < RESDET_FAST_CROP_W ? w : RESDET_FAST_CROP_W;
    *ch = h < RESDET_FAST_CROP_H ? h : RESDET_FAST_CROP_H;
    *x0 = (w - *cw) / 2;
    *y0 = (h - *ch) / 2;
}

// --- Debug: frame dump ---
// Writes the exact full-frame luma planes that go to the resolution
// detector (one per ~0.5 s) as PGM files plus frames.csv with the
// detection result at that moment. Test corpus for the offline harness.

static void read_debug_settings(struct fps_analyzer_filter *filter, obs_data_t *settings) {
    filter->debug_options = obs_data_get_bool(settings, "debug_options");
    const char *dd = obs_data_get_string(settings, "debug_dump_dir");
    snprintf(filter->dump_dir, sizeof(filter->dump_dir), "%s", dd ? dd : "");
    const char *dl = obs_data_get_string(settings, "debug_dump_label");
    snprintf(filter->dump_label, sizeof(filter->dump_label), "%s", dl ? dl : "");
    // keep the label safe as a folder name
    for (char *c = filter->dump_label; *c; ++c) {
        bool ok = (*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'z') ||
                  (*c >= 'A' && *c <= 'Z') || *c == '-' || *c == '_';
        if (!ok) *c = '_';
    }
    filter->dump_count = (int)obs_data_get_int(settings, "debug_dump_count");
    filter->dump_delay = (int)obs_data_get_int(settings, "debug_dump_delay");
    filter->trace_enabled = obs_data_get_bool(settings, "debug_fps_trace");
}

// Starts a dump session after `delay_sec`. Called from the UI thread
// (button) or the hotkey thread; the video thread does the writing.
static void debug_dump_request(struct fps_analyzer_filter *filter, int delay_sec) {
    if (dump_remaining_load(filter) > 0) {
        blog(LOG_INFO, "[FPS Analyzer] Frame dump already in progress");
        return;
    }
    if (delay_sec < 0) delay_sec = 0;
    long n = filter->dump_count > 0 ? filter->dump_count : 16;
    filter->dump_index = 0;
    filter->dump_done_frames = 0;
    filter->dump_not_before_ns = os_gettime_ns() + (uint64_t)delay_sec * 1000000000ULL;
    dump_remaining_store(filter,n);
    filter->dump_status_dirty = true;
    blog(LOG_INFO, "[FPS Analyzer] Frame dump requested: %ld frames, starting in %d s", n, delay_sec);
}

// "Reset graph history": the button and the hotkey bump a generation; every
// filter's video_tick sees the change and clears its history on the video
// thread, so nothing races with the analysis.
static std::atomic<uint32_t> g_history_reset_gen{0};
static obs_hotkey_id g_reset_hotkey = OBS_INVALID_HOTKEY_ID;

static void history_reset_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(id);
    UNUSED_PARAMETER(hotkey);
    if (pressed)
        g_history_reset_gen.fetch_add(1);
}

static bool history_reset_clicked(obs_properties_t *props, obs_property_t *property, void *data) {
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);
    UNUSED_PARAMETER(data);
    g_history_reset_gen.fetch_add(1);
    return false;
}

// Global (frontend) hotkey: OBS only lists hotkeys registered on regular
// sources in its settings, not on filters, so the hotkey is registered once
// per module and targets the most recently created filter instance.
static std::atomic<struct fps_analyzer_filter *> g_dump_hotkey_target{nullptr};
static obs_hotkey_id g_dump_hotkey = OBS_INVALID_HOTKEY_ID;

static void debug_dump_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(id);
    UNUSED_PARAMETER(hotkey);
    struct fps_analyzer_filter *filter = g_dump_hotkey_target.load();
    if (pressed && filter)
        debug_dump_request(filter, 0);
}

static obs_hotkey_id g_trace_hotkey = OBS_INVALID_HOTKEY_ID;

static void debug_trace_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed) {
    UNUSED_PARAMETER(data);
    UNUSED_PARAMETER(id);
    UNUSED_PARAMETER(hotkey);
    struct fps_analyzer_filter *filter = g_dump_hotkey_target.load();
    if (pressed && filter)
        trace_toggle_post(filter);
}

void fps_analyzer_register_hotkeys(void) {
    g_dump_hotkey = obs_hotkey_register_frontend("fps_analyzer.debug_dump",
                                                 "FPS Analyzer: dump frames (debug)",
                                                 debug_dump_hotkey, NULL);
    g_trace_hotkey = obs_hotkey_register_frontend("fps_analyzer.debug_fps_trace",
                                                  "FPS Analyzer: start/stop FPS trace (debug)",
                                                  debug_trace_hotkey, NULL);
    g_reset_hotkey = obs_hotkey_register_frontend("fps_analyzer.reset_history",
                                                  "FPS Analyzer: reset graph history",
                                                  history_reset_hotkey, NULL);
}

void fps_analyzer_unregister_hotkeys(void) {
    if (g_dump_hotkey != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(g_dump_hotkey);
        g_dump_hotkey = OBS_INVALID_HOTKEY_ID;
    }
    if (g_trace_hotkey != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(g_trace_hotkey);
        g_trace_hotkey = OBS_INVALID_HOTKEY_ID;
    }
    if (g_reset_hotkey != OBS_INVALID_HOTKEY_ID) {
        obs_hotkey_unregister(g_reset_hotkey);
        g_reset_hotkey = OBS_INVALID_HOTKEY_ID;
    }
}

static void debug_dump_finish(struct fps_analyzer_filter *filter) {
    if (filter->dump_csv) {
        fclose(filter->dump_csv);
        filter->dump_csv = NULL;
    }
    blog(LOG_INFO, "[FPS Analyzer] Frame dump finished: %d frames in %s",
         filter->dump_index, filter->dump_session_dir);
    filter->dump_done_frames = filter->dump_index;
    filter->dump_index = 0;
    dump_remaining_store(filter,0);
    filter->dump_status_dirty = true;
}

// Text for the status label in the properties dialog
static void debug_dump_status_text(struct fps_analyzer_filter *filter, char *buf, size_t size) {
    long remaining = dump_remaining_load(filter);
    uint64_t now = os_gettime_ns();
    if (remaining > 0) {
        if (now < filter->dump_not_before_ns) {
            uint64_t left = filter->dump_not_before_ns - now;
            snprintf(buf, size, "Dump status: starting in %d s...",
                     (int)((left + 999999999ULL) / 1000000000ULL));
        } else {
            snprintf(buf, size, "Dump status: writing frame %d/%d...",
                     filter->dump_index + 1, filter->dump_index + (int)remaining);
        }
    } else if (filter->dump_done_frames > 0) {
        snprintf(buf, size, "Dump status: complete - %d frames in %s",
                 filter->dump_done_frames, filter->dump_session_dir);
    } else {
        snprintf(buf, size, "Dump status: idle");
    }
}

// Text for the FPS trace status label
static void debug_trace_status_text(struct fps_analyzer_filter *filter, char *buf, size_t size) {
    uint64_t now = os_gettime_ns();
    if (filter->trace_enabled) {
        if (now < filter->trace_not_before_ns) {
            uint64_t left = filter->trace_not_before_ns - now;
            snprintf(buf, size, "Trace status: starting in %d s...",
                     (int)((left + 999999999ULL) / 1000000000ULL));
        } else if (filter->trace_file) {
            snprintf(buf, size, "Trace status: recording - %d frames in %s",
                     filter->trace_rows, filter->trace_path);
        } else {
            snprintf(buf, size, "Trace status: armed, waiting for a frame...");
        }
    } else if (filter->trace_done_rows > 0) {
        snprintf(buf, size, "Trace status: complete - %d frames in %s",
                 filter->trace_done_rows, filter->trace_done_path);
    } else {
        snprintf(buf, size, "Trace status: idle");
    }
}

static void debug_dump_frame(struct fps_analyzer_filter *filter, const uint8_t *luma,
                             uint32_t width, uint32_t height, int format, uint64_t now) {
    if (filter->dump_index == 0) {
        // New session folder: <dir>/<label>_<timestamp>/
        char stamp[32];
        time_t t = time(NULL);
        struct tm *tmv = localtime(&t);
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", tmv);
        const char *base = filter->dump_dir;
        char *fallback = NULL;
        if (!base[0]) {
            fallback = obs_module_get_config_path(obs_current_module(), "dumps");
            base = fallback;
        }
        snprintf(filter->dump_session_dir, sizeof(filter->dump_session_dir), "%s/%s%s%s",
                 base, filter->dump_label, filter->dump_label[0] ? "_" : "", stamp);
        if (fallback) bfree(fallback);
        os_mkdirs(filter->dump_session_dir);

        char csv_path[700];
        snprintf(csv_path, sizeof(csv_path), "%s/frames.csv", filter->dump_session_dir);
        filter->dump_csv = fopen(csv_path, "w");
        if (filter->dump_csv)
            fprintf(filter->dump_csv,
                    "index,t_ms,width,height,video_format,res_valid,src_w,src_h,conf_w,conf_h,status\n");
        filter->dump_start_ns = now;
        blog(LOG_INFO, "[FPS Analyzer] Frame dump started: %s", filter->dump_session_dir);
    }

    char path[700];
    snprintf(path, sizeof(path), "%s/frame_%04d.pgm", filter->dump_session_dir, filter->dump_index);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P5\n%u %u\n255\n", width, height);
        fwrite(luma, 1, (size_t)width * height, f);
        fclose(f);
    }
    if (filter->dump_csv)
        fprintf(filter->dump_csv, "%d,%.1f,%u,%u,%d,%d,%d,%d,%.3f,%.3f,%d\n",
                filter->dump_index, (now - filter->dump_start_ns) / 1000000.0,
                width, height, format, g_fps_shared.res_valid ? 1 : 0,
                g_fps_shared.res_src_w, g_fps_shared.res_src_h,
                g_fps_shared.res_conf_w, g_fps_shared.res_conf_h,
                g_fps_shared.res_status);
    filter->dump_index++;
    if (dump_remaining_dec(filter) <= 0)
        debug_dump_finish(filter);
}

// --- Debug: FPS trace recorder ---
// Format (fpstrace v4):
//   # fpstrace v4 ; plugin=... ; path=async|sync ; method=.. ; sens=.. ; tear=.. ; tsens=.. ; interval=.. ; w=.. ; h=.. ; fmt=..
//     ; obs_fps=num/den ; dev_interval=.. (the parent's "frame_interval" setting, 100 ns units; empty if none)
//     ; dev_format=.. (the parent's "video_format" setting, win-dshow VideoFormat; empty if none)
//     ; altered=0|1 (0.6.0+: the fresh core started with the altered-frame verdict carried over)
//   F,now_ns,luma_size,diff,percent,unique,tear,ft_ms,ema,fps_pf,pos,count,src_ts,d0,d1,d2,d3,d4,d5,d6,d7
//   T,now_ns,window,avg_ft,fps,frametime_ms,tearing,graph_count
//   K,now_ns,video_time_ns,frames
// diff/percent are empty when no comparison happened (first frame / size
// change); ft_ms/ema/fps_pf are empty when no frametime sample was written.
// src_ts is frame->timestamp (async path only, empty for sync sources).
// K is written on every OBS video tick: `frames` is how many frames reached
// filter_video since the previous tick. OBS calls an async filter only on a
// tick where it picked a new source frame, so frames=0 is a tick on which OBS
// repeated the previous frame. v2 lacked src_ts and K; replay ignores both.
// d0..d7 (v4) count the compared bytes by the size of their difference:
// |d| = 0, 1, 2, 3-4, 5-8, 9-16, 17-32, >32 (empty when nothing was compared).
// They tell a card that alters every frame (only tiny differences) from
// real new content.
// Doubles use %.17g so they round-trip exactly. All row kinds are written
// from the OBS video thread (async filters run inside the video tick).

static const char *trace_format_name(int fmt) {
    static char buf[16];
    switch (fmt) {
    case VIDEO_FORMAT_NV12: return "NV12";
    case VIDEO_FORMAT_I420: return "I420";
    case VIDEO_FORMAT_I444: return "I444";
    case VIDEO_FORMAT_I422: return "I422";
    case VIDEO_FORMAT_YUY2: return "YUY2";
    case VIDEO_FORMAT_UYVY: return "UYVY";
    case VIDEO_FORMAT_BGRA: return "BGRA";
    case VIDEO_FORMAT_BGRX: return "BGRX";
    // not analysable, named for the "unsupported format" message
    case VIDEO_FORMAT_YVYU: return "YVYU";
    case VIDEO_FORMAT_Y800: return "Y800";
    case VIDEO_FORMAT_BGR3: return "BGR3";
    case VIDEO_FORMAT_AYUV: return "AYUV";
    case VIDEO_FORMAT_I010: return "I010";
    case VIDEO_FORMAT_P010: return "P010";
    case VIDEO_FORMAT_RGBA: return "RGBA";
    default:
        snprintf(buf, sizeof(buf), "%d", fmt);
        return buf;
    }
}

static void trace_close(struct fps_analyzer_filter *filter) {
    if (!filter->trace_file)
        return;
    fclose(filter->trace_file);
    filter->trace_file = NULL;
    filter->trace_done_rows = filter->trace_rows;
    snprintf(filter->trace_done_path, sizeof(filter->trace_done_path), "%s", filter->trace_path);
    filter->trace_rows = 0;
    filter->dump_status_dirty = true;
    blog(LOG_INFO, "[FPS Analyzer] FPS trace finished: %d frames in %s",
         filter->trace_done_rows, filter->trace_path);
}

// Arms the recorder: the file opens on the first frame after `delay_sec`, so
// there is time to alt-tab into the game before anything is measured.
static void trace_arm(struct fps_analyzer_filter *filter, int delay_sec) {
    if (delay_sec < 0) delay_sec = 0;
    filter->trace_not_before_ns = os_gettime_ns() + (uint64_t)delay_sec * 1000000000ULL;
    filter->trace_rows = 0;
    filter->trace_done_rows = 0;
    filter->trace_done_path[0] = '\0';
    filter->dump_status_dirty = true;
}

// A trace is replayed into a freshly created analysis core, so the recording
// has to begin from the state a freshly created filter has. Without this the
// first rows carry a history built before the recording started (frametime_count
// already at 960, a warm EMA, a published FPS feeding the averaging window) that
// the replay cannot reconstruct, and the recorded ticks would never reproduce.
// Resetting here also means every trace exercises the warm-up path.
// The verdict that the device alters every frame is carried over (recorded in
// the header as altered=1, so the replay starts from it too): a fresh core would
// otherwise show that device's FPS for the ~3 s it takes to learn it again.
static void trace_reset_analysis(struct fps_analyzer_filter *filter) {
    const bool altered = fps_core_frames_altered(filter->core);
    core_recreate(filter);
    if (altered)
        fps_core_inherit_altered(filter->core);
    filter->trace_altered_start = altered;
    g_fps_shared.fps = 0;
    g_fps_shared.frametime_ms = 0.0;
    g_fps_shared.graph_count = 0;
}

static void trace_open(struct fps_analyzer_filter *filter) {
    char stamp[32];
    time_t t = time(NULL);
    struct tm *tmv = localtime(&t);
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", tmv);
    const char *base = filter->dump_dir;
    char *fallback = NULL;
    if (!base[0]) {
        fallback = obs_module_get_config_path(obs_current_module(), "dumps");
        base = fallback;
    }
    os_mkdirs(base);
    snprintf(filter->trace_path, sizeof(filter->trace_path), "%s/fpstrace_%s%s%s.csv",
             base, filter->dump_label, filter->dump_label[0] ? "_" : "", stamp);
    if (fallback) bfree(fallback);
    filter->trace_file = fopen(filter->trace_path, "w");
    if (!filter->trace_file) {
        blog(LOG_WARNING, "[FPS Analyzer] FPS trace: cannot open %s", filter->trace_path);
        filter->trace_enabled = false; // re-enabling in the properties retries
        return;
    }
    setvbuf(filter->trace_file, NULL, _IOFBF, 65536);
    filter->trace_header_pending = true;
    trace_reset_analysis(filter);
    blog(LOG_INFO, "[FPS Analyzer] FPS trace started: %s", filter->trace_path);
}

// Called by both capture paths right before the FPS analysis of a frame:
// opens the file lazily and (re)writes the header when the stream or the
// analysis parameters changed.
static void trace_note_frame(struct fps_analyzer_filter *filter, int path_kind,
                             uint32_t width, uint32_t height, int format) {
    if (!filter->trace_enabled)
        return;
    if (os_gettime_ns() < filter->trace_not_before_ns)
        return; // still counting down
    if (!filter->trace_file) {
        trace_open(filter);
        if (!filter->trace_file)
            return;
        filter->dump_status_dirty = true;
    }
    if (filter->trace_header_pending || filter->trace_kind != path_kind ||
        filter->trace_w != width || filter->trace_h != height || filter->trace_fmt != format) {
        filter->trace_kind = path_kind;
        filter->trace_w = width;
        filter->trace_h = height;
        filter->trace_fmt = format;
        filter->trace_header_pending = false;

        // What the source and OBS were set to: the capture device's own frame
        // interval (win-dshow "frame_interval", 100 ns units) and the OBS FPS.
        struct obs_video_info ovi = {};
        bool have_ovi = obs_get_video_info(&ovi);
        char dev_interval[32] = "";
        char dev_format[16] = "";
        obs_source_t *parent = obs_filter_get_parent(filter->context);
        obs_data_t *ps = parent ? obs_source_get_settings(parent) : NULL;
        if (ps) {
            if (obs_data_has_user_value(ps, "frame_interval") || obs_data_has_default_value(ps, "frame_interval"))
                snprintf(dev_interval, sizeof(dev_interval), "%lld",
                         (long long)obs_data_get_int(ps, "frame_interval"));
            if (obs_data_has_user_value(ps, "video_format") || obs_data_has_default_value(ps, "video_format"))
                snprintf(dev_format, sizeof(dev_format), "%lld", (long long)obs_data_get_int(ps, "video_format"));
            obs_data_release(ps);
        }

        fprintf(filter->trace_file,
                "# fpstrace v4 ; plugin=" FPS_TRACE_PLUGIN_VERSION " ; path=%s ; method=%d ; sens=%.17g ; "
                "tear=%d ; tsens=%.17g ; interval=%.17g ; w=%u ; h=%u ; fmt=%s ; obs_fps=%u/%u ; "
                "dev_interval=%s ; dev_format=%s ; altered=%d\n",
                path_kind == 2 ? "sync" : "async", (int)filter->analyze_method, filter->sensitivity,
                filter->enable_tearing_detection ? 1 : 0, filter->tearing_sensitivity,
                filter->update_interval, width, height, trace_format_name(format),
                have_ovi ? ovi.fps_num : 0, have_ovi ? ovi.fps_den : 0, dev_interval, dev_format,
                filter->trace_altered_start ? 1 : 0);
        fputs("# F,now_ns,luma_size,diff,percent,unique,tear,ft_ms,ema,fps_pf,pos,count,src_ts,"
              "d0,d1,d2,d3,d4,d5,d6,d7\n"
              "# T,now_ns,window,avg_ft,fps,frametime_ms,tearing,graph_count\n"
              "# K,now_ns,video_time_ns,frames\n",
              filter->trace_file);
    }
}

// One F row from what the core just did with the frame analysed at `now`.
static void trace_write_frame(struct fps_analyzer_filter *filter, uint64_t now) {
    struct fps_core_frame_debug d;
    fps_core_debug_last_frame(filter->core, &d);
    FILE *t = filter->trace_file;
    fprintf(t, "F,%llu,%zu,", (unsigned long long)now, d.luma_size);
    if (d.compared)
        fprintf(t, "%zu,%.17g,", d.diff, d.percent);
    else
        fputs(",,", t);
    fprintf(t, "%d,%d,", d.unique ? 1 : 0, d.tearing_flag ? 1 : 0);
    if (d.sample_written)
        fprintf(t, "%.17g,%.17g,%.17g,", d.ft_ms, d.ema, d.fps_pf);
    else
        fputs(",,,", t);
    fprintf(t, "%d,%d,", d.pos, d.count);
    if (filter->trace_src_ts)
        fprintf(t, "%llu", (unsigned long long)filter->trace_src_ts);
    if (d.deltas_valid)
        fprintf(t, ",%u,%u,%u,%u,%u,%u,%u,%u\n", d.deltas[0], d.deltas[1], d.deltas[2], d.deltas[3],
                d.deltas[4], d.deltas[5], d.deltas[6], d.deltas[7]);
    else
        fputs(",,,,,,,,\n", t);
    filter->trace_rows++;
}

// avg_ft and frametime_ms are the same value (the core publishes the average).
static void trace_write_tick(struct fps_analyzer_filter *filter, uint64_t now,
                             const struct fps_core_output *o) {
    fprintf(filter->trace_file, "T,%llu,%d,%.17g,%d,%.17g,%d,%d\n",
            (unsigned long long)now, o->window, o->frametime_ms, o->fps, o->frametime_ms,
            o->tearing_detected ? 1 : 0, o->graph_count);
}

// --- Frame analysis (both capture paths) ---

// Hands one frame to the core and records it in the trace. now is the
// analysis-time clock, the only timestamp that drives FPS.
static void analyze_frame(struct fps_analyzer_filter *filter, const struct fps_frame_view *view) {
    uint64_t now = os_gettime_ns();
    fps_core_feed(filter->core, view, now);
    if (filter->trace_file)
        trace_write_frame(filter, now);

    // A new game frame: one sample on the graphs' time axis
    struct fps_core_frame_debug d;
    fps_core_debug_last_frame(filter->core, &d);
    if (d.sample_written && g_fps_shared.blocked_reason == FPS_BLOCK_NONE) {
        struct plot_sample s;
        s.t_ms = (double)now / 1e6;
        s.ft_ms = d.ft_ms;
        s.ema_ms = d.ema;
        s.fps_pf = d.fps_pf;
        s.pacing_ms = d.pacing_ms;
        s.gap = d.pacing_gap;
        s.tearing = d.tearing_flag;
        plot_ring_push(&g_fps_shared.plot, &s);
    }
}

// --- Async source path (filter_video) ---

static struct obs_source_frame *fps_analyzer_filter_video(void *data,
                                                          struct obs_source_frame *frame)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (!frame || !frame->data[0])
        return frame;

    // Faktyczna częstotliwość źródła — znacznik czasu nadany przez sterownik
    // karty, niezależny od formatu pikseli (liczone także dla nieobsługiwanych)
    // (only while an overlay shows the Source refresh rate line)
    if (filter->core_beat_enabled)
        srate_push(filter->srate, frame->timestamp, os_gettime_ns());
    filter->frames_since_tick++;
    filter->trace_src_ts = frame->timestamp;

    // Log format changes for debugging
    if ((int)frame->format != filter->last_logged_format) {
        blog(LOG_INFO, "[FPS Analyzer] Video format: %d, resolution: %ux%u",
             (int)frame->format, frame->width, frame->height);
        filter->last_logged_format = (int)frame->format;
    }

    const uint32_t width = frame->width;
    const uint32_t height = frame->height;

    struct fps_frame_view view;
    view.format = core_pixfmt(frame->format);
    view.width = width;
    view.height = height;
    view.data = frame->data[0];
    view.linesize = frame->linesize[0];
    view.src_ts_ns = frame->timestamp; // the device's clock: exact refresh periods for frame pacing
    if (view.format == FPS_PIXFMT_UNSUPPORTED) {
        filter->unsupported_fmt = (int)frame->format;
        return frame;
    }
    filter->unsupported_fmt = -1;

    trace_note_frame(filter, 1, width, height, (int)frame->format);
    analyze_frame(filter, &view);

    // Detekcja rozdzielczości źródłowej (upscale) — pełna klatka co ~0.5s;
    // tym samym torem idzie debugowy zrzut klatek
    {
        uint64_t now = os_gettime_ns();
        bool detect = filter->enable_resolution_detection && filter->res_detector;
        bool dump = dump_remaining_load(filter) > 0 &&
                    now >= filter->dump_not_before_ns;
        if ((detect || dump) && now - filter->last_resdet_submit_ns >= 500000000ULL) {
            ensure_luma_buffer(filter, (size_t)width * height);
            uint8_t *luma = filter->luma_buffer;
            if (extract_full_luma_async(frame, luma)) {
                bool consumed = false;
                if (detect && resdet_submit(filter->res_detector, luma, width, height))
                    consumed = true;
                if (dump) {
                    debug_dump_frame(filter, luma, width, height, (int)frame->format, now);
                    consumed = true;
                }
                if (consumed)
                    filter->last_resdet_submit_ns = now;
            }
        }
        // Szybka ścieżka widma (30/60 FPS) — centralny wycinek na osobnym wątku
        if (fast_spectrum_due(filter, now)) {
            uint32_t x0, y0, cw, ch;
            fast_spectrum_crop(width, height, &x0, &y0, &cw, &ch);
            if (extract_crop_luma_async(frame, filter->crop_buffer, x0, y0, cw, ch) &&
                resdet_submit_spectrum(filter->res_detector, filter->crop_buffer, cw, ch))
                filter->last_spec_submit_ns = now;
        }
    }

    return frame;
}

// --- Sync source path (video_render) ---

static void fps_analyzer_video_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    obs_source_t *target = obs_filter_get_target(filter->context);
    obs_source_t *parent = obs_filter_get_parent(filter->context);

    if (!target || !parent) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // For async sources, filter_video already handles analysis — just pass through
    uint32_t parent_flags = obs_source_get_output_flags(parent);
    if (parent_flags & OBS_SOURCE_ASYNC) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);

    if (width == 0 || height == 0) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Lazy-init GPU resources
    if (!filter->texrender) {
        filter->texrender = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
    }
    if (!filter->stagesurface ||
        filter->staged_width != width || filter->staged_height != height) {
        if (filter->stagesurface)
            gs_stagesurface_destroy(filter->stagesurface);
        filter->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
        filter->staged_width = width;
        filter->staged_height = height;
    }

    // Render parent source into texrender
    gs_texrender_reset(filter->texrender);
    if (!gs_texrender_begin(filter->texrender, width, height)) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    struct vec4 clear_color;
    vec4_zero(&clear_color);
    gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
    gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
    obs_source_video_render(target);
    gs_texrender_end(filter->texrender);

    gs_texture_t *tex = gs_texrender_get_texture(filter->texrender);
    if (!tex) {
        obs_source_skip_video_filter(filter->context);
        return;
    }

    // Stage texture to CPU for analysis
    gs_stage_texture(filter->stagesurface, tex);
    uint8_t *video_data;
    uint32_t video_linesize;
    if (gs_stagesurface_map(filter->stagesurface, &video_data, &video_linesize)) {
        filter->trace_src_ts = 0; // sync sources carry no frame timestamps
        trace_note_frame(filter, 2, width, height, (int)VIDEO_FORMAT_BGRA);

        struct fps_frame_view view;
        view.format = FPS_PIXFMT_BGRA;
        view.width = width;
        view.height = height;
        view.data = video_data;
        view.linesize = video_linesize;
        view.src_ts_ns = 0; // no device timestamps: pacing from whole OBS frames
        analyze_frame(filter, &view);
        filter->unsupported_fmt = -1;

        // Detekcja rozdzielczości źródłowej (upscale) — pełna klatka co ~0.5s;
        // tym samym torem idzie debugowy zrzut klatek
        {
            uint64_t now = os_gettime_ns();
            bool detect = filter->enable_resolution_detection && filter->res_detector;
            bool dump = dump_remaining_load(filter) > 0 &&
                    now >= filter->dump_not_before_ns;
            if ((detect || dump) && now - filter->last_resdet_submit_ns >= 500000000ULL) {
                ensure_luma_buffer(filter, (size_t)width * height);
                bgra_to_luma(video_data, video_linesize,
                             filter->luma_buffer, width, height);
                bool consumed = false;
                if (detect && resdet_submit(filter->res_detector, filter->luma_buffer,
                                            width, height))
                    consumed = true;
                if (dump) {
                    debug_dump_frame(filter, filter->luma_buffer, width, height,
                                     (int)VIDEO_FORMAT_BGRA, now);
                    consumed = true;
                }
                if (consumed)
                    filter->last_resdet_submit_ns = now;
            }
            // Szybka ścieżka widma (30/60 FPS) — centralny wycinek z BGRA
            if (fast_spectrum_due(filter, now)) {
                uint32_t x0, y0, cw, ch;
                fast_spectrum_crop(width, height, &x0, &y0, &cw, &ch);
                bgra_to_luma(video_data + y0 * video_linesize + x0 * 4, video_linesize,
                             filter->crop_buffer, cw, ch);
                if (resdet_submit_spectrum(filter->res_detector, filter->crop_buffer, cw, ch))
                    filter->last_spec_submit_ns = now;
            }
        }

        gs_stagesurface_unmap(filter->stagesurface);
    }

    // Draw the rendered texture to output (passthrough)
    gs_effect_t *def_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    gs_eparam_t *image = gs_effect_get_param_by_name(def_effect, "image");
    gs_effect_set_texture(image, tex);
    while (gs_effect_loop(def_effect, "Draw")) {
        gs_draw_sprite(tex, 0, width, height);
    }
}

// --- Output (tick) ---

// Real source refresh rate vs OBS FPS — only async sources (capture cards,
// cameras) carry frame timestamps; sync sources report "not available"
// The capture device's own FPS setting (win-dshow "frame_interval", 100 ns
// units; -1 = "Match output FPS", 0 = "Highest"). Other sources: unknown.
static void read_device_rate(obs_source_t *parent, double obs_hz, double *hz, bool *matching)
{
    *hz = 0.0;
    *matching = false;
    obs_data_t *ps = parent ? obs_source_get_settings(parent) : NULL;
    if (!ps)
        return;
    if (obs_data_has_user_value(ps, "frame_interval") || obs_data_has_default_value(ps, "frame_interval")) {
        long long iv = obs_data_get_int(ps, "frame_interval");
        if (iv > 0) {
            *hz = 1e7 / (double)iv;
        } else if (iv == -1) {
            *matching = true;
            *hz = obs_hz;
        }
    }
    obs_data_release(ps);
}

// The capture device's "video_format" setting (win-dshow VideoFormat: 0 = any,
// 100s RGB, 200s planar YUV, 300s packed YUV, 400+ compressed). -1 = the
// source has no such setting.
static int read_device_format(obs_source_t *parent)
{
    int vf = -1;
    obs_data_t *ps = parent ? obs_source_get_settings(parent) : NULL;
    if (!ps)
        return vf;
    if (obs_data_has_user_value(ps, "video_format") || obs_data_has_default_value(ps, "video_format"))
        vf = (int)obs_data_get_int(ps, "video_format");
    obs_data_release(ps);
    return vf;
}

#define DSHOW_FORMAT_COMPRESSED 400 // MJPEG = 400, H264, HEVC

static const char *compressed_format_name(int vf)
{
    switch (vf) {
    case 400: return "MJPEG";
    case 401: return "H.264";
    case 402: return "HEVC";
    default: return "compressed";
    }
}

// An overlay showing the Source refresh rate line asks for it every tick;
// the timestamp check and the beat detector run only while it does.
static bool source_check_wanted(uint64_t now)
{
    uint64_t t = g_fps_shared.srate_wanted_ns;
    return t && now >= t && now - t < 1000000000ULL;
}

// Keeps the core's optional work in step with what is needed: the |d|
// histogram for the trace and the reliability check, the beat detector for
// the overlay. Turning the beat back on starts its signal check over.
static void core_sync(struct fps_analyzer_filter *filter, uint64_t now)
{
    bool measure = filter->trace_file != NULL || filter->block_unreliable;
    bool histogram = filter->trace_file != NULL; // the full |d| histogram only for the trace
    bool beat = source_check_wanted(now);
    uint64_t period = 0;
    struct obs_video_info ovi;
    if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
        period = (uint64_t)ovi.fps_den * 1000000000ULL / ovi.fps_num;
    if (measure == filter->core_measure_deltas && histogram == filter->core_measure_histogram &&
        beat == filter->core_beat_enabled && period == filter->core_pacing_period_ns)
        return;
    filter->core_measure_histogram = histogram;
    filter->core_pacing_period_ns = period;
    bool beat_changed = beat != filter->core_beat_enabled;
    filter->core_measure_deltas = measure;
    filter->core_beat_enabled = beat;
    struct fps_core_params p = core_params(filter);
    fps_core_set_params(filter->core, &p);
    if (beat_changed) {
        if (beat)
            fps_core_beat_reset(filter->core, now);
        blog(LOG_INFO, "[FPS Analyzer] Source refresh rate check %s",
             beat ? "on (shown in the overlay)" : "off (not shown in any overlay)");
    }
}

// Who drops frames: the core's attribution from the device timestamps, plus
// OBS's own render-lag counter (the only source on the sync path, where there
// are no device timestamps). Published for the overlay's warning; the FPS
// itself is not corrected.
static void publish_drops(struct fps_analyzer_filter *filter, uint64_t now)
{
    uint32_t lagged = obs_get_lagged_frames();
    if (!filter->detect_drops) {
        filter->lagged_seen = lagged;
        g_fps_shared.drops_active = false;
        g_fps_shared.drops_card_per_s = g_fps_shared.drops_obs_per_s = 0.0;
        return;
    }
    if (lagged > filter->lagged_seen) {
        filter->lag_ev_t[filter->lag_head] = now;
        filter->lag_ev_n[filter->lag_head] = (int)(lagged - filter->lagged_seen);
        filter->lag_head = (filter->lag_head + 1) % 64;
        if (filter->lag_count < 64)
            filter->lag_count++;
        filter->lag_last_ns = now;
    }
    filter->lagged_seen = lagged;
    int lag = 0;
    for (int i = 0; i < filter->lag_count; i++) {
        int k = (filter->lag_head - 1 - i + 64) % 64;
        if (now - filter->lag_ev_t[k] > FPS_DROPS_WINDOW_NS)
            break;
        lag += filter->lag_ev_n[k];
    }
    const double lag_per_s = lag / ((double)FPS_DROPS_WINDOW_NS / 1e9);
    bool lag_active = filter->lag_last_ns && now - filter->lag_last_ns < FPS_DROPS_HOLD_NS;

    struct fps_drops d;
    fps_core_get_drops(filter->core, now, &d);
    // a render lag shows in both counters: take the larger, never the sum
    double obs_per_s = d.obs_per_s > lag_per_s ? d.obs_per_s : lag_per_s;
    bool active = d.active || lag_active;
    g_fps_shared.drops_active = active;
    g_fps_shared.drops_card_per_s = d.card_per_s;
    g_fps_shared.drops_obs_per_s = obs_per_s;

    if (active && !filter->drops_logged) {
        filter->drops_logged = true;
        filter->drops_since_ns = now;
        filter->drops_card_at_start = d.card_total;
        filter->drops_obs_at_start = d.obs_total;
        blog(LOG_WARNING, "[FPS Analyzer] Dropped frames: capture device %.1f/s, OBS %.1f/s - the FPS reads low",
             d.card_per_s, obs_per_s);
    } else if (!active && filter->drops_logged) {
        filter->drops_logged = false;
        blog(LOG_INFO, "[FPS Analyzer] Dropped frames stopped after %.0f s: capture device %llu, OBS %llu frames",
             (double)(now - filter->drops_since_ns) / 1e9,
             (unsigned long long)(d.card_total - filter->drops_card_at_start),
             (unsigned long long)(d.obs_total - filter->drops_obs_at_start));
    }
}

// Can the FPS be measured exactly? Publishes the reason when it cannot.
static void update_block(struct fps_analyzer_filter *filter, obs_source_t *parent)
{
    int reason = FPS_BLOCK_NONE;
    const char *detail = "";
    if (filter->unsupported_fmt >= 0) {
        reason = FPS_BLOCK_PIXFMT;
        detail = trace_format_name(filter->unsupported_fmt);
    } else if (filter->block_unreliable) {
        int vf = read_device_format(parent);
        if (vf >= DSHOW_FORMAT_COMPRESSED) {
            reason = FPS_BLOCK_COMPRESSED;
            detail = compressed_format_name(vf);
        } else if (fps_core_frames_altered(filter->core)) {
            reason = FPS_BLOCK_ALTERED;
        }
    }
    g_fps_shared.blocked_reason = reason;
    snprintf(g_fps_shared.blocked_detail, sizeof(g_fps_shared.blocked_detail), "%s", detail);
    g_fps_shared.reliability_off = !filter->block_unreliable;

    if (reason != filter->blocked_reason) {
        if (reason == FPS_BLOCK_PIXFMT)
            blog(LOG_WARNING, "[FPS Analyzer] FPS hidden: unsupported video format %s", detail);
        else if (reason == FPS_BLOCK_COMPRESSED)
            blog(LOG_WARNING, "[FPS Analyzer] FPS hidden: the capture device sends %s, a compressed format "
                 "whose repeated frames are not identical; set its Video Format to NV12 or YUY2", detail);
        else if (reason == FPS_BLOCK_ALTERED)
            blog(LOG_WARNING, "[FPS Analyzer] FPS hidden: the capture device alters every frame "
                 "(repeated frames are never identical)");
        else
            blog(LOG_INFO, "[FPS Analyzer] FPS measurement possible again");
        filter->blocked_reason = reason;
    }
}

// Beat verdict from the frame cadence (fps-core). Works on both paths; on a
// capture card it is the one that survives a device whose timestamps only
// echo its configured rate.
static void publish_beat(struct fps_analyzer_filter *filter, obs_source_t *parent, uint64_t now, double obs_hz)
{
    // A changed OBS FPS or device FPS is a different pipeline: check it anew.
    double device_hz;
    bool matching;
    read_device_rate(parent, obs_hz, &device_hz, &matching);
    if (filter->beat_seen_rates &&
        (fabs(obs_hz - filter->beat_obs_hz) > 1e-6 || fabs(device_hz - filter->beat_device_hz) > 1e-6)) {
        fps_core_beat_reset(filter->core, now);
        blog(LOG_INFO, "[FPS Analyzer] Signal check restarted: OBS %.3f FPS, device %.3f%s", obs_hz, device_hz,
             matching ? " (match output)" : "");
    }
    filter->beat_seen_rates = true;
    filter->beat_obs_hz = obs_hz;
    filter->beat_device_hz = device_hz;
    g_fps_shared.device_hz = device_hz;
    g_fps_shared.device_matching = matching;

    struct fps_beat b;
    fps_core_get_beat(filter->core, now, obs_hz, &b);
    g_fps_shared.beat_status = b.status;
    g_fps_shared.beat_observed_s = b.observed_s;
    g_fps_shared.beat_verify_s = b.verify_s;
    g_fps_shared.beat_uneven = b.uneven;
    g_fps_shared.beat_detected = b.detected;
    g_fps_shared.beat_origin = b.origin;
    g_fps_shared.beat_period_s = b.period_s;
    g_fps_shared.beat_signal_hz = b.nominal_hz > 0.0 ? b.nominal_hz : b.signal_hz;
    g_fps_shared.beat_drift = b.detected && b.nominal_hz > 0.0 && obs_hz > 0.0 &&
                              fabs(b.nominal_hz / obs_hz - 1.0) < 1e-9;

    if (b.status == FPS_BEAT_CLEAR && !filter->beat_clear_logged) {
        filter->beat_clear_logged = true;
        blog(LOG_INFO, "[FPS Analyzer] Signal check: no capture repeats in %.0f s - OK", b.observed_s);
    } else if (b.status != FPS_BEAT_CLEAR) {
        filter->beat_clear_logged = false;
    }

    if (b.detected && !filter->beat_logged) {
        filter->beat_logged = true;
        blog(LOG_INFO, "[FPS Analyzer] Capture beat: a frame repeats every %.2f s (%s), signal ~%.3f Hz "
             "(snapped %.3f) vs OBS %.3f FPS, device setting %.3f%s",
             b.period_s, b.origin == FPS_BEAT_ORIGIN_OBS ? "OBS" : "capture device", b.signal_hz, b.nominal_hz,
             obs_hz, g_fps_shared.device_hz, g_fps_shared.device_matching ? " (match output)" : "");
    } else if (!b.detected && filter->beat_logged) {
        filter->beat_logged = false;
        blog(LOG_INFO, "[FPS Analyzer] Capture beat: gone");
    }
}

static void publish_source_rate(struct fps_analyzer_filter *filter, uint64_t now)
{
    obs_source_t *parent = obs_filter_get_parent(filter->context);
    bool async = parent && (obs_source_get_output_flags(parent) & OBS_SOURCE_ASYNC);

    struct obs_video_info ovi;
    double obs_hz = 0.0;
    if (obs_get_video_info(&ovi) && ovi.fps_den > 0)
        obs_hz = (double)ovi.fps_num / ovi.fps_den;
    g_fps_shared.srate_obs_hz = obs_hz;
    if (!filter->core_beat_enabled) {
        // Nobody shows the line: neither check runs (core_sync)
        g_fps_shared.srate_available = false;
        return;
    }
    publish_beat(filter, parent, now, obs_hz);

    g_fps_shared.srate_available = async && filter->srate;
    if (!g_fps_shared.srate_available)
        return;

    // No frames for a while (source hidden, signal lost): start over
    srate_expire(filter->srate, now, 1000000000ULL);

    struct srate_result r;
    srate_get(filter->srate, obs_hz, &r);
    g_fps_shared.srate_valid = r.valid;
    g_fps_shared.srate_status = r.status;
    g_fps_shared.srate_source_hz = r.source_hz;
    g_fps_shared.srate_beat_s = r.beat_period_s;
    g_fps_shared.srate_recommended_hz = r.recommended_hz;

    // Still no verdict: every 5 s, why the window does not fill up
    if (!r.valid && now - filter->srate_diag_log_ns >= 5000000000ULL) {
        filter->srate_diag_log_ns = now;
        struct srate_diag d;
        srate_take_diag(filter->srate, &d);
        blog(LOG_INFO, "[FPS Analyzer] Source rate status=%d: %d frames in %.2f s "
             "(median %.3f ms, min %.3f, max %.3f, off-grid %d, rms %.3f ms) | "
             "last 5 s: pushed %d, ignored %d (back up to %.3f ms), gap resets %d "
             "(max %.1f ms), trims %d, idle resets %d",
             r.status, d.count, d.span_s, d.period_ms, d.min_ms, d.max_ms,
             d.fractional, d.rms_ms, d.pushes, d.ignored, d.max_back_ms,
             d.gap_resets, d.max_gap_ms, d.trims, d.expires);
    }

    // One log line per verdict change — useful in user-submitted logs
    if (r.valid && r.status != filter->srate_logged_status) {
        filter->srate_logged_status = r.status;
        if (r.status == SRATE_STATUS_MISMATCH)
            blog(LOG_INFO, "[FPS Analyzer] Source %.3f Hz vs OBS %.3f FPS: mismatch, "
                 "duplicate/skip every %.1f s (set OBS FPS to %.3f)",
                 r.source_hz, obs_hz, r.beat_period_s, r.recommended_hz);
        else if (r.status == SRATE_STATUS_MATCHED)
            blog(LOG_INFO, "[FPS Analyzer] Source %.3f Hz matches OBS %.3f FPS",
                 r.source_hz, obs_hz);
    }
}

static void fps_analyzer_video_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    uint64_t now = os_gettime_ns();
    core_sync(filter, now);

    // "Reset graph history" (button / hotkey)
    uint32_t gen = g_history_reset_gen.load();
    if (gen != filter->history_gen_seen) {
        filter->history_gen_seen = gen;
        fps_core_clear_history(filter->core);
        plot_ring_clear(&g_fps_shared.plot);
        g_fps_shared.graph_count = 0;
        g_fps_shared.fps = 0;
        g_fps_shared.frametime_ms = 0.0;
        blog(LOG_INFO, "[FPS Analyzer] Graph history reset");
    }

    // Per OBS tick on an async source: how many source frames OBS handed to
    // the filter since the previous tick (0 = OBS repeated the previous
    // frame). Feeds the beat detector and the trace's K rows.
    obs_source_t *parent = obs_filter_get_parent(filter->context);
    update_block(filter, parent);
    publish_drops(filter, now);
    if (parent && (obs_source_get_output_flags(parent) & OBS_SOURCE_ASYNC)) {
        const uint64_t video_time = obs_get_video_frame_time();
        fps_core_obs_tick_at(filter->core, now, filter->frames_since_tick, video_time);
        if (filter->trace_file)
            fprintf(filter->trace_file, "K,%llu,%llu,%d\n", (unsigned long long)now,
                    (unsigned long long)video_time, filter->frames_since_tick);
    }
    filter->frames_since_tick = 0;

    // FPS trace hotkey: starts without the countdown (the key is pressed from
    // inside the game already) and stops immediately. The checkbox follows, so
    // the properties dialog shows the real state.
    if (trace_toggle_take(filter)) {
        if (filter->trace_enabled) {
            filter->trace_enabled = false;
            trace_close(filter);
        } else {
            filter->trace_enabled = true;
            trace_arm(filter, 0);
        }
        obs_data_t *settings = obs_source_get_settings(filter->context);
        if (settings) {
            obs_data_set_bool(settings, "debug_fps_trace", filter->trace_enabled);
            obs_data_release(settings);
        }
        filter->dump_status_dirty = true;
        blog(LOG_INFO, "[FPS Analyzer] FPS trace hotkey: %s",
             filter->trace_enabled ? "started" : "stopped");
    }

    // Debug dump and FPS trace: keep the properties dialog's status labels
    // fresh — once per second while a session is counting down or running, and
    // once when it finishes. (OBS rebuilds the dialog on update_properties.)
    if (filter->debug_options) {
        bool active = dump_remaining_load(filter) > 0 || filter->trace_enabled;
        if (filter->dump_status_dirty ||
            (active && now - filter->dump_status_refresh_ns >= 1000000000ULL)) {
            filter->dump_status_dirty = false;
            filter->dump_status_refresh_ns = now;
            obs_source_update_properties(filter->context);
        }
    }

    // Spectrum thumbnail — polled every tick (not gated by update_interval)
    // so the 30/60 FPS fast path reaches the overlay at full rate
    if (filter->enable_resolution_detection && filter->res_detector &&
        resdet_get_spectrum(filter->res_detector, g_fps_shared.res_spectrum))
        g_fps_shared.res_spectrum_version++;

    // Update-interval gate, stale reset, averaging window and graph: fps-core
    struct fps_core_output *out = &filter->core_out;
    if (!fps_core_tick(filter->core, now, out))
        return;
    const int fps_smooth = out->fps;
    const double frametime_ms = out->frametime_ms;
    if (filter->trace_file)
        trace_write_tick(filter, now, out);

    // FPS that cannot be measured exactly is not published at all: the
    // overlay shows the reason (update_block) and the CSV gets nothing.
    const bool blocked = g_fps_shared.blocked_reason != FPS_BLOCK_NONE;
    if (blocked) {
        g_fps_shared.fps = 0;
        g_fps_shared.frametime_ms = 0.0;
        g_fps_shared.tearing_detected = false;
        g_fps_shared.graph_count = 0;
        plot_ring_clear(&g_fps_shared.plot);
        g_fps_shared.last_update_ns = now;
        publish_source_rate(filter, now);
        return;
    }

    // Update shared data for overlay source
    g_fps_shared.fps = fps_smooth;
    g_fps_shared.frametime_ms = frametime_ms;
    g_fps_shared.tearing_detected = out->tearing_detected;
    g_fps_shared.last_update_ns = now;

    // Graph arrays, oldest -> newest
    int count = out->graph_count;
    if (count > FPS_GRAPH_HISTORY) count = FPS_GRAPH_HISTORY;
    memcpy(g_fps_shared.graph_frametimes, out->graph_frametimes, sizeof(double) * (size_t)count);
    memcpy(g_fps_shared.graph_frametimes_raw, out->graph_frametimes_raw, sizeof(double) * (size_t)count);
    memcpy(g_fps_shared.graph_fps, out->graph_fps, sizeof(double) * (size_t)count);
    memcpy(g_fps_shared.graph_tearing, out->graph_tearing, sizeof(bool) * (size_t)count);
    memcpy(g_fps_shared.graph_pacing, out->graph_pacing, sizeof(double) * (size_t)count);
    memcpy(g_fps_shared.graph_pacing_gap, out->graph_pacing_gap, sizeof(bool) * (size_t)count);
    g_fps_shared.graph_count = count;

    publish_source_rate(filter, now);

    // Upscale resolution detection — publish latest result
    g_fps_shared.res_detect_enabled = filter->enable_resolution_detection;
    if (filter->enable_resolution_detection && filter->res_detector) {
        struct resdet_result r;
        if (resdet_get_result(filter->res_detector, &r) && r.valid) {
            g_fps_shared.res_valid = true;
            g_fps_shared.res_frame_w = r.frame_w;
            g_fps_shared.res_frame_h = r.frame_h;
            g_fps_shared.res_src_w = r.src_w;
            g_fps_shared.res_src_h = r.src_h;
            g_fps_shared.res_conf_w = r.conf_w;
            g_fps_shared.res_conf_h = r.conf_h;
            g_fps_shared.res_status = r.status;
        }
    } else {
        g_fps_shared.res_valid = false;
    }

    // Optional CSV logging
    if (filter->enable_csv) {
        char csv_path[512];
        build_csv_path(filter->output_path, csv_path, sizeof(csv_path));
        FILE *csv = fopen(csv_path, "a");
        if (csv) {
            time_t t = time(NULL);
            char line[96];
            fps_core_format_csv_line(line, sizeof(line), (long long)t, fps_smooth, frametime_ms);
            fputs(line, csv);
            fclose(csv);
        }
        fps_core_csv_keep_last_n_lines(csv_path, FPS_CSV_HISTORY_LIMIT);
    }
}

// --- Lifecycle ---

static void fps_analyzer_destroy(void *data)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (filter) {
        g_fps_shared.active_filter_count--;
        if (filter->core) fps_core_destroy(filter->core);
        if (filter->luma_buffer) bfree(filter->luma_buffer);
        if (filter->crop_buffer) bfree(filter->crop_buffer);
        if (filter->dump_csv) fclose(filter->dump_csv);
        trace_close(filter);
        struct fps_analyzer_filter *expected = filter;
        g_dump_hotkey_target.compare_exchange_strong(expected, nullptr);
        if (filter->res_detector) resdet_destroy(filter->res_detector);
        srate_destroy(filter->srate);
        obs_enter_graphics();
        if (filter->texrender) gs_texrender_destroy(filter->texrender);
        if (filter->stagesurface) gs_stagesurface_destroy(filter->stagesurface);
        obs_leave_graphics();
    }
    bfree(data);
}

static void *fps_analyzer_create(obs_data_t *settings, obs_source_t *context)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)bzalloc(sizeof(struct fps_analyzer_filter));
    filter->context = context;
    filter->output_path[0] = '\0';
    filter->update_interval = 1.0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
    }
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->analyze_method = (analyze_method_t)obs_data_get_int(settings, "analyze_method");
    filter->sensitivity = obs_data_get_double(settings, "sensitivity");
    filter->enable_tearing_detection = obs_data_get_bool(settings, "enable_tearing_detection");
    filter->tearing_sensitivity = obs_data_get_double(settings, "tearing_sensitivity");
    filter->block_unreliable = obs_data_get_bool(settings, "block_unreliable");
    filter->detect_drops = obs_data_get_bool(settings, "detect_drops");
    filter->unsupported_fmt = -1;
    filter->history_gen_seen = g_history_reset_gen.load(); // only resets from now on
    filter->lagged_seen = obs_get_lagged_frames(); // only lag from now on counts
    filter->core_measure_deltas = filter->block_unreliable;
    filter->core_beat_enabled = false; // until an overlay asks for it (core_sync)
    filter->core = NULL;
    core_recreate(filter);
    // Dynamic luma buffer
    filter->luma_buffer = NULL;
    filter->luma_buffer_size = 0;
    // GPU staging (lazy init in video_render)
    filter->texrender = NULL;
    filter->stagesurface = NULL;
    filter->staged_width = 0;
    filter->staged_height = 0;
    filter->last_logged_format = -1;
    // CSV logging
    filter->enable_csv = obs_data_get_bool(settings, "enable_csv");
    // Upscale resolution detection (worker threads created lazily on enable)
    filter->enable_resolution_detection = obs_data_get_bool(settings, "enable_resolution_detection");
    filter->res_detector = NULL;
    filter->last_resdet_submit_ns = 0;
    filter->spectrum_fps = (int)obs_data_get_int(settings, "spectrum_fps");
    filter->last_spec_submit_ns = 0;
    filter->crop_buffer = (uint8_t *)bzalloc((size_t)RESDET_FAST_CROP_W * RESDET_FAST_CROP_H);
    if (filter->enable_resolution_detection) {
        filter->res_detector = resdet_create();
        resdet_set_fast_spectrum(filter->res_detector, filter->spectrum_fps > 0);
    }
    // Debug frame dump (session state starts idle); the global hotkey targets
    // the most recently created filter
    read_debug_settings(filter, settings);
    // A trace checkbox left ticked in the scene collection arms the recorder
    // with its countdown rather than recording from the first frame.
    if (filter->trace_enabled)
        trace_arm(filter, filter->dump_delay);
    filter->dump_remaining = 0;
    filter->dump_not_before_ns = 0;
    filter->dump_done_frames = 0;
    filter->dump_status_dirty = false;
    filter->dump_status_refresh_ns = 0;
    filter->dump_index = 0;
    filter->dump_csv = NULL;
    filter->srate = srate_create();
    filter->srate_logged_status = -1;
    filter->beat_logged = false;
    filter->srate_diag_log_ns = 0;
    g_dump_hotkey_target.store(filter);
    g_fps_shared.active_filter_count++;
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.6.0";
}

// --- Properties ---

static bool analyze_method_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);
    int method = (int)obs_data_get_int(settings, "analyze_method");
    obs_property_t *slider = obs_properties_get(props, "sensitivity");
    obs_property_set_visible(slider, method == ANALYZE_DIFF || method == ANALYZE_LAST_LINE);
    return true;
}

static bool enable_csv_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);
    bool csv_on = obs_data_get_bool(settings, "enable_csv");
    obs_property_set_visible(obs_properties_get(props, "output_path"), csv_on);
    obs_property_set_visible(obs_properties_get(props, "clear_csv_on_start"), csv_on);
    return true;
}

static bool debug_options_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);
    bool on = obs_data_get_bool(settings, "debug_options");
    obs_property_set_visible(obs_properties_get(props, "debug_dump_dir"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_dump_label"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_dump_count"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_dump_delay"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_dump_now"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_dump_status"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_fps_trace"), on);
    obs_property_set_visible(obs_properties_get(props, "debug_fps_trace_status"), on);
    return true;
}

// "Dump frames now" button — runs on the UI thread; the video thread picks
// up dump_remaining and writes frames on its next detector submissions.
static bool debug_dump_button(obs_properties_t *props, obs_property_t *p, void *data)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(p);
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (filter)
        debug_dump_request(filter, filter->dump_delay);
    return false;
}

static bool resolution_detection_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);
    bool on = obs_data_get_bool(settings, "enable_resolution_detection");
    obs_property_set_visible(obs_properties_get(props, "spectrum_fps"), on);
    return true;
}

static obs_properties_t *fps_analyzer_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();

    // Reset graph history (also a hotkey: Settings -> Hotkeys, top section)
    obs_property_t *reset = obs_properties_add_button(props, "reset_history", "Reset graph history",
                                                      history_reset_clicked);
    obs_property_set_long_description(reset,
        "Clears the frame pacing, frametime and framerate graphs and the averages, "
        "e.g. before a new test run. The hotkey \"FPS Analyzer: reset graph history\" "
        "(Settings -> Hotkeys) does the same from inside the game.");

    // Sections: plain groups for what is always there, checkable groups (the
    // checkbox is the same setting as before, so existing filters keep their
    // state) for what can be turned off - unchecked, the section folds to its title.

    // --- Analysis ---
    obs_properties_t *an = obs_properties_create();
    obs_property_t *method = obs_properties_add_list(an, "analyze_method", "Analysis method",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(method, "Last line diff (pixel analysis)", ANALYZE_LAST_LINE);
    obs_property_list_add_int(method, "Full frame diff (all lines)", ANALYZE_DIFF);
    obs_property_set_modified_callback(method, analyze_method_modified);

    // Sensitivity threshold
    obs_property_t *slider = obs_properties_add_float_slider(an, "sensitivity", "Sensitivity threshold (%)", 0.0, 5.0, 0.1);
    int method_val = data ? ((struct fps_analyzer_filter*)data)->analyze_method : ANALYZE_LAST_LINE;
    obs_property_set_visible(slider, method_val == ANALYZE_DIFF || method_val == ANALYZE_LAST_LINE);

    // Update interval
    obs_property_t *interval = obs_properties_add_list(
        an, "update_interval", "Update interval (seconds)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(interval, "60 FPS (0.017s)", 1.0 / 60.0);
    obs_property_list_add_float(interval, "30 FPS (0.033s)", 1.0 / 30.0);
    obs_property_list_add_float(interval, "0.5s", 0.5);
    obs_property_list_add_float(interval, "1s", 1.0);
    obs_property_list_add_float(interval, "2s", 2.0);
    obs_properties_add_group(props, "group_analysis", "Analysis", OBS_GROUP_NORMAL, an);

    // Reliability check
    obs_properties_t *rel = obs_properties_create();
    obs_property_t *block = obs_properties_add_bool(rel, "block_unreliable",
                                                    "Block FPS when the capture is unreliable (recommended)");
    obs_property_set_long_description(block,
        "Hides the FPS and shows the reason instead when it cannot be measured exactly: "
        "a compressed capture format (MJPEG, H.264) or a capture device that alters every frame, "
        "so repeated frames are never identical. Turn off only for testing: the readings are then "
        "shown, but they can be wrong. See README: Supported capture devices.");
    obs_property_t *drops = obs_properties_add_bool(rel, "detect_drops",
                                                    "Warn when the capture device or OBS drops frames (recommended)");
    obs_property_set_long_description(drops,
        "A frame lost on the way to the analysis makes the FPS read low although the game "
        "did not lose it. The capture device's timestamps show such a gap; an OBS tick that "
        "got no frame right before it means the device lost it, none means OBS did "
        "(rendering lag). The overlay then says who drops frames and how many per second; "
        "the FPS stays visible but is not corrected. Off: nothing is measured.");
    obs_properties_add_group(props, "group_reliability", "Reliability", OBS_GROUP_NORMAL, rel);

    // Tearing detection
    obs_properties_t *tear = obs_properties_create();
    obs_properties_add_float_slider(tear, "tearing_sensitivity", "Sensitivity threshold (%)", 0.1, 10.0, 0.1);
    obs_properties_add_group(props, "enable_tearing_detection", "Tearing detection", OBS_GROUP_CHECKABLE, tear);

    // Upscale source resolution detection
    obs_properties_t *ups = obs_properties_create();
    obs_property_t *spec_fps = obs_properties_add_list(ups, "spectrum_fps",
        "Spectrum refresh rate", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_t *res_prop = obs_properties_add_group(
        props, "enable_resolution_detection", "Upscale source resolution detection (DCT)", OBS_GROUP_CHECKABLE, ups);
    obs_property_set_long_description(res_prop,
        "Estimates the internal render resolution the image was upscaled from "
        "using DCT spectral analysis (~1 analysis per second on a background thread). "
        "Works with traditional scaling (bilinear/bicubic/lanczos); does not detect "
        "AI upscalers like DLSS/FSR2+/PSSR.");
    obs_property_set_modified_callback(res_prop, resolution_detection_modified);

    obs_property_list_add_int(spec_fps, "Analysis rate (~2/s, lowest CPU)", 0);
    obs_property_list_add_int(spec_fps, "30 FPS (center crop, ~20% of one core)", 30);
    obs_property_list_add_int(spec_fps, "60 FPS (center crop, ~40% of one core)", 60);
    obs_property_set_long_description(spec_fps,
        "How often the overlay's spectrum panel refreshes. 30/60 FPS compute the "
        "spectrum from a 640x360 center crop on a second background thread; "
        "detection itself always runs on the full frame at the analysis rate.");
    obs_property_set_visible(spec_fps,
        data ? ((struct fps_analyzer_filter*)data)->enable_resolution_detection : false);

    // CSV logging
    obs_properties_t *csv = obs_properties_create();
    obs_property_t *path_prop = obs_properties_add_path(csv, "output_path", "CSV Output file",
                            OBS_PATH_FILE_SAVE, "CSV File (*.csv)", NULL);
    obs_property_t *clear_prop = obs_properties_add_bool(csv, "clear_csv_on_start", "Clear CSV file on start");
    obs_property_t *csv_toggle = obs_properties_add_group(props, "enable_csv", "CSV logging", OBS_GROUP_CHECKABLE, csv);
    obs_property_set_modified_callback(csv_toggle, enable_csv_modified);

    // Set initial CSV fields visibility
    bool csv_on = data ? ((struct fps_analyzer_filter*)data)->enable_csv : false;
    obs_property_set_visible(path_prop, csv_on);
    obs_property_set_visible(clear_prop, csv_on);

    // Debug options
    obs_properties_t *dbgp = obs_properties_create();
    obs_property_t *dump_dir = obs_properties_add_path(dbgp, "debug_dump_dir", "Frame dump folder",
                                                       OBS_PATH_DIRECTORY, NULL, NULL);
    obs_property_set_long_description(dump_dir,
        "Where frame dumps are written (one subfolder per dump: <label>_<timestamp>). "
        "Empty = the plugin's config folder.");
    obs_property_t *dump_label = obs_properties_add_text(dbgp, "debug_dump_label",
                                                         "Dump label (case name)", OBS_TEXT_DEFAULT);
    obs_property_t *dump_count = obs_properties_add_int(dbgp, "debug_dump_count",
                                                        "Frames per dump", 1, 200, 1);
    obs_property_t *dump_delay = obs_properties_add_int_slider(dbgp, "debug_dump_delay",
                                                               "Start delay (s)", 0, 30, 1);
    obs_property_set_long_description(dump_delay,
        "Countdown before recording actually starts - time to Alt+Tab back into the game "
        "and leave the pause menu. Applies both to \"Dump frames now\" and to ticking "
        "\"Record FPS trace\". The hotkeys (Settings -> Hotkeys, top section) skip the "
        "countdown and start immediately.");
    obs_property_t *dump_btn = obs_properties_add_button(dbgp, "debug_dump_now",
                                                         "Dump frames now", debug_dump_button);
    obs_property_set_long_description(dump_btn,
        "After the start delay, writes the next N full-frame luma planes fed to the "
        "resolution detector (one every 0.5 s) as PGM files, plus frames.csv with the "
        "detection result at each frame. Works with detection enabled or disabled.");
    char status[800] = "Dump status: idle";
    if (data)
        debug_dump_status_text((struct fps_analyzer_filter *)data, status, sizeof(status));
    obs_property_t *dump_status = obs_properties_add_text(dbgp, "debug_dump_status", status, OBS_TEXT_INFO);
    obs_property_t *trace_prop = obs_properties_add_bool(dbgp, "debug_fps_trace",
                                                         "Record FPS trace (per-frame CSV)");
    obs_property_set_long_description(trace_prop,
        "While enabled, writes fpstrace_<label>_<timestamp>.csv into the frame dump folder: "
        "one row per analysed frame (timestamp, diff, unique/tearing decision, frametime, EMA) "
        "and one per published tick (window, FPS, frametime). Replayed by the offline test "
        "tools to pin the FPS algorithm's behaviour. ~60 rows/s, a few KB per second. "
        "Recording begins after \"Start delay\", or immediately on the hotkey "
        "\"FPS Analyzer: start/stop FPS trace (debug)\", which also stops it.");
    char trace_status[800] = "Trace status: idle";
    if (data)
        debug_trace_status_text((struct fps_analyzer_filter *)data, trace_status, sizeof(trace_status));
    obs_property_t *trace_status_prop = obs_properties_add_text(dbgp, "debug_fps_trace_status",
                                                               trace_status, OBS_TEXT_INFO);
    obs_property_t *dbg = obs_properties_add_group(props, "debug_options", "Debug options", OBS_GROUP_CHECKABLE, dbgp);
    obs_property_set_modified_callback(dbg, debug_options_modified);
    bool dbg_on = data ? ((struct fps_analyzer_filter*)data)->debug_options : false;
    obs_property_set_visible(dump_dir, dbg_on);
    obs_property_set_visible(dump_label, dbg_on);
    obs_property_set_visible(dump_count, dbg_on);
    obs_property_set_visible(dump_delay, dbg_on);
    obs_property_set_visible(dump_btn, dbg_on);
    obs_property_set_visible(dump_status, dbg_on);
    obs_property_set_visible(trace_prop, dbg_on);
    obs_property_set_visible(trace_status_prop, dbg_on);

    return props;
}

static void fps_analyzer_update(void *data, obs_data_t *settings)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
    }
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    filter->enable_tearing_detection = obs_data_get_bool(settings, "enable_tearing_detection");
    filter->tearing_sensitivity = obs_data_get_double(settings, "tearing_sensitivity");
    filter->analyze_method = (analyze_method_t)obs_data_get_int(settings, "analyze_method");
    filter->sensitivity = obs_data_get_double(settings, "sensitivity");
    filter->block_unreliable = obs_data_get_bool(settings, "block_unreliable");
    filter->detect_drops = obs_data_get_bool(settings, "detect_drops");
    {
        struct fps_core_params p = core_params(filter);
        fps_core_set_params(filter->core, &p); // keeps the analysis state, as before
    }
    filter->enable_csv = obs_data_get_bool(settings, "enable_csv");
    filter->enable_resolution_detection = obs_data_get_bool(settings, "enable_resolution_detection");
    filter->spectrum_fps = (int)obs_data_get_int(settings, "spectrum_fps");
    if (filter->enable_resolution_detection && !filter->res_detector)
        filter->res_detector = resdet_create();
    if (filter->res_detector)
        resdet_set_fast_spectrum(filter->res_detector, filter->spectrum_fps > 0);
    const bool trace_was_enabled = filter->trace_enabled;
    read_debug_settings(filter, settings);
    // FPS trace: ticking the checkbox arms the recorder and starts the same
    // countdown the frame dump uses, so there is time to alt-tab into the game
    // before the first frame is measured. Unticking closes the file. A change
    // while it is already running only writes a fresh header.
    if (filter->trace_enabled && !trace_was_enabled)
        trace_arm(filter, filter->dump_delay);
    else if (filter->trace_enabled)
        filter->trace_header_pending = true;
    else
        trace_close(filter);
}

// --- File helpers ---

// Funkcja do budowania ścieżki pliku CSV
static void build_csv_path(const char *output_path, char *csv_path, size_t csv_path_size) {
    if (output_path[0]) {
        strncpy(csv_path, output_path, csv_path_size);
        csv_path[csv_path_size-1] = '\0';
        char *dot = strrchr(csv_path, '.');
        if (dot) {
            strcpy(dot, ".csv");
        } else {
            strcat(csv_path, ".csv");
        }
    } else {
        strcpy(csv_path, "fps.csv");
    }
}

// --- Defaults ---

static void fps_analyzer_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_double(settings, "update_interval", 1.0 / 30.0);
    obs_data_set_default_bool(settings, "enable_csv", false);
    obs_data_set_default_bool(settings, "clear_csv_on_start", true);
    obs_data_set_default_bool(settings, "enable_tearing_detection", true);
    obs_data_set_default_bool(settings, "block_unreliable", true);
    obs_data_set_default_bool(settings, "detect_drops", true);
    obs_data_set_default_double(settings, "tearing_sensitivity", 1.0);
    obs_data_set_default_int(settings, "analyze_method", ANALYZE_LAST_LINE);
    obs_data_set_default_double(settings, "sensitivity", 0.1);
    obs_data_set_default_bool(settings, "enable_resolution_detection", false);
    obs_data_set_default_int(settings, "spectrum_fps", 0);
    obs_data_set_default_bool(settings, "debug_options", false);
    obs_data_set_default_string(settings, "debug_dump_dir", "");
    obs_data_set_default_string(settings, "debug_dump_label", "");
    obs_data_set_default_int(settings, "debug_dump_count", 16);
    obs_data_set_default_int(settings, "debug_dump_delay", 5);
    obs_data_set_default_bool(settings, "debug_fps_trace", false);
}

// --- Source info ---

struct obs_source_info fps_analyzer_filter_info = {
    .id = "fps_analyzer_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = fps_analyzer_get_name,
    .create = fps_analyzer_create,
    .destroy = fps_analyzer_destroy,
    .get_defaults = fps_analyzer_get_defaults,
    .get_properties = fps_analyzer_properties,
    .update = fps_analyzer_update,
    .video_tick = fps_analyzer_video_tick,
    .video_render = fps_analyzer_video_render,
    .filter_video = fps_analyzer_filter_video,
};
