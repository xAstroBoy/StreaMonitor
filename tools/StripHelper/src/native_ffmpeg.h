#pragma once
// ─────────────────────────────────────────────────────────────────
// StripHelper — Native FFmpeg operations
// Replaces subprocess-based FFmpeg/FFprobe calls with direct
// libavformat/libavcodec API for faster probing, remuxing, concat.
// Re-encode paths still use subprocess for encoder flexibility.
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include <cstdint>

namespace fs = std::filesystem;

namespace sh
{
    // ── Probe result ─────────────────────────────────────────────────
    struct NativeProbeResult
    {
        double duration = 0;
        int width = 0, height = 0;
        double fps = 0;
        std::string videoCodec, audioCodec;
        std::string pixFmt;
        int sampleRate = 0, channels = 0;
        bool hasVideo = false, hasAudio = false;
        int64_t fileSize = 0;
    };

    // Probe a media file — returns full stream info via native FFmpeg API.
    // ~10-50ms vs ~100-500ms for subprocess ffprobe.
    NativeProbeResult nativeProbe(const fs::path &filePath);

    // Get the best-effort duration in seconds.
    double nativeProbeDuration(const fs::path &filePath);

    // Quick validity check — can the file be opened and has streams?
    bool nativeProbeOk(const fs::path &filePath);

    // ── Native remux (stream copy) ──────────────────────────────────
    // Progress callback: (timeSeconds, bytesWritten, totalEstimate)
    using NativeProgressCb = std::function<void(double, int64_t, int64_t)>;

    // Stream-copy remux a single file (any format → MKV).
    // Handles timestamp normalization (avoid_negative_ts + reset_timestamps).
    bool nativeRemux(const fs::path &input, const fs::path &output,
                     const fs::path &workDir,
                     NativeProgressCb progress = nullptr);

    // Cancel callback type for stop checking
    using NativeCancelCb = std::function<bool()>;

    // ── Native concat (stream copy) ─────────────────────────────────
    // Concatenate multiple files into one MKV via the concat demuxer.
    // Equivalent to: ffmpeg -f concat -safe 0 -i list.txt -c copy out.mkv
    // cancelCb: if provided and returns true, abort the operation
    bool nativeConcat(const fs::path &concatListFile, const fs::path &output,
                      const fs::path &workDir,
                      NativeProgressCb progress = nullptr,
                      NativeCancelCb cancelCb = nullptr);

    // ── PTS analysis ─────────────────────────────────────────────────
    // Detect the maximum PTS jump/discontinuity in a file.
    double nativeDetectMaxPtsJump(const fs::path &filePath);

} // namespace sh
