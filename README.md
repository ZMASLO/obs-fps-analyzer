# obs-fps-analyzer

OBS FPS Analyzer is a plugin for OBS Studio that analyzes video in real-time and calculates "real FPS" of games on consoles or PC. The plugin is heavily inspired by tools like trdrop and the proprietary FPSGui tool from Digital Foundry.

## Why obs-fps-analyzer (vs trdrop)?

Don't get me wrong, trdrop is a good tool and I have great respect for the authors, but the software has some drawbacks that really bothered me.

### Pros:
- **Real-time analysis** - you can tweak your testing procedure on the fly
- **No need to record RAW video** - saves a lot of disk space
- **Faster operation** - trdrop takes a very long time to process graphs and can only produce pictures that you have to manually combine and render

### Cons:
- **No fancy graphs** - text only
- **Works only with V-SYNC enabled**
- **Some OBS limitations**

## Supported capture devices:

The FPS is counted from frames that differ from the previous one, so it is only exact when the capture card passes every frame through **untouched**: a frame the game repeated must arrive byte for byte identical. When a reading cannot be exact, the plugin does not show a number at all, it shows the reason instead (see **Reliability check** below). A wrong FPS is worse than none.

- **Elgato 4K Mk2** (PCI-E) — **works**. Tested with v0.5.2 (YUY2 4K, PS5): repeated frames are identical, a 30 FPS game reads 30
- **Elgato HD60 X** — works (reported earlier)
- **Avermedia Livegamer 4K** — works (reported earlier)
- **Elgato 4K X** (USB) — **not supported, the FPS is hidden**. The card alters every frame (dithering): a frame the game repeated arrives with ±1 luma differences on 6–34 % of its pixels, so it counts as new and a 30 FPS game reads 60. Tested with a PS5 in every mode the card offers in OBS, none of them passes frames untouched:

  | 4K X capture mode | PS5 signal | Repeated frame differs on | Reading (game at 30) |
  |---|---|---|---|
  | NV12 4K60 | 4K YUV 10-bit HDR | 15–20 % of pixels | 60 |
  | NV12 4K60 | 4K RGB 8-bit SDR | 27–30 % | 60 |
  | I420 4K60 | 4K RGB 8-bit SDR | 8–10 % | 60 |
  | YUY2 1080p60 | 4K RGB 8-bit SDR | 6–7 % | 60 |
  | XRGB 1080p60 | 4K RGB 8-bit SDR | 6–8 % | 60 |
  | XRGB 1080p60 | 1080p RGB, limited or full range | 31–34 % | 60 |
  | MJPEG (any) | any | compressed, see below | 60 |

  Even a paused game, where nothing on screen moves, arrives as a different picture on every frame. The same console on a 4K Mk2 gives identical repeats

If you have a different capture card, please report whether it works: record an FPS trace (Debug options) with a game capped at 30 FPS and open an issue with the file.

## Supported formats:
- **Pixel formats** the analysis reads: NV12, I420, I422, I444, YUY2, UYVY, BGRA, **BGRX** (XRGB capture, newly supported), RGBA. Any other format (P010, I010, YVYU, Y800, AYUV, …) shows `FPS not measured: unsupported video format (P010)` and no number
- **Compressed capture formats** (MJPEG, H.264, HEVC) in Video Capture Device → Video Format are **not supported**: every frame is compressed on its own, so a frame the game repeated decodes to a slightly different picture and counts as new. The plugin shows `FPS not measured: the capture device sends MJPEG…` instead of a number. Set Video Format to an uncompressed one (NV12, YUY2, I420). Leave it on an explicit format rather than "Any": with "Any" OBS may pick MJPEG without saying so (the altered-frame check below still catches it)

## Installation:
Copy `fps-analyzer.dll` to `C:\Program Files\obs-studio\obs-plugins\64bit`

## How to use?

### Basic Configuration:
1. **Add filter** to source (e.g., Elgato HD60 X)
2. **Select analysis method** (recommended: Last line diff for most cases)
3. **Set output file path**
4. **Configure update interval** (default: 1 second)
5. add **Text (GDI+)**: Use TXT file as text source

## Analysis Methods:

The plugin offers two different image analysis methods:

### 1. Last line diff (pixel analysis) - **Default**
- **Speed**: Fastest
- **Accuracy**: High for stable sources
- **Use case**: Ideal for most games with V-Sync
- **Description**: Analyzes pixel differences in the last line of the image (luminance)
- **Settings**: "Sensitivity threshold" slider (0.0-5.0%)

### 2. Full frame analysis (all lines)
- **Speed**: Medium
- **Accuracy**: Very high
- **Use case**: When you need maximum precision
- **Description**: Analyzes every pixel in the frame and calculates the percentage of differences
- **Settings**: "Sensitivity threshold" slider (0.0-5.0%)


### Dropped frames (capture device or OBS):
- **Setting**: "Warn when the capture device or OBS drops frames (recommended)" in the filter's Reliability section, **on** by default
- **What it shows**: a frame lost on the way to the analysis makes the FPS read low although the game did not lose it. The overlay then says who drops frames and how many per second — `Capture device drops frames (13/s) - FPS reads low` or `OBS drops frames (5/s, rendering lag) - FPS reads low` — and keeps saying it for 5 s after the last loss. The FPS stays visible and is not corrected (whether a lost frame was new or a repeat cannot be known); the frame pacing graph marks those steps grey. The OBS log gets a line when an episode starts and one with the totals when it ends
- **How it tells them apart**: the capture device's timestamps show the gap (two periods instead of one). An OBS tick that got no new frame within 3 ticks of it (OBS buffers frames, so the empty tick can come a little before or after) means the device never delivered the frame; no such tick means it did and OBS took the newer of two (rendering lag). OBS's own lagged-frame counter is used too, which is the only source for Game/Display Capture (no device timestamps). A 59.94-into-60 beat (empty ticks without a gap) and a game's own stutter (no gap at all) are not counted
- **What to do**: the capture device → restart the source in OBS (Deactivate / Activate, or change the resolution and back), check the card's temperature and airflow, turn off HDR tone mapping in Elgato 4K Capture Utility, update the driver. The 4K Mk2 has been seen dropping 5–13 frames a second until restarted. OBS → reduce the rendering load (fewer/lighter sources, lower canvas resolution)
- **Limitations**: sources whose frame intervals are not whole multiples of one period (cameras, VRR) are left out; while the card drops many frames, an occasional OBS loss right next to one of them is counted for the card

### Reliability check (FPS hidden when it cannot be exact):
- **Setting**: "Block FPS when the capture is unreliable (recommended)" in the filter, **on** by default. Turn it off only for testing: the readings are then shown, but they can be wrong, and the overlay says `(reliability check off)`
- **What it blocks**: a compressed capture format (above) and a capture device that **alters every frame**. Instead of FPS, frametime and the graphs the overlay shows the reason, e.g. `FPS not measured: the capture device alters every frame (repeated frames are never identical)…`. Nothing is written to the CSV/TXT output while blocked, and the OBS log gets one line when it starts and one when it ends
- **How a frame-altering device is recognised**: the filter sorts every difference between consecutive frames by size. A dithering card makes a repeated frame differ only by 1–2 luma levels, while new game content always has larger differences somewhere. 90 frames in a row that were counted as new although every difference was at most 2 levels, with no identical frame in between (~3 s of a 30 FPS game, ~1.5 s on a still picture), hide the FPS. 5 identical frames, which only a device that passes frames untouched can deliver, bring it back
- **Limitations**:
  - a game at a locked 60 FPS has no repeated frames, so a frame-altering device cannot be told apart until the picture stands still or the game drops a frame; the reading is then 60, which is right until the first repeat
  - on a good card, a fade lasting more than ~1.5 s in which every frame changes by at most 2 levels, with no repeated frame, can hide the FPS until the next repeated frame
- **Cost**: the size sort runs in the same loop as the frame comparison, on the same pixels (one line with "Last line diff")

