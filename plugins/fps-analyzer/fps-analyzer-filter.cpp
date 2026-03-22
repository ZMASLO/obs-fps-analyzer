#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

#include "fps-shared-data.h"

// Global shared data — read by fps-analyzer-overlay.cpp
struct fps_shared_data g_fps_shared = {0, 0.0, false, 0, 0, -1, {}, 0};

// Declare overlay info for registration in overlay file
extern struct obs_source_info fps_overlay_source_info;

#define FPS_CSV_HISTORY_LIMIT 300
#define ROLLING_MAX 120 // max 2 sekundy przy 60 FPS
#define FRAMETIME_HISTORY 960

// Dodaj enum do wyboru metody analizy
typedef enum {
    ANALYZE_LAST_LINE = 0,
    ANALYZE_DIFF = 1
} analyze_method_t;

// Prototypes
static void keep_last_n_lines(const char *csv_path, int n);
static void build_csv_path(const char *output_path, char *csv_path, size_t csv_path_size);

struct fps_analyzer_filter {
    obs_source_t *context;
    char output_path[512];
    double update_interval;
    uint64_t last_unique_frame_time;
    bool clear_csv_on_start;
    bool enable_csv;
    uint64_t rolling_times[ROLLING_MAX];
    int rolling_count;
    int rolling_start;
    uint64_t last_write_time;
    double frametime_history[FRAMETIME_HISTORY];
    int frametime_pos;
    int frametime_count;
    analyze_method_t analyze_method;
    double sensitivity;
    uint8_t *prev_frame;
    size_t prev_frame_size;
    int tearing_detected;
    bool enable_tearing_detection;
    double tearing_sensitivity;
    uint8_t *prev_lines[3];
    size_t prev_lines_size;
    int tearing_history[5];
    int tearing_history_pos;
    // Dynamic luma buffer (replaces static buffers)
    uint8_t *luma_buffer;
    size_t luma_buffer_size;
    // GPU staging resources for sync sources (video_render path)
    gs_texrender_t *texrender;
    gs_stagesurf_t *stagesurface;
    uint32_t staged_width;
    uint32_t staged_height;
    // Debug: log format once
    int last_logged_format;
    // Tearing history per-frame (aligned with frametime_history)
    bool tearing_per_frame[FRAMETIME_HISTORY];
    double fps_per_frame[FRAMETIME_HISTORY];
    double smoothed_frametime[FRAMETIME_HISTORY];
    double ema_frametime; // EMA state for frametime smoothing
};

// --- Utility functions ---

static void ensure_luma_buffer(struct fps_analyzer_filter *filter, size_t needed) {
    if (filter->luma_buffer_size >= needed)
        return;
    if (filter->luma_buffer) bfree(filter->luma_buffer);
    filter->luma_buffer = (uint8_t *)bzalloc(needed);
    filter->luma_buffer_size = needed;
}

// Funkcja do liczenia różniących się bajtów
static size_t count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t size) {
    size_t diff = 0;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) ++diff;
    }
    return diff;
}

// Funkcja do inicjalizacji buforów dla poprzednich linii
static void init_prev_lines_buffers(struct fps_analyzer_filter *filter, int roi_width) {
    for (int i = 0; i < 3; ++i) {
        if (filter->prev_lines[i]) bfree(filter->prev_lines[i]);
        filter->prev_lines[i] = (uint8_t*)bzalloc(roi_width);
    }
    filter->prev_lines_size = roi_width;
}

