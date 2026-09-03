// Upscale source resolution detector — see resolution-detector.h.
// DCT-II computed with KissFFT via the mirrored 2N real-FFT identity,
// detection method "sign" ported from resdet (MIT).

#include "resolution-detector.h"
#include "kissfft/kiss_fftr.h"

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

// resdet uses range 12, threshold 0.55 for the "sign" method on single
// images. Real game content is noisier than photos, so we accumulate
// results over multiple frames (EMA) and use a higher threshold.
#define RESDET_RANGE 12
#define RESDET_THRESHOLD 0.60f
// EMA weight of each new frame's result vector
#define RESDET_ACCUM_ALPHA 0.25f
// Analyses required before reporting anything (lets the EMA settle)
#define RESDET_WARMUP_FRAMES 4
// Don't report sources scaled by less than ~3% — too close to native
// to be meaningful and a common region for false positives.
#define RESDET_MIN_SCALE 0.97
// Don't report sources below 1/4 of the frame dimension — real upscales
// rarely exceed 4x and the low-index region is full of false positives
// on real content (e.g. "250x26" on a native 1440p game).
#define RESDET_MIN_FRACTION 0.25

#define RESDET_MAX_DIM 4096

// Magnitude-knee detection: native-res overlays (HUD, film grain) and
// temporal upscalers corrupt the *signs* of near-zero DCT coefficients,
// killing the sign method, but the *energy envelope* still shows a step
// (knee) at the source resolution. We profile log10 magnitude along each
// axis and look for the strongest step.
#define KNEE_LOG_EPS 1e-2f
// Minimum step height in log10 decades to accept a knee
#define KNEE_THRESHOLD 0.45
// Step height (decades) mapped to confidence 1.0
#define KNEE_CONF_FULL 1.5

// Consensus over recent analyses (Brazil-Pixel-style mode statistics):
// an axis reports a resolution only when enough recent analyses agree.
#define RESDET_HISTORY 8
#define RESDET_MIN_VOTES 3

struct dct_plan {
    size_t width = 0, height = 0;
    kiss_fftr_cfg cfg_w = nullptr, cfg_h = nullptr;
    std::vector<float> mirror;            // 2*max(w,h)
    std::vector<kiss_fft_cpx> F;          // max(w,h)+1
    std::vector<kiss_fft_cpx> shift_w, shift_h;

    void release() {
        if (cfg_w) free(cfg_w);
        if (cfg_h && cfg_h != cfg_w) free(cfg_h);
        cfg_w = cfg_h = nullptr;
        width = height = 0;
    }

    bool setup(size_t w, size_t h) {
        if (width == w && height == h)
            return cfg_w != nullptr;
        release();

        cfg_w = kiss_fftr_alloc((int)(w * 2), 0, nullptr, nullptr);
        cfg_h = (w == h) ? cfg_w : kiss_fftr_alloc((int)(h * 2), 0, nullptr, nullptr);
        if (!cfg_w || !cfg_h) {
            release();
            return false;
        }

        size_t maxdim = w > h ? w : h;
        mirror.resize(maxdim * 2);
        F.resize(maxdim + 1);

        const double pi = 3.14159265358979323846;
        shift_w.resize(w);
        for (size_t x = 0; x < w; x++) {
            shift_w[x].r = (float)cos(-pi * x / (2.0 * w));
            shift_w[x].i = (float)sin(-pi * x / (2.0 * w));
        }
        shift_h.resize(h);
        for (size_t y = 0; y < h; y++) {
            shift_h[y].r = (float)cos(-pi * y / (2.0 * h));
            shift_h[y].i = (float)sin(-pi * y / (2.0 * h));
        }

        width = w;
        height = h;
        return true;
    }

    ~dct_plan() { release(); }
};

// In-place 1D DCT-II over n lines of `length` samples.
// stride = step between samples within a line, dist = step between lines.
static void dct_pass(kiss_fftr_cfg cfg, float *f, kiss_fft_cpx *F, float *mirror,
                     const kiss_fft_cpx *shift, size_t n, size_t length,
                     size_t stride, size_t dist)
{
    for (size_t j = 0; j < n; j++) {
        for (size_t i = 0; i < length; i++)
            mirror[i] = mirror[length * 2 - 1 - i] = f[j * dist + i * stride];
        kiss_fftr(cfg, mirror, F);
        for (size_t i = 0; i < length; i++)
            f[j * dist + i * stride] = F[i].r * shift[i].r - F[i].i * shift[i].i;
    }
}

