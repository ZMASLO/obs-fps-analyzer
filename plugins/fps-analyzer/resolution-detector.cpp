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
// Sign candidates: minimum vote to be considered at all, the W+H vote sum
// an aspect-consistent pair needs to win jointly (natives on the corpus
// never form a pair with both >= 0.55; true pairs on a temporal upscaler
// in motion sum ~1.13-1.19, spurious pairs <= 1.11), and the flank test
// that rejects comb-like peaks (see collect_sign_candidates).
#define SIGN_CAND_MIN 0.55f
#define SIGN_JOINT_SUM_MIN 1.13f
#define SIGN_FLANK_MIN 0.47f
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

// Spectrum thumbnail contrast: percentile stretch of the log10 block
// values — the noise floor (P_LOW) maps to black, the content band
// (P_HIGH) to white, so an upscale's energy rectangle stands out
// regardless of how much energy sits near DC.
#define SPEC_HIST_BINS 512
#define SPEC_P_LOW 0.10
#define SPEC_P_HIGH 0.90
#define SPEC_MIN_RANGE 1.0f // decades; avoids amplifying noise on flat spectra
// Fast (30/60 FPS, center-crop) spectrum path: per-frame EMA weight, and
// smoothing of the percentile bounds so the contrast doesn't flicker.
#define SPEC_FAST_ALPHA 0.30f
#define SPEC_BOUNDS_ALPHA 0.20f

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

// Knee score for every position of a (smoothed) log-magnitude profile:
// how much the energy steps down at k. A true upscale boundary stays low
// past the step, so a far window is checked too (rejects narrow dips such
// as filter nulls), and the local decay trend left of k is subtracted
// (soft content decaying into the noise floor is not a boundary).
// scores[] has `length` entries, 0 outside the searchable range.
static void knee_scores(const float *profile, size_t length, double *scores)
{
    for (size_t i = 0; i < length; i++)
        scores[i] = 0.0;

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

    for (size_t k = lo; k < hi; k++) {
        double left = (pre[k] - pre[k - W]) / W;
        double left_far = (pre[k - W] - pre[k - 2 * W]) / W;
        double right = (pre[k + W] - pre[k]) / W;
        size_t fend = k + 3 * W < length ? k + 3 * W : length;
        double far_right = (pre[fend] - pre[k + W]) / (double)(fend - (k + W));
        double step = left - right;
        double step_far = left - far_right;
        double score = step < step_far ? step : step_far;
        double trend = left_far - left;
        if (trend > 0.0)
            score -= trend;
        scores[k] = score > 0.0 ? score : 0.0;
    }
}

// Strongest knee above threshold, or 0 if none.
static void pick_knee(const float *profile, size_t length, float threshold,
                      int *out_idx, double *out_conf)
{
    *out_idx = 0;
    *out_conf = 0.0;
    std::vector<double> scores(length);
    knee_scores(profile, length, scores.data());
    double best = 0.0;
    size_t best_k = 0;
    for (size_t k = 0; k < length; k++)
        if (scores[k] > best) {
            best = scores[k];
            best_k = k;
        }
    if (best >= threshold) {
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
                          int min_votes, double *out_conf)
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
    if (n < min_votes)
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
    if (agree < min_votes)
        return 0;

    *out_conf = conf_sum / agree;
    return median;
}

// Sign-method candidates of one axis: positions in the searchable range
// whose accumulated vote is >= cand_min and whose flanks look random.
// Around a real mirror boundary, pairs misaligned by a few bins are
// uncorrelated (votes ~0.5). Around a spectral null of some filter (motion
// blur, reconstruction kernels) the coefficient signs flip systematically,
// so misaligned pairs are anti-correlated (votes well below 0.5) and the
// peaks come in combs — the flank test rejects those.
struct sign_cand {
    int pos;
    double vote;
};

