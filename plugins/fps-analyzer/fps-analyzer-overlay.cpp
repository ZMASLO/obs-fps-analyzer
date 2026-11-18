#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <algorithm>

#include "fps-core.h"
#include "fps-shared-data.h"

// Declare filter info for registration
extern struct obs_source_info fps_analyzer_filter_info;
// Global hotkeys owned by the filter module (fps-analyzer-filter.cpp)
void fps_analyzer_register_hotkeys(void);
void fps_analyzer_unregister_hotkeys(void);

#define GRAPH_MARGIN 20
#define GRAPH_LEGEND_WIDTH 80
#define MAX_GRID_LABELS 16
#define LINE_THICKNESS 2

// Graph styles
#define GRAPH_STYLE_BIG 0
#define GRAPH_STYLE_COMPACT 1
#define GRAPH_STYLE_CUSTOM 2

// Big: 1920x360
#define BIG_PLOT_W (1920 - 2 * GRAPH_MARGIN - GRAPH_LEGEND_WIDTH) // labels fit a 1920 canvas
#define BIG_PLOT_H 360
// Compact: 300x80
#define COMPACT_PLOT_W 300
#define COMPACT_PLOT_H 80

// Spectrum panel (DCT log-magnitude thumbnail + source-resolution markers)
#define SPEC_PAD 8
#define SPEC_LABEL_W 60
#define SPEC_LABEL_H 20
#define SPEC_TICK 10

struct fps_overlay_source
{
    obs_source_t *self;
    obs_source_t *text_source;
    obs_source_t *label_pacing;
    obs_source_t *label_frametime;
    obs_source_t *label_fps;
    int font_size;
    bool show_text_background;
    bool show_fps_text;
    bool show_frametime_text;
    bool show_tearing_text;
    bool show_resolution_text;
    bool show_resolution_spectrum;
    bool show_pacing_graph;
    bool show_frametime_graph;
    bool show_fps_graph;
    int pacing_style;
    double pacing_scale; // 0 = auto, otherwise fixed max in ms
    // Per graph: custom size (GRAPH_STYLE_CUSTOM), time window, inverted axis
    int pacing_width, pacing_height, frametime_width, frametime_height, fps_width, fps_height;
    double pacing_window, frametime_window, fps_window; // seconds shown
    bool pacing_invert, frametime_invert;
    double pc_label_step; // frame pacing grid label step in use (follows the game's frame time)
    int frametime_style; // GRAPH_STYLE_BIG or GRAPH_STYLE_COMPACT
    int fps_style;
    double frametime_scale; // 0 = auto, otherwise fixed max (e.g. 16.67, 33.33)
    double fps_scale;       // 0 = auto, otherwise fixed max (e.g. 60, 120)
    // Grid label pools
    obs_source_t *pc_grid_labels[MAX_GRID_LABELS];
    int pc_grid_count;
    double pc_grid_values[MAX_GRID_LABELS];
    obs_source_t *ft_grid_labels[MAX_GRID_LABELS];
    int ft_grid_count;
    double ft_grid_values[MAX_GRID_LABELS];
    obs_source_t *fps_grid_labels[MAX_GRID_LABELS];
    int fps_grid_count;
    double fps_grid_values[MAX_GRID_LABELS];
    char last_text[512];
    // Spectrum panel
    gs_texture_t *spec_tex;
    uint8_t *spec_bgra;
    uint32_t spec_uploaded_version;
    obs_source_t *spec_label_w;
    obs_source_t *spec_label_h;
    int spec_label_w_val, spec_label_h_val;
    // Source refresh rate line (own label: font size follows the overlay)
    bool show_source_rate;
    bool autohide_source_rate;
    obs_source_t *label_srate;
    char srate_text[256];
    uint64_t srate_ok_since_ns; // when the verdict became OK, 0 = not OK
    bool srate_autohidden;      // OK for long enough, line hidden
};

static const char *fps_overlay_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return "FPS Analyzer 0.5.2";
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