### Upscale Source Resolution Detection:
- **Independent feature**: Works with any analysis method
- **Description**: Estimates the internal render resolution the image was upscaled from (e.g. a console game rendering at 1280x720 output as 1920x1080)
- **Settings**:
  - Filter: "Detect upscale source resolution (DCT)" checkbox (default: disabled)
  - Overlay: "Show Source resolution text" checkbox (default: enabled)
  - Filter: "Spectrum refresh rate" dropdown — "Analysis rate" (~2/s, default), "30 FPS" or "60 FPS". The fast modes compute the spectrum from a 640x360 center crop on a second background thread (~20% / ~40% of one core); detection itself always uses the full frame at the analysis rate. A crop keeps the spectral cutoff at the same normalized frequency, so the markers still line up.
  - Overlay: "Show Source resolution spectrum" checkbox (default: enabled) — a small panel with the 2D DCT log-magnitude spectrum of the frame (low frequencies top-left, bright = energy) and green tick markers with the detected source width (bottom edge) and height (right edge). On an upscaled image the energy forms a visible rectangle ending at the markers; a native image fills the whole panel.
- **Algorithm**: hybrid DCT spectral analysis, ~2 analyses per second on a background thread:
  - *Sign method* (ported from [resdet](https://github.com/0x09/resdet)): traditional resamplers mirror the spectrum with inverted signs around the source resolution index — pixel-exact on clean upscales (videos, menus). Candidates whose flanks are anti-correlated (comb-like peaks caused by spectral nulls of motion blur or reconstruction kernels) are rejected, and the final pick is made *jointly* for both axes: upscaling is almost always uniform, so a pair of candidates at the same scale on width and height outranks a lone stronger peak on one axis (this is what makes temporal upscalers like FSR2 readable in motion). Independent per-axis picks remain the fallback. If several mutually inconsistent aspect-consistent pairs pass at once, the frame is treated as periodic (dither lattice harmonics, e.g. id Tech in motion) and nothing is reported.
  - *Magnitude knee* (fallback): native-res overlays like HUD or film grain corrupt the sign symmetry, but the energy envelope still drops sharply at the source resolution; the detector finds the strongest step in the log-magnitude profile of each axis.
  - Results are accumulated over consecutive frames (EMA) and reported only when recent analyses agree (median consensus) — smooths dynamic resolution scaling and rejects sporadic false positives.
- **Output** (three honest states):
  - `Source res: ~1280x720 -> 1920x1080 (95%)` — a linear upscale signature was found
  - `Source res: no upscale signature (native 2560x1440, or TAA/FSR/DLSS - not measurable)` — a native image and a temporal/AI upscaler look identical to the detector, so it does not guess
  - `Source res: unreliable (periodic pattern - dither or adaptive upscaler)` — several unrelated scales pass at once (dither lattice harmonics, nulls of an adaptive upscaler such as CAS/FSR1); any number would be a guess
- **Limitations** (from a real-frame corpus — see the testing section):
  - Works: linear scaling of the whole frame (borderless windows scaled by Windows/GPU, video content, console bilinear/bicubic output) — pixel-exact. Mild adaptive/temporal upscaling (e.g. Starfield CAS at 70%, FSR2-style in motion) — usually right thanks to the joint two-axis pick.
  - Unreliable: adaptive sharpen-upscalers (AMD CAS, FSR 1) at factors ≥ ~1.67×. Their per-pixel kernels smear the mirror signature, so other linearly-upsampled layers in the frame win — reduced-resolution post-effect buffers (depth of field, volumetrics — Starfield's ~60% buffer read as 1546x871) or spectral nulls of the output filter, which are aspect-consistent just like a real upscale.
  - Temporal/AI upscalers (DLSS, FSR 2+/3, TSR, PSSR) rebuild the spectrum and leave no mirror signature — on the corpus Starfield with FSR3 or DLSS Quality reads as native, standing still and in motion alike. Letterbox/pillarbox black bars distort the spectrum. Periodic dither patterns (id Tech) can produce spurious candidates in motion.
  - The value shown is "the strongest linear-upscale signature in the frame", which is not always the main render's resolution.

### Debug options (frame dump):
- **Settings** (filter, under the "Debug options" checkbox): "Frame dump folder", "Dump label (case name)", "Frames per dump" (default 16), "Start delay after button" (default 5 s — time to Alt+Tab back into the game) and a "Dump frames now" button
- **Status**: a "Dump status" label under the button counts down the delay, shows the frame being written and reports completion with the output folder
- **Hotkey**: "FPS Analyzer: dump frames (debug)" in Settings → Hotkeys (top, global section) starts a dump immediately, without leaving the game
- **FPS trace** ("Record FPS trace (per-frame CSV)"): while enabled, writes `fpstrace_<label>_<timestamp>.csv` into the dump folder — one row per analysed frame (analysis timestamp, diff %, unique/tearing decision, frametime, EMA, per-frame FPS) and one per published tick (window, FPS, frametime). Since v3 each frame row also carries the source frame timestamp, and a K row per OBS video tick records how many source frames reached the filter on that tick (0 = OBS repeated the previous frame); the header records the OBS FPS and the capture device's frame interval. These are what tells a repeat made by the capture card apart from one made by OBS. Since v4 each frame row also counts the compared bytes by the size of their difference (|d| = 0, 1, 2, 3–4, 5–8, 9–16, 17–32, >32) and the header records the device's Video Format, which is what the reliability check reads and replays. A few KB per second. Recording starts after "Start delay" so you can Alt+Tab into the game first, and the status line below the checkbox counts down and then shows the frame count. The hotkey "FPS Analyzer: start/stop FPS trace (debug)" skips the countdown and also stops the recording, so a session can be started and ended from inside the game. These traces are the fixtures the offline test tools replay to pin the FPS algorithm's behaviour across versions.
- **What it writes**: the next N full-frame luma planes fed to the resolution detector (one every 0.5 s) as `frame_NNNN.pgm` (binary PGM, 8-bit) into `<folder>/<label>_<timestamp>/`, plus `frames.csv` with time, frame size, video format and the detection result at each frame
- **Purpose**: build a test corpus of real captured frames for the offline detector harness — the dumps are byte-for-byte what the detector sees in OBS (unlike screenshots, which miss capture-card processing; never use JPG — its 8x8 block compression creates fake cutoffs)

### Testing the detector offline (`resdet-cli`, `resdet-selftest`):
Both tools build with the plugin (CMake option `FPS_ANALYZER_BUILD_TOOLS`, on by default) and have no OBS dependency; they land next to the DLL in `build/plugins/fps-analyzer/Release/`.
- `resdet-selftest` — synthetic regression suite (clean/bilinear upscales, native noise, film grain, HUD overlays, soft content).
- `resdet-cli --manifest cases.txt [--verbose]` — replays frame dumps through the detector and compares with the expected source resolution. Manifest line: `dir ; WxH|native ; tolerance_px ; note` (see `plugins/fps-analyzer/tools/cases.example.txt`). On failure it prints the per-frame sign/knee picks, the top candidates per axis and writes the spectrum with detected (green) vs expected (yellow) markers. Tuning flags (`--alpha`, `--sign-thr`, `--knee-thr`, `--cand-min`, `--joint-min`, `--flank-min`, `--warmup`, `--min-votes`) sweep detector parameters over the whole corpus; `--dump-votes` writes the full per-position vote/knee profiles as CSV.
- Workflow: dump frames in OBS (Debug options) for each known-truth case → add lines to your manifest → run the CLI → tune → re-run. The dumps are what the detector sees byte-for-byte, so offline results match the live plugin.

### Source Refresh Rate Check:
- **Independent feature**: Works with any analysis method, capture cards and cameras only (Video Capture Device and other async sources)
- **Description**: Measures the real refresh rate of the incoming signal from the timestamps the capture card driver puts on every frame, and compares it with the OBS FPS (Settings → Video)
- **Why it matters**: A capture card is locked to the HDMI signal. **"Match output FPS"** in Video Capture Device only picks the card's advertised format; it does not change the rate the card delivers. A PS5 sends **59.94 Hz** (Elgato Studio shows it as `2160p59`), so with OBS at 60 FPS the two clocks beat and OBS repeats a frame every `1 / |f_source − f_OBS|` seconds (~17 s). The FPS graph then shows a dip at that interval even though the game runs at a locked 60
- **Computed only when shown**: the timestamp check and the cadence check below run only while an active overlay has "Show Source refresh rate" ticked. Untick it in every overlay and the filter stops both (the log says `Source refresh rate check off`); tick it again and the check starts over. Without such an overlay there are no `Capture beat` lines in the log either
- **Overlay**: "Show Source refresh rate" (default: on) adds a line under the FPS text:
  - `Source: 59.94 Hz | OBS: 59.94 FPS - OK`
  - `Source: 59.94 Hz | OBS: 60 FPS - duplicate every ~17 s` + the OBS FPS to set
  - `measuring...` for the first ~5 s, `variable` for variable-rate sources, or `n/a` for Game/Display/Window Capture (no frame timestamps)
- **Overlay**: "Auto-hide Source refresh rate when OK" (default: off) hides the line 5 s after the verdict is OK; it comes back on a mismatch, a variable rate or while measuring again (e.g. after changing the OBS FPS)
- **Cadence check (frame repeats)**: some cards do not timestamp the signal but their own configured rate. The **Elgato 4K X** set to 60 FPS with a 59.94 Hz PS5 stamps every frame exactly 16.667 ms apart and converts the rate itself, repeating one frame every ~16.7 s, so the timestamp check reads `60 Hz - OK` while the graph still dips. The filter therefore also watches the frame cadence: a repeat after a steady stream is an event, either a tick on which OBS got no new frame from the source or a new frame with identical content from the card. Events recurring at a constant period P, where `OBS FPS − 1/P` is a standard rate, are a beat. The FPS readings are not changed, only the warning
  - `Signal ~59.94 Hz | Device: 60 | OBS: 60 FPS - capture device repeats a frame every ~17 s` + `Set the capture device FPS and OBS FPS to 59.94`
  - it takes 4 repeats (about 50 to 70 s at 59.94 vs 60) and clears itself 3 periods after the repeats stop
  - until then the line says `checking signal... 12/40 s` instead of OK: timestamps that agree with OBS only prove the device is set to the OBS rate. OK (`Signal: 60 Hz | OBS: 60 FPS - OK`) comes once the cadence has been watched for 2.4 repeat periods (40 s at 60 FPS, 80 s at 30) and no repeat was seen in the last two periods (~34 s at 60 FPS): a 59.94-vs-60 beat would have repeated by then even if one of its repeats was missed, so the mismatch is ruled out rather than just not seen yet. A mismatch in the timestamps (device at 59.94, OBS at 60) is still reported within ~5 s
  - when the game itself drops frames the check takes longer and says so (`Game frame pacing is uneven, the check takes longer`); the game's own stutter is what the FPS graph shows, the check only rules out the capture path
  - changing the OBS FPS or the device FPS, or losing the signal for 2 s, starts the check over
  - repeats made by OBS are counted separately from identical frames, since only the latter can also be a dropped game frame; a game that drops the odd frame does not raise it, nor does a game capped at 30
  - two clocks of the same nominal rate drifting apart (a repeat every few minutes) is reported as clock drift, which no setting can fix
- **Rule of thumb**: consoles → set **both** the capture device FPS and OBS to **59.94** (119.88 in 120 Hz modes), PC → **60**. Fixing only OBS is not enough when the card itself converts the rate
- **Limitation**: OBS only passes the frames it picks for its own clock, so a source running at 2× the OBS FPS or more is measured at its effective rate (119.88 Hz at OBS 60 reads as 59.94), which is also the rate that beats against OBS
- **Log**: each verdict change writes one line, e.g. `[FPS Analyzer] Source 59.944 Hz vs OBS 60.000 FPS: mismatch, duplicate/skip every 18.0 s`
- **Offline test**: `srate-selftest` (built with the tools) simulates the OBS frame pick for 59.94/60/119.88/29.97 sources, signal loss and variable rate; `fps-selftest` (scenario `s23_capture_beat`) simulates card and OBS repeats, clock drift, random game drops and a 30 fps game; `tests/cases.txt` replays two recordings from a 4K X with a PS5. `ctest` runs all of them
- **Offline test of the graph layout**: `fps-selftest` scenario `s26_plot_layout` checks `plot-layout.cpp` (the time axis without OBS): a fixed number of pixels per millisecond at 30 and 60 FPS, a 50 ms frame exactly 3x as wide as a 16.7 ms one, a 5 s window holding only the last 5 s with the right edge at now, a 20 s window showing twice the frames, the normal and inverted axis, and the sample ring wrapping in order. The mutation sweep also mutates `plot-layout.cpp`
- **Offline test of the frame pacing graph**: `fps-selftest` scenario `s25_frame_pacing` (even and juddering 30 FPS, 60 FPS, frames lost by the card, Game Capture with clock jitter, the reset); `tests/cases.txt` checks the Mk2 recordings (`pacing@3-20=33.37+-0.05` for a game at 30, 16.68 for the menu) and one in which the Mk2 dropped 98 frames (`pacinggaps>0`, every other step still 33.37)
- **Offline test of the reliability check**: `fps-selftest` scenario `s24_altered_frames` simulates a dithering card (game at 30 and 60, still picture), a good card with fades and noise below the sensitivity threshold, and a card that stops dithering; `tests/cases.txt` replays seven v4 recordings with a PS5: the 4K X in NV12/I420 with a game at 30, the home menu and a paused game (`blocked=altered`), and the 4K Mk2 with a game at 30, the home menu and the pause menu (`notblocked` and the right FPS)

### Performance tests (`fps-bench`, `run-perf.ps1`, `tools/obs-cost.py`):
- **`fps-bench`** (built with the tools, no OBS) times what the plugin does per frame (`fps_core_feed` for 1080p / 1440p / 4K, NV12 / YUY2 / BGRA, Last line / Full frame, with and without the reliability check's difference histogram) and per overlay render (the time-axis graphs), plus the beat detector, the publish and the source-rate estimator. Each case is the fastest of several rounds, in microseconds per call
- **Budget check (`ctest`, CI)**: `fps-bench --check` fails when a case exceeds a generous budget (~4x a desktop CPU, x3 more on a CI runner) — it catches only gross slowdowns
- **Baseline comparison (`run-perf.ps1`)**: runs the full table and compares it with `plugins/fps-analyzer/tests/perf-baseline.json` from the same machine; more than 20% slower is reported as a regression. `-UpdateBaseline` saves a new baseline (deliberately, like the goldens). It refuses to measure while OBS is running: OBS analysing frames itself made two runs differ by 2x
- **In OBS (`tools/obs-cost.py "<capture source>"`)**: switches the filter and the overlay graphs on and off through obs-websocket and reports OBS's CPU usage and frame render time for each, then restores everything. Measured on 2026-11-18 (4K YUY2 Full frame at 59.94 FPS): the filter +10.8 ms of render time per frame, the three graphs +1.8 ms
- **First results** (desktop CPU): Last line costs ~0.01 ms per frame at 4K; Full frame 3.0 ms at 4K NV12, but **10.6 ms with the reliability check's histogram** — the next optimisation (see TODO)

### Regression tests for the FPS analysis (`fps-selftest`):
The FPS numbers the overlay shows come from unique-frame detection, frametime history, EMA smoothing and a per-tick averaging window. `fps-selftest` freezes that behaviour so a later change cannot alter the readings unnoticed.

- It drives synthetic frame and tick streams through the analysis with an injected clock, so runs are bit-for-bit reproducible. Scenarios cover steady 24/30/45/60/120 fps, duplicated frames, a 100 ms stutter, a rate change, the stale-data reset, the sensitivity threshold, both analysis methods, tearing sequences, luma conversion per pixel format, the CSV writer and the graph ring buffer.
- Each scenario is checked twice: hand-written assertions derived from the algorithm, plus a byte-exact comparison against a golden file in `plugins/fps-analyzer/tests/golden/`.
- The goldens are produced by `plugins/fps-analyzer/tools/fps-ref-v050.cpp`, a verbatim copy of the v0.5.0 analysis code kept as a test oracle. That makes "the refactor changed nothing" a mechanical result rather than an assumption.

- Beyond the synthetic scenarios, `fps-cli` replays real material through the same core. `plugins/fps-analyzer/tests/cases.txt` lists the cases: small reference clips of known cadence generated by `tests/gen-fixtures.ps1` with ffmpeg, and traces recorded from live sessions. A clip's expected FPS follows from its cadence alone, so it is an independent check rather than a snapshot of what the code happens to produce. A listed file that is not present is skipped, so recordings can stay outside the repository.

- `tests/gen-fixtures.ps1 -Preview` renders enlarged MP4s of the clips so you can look at them; `-Local` renders lossless 1080p versions into `fixtures/local` for an end-to-end check inside OBS. A Media Source is an async source, the same code path a capture card takes, so playing one of those clips through the filter exercises the real plugin on material whose cadence is known in advance. Point the filter's CSV output at a file, play the clip, then run `tests/check-fps-csv.ps1 -Csv fps.csv -ExpectFps 60 -Skip 2 -IgnoreZeros`, which reports min, max, median and mode and fails if any row is out of range. Both folders are ignored by git.

- `tests/mutation-sweep.ps1` measures how much the suite actually catches: it breaks the core one thing at a time (thresholds, clamps, comparison operators, the order of two statements) and reports whether the tests notice, separately for the goldens and assertions alone, for the comparison against the v0.5.0 oracle, and for the reference clips. A mutation that survives is a coverage gap. This is how it was found that the replay path had no test at all and that the overlay graph arrays were compared nowhere. Currently every catchable mutation is caught, each of them without the oracle, which is what makes removing the oracle safe later. One mutation is marked equivalent: the tick window's floor of 10 can never fire, because the window starts at the sample count and the feedback only ever replaces it with a value above 10.

Run everything before pushing:

```powershell
.\run-tests.ps1
```

If you change the algorithm on purpose, regenerate the goldens with `.\run-tests.ps1 -UpdateGoldens` and commit the golden diff together with the change, so the effect on the readings is visible in review.

### Frame pacing graph:
- **Overlay**: "Show frame pacing graph" (default: off) and a scale (Auto / 33.33 / 50 / 66.67 / 100 ms; Auto picks 50 / 66.67 / 100 and clips longer frames at the edge). The graphs are drawn in the order **frame pacing, frametime, framerate**
- **Graph layout (all three graphs, each on its own)**, modelled on Digital Foundry's videos — a small frame-time box over a wide framerate strip:
  - **Time axis**: every graph shows the last N seconds and the right edge is now. The scale is fixed in time, so a 50 ms frame is always three times as wide as a 16.7 ms one, and switching between 30 and 60 FPS does not stretch the graph
  - **Time window**: 3 / 5 / 10 / 20 / 30 / 60 s per graph; default **10 s** for frame pacing and frametime, **20 s** for framerate (the frame graphs at twice the detail)
  - **Size**: Big (1800x360 plot — the labels now fit a 1920 canvas), Compact (300x80) or **Custom** (width x height in px, e.g. 350x120 for the frame graphs and 1800x200 for the framerate)
  - **Long frames down (Digital Foundry style)**: an inverted axis for frame pacing (default on) and frametime (default off) — short frames at the top, so a long frame goes down like a framerate drop
  - **A pause does not blank the graphs**: when no new frame comes for unusually long (more than twice the last frame and at least 50 ms — a menu, a loading screen, a frozen game), the right end shows the frame on screen now: on the frame graphs a step at how long it has been shown so far (it only grows until the next frame replaces it), on the framerate graph the value falls to 0 while the picture stands still. The line drawn between samples has a constant thickness, so the first frame after a long pause no longer fills a solid block
  - **The framerate graph is computed in time**: at every moment it shows how many game frames were on screen in the second before it, each counted by the share of its time inside that second, the frame still on screen included. A steady 30 FPS game at 59.94 Hz is a flat 29.97 (no 29/30 flicker), a pause falls to ~1 within a second and on towards 0 instead of a slope drawn across it, and judder averages out (it shows on the frame pacing graph). The FPS counter in the text is unchanged
  - The frame pacing labels follow the game: every 33.3 ms when the typical frame is that long (30 / 20 FPS games, like DF's 33.0 / 66.3 / 99.6), every 16.7 ms otherwise; labels that would overlap are skipped
- **What it shows**: how long **each game frame** stayed on screen, one step per frame, unsmoothed, like Digital Foundry's frame-time graph. On a capture card the value comes from the card's own timestamps, so it is a whole number of refresh periods (16.68 / 33.37 / 50.05 ms at 59.94 Hz). A flat line is even pacing; a saw-tooth between 16.7 and 50 ms is judder, even though the FPS counter still reads 30
- **How it differs from the frametime graph**: the frametime graph shows the average frame time of the second before each moment (1000 / the framerate graph's value), which smooths exactly this alternation away; the frame pacing graph shows every frame on its own. The frametime graph no longer uses an EMA: one long frame (a pause) kicked the EMA up for many frames after it, while the average is back a second after the pause. The "Frametime" text is unchanged
- **Grey steps**: the capture device lost a frame there (its timestamps jump by two periods). The value is kept, but it is not the game's pacing; a lost frame marks the step it falls in and the next one, whose start is the late stand-in. With a healthy card there are none
- **Game Capture / Display Capture** (no card timestamps): the time is taken from OBS's frames and rounded to whole OBS frames
- **Requirements**: OBS FPS equal to the signal rate (59.94 for a PS5; 119.88 for a 120 Hz mode — at OBS 60 only every other frame reaches the filter); no VRR; cards that stamp their configured rate instead of the signal (Elgato 4K X) are blocked anyway (see Supported capture devices)

### Reset graph history:
- **Button**: "Reset graph history" at the top of the filter's properties clears the frame pacing, frametime and framerate graphs and the averages, e.g. before a new test run
- **Hotkey**: "FPS Analyzer: reset graph history" in Settings → Hotkeys (top, global section) does the same from inside the game. It resets every FPS Analyzer filter; the capture-path checks (signal check, reliability check) are not reset, they are not graphs

### Tearing Detection:
- **Independent feature**: Works with any analysis method
- **Description**: Detects screen tearing by analyzing 3 lines (top, middle, bottom)
- **Settings**: 
  - "Enable tearing detection" checkbox (default: enabled)
  - "Tearing sensitivity threshold" slider (0.1-10.0%, default: 1.0%)
- **Algorithm**: Uses history of 5 frames to reduce false positives
- **Output**: Adds warning to TXT file when tearing is detected

### Output Format:
- **TXT**: `FPS: 60 | Frame Time: 16.67ms | Last Frame Time: 16.50ms`
- **CSV**: `timestamp,fps,frametime_ms` (or with additional tearing data)

## Building from source

### Requirements
- CMake 3.16+
- Visual Studio 2022 (or Build Tools)
- OBS Studio SDK (headers + import library)

### Local build

```bash
# 1. Build or obtain OBS SDK (headers + obs.lib)
# 2. Configure with CMAKE_PREFIX_PATH pointing to the SDK
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="path/to/obs-sdk"
cmake --build build --config Release
```

The output `fps-analyzer.dll` will be in `build/plugins/fps-analyzer/Release/`.

### CI/CD

GitHub Actions builds the plugin and runs `ctest` (the FPS and detector self-tests) on every push to `main` and on pull requests, so a pull request that changes the FPS readings without updating the goldens turns red. To create a release:

1. Update the version in the root `CMakeLists.txt`
2. Commit and tag: `git tag v0.3.0`
3. Push with tag: `git push origin main --tags`
4. A draft GitHub Release will be created with the DLL zip

## Troubleshooting:

### How to check if it works correctly?
- Run game on your PC and calibrate plugin.
- Run FPS Overlay like Riva RTSS or Steam overlay.
- Check if FPS matches.

### Plugin not detecting changes:
- Check if source has correct format (NV12/YUY2)
- Decrease sensitivity threshold
- Try "Full frame diff" method

### Unstable FPS readings:
- **Dips at a regular interval (e.g. every ~17 s) with a capture card**: the source refresh rate does not match the OBS FPS. Check the "Source refresh rate" line on the overlay and set OBS → Settings → Video → FPS to the value it suggests (59.94 for consoles)
- Use Last line diff method for stable sources
- Check if V-Sync is enabled
- Increase update interval

