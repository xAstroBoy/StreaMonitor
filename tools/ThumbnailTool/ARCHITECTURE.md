# ThumbnailTool — Architecture & Codebase Reference

> Quick reference for AI agents and developers — avoids needing to re-read the entire codebase.

---

## Project Structure

```
tools/ThumbnailTool/
├── src/
│   ├── app.h          — App class, enums, structs, all member declarations
│   ├── app.cpp         — Main application logic (~1700 lines)
│   └── main.cpp        — Entry point (GLFW + ImGui bootstrap)
├── CMakeLists.txt      — Build configuration for ThumbnailTool
└── ARCHITECTURE.md     — THIS FILE

src/utils/
├── thumbnail_generator.h    — Public API for all video operations
└── thumbnail_generator.cpp  — Implementation (~1820 lines)

.github/workflows/build.yml  — CI/CD (Windows + Linux)
```

---

## Key Types (app.h)

### Enums
| Enum | Values | Purpose |
|------|--------|---------|
| `ContainerType` | RealMKV, FakeMKV, Other | Actual container format (don't trust extension!) |
| `StatusTab` | All, InProgress, Pending, Done, Failed | Tab filter for the file table |

### ThreadProgress (per-thread live status)
```
active (atomic bool), filePath, action, subAction, fileSize, startTime, mutex
```

### VideoEntry (per-file scan result)
```
videoPath, thumbPath, relDisplay, container, fileSize,
hasThumb, hasCoverEmbed, coverProbed, tsFixed, remuxed, processed, failed, errorMsg
```

### App class — Key Atomics (live-updated, thread-safe)
```
thumbnailWidth{3840}, thumbnailColumns{4}, thumbnailRows{4}
embedInVideo{true}, threadCount{4}
```

---

## app.cpp — Function Map

### Static Helpers (top of file)
| Function | Purpose |
|----------|---------|
| `isVideoFile(path)` | Check extension against kVideoExts |
| `thumbPathForVideo(path)` | Replace extension with .jpg |
| `DrawBadge(text, color)` | Render colored pill badge in table |
| `formatSize(bytes)` | Human-readable size string |

### Core Lifecycle
| Function | Purpose |
|----------|---------|
| `App::App()` | Load config, set default root dir |
| `App::~App()` | Cancel work, join all threads |
| `addLog(line)` | Thread-safe log append (capped at 2000) |
| `loadConfig()` / `saveConfig()` | JSON config persistence |

### Scan System
| Function | Purpose |
|----------|---------|
| `startScan()` | Validate root dir, reset state, launch scanWorker on background thread |
| `scanWorker()` | recursive_directory_iterator → populate videos_ vector |

**Scan detects:**
- Container type via `isRealMatroska()` (EBML magic bytes check)
- Existing thumbnails (.jpg sidecar file)
- Embedded cover art via `hasCoverArt()` for Real MKV files
- File size from directory entry

### Generation System
| Function | Purpose |
|----------|---------|
| `startGeneration()` | Count work, skip already-done files, spawn N worker threads |
| `workerFunc(threadIdx)` | THE MAIN PIPELINE — processes one file at a time from shared queue |

**"Already done" skip logic:**
- Files with `processed = true` (done this run) → skip
- Real MKV + hasCoverEmbed (done from previous run) → mark processed + skip

### Worker Pipeline (5 Steps per Video)

```
STEP 1: Ensure Real MKV container
  - Other/FakeMKV → ensureRealMKV() → stream-copy remux
  - Updates videoPath if extension changed (.mp4 → .mkv)

STEP 2: Fix timestamps (if not just remuxed)
  - hasTimestampIssues() → fixTimestamps() → remux in-place

STEP 3: Generate thumbnail
  - Deferred coverProbed check (hasCoverArt probe)
  - If no thumbnail → generateContactSheet()

STEP 4: Embed cover art in MKV
  - If not already embedded → embedThumbnailInMKV()
  - If already embedded → fixCoverAttachmentMetadata() (DLNA fix)
  - Delete external .jpg after successful embed

STEP 5: VR metadata
  - isVRFromPath() checks for [SCVR]/[DCVR] in folder name
  - injectVRSpatialMetadata() → fisheye 180° SBS
```

### Thread Management
- **Lock-free dispatch**: `nextIdx_.fetch_add(1)` — no contention between workers
- **Live increase**: Settings slider spawns new workers immediately via `workerMutex_`
- **Live decrease**: Workers check `shouldExitThread()` between files (threadIdx >= threadCount)
- **Graceful exit**: Workers finish current file before exiting

### Render System

#### render() — Main Window
1. **Header bar**: Title + subtitle
2. **Toolbar**: Root dir input, Scan/Generate/STOP buttons, progress bar, ETA, Hide Done, Settings
3. **Processing stats** (visible during generation): Thread count, grid config, embed status, live counters
4. **Content area**: Splitter between table panel and log panel
5. **Status bar**: Video counts, Show/Hide Log button

#### renderTable() — File Table
1. **Tab bar**: All / In Progress / Pending / Done / Failed (with counts)
2. **In Progress tab**: 6-column table (Thread#, Action, Detail, Size, Elapsed, File)
3. **Main file table**: 5 sortable columns (File, Size, Container, Thumbnail, Status)
   - Uses ImGuiListClipper for virtual scrolling (handles 10000+ rows)
   - RowSnap pattern: Copy data under lock → render lock-free

**Done detection for table display:**
```
isDone = !failed && (processed || (RealMKV && (hasThumb || hasCoverEmbed)))
```

#### renderLogPanel() — Log Panel
- **Toolbar**: Level filter buttons (All/Info/Warn/Err), text search, Copy, Clear, Auto-scroll
- **Content**: Filtered, color-coded, virtualized log display
- **Right-click**: Copy individual log line to clipboard

#### renderSettingsPopup() — Settings Window
- Thumbnail grid: Width (1280-7680), Columns (2-8), Rows (2-12)
- Processing: Thread slider (1-32, live spawn/reduce), Embed checkbox
- Shell integration: Windows Explorer context menu install/uninstall
- Config: Save/Reload buttons

---

## thumbnail_generator.h — Public API

```cpp
namespace sm {
    // Core operations
    bool generateContactSheet(videoPath, outputPath, config, logCb) → bool
    bool hasCoverArt(videoPath) → bool          // checks ATTACHED_PIC + MJPEG/PNG
    bool isRealMatroska(path) → bool             // EBML magic bytes check
    bool hasTimestampIssues(path, logCb) → bool  // probes first 5000 packets
    bool fixTimestamps(path, logCb) → bool       // remux in-place

    // MKV operations
    std::string ensureRealMKV(path, logCb)       // remux if needed, returns MKV path
    bool embedThumbnailInMKV(video, thumb, logCb) // uses mkvpropedit
    bool fixCoverAttachmentMetadata(path, logCb)  // DLNA compatibility fix

    // VR operations
    bool isVRFromPath(path) → bool               // [SCVR]/[DCVR] in folder
    bool injectVRSpatialMetadata(path, logCb)    // fisheye 180° SBS

    struct ThumbnailConfig {
        int width = 3840;
        int columns = 4, rows = 4;
        int padding = 4, headerHeight = 80;
        int quality = 1;
        float skipStartSec = 5.0f;
        bool adaptiveWidth = true;
    };
}
```

---

## Build System

### Windows (vcpkg)
```
python build.py  # from project root
```
- CMake + MSVC, vcpkg x64-windows-static-md
- Deploys to: `C:\AstroTools\StripHelperCpp\`

### Linux (CI)
- ubuntu-24.04, Ninja, system libs
- FFmpeg from system packages (~6.1)
- Flags: `-DSM_USE_SYSTEM_LIBS=ON -DSM_USE_SYSTEM_FFMPEG=ON`

### CI/CD (.github/workflows/build.yml)
- **Windows**: windows-latest, vcpkg, ~46 min
- **Linux**: ubuntu-24.04, system libs, ~1-2 min
- Dependencies: build-essential cmake ninja-build pkg-config + libcurl/openssl/json/spdlog/ffmpeg/glfw/mesa/x264/x265/xrandr/xinerama/xcursor/xi/xkbcommon/wayland/nghttp2

---

## Key Patterns & Gotchas

1. **EBML magic bytes**: Don't trust `.mkv` extension — some files are MP4 inside MKV
2. **hasCoverArt**: Checks `AV_DISPOSITION_ATTACHED_PIC` + MJPEG/PNG codec → reliable "processed" tag
3. **Thread safety**: All UI-visible state uses atomics or mutex; `videosMutex_` protects `videos_` vector
4. **RowSnap pattern**: renderTable copies data under lock, then renders lock-free (avoids holding lock during ImGui calls)
5. **Lock-free dispatch**: Workers use `nextIdx_.fetch_add(1)` — zero contention, perfect scaling
6. **ETA**: Rolling window (5s snapshots), 70/30 smooth to prevent jumps
7. **Cancel**: Workers check `shouldCancel()` between pipeline steps; last worker cleans up orphaned files
8. **FFmpeg API versions**: hls_recorder.cpp has 3-tier display matrix API (7+, 6.1+, 5.x-6.0)
9. **friend function**: `populateOutputDurations` in hls_recorder — declared friend in .h, must NOT be `static` in .cpp (GCC error)
