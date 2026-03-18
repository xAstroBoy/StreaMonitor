#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Native FFmpeg HLS recorder
// Uses libavformat/libavcodec directly — NO subprocess!
// Full control over every packet, frame, and I/O operation.
// Supports stream-copy AND full transcoding (x265/NVENC).
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "config/config.h"
#include "net/http_client.h"
#include <spdlog/spdlog.h>
#include <string>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <deque>
#include <condition_variable>
#include <cstdint>
#include <climits>

// Forward declare FFmpeg types to avoid header pollution
struct AVFormatContext;
struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct AVStream;
struct AVDictionary;
struct AVBufferRef;
struct SwsContext;
struct AVIOContext;

namespace sm
{

    // ── Recording result ────────────────────────────────────────────
    struct RecordingResult
    {
        bool success = false;
        std::string error;
        uint64_t bytesWritten = 0;
        uint32_t segmentsRecorded = 0;
        double durationSec = 0;
        uint32_t packetsWritten = 0;
        uint32_t packetsDropped = 0;
        uint32_t stallsDetected = 0;
        uint32_t restartsPerformed = 0;
    };

    // ── Progress callback ───────────────────────────────────────────
    struct RecordingProgress
    {
        uint64_t bytesWritten;
        uint32_t packetsWritten;
        double durationSec;
        double speed; // 1.0 = realtime
        bool isStalled;
    };
    using ProgressCallback = std::function<void(const RecordingProgress &)>;

    // ── Pause/resume support (private→public transitions) ──────────
    enum class PauseAction
    {
        Wait,
        Resume,
        Stop
    };
    struct PauseResumeResult
    {
        PauseAction action = PauseAction::Stop;
        std::string newUrl;
    };
    using PauseResumeCallback = std::function<PauseResumeResult()>;

    // ── Resolution change detection ────────────────────────────────
    // Called when the input stream resolution changes (e.g. desktop→mobile).
    // Receives new width, height, and whether it looks like portrait/mobile.
    // Must return the new output file path for the recorder to switch to.
    // If empty string is returned, the recorder continues in the same file.
    struct ResolutionInfo
    {
        int width = 0;
        int height = 0;
        bool isMobile = false; // true if height > width (portrait)
    };
    using ResolutionChangeCallback = std::function<std::string(const ResolutionInfo &)>;

    // ── HLS Recorder (native FFmpeg API) ────────────────────────────
    class HLSRecorder
    {
    public:
        explicit HLSRecorder(const AppConfig &config);
        ~HLSRecorder();

        // Non-copyable
        HLSRecorder(const HLSRecorder &) = delete;
        HLSRecorder &operator=(const HLSRecorder &) = delete;

        // ── Main recording function ─────────────────────────────────
        // Records HLS stream to file. Blocks until cancelled or error.
        RecordingResult record(const std::string &hlsUrl,
                               const std::string &outputPath,
                               CancellationToken &cancel,
                               const std::string &userAgent = "",
                               const std::string &cookies = "",
                               const std::map<std::string, std::string> &headers = {},
                               const VRConfig &vrConfig = {});

        // ── Progress ────────────────────────────────────────────────
        void setProgressCallback(ProgressCallback cb);

        // ── Preview capture (on-demand, in-memory) ─────────────────
        // The GUI sets a request flag via the SitePlugin; when the
        // recorder encounters the next keyframe it decodes to RGBA
        // and delivers pixels through the callback. No file I/O.
        void setPreviewRequestFlag(std::atomic<bool> *flag) { previewRequestFlag_ = flag; }

        using PreviewDataCallback = std::function<void(std::vector<uint8_t> rgba, int w, int h)>;
        void setPreviewDataCallback(PreviewDataCallback cb) { previewDataCb_ = std::move(cb); }

        // ── Pause/resume (keep file open when stream drops) ─────────
        // Called when stream ends naturally (model went private/offline).
        // Instead of closing the output, the recorder pauses and polls
        // this callback. Return Resume+URL to continue recording into
        // the same file, or Stop to finalize and close.
        void setPauseResumeCallback(PauseResumeCallback cb) { pauseResumeCb_ = std::move(cb); }

        // ── Resolution change callback ───────────────────────────────
        // When the input stream's resolution changes (model switched to
        // mobile, or desktop changed res), the recorder closes the current
        // file and calls this to get the new output path. If not set or
        // returns empty, recording continues into the same file.
        void setResolutionChangeCallback(ResolutionChangeCallback cb) { resChangeCb_ = std::move(cb); }

        // ── Logger (optional — defaults to global spdlog) ───────────
        void setLogger(std::shared_ptr<spdlog::logger> lg) { log_ = std::move(lg); }

