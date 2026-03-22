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

#define GRAPH_MARGIN 20
#define GRAPH_LEGEND_WIDTH 80
#define MAX_GRID_LABELS 16
#define LINE_THICKNESS 2

// Graph styles
#define GRAPH_STYLE_BIG 0
#define GRAPH_STYLE_COMPACT 1

// Big: 1920x360
#define BIG_PLOT_W 1920
#define BIG_PLOT_H 360
// Compact: 300x80
#define COMPACT_PLOT_W 300
#define COMPACT_PLOT_H 80

struct fps_overlay_source
{
    obs_source_t *text_source;
    obs_source_t *label_frametime;
    obs_source_t *label_fps;
    int font_size;
    bool show_text_background;
    bool show_fps_text;
    bool show_frametime_text;
    bool show_tearing_text;
    bool show_frametime_graph;
    bool show_fps_graph;
    int frametime_style; // GRAPH_STYLE_BIG or GRAPH_STYLE_COMPACT
    int fps_style;
    double frametime_scale; // 0 = auto, otherwise fixed max (e.g. 16.67, 33.33)
    double fps_scale;       // 0 = auto, otherwise fixed max (e.g. 60, 120)
    // Grid label pools
    obs_source_t *ft_grid_labels[MAX_GRID_LABELS];
    int ft_grid_count;
    double ft_grid_values[MAX_GRID_LABELS];
    obs_source_t *fps_grid_labels[MAX_GRID_LABELS];
    int fps_grid_count;
    double fps_grid_values[MAX_GRID_LABELS];
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
    obs_data_set_int(settings, "bk_opacity", ctx->show_text_background ? 80 : 0);

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

// Build grid labels for a given step and max value
// is_ms: true = format as "Xms", false = format as integer
static void build_grid_labels(obs_source_t **labels, double *values, int *out_count,
                              double step, double max_val, bool is_ms)
{
    // Free old labels
    for (int i = 0; i < *out_count; i++)
    {
        if (labels[i])
            obs_source_release(labels[i]);
        labels[i] = NULL;
    }
    *out_count = 0;

    if (step <= 0 || max_val <= 0)
        return;

    // Include 0 and go up to and including max_val
    // Use multiplication to avoid floating point accumulation errors
    for (int n = 0; n * step <= max_val + 0.01 && *out_count < MAX_GRID_LABELS; n++)
    {
        double v = n * step;

        char text[32];
        if (is_ms)
        {
            // Display as clean frametime: show what FPS this corresponds to
            double rounded = round(v * 100.0) / 100.0;
            // Snap common values: 8.33, 16.67, 33.33, 50.00, 66.67
            if (fabs(rounded - 8.33) < 0.02)
                rounded = 8.33;
            else if (fabs(rounded - 16.67) < 0.02)
                rounded = 16.67;
            else if (fabs(rounded - 33.33) < 0.02)
                rounded = 33.33;
            else if (fabs(rounded - 50.00) < 0.02)
                rounded = 50.00;
            else if (fabs(rounded - 66.67) < 0.02)
                rounded = 66.67;
            snprintf(text, sizeof(text), "%.2fms", rounded);
        }
        else
            snprintf(text, sizeof(text), "%d", (int)round(v));

        char name[64];
        snprintf(name, sizeof(name), "grid_%d", *out_count);

        values[*out_count] = v;
        labels[*out_count] = create_label_source(text, name, 18, true);
        (*out_count)++;
    }
}

static void get_graph_dims(int style, int *pw, int *ph, int *total_w, int *total_h)
{
    if (style == GRAPH_STYLE_COMPACT)
    {
        *pw = COMPACT_PLOT_W;
        *ph = COMPACT_PLOT_H;
    }
    else
    {
        *pw = BIG_PLOT_W;
        *ph = BIG_PLOT_H;
    }
    *total_w = GRAPH_MARGIN + *pw + GRAPH_MARGIN + GRAPH_LEGEND_WIDTH;
    *total_h = GRAPH_MARGIN + *ph + GRAPH_MARGIN;
}

// --- Graph rendering ---

// ref_label1/ref_label2: text sources for reference line labels
// max_override: if >0, use as fixed Y-axis max; if 0, auto-scale
// ref_step: distance between reference lines (e.g. 10 for every 10 units). 0 = no grid.
static void render_line_graph(const double *values, int count,
                              double ref_step,
                              bool show_tearing, bool higher_is_better,
                              double green_thresh, double yellow_thresh,
                              double max_override,
                              obs_source_t **grid_labels, double *grid_values, int grid_count,
                              int style)
{
    if (count < 2)
        return;

    int gw, gh, total_w, total_h;
    get_graph_dims(style, &gw, &gh, &total_w, &total_h);
    float step = (float)gw / (float)(FPS_GRAPH_HISTORY - 1);

    // Y-axis scaling
    double max_val;
    if (max_override > 0)
    {
        max_val = max_override;
    }
    else
    {
        max_val = 1.0;
        for (int i = 0; i < count; i++)
        {
            if (values[i] > max_val)
                max_val = values[i];
        }
        max_val *= 1.1; // 10% headroom
    }

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!solid)
        return;
    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
    gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
    if (!color_param || !tech)
        return;