static obs_source_t *create_label_source_color(const char *text, const char *name, int size,
                                               bool bold, uint32_t color)
{
    obs_data_t *font_obj = obs_data_create();
    obs_data_set_string(font_obj, "face", "Arial");
    obs_data_set_int(font_obj, "size", size);
    obs_data_set_int(font_obj, "flags", bold ? 1 : 0);
    obs_data_set_string(font_obj, "style", bold ? "Bold" : "Regular");

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_data_set_obj(settings, "font", font_obj);
    obs_data_set_int(settings, "color1", color);
    obs_data_set_int(settings, "color2", color);
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

static obs_source_t *create_label_source(const char *text, const char *name, int size, bool bold)
{
    return create_label_source_color(text, name, size, bold, 0xFFFFFF);
}

static void set_label_text(obs_source_t *label, const char *text)
{
    if (!label)
        return;
    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", text);
    obs_source_update(label, settings);
    obs_data_release(settings);
}

// Is there a text block at the top? A blocked reading always shows its
// reason, whatever is ticked.
static bool overlay_any_text(const struct fps_overlay_source *ctx)
{
    return ctx->show_fps_text || ctx->show_frametime_text || ctx->show_tearing_text ||
           ctx->show_resolution_text || g_fps_shared.blocked_reason != FPS_BLOCK_NONE;
}

// --- Source refresh rate line ---

#define SRATE_AUTOHIDE_NS 5000000000ULL // OK shown this long before hiding

static bool srate_visible(struct fps_overlay_source *ctx)
{
    return ctx->show_source_rate && !ctx->srate_autohidden && ctx->label_srate && ctx->srate_text[0] &&
           g_fps_shared.active_filter_count == 1;
}

// Full settings (not just text): font size and background follow the overlay
static void update_srate_label(struct fps_overlay_source *ctx)
{
    if (!ctx->label_srate)
        return;
    int size = ctx->font_size * 2 / 3;
    if (size < 14)
        size = 14;

    obs_data_t *font_obj = obs_data_create();
    obs_data_set_string(font_obj, "face", "Arial");
    obs_data_set_int(font_obj, "size", size);
    obs_data_set_int(font_obj, "flags", 1); // bold
    obs_data_set_string(font_obj, "style", "Bold");

    obs_data_t *settings = obs_data_create();
    obs_data_set_string(settings, "text", ctx->srate_text[0] ? ctx->srate_text : " ");
    obs_data_set_obj(settings, "font", font_obj);
    obs_data_set_int(settings, "opacity", 100);
    obs_data_set_bool(settings, "outline", true);
    obs_data_set_int(settings, "outline_size", 2);
    obs_data_set_int(settings, "outline_color", 0x000000);
    obs_data_set_int(settings, "outline_opacity", 100);
    obs_data_set_int(settings, "bk_color", 0x000000);
    obs_data_set_int(settings, "bk_opacity", ctx->show_text_background ? 80 : 0);

    obs_source_update(ctx->label_srate, settings);

    obs_data_release(font_obj);
    obs_data_release(settings);
}

static void format_beat(double beat_s, char *buf, size_t size)
{
    if (beat_s >= 10.0)
        snprintf(buf, size, "every ~%.0f s", beat_s);
    else if (beat_s >= 1.0)
        snprintf(buf, size, "every ~%.1f s", beat_s);
    else
        snprintf(buf, size, "constantly");
}

// The cadence verdict: a frame repeats every P seconds although the game is
// steady. Says which setting to change (OBS FPS, the device FPS, or both).
static bool build_beat_text(char *text, size_t size)
{
    if (!g_fps_shared.beat_detected)
        return false;
    char sig[32], obs[32], dev[32], beat[32];
    srate_format_hz(g_fps_shared.beat_signal_hz, sig, sizeof(sig));
    srate_format_hz(g_fps_shared.srate_obs_hz, obs, sizeof(obs));
    format_beat(g_fps_shared.beat_period_s, beat, sizeof(beat));
    const char *who = g_fps_shared.beat_origin == FPS_BEAT_ORIGIN_OBS ? "OBS" : "capture device";

    if (g_fps_shared.beat_drift) {
        snprintf(text, size,
                 "Signal ~%s Hz | OBS: %s FPS - %s repeats a frame %s\n"
                 "Clock drift between source and PC, not fixable in settings",
                 sig, obs, who, beat);
        return true;
    }

    const double target = g_fps_shared.beat_signal_hz;
    const bool need_obs = fabs(g_fps_shared.srate_obs_hz / target - 1.0) > 1e-4;
    const bool need_dev = !g_fps_shared.device_matching && g_fps_shared.device_hz > 0.0 &&
                          fabs(g_fps_shared.device_hz / target - 1.0) > 1e-4;
    char line1[160];
    if (g_fps_shared.device_hz > 0.0 && !g_fps_shared.device_matching) {
        srate_format_hz(g_fps_shared.device_hz, dev, sizeof(dev));
        snprintf(line1, sizeof(line1), "Signal ~%s Hz | Device: %s | OBS: %s FPS - %s repeats a frame %s", sig,
                 dev, obs, who, beat);
    } else {
        snprintf(line1, sizeof(line1), "Signal ~%s Hz | OBS: %s FPS - %s repeats a frame %s", sig, obs, who,
                 beat);
    }
    if (need_obs && need_dev)
        snprintf(text, size, "%s\nSet the capture device FPS and OBS FPS to %s", line1, sig);
    else if (need_dev)
        snprintf(text, size, "%s\nSet the capture device FPS to %s (source properties)", line1, sig);
    else if (need_obs)
        snprintf(text, size, "%s\nSet OBS FPS to %s (Settings > Video)", line1, sig);
    else
        snprintf(text, size, "%s", line1);
    return true;
}

static void build_srate_text(char *text, size_t size)
{
    char src[32], obs[32], rec[32], beat[32];
    if (build_beat_text(text, size))
        return;
    if (!g_fps_shared.srate_available)
    {
        snprintf(text, size, "Source rate: n/a (not a capture device)");
        return;
    }
    srate_format_hz(g_fps_shared.srate_source_hz, src, sizeof(src));
    srate_format_hz(g_fps_shared.srate_obs_hz, obs, sizeof(obs));

    // Timestamps that agree with OBS only prove the device is set to the OBS
    // rate (a card may stamp its configured rate whatever the signal is).
    // "OK" waits until the cadence check has ruled a 59.94-vs-60 repeat out.
    const bool ts_bad = g_fps_shared.srate_valid && (g_fps_shared.srate_status == SRATE_STATUS_MISMATCH ||
                                                    g_fps_shared.srate_status == SRATE_STATUS_UNSTABLE);
    if (!ts_bad && g_fps_shared.beat_status == FPS_BEAT_CLEAR) {
        snprintf(text, size, "Signal: %s Hz | OBS: %s FPS - OK", g_fps_shared.srate_valid ? src : obs, obs);
        return;
    }
    if (!ts_bad && g_fps_shared.beat_status == FPS_BEAT_CHECKING && g_fps_shared.beat_verify_s > 0.0) {
        char head[64] = "";
        if (g_fps_shared.device_hz > 0.0 && !g_fps_shared.device_matching) {
            char dev[32];
            srate_format_hz(g_fps_shared.device_hz, dev, sizeof(dev));
            snprintf(head, sizeof(head), "Device: %s | ", dev);
        }
        double shown = g_fps_shared.beat_observed_s;
        if (shown > g_fps_shared.beat_verify_s)
            shown = g_fps_shared.beat_verify_s; // waiting out a suspect repeat
        snprintf(text, size, "%sOBS: %s FPS - checking signal... %.0f/%.0f s%s", head, obs, floor(shown),
                 g_fps_shared.beat_verify_s,
                 g_fps_shared.beat_uneven ? "\nGame frame pacing is uneven, the check takes longer" : "");
        return;
    }

    switch (g_fps_shared.srate_status)
    {
    case SRATE_STATUS_MATCHED:
        snprintf(text, size, "Source: %s Hz | OBS: %s FPS - OK", src, obs);
        break;
    case SRATE_STATUS_MISMATCH:
        srate_format_hz(g_fps_shared.srate_recommended_hz, rec, sizeof(rec));
        format_beat(g_fps_shared.srate_beat_s, beat, sizeof(beat));
        snprintf(text, size,
                 "Source: %s Hz | OBS: %s FPS - duplicate %s\n"
                 "Set OBS FPS to %s (Settings > Video)",
                 src, obs, beat, rec);
        break;
    case SRATE_STATUS_UNSTABLE:
        snprintf(text, size, "Source rate: variable (no fixed refresh rate)");
        break;
    default:
        if (g_fps_shared.srate_source_hz > 0.0)
            snprintf(text, size, "Source rate: measuring... (~%s Hz)", src);
        else
            snprintf(text, size, "Source rate: measuring...");
        break;
    }
}

// --- Spectrum panel ---

static void get_spectrum_panel_dims(int *total_w, int *total_h)
{
    *total_w = SPEC_PAD + RESDET_SPEC_W + SPEC_PAD + SPEC_LABEL_W;
    *total_h = SPEC_PAD + RESDET_SPEC_H + SPEC_PAD + SPEC_LABEL_H;
}

static bool spectrum_visible(struct fps_overlay_source *ctx)
{
    return ctx->show_resolution_spectrum && g_fps_shared.res_detect_enabled &&
           g_fps_shared.active_filter_count == 1 && g_fps_shared.res_spectrum_version > 0;
}

// Draws the DCT log-magnitude thumbnail (DC top-left) with green ticks and
// labels at the detected source resolution, like the reference overlay.
static void render_spectrum_panel(struct fps_overlay_source *ctx)
{
    if (!ctx->spec_tex)
        ctx->spec_tex = gs_texture_create(RESDET_SPEC_W, RESDET_SPEC_H, GS_BGRA, 1, NULL, GS_DYNAMIC);
    if (!ctx->spec_tex || !ctx->spec_bgra)
        return;

    if (ctx->spec_uploaded_version != g_fps_shared.res_spectrum_version)
    {
        for (int i = 0; i < RESDET_SPEC_W * RESDET_SPEC_H; i++)
        {
            uint8_t *px = ctx->spec_bgra + i * 4;
            resdet_spectrum_color(g_fps_shared.res_spectrum[i], &px[0], &px[1], &px[2]);
            px[3] = 255;
        }
        gs_texture_set_image(ctx->spec_tex, ctx->spec_bgra, RESDET_SPEC_W * 4, false);
        ctx->spec_uploaded_version = g_fps_shared.res_spectrum_version;
    }

    int total_w, total_h;
    get_spectrum_panel_dims(&total_w, &total_h);

    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
    if (!solid || !def)
        return;
    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
    gs_technique_t *tech = gs_effect_get_technique(solid, "Solid");
    gs_eparam_t *image = gs_effect_get_param_by_name(def, "image");
    if (!color_param || !tech || !image)
        return;
    struct vec4 col;

    // Panel background
    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);
    vec4_set(&col, 0.0f, 0.0f, 0.0f, 0.8f);
    gs_effect_set_vec4(color_param, &col);
    gs_draw_sprite(0, 0, (uint32_t)total_w, (uint32_t)total_h);
    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    // Spectrum image
    gs_matrix_push();
    gs_matrix_translate3f((float)SPEC_PAD, (float)SPEC_PAD, 0.0f);
    gs_effect_set_texture(image, ctx->spec_tex);
    while (gs_effect_loop(def, "Draw"))
        gs_draw_sprite(ctx->spec_tex, 0, RESDET_SPEC_W, RESDET_SPEC_H);

    // Markers at the detected source resolution (spectrum index == pixels)
    int fw = g_fps_shared.res_frame_w, fh = g_fps_shared.res_frame_h;
    int sw = g_fps_shared.res_valid ? g_fps_shared.res_src_w : 0;
    int sh = g_fps_shared.res_valid ? g_fps_shared.res_src_h : 0;
    int x_pos = (sw > 0 && fw > 0) ? (int)((int64_t)sw * RESDET_SPEC_W / fw) : -1;
    int y_pos = (sh > 0 && fh > 0) ? (int)((int64_t)sh * RESDET_SPEC_H / fh) : -1;

    if (x_pos >= 0 || y_pos >= 0)
    {
        gs_technique_begin(tech);
        gs_technique_begin_pass(tech, 0);
        if (x_pos >= 0)
        {
            vec4_set(&col, 0.0f, 1.0f, 0.0f, 0.25f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f((float)x_pos, 0.0f, 0.0f);
            gs_draw_sprite(0, 0, 1, RESDET_SPEC_H);
            gs_matrix_pop();

            vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f((float)(x_pos - 1), 0.0f, 0.0f);
            gs_draw_sprite(0, 0, 2, SPEC_TICK);
            gs_matrix_pop();
            gs_matrix_push();
            gs_matrix_translate3f((float)(x_pos - 1), (float)(RESDET_SPEC_H - SPEC_TICK), 0.0f);
            gs_draw_sprite(0, 0, 2, SPEC_TICK);
            gs_matrix_pop();
        }
        if (y_pos >= 0)
        {
            vec4_set(&col, 0.0f, 1.0f, 0.0f, 0.25f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f(0.0f, (float)y_pos, 0.0f);
            gs_draw_sprite(0, 0, RESDET_SPEC_W, 1);
            gs_matrix_pop();

            vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            gs_effect_set_vec4(color_param, &col);
            gs_matrix_push();
            gs_matrix_translate3f(0.0f, (float)(y_pos - 1), 0.0f);
            gs_draw_sprite(0, 0, SPEC_TICK, 2);
            gs_matrix_pop();
            gs_matrix_push();
            gs_matrix_translate3f((float)(RESDET_SPEC_W - SPEC_TICK), (float)(y_pos - 1), 0.0f);
            gs_draw_sprite(0, 0, SPEC_TICK, 2);
            gs_matrix_pop();
        }
        gs_technique_end_pass(tech);
        gs_technique_end(tech);
    }
    gs_matrix_pop();

    // Labels outside the image: width below the bottom tick, height right of the right tick
    if (x_pos >= 0 && ctx->spec_label_w)
    {
        uint32_t lw = obs_source_get_width(ctx->spec_label_w);
        gs_matrix_push();
        gs_matrix_translate3f((float)(SPEC_PAD + x_pos) - (float)lw / 2.0f,
                              (float)(SPEC_PAD + RESDET_SPEC_H + 2), 0.0f);
        obs_source_video_render(ctx->spec_label_w);
        gs_matrix_pop();
    }
    if (y_pos >= 0 && ctx->spec_label_h)
    {
        uint32_t lh = obs_source_get_height(ctx->spec_label_h);
        gs_matrix_push();
        gs_matrix_translate3f((float)(SPEC_PAD + RESDET_SPEC_W + 6),
                              (float)(SPEC_PAD + y_pos) - (float)lh / 2.0f, 0.0f);
        obs_source_video_render(ctx->spec_label_h);
        gs_matrix_pop();
    }
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

static void get_graph_dims(int style, int *pw, int *ph, int *total_w, int *total_h, int custom_w = 0,
                           int custom_h = 0)
{
    if (style == GRAPH_STYLE_CUSTOM && custom_w > 0 && custom_h > 0)
    {
        *pw = custom_w;
        *ph = custom_h;
    }
    else if (style == GRAPH_STYLE_COMPACT)
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

// --- Graph rendering (time axis) ---

enum plot_field { PLOT_PACING = 0, PLOT_FRAMETIME = 1, PLOT_FPS = 2 };

static double plot_field_value(const struct plot_sample *s, int field)
{
    switch (field) {
    case PLOT_PACING: return s->pacing_ms;
    case PLOT_FRAMETIME: return s->ema_ms;
    default: return s->fps_pf;
    }
}

// One graph panel: the last window_s seconds of g_fps_shared.plot, the right
// edge is now. stepped: each sample is a flat step as wide as the frame was
// on screen (frame pacing); otherwise a line through the samples. inverted:
// 0 at the top (Digital Foundry's frame-time graph). max_override > 0 fixes
// the scale; 0 scales to the samples in the window (+10 %), pacing_auto picks
// 50 / 66.67 / 100 ms instead.
static void render_time_graph(int field, double ref_step, bool show_tearing, bool higher_is_better,
                              double green_thresh, double yellow_thresh, double max_override, bool pacing_auto,
                              obs_source_t **grid_labels, double *grid_values, int grid_count, int style,
                              int custom_w, int custom_h, double window_s, bool inverted, bool stepped)
{
    int gw, gh, total_w, total_h;
    get_graph_dims(style, &gw, &gh, &total_w, &total_h, custom_w, custom_h);

    static struct plot_span spans[FPS_PLOT_CAP];
    const double now_ms = (double)os_gettime_ns() / 1e6;
    const struct plot_ring *ring = &g_fps_shared.plot;
    int n = plot_window(ring, now_ms, window_s, (float)gw, spans, FPS_PLOT_CAP);

    // Framerate: computed at every other pixel column in time (frames on screen
    // in the second before it), not interpolated between frames - a pause then
    // drops to 0 instead of drawing a slope across it (plot_fps_curve)
    static float curve_x[4096];
    static double curve_v[4096];
    int curve_n = 0;
    if (field == PLOT_FPS)
        curve_n = plot_fps_curve(ring, now_ms, window_s, (float)gw, 2.0f, curve_x, curve_v, 4096);

    // Y-axis scaling from the values on screen
    double max_val;
    if (max_override > 0)
    {
        max_val = max_override;
    }
    else
    {
        double hi = 0.0;
        for (int i = 0; i < n && field != PLOT_FPS; i++)
        {
            double v = plot_field_value(plot_ring_at(ring, spans[i].idx), field);
            if (v > hi)
                hi = v;
        }
        for (int i = 0; i < curve_n; i++)
            if (curve_v[i] > hi)
                hi = curve_v[i];
        if (pacing_auto)
            max_val = hi <= 48.0 ? 50.0 : hi <= 64.0 ? 66.67 : 100.0;
        else
            max_val = hi > 1.0 ? hi * 1.1 : 1.1; // 10% headroom
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

    // Panel background
    vec4_set(&col, 0.0f, 0.0f, 0.0f, 0.8f);
    gs_effect_set_vec4(color_param, &col);
    gs_draw_sprite(0, 0, (uint32_t)total_w, (uint32_t)total_h);

    gs_matrix_push();
    gs_matrix_translate3f((float)GRAPH_MARGIN, (float)GRAPH_MARGIN, 0.0f);

    // Reference grid lines. At most ~12: an auto scale stretched by one long
    // sample would otherwise draw hundreds of them, merging into a grey block.
    double grid_step = ref_step;
    while (grid_step > 0 && max_val / grid_step > 12.0)
        grid_step *= 2.0;
    if (grid_step > 0)
    {
        vec4_set(&col, 1.0f, 1.0f, 1.0f, 0.15f);
        gs_effect_set_vec4(color_param, &col);
        for (int k = 0; k * grid_step <= max_val + 0.01; k++)
        {
            int y_ref = (int)plot_value_y(k * grid_step, max_val, (float)gh, inverted);
            if (y_ref >= 0 && y_ref < gh)
            {
                gs_matrix_push();
                gs_matrix_translate3f(0.0f, (float)y_ref, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)gw, 1);
                gs_matrix_pop();
            }
        }
    }

    // Tearing indicators: a band over the frame's time span
    if (show_tearing)
    {
        vec4_set(&col, 1.0f, 0.0f, 0.0f, 0.4f);
        gs_effect_set_vec4(color_param, &col);
        for (int i = 0; i < n; i++)
        {
            if (!plot_ring_at(ring, spans[i].idx)->tearing)
                continue;
            float w = spans[i].x1 - spans[i].x0;
            if (w < 2.0f)
                w = 2.0f;
            gs_matrix_push();
            gs_matrix_translate3f(spans[i].x0, 0.0f, 0.0f);
            gs_draw_sprite(0, 0, (uint32_t)w, (uint32_t)gh);
            gs_matrix_pop();
        }
    }

    auto set_color = [&](double v, bool gap) {
        if (gap)
            vec4_set(&col, 0.55f, 0.55f, 0.55f, 1.0f);
        else if (higher_is_better)
        {
            if (v >= green_thresh)
                vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v >= yellow_thresh)
                vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else
                vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            if (v <= green_thresh + 0.05)
                vec4_set(&col, 0.0f, 1.0f, 0.0f, 1.0f);
            else if (v <= yellow_thresh + 0.05)
                vec4_set(&col, 1.0f, 1.0f, 0.0f, 1.0f);
            else
                vec4_set(&col, 1.0f, 0.0f, 0.0f, 1.0f);
        }
        gs_effect_set_vec4(color_param, &col);
    };
    // Colour bucket of a value (same thresholds as set_color), for batching
    auto color_index = [&](double v) {
        if (higher_is_better)
            return v >= green_thresh ? 0 : v >= yellow_thresh ? 1 : 2;
        return v <= green_thresh + 0.05 ? 0 : v <= yellow_thresh + 0.05 ? 1 : 2;
    };
    // Two triangles: a band LINE_THICKNESS wide along a -> b (inside gs_render_start)
    auto band_vertices = [&](float ax, float ay, float bx, float by) {
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy);
        float h = LINE_THICKNESS * 0.5f;
        float nx = 0.0f, ny = h;
        if (len > 0.001f)
        {
            nx = -dy / len * h;
            ny = dx / len * h;
        }
        gs_vertex2f(ax + nx, ay + ny);
        gs_vertex2f(ax - nx, ay - ny);
        gs_vertex2f(bx + nx, by + ny);
        gs_vertex2f(bx + nx, by + ny);
        gs_vertex2f(ax - nx, ay - ny);
        gs_vertex2f(bx - nx, by - ny);
    };
    auto draw_segment = [&](float ax, float ay, float bx, float by) {
        gs_render_start(true);
        band_vertices(ax, ay, bx, by);
        gs_render_stop(GS_TRIS);
    };
    const float ymax = (float)(gh - LINE_THICKNESS);
    auto yof = [&](double v) {
        float y = plot_value_y(v, max_val, (float)gh, inverted);
        return y > ymax ? ymax : y;
    };

    if (stepped)
    {
        // Each frame a flat step as wide as it was on screen, a jump between them
        for (int i = 0; i < n; i++)
        {
            const struct plot_sample *s = plot_ring_at(ring, spans[i].idx);
            double v = plot_field_value(s, field);
            float y = yof(v);
            set_color(v, s->gap);
            float w = spans[i].x1 - spans[i].x0;
            if (w < 1.0f)
                w = 1.0f;
            gs_matrix_push();
            gs_matrix_translate3f(spans[i].x0, y, 0.0f);
            gs_draw_sprite(0, 0, (uint32_t)(w + 1.0f), (uint32_t)LINE_THICKNESS);
            gs_matrix_pop();
            if (i + 1 < n)
            {
                float ny = yof(plot_field_value(plot_ring_at(ring, spans[i + 1].idx), field));
                float top = y < ny ? y : ny;
                float bot = y > ny ? y : ny;
                gs_matrix_push();
                gs_matrix_translate3f(spans[i + 1].x0, top, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)LINE_THICKNESS, (uint32_t)(bot - top + LINE_THICKNESS));
                gs_matrix_pop();
            }
        }
    }
    else
    {
        // A line through the samples (each at the end of its frame), drawn as a
        // band of constant thickness along the segment - not a box over its value
        // range, which after a long pause filled a block seconds wide
        // (one triangle batch per run of the same colour). Framerate draws its
        // per-column curve, the frametime graph its samples.
        const bool curve = field == PLOT_FPS;
        const int points = curve ? curve_n : n;
        auto px = [&](int i) { return curve ? curve_x[i] : spans[i].x1; };
        auto pv = [&](int i) { return curve ? curve_v[i] : plot_field_value(plot_ring_at(ring, spans[i].idx), field); };
        int run_color = -1;
        for (int i = 0; i + 1 < points; i++)
        {
            double v0 = pv(i);
            double v1 = pv(i + 1);
            double worst = higher_is_better ? (v0 < v1 ? v0 : v1) : (v0 > v1 ? v0 : v1);
            int c = color_index(worst);
            if (c != run_color)
            {
                if (run_color >= 0)
                    gs_render_stop(GS_TRIS);
                set_color(worst, false);
                gs_render_start(true);
                run_color = c;
            }
            band_vertices(px(i), yof(v0), px(i + 1), yof(v1));
        }
        if (run_color >= 0)
            gs_render_stop(GS_TRIS);
    }

    // The frame on screen now, when no new one has come for unusually long (a
    // pause, a loading screen): without it the line would stop and then vanish.
    // Frame graphs: a step at how long the current frame has been shown so far
    // (it only grows until the next frame replaces it). The framerate curve
    // already counts the frame on screen (plot_fps_at), so it needs none.
    if (field != PLOT_FPS)
    {
        float tx0 = 0.0f, tx1 = 0.0f;
        double elapsed = 0.0;
        if (plot_tail(ring, now_ms, window_s, (float)gw, &tx0, &tx1, &elapsed))
        {
            const struct plot_sample *last = plot_ring_at(ring, ring->count - 1);
            double lv = plot_field_value(last, field);
            float lx = n > 0 ? spans[n - 1].x1 : tx0;
            double tv = elapsed;
            set_color(tv, false);
            if (stepped)
            {
                float ly = yof(lv), ty = yof(tv);
                gs_matrix_push();
                gs_matrix_translate3f(tx0, ly < ty ? ly : ty, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)LINE_THICKNESS, (uint32_t)(fabsf(ty - ly) + LINE_THICKNESS));
                gs_matrix_pop();
                gs_matrix_push();
                gs_matrix_translate3f(tx0, ty, 0.0f);
                gs_draw_sprite(0, 0, (uint32_t)(tx1 - tx0 + 1.0f), (uint32_t)LINE_THICKNESS);
                gs_matrix_pop();
            }
            else
            {
                draw_segment(lx, yof(lv), tx1, yof(tv));
            }
        }
    }

    gs_matrix_pop();
    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    // Grid labels right of the plot; one that would overlap the previous one
    // (a small graph, a wide scale) is skipped
    int last_label_y = -100000;
    for (int g = 0; g < grid_count; g++)
    {
        if (!grid_labels[g])
            continue;
        int y_ref = (int)plot_value_y(grid_values[g], max_val, (float)gh, inverted);
        if (grid_values[g] > max_val + 0.01 || y_ref < 0 || y_ref > gh)
            continue;
        uint32_t lh = obs_source_get_height(grid_labels[g]);
        if (abs(y_ref - last_label_y) < (int)lh)
            continue;
        last_label_y = y_ref;
        gs_matrix_push();
        gs_matrix_translate3f((float)(GRAPH_MARGIN + gw + 8), (float)(GRAPH_MARGIN + y_ref - (int)lh / 2), 0.0f);
        obs_source_video_render(grid_labels[g]);
        gs_matrix_pop();
    }
}