// resdet "sign" method: sweep candidate boundaries, counting DCT coefficient
// sign inversions mirrored around each index. result has length-2*range slots
// covering indices [range, length-range).
static void detect_sign(const float *f, size_t length, size_t n, size_t stride,
                        size_t dist, size_t range, float *result)
{
    for (size_t x = range; x < length - range; x++) {
        uint32_t sign_diff = 0;
        for (size_t y = 0; y < n; y++) {
            const float *line = f + y * stride + x * dist;
            for (size_t i = 1; i <= range; i++)
                sign_diff += std::signbit(line[-(ptrdiff_t)(i * dist)]) !=
                             std::signbit(line[i * dist]);
        }
        result[x - range] = sign_diff / (float)(n * range);
    }
}

// Mean log10 magnitude of the DCT along one axis, averaged over the
// n_perp lowest-frequency lines of the perpendicular axis (where scene
// energy is concentrated). Indexing convention matches detect_sign.
static void magnitude_profile(const float *f, size_t length, size_t n_perp,
                              size_t stride, size_t dist, float *profile)
{
    for (size_t x = 0; x < length; x++) {
        float sum = 0.0f;
        for (size_t y = 0; y < n_perp; y++)
            sum += log10f(fabsf(f[y * stride + x * dist]) + KNEE_LOG_EPS);
        profile[x] = sum / (float)n_perp;
    }
}

// Strongest energy step in the (smoothed) profile, or 0 if none passes
// the threshold. A true upscale boundary stays low past the step, so we
// also check a far window — this rejects narrow dips (e.g. filter nulls).
static void pick_knee(const float *profile, size_t length, int *out_idx,
                      double *out_conf)
{
    *out_idx = 0;
    *out_conf = 0.0;

    size_t W = length / 64 < 8 ? 8 : length / 64;
    size_t lo = (size_t)(length * RESDET_MIN_FRACTION);
    size_t hi = (size_t)(length * RESDET_MIN_SCALE);
    if (lo < 2 * W)
        lo = 2 * W;
    if (hi + W >= length || lo >= hi)
        return;

    // 5-tap box smoothing into a temp copy, then prefix sums
    std::vector<float> p(length), pre(length + 1, 0.0f);
    for (size_t i = 0; i < length; i++) {
        size_t a = i < 2 ? 0 : i - 2;
        size_t b = i + 2 >= length ? length - 1 : i + 2;
        float s = 0.0f;
        for (size_t j = a; j <= b; j++)
            s += profile[j];
        p[i] = s / (float)(b - a + 1);
    }
    for (size_t i = 0; i < length; i++)
        pre[i + 1] = pre[i] + p[i];

    double best = 0.0;
    size_t best_k = 0;
    for (size_t k = lo; k < hi; k++) {
        double left = (pre[k] - pre[k - W]) / W;
        double left_far = (pre[k - W] - pre[k - 2 * W]) / W;
        double right = (pre[k + W] - pre[k]) / W;
        size_t fend = k + 3 * W < length ? k + 3 * W : length;
        double far_right = (pre[fend] - pre[k + W]) / (double)(fend - (k + W));
        double step = left - right;
        double step_far = left - far_right;
        double score = step < step_far ? step : step_far;
        // Soft content decays gradually into the noise floor; that
        // shoulder is not an upscale boundary. A real boundary must drop
        // much faster than the local decay trend left of the candidate.
        double trend = left_far - left;
        if (trend > 0.0)
            score -= trend;
        if (score > best) {
            best = score;
            best_k = k;
        }
    }

    if (best >= KNEE_THRESHOLD) {
        *out_idx = (int)best_k;
        *out_conf = best / KNEE_CONF_FULL;
        if (*out_conf > 1.0)
            *out_conf = 1.0;
    }
}

// Median-with-agreement over the recent per-axis detections: returns the
// median of nonzero values if enough of them agree within tolerance,
// otherwise 0. Smooths DRS jitter and rejects sporadic false knees.
static int axis_consensus(const int *vals, const double *confs, int count,
                          double *out_conf)
{
    *out_conf = 0.0;

    int nz[RESDET_HISTORY];
    double nzc[RESDET_HISTORY];
    int n = 0;
    for (int i = 0; i < count; i++)
        if (vals[i] > 0) {
            nz[n] = vals[i];
            nzc[n] = confs[i];
            n++;
        }
    if (n < RESDET_MIN_VOTES)
        return 0;

    // insertion sort (n <= RESDET_HISTORY), confidences follow values
    for (int i = 1; i < n; i++) {
        int v = nz[i];
        double c = nzc[i];
        int j = i - 1;
        while (j >= 0 && nz[j] > v) {
            nz[j + 1] = nz[j];
            nzc[j + 1] = nzc[j];
            j--;
        }
        nz[j + 1] = v;
        nzc[j + 1] = c;
    }
    int median = nz[n / 2];

    double tol = median * 0.02;
    if (tol < 8.0)
        tol = 8.0;
    int agree = 0;
    double conf_sum = 0.0;
    for (int i = 0; i < n; i++)
        if (fabs((double)nz[i] - median) <= tol) {
            agree++;
            conf_sum += nzc[i];
        }
    if (agree < RESDET_MIN_VOTES)
        return 0;

    *out_conf = conf_sum / agree;
    return median;
}

