#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "resolution-detector.h"
#include "source-rate.h"
#include "plot-layout.h"

#define FPS_GRAPH_HISTORY 960

// Why the FPS cannot be measured exactly right now. While it is not NONE the
// overlay shows the reason instead of any number, and nothing is logged to
// the CSV: a wrong reading is worse than none.
enum fps_block_reason {
    FPS_BLOCK_NONE = 0,
    FPS_BLOCK_PIXFMT = 1,     // the frames' pixel format is not supported
    FPS_BLOCK_COMPRESSED = 2, // the capture device sends a compressed format (MJPEG, H.264)
    FPS_BLOCK_ALTERED = 3     // the capture device alters every frame: repeats are never identical
};

// Shared data between FPS Analyzer filter and source.
// Both run on OBS's video thread (video_tick), so no mutex needed.
struct fps_shared_data {
    int fps;
    double frametime_ms;
    bool tearing_detected;
    uint64_t last_update_ns;
    int active_filter_count;
    int blocked_reason;      // FPS_BLOCK_*
    char blocked_detail[32]; // format name for PIXFMT / COMPRESSED ("P010", "MJPEG")
    bool reliability_off;    // the filter's "Block FPS when the capture is unreliable" is off
    // Dropped frames (fps_core_get_drops + OBS's render lag): the FPS reads low
    bool drops_active;
    double drops_card_per_s; // lost by the capture device
    double drops_obs_per_s;  // lost by OBS (rendering lag)
    uint64_t srate_wanted_ns; // last tick an overlay showed the Source refresh rate line:
                              // the filter computes source rate and beat only while it is fresh
    // Graph data — linearized (oldest to newest), ready for rendering
    double graph_frametimes[FPS_GRAPH_HISTORY];     // smoothed
    double graph_frametimes_raw[FPS_GRAPH_HISTORY]; // raw (for future use)
    double graph_fps[FPS_GRAPH_HISTORY];
    bool graph_tearing[FPS_GRAPH_HISTORY];
    double graph_pacing[FPS_GRAPH_HISTORY];     // frame pacing per game frame, unsmoothed (ms)
    bool graph_pacing_gap[FPS_GRAPH_HISTORY];   // the capture device lost a frame in this sample
    // The overlay's graphs: one time-stamped sample per new game frame
    // (plot-layout.h). Each graph shows its own window of it.
    struct plot_ring plot;
    int graph_count;
    // Upscale source resolution detection (DCT spectral analysis)
    bool res_detect_enabled;  // filter checkbox state
    bool res_valid;           // at least one analysis completed
    int res_frame_w, res_frame_h; // analyzed frame (output) resolution
    int res_src_w, res_src_h;     // detected source dimension, 0 = native
    double res_conf_w, res_conf_h; // confidence 0..1
    int res_status;           // RESDET_STATUS_* (detected / no signature / periodic-unreliable)
    uint8_t res_spectrum[RESDET_SPEC_W * RESDET_SPEC_H]; // DCT log-magnitude thumbnail
    uint32_t res_spectrum_version; // bumped when res_spectrum changes (0 = none yet)
    // Real source refresh rate vs OBS FPS (source-rate.h)
    bool srate_available;        // filter sits on an async source (has frame timestamps)
    bool srate_valid;            // window long enough for a verdict
    int srate_status;            // SRATE_STATUS_*
    double srate_source_hz;      // measured (preliminary while measuring)
    double srate_obs_hz;         // OBS canvas FPS
    double srate_beat_s;         // seconds between duplicates/skips, 0 = none
    double srate_recommended_hz; // OBS FPS that would match, 0 = none
    // Capture-path beat from the frame cadence (fps-core.h) — works where the
    // device's timestamps only echo its configured rate (Elgato 4K X)
    int beat_status;             // FPS_BEAT_CHECKING / CLEAR / DETECTED
    double beat_observed_s;      // how long the current signal check has run
    double beat_verify_s;        // how long it needs before CLEAR
    bool beat_uneven;            // the game drops frames: the check takes longer
    bool beat_detected;
    int beat_origin;             // FPS_BEAT_ORIGIN_* (OBS / capture device)
    double beat_period_s;
    double beat_signal_hz;       // standard rate the signal snapped to (raw estimate if none)
    bool beat_drift;             // signal and OBS share a nominal rate: crystal drift
    double device_hz;            // capture device FPS setting, 0 = unknown / highest
    bool device_matching;        // device set to "Match output FPS"
};

// Defined in fps-analyzer-filter.cpp
extern struct fps_shared_data g_fps_shared;
