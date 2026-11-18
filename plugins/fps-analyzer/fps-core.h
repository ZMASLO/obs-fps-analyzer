#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// OBS-free FPS analysis core.
//
// Everything the "FPS Analyzer" filter computes — unique-frame detection,
// frametime history, EMA smoothing, per-frame FPS, tearing detection and the
// per-tick aggregation that the overlay shows — lives behind this API with no
// libobs dependency and an explicit clock, so the exact same code can be
// driven by the OBS filter (adapter) and by the offline test tools
// (fps-selftest, fps-cli). The behaviour is pinned to plugin v0.5.0 by
// characterization tests; any intentional change must update the goldens.
//
// Do NOT include libobs headers here: the tools compile this without OBS,
// and the CI SDK is headers-only (no pthread.h).

#define FPS_CORE_HISTORY 960            // frametime/graph history (== FPS_GRAPH_HISTORY)
#define FPS_CORE_TEARING_HISTORY 5
#define FPS_CORE_MAX_TEARING_WIDTH 4096 // wider frames skip tearing detection
#define FPS_CORE_CSV_HISTORY_LIMIT 300

enum fps_analyze_method {
    FPS_ANALYZE_LAST_LINE = 0, // compare only the last luma line (fast)
    FPS_ANALYZE_DIFF = 1       // compare the whole luma plane
};

// Our own pixel-format enum; the OBS adapter maps VIDEO_FORMAT_* onto it.
enum fps_pixfmt {
    FPS_PIXFMT_UNSUPPORTED = 0,
    FPS_PIXFMT_NV12,
    FPS_PIXFMT_I420,
    FPS_PIXFMT_I444,
    FPS_PIXFMT_I422,
    FPS_PIXFMT_YUY2,
    FPS_PIXFMT_UYVY,
    FPS_PIXFMT_BGRA,
    FPS_PIXFMT_RGBA
};

// Sizes of luma differences between consecutive frames, in buckets:
// |d| = 0, 1, 2, 3-4, 5-8, 9-16, 17-32, >32. A capture card that alters
// every frame (dithering, re-encoding) shows up as differences that are all
// tiny; real new content always has large ones somewhere.
#define FPS_CORE_DELTA_BUCKETS 8

// --- Altered-frame check (needs params.measure_deltas) ---
//
// A capture device that dithers or re-encodes the picture never delivers two
// identical frames: a frame the game repeated arrives with a sprinkle of
// +-1..2 luma differences and is counted as new, so a 30 FPS game reads 60
// (Elgato 4K X, every mode). Real new content always has larger differences
// somewhere. The check counts frames that were counted as new although every
// difference is at most FPS_ALTERED_MAX_DELTA; FPS_ALTERED_TRIGGER of them
// with no identical frame in between mean the FPS cannot be measured exactly.
// FPS_ALTERED_CLEAR identical frames since then prove the device passes
// frames untouched (a slow fade at 60 FPS on a good card) and clear it.
#define FPS_ALTERED_MAX_DELTA_BUCKET 2  // |d| <= 2 (buckets 0..2)
#define FPS_ALTERED_TRIGGER 90          // ~3 s of a 30 FPS game, 1.5 s of tiny changes at 60
#define FPS_ALTERED_CLEAR 5

struct fps_core_params {
    int analyze_method;            // fps_analyze_method
    double sensitivity;            // % of differing bytes for a frame to count as new (>=)
    bool enable_tearing_detection;
    double tearing_sensitivity;    // % per line (>=)
    double update_interval;        // seconds between publishes; <= 0 falls back to 1.0
    bool measure_deltas;           // the altered-frame check: largest |d| per frame (cheap)
    bool measure_histogram;        // the full per-frame |d| histogram (frame debug, trace v4) - only
                                   // while a trace is recorded; implies what measure_deltas gives
    bool beat_enabled;             // run the capture-path beat detector (fps_core_obs_tick et al.)
    uint64_t pacing_period_ns;     // OBS frame interval: frame pacing without source timestamps
                                   // (sync path) is rounded to whole multiples of it; 0 = not rounded
    bool detect_drops;             // count frames lost by the capture device / OBS (fps_core_get_drops)
};