    struct vec4 col;

    // === PASS 1: All solid geometry ===
    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    // Panel background
    vec4_set(&col, 0.0f, 0.0f, 0.0f, 0.8f);
    gs_effect_set_vec4(color_param, &col);
    gs_draw_sprite(0, 0, (uint32_t)total_w, (uint32_t)total_h);

    // Enter plot area
    gs_matrix_push();
    gs_matrix_translate3f((float)GRAPH_MARGIN, (float)GRAPH_MARGIN, 0.0f);

    // Reference grid lines (including 0 at bottom)
    if (ref_step > 0)
    {
        vec4_set(&col, 1.0f, 1.0f, 1.0f, 0.15f);
        gs_effect_set_vec4(color_param, &col);
        for (int n = 0; n * ref_step <= max_val + 0.01; n++)
        {
            double v = n * ref_step;
            int y_ref = gh - (int)((v / max_val) * gh);
            if (y_ref >= 0 && y_ref < gh)
            {
                gs_matrix_push();
                gs_matrix_translate3f(0.0f, (float)y_ref, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)gw, 1);
                gs_matrix_pop();
            }
        }
    }

    // Tearing indicators
    int data_offset = FPS_GRAPH_HISTORY - count;
    if (show_tearing)
    {
        vec4_set(&col, 1.0f, 0.0f, 0.0f, 0.4f);
        gs_effect_set_vec4(color_param, &col);
        for (int i = 0; i < count; i++)
        {
            if (g_fps_shared.graph_tearing[i])
            {
                float x = (float)(data_offset + i) * step;
                int seg_w = (int)(step + 1.0f);
                if (seg_w < 2)
                    seg_w = 2;
                gs_matrix_push();
                gs_matrix_translate3f(x, 0.0f, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)seg_w, (uint32_t)gh);
                gs_matrix_pop();
            }
        }
    }

    // Data line
    for (int i = 0; i < count - 1; i++)
    {
        double v0 = values[i];
        double v1 = values[i + 1];
        float x0 = (float)(data_offset + i) * step;
        float x1 = (float)(data_offset + i + 1) * step;
        float fy0 = (float)(gh - (v0 / max_val) * gh);
        float fy1 = (float)(gh - (v1 / max_val) * gh);
        if (fy0 < 0)
            fy0 = 0;
        if (fy0 > gh)
            fy0 = (float)gh;
        if (fy1 < 0)
            fy1 = 0;
        if (fy1 > gh)
            fy1 = (float)gh;

        double v_worst = higher_is_better ? (v0 < v1 ? v0 : v1) : (v0 > v1 ? v0 : v1);
        if (higher_is_better)
        {
            if (v_worst >= green_thresh)
                vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v_worst >= yellow_thresh)
                vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else
                vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            if (v_worst <= green_thresh)
                vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v_worst <= yellow_thresh)
                vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else
                vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        gs_effect_set_vec4(color_param, &col);

        float top = fy0 < fy1 ? fy0 : fy1;
        float bot = fy0 > fy1 ? fy0 : fy1;
        float seg_h = bot - top;
        if (seg_h < LINE_THICKNESS)
            seg_h = (float)LINE_THICKNESS;
        float seg_w = x1 - x0;
        if (seg_w < 1.0f)
            seg_w = 1.0f;

        gs_matrix_push();
        gs_matrix_translate3f(x0, top, 0.0f);
        gs_draw_sprite(0, 0, (uint32_t)(seg_w + 1.0f), (uint32_t)seg_h);
        gs_matrix_pop();
    }

    // Leave plot area
    gs_matrix_pop();

    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    // Grid labels — rendered right of plot area
    for (int g = 0; g < grid_count; g++)
    {
        if (!grid_labels[g])
            continue;
        double v = grid_values[g];
        int y_ref = gh - (int)((v / max_val) * gh);
        if (y_ref < 0 || y_ref >= gh)
            continue;

        uint32_t lh = obs_source_get_height(grid_labels[g]);
        gs_matrix_push();
        gs_matrix_translate3f((float)(GRAPH_MARGIN + gw + 8),
                              (float)(GRAPH_MARGIN + y_ref - (int)lh / 2), 0.0f);
        obs_source_video_render(grid_labels[g]);
        gs_matrix_pop();
    }
}

