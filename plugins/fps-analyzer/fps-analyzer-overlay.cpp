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

#define GRAPH_PLOT_WIDTH 1920
#define GRAPH_PLOT_HEIGHT 360
#define GRAPH_MARGIN 20
#define GRAPH_LEGEND_WIDTH 80
#define GRAPH_TOTAL_WIDTH (GRAPH_MARGIN + GRAPH_PLOT_WIDTH + GRAPH_MARGIN + GRAPH_LEGEND_WIDTH)
#define GRAPH_TOTAL_HEIGHT (GRAPH_MARGIN + GRAPH_PLOT_HEIGHT + GRAPH_MARGIN)
#define LINE_THICKNESS 2

struct fps_overlay_source
{
    obs_source_t *text_source;
    obs_source_t *label_frametime;
    obs_source_t *label_fps;
    // Reference line labels
    obs_source_t *label_ft_ref1; // "16.67ms"
    obs_source_t *label_ft_ref2; // "33.33ms"
    obs_source_t *label_fps_ref1; // "60"
    obs_source_t *label_fps_ref2; // "30"
    int font_size;
    bool show_background;
    bool show_frametime_graph;
    bool show_fps_graph;
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

static obs_source_t *create_label_source(const char *text, const char *name, int size, bool bold)
{
    obs_data_t *font_obj = obs_data_create();
    obs_data_set_string(font_obj, "face", "Arial");
    obs_data_set_int(font_obj, "size", size);
    obs_data_set_int(font_obj, "flags", bold ? 1 : 0);
    obs_data_set_string(font_obj, "style", bold ? "Bold" : "Regular");

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_data_set_obj(settings, "font", font_obj);
    obs_data_set_int(settings, "color1", 0xFFFFFF);
    obs_data_set_int(settings, "color2", 0xFFFFFF);
    obs_data_set_int(settings, "opacity", 80);
    obs_data_set_bool(settings, "outline", true);
    obs_data_set_int(settings, "outline_size", 1);
    obs_data_set_int(settings, "outline_color", 0x000000);
    obs_data_set_int(settings, "outline_opacity", 100);

    obs_source_t *src = obs_source_create_private("text_gdiplus", name, settings);

    obs_data_release(font_obj);
    obs_data_release(settings);
    return src;
}

// --- Graph rendering ---

// ref_label1/ref_label2: text sources for reference line labels
static void render_line_graph(const double *values, int count,
                              double ref1, double ref2,
                              bool show_tearing, bool higher_is_better,
                              double green_thresh, double yellow_thresh,
                              obs_source_t *ref_label1, obs_source_t *ref_label2)
{
    if (count < 2)
        return;

    int gh = GRAPH_PLOT_HEIGHT;
    int gw = GRAPH_PLOT_WIDTH;
    float step = (float)gw / (float)(FPS_GRAPH_HISTORY - 1);

    // Find max value for scaling
    double max_val = 1.0;
    for (int i = 0; i < count; i++) {
        if (values[i] > max_val)
            max_val = values[i];
    }
    if (ref1 > 0 && ref1 * 1.1 > max_val) max_val = ref1 * 1.1;
    if (ref2 > 0 && ref2 * 1.1 > max_val) max_val = ref2 * 1.1;

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!solid) return;
    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
    gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
    if (!color_param || !tech) return;

    struct vec4 col;

    // Compute reference line Y positions (in plot coordinates)
    int y_ref1 = (ref1 > 0) ? gh - (int)((ref1 / max_val) * gh) : -1;
    int y_ref2 = (ref2 > 0) ? gh - (int)((ref2 / max_val) * gh) : -1;

    // === PASS 1: All solid geometry ===
    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    // Panel background
    vec4_set(&col, 0.0f, 0.0f, 0.0f, 0.8f);
    gs_effect_set_vec4(color_param, &col);
    gs_draw_sprite(0, 0, (uint32_t)GRAPH_TOTAL_WIDTH, (uint32_t)GRAPH_TOTAL_HEIGHT);