static void rebuild_grid_labels(struct fps_overlay_source *ctx)
{
    // Frame pacing grid — refresh periods
    double pc_max = ctx->pacing_scale > 0 ? ctx->pacing_scale : 100.0; // auto tops out at 100
    double pc_step = ctx->pc_label_step > 0 ? ctx->pc_label_step
                     : (pc_max > 33.33) ? (1000.0 / 60.0) : (1000.0 / 120.0);
    build_grid_labels(ctx->pc_grid_labels, ctx->pc_grid_values, &ctx->pc_grid_count,
                      pc_step, pc_max, true);

    // Frametime grid — step depends on scale range
    // (auto: labels for the whole range the auto scale may take; the ones
    // outside the current range, or overlapping a neighbour, are not drawn)
    double ft_max = ctx->frametime_scale > 0 ? ctx->frametime_scale : 100.0;
    double ft_step = (ft_max > 33.33) ? (1000.0 / 60.0) : (1000.0 / 120.0);
    build_grid_labels(ctx->ft_grid_labels, ctx->ft_grid_values, &ctx->ft_grid_count,
                      ft_step, ft_max, true);

    // FPS grid (auto: every 10 up to 150, so a 30 FPS graph scaled to ~33 still
    // gets its 30 label; a fixed 120 scale keeps every 20)
    double fps_max = ctx->fps_scale > 0 ? ctx->fps_scale : 150.0;
    double fps_step = (ctx->fps_scale >= 120.0) ? 20.0 : 10.0;
    build_grid_labels(ctx->fps_grid_labels, ctx->fps_grid_values, &ctx->fps_grid_count,
                      fps_step, fps_max, false);
}

