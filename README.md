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
2. **Select analysis method** (recommended: CRC32 for most cases)
3. **Set output file path**
4. **Configure update interval** (default: 1 second)
5. add **Text (GDI+)**: Use TXT file as text source

## Analysis Methods:

The plugin offers three different image analysis methods:

### 1. CRC32 (last line only) - **Default**
- **Speed**: Fastest
- **Accuracy**: High for stable sources
- **Use case**: Ideal for most games with V-Sync
- **Description**: Calculates CRC32 from only the last line of the image (luminance)

### 2. Full frame analysis (all lines)
- **Speed**: Medium
- **Accuracy**: Very high
- **Use case**: When you need maximum precision
- **Description**: Analyzes every pixel in the frame and calculates the percentage of differences
- **Settings**: "Sensitivity threshold" slider (0.0-5.0%)

### 3. Multi-line (tearing detection)
- **Speed**: Medium
- **Accuracy**: High with tearing detection
- **Use case**: Games without V-Sync where tearing occurs
- **Description**: Analyzes 3 lines (top, middle, bottom) and detects tearing
- **Settings**: "Sensitivity threshold" slider (0.0-5.0%)

### Output Format:
- **TXT**: `FPS: 60 | Frame Time: 16.67ms | Last Frame Time: 16.50ms`
- **CSV**: `timestamp,fps,frametime_ms` (or with additional tearing data)

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
- Use CRC32 method for stable sources
- Check if V-Sync is enabled
- Increase update interval