static void collect_sign_candidates(const float *votes, size_t length, size_t range,
                                    float cand_min, float flank_min,
                                    std::vector<sign_cand> &out)
{
    out.clear();
    size_t count = length - 2 * range;
    size_t lo = (size_t)(length * RESDET_MIN_FRACTION);
    size_t hi = (size_t)(length * RESDET_MIN_SCALE);
    for (size_t i = 0; i < count; i++) {
        size_t pos = i + range;
        if (pos < lo)
            continue;
        if (pos >= hi)
            break;
        if (votes[i] < cand_min)
            continue;
        // Offsets 2..3: the combs seen on real content have a ~5-bin period,
        // so these land in the anti-correlated troughs, while offset 4 would
        // already touch the next tooth and dilute the test.
        float flank = 0.0f;
        int n = 0;
        for (size_t d = 2; d <= 3; d++) {
            if (i >= d) {
                flank += votes[i - d];
                n++;
            }
            if (i + d < count) {
                flank += votes[i + d];
                n++;
            }
        }
        if (n && flank / (float)n < flank_min)
            continue;
        out.push_back({(int)pos, (double)votes[i]});
    }
}

// Spectrum thumbnail state: block sums of |coeff| from a 2D DCT, EMA of
// the log10 block means, percentile stretch (smoothed bounds) -> 8-bit.
// One instance per producer (full-frame analysis, fast crop path).
struct spectrum_state {
    std::vector<float> sum, acc;
    std::vector<uint32_t> cnt;
    std::vector<uint16_t> bx, by; // coefficient index -> block
    std::vector<uint8_t> out8;
    size_t w = 0, h = 0;
    int frames = 0;
    float lo_ema = 0.0f, hi_ema = 0.0f;

    void update(const float *coeffs, size_t cw, size_t ch, float alpha)
    {
        const size_t nblk = (size_t)RESDET_SPEC_W * RESDET_SPEC_H;
        if (w != cw || h != ch) {
            w = cw;
            h = ch;
            bx.resize(w);
            for (size_t x = 0; x < w; x++)
                bx[x] = (uint16_t)(x * RESDET_SPEC_W / w);
            by.resize(h);
            for (size_t y = 0; y < h; y++)
                by[y] = (uint16_t)(y * RESDET_SPEC_H / h);
            acc.assign(nblk, 0.0f);
            frames = 0;
        }
        out8.resize(nblk);
        sum.assign(nblk, 0.0f);
        cnt.assign(nblk, 0);
        for (size_t y = 0; y < h; y++) {
            const float *row = coeffs + y * w;
            size_t base = (size_t)by[y] * RESDET_SPEC_W;
            for (size_t x = 0; x < w; x++) {
                size_t i = base + bx[x];
                sum[i] += fabsf(row[x]);
                cnt[i]++;
            }
        }
        float vmin = 1e30f, vmax = -1e30f;
        for (size_t i = 0; i < nblk; i++) {
            float mean = cnt[i] ? sum[i] / (float)cnt[i] : 0.0f;
            float v = log10f(mean + KNEE_LOG_EPS);
            if (frames == 0)
                acc[i] = v;
            else
                acc[i] += (v - acc[i]) * alpha;
            if (acc[i] < vmin)
                vmin = acc[i];
            if (acc[i] > vmax)
                vmax = acc[i];
        }
        // Percentile stretch: noise floor -> black, content band -> white
        uint32_t histo[SPEC_HIST_BINS] = {};
        float span = (vmax - vmin) > 1e-6f ? (vmax - vmin) : 1e-6f;
        for (size_t i = 0; i < nblk; i++) {
            int b = (int)((acc[i] - vmin) / span * (SPEC_HIST_BINS - 1));
            histo[b < 0 ? 0 : (b >= SPEC_HIST_BINS ? SPEC_HIST_BINS - 1 : b)]++;
        }
        auto percentile = [&](double p) {
            uint32_t target = (uint32_t)(p * (double)nblk), a = 0;
            for (int b = 0; b < SPEC_HIST_BINS; b++) {
                a += histo[b];
                if (a >= target)
                    return vmin + span * (float)b / (float)(SPEC_HIST_BINS - 1);
            }
            return vmax;
        };
        float lo = percentile(SPEC_P_LOW);
        float hi = percentile(SPEC_P_HIGH);
        if (frames == 0) {
            lo_ema = lo;
            hi_ema = hi;
        } else {
            lo_ema += (lo - lo_ema) * SPEC_BOUNDS_ALPHA;
            hi_ema += (hi - hi_ema) * SPEC_BOUNDS_ALPHA;
        }
        frames++;
        float lo2 = lo_ema, hi2 = hi_ema;
        if (hi2 - lo2 < SPEC_MIN_RANGE)
            hi2 = lo2 + SPEC_MIN_RANGE;
        float scale = 255.0f / (hi2 - lo2);
        for (size_t i = 0; i < nblk; i++) {
            float q = (acc[i] - lo2) * scale;
            out8[i] = (uint8_t)(q < 0.0f ? 0.0f : (q > 255.0f ? 255.0f : q));
        }
    }
};

