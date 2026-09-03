#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "resolution-detector.h"

#define FPS_GRAPH_HISTORY 960

// Shared data between FPS Analyzer filter and source.
// Both run on OBS's video thread (video_tick), so no mutex needed.
struct fps_shared_data {
    int fps;
    double frametime_ms;
    bool tearing_detected;
    uint64_t last_update_ns;
    int active_filter_count;
    int unsupported_format; // -1 = ok, otherwise video_format enum value
    // Graph data — linearized (oldest to newest), ready for rendering
    double graph_frametimes[FPS_GRAPH_HISTORY];     // smoothed
    double graph_frametimes_raw[FPS_GRAPH_HISTORY]; // raw (for future use)
    double graph_fps[FPS_GRAPH_HISTORY];
    bool graph_tearing[FPS_GRAPH_HISTORY];
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
};

// Defined in fps-analyzer-filter.cpp
extern struct fps_shared_data g_fps_shared;