        // ── Stats (thread-safe) ─────────────────────────────────────
        RecordingStats getStats() const;

    private:
        // ── Internal state ──────────────────────────────────────────
        const AppConfig &config_;
        std::shared_ptr<spdlog::logger> log_ = spdlog::default_logger();
        ProgressCallback progressCb_;
        PreviewDataCallback previewDataCb_;
        PauseResumeCallback pauseResumeCb_;
        ResolutionChangeCallback resChangeCb_;
        mutable std::mutex statsMutex_;
        RecordingStats stats_;

        // Preview capture state (on-demand, in-memory)
        std::atomic<bool> *previewRequestFlag_ = nullptr; // set by SitePlugin

        // Decode a video frame to RGBA pixels (max 640px wide).
        // Returns true if successful. Pixels are stored in outRGBA.
        bool decodeFrameToRGBA(AVFrame *frame, std::vector<uint8_t> &outRGBA, int &outW, int &outH);

        // Deliver preview if requested: decode keyframe → RGBA → callback.
        // Called from the transcode path with an already-decoded frame.
        void maybeDeliverPreviewFrame(AVFrame *frame);

        // ── FFmpeg context management ───────────────────────────────
        struct FFmpegState
        {
            // Muxer / demuxer
            AVFormatContext *inputCtx = nullptr;
            AVFormatContext *outputCtx = nullptr;
            int videoIdx = -1;
            int audioIdx = -1;
            int outVideoIdx = -1;
            int outAudioIdx = -1;
            bool headerWritten = false;

            // Transcoding contexts (nullptr when stream-copying)
            AVCodecContext *videoDecCtx = nullptr; // Video decoder
            AVCodecContext *videoEncCtx = nullptr; // Video encoder (x265/NVENC)
            AVCodecContext *audioDecCtx = nullptr; // Audio decoder (when re-encoding audio)
            AVCodecContext *audioEncCtx = nullptr; // Audio encoder (AAC)
            SwsContext *swsCtx = nullptr;          // Pixel format converter
            AVFrame *decFrame = nullptr;           // Decoded frame buffer
            AVFrame *encFrame = nullptr;           // Encoder input frame (after sws)
            AVBufferRef *hwDeviceCtx = nullptr;    // CUDA hardware device context
            bool transcoding = false;              // true = decode+encode, false = stream copy

            // Timestamp rebuilding (live streams: rebuild from 0)
            // Single offset per stream: first DTS is subtracted from BOTH
            // PTS and DTS, preserving the composition time offset (B-frame
            // display order). Using separate PTS/DTS offsets would destroy
            // the CTO relationship and corrupt Matroska/MP4 timestamps.
            int64_t videoTsOffset = 0;        // First video DTS (subtract from PTS & DTS)
            int64_t audioTsOffset = 0;        // First audio DTS (subtract from PTS & DTS)
            bool videoOffsetCaptured = false; // true once videoTsOffset is set
            bool audioOffsetCaptured = false; // true once audioTsOffset is set
            int64_t videoFrameCount = 0;      // Frame counter for video PTS generation
            int64_t audioSampleCount = 0;     // Sample counter for audio PTS generation

            // Keyframe gating: skip all packets until first video keyframe.
            // Live HLS streams may start mid-GOP — the HEVC decoder cannot decode
            // P/B frames without their reference (IDR) frame, causing POC errors.
            bool gotKeyframe = false;

            // Output resolution tracking (for resolution-change detection)
            int outputWidth = 0;
            int outputHeight = 0;

            // Restart continuity (stream-copy mode): accumulated PTS offset
            // from previous restart iterations, in OUTPUT timebase units.
            int64_t lastVideoOutPts = 0;
            int64_t lastAudioOutPts = 0;
            int64_t videoRestartOffset = 0;
            int64_t audioRestartOffset = 0;
        };

        // Deliver preview from a raw keyframe packet (stream-copy mode).
        // Opens a temporary decoder to decode the single keyframe.
        bool deliverPreviewFromPacket(FFmpegState &state, AVPacket *pkt);

        // Setup / teardown
        bool openInput(FFmpegState &state, const std::string &url,
                       const std::string &userAgent, const std::string &cookies,
                       const std::map<std::string, std::string> &headers,
                       AVIOContext *customAVIO = nullptr);
        bool openOutput(FFmpegState &state, const std::string &outputPath,
                        const VRConfig &vrConfig = {});
        void closeInput(FFmpegState &state); // Close only input (keep output/encoder alive)
        void closeAll(FFmpegState &state);

