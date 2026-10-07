#include "source-rate.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

#define SRATE_CAPACITY 2048
#define SRATE_WINDOW_NS 15000000000ULL  // keep the last ~15 s of frames
#define SRATE_GAP_NS 250000000ULL       // longer gap = signal lost, restart
#define SRATE_MIN_SPAN_NS 5000000000ULL // precise reading needs >= 5 s ...
#define SRATE_MIN_INTERVALS 100         // ... and >= 100 frame intervals
#define SRATE_SNAP_TOL 200e-6           // standard-rate snapping tolerance
#define SRATE_MATCH_TOL 300e-6          // above typical crystal tolerance (~100 ppm)

// push() runs on the source's capture thread, get() on the graphics thread
struct srate {
    std::mutex mutex;
    uint64_t ts[SRATE_CAPACITY] = {};
    int start = 0;
    int count = 0;
    uint64_t last = 0;
    uint64_t last_push_ns = 0; // local clock of the newest push, 0 = none
};

struct std_rate {
    double hz;
    const char *label;
};

static const std_rate k_std_rates[] = {
    {24000.0 / 1001.0, "23.976"}, {24.0, "24"},    {25.0, "25"},
    {30000.0 / 1001.0, "29.97"},  {30.0, "30"},    {48.0, "48"},
    {50.0, "50"},                 {60000.0 / 1001.0, "59.94"},
    {60.0, "60"},                 {100.0, "100"},  {120000.0 / 1001.0, "119.88"},
    {120.0, "120"},               {144000.0 / 1001.0, "143.856"},
    {144.0, "144"},               {165.0, "165"},  {240.0, "240"},
};

static const std_rate *snap_std_rate(double hz) {
    for (const std_rate &r : k_std_rates)
        if (fabs(hz / r.hz - 1.0) <= SRATE_SNAP_TOL)
            return &r;
    return nullptr;
}

static inline uint64_t ts_at(const struct srate *s, int i) {
    return s->ts[(s->start + i) % SRATE_CAPACITY];
}

static double median(std::vector<double> v) {
    if (v.empty())
        return 0.0;
    size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    return v[mid];
}

struct srate *srate_create(void) {
    return new srate();
}

void srate_destroy(struct srate *s) {
    delete s;
}

static void reset_locked(struct srate *s) {
    s->start = 0;
    s->count = 0;
    s->last = 0;
}

void srate_reset(struct srate *s) {
    if (!s)
        return;
    std::lock_guard<std::mutex> lock(s->mutex);
    reset_locked(s);
}

void srate_push(struct srate *s, uint64_t ts_ns, uint64_t now_ns) {
    if (!s || ts_ns == 0)
        return;
    std::lock_guard<std::mutex> lock(s->mutex);
    s->last_push_ns = now_ns;
    if (s->count > 0) {
        // Deinterlacing hands the filter the previous frame again
        if (ts_ns <= s->last)
            return;
        if (ts_ns - s->last > SRATE_GAP_NS)
            reset_locked(s);
    }
    if (s->count == SRATE_CAPACITY) {
        s->start = (s->start + 1) % SRATE_CAPACITY;
        s->count--;
    }
    s->ts[(s->start + s->count) % SRATE_CAPACITY] = ts_ns;
    s->count++;
    s->last = ts_ns;
    while (s->count > 2 && ts_ns - ts_at(s, 0) > SRATE_WINDOW_NS) {
        s->start = (s->start + 1) % SRATE_CAPACITY;
        s->count--;
    }
}

bool srate_expire(struct srate *s, uint64_t now_ns, uint64_t max_idle_ns) {
    if (!s)
        return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->count == 0 || s->last_push_ns == 0)
        return false;
    // Signed: a push newer than now_ns would wrap to a huge unsigned idle
    // time and reset the window on almost every tick
    int64_t idle = (int64_t)(now_ns - s->last_push_ns);
    if (idle <= (int64_t)max_idle_ns)
        return false;
    reset_locked(s);
    return true;
}

// Drops everything but the newest `keep` timestamps
static void srate_keep_last(struct srate *s, int keep) {
    if (s->count <= keep)
        return;
    s->start = (s->start + (s->count - keep)) % SRATE_CAPACITY;
    s->count = keep;
}