// Highest-confidence candidate above threshold, or 0 if none.
static void pick_candidate(const float *result, size_t length, size_t range,
                           int *out_idx, double *out_conf)
{
    *out_idx = 0;
    *out_conf = 0.0;
    size_t count = length - 2 * range;
    size_t lo = (size_t)(length * RESDET_MIN_FRACTION);
    size_t hi = (size_t)(length * RESDET_MIN_SCALE);
    for (size_t i = 0; i < count; i++) {
        size_t idx = i + range;
        if (idx < lo)
            continue;
        if (idx >= hi)
            break;
        if (result[i] >= RESDET_THRESHOLD && result[i] > *out_conf) {
            *out_idx = (int)idx;
            *out_conf = result[i];
        }
    }
}

struct resolution_detector {
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop = false;
    bool has_job = false;
    std::vector<uint8_t> job_luma;
    uint32_t job_w = 0, job_h = 0;

    std::mutex result_mtx;
    resdet_result result = {};
    bool result_fresh = false;

    // worker-only state
    dct_plan plan;
    std::vector<float> coeffs;
    std::vector<float> xresult, yresult;
    std::vector<float> xprof, yprof;
    // temporal accumulation (EMA over consecutive analyses)
    std::vector<float> xaccum, yaccum;     // sign-method votes
    std::vector<float> xprof_acc, yprof_acc; // magnitude profiles
    uint32_t accum_w = 0, accum_h = 0;
    int frames_accumulated = 0;
    // per-axis detection history for consensus
    int hist_w[RESDET_HISTORY] = {}, hist_h[RESDET_HISTORY] = {};
    double hconf_w[RESDET_HISTORY] = {}, hconf_h[RESDET_HISTORY] = {};
    int hist_pos = 0, hist_count = 0;

    void analyze(const uint8_t *luma, uint32_t w, uint32_t h);
    void worker_loop();
};

