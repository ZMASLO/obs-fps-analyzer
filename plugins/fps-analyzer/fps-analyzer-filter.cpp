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
struct fps_shared_data g_fps_shared = {0, 0.0, false, 0, 0, -1, {}, 0};

// Declare overlay info for registration in overlay file
extern struct obs_source_info fps_overlay_source_info;

#define FPS_CSV_HISTORY_LIMIT 300
// Version stamped into FPS trace headers (see trace_note_frame)
#define FPS_TRACE_PLUGIN_VERSION "0.5.1"

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
    case VIDEO_FORMAT_BGRA: return FPS_PIXFMT_BGRA;
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
// Format (fpstrace v3):
//   # fpstrace v3 ; plugin=... ; path=async|sync ; method=.. ; sens=.. ; tear=.. ; tsens=.. ; interval=.. ; w=.. ; h=.. ; fmt=..
//     ; obs_fps=num/den ; dev_interval=.. (the parent's "frame_interval" setting, 100 ns units; empty if none)
//   F,now_ns,luma_size,diff,percent,unique,tear,ft_ms,ema,fps_pf,pos,count,src_ts
//   T,now_ns,window,avg_ft,fps,frametime_ms,tearing,graph_count
//   K,now_ns,video_time_ns,frames
// diff/percent are empty when no comparison happened (first frame / size
// change); ft_ms/ema/fps_pf are empty when no frametime sample was written.
// src_ts is frame->timestamp (async path only, empty for sync sources).
// K is written on every OBS video tick: `frames` is how many frames reached
// filter_video since the previous tick. OBS calls an async filter only on a
// tick where it picked a new source frame, so frames=0 is a tick on which OBS
// repeated the previous frame. v2 lacked src_ts and K; replay ignores both.
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
static void trace_reset_analysis(struct fps_analyzer_filter *filter) {
    core_recreate(filter);
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
        obs_source_t *parent = obs_filter_get_parent(filter->context);
        obs_data_t *ps = parent ? obs_source_get_settings(parent) : NULL;
        if (ps) {
            if (obs_data_has_user_value(ps, "frame_interval") || obs_data_has_default_value(ps, "frame_interval"))
                snprintf(dev_interval, sizeof(dev_interval), "%lld",
                         (long long)obs_data_get_int(ps, "frame_interval"));
            obs_data_release(ps);
        }

        fprintf(filter->trace_file,
                "# fpstrace v3 ; plugin=" FPS_TRACE_PLUGIN_VERSION " ; path=%s ; method=%d ; sens=%.17g ; "
                "tear=%d ; tsens=%.17g ; interval=%.17g ; w=%u ; h=%u ; fmt=%s ; obs_fps=%u/%u ; "
                "dev_interval=%s\n",
                path_kind == 2 ? "sync" : "async", (int)filter->analyze_method, filter->sensitivity,
                filter->enable_tearing_detection ? 1 : 0, filter->tearing_sensitivity,
                filter->update_interval, width, height, trace_format_name(format),
                have_ovi ? ovi.fps_num : 0, have_ovi ? ovi.fps_den : 0, dev_interval);
        fputs("# F,now_ns,luma_size,diff,percent,unique,tear,ft_ms,ema,fps_pf,pos,count,src_ts\n"
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
        fprintf(t, "%llu\n", (unsigned long long)filter->trace_src_ts);
    else
        fputc('\n', t);
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
    if (view.format == FPS_PIXFMT_UNSUPPORTED) {
        g_fps_shared.unsupported_format = (int)frame->format;
        return frame;
    }
    g_fps_shared.unsupported_format = -1;

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
        analyze_frame(filter, &view);
        g_fps_shared.unsupported_format = -1;

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

// Beat verdict from the frame cadence (fps-core). Works on both paths; on a
// capture card it is the one that survives a device whose timestamps only
// echo its configured rate.
static void publish_beat(struct fps_analyzer_filter *filter, obs_source_t *parent, uint64_t now, double obs_hz)
{
    struct fps_beat b;
    fps_core_get_beat(filter->core, now, obs_hz, &b);
    g_fps_shared.beat_detected = b.detected;
    g_fps_shared.beat_origin = b.origin;
    g_fps_shared.beat_period_s = b.period_s;
    g_fps_shared.beat_signal_hz = b.nominal_hz > 0.0 ? b.nominal_hz : b.signal_hz;
    g_fps_shared.beat_drift = b.detected && b.nominal_hz > 0.0 && obs_hz > 0.0 &&
                              fabs(b.nominal_hz / obs_hz - 1.0) < 1e-9;
    read_device_rate(parent, obs_hz, &g_fps_shared.device_hz, &g_fps_shared.device_matching);

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

    // Per OBS tick on an async source: how many source frames OBS handed to
    // the filter since the previous tick (0 = OBS repeated the previous
    // frame). Feeds the beat detector and the trace's K rows.
    obs_source_t *parent = obs_filter_get_parent(filter->context);
    if (parent && (obs_source_get_output_flags(parent) & OBS_SOURCE_ASYNC)) {
        fps_core_obs_tick(filter->core, now, filter->frames_since_tick);
        if (filter->trace_file)
            fprintf(filter->trace_file, "K,%llu,%llu,%d\n", (unsigned long long)now,
                    (unsigned long long)obs_get_video_frame_time(), filter->frames_since_tick);
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
    g_fps_shared.graph_count = count;

    if (filter->trace_file)
        trace_write_tick(filter, now, out);
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
    return "FPS Analyzer 0.5.1";
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

    // Analysis method
    obs_property_t *method = obs_properties_add_list(props, "analyze_method", "Analysis method",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(method, "Last line diff (pixel analysis)", ANALYZE_LAST_LINE);
    obs_property_list_add_int(method, "Full frame diff (all lines)", ANALYZE_DIFF);
    obs_property_set_modified_callback(method, analyze_method_modified);

    // Sensitivity threshold
    obs_property_t *slider = obs_properties_add_float_slider(props, "sensitivity", "Sensitivity threshold (%)", 0.0, 5.0, 0.1);
    int method_val = data ? ((struct fps_analyzer_filter*)data)->analyze_method : ANALYZE_LAST_LINE;
    obs_property_set_visible(slider, method_val == ANALYZE_DIFF || method_val == ANALYZE_LAST_LINE);

    // Update interval
    obs_property_t *interval = obs_properties_add_list(
        props, "update_interval", "Update interval (seconds)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(interval, "60 FPS (0.017s)", 1.0 / 60.0);
    obs_property_list_add_float(interval, "30 FPS (0.033s)", 1.0 / 30.0);
    obs_property_list_add_float(interval, "0.5s", 0.5);
    obs_property_list_add_float(interval, "1s", 1.0);
    obs_property_list_add_float(interval, "2s", 2.0);

    // Tearing detection
    obs_properties_add_bool(props, "enable_tearing_detection", "Tearing detection");
    obs_properties_add_float_slider(props, "tearing_sensitivity", "Tearing sensitivity threshold (%)", 0.1, 10.0, 0.1);

    // Upscale source resolution detection
    obs_property_t *res_prop = obs_properties_add_bool(
        props, "enable_resolution_detection", "Detect upscale source resolution (DCT)");
    obs_property_set_long_description(res_prop,
        "Estimates the internal render resolution the image was upscaled from "
        "using DCT spectral analysis (~1 analysis per second on a background thread). "
        "Works with traditional scaling (bilinear/bicubic/lanczos); does not detect "
        "AI upscalers like DLSS/FSR2+/PSSR.");
    obs_property_set_modified_callback(res_prop, resolution_detection_modified);

    obs_property_t *spec_fps = obs_properties_add_list(props, "spectrum_fps",
        "Spectrum refresh rate", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
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
    obs_property_t *csv_toggle = obs_properties_add_bool(props, "enable_csv", "Enable CSV logging");
    obs_property_set_modified_callback(csv_toggle, enable_csv_modified);

    obs_property_t *path_prop = obs_properties_add_path(props, "output_path", "CSV Output file",
                            OBS_PATH_FILE_SAVE, "CSV File (*.csv)", NULL);
    obs_property_t *clear_prop = obs_properties_add_bool(props, "clear_csv_on_start", "Clear CSV file on start");

    // Set initial CSV fields visibility
    bool csv_on = data ? ((struct fps_analyzer_filter*)data)->enable_csv : false;
    obs_property_set_visible(path_prop, csv_on);
    obs_property_set_visible(clear_prop, csv_on);

    // Debug options
    obs_property_t *dbg = obs_properties_add_bool(props, "debug_options", "Debug options");
    obs_property_set_modified_callback(dbg, debug_options_modified);
    obs_property_t *dump_dir = obs_properties_add_path(props, "debug_dump_dir", "Frame dump folder",
                                                       OBS_PATH_DIRECTORY, NULL, NULL);
    obs_property_set_long_description(dump_dir,
        "Where frame dumps are written (one subfolder per dump: <label>_<timestamp>). "
        "Empty = the plugin's config folder.");
    obs_property_t *dump_label = obs_properties_add_text(props, "debug_dump_label",
                                                         "Dump label (case name)", OBS_TEXT_DEFAULT);
    obs_property_t *dump_count = obs_properties_add_int(props, "debug_dump_count",
                                                        "Frames per dump", 1, 200, 1);
    obs_property_t *dump_delay = obs_properties_add_int_slider(props, "debug_dump_delay",
                                                               "Start delay (s)", 0, 30, 1);
    obs_property_set_long_description(dump_delay,
        "Countdown before recording actually starts - time to Alt+Tab back into the game "
        "and leave the pause menu. Applies both to \"Dump frames now\" and to ticking "
        "\"Record FPS trace\". The hotkeys (Settings -> Hotkeys, top section) skip the "
        "countdown and start immediately.");
    obs_property_t *dump_btn = obs_properties_add_button(props, "debug_dump_now",
                                                         "Dump frames now", debug_dump_button);
    obs_property_set_long_description(dump_btn,
        "After the start delay, writes the next N full-frame luma planes fed to the "
        "resolution detector (one every 0.5 s) as PGM files, plus frames.csv with the "
        "detection result at each frame. Works with detection enabled or disabled.");
    char status[800] = "Dump status: idle";
    if (data)
        debug_dump_status_text((struct fps_analyzer_filter *)data, status, sizeof(status));
    obs_property_t *dump_status = obs_properties_add_text(props, "debug_dump_status", status, OBS_TEXT_INFO);
    obs_property_t *trace_prop = obs_properties_add_bool(props, "debug_fps_trace",
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
    obs_property_t *trace_status_prop = obs_properties_add_text(props, "debug_fps_trace_status",
                                                               trace_status, OBS_TEXT_INFO);
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
