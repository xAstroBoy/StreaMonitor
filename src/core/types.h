#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Core type definitions
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <cstdint>
#include <chrono>
#include <atomic>
#include <functional>
#include <optional>
#include <vector>
#include <map>

namespace sm
{

    // ── Status enum (mirrors Python Status) ─────────────────────────
    enum class Status : int
    {
        Unknown = 1,
        NotRunning = 2,
        Error = 3,
        ConnectionError = 4, // Network/DNS/timeout failure (NOT a rate limit!)
        Restricted = 1403,
        Online = 100,
        Public = 200,
        NotExist = 400,
        Private = 403,
        Offline = 404,
        LongOffline = 410,
        Deleted = 444,
        RateLimit = 429,
        Cloudflare = 503
    };

    const char *statusToString(Status s);
    bool statusIsRecordable(Status s);
    bool statusIsTemporaryError(Status s);

    // ── Gender enum ─────────────────────────────────────────────────
    enum class Gender : int
    {
        Unknown = 0,
        Female = 1,
        Male = 2,
        Couple = 3,
        TransWoman = 5,
        TransMan = 6,
        Trans = 7,
        FemalCouple = 9,
        MaleCouple = 10
    };

    const char *genderToString(Gender g);

    // ── Resolution preference ───────────────────────────────────────
    enum class ResolutionPref
    {
        Closest,
        Exact,
        ExactOrLeastHigher,
        ExactOrHighestLower
    };

    // ── Container format ────────────────────────────────────────────
    enum class ContainerFormat
    {
        MKV,
        MP4,
        TS
    };

    struct FormatInfo
    {
        const char *extension; // ".mkv"
        const char *ffFormat;  // "matroska"
        const char *segFormat; // "matroska"
        const char *movFlags;  // nullptr or "frag_keyframe+empty_moov"
    };

    const FormatInfo &getFormatInfo(ContainerFormat fmt);
    ContainerFormat parseContainerFormat(const std::string &s);

    // ── VR projection metadata ─────────────────────────────────────
    // These map to Spherical Video V2 (sv3d/proj) metadata used by
    // DLNA players like SCVR, DeoVR, Skybox, etc.
    // NOTE: Moved here before ModelConfig because ModelConfig uses VRConfig
    enum class VRProjection
    {
        None = 0,    // Not a VR recording
        Fisheye180,  // Fisheye 180° (most common for VR cams)
        Equirect360, // Equirectangular 360°
        Equirect180, // Equirectangular 180°
        Cubemap,     // Cubemap projection
        EAC          // Equi-Angular Cubemap (YouTube-style)
    };

    enum class VRStereoMode
    {
        Mono = 0,   // Single eye
        SideBySide, // Left-Right (most common for VR)
        TopBottom,  // Top-Bottom
        Custom      // Custom layout
    };

    struct VRConfig
    {
        VRProjection projection = VRProjection::None;
        VRStereoMode stereoMode = VRStereoMode::Mono;
        int fov = 180;                                 // Field of view in degrees
        bool embedMetadata = true;                     // Write metadata to container
        std::map<std::string, std::string> customTags; // Extra FFmpeg metadata key-value tags

        bool isVR() const { return projection != VRProjection::None; }
    };

    const char *vrProjectionToString(VRProjection p);
    const char *vrStereoModeToString(VRStereoMode m);
    VRProjection parseVRProjection(const std::string &s);
    VRStereoMode parseVRStereoMode(const std::string &s);

    // ── Model configuration (persisted in config.json) ──────────────
    struct ModelConfig
    {
        std::string site;
        std::string username;
        bool running = true;
        Status lastStatus = Status::Offline;
        bool recording = false;
        Gender gender = Gender::Unknown;
        std::string country;
        std::string crossRegisterGroup; // Group name for multi-site tracking
        VRConfig vrConfig;              // Per-model VR metadata config
    };

