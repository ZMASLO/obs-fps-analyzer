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

## Tested Hardware:
- **Elgato HD60 X** (Works OK)
- **Elgato 4K Mk2** (doesn't work properly due to laggy OBS preview)
- **Avermedia Livegamer 4K** (Works OK)

If you have different capture card please report if it works correctly.

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


### Upscale Source Resolution Detection:
- **Independent feature**: Works with any analysis method
- **Description**: Estimates the internal render resolution the image was upscaled from (e.g. a console game rendering at 1280x720 output as 1920x1080)
- **Settings**:
  - Filter: "Detect upscale source resolution (DCT)" checkbox (default: disabled)
  - Overlay: "Show Source resolution text" checkbox (default: enabled)
  - Filter: "Spectrum refresh rate" dropdown — "Analysis rate" (~2/s, default), "30 FPS" or "60 FPS". The fast modes compute the spectrum from a 640x360 center crop on a second background thread (~20% / ~40% of one core); detection itself always uses the full frame at the analysis rate. A crop keeps the spectral cutoff at the same normalized frequency, so the markers still line up.
  - Overlay: "Show Source resolution spectrum" checkbox (default: enabled) — a small panel with the 2D DCT log-magnitude spectrum of the frame (low frequencies top-left, bright = energy) and green tick markers with the detected source width (bottom edge) and height (right edge). On an upscaled image the energy forms a visible rectangle ending at the markers; a native image fills the whole panel.
- **Algorithm**: hybrid DCT spectral analysis, ~2 analyses per second on a background thread:
  - *Sign method* (ported from [resdet](https://github.com/0x09/resdet)): traditional resamplers mirror the spectrum with inverted signs around the source resolution index — pixel-exact on clean upscales (videos, menus). Candidates whose flanks are anti-correlated (comb-like peaks caused by spectral nulls of motion blur or reconstruction kernels) are rejected, and the final pick is made *jointly* for both axes: upscaling is almost always uniform, so a pair of candidates at the same scale on width and height outranks a lone stronger peak on one axis (this is what makes temporal upscalers like FSR2 readable in motion). Independent per-axis picks remain the fallback.
  - *Magnitude knee* (fallback): native-res overlays like HUD or film grain corrupt the sign symmetry, but the energy envelope still drops sharply at the source resolution; the detector finds the strongest step in the log-magnitude profile of each axis.
  - Results are accumulated over consecutive frames (EMA) and reported only when recent analyses agree (median consensus) — smooths dynamic resolution scaling and rejects sporadic false positives.
- **Output**: `Source res: ~1280x720 -> 1920x1080 (95%)` or `Source res: native 1920x1080`
- **Limitations** (from a real-frame corpus — see the testing section):
  - Works: linear scaling of the whole frame (borderless windows scaled by Windows/GPU, video content, console bilinear/bicubic output) — pixel-exact. Mild adaptive/temporal upscaling (e.g. Starfield CAS at 70%, FSR2-style in motion) — usually right thanks to the joint two-axis pick.
  - Unreliable: adaptive sharpen-upscalers (AMD CAS, FSR 1) at factors ≥ ~1.67×. Their per-pixel kernels smear the mirror signature, so other linearly-upsampled layers in the frame win — reduced-resolution post-effect buffers (depth of field, volumetrics — Starfield's ~60% buffer read as 1546x871) or spectral nulls of the output filter, which are aspect-consistent just like a real upscale.
  - Temporal/AI upscalers (DLSS, FSR 2+/3, TSR, PSSR) rebuild the spectrum and leave no mirror signature — on the corpus Starfield with FSR3 or DLSS Quality reads as native, standing still and in motion alike. Letterbox/pillarbox black bars distort the spectrum. Periodic dither patterns (id Tech) can produce spurious candidates in motion.
  - The value shown is "the strongest linear-upscale signature in the frame", which is not always the main render's resolution.

### Debug options (frame dump):
- **Settings** (filter, under the "Debug options" checkbox): "Frame dump folder", "Dump label (case name)", "Frames per dump" (default 16), "Start delay after button" (default 5 s — time to Alt+Tab back into the game) and a "Dump frames now" button
- **Status**: a "Dump status" label under the button counts down the delay, shows the frame being written and reports completion with the output folder
- **Hotkey**: "FPS Analyzer: dump frames (debug)" in Settings → Hotkeys (top, global section) starts a dump immediately, without leaving the game
- **What it writes**: the next N full-frame luma planes fed to the resolution detector (one every 0.5 s) as `frame_NNNN.pgm` (binary PGM, 8-bit) into `<folder>/<label>_<timestamp>/`, plus `frames.csv` with time, frame size, video format and the detection result at each frame
- **Purpose**: build a test corpus of real captured frames for the offline detector harness — the dumps are byte-for-byte what the detector sees in OBS (unlike screenshots, which miss capture-card processing; never use JPG — its 8x8 block compression creates fake cutoffs)

### Testing the detector offline (`resdet-cli`, `resdet-selftest`):
Both tools build with the plugin (CMake option `FPS_ANALYZER_BUILD_TOOLS`, on by default) and have no OBS dependency; they land next to the DLL in `build/plugins/fps-analyzer/Release/`.
- `resdet-selftest` — synthetic regression suite (clean/bilinear upscales, native noise, film grain, HUD overlays, soft content).
- `resdet-cli --manifest cases.txt [--verbose]` — replays frame dumps through the detector and compares with the expected source resolution. Manifest line: `dir ; WxH|native ; tolerance_px ; note` (see `plugins/fps-analyzer/tools/cases.example.txt`). On failure it prints the per-frame sign/knee picks, the top candidates per axis and writes the spectrum with detected (green) vs expected (yellow) markers. Tuning flags (`--alpha`, `--sign-thr`, `--knee-thr`, `--cand-min`, `--joint-min`, `--flank-min`, `--warmup`, `--min-votes`) sweep detector parameters over the whole corpus; `--dump-votes` writes the full per-position vote/knee profiles as CSV.
- Workflow: dump frames in OBS (Debug options) for each known-truth case → add lines to your manifest → run the CLI → tune → re-run. The dumps are what the detector sees byte-for-byte, so offline results match the live plugin.

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

GitHub Actions automatically builds the plugin on every push to `main` and on pull requests. To create a release:

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
- Use Last line diff method for stable sources
- Check if V-Sync is enabled
- Increase update interval