// --- Source callbacks ---

static void *fps_overlay_create(obs_data_t *settings, obs_source_t *source)
{
    struct fps_overlay_source *ctx = (fps_overlay_source *)bzalloc(sizeof(struct fps_overlay_source));
    ctx->self = source;

    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 32;
    ctx->show_text_background = obs_data_get_bool(settings, "show_text_background");
    ctx->show_fps_text = obs_data_get_bool(settings, "show_fps_text");
    ctx->show_frametime_text = obs_data_get_bool(settings, "show_frametime_text");
    ctx->show_tearing_text = obs_data_get_bool(settings, "show_tearing_text");
    ctx->show_resolution_text = obs_data_get_bool(settings, "show_resolution_text");
    ctx->show_pacing_graph = obs_data_get_bool(settings, "show_pacing_graph");
    ctx->pacing_style = (int)obs_data_get_int(settings, "pacing_style");
    ctx->pacing_scale = obs_data_get_double(settings, "pacing_scale");
    ctx->pacing_width = (int)obs_data_get_int(settings, "pacing_width");
    ctx->pacing_height = (int)obs_data_get_int(settings, "pacing_height");
    ctx->frametime_width = (int)obs_data_get_int(settings, "frametime_width");
    ctx->frametime_height = (int)obs_data_get_int(settings, "frametime_height");
    ctx->fps_width = (int)obs_data_get_int(settings, "fps_width");
    ctx->fps_height = (int)obs_data_get_int(settings, "fps_height");
    ctx->pacing_window = obs_data_get_double(settings, "pacing_window");
    ctx->frametime_window = obs_data_get_double(settings, "frametime_window");
    ctx->fps_window = obs_data_get_double(settings, "fps_window");
    ctx->pacing_invert = obs_data_get_bool(settings, "pacing_invert");
    ctx->frametime_invert = obs_data_get_bool(settings, "frametime_invert");
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->frametime_style = (int)obs_data_get_int(settings, "frametime_style");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");
    ctx->fps_style = (int)obs_data_get_int(settings, "fps_style");
    ctx->frametime_scale = obs_data_get_double(settings, "frametime_scale");
    ctx->fps_scale = obs_data_get_double(settings, "fps_scale");
    ctx->show_resolution_spectrum = obs_data_get_bool(settings, "show_resolution_spectrum");
    ctx->show_source_rate = obs_data_get_bool(settings, "show_source_rate");
    ctx->autohide_source_rate = obs_data_get_bool(settings, "autohide_source_rate");

    ctx->last_text[0] = '\0';
    ctx->spec_tex = NULL;
    ctx->spec_bgra = (uint8_t *)bzalloc((size_t)RESDET_SPEC_W * RESDET_SPEC_H * 4);
    ctx->spec_uploaded_version = 0;
    ctx->spec_label_w_val = -1;
    ctx->spec_label_h_val = -1;

    // Create private text_gdiplus source (not visible in OBS source list)
    obs_data_t *text_settings = obs_data_create();
    obs_data_set_string(text_settings, "text", "Initializing...");
    ctx->text_source = obs_source_create_private("text_gdiplus", "fps_overlay_text", text_settings);
    obs_data_release(text_settings);

    if (ctx->text_source)
    {
        update_text_source(ctx, "Initializing...");
    }

    ctx->label_pacing = create_label_source("FRAME PACING", "fps_label_pc", 18, true);
    ctx->label_frametime = create_label_source("FRAMETIME", "fps_label_ft", 18, true);
    ctx->label_fps = create_label_source("FRAMERATE", "fps_label_fps", 18, true);
    ctx->spec_label_w = create_label_source_color("", "fps_spec_label_w", 14, true, 0x00FF00);
    ctx->spec_label_h = create_label_source_color("", "fps_spec_label_h", 14, true, 0x00FF00);
    ctx->srate_text[0] = '\0';
    ctx->label_srate = create_label_source(" ", "fps_srate_label", 20, true);
    update_srate_label(ctx);
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
        if (ctx->label_pacing)
            obs_source_release(ctx->label_pacing);
        if (ctx->label_frametime)
            obs_source_release(ctx->label_frametime);
        if (ctx->label_fps)
            obs_source_release(ctx->label_fps);
        if (ctx->spec_label_w)
            obs_source_release(ctx->spec_label_w);
        if (ctx->spec_label_h)
            obs_source_release(ctx->spec_label_h);
        if (ctx->label_srate)
            obs_source_release(ctx->label_srate);
        if (ctx->spec_bgra)
            bfree(ctx->spec_bgra);
        if (ctx->spec_tex)
        {
            obs_enter_graphics();
            gs_texture_destroy(ctx->spec_tex);
            obs_leave_graphics();
        }
        for (int i = 0; i < ctx->pc_grid_count; i++)
            if (ctx->pc_grid_labels[i])
                obs_source_release(ctx->pc_grid_labels[i]);
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
    static const char *const graphs[3][2] = {
        {"pacing", "show_pacing_graph"}, {"frametime", "show_frametime_graph"}, {"fps", "show_fps_graph"}};
    for (int g = 0; g < 3; g++)
    {
        const char *pre = graphs[g][0];
        bool on = obs_data_get_bool(settings, graphs[g][1]);
        char key[64];
        snprintf(key, sizeof(key), "%s_style", pre);
        bool custom = on && obs_data_get_int(settings, key) == GRAPH_STYLE_CUSTOM;
        static const char *const fields[] = {"style", "scale", "window", "invert"};
        for (const char *f : fields)
        {
            snprintf(key, sizeof(key), "%s_%s", pre, f);
            obs_property_t *pr = obs_properties_get(props, key);
            if (pr)
                obs_property_set_visible(pr, on);
        }
        snprintf(key, sizeof(key), "%s_width", pre);
        obs_property_set_visible(obs_properties_get(props, key), custom);
        snprintf(key, sizeof(key), "%s_height", pre);
        obs_property_set_visible(obs_properties_get(props, key), custom);
    }
    return true;
}