struct resolution_detector {
    // full-frame analysis worker
    std::thread worker;
    std::mutex mtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    bool has_job = false;
    std::vector<uint8_t> job_luma;
    uint32_t job_w = 0, job_h = 0;

    // fast spectrum worker (center crop at 30/60 FPS)
    std::thread spec_worker;
    std::mutex spec_mtx;
    std::condition_variable spec_cv;
    bool spec_has_job = false;
    std::vector<uint8_t> spec_job;
    uint32_t spec_job_w = 0, spec_job_h = 0;
    std::atomic<bool> fast_spectrum{false};

    std::mutex result_mtx;
    resdet_result result = {};
    bool result_fresh = false;
    uint8_t spectrum[RESDET_SPEC_W * RESDET_SPEC_H] = {};
    bool spectrum_fresh = false;
    // debug/test-tool snapshot of the last analysis
    resdet_debug_frame dbg_frame = {};
    bool dbg_valid = false;
    std::atomic<int> analysis_seq{0};
    // tunable parameters (defaults = the plugin's behaviour)
    resdet_params params = {RESDET_ACCUM_ALPHA, RESDET_THRESHOLD, (float)KNEE_THRESHOLD,
                            RESDET_WARMUP_FRAMES, RESDET_MIN_VOTES,
                            SIGN_CAND_MIN, SIGN_JOINT_SUM_MIN, SIGN_FLANK_MIN};

    // analysis-worker-only state
    dct_plan plan;
    std::vector<float> coeffs;
    std::vector<float> xresult, yresult;
    std::vector<float> xprof, yprof;
    spectrum_state spec_main;
    // spectrum-worker-only state
    dct_plan spec_plan;
    std::vector<float> spec_coeffs;
    spectrum_state spec_fast;
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
    void analyze_spectrum(const uint8_t *luma, uint32_t w, uint32_t h);
    void spec_worker_loop();
    void publish_spectrum(spectrum_state &st, const float *c, size_t w, size_t h, float alpha);
};

void resolution_detector::publish_spectrum(spectrum_state &st, const float *c,
                                           size_t w, size_t h, float alpha)
{
    st.update(c, w, h, alpha);
    std::lock_guard<std::mutex> lock(result_mtx);
    memcpy(spectrum, st.out8.data(), sizeof(spectrum));
    spectrum_fresh = true;
}

