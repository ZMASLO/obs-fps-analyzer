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