// Size (Custom), time window and inverted axis of one graph
static void add_graph_extras(obs_properties_t *props, const char *pre, const char *title, bool with_invert)
{
    char key[64], name[96];
    snprintf(key, sizeof(key), "%s_width", pre);
    snprintf(name, sizeof(name), "%s graph width (px)", title);
    obs_properties_add_int(props, key, name, 100, 3840, 10);
    snprintf(key, sizeof(key), "%s_height", pre);
    snprintf(name, sizeof(name), "%s graph height (px)", title);
    obs_properties_add_int(props, key, name, 40, 2160, 10);
    snprintf(key, sizeof(key), "%s_window", pre);
    snprintf(name, sizeof(name), "%s time window", title);
    obs_property_t *w = obs_properties_add_list(props, key, name, OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(w, "3 s", 3.0);
    obs_property_list_add_float(w, "5 s", 5.0);
    obs_property_list_add_float(w, "10 s", 10.0);
    obs_property_list_add_float(w, "20 s", 20.0);
    obs_property_list_add_float(w, "30 s", 30.0);
    obs_property_list_add_float(w, "60 s", 60.0);
    obs_property_set_long_description(w,
        "How many seconds the graph shows; the right edge is now. The scale is fixed in "
        "time, so a 50 ms frame is always three times as wide as a 16.7 ms one.");
    if (with_invert)
    {
        snprintf(key, sizeof(key), "%s_invert", pre);
        snprintf(name, sizeof(name), "%s: long frames down (Digital Foundry style)", title);
        obs_properties_add_bool(props, key, name);
    }
}

static obs_properties_t *fps_overlay_properties(void *data)
{
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
    obs_property_t *res_text = obs_properties_add_bool(props, "show_resolution_text", "Show Source resolution text");
    obs_property_set_long_description(res_text,
        "Shows the upscale source resolution estimate. Requires \"Detect upscale "
        "source resolution\" to be enabled in the FPS Analyzer filter.");
    obs_property_t *spec_prop = obs_properties_add_bool(props, "show_resolution_spectrum", "Show Source resolution spectrum");
    obs_property_set_long_description(spec_prop,
        "Shows the 2D DCT spectrum of the frame (low frequencies top-left) with "
        "markers at the detected source resolution. Requires \"Detect upscale "
        "source resolution\" to be enabled in the FPS Analyzer filter.");
    obs_property_t *srate_prop = obs_properties_add_bool(props, "show_source_rate", "Show Source refresh rate");
    obs_property_set_long_description(srate_prop,
        "Measures the real refresh rate of a capture card / camera signal and "
        "compares it with the OBS FPS (Settings > Video). A mismatch, e.g. a "
        "59.94 Hz console captured at 60 FPS, makes OBS repeat a frame every "
        "~17 s. Not available for Game/Display/Window Capture.");
    obs_property_t *srate_hide = obs_properties_add_bool(props, "autohide_source_rate",
                                                         "Auto-hide Source refresh rate when OK");
    obs_property_set_long_description(srate_hide,
        "Hides the Source refresh rate line 5 s after the verdict is OK. It comes "
        "back on a mismatch, a variable rate or while measuring again (e.g. after "
        "changing the OBS FPS).");
    obs_properties_add_bool(props, "show_text_background", "Show text background");

    obs_property_t *pc_toggle = obs_properties_add_bool(props, "show_pacing_graph", "Show frame pacing graph");
    obs_property_set_long_description(pc_toggle,
        "How long each game frame stayed on screen, frame by frame and unsmoothed, from the "
        "capture card's timestamps (whole refresh periods: 16.7 / 33.3 / 50 ms), like Digital "
        "Foundry's frame-time graph. A flat line is even pacing; a saw-tooth is judder even when "
        "the FPS reads 30. Grey: the capture device lost a frame there, not the game.");
    obs_property_set_modified_callback(pc_toggle, graph_toggle_modified);

    obs_property_t *pc_style = obs_properties_add_list(
        props, "pacing_style", "Frame pacing graph style",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(pc_style, "Big (1920x360)", GRAPH_STYLE_BIG);
    obs_property_list_add_int(pc_style, "Compact (300x80)", GRAPH_STYLE_COMPACT);
    obs_property_list_add_int(pc_style, "Custom", GRAPH_STYLE_CUSTOM);
    obs_property_set_modified_callback(pc_style, graph_toggle_modified);

    obs_property_t *pc_scale = obs_properties_add_list(
        props, "pacing_scale", "Frame pacing scale",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(pc_scale, "Auto", 0.0);
    obs_property_list_add_float(pc_scale, "33.33 ms", 33.33);
    obs_property_list_add_float(pc_scale, "50 ms", 50.0);
    obs_property_list_add_float(pc_scale, "66.67 ms", 66.67);
    obs_property_list_add_float(pc_scale, "100 ms", 100.0);
    add_graph_extras(props, "pacing", "Frame pacing", true);

    obs_property_t *ft_toggle = obs_properties_add_bool(props, "show_frametime_graph", "Show frametime graph");
    obs_property_set_modified_callback(ft_toggle, graph_toggle_modified);

    obs_property_t *ft_style = obs_properties_add_list(
        props, "frametime_style", "Frametime graph style",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(ft_style, "Big (1920x360)", GRAPH_STYLE_BIG);
    obs_property_list_add_int(ft_style, "Compact (300x80)", GRAPH_STYLE_COMPACT);
    obs_property_list_add_int(ft_style, "Custom", GRAPH_STYLE_CUSTOM);
    obs_property_set_modified_callback(ft_style, graph_toggle_modified);

    obs_property_t *ft_scale = obs_properties_add_list(
        props, "frametime_scale", "Frametime scale",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(ft_scale, "Auto", 0.0);
    obs_property_list_add_float(ft_scale, "16.67 ms", 16.67);
    obs_property_list_add_float(ft_scale, "33.33 ms", 33.33);
    obs_property_list_add_float(ft_scale, "66.67 ms", 66.67);
    add_graph_extras(props, "frametime", "Frametime", true);

    obs_property_t *fps_toggle = obs_properties_add_bool(props, "show_fps_graph", "Show framerate graph");
    obs_property_set_modified_callback(fps_toggle, graph_toggle_modified);

    obs_property_t *fps_style_prop = obs_properties_add_list(
        props, "fps_style", "Framerate graph style",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(fps_style_prop, "Big (1920x360)", GRAPH_STYLE_BIG);
    obs_property_list_add_int(fps_style_prop, "Compact (300x80)", GRAPH_STYLE_COMPACT);
    obs_property_list_add_int(fps_style_prop, "Custom", GRAPH_STYLE_CUSTOM);
    obs_property_set_modified_callback(fps_style_prop, graph_toggle_modified);

    obs_property_t *fps_scale = obs_properties_add_list(
        props, "fps_scale", "Framerate scale",
        OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_FLOAT);
    obs_property_list_add_float(fps_scale, "Auto", 0.0);
    obs_property_list_add_float(fps_scale, "120 FPS", 120.0);
    obs_property_list_add_float(fps_scale, "60 FPS", 60.0);
    obs_property_list_add_float(fps_scale, "30 FPS", 30.0);
    add_graph_extras(props, "fps", "Framerate", false);

    // Initial visibility
    obs_data_t *cur = data ? obs_source_get_settings(((struct fps_overlay_source *)data)->self) : NULL;
    if (cur)
    {
        graph_toggle_modified(props, NULL, cur);
        obs_data_release(cur);
    }

    return props;
}

static void fps_overlay_get_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, "font_size", 32);
    obs_data_set_default_bool(settings, "show_fps_text", true);
    obs_data_set_default_bool(settings, "show_frametime_text", true);
    obs_data_set_default_bool(settings, "show_tearing_text", true);
    obs_data_set_default_bool(settings, "show_resolution_text", true);
    obs_data_set_default_bool(settings, "show_resolution_spectrum", true);
    obs_data_set_default_bool(settings, "show_source_rate", true);
    obs_data_set_default_bool(settings, "autohide_source_rate", false);
    obs_data_set_default_bool(settings, "show_text_background", true);
    obs_data_set_default_bool(settings, "show_frametime_graph", true);
    obs_data_set_default_int(settings, "frametime_style", GRAPH_STYLE_COMPACT);
    obs_data_set_default_bool(settings, "show_pacing_graph", false);
    obs_data_set_default_int(settings, "pacing_style", GRAPH_STYLE_COMPACT);
    obs_data_set_default_double(settings, "pacing_scale", 0.0);
    obs_data_set_default_double(settings, "frametime_scale", 0.0);
    obs_data_set_default_bool(settings, "show_fps_graph", true);
    obs_data_set_default_int(settings, "fps_style", GRAPH_STYLE_COMPACT);
    obs_data_set_default_double(settings, "fps_scale", 0.0);
    // per graph: custom size, time window (frame graphs half the framerate's), axis
    obs_data_set_default_int(settings, "pacing_width", 350);
    obs_data_set_default_int(settings, "pacing_height", 120);
    obs_data_set_default_int(settings, "frametime_width", 350);
    obs_data_set_default_int(settings, "frametime_height", 120);
    obs_data_set_default_int(settings, "fps_width", 1800);
    obs_data_set_default_int(settings, "fps_height", 200);
    obs_data_set_default_double(settings, "pacing_window", 10.0);
    obs_data_set_default_double(settings, "frametime_window", 10.0);
    obs_data_set_default_double(settings, "fps_window", 20.0);
    obs_data_set_default_bool(settings, "pacing_invert", true);
    obs_data_set_default_bool(settings, "frametime_invert", false);
}

static void fps_overlay_update(void *data, obs_data_t *settings)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    ctx->font_size = (int)obs_data_get_int(settings, "font_size");
    if (ctx->font_size <= 0)
        ctx->font_size = 32;
    ctx->show_text_background = obs_data_get_bool(settings, "show_text_background");
    ctx->show_fps_text = obs_data_get_bool(settings, "show_fps_text");
    ctx->show_frametime_text = obs_data_get_bool(settings, "show_frametime_text");
    ctx->show_tearing_text = obs_data_get_bool(settings, "show_tearing_text");
    ctx->show_resolution_text = obs_data_get_bool(settings, "show_resolution_text");
    ctx->show_pacing_graph = obs_data_get_bool(settings, "show_pacing_graph");
    ctx->pacing_style = (int)obs_data_get_int(settings, "pacing_style");
    ctx->pacing_scale = obs_data_get_double(settings, "pacing_scale");
    ctx->pacing_width = (int)obs_data_get_int(settings, "pacing_width");
    ctx->pacing_height = (int)obs_data_get_int(settings, "pacing_height");
    ctx->frametime_width = (int)obs_data_get_int(settings, "frametime_width");
    ctx->frametime_height = (int)obs_data_get_int(settings, "frametime_height");
    ctx->fps_width = (int)obs_data_get_int(settings, "fps_width");
    ctx->fps_height = (int)obs_data_get_int(settings, "fps_height");
    ctx->pacing_window = obs_data_get_double(settings, "pacing_window");
    ctx->frametime_window = obs_data_get_double(settings, "frametime_window");
    ctx->fps_window = obs_data_get_double(settings, "fps_window");
    ctx->pacing_invert = obs_data_get_bool(settings, "pacing_invert");
    ctx->frametime_invert = obs_data_get_bool(settings, "frametime_invert");
    ctx->show_frametime_graph = obs_data_get_bool(settings, "show_frametime_graph");
    ctx->frametime_style = (int)obs_data_get_int(settings, "frametime_style");
    ctx->show_fps_graph = obs_data_get_bool(settings, "show_fps_graph");
    ctx->fps_style = (int)obs_data_get_int(settings, "fps_style");
    ctx->frametime_scale = obs_data_get_double(settings, "frametime_scale");
    ctx->fps_scale = obs_data_get_double(settings, "fps_scale");
    ctx->show_resolution_spectrum = obs_data_get_bool(settings, "show_resolution_spectrum");
    ctx->show_source_rate = obs_data_get_bool(settings, "show_source_rate");
    ctx->autohide_source_rate = obs_data_get_bool(settings, "autohide_source_rate");

    rebuild_grid_labels(ctx);
    update_srate_label(ctx);

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
                 "Add the \"FPS Analyzer 0.5.2\" filter\n"
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
    else if (g_fps_shared.blocked_reason == FPS_BLOCK_PIXFMT)
    {
        // No number at all: a reading that cannot be exact is not shown
        snprintf(text, sizeof(text),
                 "FPS not measured: unsupported video format (%s).\n"
                 "Supported: NV12, I420, I422, I444, YUY2, UYVY,\n"
                 "BGRA, BGRX (XRGB), RGBA. See README: Supported formats.",
                 g_fps_shared.blocked_detail);
    }
    else if (g_fps_shared.blocked_reason == FPS_BLOCK_COMPRESSED)
    {
        snprintf(text, sizeof(text),
                 "FPS not measured: the capture device sends %s,\n"
                 "a compressed format - repeated frames are not identical.\n"
                 "Set its Video Format to NV12 or YUY2. See README.",
                 g_fps_shared.blocked_detail);
    }
    else if (g_fps_shared.blocked_reason == FPS_BLOCK_ALTERED)
    {
        snprintf(text, sizeof(text),
                 "FPS not measured: the capture device alters every frame\n"
                 "(repeated frames are never identical), so the FPS\n"
                 "cannot be measured exactly. See README: Supported capture devices.");
    }
    else
    {
        text[0] = '\0';
        int pos = 0;
        if (g_fps_shared.reliability_off)
            pos += snprintf(text + pos, sizeof(text) - pos, "(reliability check off)\n");
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
        if (ctx->show_resolution_text && g_fps_shared.res_detect_enabled)
        {
            if (pos > 0)
                pos += snprintf(text + pos, sizeof(text) - pos, "\n");
            if (!g_fps_shared.res_valid)
            {
                pos += snprintf(text + pos, sizeof(text) - pos, "Source res: analyzing...");
            }
            else if (g_fps_shared.res_src_w > 0 || g_fps_shared.res_src_h > 0)
            {
                // Fall back to the frame dimension on an axis with no detection
                int sw = g_fps_shared.res_src_w > 0 ? g_fps_shared.res_src_w : g_fps_shared.res_frame_w;
                int sh = g_fps_shared.res_src_h > 0 ? g_fps_shared.res_src_h : g_fps_shared.res_frame_h;
                double conf = 1.0;
                if (g_fps_shared.res_src_w > 0 && g_fps_shared.res_conf_w < conf)
                    conf = g_fps_shared.res_conf_w;
                if (g_fps_shared.res_src_h > 0 && g_fps_shared.res_conf_h < conf)
                    conf = g_fps_shared.res_conf_h;
                pos += snprintf(text + pos, sizeof(text) - pos,
                                "Source res: ~%dx%d -> %dx%d (%d%%)",
                                sw, sh,
                                g_fps_shared.res_frame_w, g_fps_shared.res_frame_h,
                                (int)(conf * 100.0));
            }
            else if (g_fps_shared.res_status == RESDET_STATUS_PERIODIC)
            {
                // Dither lattice / adaptive-upscaler nulls offer several
                // scales at once — any number would be a guess.
                pos += snprintf(text + pos, sizeof(text) - pos,
                                "Source res: unreliable\n"
                                "(periodic pattern - dither or adaptive upscaler)");
            }
            else
            {
                // No mirror signature: a native image and a temporal/AI
                // upscaler look identical to the detector.
                pos += snprintf(text + pos, sizeof(text) - pos,
                                "Source res: no upscale signature\n"
                                "(native %dx%d, or TAA/FSR/DLSS - not measurable)",
                                g_fps_shared.res_frame_w, g_fps_shared.res_frame_h);
            }
        }
        if (pos == 0)
            snprintf(text, sizeof(text), " "); // at least a space so text source has content
    }

    // Spectrum marker labels follow the detected source resolution
    if (ctx->show_resolution_spectrum)
    {
        bool have = g_fps_shared.res_detect_enabled && g_fps_shared.res_valid;
        int sw = have ? g_fps_shared.res_src_w : 0;
        int sh = have ? g_fps_shared.res_src_h : 0;
        char buf[16];
        if (sw != ctx->spec_label_w_val)
        {
            snprintf(buf, sizeof(buf), "%d", sw);
            set_label_text(ctx->spec_label_w, buf);
            ctx->spec_label_w_val = sw;
        }
        if (sh != ctx->spec_label_h_val)
        {
            snprintf(buf, sizeof(buf), "%d", sh);
            set_label_text(ctx->spec_label_h, buf);
            ctx->spec_label_h_val = sh;
        }
    }

    // Frame pacing labels follow the game's frame time, like Digital Foundry's
    // 33.0 / 66.3 / 99.6 for a 30 or 20 FPS game: every 33.3 ms when the
    // typical frame is that long, every 16.7 ms otherwise
    if (ctx->show_pacing_graph)
    {
        static struct plot_span spans[FPS_PLOT_CAP];
        static double vals[FPS_PLOT_CAP];
        int n = plot_window(&g_fps_shared.plot, (double)os_gettime_ns() / 1e6, ctx->pacing_window, 1000.0f,
                            spans, FPS_PLOT_CAP);
        for (int i = 0; i < n; i++)
            vals[i] = plot_ring_at(&g_fps_shared.plot, spans[i].idx)->pacing_ms;
        double step = 1000.0 / 60.0;
        if (n > 0)
        {
            std::nth_element(vals, vals + n / 2, vals + n);
            if (vals[n / 2] >= 30.0)
                step = 1000.0 / 30.0;
        }
        if (fabs(step - ctx->pc_label_step) > 0.01)
        {
            ctx->pc_label_step = step;
            rebuild_grid_labels(ctx);
        }
    }

    // Source refresh rate line — relabel only on change. Showing it is what
    // makes the filter compute it (srate_wanted_ns): hidden, it costs nothing.
    if (ctx->show_source_rate && obs_source_active(ctx->self))
        g_fps_shared.srate_wanted_ns = os_gettime_ns();
    if (ctx->show_source_rate)
    {
        char srate_text[sizeof(ctx->srate_text)];
        build_srate_text(srate_text, sizeof(srate_text));
        if (strcmp(srate_text, ctx->srate_text) != 0)
        {
            strncpy(ctx->srate_text, srate_text, sizeof(ctx->srate_text));
            ctx->srate_text[sizeof(ctx->srate_text) - 1] = '\0';
            update_srate_label(ctx);
        }

        // Auto-hide: OK for 5 s -> hidden, anything else -> shown right away
        bool ok = g_fps_shared.srate_available && g_fps_shared.srate_valid &&
                  g_fps_shared.srate_status == SRATE_STATUS_MATCHED && g_fps_shared.beat_status == FPS_BEAT_CLEAR;
        uint64_t now = os_gettime_ns();
        if (!ok)
            ctx->srate_ok_since_ns = 0;
        else if (!ctx->srate_ok_since_ns)
            ctx->srate_ok_since_ns = now;
        ctx->srate_autohidden = ctx->autohide_source_rate && ctx->srate_ok_since_ns &&
                                now - ctx->srate_ok_since_ns >= SRATE_AUTOHIDE_NS;
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
    bool any_graph = (ctx->show_pacing_graph || ctx->show_frametime_graph || ctx->show_fps_graph) &&
                     g_fps_shared.plot.count >= 2;
    bool show_spec = spectrum_visible(ctx);

    // 1. Render text at top with margin
    bool any_text = overlay_any_text(ctx);
    uint32_t y_offset = 0;
    if (any_text && ctx->text_source)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)GRAPH_MARGIN, 0.0f);
        obs_source_video_render(ctx->text_source);
        gs_matrix_pop();
        y_offset = obs_source_get_height(ctx->text_source) + GRAPH_MARGIN * 2;
    }

    // 1b. Source refresh rate line, right under the text
    if (srate_visible(ctx))
    {
        if (y_offset == 0)
            y_offset = GRAPH_MARGIN;
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        obs_source_video_render(ctx->label_srate);
        gs_matrix_pop();
        y_offset += obs_source_get_height(ctx->label_srate) + GRAPH_MARGIN;
    }

    if (!any_graph && !show_spec)
        return;

    gs_blend_state_push();
    gs_reset_blend_state();
    gs_enable_blending(true);
    gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

    // 2. Spectrum panel
    if (show_spec)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        render_spectrum_panel(ctx);
        gs_matrix_pop();
        int pw, ph;
        get_spectrum_panel_dims(&pw, &ph);
        y_offset += (uint32_t)ph + GRAPH_MARGIN;
    }

    // 3. Frame pacing graph (per game frame, unsmoothed, stepped)
    if (any_graph && ctx->show_pacing_graph)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        double pc_step = (ctx->pacing_scale > 0 && ctx->pacing_scale <= 33.33) ? 8.33 : 16.67;
        // Auto scale: 50 / 66.67 / 100 ms (Digital Foundry's graph tops out at 100); a
        // longer frame (a pause, the source starting) is clipped at the edge.
        render_time_graph(PLOT_PACING, pc_step, false, false, 16.67, 33.33, ctx->pacing_scale,
                          ctx->pacing_scale <= 0, ctx->pc_grid_labels, ctx->pc_grid_values,
                          ctx->pc_grid_count, ctx->pacing_style, ctx->pacing_width, ctx->pacing_height,
                          ctx->pacing_window, ctx->pacing_invert, true);
        if (ctx->label_pacing)
        {
            gs_matrix_push();
            gs_matrix_translate3f((float)GRAPH_MARGIN, 2.0f, 0.0f);
            obs_source_video_render(ctx->label_pacing);
            gs_matrix_pop();
        }
        gs_matrix_pop();
        {
            int pw, ph, tw, th;
            get_graph_dims(ctx->pacing_style, &pw, &ph, &tw, &th, ctx->pacing_width, ctx->pacing_height);
            y_offset += th + GRAPH_MARGIN;
        }
    }

    // 4. Frametime graph
    if (any_graph && ctx->show_frametime_graph)
    {
        gs_matrix_push();
        gs_matrix_translate3f(0.0f, (float)y_offset, 0.0f);
        double ft_step = (ctx->frametime_scale > 33.33) ? 16.67 : 8.33;

        render_time_graph(PLOT_FRAMETIME, ft_step, true, false, 16.67, 33.33, ctx->frametime_scale, false,
                          ctx->ft_grid_labels, ctx->ft_grid_values, ctx->ft_grid_count, ctx->frametime_style,
                          ctx->frametime_width, ctx->frametime_height, ctx->frametime_window,
                          ctx->frametime_invert, false);
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
            get_graph_dims(ctx->frametime_style, &pw, &ph, &tw, &th, ctx->frametime_width, ctx->frametime_height);
            y_offset += th + GRAPH_MARGIN;
        }
    }

    // 5. FPS graph
    if (any_graph && ctx->show_fps_graph)
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

        render_time_graph(PLOT_FPS, fps_step, true, true, 60.0, 30.0, ctx->fps_scale, false,
                          ctx->fps_grid_labels, ctx->fps_grid_values, ctx->fps_grid_count, ctx->fps_style,
                          ctx->fps_width, ctx->fps_height, ctx->fps_window, false, false);
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
    bool any_text = overlay_any_text(ctx);
    uint32_t max_w = 0;
    if (any_text && ctx->text_source)
        max_w = obs_source_get_width(ctx->text_source) + GRAPH_MARGIN * 2;
    if (srate_visible(ctx))
    {
        uint32_t lw = obs_source_get_width(ctx->label_srate) + GRAPH_MARGIN * 2;
        if (lw > max_w)
            max_w = lw;
    }
    if (spectrum_visible(ctx))
    {
        int sw, sh;
        get_spectrum_panel_dims(&sw, &sh);
        if ((uint32_t)sw > max_w)
            max_w = (uint32_t)sw;
    }
    int pw, ph, tw, th;
    if (ctx->show_pacing_graph)
    {
        get_graph_dims(ctx->pacing_style, &pw, &ph, &tw, &th, ctx->pacing_width, ctx->pacing_height);
        if ((uint32_t)tw > max_w)
            max_w = (uint32_t)tw;
    }
    if (ctx->show_frametime_graph)
    {
        get_graph_dims(ctx->frametime_style, &pw, &ph, &tw, &th, ctx->frametime_width, ctx->frametime_height);
        if ((uint32_t)tw > max_w)
            max_w = (uint32_t)tw;
    }
    if (ctx->show_fps_graph)
    {
        get_graph_dims(ctx->fps_style, &pw, &ph, &tw, &th, ctx->fps_width, ctx->fps_height);
        if ((uint32_t)tw > max_w)
            max_w = (uint32_t)tw;
    }
    return max_w;
}

