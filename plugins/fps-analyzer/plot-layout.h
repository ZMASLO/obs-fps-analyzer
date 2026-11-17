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

// Pixel row of a value: 0 = top, height = bottom. Normal: 0 at the bottom,
// max at the top. Inverted (Digital Foundry's frame-time graph): 0 at the
// top, so a longer frame goes down like a drop in the framerate graph.
float plot_value_y(double v, double max_val, float height, bool inverted);