static void rebuild_grid_labels(struct fps_overlay_source *ctx)
{
    // Frametime grid — step depends on scale range
    double ft_max = ctx->frametime_scale > 0 ? ctx->frametime_scale : 50.0;
    double ft_step = (ft_max > 33.33) ? (1000.0 / 60.0) : (1000.0 / 120.0);
    build_grid_labels(ctx->ft_grid_labels, ctx->ft_grid_values, &ctx->ft_grid_count,
                      ft_step, ft_max, true);

    // FPS grid
    double fps_max = ctx->fps_scale > 0 ? ctx->fps_scale : 120.0;
    double fps_step = 0;
    if (fps_max >= 120.0)
        fps_step = 20.0;
    else if (fps_max >= 60.0)
        fps_step = 10.0;
    else
        fps_step = 10.0;
    build_grid_labels(ctx->fps_grid_labels, ctx->fps_grid_values, &ctx->fps_grid_count,
                      fps_step, fps_max, false);
}

// --- Source callbacks ---

static void *fps_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    UNUSED_PARAMETER(source);
    struct fps_overlay_source *ctx = (fps_overlay_source *)bzalloc(sizeof(struct fps_overlay_source));

    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_text_background = obs_data_get_bool(settings, "show_text_background");
    ctx->show_fps_text = obs_data_get_bool(settings, "show_fps_text");
    ctx->show_frametime_text = obs_data_get_bool(settings, "show_frametime_text");
    ctx->show_tearing_text = obs_data_get_bool(settings, "show_tearing_text");
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->frametime_style = (int)obs_data_get_int(settings, "frametime_style");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");
    ctx->fps_style = (int)obs_data_get_int(settings, "fps_style");
    ctx->frametime_scale = obs_data_get_double(settings, "frametime_scale");
    ctx->fps_scale = obs_data_get_double(settings, "fps_scale");

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
    rebuild_grid_labels(ctx);

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
        for (int i = 0; i < ctx->ft_grid_count; i++)
            if (ctx->ft_grid_labels[i])
                obs_source_release(ctx->ft_grid_labels[i]);
        for (int i = 0; i < ctx->fps_grid_count; i++)
            if (ctx->fps_grid_labels[i])
                obs_source_release(ctx->fps_grid_labels[i]);
    }
    bfree(data);
}

