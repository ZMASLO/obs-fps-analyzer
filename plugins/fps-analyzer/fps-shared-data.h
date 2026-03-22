#pragma once
#include <stdint.h>
#include <stdbool.h>

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
    double graph_frametimes[FPS_GRAPH_HISTORY];
    double graph_fps[FPS_GRAPH_HISTORY];
    bool graph_tearing[FPS_GRAPH_HISTORY];
    int graph_count;
};

// Defined in fps-analyzer-filter.cpp
extern struct fps_shared_data g_fps_shared;
