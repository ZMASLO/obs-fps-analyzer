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

struct fps_core_params {
    int analyze_method;            // fps_analyze_method
    double sensitivity;            // % of differing bytes for a frame to count as new (>=)
    bool enable_tearing_detection;
    double tearing_sensitivity;    // % per line (>=)
    double update_interval;        // seconds between publishes; <= 0 falls back to 1.0
};

static inline struct fps_core_params fps_core_params_defaults(void)
{
    struct fps_core_params p;
    p.analyze_method = FPS_ANALYZE_LAST_LINE;
    p.sensitivity = 0.1;
    p.enable_tearing_detection = true;
    p.tearing_sensitivity = 1.0;
    p.update_interval = 1.0 / 30.0;
    return p;
}

// One captured frame, plane 0 only — that is all the analysis reads.
struct fps_frame_view {
    int format;                    // fps_pixfmt
    uint32_t width, height;
    const uint8_t *data;
    uint32_t linesize;
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
//     more clusters are required.
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

enum fps_beat_origin {
    FPS_BEAT_ORIGIN_NONE = 0,
    FPS_BEAT_ORIGIN_OBS = 1,     // ticks without a new frame
    FPS_BEAT_ORIGIN_CAPTURE = 2  // identical frames from the capture device
};

struct fps_beat {
    bool detected;
    double period_s;     // span of the chain / its number of steps
    int origin;          // fps_beat_origin of the chain
    double signal_hz;    // delivery_hz - 1/period_s (0 when delivery_hz <= 0)
    double nominal_hz;   // the standard rate signal_hz snapped to (0 = none / delivery unknown)
    int chain;           // clusters in the chain (diagnostic)
    int clusters;        // clusters seen since creation (diagnostic)
};

// One OBS video tick on an async source: `frames` is how many source frames
// reached the filter since the previous tick (0 = OBS repeated a frame).
void fps_core_obs_tick(struct fps_core *c, uint64_t now_ns, int frames);

// The current beat verdict. delivery_hz is the rate frames are delivered at
// (the OBS FPS); 0 skips the standard-rate check. Expires after 3 periods
// without a new cluster.
void fps_core_get_beat(const struct fps_core *c, uint64_t now_ns, double delivery_hz, struct fps_beat *out);

// --- Pure helpers (also used by the adapter and the tests) ---
size_t fps_core_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n);
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
    int pos, count;
};
void fps_core_debug_last_frame(const struct fps_core *c, struct fps_core_frame_debug *out);

// Replay a recorded decision without pixels: sets the tearing verdict, then
// runs the unique-frame bookkeeping iff `unique`. This is exactly the tail of
// fps_core_feed, so a recorded trace reproduces the recorded outputs.
void fps_core_debug_feed_decision(struct fps_core *c, bool unique, bool tearing, uint64_t now_ns);

// The FPS value the last publish reported (feeds back into the next window).
int fps_core_debug_last_published_fps(const struct fps_core *c);