static uint32_t fps_overlay_get_height(void *data)
{
    struct fps_overlay_source *ctx = (struct fps_overlay_source *)data;
    bool any_text = overlay_any_text(ctx);
    uint32_t text_h = 0;
    if (any_text && ctx->text_source)
        text_h = obs_source_get_height(ctx->text_source) + GRAPH_MARGIN * 2;
    if (srate_visible(ctx))
    {
        if (text_h == 0)
            text_h = GRAPH_MARGIN;
        text_h += obs_source_get_height(ctx->label_srate) + GRAPH_MARGIN;
    }
    if (spectrum_visible(ctx))
    {
        int sw, sh;
        get_spectrum_panel_dims(&sw, &sh);
        text_h += (uint32_t)sh + GRAPH_MARGIN;
    }
    uint32_t graph_h = 0;
    int pw, ph, tw, th;
    const bool shown[3] = {ctx->show_pacing_graph, ctx->show_frametime_graph, ctx->show_fps_graph};
    const int styles[3] = {ctx->pacing_style, ctx->frametime_style, ctx->fps_style};
    const int widths[3] = {ctx->pacing_width, ctx->frametime_width, ctx->fps_width};
    const int heights[3] = {ctx->pacing_height, ctx->frametime_height, ctx->fps_height};
    for (int g = 0; g < 3; g++)
    {
        if (!shown[g])
            continue;
        get_graph_dims(styles[g], &pw, &ph, &tw, &th, widths[g], heights[g]);
        if (graph_h > 0)
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
    fps_analyzer_register_hotkeys();
    blog(LOG_INFO, "FPS Analyzer 0.5.2 loaded");
    return true;
}

void obs_module_unload(void)
{
    fps_analyzer_unregister_hotkeys();
}
