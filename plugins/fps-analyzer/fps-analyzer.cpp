#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <stdio.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fps-analyzer", "en-US")

// Filter context structure
struct fps_analyzer_filter {
    obs_source_t *context;
    uint64_t last_frame_time;
    int frame_count;
    float current_fps;
};

// Called when a new frame is rendered
static void fps_analyzer_render(void *data, gs_effect_t *effect)
{
    struct fps_analyzer_filter *filter = (fps_analyzer_filter *)data;
    obs_source_t *target = obs_filter_get_target(filter->context);
    if (target) {
        obs_source_video_render(target);
    }

    // Optionally: draw a simple rectangle as overlay (for debug)
    // gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    // if (solid) {
    //     gs_effect_set_color(solid, "color", 1.0f, 1.0f, 1.0f, 0.3f); // white, 30% opacity
    //     while (gs_effect_loop(solid, "Draw")) {
    //         gs_draw_sprite(NULL, 0, 100, 40); // rectangle 100x40 px
    //     }
    // }
}

// Called for every video frame
static void fps_analyzer_video_tick(void *data, float seconds)
{
    struct fps_analyzer_filter *filter = (fps_analyzer_filter *)data;
    uint64_t now = os_gettime_ns();
    filter->frame_count++;
    if (filter->last_frame_time == 0) {
        filter->last_frame_time = now;
        filter->frame_count = 0;
        filter->current_fps = 0.0f;
        return;
    }
    double elapsed = (now - filter->last_frame_time) / 1000000000.0;
    if (elapsed >= 1.0) {
        filter->current_fps = filter->frame_count / (float)elapsed;
        filter->last_frame_time = now;
        filter->frame_count = 0;

        // Save FPS to file
        FILE *f = fopen("C:/Sources/obs-studio/build/rundir/Debug/fps.txt", "w");
        if (f) {
            fprintf(f, "FPS: %.1f\n", filter->current_fps);
            fclose(f);
        }
    }
}

// Filter destroy
static void fps_analyzer_destroy(void *data)
{
    bfree(data);
}

// Filter create
static void *fps_analyzer_create(obs_data_t *settings, obs_source_t *context)
{
    struct fps_analyzer_filter *filter = (fps_analyzer_filter *)bzalloc(sizeof(struct fps_analyzer_filter));
    filter->context = context;
    filter->last_frame_time = 0;
    filter->frame_count = 0;
    filter->current_fps = 0.0f;
    return filter;
}

// Filter get name
static const char *fps_analyzer_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer";
}

// OBS filter definition
struct obs_source_info fps_analyzer_filter_info = {
	.id = "fps_analyzer_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = fps_analyzer_get_name,
	.create = fps_analyzer_create,
	.destroy = fps_analyzer_destroy,
	.video_tick = fps_analyzer_video_tick,
	.video_render = fps_analyzer_render,
};

bool obs_module_load(void)
{
    obs_register_source(&fps_analyzer_filter_info);
    blog(LOG_INFO, "FPS Analyzer filter loaded!");
    return true;
} 