#pragma once
// StripHelper C++ — Pipeline declarations
// Port of striphelper/pipeline.py + validators.py

#include "subprocess.h"
#include <nlohmann/json.hpp>

namespace sh
{

    // ── Encoder ladder (hardware-first) ─────────────────────────────────────────
    struct EncoderArgs
    {
        std::vector<std::string> args;
        std::string label;
    };
    std::vector<EncoderArgs> encoderArgsetsFast();

    // ── Validation results ──────────────────────────────────────────────────────
    struct ValidationMetrics
    {
        int64_t srcSize = 0, outSize = 0;
        double srcDur = 0, outDur = 0;
        double srcProbe = 0, srcPkt = 0, srcFrames = 0;
        double outProbe = 0, outPkt = 0, outFrames = 0;
        int64_t minSize = 0;
        double minDur = 0;
        bool sizeOk = false, durOk = false;
        double tailDelta = 0, tailAllowed = 0;
        std::string acceptReason;
    };

    struct ValidationResult
    {
        bool ok = false;
        std::string msg;
        ValidationMetrics met;
    };

    // ── Duration helpers ────────────────────────────────────────────────────────
    struct DurationInfo
    {
        double best = 0, probed = 0, pkt = 0, frames = 0;
    };
    DurationInfo bestDurationWithFallback(const fs::path &fp, const fs::path &cwd, double refTotal = -1.0);

    // ── Validation ──────────────────────────────────────────────────────────────
    ValidationResult validateAgainstSource(const fs::path &folder, const fs::path &src, const fs::path &out,
                                           bool isTs, bool ignoreSize = false);
    ValidationResult validateMerge(const fs::path &folder, const fs::path &out,
                                   int64_t targetBytes, double totalDur, bool isTs = false,
                                   bool ignoreSize = false);

    // ── Pipeline steps ──────────────────────────────────────────────────────────
    void tsStreamcopyPostprocess(const fs::path &folder, const fs::path &src, const fs::path &dst,
                                 NoteCb note, GuiCb gui = nullptr);

    void mkvStreamcopyPostprocess(const fs::path &folder, const fs::path &src, const fs::path &dst,
                                  NoteCb note, GuiCb gui = nullptr);

    std::pair<bool, ValidationMetrics>
    runPassesWithValidate(const fs::path &folder, const fs::path &src, const fs::path &dst,
                          bool isTs, NoteCb note, MetricCb metric = nullptr,
                          const std::string &prefix = "remux", GuiCb gui = nullptr);

    bool salvageReencode(const fs::path &folder, const fs::path &src, const fs::path &dst,
                         bool isTs, NoteCb note, MetricCb metric = nullptr,
                         const std::string &prefix = "salvage", GuiCb gui = nullptr);

    // ── Concat ──────────────────────────────────────────────────────────────────
    fs::path writeConcat(const fs::path &folder, const std::vector<fs::path> &parts,
                         bool includeZero, const std::string &fname = "concat_list.txt");

    void concatCopyMkv(const fs::path &folder, const fs::path &concatTxt,
                       ProgressCb prog = nullptr, int64_t targetBytes = 0, NoteCb note = nullptr,
                       const std::string &stageLabel = "concat copy -> MKV");

    std::vector<fs::path> normalizeParts(const std::vector<fs::path> &parts, const fs::path &folder, NoteCb note);

    bool concatReencodeFilter(const std::vector<fs::path> &parts, const fs::path &folder,
                              NoteCb note = nullptr, ProgressCb prog = nullptr);

    // ── High-level ──────────────────────────────────────────────────────────────
    std::vector<fs::path> findWorkFolders(const fs::path &root);

    void mergeFolder(const fs::path &folder, GuiCb gui = nullptr,
                     const nlohmann::json &cfg = {}, bool mkLinks = false,
                     NoteCb note = nullptr, MetricCb metric = nullptr,
                     bool fixPts = true);

    void makeSymlink(const fs::path &folder, const nlohmann::json &cfg);
    void resetToProcessPurge(); // call before a new batch to re-arm the one-time wipe

    void purgeTemp(const fs::path &folder);

    // ── PTS repair ──────────────────────────────────────────────────────────────
    // Re-encode with CUDA hwaccel + N/fps/TB sequential timestamps.
    // Preserves ALL actual frames — no freeze frames, no frame drops, no gaps.
    bool repairTimestamps(const fs::path &folder, const fs::path &src, const fs::path &dst,
                          NoteCb note = nullptr, GuiCb gui = nullptr);

    // ── PTS-aware .ts conversion ───────────────────────────────────────────────
    // Smart pre-processor for .ts files from stream captures.
    // 1) Quick eligibility: must be MPEG-TS, above PTS_CHECK_MIN_BYTES
    // 2) Probes PTS — if max gap <= threshold, returns false (file is clean)
    // 3) If broken: re-encodes .ts → .mkv with CUDA + N/fps/TB, returns true
    // The caller should skip normal streamcopy when this returns true.
    bool fixTsPts(const fs::path &folder, const fs::path &tsFile, const fs::path &mkvOut,
                  NoteCb note = nullptr, GuiCb gui = nullptr);

} // namespace sh
