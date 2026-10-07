#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Source refresh rate estimator.
// Measures the real frame rate of an async source (capture card, camera)
// from the timestamps its driver puts on each frame, and compares it with
// the OBS canvas FPS. A capture card is locked to the HDMI signal, so
// "Match output FPS" in Video Capture Device does not change the rate it
// delivers: a PS5 sends 59.94 Hz even when the card format says 60. With
// OBS at 60 FPS the two clocks beat and OBS repeats a frame every
// 1 / |f_src - f_obs| seconds (~16.7 s for 59.94 vs 60).
//
// The filter only sees the frames OBS picks for its own clock, so frames
// OBS skips never arrive here. Intervals are therefore counted in whole
// source periods (a skipped frame shows up as a double interval). When the
// source runs at >= 2x the OBS FPS, only every k-th frame reaches the
// filter and the measured rate is the effective one (119.88 -> 59.94),
// which is also the rate that beats against the OBS clock.

#define SRATE_STATUS_MEASURING 0 // not enough data yet
#define SRATE_STATUS_MATCHED 1   // source rate fits the OBS FPS
#define SRATE_STATUS_MISMATCH 2  // clocks differ: periodic duplicates/skips
#define SRATE_STATUS_UNSTABLE 3  // variable frame rate, no single rate to report

struct srate_result {
    bool valid;            // window long enough for a precise reading
    double source_hz;      // measured rate (also set while measuring, if any)
    double nominal_hz;     // closest standard rate (59.94, 60, ...), 0 = none
    double obs_hz;         // OBS canvas FPS used for the comparison
    int ratio;             // k: OBS shows each source frame k times (k > 1),
                           // or takes every k-th source frame (k < -1); 1 = 1:1
    double beat_period_s;  // seconds between duplicates/skips, 0 = none
    double recommended_hz; // OBS FPS that would match the source, 0 = none
    int status;            // SRATE_STATUS_*
};

struct srate;

struct srate *srate_create(void);
void srate_destroy(struct srate *s);
void srate_reset(struct srate *s);

// Feed the timestamp (ns) of every frame the filter receives, with the
// local clock (os_gettime_ns) at arrival. Repeated or out-of-order
// timestamps are ignored; a gap > 250 ms restarts the window.
// Thread-safe: push runs on the capture thread, the rest on the graphics one.
void srate_push(struct srate *s, uint64_t ts_ns, uint64_t now_ns);

// Restarts the window when no frame arrived for longer than max_idle_ns
// (source hidden, signal lost). now_ns may be older than the last push (a
// frame can arrive after the caller read its clock): that is not idle.
// Returns true when the window was reset.
bool srate_expire(struct srate *s, uint64_t now_ns, uint64_t max_idle_ns);

// Estimate the source rate and compare it with obs_hz (<= 0: no comparison).
// May trim the window when the source rate changes. Returns out->valid.
bool srate_get(struct srate *s, double obs_hz, struct srate_result *out);

// What happened to the window since the previous call (counters reset on
// each call), plus its current shape. For the OBS log while measuring.
struct srate_diag {
    int pushes;           // frames fed
    int ignored;          // repeated / older timestamps
    int gap_resets;       // restarts on a gap > 250 ms between timestamps
    double max_gap_ms;    // largest such gap
    double max_back_ms;   // largest step back among ignored timestamps
    int trims;            // window cut to the recent part (rate change)
    int expires;          // restarts on no frames for > max_idle_ns
    int count;            // timestamps in the window
    double span_s;        // window length
    double period_ms;     // median interval from the last srate_get
    double min_ms, max_ms; // shortest / longest interval in the window
    int fractional;       // intervals that are no whole/half period (last get)
    double rms_ms;        // fit residual (last get)
};
void srate_take_diag(struct srate *s, struct srate_diag *out);

// Formats a rate as a standard label when it matches one ("59.94", "60",
// "23.976"), otherwise with three decimals ("59.937").
void srate_format_hz(double hz, char *buf, size_t size);