// Funkcja do inicjalizacji bufora poprzedniej klatki
static void init_prev_frame_buffer(struct fps_analyzer_filter *filter, size_t roi_size, const uint8_t *roi_ptr) {
    if (filter->prev_frame) bfree(filter->prev_frame);
    filter->prev_frame = (uint8_t*)bzalloc(roi_size);
    filter->prev_frame_size = roi_size;
    memcpy(filter->prev_frame, roi_ptr, roi_size);
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

// --- Shared analysis logic ---

// Wspólna logika analizy klatek — porównanie z poprzednią klatką, rolling window, frametime
static void analyze_luma_frame(struct fps_analyzer_filter *filter,
                               const uint8_t *luma_ptr, size_t luma_size) {
    int is_unique = 0;
    if (!filter->prev_frame || filter->prev_frame_size != luma_size) {
        init_prev_frame_buffer(filter, luma_size, luma_ptr);
        is_unique = 1;
    } else {
        size_t diff = count_diff_bytes(luma_ptr, filter->prev_frame, luma_size);
        double percent = (luma_size > 0) ? (100.0 * diff / luma_size) : 0.0;
        if (percent >= filter->sensitivity) {
            is_unique = 1;
        }
        memcpy(filter->prev_frame, luma_ptr, luma_size);
    }
    if (is_unique) {
        uint64_t now = os_gettime_ns();
        int idx = (filter->rolling_start + filter->rolling_count) % ROLLING_MAX;
        filter->rolling_times[idx] = now;
        if (filter->rolling_count < ROLLING_MAX) {
            filter->rolling_count++;
        } else {
            filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
        }
        while (filter->rolling_count > 0 &&
               now - filter->rolling_times[filter->rolling_start] > 1000000000ULL) {
            filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
            filter->rolling_count--;
        }
        if (filter->last_unique_frame_time != 0) {
            double ft = (now - filter->last_unique_frame_time) / 1000000.0;
            filter->frametime_history[filter->frametime_pos] = ft;
            filter->tearing_per_frame[filter->frametime_pos] = filter->tearing_detected;

            // Smoothed frametime: EMA (exponential moving average)
            // alpha=0.15 — responsive enough to show stutters, smooth enough to reduce noise
            {
                const double alpha = 0.15;
                if (filter->ema_frametime <= 0.0)
                    filter->ema_frametime = ft; // init to first value
                else
                    filter->ema_frametime = filter->ema_frametime * (1.0 - alpha) + ft * alpha;
                filter->smoothed_frametime[filter->frametime_pos] = filter->ema_frametime;
            }

            // Compute smoothed FPS for this point: average frametime over ~1s window
            // Window size based on instantaneous FPS estimate
            int inst_fps = (ft > 0.0) ? (int)round(1000.0 / ft) : 30;
            if (inst_fps < 10) inst_fps = 10;
            if (inst_fps > 120) inst_fps = 120;
            int window = inst_fps;
            if (window > filter->frametime_count) window = filter->frametime_count;
            if (window < 1) window = 1;
            double sum = 0.0;
            for (int w = 0; w < window; w++) {
                int idx = (filter->frametime_pos - w + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
                sum += filter->frametime_history[idx];
            }
            double avg_ft = sum / window;
            filter->fps_per_frame[filter->frametime_pos] = (avg_ft > 0.0) ? round(1000.0 / avg_ft) : 0.0;

            filter->frametime_pos = (filter->frametime_pos + 1) % FRAMETIME_HISTORY;
            if (filter->frametime_count < FRAMETIME_HISTORY)
                filter->frametime_count++;
        }
        filter->last_unique_frame_time = now;
    }
}

// --- Tearing detection ---

// Core tearing detection — takes 3 concatenated luma lines (top, mid, bottom), each roi_width bytes
static bool detect_tearing_core(struct fps_analyzer_filter *filter,
                                const uint8_t *lines_luma, int roi_width) {
    if (!filter->enable_tearing_detection) return false;

    if (!filter->prev_lines[0] || filter->prev_lines_size != (size_t)roi_width) {
        init_prev_lines_buffers(filter, roi_width);
        memcpy(filter->prev_lines[0], lines_luma, roi_width);
        memcpy(filter->prev_lines[1], lines_luma + roi_width, roi_width);
        memcpy(filter->prev_lines[2], lines_luma + 2 * roi_width, roi_width);
        return false;
    }

    // Porównaj 3 linie osobno z progiem czułości
    double change_percent[3] = {0.0, 0.0, 0.0};
    for (int i = 0; i < 3; ++i) {
        size_t diff = count_diff_bytes(lines_luma + i * roi_width, filter->prev_lines[i], roi_width);
        change_percent[i] = (roi_width > 0) ? (100.0 * diff / roi_width) : 0.0;
    }

    // Zapisz aktualne linie
    memcpy(filter->prev_lines[0], lines_luma, roi_width);
    memcpy(filter->prev_lines[1], lines_luma + roi_width, roi_width);
    memcpy(filter->prev_lines[2], lines_luma + 2 * roi_width, roi_width);

    // Ulepszona logika wykrywania tearingu
    bool significant_change[3] = {false, false, false};
    for (int i = 0; i < 3; ++i) {
        significant_change[i] = (change_percent[i] >= filter->tearing_sensitivity);
    }

    // Wykryj tearing: jeśli nie wszystkie linie się zmieniły jednocześnie
    bool all_changed = significant_change[0] && significant_change[1] && significant_change[2];
    bool none_changed = !significant_change[0] && !significant_change[1] && !significant_change[2];
    bool tearing = !(all_changed || none_changed);

    // Dodaj do historii tearingu
    filter->tearing_history[filter->tearing_history_pos] = tearing ? 1 : 0;
    filter->tearing_history_pos = (filter->tearing_history_pos + 1) % 5;

    // Sprawdź czy w ostatnich 5 klatkach było więcej niż 2 wykrycia tearingu
    int recent_tears = 0;
    for (int i = 0; i < 5; ++i) {
        recent_tears += filter->tearing_history[i];
    }
    return (recent_tears >= 2);
}

// Extract 3 tearing lines from async frame and run detection
static bool detect_tearing_async(struct fps_analyzer_filter *filter,
                                 struct obs_source_frame *frame) {
    if (!filter->enable_tearing_detection) return false;

    const int roi_width = (int)frame->width;
    if (roi_width > 4096) return false;

    uint8_t lines_luma[3 * 4096];
    int line_ys[3] = {0, (int)frame->height / 2, (int)frame->height - 1};

    switch (frame->format) {
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
    case VIDEO_FORMAT_I444:
    case VIDEO_FORMAT_I422:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            memcpy(lines_luma + i * roi_width,
                   frame->data[0] + y * frame->linesize[0], roi_width);
        }
        break;
    case VIDEO_FORMAT_YUY2:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            uint8_t *src = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < roi_width; ++x) {
                lines_luma[i * roi_width + x] = src[x * 2];
            }
        }
        break;
    case VIDEO_FORMAT_UYVY:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            uint8_t *src = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < roi_width; ++x) {
                lines_luma[i * roi_width + x] = src[x * 2 + 1];
            }
        }
        break;
    case VIDEO_FORMAT_BGRA:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < roi_width; ++x) {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                lines_luma[i * roi_width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
            }
        }
        break;
    case VIDEO_FORMAT_RGBA:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *row = frame->data[0] + y * frame->linesize[0];
            for (int x = 0; x < roi_width; ++x) {
                uint8_t r = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t b = row[x * 4 + 2];
                lines_luma[i * roi_width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
            }
        }
        break;
    default:
        return false;
    }

    return detect_tearing_core(filter, lines_luma, roi_width);
}

