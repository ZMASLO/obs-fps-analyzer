#include "plot-layout.h"

#include <string.h>

void plot_ring_clear(struct plot_ring *r)
{
    if (!r)
        return;
    r->head = 0;
    r->count = 0;
}

void plot_ring_push(struct plot_ring *r, const struct plot_sample *s)
{
    if (!r || !s)
        return;
    r->s[r->head] = *s;
    r->head = (r->head + 1) % FPS_PLOT_CAP;
    if (r->count < FPS_PLOT_CAP)
        r->count++;
}

const struct plot_sample *plot_ring_at(const struct plot_ring *r, int i)
{
    if (!r || i < 0 || i >= r->count)
        return nullptr;
    int idx = (r->head - r->count + i + FPS_PLOT_CAP) % FPS_PLOT_CAP;
    return &r->s[idx];
}

int plot_window(const struct plot_ring *r, double now_ms, double window_s, float width_px,
                struct plot_span *out, int max_out)
{
    if (!r || !out || max_out <= 0 || window_s <= 0.0 || width_px <= 0.0f)
        return 0;
    const double window_ms = window_s * 1000.0;
    const double start_ms = now_ms - window_ms;
    const double px_per_ms = (double)width_px / window_ms;

    // Newest first, stop at the first sample that ended before the window
    int first = r->count;
    while (first > 0) {
        const struct plot_sample *s = plot_ring_at(r, first - 1);
        if (s->t_ms <= start_ms)
            break;
        first--;
    }
    if (r->count - first > max_out)
        first = r->count - max_out;

    int n = 0;
    for (int i = first; i < r->count; i++) {
        const struct plot_sample *s = plot_ring_at(r, i);
        double x1 = (s->t_ms - start_ms) * px_per_ms;
        double x0 = (s->t_ms - s->ft_ms - start_ms) * px_per_ms;
        if (x0 < 0.0)
            x0 = 0.0;
        if (x1 > width_px)
            x1 = width_px;
        if (x1 < x0)
            x1 = x0;
        out[n].x0 = (float)x0;
        out[n].x1 = (float)x1;
        out[n].idx = i;
        n++;
    }
    return n;
}

float plot_value_y(double v, double max_val, float height, bool inverted)
{
    if (max_val <= 0.0)
        return height;
    double f = v / max_val;
    if (f < 0.0)
        f = 0.0;
    if (f > 1.0)
        f = 1.0;
    return (float)(inverted ? f * height : (1.0 - f) * height);
}