    // Enter plot area
    gs_matrix_push();
    gs_matrix_translate3f((float)GRAPH_MARGIN, (float)GRAPH_MARGIN, 0.0f);

    // Reference lines
    vec4_set(&col, 1.0f, 1.0f, 1.0f, 0.3f);
    gs_effect_set_vec4(color_param, &col);
    if (y_ref1 >= 0 && y_ref1 < gh) {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_ref1, 0.0f);
        gs_draw_sprite(0, 0, (uint32_t)gw, 1);
        gs_matrix_pop();
    }
    if (y_ref2 >= 0 && y_ref2 < gh) {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_ref2, 0.0f);
        gs_draw_sprite(0, 0, (uint32_t)gw, 1);
        gs_matrix_pop();
    }

    // Tearing indicators
    int data_offset = FPS_GRAPH_HISTORY - count;
    if (show_tearing) {
        vec4_set(&col, 1.0f, 0.0f, 0.0f, 0.4f);
        gs_effect_set_vec4(color_param, &col);
        for (int i = 0; i < count; i++) {
            if (g_fps_shared.graph_tearing[i]) {
                float x = (float)(data_offset + i) * step;
                int seg_w = (int)(step + 1.0f);
                if (seg_w < 2) seg_w = 2;
                gs_matrix_push();
                gs_matrix_translate3f(x, 0.0f, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)seg_w, (uint32_t)gh);
                gs_matrix_pop();
            }
        }
    }

    // Data line
    for (int i = 0; i < count - 1; i++) {
        double v0 = values[i];
        double v1 = values[i + 1];
        float x0 = (float)(data_offset + i) * step;
        float x1 = (float)(data_offset + i + 1) * step;
        float fy0 = (float)(gh - (v0 / max_val) * gh);
        float fy1 = (float)(gh - (v1 / max_val) * gh);
        if (fy0 < 0) fy0 = 0; if (fy0 > gh) fy0 = (float)gh;
        if (fy1 < 0) fy1 = 0; if (fy1 > gh) fy1 = (float)gh;

        double v_worst = higher_is_better ? (v0 < v1 ? v0 : v1) : (v0 > v1 ? v0 : v1);
        if (higher_is_better) {
            if (v_worst >= green_thresh) vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v_worst >= yellow_thresh) vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        } else {
            if (v_worst <= green_thresh) vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v_worst <= yellow_thresh) vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        gs_effect_set_vec4(color_param, &col);

        float top = fy0 < fy1 ? fy0 : fy1;
        float bot = fy0 > fy1 ? fy0 : fy1;
        float seg_h = bot - top;
        if (seg_h < LINE_THICKNESS) seg_h = (float)LINE_THICKNESS;
        float seg_w = x1 - x0;
        if (seg_w < 1.0f) seg_w = 1.0f;

        gs_matrix_push();
        gs_matrix_translate3f(x0, top, 0.0f);
        gs_draw_sprite(0, 0, (uint32_t)(seg_w + 1.0f), (uint32_t)seg_h);
        gs_matrix_pop();
    }

    // Leave plot area
    gs_matrix_pop();

    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    // === PASS 2: Text labels (outside technique) ===

    // Reference label 1 — right side, aligned to ref line
    if (ref_label1 && y_ref1 >= 0 && y_ref1 < gh) {
        uint32_t lh = obs_source_get_height(ref_label1);
        gs_matrix_push();
        gs_matrix_translate3f((float)(GRAPH_MARGIN + gw + 8),
                              (float)(GRAPH_MARGIN + y_ref1 - (int)lh / 2), 0.0f);
        obs_source_video_render(ref_label1);
        gs_matrix_pop();
    }

    // Reference label 2
    if (ref_label2 && y_ref2 >= 0 && y_ref2 < gh) {
        uint32_t lh = obs_source_get_height(ref_label2);
        gs_matrix_push();
        gs_matrix_translate3f((float)(GRAPH_MARGIN + gw + 8),
                              (float)(GRAPH_MARGIN + y_ref2 - (int)lh / 2), 0.0f);
        obs_source_video_render(ref_label2);
        gs_matrix_pop();
    }
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
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");

    ctx->last_text[0] = '\0';

    // Create private text_gdiplus source (not visible in OBS source list)
    obs_data_t *text_settings = obs_data_create();
    obs_data_set_string(text_settings, "text", "Initializing...");
    ctx->text_source = obs_source_create_private("text_gdiplus", "fps_overlay_text", text_settings);
    obs_data_release(text_settings);

    if (ctx->text_source)
    {
        update_text_source(ctx, "Initializing...");
    }

    ctx->label_frametime = create_label_source("FRAMETIME", "fps_label_ft", 18, true);
    ctx->label_fps = create_label_source("FRAMERATE", "fps_label_fps", 18, true);
    ctx->label_ft_ref1 = create_label_source("16.67ms", "fps_label_ft_r1", 14, false);
    ctx->label_ft_ref2 = create_label_source("33.33ms", "fps_label_ft_r2", 14, false);
    ctx->label_fps_ref1 = create_label_source("60", "fps_label_fps_r1", 14, false);
    ctx->label_fps_ref2 = create_label_source("30", "fps_label_fps_r2", 14, false);

    return ctx;
}

