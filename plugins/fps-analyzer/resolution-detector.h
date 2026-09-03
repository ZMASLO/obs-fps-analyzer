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
