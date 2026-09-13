// Reference implementation of the v0.5.0 FPS analysis (see fps-ref-v050.h).
// The function bodies below are the v0.5.0 filter code with the mechanical
// substitutions listed in the header. Keep them as they are: the point is
// to be a faithful oracle, not good code.

#include "fps-ref-v050.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROLLING_MAX 120 // max 2 sekundy przy 60 FPS
#define FRAMETIME_HISTORY 960
#define FPS_GRAPH_HISTORY 960
#define FPS_CSV_HISTORY_LIMIT 300

// v0.5.0 struct fps_analyzer_filter, FPS-related fields only, plus the
// per-instance stand-in for g_fps_shared.fps and the debug snapshot.
struct fpsref_core {
    double update_interval;
    uint64_t last_unique_frame_time;
    uint64_t rolling_times[ROLLING_MAX];
    int rolling_count;
    int rolling_start;
    uint64_t last_write_time;
    double frametime_history[FRAMETIME_HISTORY];
    int frametime_pos;
    int frametime_count;
    int analyze_method;
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
    uint8_t *luma_buffer;
    size_t luma_buffer_size;
    bool tearing_per_frame[FRAMETIME_HISTORY];
    double fps_per_frame[FRAMETIME_HISTORY];
    double smoothed_frametime[FRAMETIME_HISTORY];
    double ema_frametime;
    int last_published_fps; // v0.5.0 read g_fps_shared.fps here
    struct fps_core_frame_debug dbg;
};

// --- Utility functions (v0.5.0) ---

