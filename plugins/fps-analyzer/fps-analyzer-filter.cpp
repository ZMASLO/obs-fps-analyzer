#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/crc32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

// Declare overlay info for registration in overlay file
extern struct obs_source_info fps_overlay_source_info;

#define FPS_CSV_HISTORY_LIMIT 300
#define ROLLING_MAX 120 // max 2 sekundy przy 60 FPS

// Prototypes
static void clear_csv_file(const char *csv_path);
static void keep_last_n_lines(const char *csv_path, int n);

struct fps_analyzer_filter {
    obs_source_t *context;
    uint64_t last_frame_time;
    int unique_frame_count;
    float current_fps;
    uint32_t prev_crc;
    bool has_prev;
    char output_path[512];
    double sensitivity_percent;
    double update_interval;
    uint8_t *prev_frame;
    size_t prev_frame_size;
    uint64_t last_unique_frame_time;
    double last_frametime_ms;
    bool clear_csv_on_start;
    uint64_t rolling_times[ROLLING_MAX];
    int rolling_count;
    int rolling_start;
};

static size_t count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t size)
{
    size_t diff = 0;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) diff++;
    }
    return diff;
}

static struct obs_source_frame *fps_analyzer_filter_video(void *data, struct obs_source_frame *frame)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (frame && frame->data[0]) {
        size_t frame_size = frame->linesize[0] * frame->height;
        int is_unique = 0;
        if (filter->has_prev && filter->prev_frame && filter->prev_frame_size == frame_size) {
            size_t diff = count_diff_bytes((const uint8_t *)frame->data[0], filter->prev_frame, frame_size);
            double percent = (frame_size > 0) ? (100.0 * diff / frame_size) : 0.0;
            if (percent > filter->sensitivity_percent) {
                is_unique = 1;
            }
        } else {
            is_unique = 1; // First frame or size changed
        }
        if (is_unique) {
            uint64_t now = os_gettime_ns();
            // Dodaj nowy timestamp do rolling window
            int idx = (filter->rolling_start + filter->rolling_count) % ROLLING_MAX;
            filter->rolling_times[idx] = now;
            if (filter->rolling_count < ROLLING_MAX) {
                filter->rolling_count++;
            } else {
                filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
            }
            // Usuń stare klatki spoza okna 1s
            while (filter->rolling_count > 0 &&
                   now - filter->rolling_times[filter->rolling_start] > 1000000000ULL) {
                filter->rolling_start = (filter->rolling_start + 1) % ROLLING_MAX;
                filter->rolling_count--;
            }
            // FPS = rolling_count
            filter->current_fps = (float)filter->rolling_count;
            // Frametime (odstęp od poprzedniej unikalnej klatki)
            if (filter->last_unique_frame_time != 0) {
                filter->last_frametime_ms = (now - filter->last_unique_frame_time) / 1000000.0;
            }
            filter->last_unique_frame_time = now;
        }
        if (!filter->prev_frame || filter->prev_frame_size != frame_size) {
            free(filter->prev_frame);
            filter->prev_frame = (uint8_t *)malloc(frame_size);
            filter->prev_frame_size = frame_size;
        }
        memcpy(filter->prev_frame, frame->data[0], frame_size);
        filter->has_prev = true;
    }
    return frame;
}

static void fps_analyzer_video_tick(void *data, float seconds)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    // Rolling window: FPS już jest liczony na bieżąco w filter->current_fps
    float fps = filter->current_fps;
    double frametime_ms = (fps > 0.0f) ? (1000.0 / fps) : 0.0;
    double last_frametime_ms = filter->last_frametime_ms;
    char path[512];
    if (filter->output_path[0]) {
        strncpy(path, filter->output_path, sizeof(path));
        path[sizeof(path)-1] = '\0';
        if (strlen(path) < 4 || strcasecmp(path + strlen(path) - 4, ".txt") != 0) {
            if (strlen(path) + 4 < sizeof(path)) {
                strcat(path, ".txt");
            }
        }
    } else {
        strcpy(path, "fps.txt");
    }
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "FPS: %.2f\nFrametime: %.2f ms\nLast frametime: %.2f ms\n", fps, frametime_ms, last_frametime_ms);
        fclose(f);
    }
    char csv_path[512];
    if (filter->output_path[0]) {
        strncpy(csv_path, filter->output_path, sizeof(csv_path));
        csv_path[sizeof(csv_path)-1] = '\0';
        char *dot = strrchr(csv_path, '.');
        if (dot) {
            strcpy(dot, ".csv");
        } else {
            strcat(csv_path, ".csv");
        }
    } else {
        strcpy(csv_path, "fps.csv");
    }
    FILE *csv = fopen(csv_path, "a");
    if (csv) {
        time_t t = time(NULL);
        fprintf(csv, "%lld,%.2f,%.2f,%.2f\n", (long long)t, fps, frametime_ms, last_frametime_ms);
        fclose(csv);
    }
    keep_last_n_lines(csv_path, FPS_CSV_HISTORY_LIMIT);
}

