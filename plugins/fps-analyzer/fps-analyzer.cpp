#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <util/crc32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fps-analyzer", "en-US")

struct fps_analyzer_filter {
    obs_source_t *context;
    uint64_t last_frame_time;
    int unique_frame_count;
    float current_fps;
    uint32_t prev_crc;
    bool has_prev;
};

// Called for every video frame (pixel data)
static struct obs_source_frame *fps_analyzer_filter_video(void *data, struct obs_source_frame *frame)
{
    struct fps_analyzer_filter *filter = (struct fps_analyzer_filter *)data;
    if (frame && frame->data[0]) {
        // Calculate CRC32 hash of the frame pixel data
        uint32_t cur_crc = calc_crc32(0, frame->data[0], frame->linesize[0] * frame->height);
        if (filter->has_prev) {
            if (cur_crc != filter->prev_crc) {
                filter->unique_frame_count++;
            }
        }
        filter->prev_crc = cur_crc;
        filter->has_prev = true;
    }
    return frame; // Return unmodified frame
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

        // Save FPS to file (relative to OBS working dir)
        FILE *f = fopen("fps.txt", "w");
        if (f) {
            fprintf(f, "FPS: %.1f\n", filter->current_fps);
            fclose(f);
        }
    }
}

static void fps_analyzer_destroy(void *data)
{
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
    return filter;
}

static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer";
}

struct obs_source_info fps_analyzer_filter_info = {
    .id = "fps_analyzer_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = fps_analyzer_get_name,
    .create = fps_analyzer_create,
    .destroy = fps_analyzer_destroy,
    .video_tick = fps_analyzer_video_tick,
    .filter_video = fps_analyzer_filter_video,
};

bool obs_module_load(void)
{
    obs_register_source(&fps_analyzer_filter_info);
    blog(LOG_INFO, "FPS Analyzer filter loaded!");
    return true;
} 