static void ensure_luma_buffer(struct fpsref_core *filter, size_t needed) {
    if (filter->luma_buffer_size >= needed)
        return;
    if (filter->luma_buffer) free(filter->luma_buffer);
    filter->luma_buffer = (uint8_t *)calloc(1, needed);
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
static void init_prev_lines_buffers(struct fpsref_core *filter, int roi_width) {
    for (int i = 0; i < 3; ++i) {
        if (filter->prev_lines[i]) free(filter->prev_lines[i]);
        filter->prev_lines[i] = (uint8_t*)calloc(1, roi_width);
    }
    filter->prev_lines_size = roi_width;
}

// Funkcja do inicjalizacji bufora poprzedniej klatki
static void init_prev_frame_buffer(struct fpsref_core *filter, size_t roi_size, const uint8_t *roi_ptr) {
    if (filter->prev_frame) free(filter->prev_frame);
    filter->prev_frame = (uint8_t*)calloc(1, roi_size);
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

// v0.5.0 extract_full_luma_async / extract_line_luma_async, generalised to
// a row range with the same per-format pixel access.
static bool extract_luma_rows(const struct fps_frame_view *frame, uint8_t *luma,
                              uint32_t first_line, uint32_t lines) {
    const uint32_t width = frame->width;
    switch (frame->format) {
    case FPS_PIXFMT_NV12:
    case FPS_PIXFMT_I420:
    case FPS_PIXFMT_I444:
    case FPS_PIXFMT_I422:
        for (uint32_t y = 0; y < lines; ++y) {
            memcpy(luma + y * width,
                   frame->data + (first_line + y) * frame->linesize, width);
        }
        return true;
    case FPS_PIXFMT_YUY2:
        for (uint32_t y = 0; y < lines; ++y) {
            const uint8_t *src = frame->data + (first_line + y) * frame->linesize;
            for (uint32_t x = 0; x < width; ++x) {
                luma[y * width + x] = src[x * 2];
            }
        }
        return true;
    case FPS_PIXFMT_UYVY:
        for (uint32_t y = 0; y < lines; ++y) {
            const uint8_t *src = frame->data + (first_line + y) * frame->linesize;
            for (uint32_t x = 0; x < width; ++x) {
                luma[y * width + x] = src[x * 2 + 1];
            }
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

// --- Unique-frame analysis (v0.5.0 analyze_luma_frame, split in two) ---

// The tail of analyze_luma_frame: everything under `if (is_unique)`.
static void analyze_unique(struct fpsref_core *filter, uint64_t now) {
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
            int idx2 = (filter->frametime_pos - w + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
            sum += filter->frametime_history[idx2];
        }
        double avg_ft = sum / window;
        filter->fps_per_frame[filter->frametime_pos] = (avg_ft > 0.0) ? round(1000.0 / avg_ft) : 0.0;

        filter->dbg.sample_written = true;
        filter->dbg.ft_ms = ft;
        filter->dbg.ema = filter->ema_frametime;
        filter->dbg.fps_pf = filter->fps_per_frame[filter->frametime_pos];

        filter->frametime_pos = (filter->frametime_pos + 1) % FRAMETIME_HISTORY;
        if (filter->frametime_count < FRAMETIME_HISTORY)
            filter->frametime_count++;
    }
    filter->last_unique_frame_time = now;
}

static void analyze_luma_frame(struct fpsref_core *filter,
                               const uint8_t *luma_ptr, size_t luma_size, uint64_t now) {
    int is_unique = 0;
    filter->dbg.luma_size = luma_size;
    filter->dbg.compared = false;
    filter->dbg.diff = 0;
    filter->dbg.percent = 0.0;
    filter->dbg.sample_written = false;
    filter->dbg.ft_ms = filter->dbg.ema = filter->dbg.fps_pf = 0.0;
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
        filter->dbg.compared = true;
        filter->dbg.diff = diff;
        filter->dbg.percent = percent;
    }
    filter->dbg.unique = is_unique != 0;
    if (is_unique) {
        analyze_unique(filter, now);
    }
    filter->dbg.pos = filter->frametime_pos;
    filter->dbg.count = filter->frametime_count;
}

// --- Tearing detection (v0.5.0) ---

static bool detect_tearing_core(struct fpsref_core *filter,
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

// v0.5.0 detect_tearing_async (the BGRA branch equals detect_tearing_bgra)
static bool detect_tearing_frame(struct fpsref_core *filter,
                                 const struct fps_frame_view *frame) {
    if (!filter->enable_tearing_detection) return false;

    const int roi_width = (int)frame->width;
    if (roi_width > 4096) return false;

    uint8_t lines_luma[3 * 4096];
    int line_ys[3] = {0, (int)frame->height / 2, (int)frame->height - 1};

    switch (frame->format) {
    case FPS_PIXFMT_NV12:
    case FPS_PIXFMT_I420:
    case FPS_PIXFMT_I444:
    case FPS_PIXFMT_I422:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            memcpy(lines_luma + i * roi_width,
                   frame->data + y * frame->linesize, roi_width);
        }
        break;
    case FPS_PIXFMT_YUY2:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *src = frame->data + y * frame->linesize;
            for (int x = 0; x < roi_width; ++x) {
                lines_luma[i * roi_width + x] = src[x * 2];
            }
        }
        break;
    case FPS_PIXFMT_UYVY:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *src = frame->data + y * frame->linesize;
            for (int x = 0; x < roi_width; ++x) {
                lines_luma[i * roi_width + x] = src[x * 2 + 1];
            }
        }
        break;
    case FPS_PIXFMT_BGRA:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *row = frame->data + y * frame->linesize;
            for (int x = 0; x < roi_width; ++x) {
                uint8_t b = row[x * 4 + 0];
                uint8_t g = row[x * 4 + 1];
                uint8_t r = row[x * 4 + 2];
                lines_luma[i * roi_width + x] = (uint8_t)((r * 66 + g * 129 + b * 25 + 128) >> 8) + 16;
            }
        }
        break;
    case FPS_PIXFMT_RGBA:
        for (int i = 0; i < 3; ++i) {
            int y = line_ys[i];
            const uint8_t *row = frame->data + y * frame->linesize;
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

// --- Public API ---

static void apply_params(struct fpsref_core *filter, const struct fps_core_params *p) {
    filter->update_interval = p->update_interval;
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->enable_tearing_detection = p->enable_tearing_detection;
    filter->tearing_sensitivity = p->tearing_sensitivity;
    filter->analyze_method = p->analyze_method;
    filter->sensitivity = p->sensitivity;
}

struct fpsref_core *fpsref_create(const struct fps_core_params *p) {
    struct fpsref_core *filter = (struct fpsref_core *)calloc(1, sizeof(struct fpsref_core));
    if (!filter)
        return NULL;
    struct fps_core_params d = fps_core_params_defaults();
    apply_params(filter, p ? p : &d);
    return filter;
}

void fpsref_destroy(struct fpsref_core *filter) {
    if (!filter)
        return;
    if (filter->prev_frame) free(filter->prev_frame);
    for (int i = 0; i < 3; ++i)
        if (filter->prev_lines[i]) free(filter->prev_lines[i]);
    if (filter->luma_buffer) free(filter->luma_buffer);
    free(filter);
}

void fpsref_set_params(struct fpsref_core *filter, const struct fps_core_params *p) {
    if (filter && p)
        apply_params(filter, p);
}

void fpsref_get_params(const struct fpsref_core *filter, struct fps_core_params *out) {
    if (!filter || !out)
        return;
    out->analyze_method = filter->analyze_method;
    out->sensitivity = filter->sensitivity;
    out->enable_tearing_detection = filter->enable_tearing_detection;
    out->tearing_sensitivity = filter->tearing_sensitivity;
    out->update_interval = filter->update_interval;
}

// v0.5.0 fps_analyzer_filter_video (async path) / video_render (sync path):
// extraction -> unsupported format returns before anything else ->
// tearing -> analysis.
int fpsref_feed(struct fpsref_core *filter, const struct fps_frame_view *frame, uint64_t now) {
    if (!frame || !frame->data)
        return FPS_FEED_NO_DATA;

    const uint32_t width = frame->width;
    const uint32_t height = frame->height;
    int roi_line, roi_lines;

    if (filter->analyze_method == FPS_ANALYZE_DIFF) {
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

    if (!extract_luma_rows(frame, luma, (uint32_t)roi_line, (uint32_t)roi_lines))
        return FPS_FEED_UNSUPPORTED_FORMAT;

    // Wykrywanie tearingu (niezależne od metody analizy)
    filter->tearing_detected = detect_tearing_frame(filter, frame);
    filter->dbg.tearing_flag = filter->tearing_detected != 0;

    // Analiza klatki
    analyze_luma_frame(filter, luma, luma_size, now);
    return FPS_FEED_OK;
}

// v0.5.0 fps_analyzer_video_tick, aggregation part
bool fpsref_tick(struct fpsref_core *filter, uint64_t now, struct fps_core_output *out) {
    double elapsed = (now - filter->last_write_time) / 1000000000.0;
    if (elapsed < filter->update_interval)
        return false;
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
    if (filter->last_published_fps > 10 && filter->last_published_fps < window)
        window = filter->last_published_fps;
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
    filter->last_published_fps = fps_smooth;
    out->fps = fps_smooth;
    out->frametime_ms = frametime_ms;
    out->tearing_detected = filter->tearing_detected != 0;
    out->window = window;

    // Linearize circular frametime buffer for graph (oldest → newest)
    int count = filter->frametime_count;
    if (count > FPS_GRAPH_HISTORY) count = FPS_GRAPH_HISTORY;
    for (int i = 0; i < count; i++) {
        int idx = (filter->frametime_pos - count + i + FRAMETIME_HISTORY) % FRAMETIME_HISTORY;
        out->graph_frametimes[i] = filter->smoothed_frametime[idx];
        out->graph_frametimes_raw[i] = filter->frametime_history[idx];
        out->graph_fps[i] = filter->fps_per_frame[idx];
        out->graph_tearing[i] = filter->tearing_per_frame[idx];
    }
    out->graph_count = count;
    return true;
}

// --- Pure helpers ---

size_t fpsref_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n) {
    return count_diff_bytes(a, b, n);
}

void fpsref_bgra_to_luma(const uint8_t *bgra, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height) {
    bgra_to_luma(bgra, linesize, luma, width, height);
}

void fpsref_rgba_to_luma(const uint8_t *rgba, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height) {
    rgba_to_luma(rgba, linesize, luma, width, height);
}

bool fpsref_extract_luma(const struct fps_frame_view *f, uint8_t *luma, uint32_t first_line, uint32_t lines) {
    return extract_luma_rows(f, luma, first_line, lines);
}

int fpsref_format_csv_line(char *buf, size_t size, long long time_s, int fps, double frametime_ms) {
    return snprintf(buf, size, "%lld,%d,%.2f\n", time_s, fps, frametime_ms);
}

// v0.5.0 keep_last_n_lines, verbatim (256-byte fgets granularity included)
void fpsref_csv_keep_last_n_lines(const char *csv_path, int n) {
    FILE *f = fopen(csv_path, "r");
    if (!f) return;
    char *lines[FPS_CSV_HISTORY_LIMIT+1];
    int count = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        lines[count] = _strdup(buf);
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

// --- Debug / replay ---

void fpsref_debug_last_frame(const struct fpsref_core *filter, struct fps_core_frame_debug *out) {
    if (filter && out)
        *out = filter->dbg;
}

void fpsref_debug_feed_decision(struct fpsref_core *filter, bool unique, bool tearing, uint64_t now) {
    filter->tearing_detected = tearing ? 1 : 0;
    filter->dbg.tearing_flag = tearing;
    filter->dbg.luma_size = 0;
    filter->dbg.compared = false;
    filter->dbg.diff = 0;
    filter->dbg.percent = 0.0;
    filter->dbg.sample_written = false;
    filter->dbg.ft_ms = filter->dbg.ema = filter->dbg.fps_pf = 0.0;
    filter->dbg.unique = unique;
    if (unique)
        analyze_unique(filter, now);
    filter->dbg.pos = filter->frametime_pos;
    filter->dbg.count = filter->frametime_count;
}

int fpsref_debug_last_published_fps(const struct fpsref_core *filter) {
    return filter ? filter->last_published_fps : 0;
}
