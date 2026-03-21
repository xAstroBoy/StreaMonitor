#pragma once
// StripHelper C++ — Win32 subprocess management for FFmpeg/FFprobe

#include "config.h"
#include <string>
#include <vector>
#include <map>
#include <functional>
#include <filesystem>
#include <stdexcept>
#include <atomic>

#include <nlohmann/json.hpp>

namespace sh
{

    // ── Exception ───────────────────────────────────────────────────────────────
    class PipelineError : public std::runtime_error
    {
    public:
        fs::path folder;
        std::string stage;
        std::string detail;

        PipelineError(const fs::path &f, const std::string &stg, const std::string &msg)
            : std::runtime_error(stg + ": " + msg), folder(f), stage(stg), detail(msg) {}
    };

    // Thrown when the user hits Stop — NOT a PipelineError, so existing catch
    // blocks for PipelineError won't swallow it.
    class StopRequested : public std::runtime_error
    {
    public:
        StopRequested() : std::runtime_error("Processing stopped by user") {}
    };

    // ── Callback types ──────────────────────────────────────────────────────────
    using ProgressCb = std::function<void(double outTimeSec, int64_t bytesWritten, int64_t targetBytes)>;
    using NoteCb = std::function<void(const std::string &)>;
    using MetricCb = std::function<void(const std::string &stage, int64_t srcSize, int64_t outSize,
                                        double srcDur, double outDur, const std::string &info)>;
    using GuiCb = std::function<void(float pct, float eta, int64_t written, int64_t target)>;

    // ── Probe signature ─────────────────────────────────────────────────────────
    struct ProbeSig
    {
        bool hasV = false, hasA = false;
        std::string vCodec, vPix, aCodec, aLayout;
        int vW = 0, vH = 0, aSr = 0, aCh = 0;
        double vFps = 0.0;
    };

    // ── Process helpers ─────────────────────────────────────────────────────────
    struct RunResult
    {
        int exitCode = -1;
        std::string output; // captured stderr (or stdout for ffprobe)
    };

    // Low-level: run a process, capture one handle (stderr or stdout)
    RunResult runProcess(const std::vector<std::string> &args, const fs::path &cwd,
                         bool captureStdout = false);

    // Run FFmpeg with optional progress parsing (parses -progress pipe:2 from stderr)
    void runFfmpeg(const std::vector<std::string> &cmd, const fs::path &cwd,
                   const std::string &stage, ProgressCb prog = nullptr,
                   int64_t targetBytes = 0, NoteCb note = nullptr);

    // ── FFprobe helpers ─────────────────────────────────────────────────────────
    bool ffprobeOk(const fs::path &fp, const fs::path &cwd);
    double ffprobeBestDuration(const fs::path &fp, const fs::path &cwd);
    double ffprobeFormatDuration(const fs::path &fp, const fs::path &cwd);
    double ffprobeStreamDuration(const fs::path &fp, const fs::path &cwd, const std::string &sel);
    double ffprobeLastPacketPts(const fs::path &fp, const fs::path &cwd);
    double ffprobeFramesDuration(const fs::path &fp, const fs::path &cwd);
    nlohmann::json ffprobeJsonFull(const fs::path &fp, const fs::path &cwd);
    ProbeSig probeSignature(const fs::path &fp, const fs::path &cwd);

    // ── Encoder detection ───────────────────────────────────────────────────────
    std::map<std::string, bool> detectEncoders();
    bool hasAacMf(); // aac_mf available?
    std::string aacEncoderName();

    // ── Hardware acceleration ───────────────────────────────────────────────────
    bool hasCudaHwaccel(); // CUDA decode available?

    // ── PTS continuity analysis ─────────────────────────────────────────────────
    // Returns the largest forward PTS gap (seconds) between consecutive video packets.
    // Normal video has max ~0.04 s; anything > PTS_JUMP_THRESHOLD_SEC is a discontinuity.
    double detectMaxPtsJump(const fs::path &fp, const fs::path &cwd);

    // ── Global stop mechanism ───────────────────────────────────────────────────
    // Kills all active FFmpeg/FFprobe processes instantly and causes runFfmpeg to
    // throw StopRequested.  Call resetGlobalStop() before starting a new batch.
    extern std::atomic<bool> g_stopRequested;
    void requestGlobalStop(); // set flag + terminate all active child processes
    void resetGlobalStop();   // clear flag for next run
    void killAllFfmpeg();     // terminate all tracked child processes

} // namespace sh
