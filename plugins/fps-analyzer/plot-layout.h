#pragma once
#include <stdbool.h>
#include <stdint.h>

// Graph samples on a time axis (OBS-free, like fps-core, so the test tools can
// check the geometry).
//
// Every new game frame adds one sample, stamped with the analysis clock. A
// graph shows the last `window_s` seconds: the right edge is "now", and the
// horizontal scale is fixed in pixels per second, so a 50 ms frame is always
// three times as wide as a 16.7 ms one and switching between 30 and 60 FPS
// does not stretch the graph. The sample at t describes the frame that ended
// at t: it spans [t - ft_ms, t].

#define FPS_PLOT_CAP 8192 // 60 s at 136 FPS, 34 s at 240

struct plot_sample {
    double t_ms;      // analysis clock when the new frame arrived
    double ft_ms;     // time since the previous new frame (analysis clock)
    double ema_ms;    // smoothed frametime (frametime graph)
    double fps_pf;    // per-frame FPS (framerate graph)
    double pacing_ms; // frame pacing (device timestamps, whole refresh periods)
    bool gap;         // the capture device lost a frame in this sample
    bool tearing;
};

struct plot_ring {
    struct plot_sample s[FPS_PLOT_CAP];
    int head;  // next slot to write
    int count; // valid samples, oldest at (head - count)
};

void plot_ring_clear(struct plot_ring *r);
void plot_ring_push(struct plot_ring *r, const struct plot_sample *s);
// i = 0 is the oldest sample still in the ring
const struct plot_sample *plot_ring_at(const struct plot_ring *r, int i);

// One sample placed on the graph: [x0, x1] in pixels from the plot's left
// edge, clipped to [0, width]. idx is the ring index (plot_ring_at).
struct plot_span {
    float x0, x1;
    int idx;
};

// The samples of the last window_s seconds before now_ms, oldest first.
// Returns how many were written to out (at most max_out; the newest are kept).
int plot_window(const struct plot_ring *r, double now_ms, double window_s, float width_px,
                struct plot_span *out, int max_out);

// The frame on screen right now, when it has stayed there unusually long (a
// pause, a loading screen, the game freezing): no new sample arrives, so
// without this the line would stop and then vanish from the window. Returns
// false while frames keep coming; otherwise the span from the last sample to
// the right edge and how long the current frame has been shown so far (a
// lower bound of its time: it only grows until the next frame replaces it).
// "Unusually long": more than twice the last frame and at least 50 ms, so
// ordinary judder (16.7 / 50 ms) does not flicker at the edge.
bool plot_tail(const struct plot_ring *r, double now_ms, double window_s, float width_px, float *x0, float *x1,
               double *elapsed_ms);

// New game frames in the last span_ms (FPS "now" for span 1000): falls to 0
// while the picture stands still.
int plot_recent_frames(const struct plot_ring *r, double now_ms, double span_ms);

// Framerate at time t_ms (<= now_ms): the game frames on screen during the
// last second before t, each counted by the share of its time that falls in
// that second:  FPS(t) = sum overlap([t_i - ft_i, t_i], [t - 1000, t]) / ft_i.
// A steady 30 FPS game at 59.94 Hz reads 29.97 at every moment (no 29/30
// flicker). The frame still on screen (from the last sample to now) counts
// with the time it has been shown so far, so during a pause the value falls
// to ~1 within a second and on towards 0, instead of a straight line drawn
// across the pause to the first frame after it.
double plot_fps_at(const struct plot_ring *r, double t_ms, double now_ms);

// plot_fps_at every step_px across the graph (the window_s seconds before
// now_ms in width_px). Columns whose second reaches back before the oldest
// sample are left out (they would read too low). Returns the columns
// written to xs / vals (at most max_out).
int plot_fps_curve(const struct plot_ring *r, double now_ms, double window_s, float width_px, float step_px,
                   float *xs, double *vals, int max_out);

// Pixel row of a value: 0 = top, height = bottom. Normal: 0 at the bottom,
// max at the top. Inverted (Digital Foundry's frame-time graph): 0 at the
// top, so a longer frame goes down like a drop in the framerate graph.
float plot_value_y(double v, double max_val, float height, bool inverted);
