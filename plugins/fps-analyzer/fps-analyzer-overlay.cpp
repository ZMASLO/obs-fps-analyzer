#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "fps-shared-data.h"

// Declare filter info for registration
extern struct obs_source_info fps_analyzer_filter_info;

#define GRAPH_WIDTH 1920
#define GRAPH_HEIGHT 360
#define LINE_THICKNESS 2

struct fps_overlay_source {
    obs_source_t *text_source;
    int font_size;
    bool show_background;
    bool show_graph;
    char last_text[512];
};

static const char *fps_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.4";
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

// --- Graph rendering ---

static void render_frametime_graph(struct fps_overlay_source *ctx)
{
    UNUSED_PARAMETER(ctx);
    int count = g_fps_shared.graph_count;
    if (count < 2)
        return;

    int gh = GRAPH_HEIGHT;
    int gw = GRAPH_WIDTH;
    float step = (float)gw / (float)(FPS_GRAPH_HISTORY - 1);

    // Find max frametime for scaling (minimum 50ms so graph isn't too zoomed)
    double max_ft = 50.0;
    for (int i = 0; i < count; i++) {
        if (g_fps_shared.graph_frametimes[i] > max_ft)
            max_ft = g_fps_shared.graph_frametimes[i];
    }

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!solid)
        return;

    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
    gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
    if (!color_param || !tech)
        return;

    struct vec4 col;

    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    // Background — black 80% opacity
    vec4_set(&col, 0.0f, 0.0f, 0.0f, 0.8f);
    gs_effect_set_vec4(color_param, &col);
    gs_draw_sprite(0, 0, (uint32_t)gw, (uint32_t)gh);

    // Reference line 60fps (16.67ms) — white 30%
    {
        int y_60 = gh - (int)((16.67 / max_ft) * gh);
        if (y_60 >= 0 && y_60 < gh) {
            vec4_set(&col, 1.0f, 1.0f, 1.0f, 0.3f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f(0.0f, (float)y_60, 0.0f);
            gs_draw_sprite(0, 0, (uint32_t)gw, 1);
            gs_matrix_pop();
        }
    }

    // Reference line 30fps (33.33ms) — white 30%
    {
        int y_30 = gh - (int)((33.33 / max_ft) * gh);
        if (y_30 >= 0 && y_30 < gh) {
            vec4_set(&col, 1.0f, 1.0f, 1.0f, 0.3f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f(0.0f, (float)y_30, 0.0f);
            gs_draw_sprite(0, 0, (uint32_t)gw, 1);
            gs_matrix_pop();
        }
    }

    // Offset so data is right-aligned (newest samples at right edge)
    int offset = FPS_GRAPH_HISTORY - count;

    // Tearing indicators — full-height red bars where tearing was detected
    vec4_set(&col, 1.0f, 0.0f, 0.0f, 0.4f);
    gs_effect_set_vec4(color_param, &col);
    for (int i = 0; i < count; i++) {
        if (g_fps_shared.graph_tearing[i]) {
            float x = (float)(offset + i) * step;
            int seg_w = (int)(step + 1.0f);
            if (seg_w < 2) seg_w = 2;
            gs_matrix_push();
            gs_matrix_translate3f(x, 0.0f, 0.0f);
            gs_draw_sprite(0, 0, (uint32_t)seg_w, (uint32_t)gh);
            gs_matrix_pop();
        }
    }

    // Frametime line — connect consecutive points with filled segments
    for (int i = 0; i < count - 1; i++) {
        double ft0 = g_fps_shared.graph_frametimes[i];
        double ft1 = g_fps_shared.graph_frametimes[i + 1];

        float x0 = (float)(offset + i) * step;
        float x1 = (float)(offset + i + 1) * step;
        float y0 = (float)(gh - (ft0 / max_ft) * gh);
        float y1 = (float)(gh - (ft1 / max_ft) * gh);

        // Clamp
        if (y0 < 0) y0 = 0; if (y0 > gh) y0 = (float)gh;
        if (y1 < 0) y1 = 0; if (y1 > gh) y1 = (float)gh;

        // Color based on worse frametime of the pair
        double ft_worst = ft0 > ft1 ? ft0 : ft1;
        if (ft_worst <= 16.67)
            vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f); // green
        else if (ft_worst <= 33.33)
            vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f); // yellow
        else
            vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f); // red
        gs_effect_set_vec4(color_param, &col);

        // Draw filled rectangle covering the segment
        float top = y0 < y1 ? y0 : y1;
        float bot = y0 > y1 ? y0 : y1;
        float seg_h = bot - top;
        if (seg_h < LINE_THICKNESS) seg_h = (float)LINE_THICKNESS;
        float seg_w = x1 - x0;
        if (seg_w < 1.0f) seg_w = 1.0f;

        gs_matrix_push();
        gs_matrix_translate3f(x0, top, 0.0f);
        gs_draw_sprite(0, 0, (uint32_t)(seg_w + 1.0f), (uint32_t)seg_h);
        gs_matrix_pop();
    }

    gs_technique_end_pass(tech);
    gs_technique_end(tech);
}

// --- Source callbacks ---

static void *fps_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(source);
    struct fps_overlay_source *ctx = (fps_overlay_source *)bzalloc(sizeof(struct fps_overlay_source));

    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_background = obs_data_get_bool(settings, "show_background");
    ctx->show_graph = obs_data_get_bool(settings, "show_graph");

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

    obs_properties_add_bool(props, "show_graph", "Show frametime graph");

    return props;
}

static void fps_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "font_size", 64);
    obs_data_set_default_bool(settings, "show_background", true);
    obs_data_set_default_bool(settings, "show_graph", false);
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_background = obs_data_get_bool(settings, "show_background");
    ctx->show_graph = obs_data_get_bool(settings, "show_graph");

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
            "Add the \"FPS Analyzer 0.4\" filter\n"
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

    // 1. Render text at top
    if (ctx->text_source)
        obs_source_video_render(ctx->text_source);

    // 2. Render graph below text (separate effect setup)
    if (ctx->show_graph && g_fps_shared.graph_count > 0) {
        uint32_t text_h = ctx->text_source ? obs_source_get_height(ctx->text_source) : 0;

        gs_blend_state_push();
        gs_reset_blend_state();
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)text_h, 0.0f);
        render_frametime_graph(ctx);
        gs_matrix_pop();

        gs_blend_state_pop();
    }
}

static uint32_t fps_overlay_get_width(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    uint32_t text_w = 0;
    if (ctx->text_source)
        text_w = obs_source_get_width(ctx->text_source);
    if (ctx->show_graph)
        return text_w > GRAPH_WIDTH ? text_w : GRAPH_WIDTH;
    return text_w;
}

static uint32_t fps_overlay_get_height(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    uint32_t text_h = 0;
    if (ctx->text_source)
        text_h = obs_source_get_height(ctx->text_source);
    if (ctx->show_graph)
        return text_h + GRAPH_HEIGHT;
    return text_h;
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
    blog(LOG_INFO, "FPS Analyzer 0.4 loaded");
    return true;
}
