# ZeroCut

Lightweight, fast, portable lossless video & audio cutter and merger written in pure **C** (GTK3 + libmpv + FFmpeg `libavformat`). Designed for **WMV, ASF, WMA, MP4, MKV, AVI** and other formats — zero re-encoding, zero quality loss.

![ZeroCut Screenshot](assets/screenshot.png)

## Download (Linux 64-bit Portable)

No installation or dependencies required — all required shared libraries are bundled inside the archive:

1. Go to the [**Releases page**](https://github.com/RubyKeeper/ZeroCut/releases/latest).
2. Download **`ZeroCut-v1.0.0-linux-x86_64.tar.gz`**.
3. Extract and run:
   ```bash
   tar -xzf ZeroCut-v1.0.0-linux-x86_64.tar.gz
   ./ZeroCut
   ```

## Features
- **Lossless Cutting & Merging:** Direct stream-copy via custom 64-bit `zerocut-cli` (`libavformat`) with accurate PTS/DTS recalculation.
- **Keyframe Navigation:** Frame-by-frame stepping (`←` / `→`) and exact keyframe jumping (`Shift + ←` / `Shift + →`).
- **Smart Segment Workflow:** Automatically creates and edits segments on the timeline with `I` (In) and `O` (Out), supports multi-segment concatenation, and instant jump to segment boundaries (`[` / `]`).
- **Portable Ready:** Built with `$ORIGIN` RPATH so all `.so` libraries can be bundled inside `lib/` for zero-install portability.

## Build from Source (Ubuntu / Linux Mint / Debian)

```bash
sudo apt install build-essential pkg-config libgtk-3-dev libmpv-dev libavformat-dev libavcodec-dev libavutil-dev
make
./ZeroCut
```

## Keyboard & Mouse Shortcuts
- `Space` or **Click video**: Play / Pause
- `←` / `→`: Step backward / forward by 1 frame
- `Shift + ←` / `Shift + →`: Jump to previous / next keyframe
- `I` / `O`: Set segment start / end (auto-creates or edits segment at cursor)
- `N`: Force new segment at cursor
- `[` / `]`: Jump to active segment start / end
- **Double-click segment**: Jump to segment start
- `↑` / `↓` or **Mouse Wheel**: Volume control (`M` to mute)
