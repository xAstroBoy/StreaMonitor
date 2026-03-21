#pragma once

// ─────────────────────────────────────────────────────────────────
// Video Contact Sheet (Thumbnail) Generator
// Uses native FFmpeg API (libavformat/libavcodec/libswscale)
// Generates a grid of frames extracted at even intervals from a video.
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <functional>

namespace sm
{
    struct ThumbnailConfig
    {
        int width = 1280;       // Total image width in pixels
        int columns = 4;        // Number of thumbnail columns
        int rows = 4;           // Number of thumbnail rows
        int spacing = 2;        // Padding between thumbnails in pixels
        int headerHeight = 40;  // Header bar height
        int quality = 4;        // JPEG quality (1=best, 31=worst, MJPEG qscale)
        float skipStartSec = 5; // Skip first N seconds (avoid black/intro frames)

        int totalFrames() const { return columns * rows; }
    };

    // Generate a video contact sheet (grid of thumbnails).
    //  videoPath  — input video file
    //  outputPath — output JPEG file (e.g., "video.thumb.jpg")
    //  cfg        — layout/quality settings
    //  logCb      — optional log callback (info messages)
    // Returns true on success.
    bool generateContactSheet(
        const std::string &videoPath,
        const std::string &outputPath,
        const ThumbnailConfig &cfg = {},
        std::function<void(const std::string &)> logCb = nullptr);

} // namespace sm
