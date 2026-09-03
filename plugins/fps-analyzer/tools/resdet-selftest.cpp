// resdet-selftest — synthetic regression suite for resolution-detector.cpp (no OBS deps).
#include "resolution-detector.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

static std::vector<uint8_t> make_noise(int w, int h, unsigned seed) {
    srand(seed);
    std::vector<uint8_t> img((size_t)w * h);
    for (auto &p : img)
        p = (uint8_t)(rand() % 256);
    return img;
}

static std::vector<uint8_t> bilinear_upscale(const std::vector<uint8_t> &src,
                                             int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst((size_t)dw * dh);
    for (int y = 0; y < dh; y++) {
        double fy = (y + 0.5) * sh / dh - 0.5;
        int y0 = (int)floor(fy);
        double wy = fy - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= sh) y1 = sh - 1;
        for (int x = 0; x < dw; x++) {
            double fx = (x + 0.5) * sw / dw - 0.5;
            int x0 = (int)floor(fx);
            double wx = fx - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 >= sw) x1 = sw - 1;
            double v = src[(size_t)y0 * sw + x0] * (1 - wx) * (1 - wy) +
                       src[(size_t)y0 * sw + x1] * wx * (1 - wy) +
                       src[(size_t)y1 * sw + x0] * (1 - wx) * wy +
                       src[(size_t)y1 * sw + x1] * wx * wy;
            dst[(size_t)y * dw + x] = (uint8_t)(v + 0.5);
        }
    }
    return dst;
}

// Box blur - decaying spectrum like real content; its nulls also stress
// the knee detector's dip rejection.
static std::vector<uint8_t> box_blur(const std::vector<uint8_t> &src, int w, int h) {
    std::vector<uint8_t> dst((size_t)w * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int sum = 0, cnt = 0;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && xx < w && yy >= 0 && yy < h) {
                        sum += src[(size_t)yy * w + xx];
                        cnt++;
                    }
                }
            dst[(size_t)y * w + x] = (uint8_t)(sum / cnt);
        }
    return dst;
}