bool srate_get(struct srate *s, double obs_hz, struct srate_result *out) {
    *out = srate_result{};
    out->obs_hz = obs_hz;
    out->status = SRATE_STATUS_MEASURING;
    if (!s)
        return false;
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->count < 3)
        return false;

    std::vector<double> deltas;
    double period = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        int n = s->count;
        deltas.resize((size_t)n - 1);
        for (int i = 1; i < n; i++)
            deltas[(size_t)i - 1] = (double)(ts_at(s, i) - ts_at(s, i - 1));
        period = median(deltas);

        // Source switched rate (e.g. 60 -> 120 Hz): restart from the recent part
        const int recent = 30;
        if (pass == 0 && (int)deltas.size() >= 2 * recent) {
            std::vector<double> tail(deltas.end() - recent, deltas.end());
            double recent_period = median(tail);
            if (fabs(recent_period / period - 1.0) > 0.10) {
                srate_keep_last(s, recent + 1);
                continue;
            }
        }
        break;
    }
    if (period <= 0.0)
        return false;

    // Count intervals in source periods: a frame OBS skipped leaves a double
    // interval. A decimated source (OBS takes every 2nd frame) shows half
    // steps near a phase crossing: 0.5P then 1.5P instead of P + P. Anything
    // else is added as-is so the sum still holds, and counts as variable.
    int n = s->count;
    int fractional = 0;
    double x = 0.0;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    std::vector<double> xs((size_t)n), ys((size_t)n);
    uint64_t t0 = ts_at(s, 0);
    for (int i = 0; i < n; i++) {
        if (i > 0) {
            double r = deltas[(size_t)i - 1] / period;
            double ri = std::round(r);
            double rh = std::round(2.0 * r) / 2.0;
            if (ri >= 1.0 && fabs(r - ri) <= 0.25) {
                x += ri;
            } else if (rh >= 0.5 && fabs(r - rh) <= 0.15) {
                x += rh;
            } else {
                x += r;
                fractional++;
            }
        }
        double y = (double)(ts_at(s, i) - t0);
        xs[(size_t)i] = x;
        ys[(size_t)i] = y;
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
    }
    double denom = n * sxx - sx * sx;
    if (denom <= 0.0)
        return false;
    double slope = (n * sxy - sx * sy) / denom; // ns per source period
    double icept = (sy - slope * sx) / n;
    if (slope <= 0.0)
        return false;
    double sse = 0.0;
    for (int i = 0; i < n; i++) {
        double e = ys[(size_t)i] - (icept + slope * xs[(size_t)i]);
        sse += e * e;
    }
    double rms = sqrt(sse / n);
    int intervals = n - 1;
    double span = ys[(size_t)n - 1];

    out->source_hz = 1e9 / slope;
    const std_rate *snap = snap_std_rate(out->source_hz);
    out->nominal_hz = snap ? snap->hz : 0.0;

    // Judged only on a long enough window: a handful of intervals says
    // nothing about the source being variable
    bool unstable = intervals >= SRATE_MIN_INTERVALS &&
                    (fractional > intervals / 20 || rms > 0.25 * slope);
    if (unstable) {
        out->status = SRATE_STATUS_UNSTABLE;
        return false;
    }
    if (span < (double)SRATE_MIN_SPAN_NS || intervals < SRATE_MIN_INTERVALS)
        return false; // still SRATE_STATUS_MEASURING, source_hz is preliminary
    out->valid = true;

    if (obs_hz <= 0.0) {
        out->status = SRATE_STATUS_MATCHED;
        return true;
    }

    // k: how source and OBS frames line up (1:1, OBS repeats k times, or
    // OBS takes every k-th source frame)
    double src = out->source_hz;
    double best = out->nominal_hz > 0.0 ? out->nominal_hz : src;
    double diff, rec;
    if (src >= obs_hz) {
        int k = std::max(1, (int)std::lround(src / obs_hz));
        diff = src - k * obs_hz;
        rec = best / k;
        out->ratio = k == 1 ? 1 : -k;
    } else {
        int k = std::max(1, (int)std::lround(obs_hz / src));
        diff = k * src - obs_hz;
        rec = best * k;
        out->ratio = k;
    }

    // With a standard source rate compare nominal values: a 59.94 source a
    // few ppm off its crystal still matches OBS at 59.94, and nothing in
    // OBS settings could get closer
    bool matched = out->nominal_hz > 0.0 ? fabs(rec / obs_hz - 1.0) < 1e-5
                                         : fabs(diff) / obs_hz < SRATE_MATCH_TOL;
    if (matched) {
        out->status = SRATE_STATUS_MATCHED;
    } else {
        out->status = SRATE_STATUS_MISMATCH;
        out->recommended_hz = rec;
        out->beat_period_s = fabs(diff) > 1e-9 ? 1.0 / fabs(diff) : 0.0;
    }
    return true;
}

void srate_format_hz(double hz, char *buf, size_t size) {
    const std_rate *snap = snap_std_rate(hz);
    if (snap)
        snprintf(buf, size, "%s", snap->label);
    else
        snprintf(buf, size, "%.3f", hz);
}
