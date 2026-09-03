#pragma once
#include <stdint.h>
#include <stdbool.h>

// Upscale source resolution detector.
// Spectral method ported from resdet (https://github.com/0x09/resdet, MIT):
// traditional resamplers (bilinear/bicubic/lanczos) act as an odd extension
// of the signal's frequency domain — the DCT of an upscaled image shows
// sign inversions mirrored around the index of the original resolution.
// Note: does NOT detect AI-based upscalers (DLSS/FSR2+/PSSR), which add
// new information instead of resampling.

struct resdet_result {
    bool valid;        // at least one analysis completed
    int frame_w, frame_h;
    int src_w, src_h;  // detected source dimension, 0 = native (no upscale found)
    double conf_w, conf_h; // confidence 0..1 for the detected candidate
};

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