// Extract 3 tearing lines from BGRA staging data and run detection
static bool detect_tearing_bgra(struct fps_analyzer_filter *filter,
                                const uint8_t *bgra, uint32_t linesize,
                                uint32_t width, uint32_t height) {
    if (!filter->enable_tearing_detection) return false;
    if (width > 4096) return false;

    uint8_t lines_luma[3 * 4096];
    int line_ys[3] = {0, (int)height / 2, (int)height - 1};

    for (int i = 0; i < 3; ++i) {
        const uint8_t *row = bgra + line_ys[i] * linesize;
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t b = row[x * 4 + 0];
            uint8_t g = row[x * 4 + 1];
            uint8_t r = row[x * 4 + 2];
            lines_luma[i * width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
        }
    }

    return detect_tearing_core(filter, lines_luma, (int)width);
}

// --- Async source path (filter_video) ---

static struct obs_source_frame *fps_analyzer_filter_video(void *data,
                                                          struct obs_source_frame *frame)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (!frame || !frame->data[0])
        return frame;

    // Log format changes for debugging
    if ((int)frame->format != filter->last_logged_format) {
        blog(LOG_INFO, "[FPS Analyzer] Video format: %d, resolution: %ux%u",
             (int)frame->format, frame->width, frame->height);
        filter->last_logged_format = (int)frame->format;
    }

    const uint32_t width = frame->width;
    const uint32_t height = frame->height;
    int roi_line, roi_lines;

    if (filter->analyze_method == ANALYZE_DIFF) {
        roi_line = 0;
        roi_lines = height;
    } else {
        roi_line = height - 1;
        roi_lines = 1;
    }

    size_t luma_size = (size_t)width * roi_lines;
    // Allocate enough for full frame (needed for tearing detection even in LAST_LINE mode)
    ensure_luma_buffer(filter, (size_t)width * height);
    uint8_t *luma = filter->luma_buffer;
    bool format_ok = true;

    switch (frame->format) {
    case VIDEO_FORMAT_NV12:
    case VIDEO_FORMAT_I420:
    case VIDEO_FORMAT_I444:
    case VIDEO_FORMAT_I422:
        // Y plane in data[0] with linesize[0] stride
        if (filter->analyze_method == ANALYZE_DIFF) {
            for (uint32_t y = 0; y < height; ++y) {
                memcpy(luma + y * width,
                       frame->data[0] + y * frame->linesize[0], width);
            }
        } else {
            memcpy(luma, frame->data[0] + roi_line * frame->linesize[0], width);
        }
        break;
    case VIDEO_FORMAT_YUY2:
        // Y at even byte positions: src[x*2]
        if (filter->analyze_method == ANALYZE_DIFF) {
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t *src = frame->data[0] + y * frame->linesize[0];
                for (uint32_t x = 0; x < width; ++x) {
                    luma[y * width + x] = src[x * 2];
                }
            }
        } else {
            uint8_t *src = frame->data[0] + roi_line * frame->linesize[0];
            for (uint32_t x = 0; x < width; ++x) {
                luma[x] = src[x * 2];
            }
        }
        break;
    case VIDEO_FORMAT_UYVY:
        // Y at odd byte positions: src[x*2+1]
        if (filter->analyze_method == ANALYZE_DIFF) {
            for (uint32_t y = 0; y < height; ++y) {
                uint8_t *src = frame->data[0] + y * frame->linesize[0];
                for (uint32_t x = 0; x < width; ++x) {
                    luma[y * width + x] = src[x * 2 + 1];
                }
            }
        } else {
            uint8_t *src = frame->data[0] + roi_line * frame->linesize[0];
            for (uint32_t x = 0; x < width; ++x) {
                luma[x] = src[x * 2 + 1];
            }
        }
        break;
    case VIDEO_FORMAT_BGRA:
        if (filter->analyze_method == ANALYZE_DIFF) {
            bgra_to_luma(frame->data[0], frame->linesize[0], luma, width, height);
        } else {
            bgra_to_luma(frame->data[0] + roi_line * frame->linesize[0],
                         frame->linesize[0], luma, width, 1);
        }
        break;
    case VIDEO_FORMAT_RGBA:
        if (filter->analyze_method == ANALYZE_DIFF) {
            rgba_to_luma(frame->data[0], frame->linesize[0], luma, width, height);
        } else {
            rgba_to_luma(frame->data[0] + roi_line * frame->linesize[0],
                         frame->linesize[0], luma, width, 1);
        }
        break;
    default:
        format_ok = false;
        break;
    }

    if (!format_ok) {
        g_fps_shared.unsupported_format = (int)frame->format;
        return frame;
    }
    g_fps_shared.unsupported_format = -1;

    // Wykrywanie tearingu (niezależne od metody analizy)
    filter->tearing_detected = detect_tearing_async(filter, frame);

    // Analiza klatki
    analyze_luma_frame(filter, luma, luma_size);

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
        // Tearing detection from BGRA staging data
        filter->tearing_detected = detect_tearing_bgra(
            filter, video_data, video_linesize, width, height);

        // Extract luma for analysis
        size_t luma_size;
        if (filter->analyze_method == ANALYZE_DIFF) {
            luma_size = (size_t)width * height;
            ensure_luma_buffer(filter, luma_size);
            bgra_to_luma(video_data, video_linesize,
                         filter->luma_buffer, width, height);
        } else {
            luma_size = width;
            ensure_luma_buffer(filter, luma_size);
            bgra_to_luma(video_data + (height - 1) * video_linesize,
                         video_linesize, filter->luma_buffer, width, 1);
        }

        analyze_luma_frame(filter, filter->luma_buffer, luma_size);
        g_fps_shared.unsupported_format = -1;

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