static bool graph_toggle_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
    UNUSED_PARAMETER(p);
    bool ft_on = obs_data_get_bool(settings, "show_frametime_graph");
    bool fps_on = obs_data_get_bool(settings, "show_fps_graph");
    obs_property_set_visible(obs_properties_get(props, "frametime_style"), ft_on);
    obs_property_set_visible(obs_properties_get(props, "frametime_scale"), ft_on);
    obs_property_set_visible(obs_properties_get(props, "fps_style"), fps_on);
    obs_property_set_visible(obs_properties_get(props, "fps_scale"), fps_on);
    return true;
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

    obs_properties_add_bool(props, "show_fps_text", "Show FPS text");
    obs_properties_add_bool(props, "show_frametime_text", "Show Frametime text");
    obs_properties_add_bool(props, "show_tearing_text", "Show Tearing warning");
    obs_properties_add_bool(props, "show_text_background", "Show text background");

    obs_property_t *ft_toggle = obs_properties_add_bool(props, "show_frametime_graph", "Show frametime graph");
    obs_property_set_modified_callback(ft_toggle, graph_toggle_modified);

    obs_property_t *ft_style = obs_properties_add_list(
        props, "frametime_style", "Frametime graph style",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ft_style, "Big (1920x360)", GRAPH_STYLE_BIG);
    obs_property_list_add_int(ft_style, "Compact (300x80)", GRAPH_STYLE_COMPACT);

    obs_property_t *ft_scale = obs_properties_add_list(
        props, "frametime_scale", "Frametime scale",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(ft_scale, "Auto", 0.0);
    obs_property_list_add_float(ft_scale, "16.67 ms", 16.67);
    obs_property_list_add_float(ft_scale, "33.33 ms", 33.33);
    obs_property_list_add_float(ft_scale, "66.67 ms", 66.67);

    obs_property_t *fps_toggle = obs_properties_add_bool(props, "show_fps_graph", "Show framerate graph");
    obs_property_set_modified_callback(fps_toggle, graph_toggle_modified);

    obs_property_t *fps_style_prop = obs_properties_add_list(
        props, "fps_style", "Framerate graph style",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(fps_style_prop, "Big (1920x360)", GRAPH_STYLE_BIG);
    obs_property_list_add_int(fps_style_prop, "Compact (300x80)", GRAPH_STYLE_COMPACT);

    obs_property_t *fps_scale = obs_properties_add_list(
        props, "fps_scale", "Framerate scale",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(fps_scale, "Auto", 0.0);
    obs_property_list_add_float(fps_scale, "120 FPS", 120.0);
    obs_property_list_add_float(fps_scale, "60 FPS", 60.0);
    obs_property_list_add_float(fps_scale, "30 FPS", 30.0);

    // Initial visibility
    bool ft_on = data ? ((struct fps_overlay_source *)data)->show_frametime_graph : false;
    bool fps_on = data ? ((struct fps_overlay_source *)data)->show_fps_graph : false;
    obs_property_set_visible(ft_style, ft_on);
    obs_property_set_visible(ft_scale, ft_on);
    obs_property_set_visible(fps_style_prop, fps_on);
    obs_property_set_visible(fps_scale, fps_on);

    return props;
}

static void fps_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "font_size", 64);
    obs_data_set_default_bool(settings, "show_fps_text", true);
    obs_data_set_default_bool(settings, "show_frametime_text", true);
    obs_data_set_default_bool(settings, "show_tearing_text", true);
    obs_data_set_default_bool(settings, "show_text_background", true);
    obs_data_set_default_bool(settings, "show_frametime_graph", false);
    obs_data_set_default_int(settings, "frametime_style", GRAPH_STYLE_BIG);
    obs_data_set_default_double(settings, "frametime_scale", 0.0);
    obs_data_set_default_bool(settings, "show_fps_graph", false);
    obs_data_set_default_int(settings, "fps_style", GRAPH_STYLE_BIG);
    obs_data_set_default_double(settings, "fps_scale", 0.0);
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 64;
    ctx->show_text_background = obs_data_get_bool(settings, "show_text_background");
    ctx->show_fps_text = obs_data_get_bool(settings, "show_fps_text");
    ctx->show_frametime_text = obs_data_get_bool(settings, "show_frametime_text");
    ctx->show_tearing_text = obs_data_get_bool(settings, "show_tearing_text");
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->frametime_style = (int)obs_data_get_int(settings, "frametime_style");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");
    ctx->fps_style = (int)obs_data_get_int(settings, "fps_style");
    ctx->frametime_scale = obs_data_get_double(settings, "frametime_scale");
    ctx->fps_scale = obs_data_get_double(settings, "fps_scale");

    rebuild_grid_labels(ctx);

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
        text[0] = '\0';
        int pos = 0;
        if (ctx->show_fps_text)
            pos += snprintf(text + pos, sizeof(text) - pos, "FPS: %d", g_fps_shared.fps);
        if (ctx->show_frametime_text)
        {
            if (pos > 0)
                pos += snprintf(text + pos, sizeof(text) - pos, "\n");
            pos += snprintf(text + pos, sizeof(text) - pos, "Frametime: %.2f ms", g_fps_shared.frametime_ms);
        }
        if (ctx->show_tearing_text && g_fps_shared.tearing_detected)
        {
            if (pos > 0)
                pos += snprintf(text + pos, sizeof(text) - pos, "\n");
            pos += snprintf(text + pos, sizeof(text) - pos, "Warning: Tearing detected");
        }
        if (pos == 0)
            snprintf(text, sizeof(text), " "); // at least a space so text source has content
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

    // 1. Render text at top with margin
    bool any_text = ctx->show_fps_text || ctx->show_frametime_text || ctx->show_tearing_text;
    uint32_t y_offset = 0;
    if (any_text && ctx->text_source)
    {
        gs_matrix_push();
        gs_matrix_translate3f((float)GRAPH_MARGIN, (float)GRAPH_MARGIN, 0.0f);
        obs_source_video_render(ctx->text_source);
        gs_matrix_pop();
        y_offset = obs_source_get_height(ctx->text_source) + GRAPH_MARGIN * 2;
    }

    if (!any_graph)
        return;

    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    // 2. Frametime graph
    if (ctx->show_frametime_graph)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        double ft_step = (ctx->frametime_scale > 33.33) ? 16.67 : 8.33;

        render_line_graph(g_fps_shared.graph_frametimes, count,
                          ft_step, true, false, 16.67, 33.33,
                          ctx->frametime_scale,
                          ctx->ft_grid_labels, ctx->ft_grid_values, ctx->ft_grid_count,
                          ctx->frametime_style);
        // Title label
        if (ctx->label_frametime)
        {
            gs_matrix_push();
            gs_matrix_translate3f((float)GRAPH_MARGIN, 2.0f, 0.0f);
            obs_source_video_render(ctx->label_frametime);
            gs_matrix_pop();
        }
        gs_matrix_pop();
        {
            int pw, ph, tw, th;
            get_graph_dims(ctx->frametime_style, &pw, &ph, &tw, &th);
            y_offset += th + GRAPH_MARGIN;
        }
    }

    // 3. FPS graph
    if (ctx->show_fps_graph)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        // FPS ref_step: based on scale
        double fps_step = 0;
        if (ctx->fps_scale >= 120.0)
            fps_step = 20.0;
        else if (ctx->fps_scale >= 60.0)
            fps_step = 10.0;
        else if (ctx->fps_scale >= 30.0)
            fps_step = 10.0;
        else
            fps_step = 10.0; // auto: every 10 FPS

        render_line_graph(g_fps_shared.graph_fps, count,
                          fps_step, true, true, 60.0, 30.0,
                          ctx->fps_scale,
                          ctx->fps_grid_labels, ctx->fps_grid_values, ctx->fps_grid_count,
                          ctx->fps_style);
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
    bool any_text = ctx->show_fps_text || ctx->show_frametime_text || ctx->show_tearing_text;
    uint32_t text_w = 0;
    if (any_text && ctx->text_source)
        text_w = obs_source_get_width(ctx->text_source) + GRAPH_MARGIN * 2;
    if (ctx->show_frametime_graph || ctx->show_fps_graph)
    {
        int pw, ph, tw, th;
        uint32_t max_gw = 0;
        if (ctx->show_frametime_graph)
        {
            get_graph_dims(ctx->frametime_style, &pw, &ph, &tw, &th);
            if ((uint32_t)tw > max_gw)
                max_gw = (uint32_t)tw;
        }
        if (ctx->show_fps_graph)
        {
            get_graph_dims(ctx->fps_style, &pw, &ph, &tw, &th);
            if ((uint32_t)tw > max_gw)
                max_gw = (uint32_t)tw;
        }
        return text_w > max_gw ? text_w : max_gw;
    }
    return text_w;
}

static uint32_t fps_overlay_get_height(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    bool any_text = ctx->show_fps_text || ctx->show_frametime_text || ctx->show_tearing_text;
    uint32_t text_h = 0;
    if (any_text && ctx->text_source)
        text_h = obs_source_get_height(ctx->text_source) + GRAPH_MARGIN * 2;
    int graphs = 0;
    if (ctx->show_frametime_graph)
        graphs++;
    if (ctx->show_fps_graph)
        graphs++;
    uint32_t graph_h = 0;
    int pw, ph, tw, th;
    if (ctx->show_frametime_graph)
    {
        get_graph_dims(ctx->frametime_style, &pw, &ph, &tw, &th);
        graph_h += (uint32_t)th;
    }
    if (ctx->show_fps_graph)
    {
        get_graph_dims(ctx->fps_style, &pw, &ph, &tw, &th);
        if (ctx->show_frametime_graph)
            graph_h += GRAPH_MARGIN;
        graph_h += (uint32_t)th;
    }
    return text_h + graph_h;
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