// Fast path: 2D DCT of a luma crop, thumbnail only (no detection).
void resolution_detector::analyze_spectrum(const uint8_t *luma, uint32_t w, uint32_t h)
{
    if (w < 16 || h < 16 || w > RESDET_MAX_DIM || h > RESDET_MAX_DIM)
        return;
    if (!spec_plan.setup(w, h))
        return;
    spec_coeffs.resize((size_t)w * h);
    for (size_t i = 0; i < (size_t)w * h; i++)
        spec_coeffs[i] = (float)luma[i];
    dct_pass(spec_plan.cfg_w, spec_coeffs.data(), spec_plan.F.data(), spec_plan.mirror.data(),
             spec_plan.shift_w.data(), h, w, 1, w);
    dct_pass(spec_plan.cfg_h, spec_coeffs.data(), spec_plan.F.data(), spec_plan.mirror.data(),
             spec_plan.shift_h.data(), w, h, w, 1);
    publish_spectrum(spec_fast, spec_coeffs.data(), w, h, SPEC_FAST_ALPHA);
}

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
            xaccum[i] += (xresult[i] - xaccum[i]) * params.accum_alpha;
        for (size_t i = 0; i < yaccum.size(); i++)
            yaccum[i] += (yresult[i] - yaccum[i]) * params.accum_alpha;
        for (size_t i = 0; i < xprof_acc.size(); i++)
            xprof_acc[i] += (xprof[i] - xprof_acc[i]) * params.accum_alpha;
        for (size_t i = 0; i < yprof_acc.size(); i++)
            yprof_acc[i] += (yprof[i] - yprof_acc[i]) * params.accum_alpha;
        frames_accumulated++;
    }

    // Spectrum thumbnail from the full-frame DCT — every analysis (also
    // during warmup) so the panel shows early. Skipped while the fast
    // crop-based path owns the thumbnail.
    if (!fast_spectrum.load())
        publish_spectrum(spec_main, coeffs.data(), w, h, params.accum_alpha);

    if (frames_accumulated < params.warmup_frames)
        return;

    // Sign method: candidates per axis, then a joint pick. Upscaling is
    // (almost always) uniform, so two candidates at the same scale on both
    // axes are far stronger evidence than either axis alone — a pair with
    // votes 0.59+0.58 beats a lone 0.63 that has no partner. Independent
    // per-axis picks remain the fallback (horizontal-only scaling, or one
    // axis drowned in soft content).
    std::vector<sign_cand> cands_w, cands_h;
    collect_sign_candidates(xaccum.data(), w, RESDET_RANGE, params.sign_cand_min,
                            params.sign_flank_min, cands_w);
    collect_sign_candidates(yaccum.data(), h, RESDET_RANGE, params.sign_cand_min,
                            params.sign_flank_min, cands_h);
    int sign_w = 0, sign_h = 0, sign_mode = 0;
    double sconf_w = 0.0, sconf_h = 0.0;
    {
        std::vector<double> hvote(h, 0.0);
        for (auto &c : cands_h)
            hvote[c.pos] = c.vote;
        double best = 0.0;
        for (auto &c : cands_w) {
            int ky = (int)((double)c.pos * h / w + 0.5);
            for (int d = -1; d <= 1; d++) {
                int k = ky + d;
                if (k < 0 || k >= (int)h || hvote[k] <= 0.0)
                    continue;
                double s = c.vote + hvote[k];
                if (s > best) {
                    best = s;
                    sign_w = c.pos;
                    sign_h = k;
                    sconf_w = c.vote;
                    sconf_h = hvote[k];
                }
            }
        }
        if (best >= params.sign_joint_min) {
            sign_mode = 2;
        } else {
            sign_w = sign_h = 0;
            sconf_w = sconf_h = 0.0;
            for (auto &c : cands_w)
                if (c.vote >= params.sign_threshold && c.vote > sconf_w) {
                    sign_w = c.pos;
                    sconf_w = c.vote;
                }
            for (auto &c : cands_h)
                if (c.vote >= params.sign_threshold && c.vote > sconf_h) {
                    sign_h = c.pos;
                    sconf_h = c.vote;
                }
            if (sign_w || sign_h)
                sign_mode = 1;
        }
    }

    // Magnitude knee: fallback when overlays/grain destroy the sign
    // symmetry. Computed every analysis so the debug tools can compare.
    int knee_w = 0, knee_h = 0;
    double kconf_w = 0.0, kconf_h = 0.0;
    pick_knee(xprof_acc.data(), w, params.knee_threshold, &knee_w, &kconf_w);
    pick_knee(yprof_acc.data(), h, params.knee_threshold, &knee_h, &kconf_h);
    int cand_w = sign_w ? sign_w : knee_w;
    int cand_h = sign_h ? sign_h : knee_h;
    double cconf_w = sign_w ? sconf_w : kconf_w;
    double cconf_h = sign_h ? sconf_h : kconf_h;
    {
        std::lock_guard<std::mutex> lock(result_mtx);
        dbg_frame.frame_w = (int)w;
        dbg_frame.frame_h = (int)h;
        dbg_frame.sign_w = sign_w;
        dbg_frame.sign_h = sign_h;
        dbg_frame.sign_conf_w = sconf_w;
        dbg_frame.sign_conf_h = sconf_h;
        dbg_frame.sign_mode = sign_mode;
        dbg_frame.knee_w = knee_w;
        dbg_frame.knee_h = knee_h;
        dbg_frame.knee_conf_w = kconf_w;
        dbg_frame.knee_conf_h = kconf_h;
        dbg_frame.frames_accumulated = frames_accumulated;
        dbg_valid = true;
    }

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
    r.src_w = axis_consensus(hist_w, hconf_w, hist_count, params.min_votes, &r.conf_w);
    r.src_h = axis_consensus(hist_h, hconf_h, hist_count, params.min_votes, &r.conf_h);

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
            cv.wait(lock, [this] { return stop.load() || has_job; });
            if (stop.load())
                return;
            luma.swap(job_luma);
            w = job_w;
            h = job_h;
        }
        analyze(luma.data(), w, h);
        analysis_seq++;
        {
            std::lock_guard<std::mutex> lock(mtx);
            has_job = false;
        }
    }
}