static inline struct fps_core_params fps_core_params_defaults(void)
{
    struct fps_core_params p;
    p.analyze_method = FPS_ANALYZE_LAST_LINE;
    p.sensitivity = 0.1;
    p.enable_tearing_detection = true;
    p.tearing_sensitivity = 1.0;
    p.update_interval = 1.0 / 30.0;
    p.measure_deltas = false;
    p.measure_histogram = false;
    p.beat_enabled = true;
    p.pacing_period_ns = 0;
    p.detect_drops = true;
    return p;
}

// One captured frame, plane 0 only — that is all the analysis reads.
struct fps_frame_view {
    int format;                    // fps_pixfmt
    uint32_t width, height;
    const uint8_t *data;
    uint32_t linesize;
    uint64_t src_ts_ns;            // the capture device's timestamp of this frame, 0 = none (sync path)
};

enum fps_feed_status {
    FPS_FEED_OK = 0,
    FPS_FEED_UNSUPPORTED_FORMAT = 1, // nothing was analysed, state unchanged
    FPS_FEED_NO_DATA = 2
};

// What one publish (tick) produces for the overlay.
struct fps_core_output {
    int fps;
    double frametime_ms;
    bool tearing_detected;         // latest per-frame verdict
    int window;                    // samples averaged for this publish (diagnostic)
    int graph_count;
    double graph_frametimes[FPS_CORE_HISTORY];     // EMA-smoothed, oldest -> newest
    double graph_frametimes_raw[FPS_CORE_HISTORY];
    double graph_fps[FPS_CORE_HISTORY];
    bool graph_tearing[FPS_CORE_HISTORY];
    // Frame pacing (Digital Foundry style): how long each game frame was on
    // screen, unsmoothed, from the capture device's timestamps (whole refresh
    // periods). One sample per new frame, same positions as graph_frametimes.
    double graph_pacing[FPS_CORE_HISTORY];
    // the capture device lost a frame within this sample: the value is kept,
    // but it is not the game's pacing
    bool graph_pacing_gap[FPS_CORE_HISTORY];
};

struct fps_core;

struct fps_core *fps_core_create(const struct fps_core_params *p);
void fps_core_destroy(struct fps_core *c);
// Applies new parameters without resetting any state (matches the filter's update()).
void fps_core_set_params(struct fps_core *c, const struct fps_core_params *p);
void fps_core_get_params(const struct fps_core *c, struct fps_core_params *out);

// Analyse one frame. now_ns is the analysis-time clock (the filter passes
// os_gettime_ns(); the tools pass a synthetic or recorded time) — it is the
// only timestamp that drives FPS. Order: tearing detection (every frame, if
// enabled) -> luma extraction per method -> compare with the previous frame
// -> frametime/EMA/per-frame-FPS bookkeeping for unique frames.
int fps_core_feed(struct fps_core *c, const struct fps_frame_view *frame, uint64_t now_ns);

// One OBS video tick. Returns true when the update-interval gate passed and
// `out` was filled (the overlay values changed).
bool fps_core_tick(struct fps_core *c, uint64_t now_ns, struct fps_core_output *out);

