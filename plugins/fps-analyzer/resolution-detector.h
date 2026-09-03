#pragma once
#include <stdint.h>
#include <stdbool.h>

// Upscale source resolution detector.
// Hybrid DCT spectral analysis:
//  - sign method ported from resdet (https://github.com/0x09/resdet, MIT):
//    traditional resamplers act as an odd extension of the frequency
//    domain, so the DCT shows sign inversions mirrored around the index of
//    the original resolution — pixel-exact on clean upscales
//  - magnitude knee: native-res overlays (HUD, film grain) corrupt the
//    signs but the energy envelope still steps down at the source
//    resolution
// Temporal/AI upscalers (DLSS/FSR2+/TSR) partially rebuild the spectrum;
// results there are approximate at best.

struct resdet_result {
    bool valid;        // at least one analysis completed
    int frame_w, frame_h;
    int src_w, src_h;  // detected source dimension, 0 = native (no upscale found)
    double conf_w, conf_h; // confidence 0..1 for the detected candidate
};

// Spectrum thumbnail: log-magnitude of the 2D DCT (DC at top-left,
// Nyquist at bottom-right) downsampled to a fixed-size 8-bit image.
#define RESDET_SPEC_W 320
#define RESDET_SPEC_H 180

struct resolution_detector;

struct resolution_detector *resdet_create(void);
void resdet_destroy(struct resolution_detector *rd);

// Copies the luma plane and queues it for analysis on a worker thread.
// Returns false (and skips the frame) if the worker is still busy.
bool resdet_submit(struct resolution_detector *rd, const uint8_t *luma,
                   uint32_t width, uint32_t height);

// Fetches the latest analysis result. Returns true if a new result was
// produced since the previous call.
bool resdet_get_result(struct resolution_detector *rd, struct resdet_result *out);

// Copies the latest spectrum thumbnail (RESDET_SPEC_W*RESDET_SPEC_H bytes,
// 0 = no energy, 255 = peak). Returns true if it changed since last call.
bool resdet_get_spectrum(struct resolution_detector *rd, uint8_t *out);

// Fast spectrum path: the filter feeds a center crop of this size at
// 30/60 FPS to a second worker thread that only computes the thumbnail
// (detection stays on the full frame at the analysis rate). A crop — not a
// downscale — keeps the spectral cutoff at the same normalized frequency,
// so the markers still line up. ~3-5 ms per crop.
#define RESDET_FAST_CROP_W 640
#define RESDET_FAST_CROP_H 360

// While enabled, the full-frame analysis no longer overwrites the
// thumbnail; the fast path owns it.
void resdet_set_fast_spectrum(struct resolution_detector *rd, bool enabled);

// Queues a luma crop for the fast thumbnail. Returns false (frame skipped)
// if the spectrum worker is still busy.
bool resdet_submit_spectrum(struct resolution_detector *rd, const uint8_t *luma,
                            uint32_t width, uint32_t height);

// --- Debug / test-tool API (not used by the OBS plugin itself) ---

// Tunable detector parameters. The plugin runs with the defaults; the test
// tools override them per run to sweep thresholds against a frame corpus.
struct resdet_params {
    float accum_alpha;    // EMA weight of each analysis (default 0.25)
    float sign_threshold; // sign-vote fraction needed for a candidate (default 0.60)
    float knee_threshold; // knee step in log10 decades (default 0.45)
    int warmup_frames;    // analyses before anything is reported (default 4)
    int min_votes;        // agreeing analyses in the consensus history (default 3)
    float sign_cand_min;  // vote needed to be a sign candidate at all (default 0.55)
    float sign_joint_min; // W+H vote sum for an aspect-consistent pair (default 1.13)
    float sign_flank_min; // min mean vote at offsets 2..3 around a peak (default 0.47);
                          // lower = comb-like peak from a filter null, rejected
    int sign_weighted;    // 0 = count sign inversions (resdet); 1 = weight each pair by
                          // min(|a|,|b|) so the layer carrying the image's energy dominates
};
void resdet_debug_get_params(struct resolution_detector *rd, struct resdet_params *out);
// Set before submitting frames (not thread-safe against a running analysis).
void resdet_debug_set_params(struct resolution_detector *rd, const struct resdet_params *p);

struct resdet_debug_frame {
    int frame_w, frame_h;
    int sign_w, sign_h;           // sign-method pick this analysis (0 = none)
    double sign_conf_w, sign_conf_h;
    int sign_mode;                // 0 = none, 1 = independent per-axis picks, 2 = joint (same scale on both axes)
    int knee_w, knee_h;           // magnitude-knee pick this analysis (0 = none)
    double knee_conf_w, knee_conf_h;
    int frames_accumulated;       // analyses since the last dimension change
};

// Number of full-frame analyses completed so far (lets tools wait for the worker).
int resdet_debug_analysis_count(struct resolution_detector *rd);

// Raw per-analysis picks behind the last published result.
bool resdet_debug_last_frame(struct resolution_detector *rd, struct resdet_debug_frame *out);

// Copies the accumulated sign votes (votes[i] belongs to position i+range)
// and the accumulated log10 magnitude profile of an axis (0 = width,
// 1 = height). Returns the axis length, or 0 before the first analysis.
// Only call while the worker is idle (test tools).
size_t resdet_debug_axis(struct resolution_detector *rd, int axis, float *votes, size_t votes_cap,
                         float *profile, size_t profile_cap, int *range);

// Knee score for every position of a profile (0 outside the searchable range).
void resdet_debug_knee_scores(const float *profile, size_t length, double *scores);

// Colormap for the spectrum thumbnail: dark -> purple -> white (BGRA).
static inline void resdet_spectrum_color(uint8_t v, uint8_t *b, uint8_t *g, uint8_t *r)
{
    // 0..127: (8,4,16) -> (150,40,200); 128..255: -> (255,255,255)
    if (v < 128) {
        int t = v * 2; // 0..254
        *r = (uint8_t)(8 + (150 - 8) * t / 255);
        *g = (uint8_t)(4 + (40 - 4) * t / 255);
        *b = (uint8_t)(16 + (200 - 16) * t / 255);
    } else {
        int t = (v - 128) * 2; // 0..254
        *r = (uint8_t)(150 + (255 - 150) * t / 255);
        *g = (uint8_t)(40 + (255 - 40) * t / 255);
        *b = (uint8_t)(200 + (255 - 200) * t / 255);
    }
}