    // ── Cross-register entry (links models across sites) ────────────
    struct CrossRegisterGroup
    {
        std::string groupName;                                    // User-defined group name
        std::vector<std::pair<std::string, std::string>> members; // (site, username) pairs
        bool linkRecording = false;                               // When one records, prefer that site
        bool linkStatus = true;                                   // Share status info across members
    };

    // ── Recording statistics ────────────────────────────────────────
    struct RecordingStats
    {
        uint64_t bytesWritten = 0;
        uint32_t segmentsRecorded = 0;
        uint32_t stallsDetected = 0;
        uint32_t restartsPerformed = 0;
        double currentSpeed = 0.0;
        std::chrono::steady_clock::time_point recordingStarted;
        std::string currentFile;

        RecordingStats() = default;
        RecordingStats(const RecordingStats &) = default;
        RecordingStats &operator=(const RecordingStats &) = default;
        RecordingStats(RecordingStats &&) = default;
        RecordingStats &operator=(RecordingStats &&) = default;

        void reset()
        {
            bytesWritten = 0;
            segmentsRecorded = 0;
            stallsDetected = 0;
            restartsPerformed = 0;
            currentSpeed = 0.0;
            currentFile.clear();
        }
    };

    // ── HLS variant info (parsed from master playlist) ──────────────
    struct HLSVariant
    {
        std::string url;
        int width = 0;
        int height = 0;
        int bandwidth = 0;
        std::string codecs;
    };

    // ── Cancellation token ──────────────────────────────────────────
    class CancellationToken
    {
    public:
        void cancel() { cancelled_.store(true, std::memory_order_release); }
        bool isCancelled() const { return cancelled_.load(std::memory_order_acquire); }
        void reset() { cancelled_.store(false, std::memory_order_release); }

    private:
        std::atomic<bool> cancelled_{false};
    };

    // ── Video encoder selection ────────────────────────────────────
    enum class EncoderType
    {
        Copy,       // Stream copy (no re-encoding) — fastest, preserves original
        X265,       // libx265 (HEVC software) — preferred default
        X264,       // libx264 (H.264 software) — widest compatibility
        NVENC_HEVC, // NVIDIA NVENC HEVC (hardware) — needs CUDA GPU
        NVENC_H264  // NVIDIA NVENC H.264 (hardware) — needs CUDA GPU
    };

    // ── Encoding configuration ──────────────────────────────────────
    struct EncodingConfig
    {
        EncoderType encoder = EncoderType::X265; // Default: x265 software encoding
        bool enableCuda = false;                 // CUDA/NVENC OFF by default
        int crf = 23;                            // Constant Rate Factor (0-51, lower=better, 23=default)
        std::string preset = "medium";           // Encoding speed/quality trade-off
        int audioBitrate = 128;                  // Audio bitrate in kbps (AAC re-encode)
        bool copyAudio = true;                   // true = copy audio stream, false = re-encode audio
        int maxWidth = 0;                        // Max output width (0 = keep original)
        int maxHeight = 0;                       // Max output height (0 = keep original)
        int threads = 0;                         // Encoder threads (0 = auto)

        // Returns the FFmpeg encoder name for the selected encoder
        const char *ffmpegEncoderName() const
        {
            switch (encoder)
            {
            case EncoderType::Copy:
                return nullptr; // stream copy, no encoder
            case EncoderType::X265:
                return "libx265";
            case EncoderType::X264:
                return "libx264";
            case EncoderType::NVENC_HEVC:
                return "hevc_nvenc";
            case EncoderType::NVENC_H264:
                return "h264_nvenc";
            }
            return "libx265";
        }

        // Whether this config requires actual transcoding (decode + encode)
        bool needsTranscoding() const { return encoder != EncoderType::Copy; }

        // Whether this config uses hardware acceleration
        bool usesHardware() const
        {
            return encoder == EncoderType::NVENC_HEVC || encoder == EncoderType::NVENC_H264;
        }
    };

    const char *encoderTypeToString(EncoderType e);
    EncoderType parseEncoderType(const std::string &s);

    // ── Clock alias ─────────────────────────────────────────────────
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Duration = Clock::duration;
    using namespace std::chrono_literals;

} // namespace sm