// --- Capture-path beat detector ---
//
// A source whose frame rate differs slightly from the rate frames are
// delivered at (59.94 Hz console into a 60 FPS pipeline) repeats one frame
// every 1 / (f_delivery - f_source) seconds (~16.7 s for 59.94 vs 60). The
// FPS analysis then shows a periodic dip although the game runs locked. The
// repeat is made either by OBS (a tick on which no new source frame reached
// the filter) or by the capture card itself (a new frame with identical
// content, while the card's timestamps claim a perfect 60 Hz). Neither
// depends on the frame timestamps, so this detector works from the cadence:
//
//   - an event is an OBS tick without a new frame, or a duplicate frame,
//     arriving after at least FPS_BEAT_CLEAN_RUN clean units (ticks with
//     exactly one frame / unique frames) — a game running below the delivery
//     rate never gets that far;
//   - events closer than FPS_BEAT_CLUSTER_NS form one cluster, timed at the
//     mean of its events (OBS repeats come in bursts around the phase
//     crossing, wider the more the arrival times jitter). FPS_BEAT_STALL_RUN
//     events in a row (a static scene, a loading screen, a lost signal) make
//     the cluster a stall, which is discarded;
//   - a beat is a chain of clusters ending with the latest one, spaced by a
//     constant period P, each within a tolerance of where P predicts it.
//     Clusters in between are ignored, so a game that drops the odd frame
//     does not hide the beat. OBS clusters (empty ticks) and capture clusters
//     (identical frames) are chained separately:
//       OBS:      no game can cause an empty tick, but arrival jitter makes
//                 stray ones and blurs their timing ->
//                 FPS_BEAT_CHAIN_OBS clusters within FPS_BEAT_TOL_OBS_S;
//       capture:  a dropped game frame looks exactly like one, but the card's
//                 repeats are sharp in time -> FPS_BEAT_CHAIN_CAPTURE clusters
//                 within FPS_BEAT_TOL_CAPTURE_S;
//   - the shortest period with a full chain decides; when the delivery
//     rate D is known, D - 1/P must be within
//     FPS_BEAT_SNAP_TOL of a standard frame rate (59.94, 60, ...): a real
//     beat is the difference of two standard clocks, a coincidence of game
//     hitches almost never is. When it snaps to D itself (two crystals of the
//     same nominal rate drifting, P of minutes) FPS_BEAT_CHAIN_DRIFT_EXTRA
//     more clusters are required;
//   - a device losing frames in bursts (the 4K Mk2 with a PC sending YCbCr,
//     18.11.2026: bursts every ~16.7 s of empty ticks, timestamp gaps and
//     identical frames) must not pass for a beat:
//       OBS:      empty ticks every P need source frames arriving about 1/P
//                 off the OBS rate. Once FPS_BEAT_ARRIVAL_MIN_S has been
//                 watched, the arrival rate (the device's frame rate scaled
//                 by its clock against the analysis clock) must differ from
//                 the delivery rate by at least FPS_BEAT_ARRIVAL_MIN_FRAC / P,
//                 else the chain is rejected (arrival_rejected). Beats slower
//                 than FPS_BEAT_ARRIVAL_MIN_BEAT_HZ (drift) skip this test;
//       capture:  a card resampling a slower source keeps its timestamps
//                 perfect, so a capture cluster with a timestamp gap in it,
//                 or within FPS_BEAT_GAP_BEFORE_NS before it, is the device
//                 losing frames and is discarded (lossy_clusters).
//     Without device timestamps (sync path) neither test applies.
//
// Until a beat is confirmed the verdict is CHECKING, not "fine": a console
// at 59.94 Hz into 60 FPS repeats a frame only every 1001/D seconds, so
// "no repeat yet" means nothing for the first ~17 s. CLEAR is given once the
// detector has watched for verify_s = max(FPS_BEAT_VERIFY_MIN_S,
// FPS_BEAT_VERIFY_PERIODS * 1001 / D) and no repeat was seen in the last
// 2 * 1001/D: a beat would have repeated within that time, even with one of
// its repeats missed. A game that keeps dropping frames keeps the check
// waiting (`uneven`); that stutter is the game's and shows on the FPS graph.
// Repeats 1 or 2 periods from one another are the card's own beat, not the
// game, and do not count towards `uneven`. A lost signal (no frames for
// FPS_BEAT_RESET_GAP_NS) or fps_core_beat_reset() starts the check over.
// Clock drift (a period of minutes) cannot be ruled out this fast; CLEAR
// means "no 1000/1001 mismatch", and drift is still reported if it shows.
//
// It only observes: the FPS readings are unaffected.

#define FPS_BEAT_CLEAN_RUN 30
#define FPS_BEAT_CLUSTER_NS 1000000000ULL
#define FPS_BEAT_STALL_RUN 5
#define FPS_BEAT_MAX_EVENTS 120
#define FPS_BEAT_MIN_PERIOD_S 2.0
#define FPS_BEAT_CHAIN_OBS 4
#define FPS_BEAT_TOL_OBS_S 0.3
#define FPS_BEAT_CHAIN_CAPTURE 4
#define FPS_BEAT_TOL_CAPTURE_S 0.1
#define FPS_BEAT_CHAIN_DRIFT_EXTRA 1
#define FPS_BEAT_SNAP_TOL 150e-6   // a real beat lands within ~20 ppm; crystals add up to ~100
#define FPS_BEAT_END_SEARCH 3   // chains may end this many clusters before the newest
#define FPS_BEAT_HISTORY 16
#define FPS_BEAT_VERIFY_MIN_S 40.0
#define FPS_BEAT_VERIFY_PERIODS 2.4
#define FPS_BEAT_RESET_GAP_NS 2000000000ULL
#define FPS_BEAT_UNEVEN_CLUSTERS 3
#define FPS_BEAT_ARRIVAL_MIN_S 20.0       // arrival rate measured over at least this long
#define FPS_BEAT_ARRIVAL_MIN_FRAC 0.5     // |delivery - arrival| >= this * 1/P for an OBS beat
#define FPS_BEAT_ARRIVAL_MIN_BEAT_HZ 0.02 // slower beats (drift) skip the arrival test
#define FPS_BEAT_GAP_BEFORE_NS 500000000ULL