        // Encoder setup (returns false to fall back to stream copy)
        bool setupVideoEncoder(FFmpegState &state, const EncodingConfig &encCfg);
        bool setupVideoDecoder(FFmpegState &state);
        bool initCudaDevice(FFmpegState &state);

        // VR metadata embedding
        void applyVRMetadata(FFmpegState &state, const VRConfig &vrConfig);

        // Packet processing (stream copy path)
        bool processPacket(FFmpegState &state, AVPacket *pkt,
                           uint64_t &bytesWritten, uint32_t &packetsWritten,
                           uint32_t &packetsDropped);

        // Frame processing (transcode path: decode → [sws] → encode → write)
        bool transcodePacket(FFmpegState &state, AVPacket *pkt,
                             uint64_t &bytesWritten, uint32_t &packetsWritten,
                             uint32_t &packetsDropped);

        // Flush encoder (drain remaining frames at end of stream)
        void flushEncoder(FFmpegState &state, uint64_t &bytesWritten,
                          uint32_t &packetsWritten);

        // ── CUDA/NVENC availability detection ───────────────────────
        static bool isNvencAvailable();

        // Stall detection
        struct StallDetector
        {
            TimePoint lastPacketTime;
            TimePoint lastPtsAdvance;
            int64_t lastPts = 0;
            int consecutiveStalls = 0;
            double avgSpeed = 1.0;
            bool isStalled = false;

            void reset();
            void onPacket(int64_t pts, int64_t fileSize);
            bool checkStall(const AppConfig::FFmpegTuning &tuning) const;
        };

        // ── Segment feeder (direct segment download, no temp file) ─────
        // Instead of writing a playlist to a temp file and letting FFmpeg's
        // HLS demuxer re-read it (which races on Windows), this:
        //   1. Polls the remote m3u8 periodically
        //   2. Parses segment URLs with M3U8Parser
        //   3. Downloads each NEW segment via HttpClient
        //   4. Feeds raw bytes into a ring buffer
        //   5. FFmpeg reads from a custom AVIOContext backed by the buffer
        //
        // Result: FFmpeg sees a continuous fMP4/MPEG-TS stream. No HLS demuxer,
        // no temp files, no file-locking races, no m3u8_hold_counters tuning.
        using PlaylistDecoder = std::function<std::string(const std::string &)>;

        struct SegmentFeeder
        {
            // ── Ring buffer: segment bytes flow from feeder → FFmpeg ──
            std::mutex bufMutex;
            std::condition_variable bufCv;
            std::deque<uint8_t> ringBuf;
            bool finished = false; // no more data coming (stream ended)
            bool hasError = false; // fatal error occurred

            // ── Thread state ──
            std::thread thread;
            std::atomic<bool> running{false};
            CancellationToken *cancel = nullptr;

            // ── AVIO ──
            AVIOContext *avioCtx = nullptr;
            uint8_t *avioBuf = nullptr;
            static constexpr int AVIO_BUF_SIZE = 32768; // 32KB read chunks

            // ── Staleness tracking ──
            std::atomic<int64_t> lastFeedEpochNs{0};

            // ── Config ──
            std::shared_ptr<spdlog::logger> log = spdlog::default_logger();
            const AppConfig *config = nullptr;

            bool isStale(int stallSec = 30) const
            {
                auto ts = lastFeedEpochNs.load(std::memory_order_relaxed);
                if (ts == 0)
                    return false;
                auto now = std::chrono::steady_clock::now().time_since_epoch().count();
                auto ageSec = (now - ts) / 1'000'000'000LL;
                return ageSec > stallSec;
            }

            // Start the feeder: initial fetch + validation, then spawn thread.
            // Returns true if the initial segment data is ready for FFmpeg.
            bool start(const std::string &playlistUrl,
                       const std::string &userAgent,
                       CancellationToken &cancelToken,
                       PlaylistDecoder decoder = nullptr);
            void stop();

            // Create an AVIOContext for FFmpeg to read from.
            // Caller must NOT free — stop() handles cleanup.
            AVIOContext *createAVIO();

            // AVIO read callback (static, opaque = SegmentFeeder*)
            static int readCallback(void *opaque, uint8_t *buf, int bufSize);

        private:
            // Push raw bytes into the ring buffer
            void feedBytes(const uint8_t *data, size_t len);
            void feedBytes(const std::string &data);
        };

        // Interrupt callback for FFmpeg (allows cancellation)
        static int interruptCallback(void *opaque);
    };

    // ── Utility: probe if an HLS playlist URL is still alive ────────
    bool probeHLSPlaylist(const std::string &url, const std::string &userAgent,
                          int timeoutSec = 8);

} // namespace sm
