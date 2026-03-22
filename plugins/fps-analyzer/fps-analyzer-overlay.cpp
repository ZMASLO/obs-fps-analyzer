#include <obs-module.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>

#include "fps-shared-data.h"

// Declare filter info for registration
extern struct obs_source_info fps_analyzer_filter_info;

struct fps_overlay_source {
    obs_source_t *text_source;
    int font_size;
    bool show_background;
    char last_text[512];
};

static const char *fps_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.3";
}

static void update_text_source(struct fps_overlay_source *ctx, const char *text)
{
    if (!ctx->text_source)
        return;

    obs_data_t *font_obj = obs_data_create();
    obs_data_set_string(font_obj, "face", "Arial");
    obs_data_set_int(font_obj, "size", ctx->font_size);
    obs_data_set_int(font_obj, "flags", 1); // bold
    obs_data_set_string(font_obj, "style", "Bold");

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_data_set_obj(settings, "font", font_obj);
    obs_data_set_int(settings, "color1", 0xFFFFFF);
    obs_data_set_int(settings, "color2", 0xFFFFFF);
    obs_data_set_int(settings, "opacity", 100);
    obs_data_set_bool(settings, "outline", true);
    obs_data_set_int(settings, "outline_size", 2);
    obs_data_set_int(settings, "outline_color", 0x000000);
    obs_data_set_int(settings, "outline_opacity", 100);

    // Background
    obs_data_set_int(settings, "bk_color", 0x000000);
    obs_data_set_int(settings, "bk_opacity", ctx->show_background ? 80 : 0);

    obs_source_update(ctx->text_source, settings);

    obs_data_release(font_obj);
    obs_data_release(settings);
}

static void *fps_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(source);
    struct fps_overlay_source *ctx = (fps_overlay_source *)bzalloc(sizeof(struct fps_overlay_source));

    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_background = obs_data_get_bool(settings, "show_background");

    ctx->last_text[0] = '\0';

    // Create private text_gdiplus source (not visible in OBS source list)
    obs_data_t *text_settings = obs_data_create();
    obs_data_set_string(text_settings, "text", "Initializing...");
    ctx->text_source = obs_source_create_private("text_gdiplus", "fps_overlay_text", text_settings);
    obs_data_release(text_settings);

    if (ctx->text_source) {
        update_text_source(ctx, "Initializing...");
    }

    return ctx;
}

static void fps_overlay_destroy(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    if (ctx) {
        if (ctx->text_source)
            obs_source_release(ctx->text_source);
    }
    bfree(data);
}

static obs_properties_t *fps_overlay_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *props = obs_properties_create();

    obs_property_t *font = obs_properties_add_list(
        props, "font_size", "Font size",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(font, "16", 16);
    obs_property_list_add_int(font, "24", 24);
    obs_property_list_add_int(font, "32", 32);
    obs_property_list_add_int(font, "48", 48);
    obs_property_list_add_int(font, "64", 64);

    obs_properties_add_bool(props, "show_background", "Show background");

    return props;
}

static void fps_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "font_size", 64);
    obs_data_set_default_bool(settings, "show_background", true);
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_background = obs_data_get_bool(settings, "show_background");

    // Force re-render with new settings
    update_text_source(ctx, ctx->last_text[0] ? ctx->last_text : "FPS: --");
}

static void fps_overlay_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;

    char text[512];

    if (g_fps_shared.active_filter_count <= 0) {
        snprintf(text, sizeof(text),
            "No FPS Analyzer filter active.\n"
            "Add the \"FPS Analyzer 0.3\" filter\n"
            "to a video source to start.");
    } else if (g_fps_shared.active_filter_count > 1) {
        snprintf(text, sizeof(text),
            "Warning: %d FPS Analyzer filters active.\n"
            "Use only one filter at a time\n"
            "for accurate results.",
            g_fps_shared.active_filter_count);
    } else if (g_fps_shared.unsupported_format >= 0) {
        snprintf(text, sizeof(text),
            "Unsupported video format (id: %d).\n"
            "Supported: NV12, I420, I422, I444,\n"
            "YUY2, UYVY, BGRA, RGBA.",
            g_fps_shared.unsupported_format);
    } else {
        if (g_fps_shared.tearing_detected) {
            snprintf(text, sizeof(text),
                "FPS: %d\nFrametime: %.2f ms\nWarning: Tearing detected",
                g_fps_shared.fps, g_fps_shared.frametime_ms);
        } else {
            snprintf(text, sizeof(text),
                "FPS: %d\nFrametime: %.2f ms",
                g_fps_shared.fps, g_fps_shared.frametime_ms);
        }
    }

    // Only update the text source if the text actually changed
    if (strcmp(text, ctx->last_text) != 0) {
        strncpy(ctx->last_text, text, sizeof(ctx->last_text));
        ctx->last_text[sizeof(ctx->last_text) - 1] = '\0';
        update_text_source(ctx, text);
    }
}

static void fps_overlay_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    if (ctx->text_source)
        obs_source_video_render(ctx->text_source);
}

static uint32_t fps_overlay_get_width(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    if (ctx->text_source)
        return obs_source_get_width(ctx->text_source);
    return 0;
}

static uint32_t fps_overlay_get_height(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    if (ctx->text_source)
        return obs_source_get_height(ctx->text_source);
    return 0;
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
    .get_defaults = fps_overlay_get_defaults,
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
    blog(LOG_INFO, "FPS Analyzer 0.3 loaded");
    return true;
}
