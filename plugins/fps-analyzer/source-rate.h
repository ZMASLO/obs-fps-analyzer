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

// Feed the timestamp (ns) of every frame the filter receives. Repeated or
// out-of-order timestamps are ignored; a gap > 250 ms restarts the window.
void srate_push(struct srate *s, uint64_t ts_ns);

// Estimate the source rate and compare it with obs_hz (<= 0: no comparison).
// May trim the window when the source rate changes. Returns out->valid.
bool srate_get(struct srate *s, double obs_hz, struct srate_result *out);

// Formats a rate as a standard label when it matches one ("59.94", "60",
// "23.976"), otherwise with three decimals ("59.937").
void srate_format_hz(double hz, char *buf, size_t size);
