#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>

// Declare filter info for registration
extern struct obs_source_info fps_analyzer_filter_info;

struct fps_overlay_source {
    char input_path[512];
    char last_text[256];
    uint64_t last_read_time;
    double update_interval;
};

static const char *fps_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Overlay (not working yet)";
}

static void *fps_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    struct fps_overlay_source *ctx = (fps_overlay_source *)bzalloc(sizeof(struct fps_overlay_source));
    ctx->input_path[0] = '\0';
    ctx->last_text[0] = '\0';
    ctx->last_read_time = 0;
    ctx->update_interval = 0.2;
    const char *path = obs_data_get_string(settings, "input_path");
    if (path) {
        strncpy(ctx->input_path, path, sizeof(ctx->input_path));
        ctx->input_path[sizeof(ctx->input_path)-1] = '\0';
    }
    ctx->update_interval = obs_data_get_double(settings, "update_interval");
    if (ctx->update_interval <= 0.0)
        ctx->update_interval = 0.2;
    return ctx;
}

static void fps_overlay_destroy(void *data)
{
    bfree(data);
}

static obs_properties_t *fps_overlay_properties(void *data)
{
    obs_properties_t *props = obs_properties_create();
    obs_properties_add_path(props, "input_path", "FPS Data file",
                            OBS_PATH_FILE, "Text File (*.txt)", NULL);
    obs_property_t *interval = obs_properties_add_list(
        props, "update_interval", "Update interval (seconds)",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(interval, "0.1", 0.1);
    obs_property_list_add_float(interval, "0.2", 0.2);
    obs_property_list_add_float(interval, "0.5", 0.5);
    obs_property_list_add_float(interval, "1", 1.0);
    obs_property_set_long_description(interval, "How often overlay reads the file.");
    return props;
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    const char *path = obs_data_get_string(settings, "input_path");
    if (path) {
        strncpy(ctx->input_path, path, sizeof(ctx->input_path));
        ctx->input_path[sizeof(ctx->input_path)-1] = '\0';
    }
    ctx->update_interval = obs_data_get_double(settings, "update_interval");
    if (ctx->update_interval <= 0.0)
        ctx->update_interval = 0.2;
}

static void fps_overlay_tick(void *data, float seconds)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    uint64_t now = os_gettime_ns();
    double elapsed = (now - ctx->last_read_time) / 1000000000.0;
    if (elapsed >= ctx->update_interval && ctx->input_path[0]) {
        FILE *f = fopen(ctx->input_path, "r");
        if (f) {
            fgets(ctx->last_text, sizeof(ctx->last_text), f);
            fclose(f);
        }
        ctx->last_read_time = now;
    }
}

static void fps_overlay_render(void *data, gs_effect_t *effect)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    // Draw simple white rectangle as background
    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (solid) {
        gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
        gs_effect_set_color(color, 0xFFFFFFFF); // white
        while (gs_effect_loop(solid, "Draw")) {
            gs_draw_sprite(NULL, 0, 200, 40);
        }
    }
    // Draw FPS text (for now, just log to OBS log)
    // TODO: Replace with actual text rendering if needed
    blog(LOG_INFO, "FPS Overlay: %s", ctx->last_text);
}

static uint32_t fps_overlay_get_width(void *data) {
    UNUSED_PARAMETER(data);
    return 200; // overlay width in px
}

static uint32_t fps_overlay_get_height(void *data) {
    UNUSED_PARAMETER(data);
    return 40; // overlay height in px
}

struct obs_source_info fps_overlay_source_info = {
    .id = "fps_overlay_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = fps_overlay_get_name,
    .create = fps_overlay_create,
    .destroy = fps_overlay_destroy,
    .get_width = fps_overlay_get_width,
    .get_height = fps_overlay_get_height,
    .get_properties = fps_overlay_properties,
    .update = fps_overlay_update,
    .video_tick = fps_overlay_tick,
    .video_render = fps_overlay_render,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("fps-analyzer", "en-US")

bool obs_module_load(void)
{
    obs_register_source(&fps_analyzer_filter_info);
    obs_register_source(&fps_overlay_source_info);
    blog(LOG_INFO, "FPS Analyzer filter and overlay loaded!");
    return true;
} 