static void fps_overlay_destroy(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    if (ctx)
    {
        if (ctx->text_source)
            obs_source_release(ctx->text_source);
        if (ctx->label_frametime)
            obs_source_release(ctx->label_frametime);
        if (ctx->label_fps)
            obs_source_release(ctx->label_fps);
        if (ctx->label_ft_ref1)
            obs_source_release(ctx->label_ft_ref1);
        if (ctx->label_ft_ref2)
            obs_source_release(ctx->label_ft_ref2);
        if (ctx->label_fps_ref1)
            obs_source_release(ctx->label_fps_ref1);
        if (ctx->label_fps_ref2)
            obs_source_release(ctx->label_fps_ref2);
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

    obs_properties_add_bool(props, "show_frametime_graph", "Show frametime graph");
    obs_properties_add_bool(props, "show_fps_graph", "Show framerate graph");

    return props;
}

static void fps_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "font_size", 64);
    obs_data_set_default_bool(settings, "show_background", true);
    obs_data_set_default_bool(settings, "show_frametime_graph", false);
    obs_data_set_default_bool(settings, "show_fps_graph", false);
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_background = obs_data_get_bool(settings, "show_background");
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");

    // Force re-render with new settings
    update_text_source(ctx, ctx->last_text[0] ? ctx->last_text : "FPS: --");
}

static void fps_overlay_tick(void *data, float seconds)
{
    UNUSED_PARAMETER(seconds);
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;

    char text[512];

    if (g_fps_shared.active_filter_count <= 0)
    {
        snprintf(text, sizeof(text),
                 "No FPS Analyzer filter active.\n"
                 "Add the \"FPS Analyzer 0.4\" filter\n"
                 "to a video source to start.");
    }
    else if (g_fps_shared.active_filter_count > 1)
    {
        snprintf(text, sizeof(text),
                 "Warning: %d FPS Analyzer filters active.\n"
                 "Use only one filter at a time\n"
                 "for accurate results.",
                 g_fps_shared.active_filter_count);
    }
    else if (g_fps_shared.unsupported_format >= 0)
    {
        snprintf(text, sizeof(text),
                 "Unsupported video format (id: %d).\n"
                 "Supported: NV12, I420, I422, I444,\n"
                 "YUY2, UYVY, BGRA, RGBA.",
                 g_fps_shared.unsupported_format);
    }
    else
    {
        if (g_fps_shared.tearing_detected)
        {
            snprintf(text, sizeof(text),
                     "FPS: %d\nFrametime: %.2f ms\nWarning: Tearing detected",
                     g_fps_shared.fps, g_fps_shared.frametime_ms);
        }
        else
        {
            snprintf(text, sizeof(text),
                     "FPS: %d\nFrametime: %.2f ms",
                     g_fps_shared.fps, g_fps_shared.frametime_ms);
        }
    }

    // Only update the text source if the text actually changed
    if (strcmp(text, ctx->last_text) != 0)
    {
        strncpy(ctx->last_text, text, sizeof(ctx->last_text));
        ctx->last_text[sizeof(ctx->last_text) - 1] = '\0';
        update_text_source(ctx, text);
    }
}