static void fps_analyzer_video_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    uint64_t now = os_gettime_ns();
    double elapsed = (now - filter->last_write_time) / 1000000000.0;
    if (elapsed < filter->update_interval)
        return;
    filter->last_write_time = now;

    // Stale data check — if no unique frame detected for >2s, reset to 0
    if (filter->last_unique_frame_time != 0 &&
        now - filter->last_unique_frame_time > 2000000000ULL) {
        filter->frametime_count = 0;
        filter->frametime_pos = 0;
        filter->rolling_count = 0;
        filter->rolling_start = 0;
    }

    // --- FPS from rolling window: average last ~1 second of frametimes ---
    // Use min(frametime_count, last_fps) as window, minimum 10, maximum 120
    int window = filter->frametime_count;
    if (window > 120) window = 120;
    // Dynamically shrink window to ~1 second based on previous FPS
    if (g_fps_shared.fps > 10 && g_fps_shared.fps < window)
        window = g_fps_shared.fps;
    if (window < 10 && filter->frametime_count >= 10) window = 10;
    if (window > filter->frametime_count) window = filter->frametime_count;

    double avg_frametime = 0.0;
    for (int i = 0; i < window; ++i) {
        int idx = (filter->frametime_pos - window + i + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
        avg_frametime += filter->frametime_history[idx];
    }
    if (window > 0)
        avg_frametime /= window;
    double fps = (avg_frametime > 0.0) ? (1000.0 / avg_frametime) : 0.0;
    int fps_smooth = (int)round(fps);
    double frametime_ms = avg_frametime;

    // Update shared data for overlay source
    g_fps_shared.fps = fps_smooth;
    g_fps_shared.frametime_ms = frametime_ms;
    g_fps_shared.tearing_detected = filter->tearing_detected;
    g_fps_shared.last_update_ns = now;

    // Linearize circular frametime buffer for graph (oldest → newest)
    int count = filter->frametime_count;
    if (count > FPS_GRAPH_HISTORY) count = FPS_GRAPH_HISTORY;
    for (int i = 0; i < count; i++) {
        int idx = (filter->frametime_pos - count + i + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
        g_fps_shared.graph_frametimes[i] = filter->smoothed_frametime[idx];
        g_fps_shared.graph_frametimes_raw[i] = filter->frametime_history[idx];
        g_fps_shared.graph_fps[i] = filter->fps_per_frame[idx];
        g_fps_shared.graph_tearing[i] = filter->tearing_per_frame[idx];
    }
    g_fps_shared.graph_count = count;

    // Optional CSV logging
    if (filter->enable_csv) {
        char csv_path[512];
        build_csv_path(filter->output_path, csv_path, sizeof(csv_path));
        FILE *csv = fopen(csv_path, "a");
        if (csv) {
            time_t t = time(NULL);
            fprintf(csv, "%lld,%d,%.2f\n", (long long)t, fps_smooth, frametime_ms);
            fclose(csv);
        }
        keep_last_n_lines(csv_path, FPS_CSV_HISTORY_LIMIT);
    }
}

// --- Lifecycle ---

static void fps_analyzer_destroy(void *data)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (filter) {
        g_fps_shared.active_filter_count--;
        if (filter->prev_frame) bfree(filter->prev_frame);
        for (int i = 0; i < 3; ++i) {
            if (filter->prev_lines[i]) bfree(filter->prev_lines[i]);
        }
        if (filter->luma_buffer) bfree(filter->luma_buffer);
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
    filter->last_unique_frame_time = 0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
    }
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->rolling_count = 0;
    filter->rolling_start = 0;
    filter->last_write_time = 0;
    filter->frametime_pos = 0;
    filter->frametime_count = 0;
    filter->analyze_method = (analyze_method_t)obs_data_get_int(settings, "analyze_method");
    filter->sensitivity = obs_data_get_double(settings, "sensitivity");
    filter->tearing_detected = 0;
    filter->enable_tearing_detection = obs_data_get_bool(settings, "enable_tearing_detection");
    filter->tearing_sensitivity = obs_data_get_double(settings, "tearing_sensitivity");
    filter->prev_lines[0] = NULL;
    filter->prev_lines[1] = NULL;
    filter->prev_lines[2] = NULL;
    filter->prev_lines_size = 0;
    filter->tearing_history_pos = 0;
    for (int i = 0; i < 5; ++i) {
        filter->tearing_history[i] = 0;
    }
    // Dynamic luma buffer
    filter->luma_buffer = NULL;
    filter->luma_buffer_size = 0;
    // GPU staging (lazy init in video_render)
    filter->texrender = NULL;
    filter->stagesurface = NULL;
    filter->staged_width = 0;
    filter->staged_height = 0;
    filter->last_logged_format = -1;
    filter->ema_frametime = 0.0;
    // CSV logging
    filter->enable_csv = obs_data_get_bool(settings, "enable_csv");
    g_fps_shared.active_filter_count++;
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.4";
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
    obs_property_list_add_float(interval, "0.5", 0.5);
    obs_property_list_add_float(interval, "1", 1.0);
    obs_property_list_add_float(interval, "2", 2.0);

    // Tearing detection
    obs_properties_add_bool(props, "enable_tearing_detection", "Tearing detection");
    obs_properties_add_float_slider(props, "tearing_sensitivity", "Tearing sensitivity threshold (%)", 0.1, 10.0, 0.1);

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
    filter->enable_csv = obs_data_get_bool(settings, "enable_csv");
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

static void keep_last_n_lines(const char *csv_path, int n) {
    FILE *f = fopen(csv_path, "r");
    if (!f) return;
    char *lines[FPS_CSV_HISTORY_LIMIT+1];
    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        lines[count] = strdup(buf);
        count++;
        if (count > n) {
            free(lines[0]);
            memmove(lines, lines+1, sizeof(char*)*n);
            count = n;
        }
    }
    fclose(f);
    f = fopen(csv_path, "w");
    if (f) {
        for (int i = 0; i < count; ++i) {
            fputs(lines[i], f);
            free(lines[i]);
        }
        fclose(f);
    } else {
        for (int i = 0; i < count; ++i) free(lines[i]);
    }
}

// --- Defaults ---

static void fps_analyzer_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "enable_csv", false);
    obs_data_set_default_bool(settings, "clear_csv_on_start", true);
    obs_data_set_default_bool(settings, "enable_tearing_detection", true);
    obs_data_set_default_double(settings, "tearing_sensitivity", 1.0);
    obs_data_set_default_int(settings, "analyze_method", ANALYZE_LAST_LINE);
    obs_data_set_default_double(settings, "sensitivity", 0.1);
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
