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

bool plot_tail(const struct plot_ring *r, double now_ms, double window_s, float width_px, float *x0, float *x1,
               double *elapsed_ms)
{
    if (!r || r->count == 0 || window_s <= 0.0 || width_px <= 0.0f)
        return false;
    const struct plot_sample *last = plot_ring_at(r, r->count - 1);
    double elapsed = now_ms - last->t_ms;
    double threshold = 2.0 * last->ft_ms;
    if (threshold < 50.0)
        threshold = 50.0;
    if (elapsed <= threshold)
        return false;
    const double window_ms = window_s * 1000.0;
    double x = (last->t_ms - (now_ms - window_ms)) * (double)width_px / window_ms;
    if (x < 0.0)
        x = 0.0;
    if (x > width_px)
        x = width_px;
    if (x0)
        *x0 = (float)x;
    if (x1)
        *x1 = width_px;
    if (elapsed_ms)
        *elapsed_ms = elapsed;
    return true;
}

int plot_recent_frames(const struct plot_ring *r, double now_ms, double span_ms)
{
    if (!r)
        return 0;
    int n = 0;
    for (int i = r->count - 1; i >= 0; i--) {
        if (plot_ring_at(r, i)->t_ms <= now_ms - span_ms)
            break;
        n++;
    }
    return n;
}

#define PLOT_FPS_SPAN_MS 1000.0

// First ring index whose frame ends after t_ms (samples are in time order)
static int first_ending_after(const struct plot_ring *r, double t_ms)
{
    int lo = 0, hi = r->count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (plot_ring_at(r, mid)->t_ms <= t_ms)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

double plot_fps_at(const struct plot_ring *r, double t_ms, double now_ms)
{
    if (!r || r->count == 0)
        return 0.0;
    const double ws = t_ms - PLOT_FPS_SPAN_MS;
    double sum = 0.0;
    for (int i = first_ending_after(r, ws); i < r->count; i++) {
        const struct plot_sample *s = plot_ring_at(r, i);
        double start = s->t_ms - s->ft_ms;
        if (start >= t_ms)
            break;
        if (s->ft_ms <= 0.0)
            continue;
        double a = start > ws ? start : ws;
        double b = s->t_ms < t_ms ? s->t_ms : t_ms;
        if (b > a)
            sum += (b - a) / s->ft_ms;
    }
    // the frame still on screen, with the time it has been shown so far
    const struct plot_sample *last = plot_ring_at(r, r->count - 1);
    double shown = now_ms - last->t_ms;
    if (shown > 0.0 && t_ms > last->t_ms) {
        double a = last->t_ms > ws ? last->t_ms : ws;
        double b = t_ms < now_ms ? t_ms : now_ms;
        if (b > a)
            sum += (b - a) / shown;
    }
    return sum * 1000.0 / PLOT_FPS_SPAN_MS;
}

double plot_fps_to_frametime(double fps)
{
    return fps > 0.0 ? 1000.0 / fps : 1e9;
}

double plot_frametime_at(const struct plot_ring *r, double t_ms, double now_ms)
{
    return plot_fps_to_frametime(plot_fps_at(r, t_ms, now_ms));
}

int plot_fps_curve(const struct plot_ring *r, double now_ms, double window_s, float width_px, float step_px,
                   float *xs, double *vals, int max_out)
{
    if (!r || r->count == 0 || !xs || !vals || max_out <= 0 || window_s <= 0.0 || width_px <= 0.0f ||
        step_px <= 0.0f)
        return 0;
    const double window_ms = window_s * 1000.0;
    const struct plot_sample *first = plot_ring_at(r, 0);
    const double covered_from = first->t_ms - first->ft_ms + PLOT_FPS_SPAN_MS;
    int n = 0;
    for (float x = 0.0f; n < max_out; x += step_px) {
        if (x > width_px)
            x = width_px;
        double t = now_ms - window_ms + (double)x / width_px * window_ms;
        if (t >= covered_from) {
            xs[n] = x;
            vals[n] = plot_fps_at(r, t, now_ms);
            n++;
        }
        if (x >= width_px)
            break;
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
