#pragma once
// Reference implementation: the FPS-analysis functions of plugin v0.5.0
// (commit 2a2bbb4, plugins/fps-analyzer/fps-analyzer-filter.cpp) copied
// verbatim behind the fps-core API shapes. Test-only oracle used to prove
// that the extracted fps-core reproduces v0.5.0 bit for bit. Mechanical
// substitutions only: bzalloc/bfree -> calloc/free, os_gettime_ns() ->
// now_ns argument, struct obs_source_frame -> fps_frame_view,
// VIDEO_FORMAT_* -> FPS_PIXFMT_*, g_fps_shared.fps -> last_published_fps.
// Scheduled for removal once the core has shipped in a release.
#include "fps-core.h"

struct fpsref_core;

struct fpsref_core *fpsref_create(const struct fps_core_params *p);
void fpsref_destroy(struct fpsref_core *c);
void fpsref_set_params(struct fpsref_core *c, const struct fps_core_params *p);
void fpsref_get_params(const struct fpsref_core *c, struct fps_core_params *out);
int fpsref_feed(struct fpsref_core *c, const struct fps_frame_view *frame, uint64_t now_ns);
bool fpsref_tick(struct fpsref_core *c, uint64_t now_ns, struct fps_core_output *out);

size_t fpsref_count_diff_bytes(const uint8_t *a, const uint8_t *b, size_t n);
void fpsref_bgra_to_luma(const uint8_t *bgra, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height);
void fpsref_rgba_to_luma(const uint8_t *rgba, uint32_t linesize, uint8_t *luma, uint32_t width, uint32_t height);
bool fpsref_extract_luma(const struct fps_frame_view *f, uint8_t *luma, uint32_t first_line, uint32_t lines);
int fpsref_format_csv_line(char *buf, size_t size, long long time_s, int fps, double frametime_ms);
void fpsref_csv_keep_last_n_lines(const char *path, int n);

void fpsref_debug_last_frame(const struct fpsref_core *c, struct fps_core_frame_debug *out);
void fpsref_debug_feed_decision(struct fpsref_core *c, bool unique, bool tearing, uint64_t now_ns);
int fpsref_debug_last_published_fps(const struct fpsref_core *c);