void resolution_detector::analyze(const uint8_t *luma, uint32_t w, uint32_t h)
{
    if (w < RESDET_RANGE * 2 + 2 || h < RESDET_RANGE * 2 + 2 ||
        w > RESDET_MAX_DIM || h > RESDET_MAX_DIM)
        return;
    if (!plan.setup(w, h))
        return;

    coeffs.resize((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++)
        coeffs[i] = (float)luma[i];

    // 2D DCT: rows then columns
    dct_pass(plan.cfg_w, coeffs.data(), plan.F.data(), plan.mirror.data(),
             plan.shift_w.data(), h, w, 1, w);
    dct_pass(plan.cfg_h, coeffs.data(), plan.F.data(), plan.mirror.data(),
             plan.shift_h.data(), w, h, w, 1);

    // Both detectors only use the lowest-frequency quarter of the
    // perpendicular axis: that's where scene energy lives (sources below
    // 1/4 of the frame aren't reported anyway), and the all-dead lines
    // beyond the source band would just dilute the votes with noise.
    size_t n_perp_x = h / 4 ? h / 4 : 1;
    size_t n_perp_y = w / 4 ? w / 4 : 1;

    xresult.assign(w - 2 * RESDET_RANGE, 0.0f);
    yresult.assign(h - 2 * RESDET_RANGE, 0.0f);
    detect_sign(coeffs.data(), w, n_perp_x, w, 1, RESDET_RANGE, xresult.data());
    detect_sign(coeffs.data(), h, n_perp_y, 1, w, RESDET_RANGE, yresult.data());

    xprof.assign(w, 0.0f);
    yprof.assign(h, 0.0f);
    magnitude_profile(coeffs.data(), w, n_perp_x, w, 1, xprof.data());
    magnitude_profile(coeffs.data(), h, n_perp_y, 1, w, yprof.data());

    // Accumulate over consecutive frames: random sign noise converges
    // toward ~0.5 and profile noise (grain) averages out, while a real
    // upscale boundary stays put on every frame.
    if (accum_w != w || accum_h != h) {
        xaccum = xresult;
        yaccum = yresult;
        xprof_acc = xprof;
        yprof_acc = yprof;
        accum_w = w;
        accum_h = h;
        frames_accumulated = 1;
        hist_pos = 0;
        hist_count = 0;
    } else {
        for (size_t i = 0; i < xaccum.size(); i++)
            xaccum[i] += (xresult[i] - xaccum[i]) * RESDET_ACCUM_ALPHA;
        for (size_t i = 0; i < yaccum.size(); i++)
            yaccum[i] += (yresult[i] - yaccum[i]) * RESDET_ACCUM_ALPHA;
        for (size_t i = 0; i < xprof_acc.size(); i++)
            xprof_acc[i] += (xprof[i] - xprof_acc[i]) * RESDET_ACCUM_ALPHA;
        for (size_t i = 0; i < yprof_acc.size(); i++)
            yprof_acc[i] += (yprof[i] - yprof_acc[i]) * RESDET_ACCUM_ALPHA;
        frames_accumulated++;
    }

    if (frames_accumulated < RESDET_WARMUP_FRAMES)
        return;

    // Hybrid: exact sign method first; magnitude knee as the robust
    // fallback when overlays/grain destroy the sign symmetry.
    int cand_w = 0, cand_h = 0;
    double cconf_w = 0.0, cconf_h = 0.0;
    pick_candidate(xaccum.data(), w, RESDET_RANGE, &cand_w, &cconf_w);
    pick_candidate(yaccum.data(), h, RESDET_RANGE, &cand_h, &cconf_h);
    if (!cand_w)
        pick_knee(xprof_acc.data(), w, &cand_w, &cconf_w);
    if (!cand_h)
        pick_knee(yprof_acc.data(), h, &cand_h, &cconf_h);

    hist_w[hist_pos] = cand_w;
    hconf_w[hist_pos] = cconf_w;
    hist_h[hist_pos] = cand_h;
    hconf_h[hist_pos] = cconf_h;
    hist_pos = (hist_pos + 1) % RESDET_HISTORY;
    if (hist_count < RESDET_HISTORY)
        hist_count++;

    resdet_result r = {};
    r.valid = true;
    r.frame_w = (int)w;
    r.frame_h = (int)h;
    r.src_w = axis_consensus(hist_w, hconf_w, hist_count, &r.conf_w);
    r.src_h = axis_consensus(hist_h, hconf_h, hist_count, &r.conf_h);

    // Uniform-scale inference: upscaling almost always preserves aspect,
    // so when one axis has a clean signature and the other drowned in
    // soft content/grain, derive the missing one from the frame aspect.
    if (r.src_w > 0 && r.src_h == 0) {
        int est = (int)((double)r.src_w * h / w + 0.5);
        if (est >= (int)(h * RESDET_MIN_FRACTION) &&
            est < (int)(h * RESDET_MIN_SCALE)) {
            r.src_h = est;
            r.conf_h = r.conf_w;
        }
    } else if (r.src_h > 0 && r.src_w == 0) {
        int est = (int)((double)r.src_h * w / h + 0.5);
        if (est >= (int)(w * RESDET_MIN_FRACTION) &&
            est < (int)(w * RESDET_MIN_SCALE)) {
            r.src_w = est;
            r.conf_w = r.conf_h;
        }
    }

    std::lock_guard<std::mutex> lock(result_mtx);
    result = r;
    result_fresh = true;
}

void resolution_detector::worker_loop()
{
    std::vector<uint8_t> luma;
    uint32_t w, h;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(mtx);
            cv.wait(lock, [this] { return stop || has_job; });
            if (stop)
                return;
            luma.swap(job_luma);
            w = job_w;
            h = job_h;
        }
        analyze(luma.data(), w, h);
        {
            std::lock_guard<std::mutex> lock(mtx);
            has_job = false;
        }
    }
}

struct resolution_detector *resdet_create(void)
{
    resolution_detector *rd = new resolution_detector();
    rd->worker = std::thread(&resolution_detector::worker_loop, rd);
    return rd;
}

void resdet_destroy(struct resolution_detector *rd)
{
    if (!rd)
        return;
    {
        std::lock_guard<std::mutex> lock(rd->mtx);
        rd->stop = true;
    }
    rd->cv.notify_all();
    if (rd->worker.joinable())
        rd->worker.join();
    delete rd;
}

bool resdet_submit(struct resolution_detector *rd, const uint8_t *luma,
                   uint32_t width, uint32_t height)
{
    if (!rd || !luma || !width || !height)
        return false;
    {
        std::lock_guard<std::mutex> lock(rd->mtx);
        if (rd->has_job)
            return false;
        rd->job_luma.assign(luma, luma + (size_t)width * height);
        rd->job_w = width;
        rd->job_h = height;
        rd->has_job = true;
    }
    rd->cv.notify_one();
    return true;
}

bool resdet_get_result(struct resolution_detector *rd, struct resdet_result *out)
{
    if (!rd || !out)
        return false;
    std::lock_guard<std::mutex> lock(rd->result_mtx);
    *out = rd->result;
    bool fresh = rd->result_fresh;
    rd->result_fresh = false;
    return fresh;
}