static void fps_analyzer_destroy(void *data)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (filter->prev_frame) free(filter->prev_frame);
    bfree(data);
}

static void *fps_analyzer_create(obs_data_t *settings, obs_source_t *context)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)bzalloc(sizeof(struct fps_analyzer_filter));
    filter->context = context;
    filter->last_frame_time = 0;
    filter->unique_frame_count = 0;
    filter->current_fps = 0.0f;
    filter->prev_crc = 0;
    filter->has_prev = false;
    filter->output_path[0] = '\0';
    filter->sensitivity_percent = 0.0;
    filter->update_interval = 1.0;
    filter->prev_frame = NULL;
    filter->prev_frame_size = 0;
    filter->last_unique_frame_time = 0;
    filter->last_frametime_ms = 0.0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
        if (strlen(filter->output_path) < 4 || strcasecmp(filter->output_path + strlen(filter->output_path) - 4, ".txt") != 0) {
            if (strlen(filter->output_path) + 4 < sizeof(filter->output_path)) {
                strcat(filter->output_path, ".txt");
            }
        }
    }
    filter->sensitivity_percent = obs_data_get_double(settings, "sensitivity_percent");
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->rolling_count = 0;
    filter->rolling_start = 0;
    // --- czyszczenie pliku CSV jeśli trzeba ---
    char csv_path[512];
    if (filter->output_path[0]) {
        strncpy(csv_path, filter->output_path, sizeof(csv_path));
        csv_path[sizeof(csv_path)-1] = '\0';
        char *dot = strrchr(csv_path, '.');
        if (dot) {
            strcpy(dot, ".csv");
        } else {
            strcat(csv_path, ".csv");
        }
    } else {
        strcpy(csv_path, "fps.csv");
    }
    if (filter->clear_csv_on_start) {
        clear_csv_file(csv_path);
    }
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer";
}

static obs_properties_t *fps_analyzer_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "output_path", "FPS Output file",
                            OBS_PATH_FILE_SAVE, "Text File (*.txt)", NULL);
    obs_properties_add_bool(props, "clear_csv_on_start", "Clear CSV file on start (default: yes)");
    obs_properties_add_text(
        props,
        "sensitivity_info",
        "",
        OBS_TEXT_INFO
    );
    obs_property_t *slider = obs_properties_add_float_slider(
        props, "sensitivity_percent", "Sensitivity threshold (%)", 0.0, 5.0, 0.01);
    obs_property_t *info = obs_properties_get(props, "sensitivity_info");
    if (info) {
        obs_property_set_description(info, "Minimal percent of changed bytes between frames to count as a new frame.\n0% = every change, 1% = ignore small noise, 5% = only big changes.");
    }
    obs_property_t *interval = obs_properties_add_list(
        props, "update_interval", "Update interval (seconds)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(interval, "0.5", 0.5);
    obs_property_list_add_float(interval, "1", 1.0);
    obs_property_list_add_float(interval, "2", 2.0);
    obs_property_set_long_description(interval, "How often FPS/frametime is written to file.");
    return props;
}

static void fps_analyzer_update(void *data, obs_data_t *settings)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
        if (strlen(filter->output_path) < 4 || strcasecmp(filter->output_path + strlen(filter->output_path) - 4, ".txt") != 0) {
            if (strlen(filter->output_path) + 4 < sizeof(filter->output_path)) {
                strcat(filter->output_path, ".txt");
            }
        }
    }
    filter->sensitivity_percent = obs_data_get_double(settings, "sensitivity_percent");
    filter->update_interval = obs_data_get_double(settings, "update_interval");
    if (filter->update_interval <= 0.0)
        filter->update_interval = 1.0;
    filter->clear_csv_on_start = obs_data_get_bool(settings, "clear_csv_on_start");
}

static void clear_csv_file(const char *csv_path) {
    FILE *f = fopen(csv_path, "w");
    if (f) fclose(f);
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

static void fps_analyzer_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_bool(settings, "clear_csv_on_start", true);
}

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
    .filter_video = fps_analyzer_filter_video,
}; 