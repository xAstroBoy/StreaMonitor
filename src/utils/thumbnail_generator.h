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
        int width = 3840;          // Total image width in pixels (high-res default)
        int columns = 4;           // Number of thumbnail columns
        int rows = 4;              // Number of thumbnail rows
        int spacing = 4;           // Padding between thumbnails in pixels
        int headerHeight = 80;     // Header bar height
        int quality = 1;           // JPEG quality (1=best, 31=worst, MJPEG qscale)
        float skipStartSec = 5;    // Skip first N seconds (avoid black/intro frames)
        bool adaptiveWidth = true; // Auto-scale to at least source video width

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
        std::function<void(const std::string &)> logCb = nullptr,
        std::function<bool()> cancelCb = nullptr);

    // Check if a video file already has cover art (attached picture) embedded.
    bool hasCoverArt(const std::string &videoPath);

    // Check the first 4 bytes for EBML magic — true only for real Matroska containers.
    bool isRealMatroska(const std::string &path);

    // Quick check for timestamp discontinuities (DTS jumps / backward gaps).
    // Probes first ~5000 packets. Returns true if issues found.
    bool hasTimestampIssues(const std::string &videoPath,
                            std::function<void(const std::string &)> logCb = nullptr);

    // Fix timestamps by remuxing in-place (stream copy, normalises DTS/PTS).
    // Works on real Matroska files — remuxes to clean Matroska with monotonic timestamps.
    // progressCb receives bytes written so far for I/O speed tracking.
    bool fixTimestamps(const std::string &videoPath,
                       std::function<void(const std::string &)> logCb = nullptr,
                       std::function<bool()> cancelCb = nullptr,
                       std::function<void(int64_t)> progressCb = nullptr);

    // Check if a video path indicates VR content (folder contains [SCVR], [DCVR], etc.)
    bool isVRFromPath(const std::string &videoPath);

    // Inject VR 180° SBS spatial metadata into a Matroska file (native remux).
    // Sets Matroska track elements via FFmpeg side-data during stream-copy remux:
    //   StereoMode=1 (side-by-side, left eye first)
    //   ProjectionType=equirectangular (180° hemisphere via tile bounds)
    // Idempotent — safe to call multiple times on the same file.
    bool injectVRSpatialMetadata(
        const std::string &mkvPath,
        std::function<void(const std::string &)> logCb = nullptr,
        const std::string &mkvpropeditPath = "");

    // Embed a JPEG thumbnail as cover art + inject VR spatial metadata (native remux).
    // Handles all video formats: remuxes to MKV first, embeds cover via native
    // FFmpeg attached_pic stream, then injects VR180 SBS metadata for VR content
    // (auto-detected from folder path). All done in a single remux pass.
    // Returns true on success; on failure the .jpg file is kept alongside.
    bool embedThumbnailInMKV(
        const std::string &videoPath,
        const std::string &jpegPath,
        std::function<void(const std::string &)> logCb = nullptr,
        const std::string &mkvpropeditPath = "",
        bool forceRegenerate = false);

    // Fix existing cover art attachment to have proper description for DLNA compatibility.
    // Uses native remux to update attachment metadata (name + description).
    // Idempotent — safe to call on files that already have correct metadata.
    bool fixCoverAttachmentMetadata(
        const std::string &mkvPath,
        std::function<void(const std::string &)> logCb = nullptr,
        const std::string &mkvpropeditPath = "");

    // Ensure a video file is in a real Matroska (.mkv) container.
    // - .mkv with EBML header → returns same path (already good)
    // - .mkv without EBML → remuxes in-place, returns same path
    // - Non-.mkv → creates .mkv alongside original, returns new path
    // All remuxing is stream-copy (zero re-encoding).
    // Fixes timestamp discontinuities during remux.
    // progressCb receives bytes written so far for I/O speed tracking.
    // Returns empty string on failure.
    std::string ensureRealMKV(
        const std::string &videoPath,
        std::function<void(const std::string &)> logCb = nullptr,
        std::function<bool()> cancelCb = nullptr,
        std::function<void(int64_t)> progressCb = nullptr);

    // Check if a video file has been marked as processed by ThumbnailTool.
    // Reads MKV global tags looking for THUMBNAILED=done.
    bool hasProcessedTag(const std::string &videoPath);

    // Write a "processed" tag into MKV metadata (native remux).
    // Sets global tag THUMBNAILED=done. Idempotent.
    bool writeProcessedTag(
        const std::string &mkvPath,
        std::function<void(const std::string &)> logCb = nullptr,
        const std::string &mkvpropeditPath = "");

} // namespace sm
