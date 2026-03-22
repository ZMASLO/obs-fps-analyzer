#pragma once
#include <stdint.h>
#include <stdbool.h>

// Shared data between FPS Analyzer filter and source.
// Both run on OBS's video thread (video_tick), so no mutex needed.
struct fps_shared_data {
    int fps;
    double frametime_ms;
    bool tearing_detected;
    uint64_t last_update_ns;
    int active_filter_count;
    int unsupported_format; // -1 = ok, otherwise video_format enum value
};

// Defined in fps-analyzer-filter.cpp
extern struct fps_shared_data g_fps_shared;