void resolution_detector::spec_worker_loop()
{
    std::vector<uint8_t> luma;
    uint32_t w, h;
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(spec_mtx);
            spec_cv.wait(lock, [this] { return stop.load() || spec_has_job; });
            if (stop.load())
                return;
            luma.swap(spec_job);
            w = spec_job_w;
            h = spec_job_h;
        }
        analyze_spectrum(luma.data(), w, h);
        {
            std::lock_guard<std::mutex> lock(spec_mtx);
            spec_has_job = false;
        }
    }
}

struct resolution_detector *resdet_create(void)
{
    resolution_detector *rd = new resolution_detector();
    rd->worker = std::thread(&resolution_detector::worker_loop, rd);
    rd->spec_worker = std::thread(&resolution_detector::spec_worker_loop, rd);
    return rd;
}

void resdet_destroy(struct resolution_detector *rd)
{
    if (!rd)
        return;
    {
        std::lock_guard<std::mutex> lock(rd->mtx);
        std::lock_guard<std::mutex> lock2(rd->spec_mtx);
        rd->stop = true;
    }
    rd->cv.notify_all();
    rd->spec_cv.notify_all();
    if (rd->worker.joinable())
        rd->worker.join();
    if (rd->spec_worker.joinable())
        rd->spec_worker.join();
    delete rd;
}

void resdet_set_fast_spectrum(struct resolution_detector *rd, bool enabled)
{
    if (rd)
        rd->fast_spectrum = enabled;
}

bool resdet_submit_spectrum(struct resolution_detector *rd, const uint8_t *luma,
                            uint32_t width, uint32_t height)
{
    if (!rd || !luma || !width || !height)
        return false;
    {
        std::lock_guard<std::mutex> lock(rd->spec_mtx);
        if (rd->spec_has_job)
            return false;
        rd->spec_job.assign(luma, luma + (size_t)width * height);
        rd->spec_job_w = width;
        rd->spec_job_h = height;
        rd->spec_has_job = true;
    }
    rd->spec_cv.notify_one();
    return true;
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

bool resdet_get_spectrum(struct resolution_detector *rd, uint8_t *out)
{
    if (!rd || !out)
        return false;
    std::lock_guard<std::mutex> lock(rd->result_mtx);
    if (!rd->spectrum_fresh)
        return false;
    memcpy(out, rd->spectrum, sizeof(rd->spectrum));
    rd->spectrum_fresh = false;
    return true;
}

// --- Debug / test-tool API ---

void resdet_debug_get_params(struct resolution_detector *rd, struct resdet_params *out)
{
    if (rd && out)
        *out = rd->params;
}

void resdet_debug_set_params(struct resolution_detector *rd, const struct resdet_params *p)
{
    if (rd && p)
        rd->params = *p;
}

int resdet_debug_analysis_count(struct resolution_detector *rd)
{
    return rd ? rd->analysis_seq.load() : 0;
}

bool resdet_debug_last_frame(struct resolution_detector *rd, struct resdet_debug_frame *out)
{
    if (!rd || !out)
        return false;
    std::lock_guard<std::mutex> lock(rd->result_mtx);
    if (!rd->dbg_valid)
        return false;
    *out = rd->dbg_frame;
    return true;
}

size_t resdet_debug_axis(struct resolution_detector *rd, int axis, float *votes, size_t votes_cap,
                         float *profile, size_t profile_cap, int *range)
{
    if (!rd || rd->accum_w == 0 || rd->accum_h == 0)
        return 0;
    const std::vector<float> &v = axis == 0 ? rd->xaccum : rd->yaccum;
    const std::vector<float> &p = axis == 0 ? rd->xprof_acc : rd->yprof_acc;
    size_t length = axis == 0 ? rd->accum_w : rd->accum_h;
    if (votes) {
        size_t n = v.size() < votes_cap ? v.size() : votes_cap;
        memcpy(votes, v.data(), n * sizeof(float));
    }
    if (profile) {
        size_t n = p.size() < profile_cap ? p.size() : profile_cap;
        memcpy(profile, p.data(), n * sizeof(float));
    }
    if (range)
        *range = RESDET_RANGE;
    return length;
}

void resdet_debug_knee_scores(const float *profile, size_t length, double *scores)
{
    knee_scores(profile, length, scores);
}
