#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/crc32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#define strcasecmp _stricmp
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fps-analyzer", "en-US")

struct fps_analyzer_filter {
    obs_source_t *context;
    uint64_t last_frame_time;
    int unique_frame_count;
    float current_fps;
    uint32_t prev_crc;
    bool has_prev;
    char output_path[512];
    double sensitivity_percent; // threshold in percent (0-100)
    uint8_t *prev_frame;
    size_t prev_frame_size;
};

// Helper: ensure .txt extension
static void ensure_txt_extension(char *path, size_t maxlen)
{
    size_t len = strlen(path);
    if (len < 4 || strcasecmp(path + len - 4, ".txt") != 0) {
        if (len + 4 < maxlen) {
            strcat(path, ".txt");
        }
    }
}

// Helper: count number of different bytes between two frames
static size_t count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t size)
{
    size_t diff = 0;
    for (size_t i = 0; i < size; ++i) {
        if (a[i] != b[i]) diff++;
    }
    return diff;
}

// Called for every video frame (pixel data)
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
            filter->unique_frame_count++;
        }
        // Save current frame for next comparison
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
    uint64_t now = os_gettime_ns();
    if (filter->last_frame_time == 0) {
        filter->last_frame_time = now;
        filter->unique_frame_count = 0;
        filter->current_fps = 0.0f;
        return;
    }
    double elapsed = (now - filter->last_frame_time) / 1000000000.0;
    if (elapsed >= 1.0) {
        filter->current_fps = filter->unique_frame_count / (float)elapsed;
        filter->last_frame_time = now;
        filter->unique_frame_count = 0;

        // Save FPS to file (user-defined path or default)
        char path[512];
        int fps_int = (int)(filter->current_fps + 0.5); // rounded FPS
        double frametime_ms = (fps_int > 0) ? (1000.0 / fps_int) : 0.0;
        if (filter->output_path[0]) {
            strncpy(path, filter->output_path, sizeof(path));
            path[sizeof(path)-1] = '\0';
            ensure_txt_extension(path, sizeof(path));
        } else {
            strcpy(path, "fps.txt");
        }
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "FPS: %d\nFrametime: %.2f ms\n", fps_int, frametime_ms);
            fclose(f);
        }
    }
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
    filter->prev_frame = NULL;
    filter->prev_frame_size = 0;
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
        ensure_txt_extension(filter->output_path, sizeof(filter->output_path));
    }
    filter->sensitivity_percent = obs_data_get_double(settings, "sensitivity_percent");
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer";
}

// Properties for filter settings
static obs_properties_t *fps_analyzer_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "output_path", "FPS Output file",
                            OBS_PATH_FILE_SAVE, "Text File (*.txt)", NULL);

    // Add info label above the slider
    obs_properties_add_text(
        props,
        "sensitivity_info",
        "",
        OBS_TEXT_INFO
    );
    obs_property_t *slider = obs_properties_add_float_slider(
        props, "sensitivity_percent", "Sensitivity threshold (%)", 0.0, 5.0, 0.01);
    // Description for the label
    obs_property_t *info = obs_properties_get(props, "sensitivity_info");
    if (info) {
        obs_property_set_description(info, "Minimal percent of changed bytes between frames to count as a new frame.\n0% = every change, 1% = ignore small noise, 5% = only big changes.");
    }
    return props;
}

// Update filter settings
static void fps_analyzer_update(void *data, obs_data_t *settings)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    const char *path = obs_data_get_string(settings, "output_path");
    if (path) {
        strncpy(filter->output_path, path, sizeof(filter->output_path));
        filter->output_path[sizeof(filter->output_path)-1] = '\0';
        ensure_txt_extension(filter->output_path, sizeof(filter->output_path));
    }
    filter->sensitivity_percent = obs_data_get_double(settings, "sensitivity_percent");
}

struct obs_source_info fps_analyzer_filter_info = {
	.id = "fps_analyzer_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fps_analyzer_get_name,
	.create = fps_analyzer_create,
	.destroy = fps_analyzer_destroy,
	.get_properties = fps_analyzer_properties,
	.update = fps_analyzer_update,
	.video_tick = fps_analyzer_video_tick,
	.filter_video = fps_analyzer_filter_video,
};

bool obs_module_load(void)
{
    obs_register_source(&fps_analyzer_filter_info);
    blog(LOG_INFO, "FPS Analyzer filter loaded!");
    return true;
} 