enum fps_beat_status {
    FPS_BEAT_CHECKING = 0,  // not watched long enough to rule a mismatch out
    FPS_BEAT_CLEAR = 1,     // watched verify_s without a suspect repeat
    FPS_BEAT_DETECTED = 2   // a confirmed beat (see the fields below)
};

enum fps_beat_origin {
    FPS_BEAT_ORIGIN_NONE = 0,
    FPS_BEAT_ORIGIN_OBS = 1,     // ticks without a new frame
    FPS_BEAT_ORIGIN_CAPTURE = 2  // identical frames from the capture device
};

struct fps_beat {
    int status;          // fps_beat_status
    double observed_s;   // time watched since the check (re)started
    double verify_s;     // how long CLEAR needs (0 when the delivery rate is unknown)
    bool uneven;         // FPS_BEAT_UNEVEN_CLUSTERS+ repeats in the window that no beat explains: the game drops frames
    bool detected;       // == (status == FPS_BEAT_DETECTED)
    double period_s;     // span of the chain / its number of steps
    int origin;          // fps_beat_origin of the chain
    double signal_hz;    // delivery_hz - 1/period_s (0 when delivery_hz <= 0)
    double nominal_hz;   // the standard rate signal_hz snapped to (0 = none / delivery unknown)
    int chain;           // clusters in the chain (diagnostic)
    int clusters;        // clusters seen since creation (diagnostic)
    int lossy_clusters;  // capture clusters discarded: a timestamp gap in them (diagnostic)
    double arrival_hz;   // source frames per second on the analysis clock (0 = unknown)
    bool arrival_rejected; // an OBS chain was rejected: frames arrive in step with OBS
};

// One OBS video tick on an async source: `frames` is how many source frames
// reached the filter since the previous tick (0 = OBS repeated a frame).
void fps_core_obs_tick(struct fps_core *c, uint64_t now_ns, int frames);

// Starts the check over (the OBS FPS or the capture device's FPS changed).
void fps_core_beat_reset(struct fps_core *c, uint64_t now_ns);

// The current beat verdict. delivery_hz is the rate frames are delivered at
// (the OBS FPS); 0 skips the standard-rate check. Expires after 3 periods
// without a new cluster.
void fps_core_get_beat(const struct fps_core *c, uint64_t now_ns, double delivery_hz, struct fps_beat *out);

// --- Pure helpers (also used by the adapter and the tests) ---
size_t fps_core_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n);
// The differing bytes and the largest |a - b| in one pass (SSE2 on x86-64,
// a plain loop elsewhere and for the tail; both give the same result).
size_t fps_core_count_diff_max(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *max_delta);
// The same, the plain loop only (reference for the tests)
size_t fps_core_count_diff_max_scalar(const uint8_t *a, const uint8_t *b, size_t n, uint8_t *max_delta);
void fps_core_bgra_to_luma(const uint8_t *bgra, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height);
void fps_core_rgba_to_luma(const uint8_t *rgba, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height);
// Luma of `lines` rows starting at `first_line` into `luma` (width bytes per
// row). Returns false for an unsupported pixel format.
bool fps_core_extract_luma(const struct fps_frame_view *f, uint8_t *luma, uint32_t first_line, uint32_t lines);
// The CSV log line: "<time>,<fps>,<frametime with 2 decimals>\n"
int fps_core_format_csv_line(char *buf, size_t size, long long time_s, int fps, double frametime_ms);
// Truncates a CSV file to its last n lines (256-byte line granularity, as v0.5.0).
void fps_core_csv_keep_last_n_lines(const char *path, int n);