static void fps_overlay_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    int count = g_fps_shared.graph_count;
    bool any_graph = (ctx->show_frametime_graph || ctx->show_fps_graph) && count >= 2;

    // 1. Render text at top
    if (ctx->text_source)
        obs_source_video_render(ctx->text_source);

    if (!any_graph)
        return;

    uint32_t y_offset = ctx->text_source ? obs_source_get_height(ctx->text_source) : 0;

    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    // 2. Frametime graph
    if (ctx->show_frametime_graph)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        render_line_graph(g_fps_shared.graph_frametimes, count,
                          16.67, 33.33, true, false, 16.67, 33.33,
                          ctx->label_ft_ref1, ctx->label_ft_ref2);
        // Title label
        if (ctx->label_frametime)
        {
            gs_matrix_push();
            gs_matrix_translate3f((float)GRAPH_MARGIN, 2.0f, 0.0f);
            obs_source_video_render(ctx->label_frametime);
            gs_matrix_pop();
        }
        gs_matrix_pop();
        y_offset += GRAPH_TOTAL_HEIGHT;
    }

    // 3. FPS graph
    if (ctx->show_fps_graph)
    {
        // Smooth FPS: window size = current FPS (1 second of samples)
        double fps_values[FPS_GRAPH_HISTORY];
        int smooth_window = g_fps_shared.fps > 0 ? g_fps_shared.fps : 30;
        if (smooth_window < 5)
            smooth_window = 5;
        if (smooth_window > count)
            smooth_window = count;
        for (int i = 0; i < count; i++)
        {
            double sum = 0.0;
            int n = 0;
            for (int j = i - smooth_window + 1; j <= i; j++)
            {
                if (j >= 0 && j < count)
                {
                    sum += g_fps_shared.graph_frametimes[j];
                    n++;
                }
            }
            double avg_ft = (n > 0) ? (sum / n) : 0.0;
            fps_values[i] = (avg_ft > 0.0) ? round(1000.0 / avg_ft) : 0.0;
        }

        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        render_line_graph(fps_values, count,
                          60.0, 30.0, true, true, 60.0, 30.0,
                          ctx->label_fps_ref1, ctx->label_fps_ref2);
        // Title label
        if (ctx->label_fps)
        {
            gs_matrix_push();
            gs_matrix_translate3f((float)GRAPH_MARGIN, 2.0f, 0.0f);
            obs_source_video_render(ctx->label_fps);
            gs_matrix_pop();
        }
        gs_matrix_pop();
    }

    gs_blend_state_pop();
}

static uint32_t fps_overlay_get_width(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    uint32_t text_w = 0;
    if (ctx->text_source)
        text_w = obs_source_get_width(ctx->text_source);
    if (ctx->show_frametime_graph || ctx->show_fps_graph)
        return text_w > GRAPH_TOTAL_WIDTH ? text_w : GRAPH_TOTAL_WIDTH;
    return text_w;
}

static uint32_t fps_overlay_get_height(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    uint32_t text_h = 0;
    if (ctx->text_source)
        text_h = obs_source_get_height(ctx->text_source);
    int graphs = 0;
    if (ctx->show_frametime_graph)
        graphs++;
    if (ctx->show_fps_graph)
        graphs++;
    return text_h + (uint32_t)(graphs * GRAPH_TOTAL_HEIGHT);
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