// Native-res additive grain (fresh pattern per seed, like film grain)
static std::vector<uint8_t> add_grain(std::vector<uint8_t> img, int amp, unsigned seed) {
    srand(seed);
    for (auto &p : img) {
        int v = p + (rand() % (2 * amp + 1)) - amp;
        p = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    return img;
}

// Native-res "HUD": sharp white boxes with black borders near the edges
static void draw_hud(std::vector<uint8_t> &img, int w, int h) {
    int rects[][4] = {
        {40, 40, 320, 90},
        {w - 360, 40, 320, 60},
        {40, h - 140, 420, 100},
        {w - 460, h - 120, 420, 80},
        {w / 2 - 12, h / 2 - 12, 24, 24},
    };
    for (auto &r : rects)
        for (int y = r[1]; y < r[1] + r[3]; y++)
            for (int x = r[0]; x < r[0] + r[2]; x++) {
                bool border = (y - r[1] < 4) || (r[1] + r[3] - y <= 4) ||
                              (x - r[0] < 4) || (r[0] + r[2] - x <= 4);
                img[(size_t)y * w + x] = border ? 0 : 230;
            }
}

// Writes the spectrum thumbnail as a 24-bit BMP with the overlay colormap
// and green marker lines at the detected source resolution.
static void dump_spectrum_bmp(const char *path, const uint8_t *spec, const resdet_result &r) {
    const int W = RESDET_SPEC_W, H = RESDET_SPEC_H;
    int x_pos = (r.src_w > 0 && r.frame_w > 0) ? r.src_w * W / r.frame_w : -1;
    int y_pos = (r.src_h > 0 && r.frame_h > 0) ? r.src_h * H / r.frame_h : -1;
    std::vector<uint8_t> px((size_t)W * H * 3);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = &px[((size_t)(H - 1 - y) * W + x) * 3]; // BMP is bottom-up
            resdet_spectrum_color(spec[(size_t)y * W + x], &p[0], &p[1], &p[2]);
            if (x == x_pos || y == y_pos) { p[0] = 0; p[1] = 255; p[2] = 0; }
        }
    uint32_t rowbytes = W * 3, imgsize = rowbytes * H, filesize = 54 + imgsize;
    uint8_t hdr[54] = {0x42, 0x4D};
    auto put32 = [&](int off, uint32_t v) { for (int i = 0; i < 4; i++) hdr[off + i] = (uint8_t)(v >> (8 * i)); };
    auto put16 = [&](int off, uint16_t v) { hdr[off] = (uint8_t)v; hdr[off + 1] = (uint8_t)(v >> 8); };
    put32(2, filesize); put32(10, 54); put32(14, 40); put32(18, W); put32(22, H);
    put16(26, 1); put16(28, 24); put32(34, imgsize);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(hdr, 1, 54, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
    printf("  spectrum written: %s (markers x=%d y=%d)\n", path, x_pos, y_pos);
}

// Feeds `frames` generated frames through the detector and returns the
// last published result (consensus needs warmup + history agreement).
static resdet_result run_seq(const std::function<std::vector<uint8_t>(int)> &gen,
                             int w, int h, int frames = 12, const char *dump_bmp = nullptr) {
    resolution_detector *rd = resdet_create();
    resdet_result r = {};
    for (int f = 0; f < frames; f++) {
        auto img = gen(f);
        while (!resdet_submit(rd, img.data(), w, h))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        resdet_get_result(rd, &r);
    }
    for (int i = 0; i < 200; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        resdet_result tmp;
        if (resdet_get_result(rd, &tmp))
            r = tmp;
        else if (r.valid)
            break;
    }
    if (dump_bmp) {
        static uint8_t spec[RESDET_SPEC_W * RESDET_SPEC_H];
        if (resdet_get_spectrum(rd, spec))
            dump_spectrum_bmp(dump_bmp, spec, r);
        else
            printf("  (no spectrum available to dump)\n");
    }
    resdet_destroy(rd);
    return r;
}

static int check(const char *name, const resdet_result &r,
                 int exp_w, int exp_h, int tol) {
    printf("%-44s detected %dx%d (conf %.2f/%.2f)\n",
           name, r.src_w, r.src_h, r.conf_w, r.conf_h);
    if (abs(r.src_w - exp_w) > tol || abs(r.src_h - exp_h) > tol) {
        printf("  FAIL: expected ~%dx%d (tol %d)\n", exp_w, exp_h, tol);
        return 1;
    }
    return 0;
}

int main() {
    int fails = 0;

    { // 1. clean bilinear upscale - sign path, pixel-exact
        auto up = bilinear_upscale(make_noise(640, 360, 1234), 640, 360, 1920, 1080);
        auto t0 = std::chrono::steady_clock::now();
        auto r = run_seq([&](int) { return up; }, 1920, 1080);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        fails += check("clean 640x360 -> 1920x1080:", r, 640, 360, 2);
        printf("  (12 frames in ~%lld ms)\n", (long long)ms);
    }

    { // 2. clean upscale, higher source
        auto up = bilinear_upscale(make_noise(1280, 720, 99), 1280, 720, 1920, 1080);
        fails += check("clean 1280x720 -> 1920x1080:",
                       run_seq([&](int) { return up; }, 1920, 1080), 1280, 720, 2);
    }

    { // 3. native noise - no detection
        auto native = make_noise(1920, 1080, 555);
        fails += check("native 1920x1080 noise:",
                       run_seq([&](int) { return native; }, 1920, 1080), 0, 0, 0);
    }

    { // 4. native structured (box nulls stress dip rejection)
        auto native = box_blur(box_blur(make_noise(2560, 1440, 777), 2560, 1440), 2560, 1440);
        fails += check("native 2560x1440 blurred:",
                       run_seq([&](int) { return native; }, 2560, 1440), 0, 0, 0);
    }

    { // 5. structured upscale
        auto up = bilinear_upscale(box_blur(make_noise(1280, 720, 31337), 1280, 720),
                                   1280, 720, 2560, 1440);
        fails += check("blurred 1280x720 -> 2560x1440:",
                       run_seq([&](int) { return up; }, 2560, 1440), 1280, 720, 2);
    }

    { // 6. upscale + native-res film grain (kills sign, knee must catch)
        auto up = bilinear_upscale(make_noise(1280, 720, 4242), 1280, 720, 2560, 1440);
        fails += check("1280x720 -> 2560x1440 + grain12:",
                       run_seq([&](int f) { return add_grain(up, 12, 9000 + f); },
                               2560, 1440), 1280, 720, 16);
    }

    { // 7. native + film grain - no detection
        auto native = box_blur(make_noise(2560, 1440, 808), 2560, 1440);
        fails += check("native 2560x1440 + grain12:",
                       run_seq([&](int f) { return add_grain(native, 12, 7000 + f); },
                               2560, 1440), 0, 0, 0);
    }

    { // 8. upscale + static native-res HUD + light grain
        auto up = bilinear_upscale(make_noise(1707, 960, 60606), 1707, 960, 2560, 1440);
        draw_hud(up, 2560, 1440);
        fails += check("1707x960 + HUD + grain6:",
                       run_seq([&](int f) { return add_grain(up, 6, 3000 + f); },
                               2560, 1440, 12, "spectrum_hud.bmp"), 1707, 960, 16);
    }

    { // 9. soft NATIVE content + grain - spectrum decays into the grain
      // floor mid-spectrum (the in-game "shoulder" false-positive); no knee.
        auto soft = make_noise(2560, 1440, 2025);
        for (int i = 0; i < 4; i++)
            soft = box_blur(soft, 2560, 1440);
        fails += check("native soft 2560x1440 + grain4:",
                       run_seq([&](int f) { return add_grain(soft, 4, 1000 + f); },
                               2560, 1440, 12, "spectrum_native.bmp"), 0, 0, 0);
    }

    { // 10. soft UPSCALED content + grain
        auto src = box_blur(make_noise(1920, 1080, 11111), 1920, 1080);
        auto up = bilinear_upscale(src, 1920, 1080, 2560, 1440);
        fails += check("blurred 1920x1080 -> 2560x1440 + grain8:",
                       run_seq([&](int f) { return add_grain(up, 8, 5000 + f); },
                               2560, 1440, 12, "spectrum_soft_up.bmp"), 1920, 1080, 16);
    }

    { // 11. fast spectrum path: 640x360 center crops of the HUD+grain frame
        auto up = bilinear_upscale(make_noise(1707, 960, 60606), 1707, 960, 2560, 1440);
        draw_hud(up, 2560, 1440);
        const int W = 2560, H = 1440, cw = RESDET_FAST_CROP_W, ch = RESDET_FAST_CROP_H;
        const int x0 = (W - cw) / 2, y0 = (H - ch) / 2;
        std::vector<std::vector<uint8_t>> crops;
        for (int v = 0; v < 5; v++) {
            auto g = add_grain(up, 6, 8000 + v);
            std::vector<uint8_t> c((size_t)cw * ch);
            for (int y = 0; y < ch; y++)
                memcpy(&c[(size_t)y * cw], &g[(size_t)(y0 + y) * W + x0], cw);
            crops.push_back(std::move(c));
        }
        resolution_detector *rd = resdet_create();
        resdet_set_fast_spectrum(rd, true);
        auto t0 = std::chrono::steady_clock::now();
        const int n = 60;
        // spin (yield) instead of sleeping: Windows sleep granularity (~1-15 ms)
        // would otherwise dominate the measurement
        for (int f = 0; f < n; f++)
            while (!resdet_submit_spectrum(rd, crops[f % 5].data(), cw, ch))
                std::this_thread::yield();
        while (!resdet_submit_spectrum(rd, crops[0].data(), cw, ch)) // wait for last job to finish
            std::this_thread::yield();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static uint8_t spec[RESDET_SPEC_W * RESDET_SPEC_H];
        resdet_result fake = {};
        fake.valid = true; fake.frame_w = W; fake.frame_h = H; fake.src_w = 1707; fake.src_h = 960;
        printf("fast spectrum (640x360 crop): %d crops in ~%lld ms (~%.1f ms/crop)\n",
               n, (long long)ms, (double)ms / n);
        if (resdet_get_spectrum(rd, spec))
            dump_spectrum_bmp("spectrum_fast.bmp", spec, fake);
        else {
            printf("  FAIL: fast path produced no spectrum\n");
            fails++;
        }
        resdet_destroy(rd);
    }

    printf(fails ? "RESULT: %d FAILURES\n" : "RESULT: ALL OK\n", fails);
    return fails;
}