// --- Debug / replay API (test tools and the trace recorder) ---

// Details of the last fps_core_feed. pos/count are the history position and
// size after the frame was processed.
struct fps_core_frame_debug {
    size_t luma_size;
    bool compared;                 // false on the first frame / size change (no diff computed)
    size_t diff;
    double percent;
    bool unique;
    bool tearing_flag;             // verdict for this frame
    bool sample_written;           // a frametime sample was recorded
    double ft_ms, ema, fps_pf;     // valid when sample_written
    double pacing_ms;              // frame pacing sample, valid when sample_written
    bool pacing_gap;               // ... with a frame lost by the capture device
    int pos, count;
    bool deltas_valid;             // compared and params.measure_histogram
    uint32_t deltas[FPS_CORE_DELTA_BUCKETS]; // bytes per |d| bucket; deltas[0] = luma_size - diff
};
void fps_core_debug_last_frame(const struct fps_core *c, struct fps_core_frame_debug *out);

// Replay a recorded decision without pixels: sets the tearing verdict, then
// runs the unique-frame bookkeeping iff `unique`. This is exactly the tail of
// fps_core_feed, so a recorded trace reproduces the recorded outputs.
void fps_core_debug_feed_decision(struct fps_core *c, bool unique, bool tearing, uint64_t now_ns);

// The FPS value the last publish reported (feeds back into the next window).
int fps_core_debug_last_published_fps(const struct fps_core *c);

// True while the altered-frame check holds (see FPS_ALTERED_TRIGGER).
bool fps_core_frames_altered(const struct fps_core *c);

// --- Dropped frames (who lost them: the capture device or OBS) ---
//
// A frame the capture device delivered at 2x (3x...) the usual interval
// after the previous one means frames were lost on the way: round(gap /
// median) - 1 of them. An OBS tick without a new frame within 3 ticks of the
// gap, before or after it (OBS buffers frames, so it is rarely right next to
// it) says the device never delivered them (the card lost them); no such
// tick says
// the device did, but OBS took the newer of two frames on one tick (OBS lost
// them, rendering lag). A 59.94-into-60 beat makes empty ticks without a gap
// and a game's own stutter makes neither, so neither is counted. A source
// whose intervals are not whole multiples of one period (a camera, VRR) is
// variable: nothing is counted there. The FPS is not corrected - whether a
// lost frame was new or a repeat cannot be known - the overlay warns that it
// reads low.
#define FPS_DROPS_WINDOW_NS 5000000000ULL // rate over the last 5 s
#define FPS_DROPS_HOLD_NS 5000000000ULL   // the warning stays 5 s after the last loss
#define FPS_DROPS_MAX_GAP_NS 250000000ULL // longer: the stream was interrupted, nothing counted
#define FPS_DROPS_EMPTY_TICK_LOOKBACK 3  // an empty OBS tick within this many ticks of a gap: the card lost it

struct fps_drops {
    double card_per_s, obs_per_s; // lost frames per second over the last 5 s
    uint64_t card_total, obs_total;
    bool active;                  // a loss within FPS_DROPS_HOLD_NS
    bool variable_source;         // intervals are not whole periods: not counted
};
void fps_core_get_drops(const struct fps_core *c, uint64_t now_ns, struct fps_drops *out);

// Empties the graph histories (frametime, EMA, per-frame FPS, tearing, frame
// pacing) and the published averages, as if the source had just started.
// Keeps the reference frame and the beat / altered-frame checks: those are
// verdicts about the capture path, not graphs.
void fps_core_clear_history(struct fps_core *c);

// Replay: the source timestamp of the next frame given to
// fps_core_debug_feed_decision (a trace's src_ts column), 0 = none.
void fps_core_debug_set_source_ts(struct fps_core *c, uint64_t src_ts_ns);

// Replay of a v4 trace row's difference histogram into the altered-frame
// check, without pixels. Call before fps_core_debug_feed_decision for the
// same frame; `unique` is the recorded decision.
void fps_core_debug_feed_deltas(struct fps_core *c, size_t diff, bool unique,
                                const uint32_t deltas[FPS_CORE_DELTA_BUCKETS]);
