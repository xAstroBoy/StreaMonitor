// StripHelper C++ — Full FFmpeg pipeline
// Faithful port of striphelper/pipeline.py + validators.py

#include "pipeline.h"
#include "native_ffmpeg.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <set>
#include <chrono>
#include <sstream>
#include <windows.h>

namespace sh
{

    // ── Global encoder mode (read by all pipeline threads) ─────────────────────
    std::atomic<EncoderMode> g_encoderMode{EncoderMode::CPU};

    // ── Encoder ladder (respects g_encoderMode) ─────────────────────────────────

    std::vector<EncoderArgs> encoderArgsetsFast()
    {
        auto have = detectEncoders();
        std::vector<EncoderArgs> out;
        bool cuda = (g_encoderMode.load(std::memory_order_relaxed) == EncoderMode::CUDA);

        auto add = [&](const char *key, std::vector<std::string> args, std::string label)
        {
            if (have.count(key) && have[key])
                out.push_back({std::move(args), std::move(label)});
        };

        if (cuda)
        {
            // CUDA mode: NVENC first, then QSV/AMF, CPU as last resort
            add("hevc_nvenc", {"-c:v", "hevc_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", "19", "-bf", "0"}, "hevc_nvenc_p5_cq19");
            add("h264_nvenc", {"-c:v", "h264_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", "19", "-bf", "0"}, "h264_nvenc_p5_cq19");
            add("av1_nvenc", {"-c:v", "av1_nvenc", "-preset", "p5", "-rc", "vbr", "-cq", "23", "-bf", "0"}, "av1_nvenc_p5_cq23");
            add("hevc_qsv", {"-c:v", "hevc_qsv", "-global_quality", "21", "-look_ahead", "0"}, "hevc_qsv_gq21");
            add("h264_qsv", {"-c:v", "h264_qsv", "-global_quality", "21", "-look_ahead", "0"}, "h264_qsv_gq21");
            add("av1_qsv", {"-c:v", "av1_qsv", "-global_quality", "27"}, "av1_qsv_gq27");
            add("hevc_amf", {"-c:v", "hevc_amf", "-quality", "speed", "-usage", "transcoding"}, "hevc_amf_speed");
            add("h264_amf", {"-c:v", "h264_amf", "-quality", "speed", "-usage", "transcoding"}, "h264_amf_speed");
            add("av1_amf", {"-c:v", "av1_amf", "-quality", "speed", "-usage", "transcoding"}, "av1_amf_speed");
            // CPU fallback
            if (have.count("libx264") && have["libx264"])
                out.push_back({{"-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-tune", "fastdecode"}, "libx264_vfast_crf20"});
            if (have.count("libx265") && have["libx265"])
                out.push_back({{"-c:v", "libx265", "-preset", "fast", "-x265-params", "crf=22:pmode=1:pme=1"}, "libx265_fast_crf22"});
        }
        else
        {
            // CPU mode: software encoders only — no GPU involvement
            if (have.count("libx264") && have["libx264"])
                out.push_back({{"-c:v", "libx264", "-preset", "veryfast", "-crf", "20", "-tune", "fastdecode"}, "libx264_vfast_crf20"});
            if (have.count("libx265") && have["libx265"])
                out.push_back({{"-c:v", "libx265", "-preset", "fast", "-x265-params", "crf=22:pmode=1:pme=1"}, "libx265_fast_crf22"});
        }

        return out;
    }

    // ── Duration with multi-fallback ────────────────────────────────────────────

    DurationInfo bestDurationWithFallback(const fs::path &fp, const fs::path &cwd, double refTotal)
    {
        DurationInfo d;

        // First try native probe (fast, uses libavformat directly)
        auto fullPath = (cwd / fp.filename());
        d.probed = nativeProbeDuration(fullPath);

        // Only fallback to subprocess if native probe failed
        if (d.probed <= 0.0)
            d.probed = ffprobeBestDuration(fp, cwd);

        // For MKV files with valid duration, trust it and skip expensive packet scanning
        auto ext = fp.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool isMkv = (ext == ".mkv");

        // Only do expensive fallbacks for truly broken files (duration missing or way too short)
        // For merged MKV files, container duration should be accurate
        bool needPkt = !isMkv && ((d.probed <= 0.0) ||
                                  (refTotal > 0 && d.probed < refTotal * 0.20));
        d.pkt = needPkt ? ffprobeLastPacketPts(fp, cwd) : 0.0;
        d.best = std::max(d.probed, d.pkt);

        // NEVER do frame counting - it's insanely slow and decodes the entire file
        // Trust the container duration for MKV files
        d.frames = 0.0;
        return d;
    }

    // ── Validation against source ───────────────────────────────────────────────

    ValidationResult validateAgainstSource(const fs::path &folder, const fs::path &src, const fs::path &out,
                                           bool isTs, bool ignoreSize)
    {
        ValidationResult r;
        try
        {
            if (!fs::exists(folder / out.filename()))
            {
                r.msg = "Output does not exist";
                return r;
            }

            auto srcSize = (int64_t)fs::file_size(folder / src.filename());
            auto outSize = (int64_t)fs::file_size(folder / out.filename());

            auto srcD = bestDurationWithFallback(src, folder);
            auto outD = bestDurationWithFallback(out, folder, srcD.best > 0 ? srcD.best : -1.0);

            double minSizeRatio = isTs ? MIN_SIZE_RATIO_TS2MUX : MIN_SIZE_RATIO_GENERIC;
            double minDurRatio = isTs ? MIN_DUR_RATIO_TS2MUX : MIN_DUR_RATIO_GENERIC;
            double tailAllowed = isTs ? TAIL_TOL_SEC_TS2MUX : TAIL_TOL_SEC_GENERIC;

            int64_t minSize = (int64_t)(srcSize * minSizeRatio);
            double minDur = srcD.best * minDurRatio;

            bool sizeOk = ignoreSize || (outSize >= minSize);
            bool durOk = (srcD.best <= 0) || (outD.best <= 0) || (outD.best >= minDur);

            double tailDelta = (srcD.best > 0 && outD.best > 0) ? (srcD.best - outD.best) : 0.0;
            bool durTolOk = sizeOk && (srcD.best > 0) && (outD.best > 0) && (tailDelta <= tailAllowed);

            // TS probe bogus relaxation
            bool durRelaxed = false;
            if (isTs && srcD.pkt <= 0 && outD.best >= 600.0 && srcD.probed >= outD.best * 1.20)
            {
                durOk = true;
                durRelaxed = true;
            }

            bool ok = sizeOk && (durOk || durTolOk);

            r.ok = ok;
            r.met = {srcSize, outSize, srcD.best, outD.best,
                     srcD.probed, srcD.pkt, srcD.frames,
                     outD.probed, outD.pkt, outD.frames,
                     minSize, minDur, sizeOk, durOk,
                     tailDelta, tailAllowed, ""};

            if (ok)
            {
                std::string reason;
                if (ignoreSize)
                    reason += "size ignored";
                if (!durOk && durTolOk)
                {
                    if (!reason.empty())
                        reason += " | ";
                    reason += "tail-tolerance (d=" + humanSecs(tailDelta) + " <= " + humanSecs(tailAllowed) + ")";
                }
                if (durRelaxed)
                {
                    if (!reason.empty())
                        reason += " | ";
                    reason += "ts_probe_bogus";
                }
                r.met.acceptReason = reason;
            }
            else
            {
                std::ostringstream os;
                os << "Validation failed: out=" << outSize << "B/" << outD.best << "s"
                   << " vs src=" << srcSize << "B/" << srcD.best << "s";
                r.msg = os.str();
            }
        }
        catch (const std::exception &e)
        {
            r.msg = e.what();
        }
        return r;
    }

    // ── Validate merge ──────────────────────────────────────────────────────────

    ValidationResult validateMerge(const fs::path &folder, const fs::path &out,
                                   int64_t targetBytes, double totalDur, bool isTs,
                                   bool ignoreSize)
    {
        ValidationResult r;
        try
        {
            auto outFull = folder / out.filename();
            int64_t outSize = fs::exists(outFull) ? (int64_t)fs::file_size(outFull) : 0;
            auto d = bestDurationWithFallback(out, folder, totalDur);

            double minSizeRatio = isTs ? MIN_SIZE_RATIO_TS2MUX : MIN_SIZE_RATIO_GENERIC;
            double minDurRatio = isTs ? MIN_DUR_RATIO_TS2MUX : MIN_DUR_RATIO_GENERIC;
            int64_t minSize = (int64_t)(targetBytes * minSizeRatio);
            double minDur = totalDur * minDurRatio;
            double tailDelta = (totalDur > 0 && d.best > 0) ? (totalDur - d.best) : 0.0;

            bool sizeOk = ignoreSize || (outSize >= minSize);
            bool durOk = (totalDur <= 0) || (d.best <= 0) || (d.best >= minDur);
            bool durTolOk = sizeOk && (totalDur > 0) && (d.best > 0) && (tailDelta <= TAIL_TOL_SEC_MERGE);

            bool durRelaxed = false;
            if (isTs && d.pkt <= 0 && d.best >= 600.0 && d.probed >= d.best * 1.20)
            {
                durOk = true;
                durRelaxed = true;
            }

            r.ok = sizeOk && (durOk || durTolOk);
            r.met = {targetBytes, outSize, totalDur, d.best,
                     0, 0, 0, d.probed, d.pkt, d.frames,
                     minSize, minDur, sizeOk, durOk,
                     tailDelta, TAIL_TOL_SEC_MERGE, ""};

            if (r.ok)
            {
                std::string reason;
                if (ignoreSize)
                    reason += "size ignored";
                if (!durOk && durTolOk)
                {
                    if (!reason.empty())
                        reason += " | ";
                    reason += "tail-tolerance";
                }
                if (durRelaxed)
                {
                    if (!reason.empty())
                        reason += " | ";
                    reason += "ts_probe_bogus";
                }
                r.met.acceptReason = reason;
            }
            else
            {
                std::ostringstream os;
                os << "merge-validate FAIL: out=" << outSize << "B/" << d.best << "s"
                   << " need>=" << minSize << "B/>=" << minDur << "s"
                   << " (target=" << targetBytes << "B/" << totalDur << "s)";
                r.msg = os.str();
            }
        }
        catch (const std::exception &e)
        {
            r.msg = e.what();
        }
        return r;
    }

    // ── Build FFmpeg pass command ───────────────────────────────────────────────

    static std::vector<std::string> buildPassCmd(const std::string &mode, const std::string &inName, const std::string &outName)
    {
        std::vector<std::string> common = {"-hide_banner", "-loglevel", "error",
                                           "-err_detect", "+ignore_err", "-analyzeduration", "2147483647", "-probesize", "2147483647"};

        std::vector<std::string> cmd = {g_ffmpeg, "-y"};
        cmd.insert(cmd.end(), common.begin(), common.end());

        if (mode == "A_default")
        {
            auto tail = std::vector<std::string>{
                "-fflags", "+genpts+igndts+discardcorrupt", "-use_wallclock_as_timestamps", "1", "-copytb", "0",
                "-i", inName,
                "-map", "0:v:0?", "-map", "0:a?", "-dn", "-sn", "-c", "copy", "-copyinkf",
                "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0", outName};
            cmd.insert(cmd.end(), tail.begin(), tail.end());
        }
        else if (mode == "B_nowallclock")
        {
            auto tail = std::vector<std::string>{
                "-fflags", "+genpts+igndts+discardcorrupt",
                "-i", inName,
                "-map", "0:v:0?", "-map", "0:a?", "-dn", "-sn", "-c", "copy", "-copyinkf",
                "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0", outName};
            cmd.insert(cmd.end(), tail.begin(), tail.end());
        }
        else if (mode == "C_copyts")
        {
            auto tail = std::vector<std::string>{
                "-fflags", "+discardcorrupt", "-copyts", "-start_at_zero",
                "-i", inName,
                "-map", "0:v:0?", "-map", "0:a?", "-dn", "-sn", "-c", "copy", "-copyinkf",
                "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0", outName};
            cmd.insert(cmd.end(), tail.begin(), tail.end());
        }
        return cmd;
    }

    // ── TS streamcopy postprocess ───────────────────────────────────────────────

    void tsStreamcopyPostprocess(const fs::path &folder, const fs::path &src, const fs::path &dst,
                                 NoteCb note, GuiCb gui)
    {
        if (note)
            note("ts-postprocess " + src.filename().string() + " -> " + dst.filename().string());

        int64_t srcSize = 0;
        try
        {
            srcSize = (int64_t)fs::file_size(folder / src.filename());
        }
        catch (...)
        {
        }

        // ── Native fast-path (direct FFmpeg API, ~3-5x faster) ──
        try
        {
            NativeProgressCb nProg = nullptr;
            if (gui)
            {
                nProg = [&](double, int64_t written, int64_t target)
                {
                    int64_t tgt = target > 0 ? target : (srcSize > 0 ? srcSize : 1);
                    float pct = 100.0f * std::min<float>((float)written / (float)tgt, 1.0f);
                    gui(pct, 0, written, tgt);
                };
            }
            if (nativeRemux(folder / src.filename(), folder / dst.filename(), folder, nProg))
            {
                if (note)
                    note("ts streamcopy [" + src.filename().string() + "] OK (native)");
                return;
            }
        }
        catch (...)
        {
        }

        // ── Subprocess fallback ──
        if (note)
            note("native remux failed, falling back to subprocess");

        ProgressCb prog = nullptr;
        if (gui)
        {
            prog = [&](double, int64_t written, int64_t target)
            {
                int64_t tgt = target > 0 ? target : (srcSize > 0 ? srcSize : 1);
                float pct = 100.0f * std::min<float>((float)written / (float)tgt, 1.0f);
                gui(pct, 0, written, tgt);
            };
        }

        std::vector<std::string> cmd = {
            g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
            "-fflags", "+genpts+discardcorrupt",
            "-analyzeduration", "2147483647", "-probesize", "2147483647",
            "-err_detect", "+ignore_err",
            "-i", "./" + src.filename().string(),
            "-map", "0", "-c", "copy", "-copyinkf",
            "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
            "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
            "./" + dst.filename().string()};
        runFfmpeg(cmd, folder, "ts streamcopy [" + src.filename().string() + "]", prog, srcSize, note);
    }

    void mkvStreamcopyPostprocess(const fs::path &folder, const fs::path &src, const fs::path &dst,
                                  NoteCb note, GuiCb gui)
    {
        if (note)
            note("merge postprocess streamcopy");

        int64_t srcSize = 0;
        try
        {
            srcSize = (int64_t)fs::file_size(folder / src.filename());
        }
        catch (...)
        {
        }

        // ── Native fast-path ──
        try
        {
            NativeProgressCb nProg = nullptr;
            if (gui)
            {
                nProg = [&](double, int64_t written, int64_t target)
                {
                    int64_t tgt = target > 0 ? target : (srcSize > 0 ? srcSize : 1);
                    float pct = 100.0f * std::min<float>((float)written / (float)tgt, 1.0f);
                    gui(pct, 0, written, tgt);
                };
            }
            if (nativeRemux(folder / src.filename(), folder / dst.filename(), folder, nProg))
            {
                if (note)
                    note("merge postprocess OK (native)");
                return;
            }
        }
        catch (...)
        {
        }

        // ── Subprocess fallback ──
        if (note)
            note("native remux failed, falling back to subprocess");

        ProgressCb prog = nullptr;
        if (gui)
        {
            prog = [&](double, int64_t written, int64_t target)
            {
                int64_t tgt = target > 0 ? target : (srcSize > 0 ? srcSize : 1);
                float pct = 100.0f * std::min<float>((float)written / (float)tgt, 1.0f);
                gui(pct, 0, written, tgt);
            };
        }

        std::vector<std::string> cmd = {
            g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
            "-fflags", "+genpts+discardcorrupt",
            "-analyzeduration", "2147483647", "-probesize", "2147483647",
            "-err_detect", "+ignore_err",
            "-i", "./" + src.filename().string(),
            "-map", "0", "-c", "copy", "-copyinkf",
            "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
            "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
            "./" + dst.filename().string()};
        runFfmpeg(cmd, folder, "merge postprocess", prog, srcSize, note);
    }

    // ── Multi-pass remux with validation ────────────────────────────────────────

    static bool shouldSalvage(const ValidationMetrics &met, bool isTs)
    {
        double tailAllowed = isTs ? TAIL_TOL_SEC_TS2MUX : TAIL_TOL_SEC_GENERIC;
        return met.tailDelta > tailAllowed;
    }

    std::pair<bool, ValidationMetrics>
    runPassesWithValidate(const fs::path &folder, const fs::path &src, const fs::path &dst,
                          bool isTs, NoteCb note, MetricCb metric,
                          const std::string &prefix, GuiCb gui)
    {
        const char *modes[] = {"A_default", "B_nowallclock", "C_copyts"};
        ValidationMetrics lastMet;

        double srcDur = std::max(ffprobeBestDuration(src, folder), 0.0);
        int64_t srcSize = 0;
        try
        {
            srcSize = (int64_t)fs::file_size(folder / src.filename());
        }
        catch (...)
        {
        }

        ProgressCb prog = nullptr;
        if (gui)
        {
            prog = [&](double, int64_t written, int64_t target)
            {
                int64_t tgt = target > 0 ? target : (srcSize > 0 ? srcSize : 1);
                float pct = 100.0f * std::min<float>((float)written / (float)tgt, 1.0f);
                gui(pct, 0, written, tgt);
            };
        }

        for (int i = 0; i < 3; ++i)
        {
            if (note)
                note(prefix + " " + src.filename().string() + " pass" + std::to_string(i + 1) + " [" + modes[i] + "]");
            auto cmd = buildPassCmd(modes[i], src.filename().string(), dst.filename().string());

            try
            {
                runFfmpeg(cmd, folder, prefix + " pass" + std::to_string(i + 1), prog, srcSize, note);
            }
            catch (const PipelineError &)
            {
                // pass failed, try next
            }

            auto vr = validateAgainstSource(folder, src, dst, isTs, isTs);
            lastMet = vr.met;

            if (metric)
            {
                metric(prefix + " pass" + std::to_string(i + 1) + " [" + modes[i] + "]",
                       vr.met.srcSize, vr.met.outSize, vr.met.srcDur, vr.met.outDur,
                       "size_ok=" + std::to_string(vr.met.sizeOk) + " dur_ok=" + std::to_string(vr.met.durOk));
            }

            if (!vr.ok && isTs && vr.met.durOk)
            {
                if (note)
                    note(prefix + " pass" + std::to_string(i + 1) + " OK [TS dur match]");
                return {true, vr.met};
            }
            if (vr.ok)
            {
                if (note)
                    note(prefix + " pass" + std::to_string(i + 1) + " OK");
                return {true, vr.met};
            }

            // Clean up failed output
            std::error_code ec;
            fs::remove(folder / dst.filename(), ec);
        }
        return {false, lastMet};
    }

    // ── Salvage re-encode ───────────────────────────────────────────────────────

    bool salvageReencode(const fs::path &folder, const fs::path &src, const fs::path &dst,
                         bool isTs, NoteCb note, MetricCb metric,
                         const std::string &prefix, GuiCb gui)
    {
        auto sig = probeSignature(src, folder);
        int fps = std::max((int)std::round(sig.vFps), 0);
        if (fps == 0)
            fps = DEFAULT_TARGET_FPS;
        bool hasAudio = sig.hasA;

        double srcDur = std::max(ffprobeBestDuration(src, folder), 0.0);
        int64_t srcSize = 0;
        try
        {
            srcSize = (int64_t)fs::file_size(folder / src.filename());
        }
        catch (...)
        {
        }

        // PTS rebuild filters
        bool fullRange = sig.vPix.rfind("yuvj", 0) == 0;
        std::string vf = "setpts=PTS-STARTPTS";
        if (fullRange)
            vf += ",scale=in_range=full:out_range=tv";
        vf += ",format=yuv420p";
        std::string af = "asetpts=PTS-STARTPTS,aresample=async=1:min_comp=0.001:first_pts=0";

        ProgressCb prog = nullptr;
        if (gui)
        {
            prog = [&](double outTime, int64_t written, int64_t target)
            {
                float pct = (srcDur > 1.0) ? 100.0f * std::min(1.0f, (float)(outTime / srcDur)) : 0.0f;
                int64_t tgt = target > 0 ? target : srcSize;
                gui(pct, 0, written, tgt);
            };
        }

        std::vector<std::string> errs;
        auto encoders = encoderArgsetsFast();
        bool cudaMode = (g_encoderMode.load(std::memory_order_relaxed) == EncoderMode::CUDA);
        bool hasCuda = cudaMode && hasCudaHwaccel();
        int hwFailCount = 0;
        constexpr int MAX_HW_FAILURES = 2; // bail to CPU after 2 HW encoder failures

        for (auto &enc : encoders)
        {
            bool isSoftware = (enc.label.find("libx264") != std::string::npos ||
                               enc.label.find("libx265") != std::string::npos);
            if (!isSoftware && hwFailCount >= MAX_HW_FAILURES)
                continue;

            bool isNvenc = (enc.label.find("nvenc") != std::string::npos);
            // For NVENC: try CUDA decode first, then CPU decode + NVENC if CUDA decode chokes
            int passes = (hasCuda && isNvenc) ? 2 : 1;
            for (int pass = 0; pass < passes; ++pass)
            {
                bool useCudaDec = (pass == 0 && hasCuda && isNvenc);
                try
                {
                    if (note)
                        note(prefix + " reencode [" + enc.label + (useCudaDec ? " +CUDA" : "") + "] ~" + std::to_string(fps) + "fps");
                    std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                                    "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err"};
                    if (useCudaDec)
                        cmd.insert(cmd.end(), {"-hwaccel", "cuda"});
                    cmd.insert(cmd.end(), {"-i", "./" + src.filename().string()});

                    if (hasAudio)
                    {
                        cmd.insert(cmd.end(), {"-map", "0:v:0?", "-map", "0:a:0?"});
                    }
                    else
                    {
                        std::string durStr = std::to_string(srcDur > 0 ? srcDur : 0.1);
                        cmd.insert(cmd.end(), {"-f", "lavfi", "-t", durStr, "-i",
                                               "anullsrc=r=" + std::to_string(TARGET_AUDIO_SR) + ":cl=stereo",
                                               "-map", "0:v:0?", "-map", "1:a:0"});
                    }

                    cmd.insert(cmd.end(), {"-vf", vf, "-af", af, "-vsync", "cfr", "-r", std::to_string(fps)});
                    cmd.insert(cmd.end(), enc.args.begin(), enc.args.end());
                    cmd.insert(cmd.end(), {"-c:a", aacEncoderName(), "-ar", std::to_string(TARGET_AUDIO_SR),
                                           "-ac", std::to_string(TARGET_AUDIO_CH)});
                    cmd.insert(cmd.end(), {"-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                                           "./" + dst.filename().string()});

                    runFfmpeg(cmd, folder, prefix + " [" + enc.label + "]", prog, srcSize, note);

                    auto vr = validateAgainstSource(folder, src, dst, isTs, SALVAGE_IGNORE_SIZE);
                    if (vr.ok)
                    {
                        if (note)
                            note(prefix + " OK [" + enc.label + (useCudaDec ? " +CUDA" : "") + "]");
                        return true;
                    }
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    errs.push_back("[" + enc.label + "] validate: " + vr.msg);
                    break; // validation failed — not a CUDA decode issue, skip to next encoder
                }
                catch (const PipelineError &e)
                {
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    std::string tag = "[" + enc.label + (useCudaDec ? " +CUDA" : "") + "]";
                    errs.push_back(tag + " ffmpeg: " + e.detail.substr(0, 500));
                    if (!isSoftware)
                        ++hwFailCount;
                    if (note && useCudaDec && passes > 1)
                        note("CUDA decode failed — retrying " + enc.label + " with CPU decode");
                }
            }
        }

        // ── NVENC bare fallback tiers before CPU (skip if HW already exhausted) ──
        if (hwFailCount < MAX_HW_FAILURES)
        {
            auto have = detectEncoders();
            const char *nvencFallbacks[] = {"h264_nvenc"}; // only try ONE bare fallback
            for (auto &encName : nvencFallbacks)
            {
                if (!have.count(encName) || !have[encName])
                    continue;
                try
                {
                    if (note)
                        note(prefix + " fallback [" + std::string(encName) + " bare]");
                    std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                                    "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err",
                                                    "-i", "./" + src.filename().string(),
                                                    "-map", "0:v:0?", "-map", "0:a:0?",
                                                    "-vf", vf,
                                                    "-af", af,
                                                    "-vsync", "cfr", "-r", std::to_string(fps),
                                                    "-c:v", encName, "-preset", "p4", "-rc", "vbr", "-cq", "22",
                                                    "-c:a", aacEncoderName(), "-ar", std::to_string(TARGET_AUDIO_SR),
                                                    "-ac", std::to_string(TARGET_AUDIO_CH),
                                                    "-avoid_negative_ts", "make_zero",
                                                    "./" + dst.filename().string()};
                    runFfmpeg(cmd, folder, prefix + " [" + std::string(encName) + " bare]", prog, srcSize, note);

                    auto vr = validateAgainstSource(folder, src, dst, isTs, SALVAGE_IGNORE_SIZE);
                    if (vr.ok)
                    {
                        if (note)
                            note(prefix + " OK [" + std::string(encName) + " bare]");
                        return true;
                    }
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    errs.push_back("[" + std::string(encName) + " bare] validate: " + vr.msg);
                }
                catch (const PipelineError &e)
                {
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    errs.push_back("[" + std::string(encName) + " bare] " + e.detail.substr(0, 300));
                    if (note)
                        note(prefix + " [" + std::string(encName) + " bare] failed: " + e.detail.substr(0, 200));
                }
            }
        }

        // Final fallback: libx264 safe mode — ultrafast for max speed
        try
        {
            if (note)
                note(prefix + " fallback [libx264_safe]");
            std::vector<std::string> cmd = {g_ffmpeg, "-y", "-threads", "0",
                                            "-i", "./" + src.filename().string(),
                                            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
                                            "-c:a", "aac", "-ar", std::to_string(TARGET_AUDIO_SR), "-ac", std::to_string(TARGET_AUDIO_CH),
                                            "./" + dst.filename().string()};
            runFfmpeg(cmd, folder, prefix + " [libx264_safe]", prog, srcSize, note);
            return true;
        }
        catch (const std::exception &e)
        {
            if (g_stopRequested.load())
                throw; // don't swallow stop
            std::string all;
            for (auto &s : errs)
                all += s + "\n";
            throw PipelineError(folder, prefix, "All encoders failed:\n" + all);
        }
    }

    // ── Concat helpers ──────────────────────────────────────────────────────────

    fs::path writeConcat(const fs::path &folder, const std::vector<fs::path> &parts,
                         bool includeZero, const std::string &fname)
    {
        auto txt = folder / fname;
        std::ofstream f(txt, std::ios::out | std::ios::trunc);
        // Write absolute paths with forward slashes so the FFmpeg concat
        // demuxer never needs relative path resolution. Fully thread-safe
        // and avoids creating a second temp .txt file.
        auto writeAbsEntry = [&](const std::string &filename)
        {
            auto absPath = (folder / filename).string();
            // FFmpeg expects forward slashes even on Windows
            std::replace(absPath.begin(), absPath.end(), '\\', '/');
            // Escape single quotes for FFmpeg concat
            std::string safe;
            for (char c : absPath)
            {
                if (c == '\'')
                    safe += "'\\''";
                else
                    safe += c;
            }
            f << "file '" << safe << "'\n";
        };
        if (includeZero)
            writeAbsEntry("0.mkv");
        for (auto &p : parts)
        {
            if (p.filename() == "0.mkv")
                continue;
            writeAbsEntry(p.filename().string());
        }
        return txt;
    }

    void concatCopyMkv(const fs::path &folder, const fs::path &concatTxt,
                       ProgressCb prog, int64_t targetBytes, NoteCb note,
                       const std::string &stageLabel)
    {
        auto tmp = folder / "0~merge.mkv";

        // ── Native fast-path ──
        try
        {
            NativeProgressCb nProg = nullptr;
            bool finalizingShown = false;
            if (prog || note)
            {
                nProg = [&](double timeSec, int64_t written, int64_t target)
                {
                    // timeSec == -1 signals finalization phase (av_write_trailer)
                    if (timeSec < 0 && !finalizingShown)
                    {
                        finalizingShown = true;
                        if (note)
                            note("finalizing MKV index...");
                    }
                    if (prog && timeSec >= 0)
                        prog(timeSec, written, target > 0 ? target : targetBytes);
                };
            }
            NativeCancelCb cancelCb = []()
            { return g_stopRequested.load(); };
            if (nativeConcat(folder / concatTxt.filename(), tmp, folder, nProg, cancelCb))
            {
                if (note)
                    note(stageLabel + " OK (native)");
                return;
            }
            // If stop was requested, don't fall through to subprocess — propagate stop
            if (g_stopRequested.load())
                throw StopRequested();
        }
        catch (const StopRequested &)
        {
            throw; // propagate immediately
        }
        catch (...)
        {
            if (g_stopRequested.load())
                throw StopRequested();
        }

        // ── Subprocess fallback ──
        if (note)
            note("native concat failed, falling back to subprocess");

        std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                        "-f", "concat", "-safe", "0", "-i", "./" + concatTxt.filename().string(),
                                        "-c", "copy", "-map", "0", "-copyinkf",
                                        "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                                        "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
                                        "./" + tmp.filename().string()};
        runFfmpeg(cmd, folder, stageLabel, prog, targetBytes, note);
    }

    std::vector<fs::path> normalizeParts(const std::vector<fs::path> &parts, const fs::path &folder, NoteCb note)
    {
        std::vector<fs::path> norm;
        for (auto &p : parts)
        {
            auto sig = probeSignature(p, folder);
            auto out = p;
            out.replace_extension(".norm.mkv");
            if (note)
                note("normalize " + p.filename().string() + " -> AAC stereo");

            std::vector<std::string> cmd;
            if (sig.hasA)
            {
                cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                       "-i", "./" + p.filename().string(),
                       "-map", "0:v:0", "-map", "0:a:0", "-dn", "-sn",
                       "-c:v", "copy", "-c:a", aacEncoderName(),
                       "-ar", std::to_string(TARGET_AUDIO_SR), "-ac", std::to_string(TARGET_AUDIO_CH),
                       "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                       "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
                       "./" + out.filename().string()};
            }
            else
            {
                double dur = std::max(ffprobeBestDuration(p, folder), 0.1);
                cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                       "-i", "./" + p.filename().string(),
                       "-f", "lavfi", "-t", std::to_string(dur),
                       "-i", "anullsrc=r=" + std::to_string(TARGET_AUDIO_SR) + ":cl=stereo",
                       "-map", "0:v:0", "-map", "1:a:0", "-dn", "-sn",
                       "-c:v", "copy", "-c:a", aacEncoderName(),
                       "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                       "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
                       "./" + out.filename().string()};
            }
            runFfmpeg(cmd, folder, "normalize " + p.filename().string(), nullptr, 0, note);
            norm.push_back(out);
        }
        return norm;
    }

    static std::tuple<int, int, int> chooseTargetGeometry(const std::vector<fs::path> &parts, const fs::path &folder)
    {
        int maxW = 0, maxH = 0;
        std::vector<double> fpsVals;
        for (auto &p : parts)
        {
            auto sig = probeSignature(p, folder);
            maxW = std::max(maxW, sig.vW);
            maxH = std::max(maxH, sig.vH);
            if (sig.vFps > 0)
                fpsVals.push_back(sig.vFps);
        }
        int targetFps = DEFAULT_TARGET_FPS;
        if (!fpsVals.empty())
            targetFps = std::max(1, (int)std::round(*std::max_element(fpsVals.begin(), fpsVals.end())));
        if (maxW <= 0)
            maxW = 640;
        if (maxH <= 0)
            maxH = 360;
        if (maxW % 2)
            --maxW;
        if (maxH % 2)
            --maxH;
        return {maxW, maxH, targetFps};
    }

    bool concatReencodeFilter(const std::vector<fs::path> &parts, const fs::path &folder,
                              NoteCb note, ProgressCb prog)
    {
        auto [w, h, fps] = chooseTargetGeometry(parts, folder);
        int n = (int)parts.size();

        // Build filter_complex
        std::string filt;
        for (int i = 0; i < n; ++i)
        {
            filt += "[" + std::to_string(i) + ":v:0]setpts=PTS-STARTPTS,fps=" + std::to_string(fps) +
                    ",scale=w=min(iw\\," + std::to_string(w) + "):h=min(ih\\," + std::to_string(h) +
                    "):force_original_aspect_ratio=decrease:flags=bicubic,setsar=1,pad=" +
                    std::to_string(w) + ":" + std::to_string(h) + ":(ow-iw)/2:(oh-ih)/2:color=black,format=yuv420p[v" +
                    std::to_string(i) + "];";
            filt += "[" + std::to_string(i) + ":a:0]asetpts=PTS-STARTPTS,aresample=async=1:first_pts=0[a" +
                    std::to_string(i) + "];";
        }
        std::string vIn, aIn;
        for (int i = 0; i < n; ++i)
        {
            vIn += "[v" + std::to_string(i) + "]";
            aIn += "[a" + std::to_string(i) + "]";
        }
        filt += vIn + aIn + "concat=n=" + std::to_string(n) + ":v=1:a=1[v][a]";

        auto encoders = encoderArgsetsFast();
        std::vector<std::string> errs;
        bool cudaMode = (g_encoderMode.load(std::memory_order_relaxed) == EncoderMode::CUDA);
        bool hasCuda = cudaMode && hasCudaHwaccel();

        for (auto &enc : encoders)
        {
            bool isNvenc = (enc.label.find("nvenc") != std::string::npos);
            int passes = (hasCuda && isNvenc) ? 2 : 1;
            for (int pass = 0; pass < passes; ++pass)
            {
                bool useCudaDec = (pass == 0 && hasCuda && isNvenc);
                try
                {
                    if (note)
                        note("concat reencode [" + enc.label + (useCudaDec ? " +CUDA" : "") + "] " + std::to_string(w) + "x" + std::to_string(h));
                    auto tmp = folder / "0~merge.mkv";
                    std::vector<std::string> cmd = {g_ffmpeg, "-y"};
                    for (auto &p : parts)
                    {
                        if (useCudaDec)
                            cmd.insert(cmd.end(), {"-hwaccel", "cuda"});
                        cmd.insert(cmd.end(), {"-i", "./" + p.filename().string()});
                    }
                    cmd.insert(cmd.end(), {"-filter_complex", filt, "-map", "[v]", "-map", "[a]"});
                    cmd.insert(cmd.end(), enc.args.begin(), enc.args.end());
                    cmd.insert(cmd.end(), {"-vsync", "cfr", "-r", std::to_string(fps),
                                           "-pix_fmt", "yuv420p", "-c:a", aacEncoderName(), "-b:a", "160k",
                                           "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                                           "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0",
                                           "./" + tmp.filename().string()});
                    runFfmpeg(cmd, folder, "concat reencode [" + enc.label + "]", prog, 0, note);
                    return true;
                }
                catch (const PipelineError &e)
                {
                    std::string tag = "[" + enc.label + (useCudaDec ? " +CUDA" : "") + "]";
                    errs.push_back(tag + " " + e.detail.substr(0, 500));
                    if (note && useCudaDec && passes > 1)
                        note("CUDA decode failed — retrying " + enc.label + " with CPU decode");
                }
            }
        }
        throw PipelineError(folder, "concat-reencode", "All encoders failed");
    }

    // ── Work folder discovery ───────────────────────────────────────────────────

    static bool dirHasMedia(const fs::path &d)
    {
        try
        {
            for (auto &e : fs::directory_iterator(d))
            {
                if (!e.is_regular_file())
                    continue;
                auto name = e.path().filename().string();
                auto ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if ((name.ends_with(".tmp.ts") || name.ends_with(".tmp.mp4")) ||
                    (isMediaExt(ext) && name != "0.mkv") ||
                    (name == "0.mkv"))
                    return true;
            }
        }
        catch (...)
        {
        }
        return false;
    }

    std::vector<fs::path> findWorkFolders(const fs::path &root)
    {
        std::vector<fs::path> folders;
        if (dirHasMedia(root))
            folders.push_back(root);
        try
        {
            for (auto &e : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied))
            {
                if (e.is_directory() && e.path() != root && dirHasMedia(e.path()))
                    folders.push_back(e.path());
            }
        }
        catch (...)
        {
        }
        // Deduplicate
        std::set<fs::path> seen;
        std::vector<fs::path> out;
        for (auto &f : folders)
        {
            if (seen.insert(f).second)
                out.push_back(f);
        }
        return out;
    }

    // ── Temp cleanup ────────────────────────────────────────────────────────────

    void purgeTemp(const fs::path &folder)
    {
        const char *junk[] = {"concat_list.txt", "concat_list_norm.txt", ".concat_native_abs.txt", "0~merge.mp4", "0~merge.mkv", ".stage.mkv", ".merge2.mkv"};
        for (auto &f : junk)
        {
            std::error_code ec;
            fs::remove(folder / f, ec);
        }
        try
        {
            for (auto &e : fs::directory_iterator(folder))
            {
                if (e.path().extension() == ".mkv" && e.path().filename().string().ends_with(".norm.mkv"))
                {
                    std::error_code ec;
                    fs::remove(e.path(), ec);
                }
            }
        }
        catch (...)
        {
        }
    }

    // ── Symlink creation ────────────────────────────────────────────────────────

    static std::atomic<bool> g_purgedToProcess{false};

    void purgeToProcessDir()
    {
        try
        {
            if (fs::exists(TO_PROCESS))
            {
                fs::remove_all(TO_PROCESS);
            }
            fs::create_directories(TO_PROCESS);
        }
        catch (...)
        {
        }
    }

    void resetToProcessPurge()
    {
        g_purgedToProcess.store(false);
    }

    void makeSymlink(const fs::path &folder, const nlohmann::json &cfg)
    {
        // Wipe To Process once per run (atomic CAS — thread-safe, no UB)
        bool expected = false;
        if (g_purgedToProcess.compare_exchange_strong(expected, true))
            purgeToProcessDir();
        try
        {
            auto src = folder / "0.mkv";
            if (!fs::exists(src))
                return;

            // Find model root (folder with [ ] in name)
            fs::path modelRoot = folder;
            for (auto p = folder; p.has_parent_path() && p != p.root_path(); p = p.parent_path())
            {
                auto name = p.filename().string();
                if (name.find('[') != std::string::npos && name.find(']') != std::string::npos)
                {
                    modelRoot = p;
                    break;
                }
            }

            // Extract model name and clean tags/aliases
            std::string rawModelName = modelRoot.filename().string();
            std::string cleanedModelName = rawModelName;

            // Remove [tags] for the model folder name
            size_t tagStart = cleanedModelName.find('[');
            if (tagStart != std::string::npos)
            {
                size_t tagEnd = cleanedModelName.find(']', tagStart);
                if (tagEnd != std::string::npos)
                {
                    cleanedModelName.erase(tagStart, tagEnd - tagStart + 1);
                }
            }

            // Trim trailing spaces
            while (!cleanedModelName.empty() && std::isspace(cleanedModelName.back()))
            {
                cleanedModelName.pop_back();
            }

            // Get VR/Desktop tokens
            std::vector<std::string> vrTokens, dtTokens;
            if (cfg.contains("VR") && cfg["VR"].is_array())
                for (auto &v : cfg["VR"])
                {
                    auto s = v.get<std::string>();
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    vrTokens.push_back(s);
                }
            if (cfg.contains("Desktop") && cfg["Desktop"].is_array())
                for (auto &v : cfg["Desktop"])
                {
                    auto s = v.get<std::string>();
                    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
                    dtTokens.push_back(s);
                }

            auto nameLower = rawModelName;
            std::string lo = nameLower;
            std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);

            // Remove VR and Desktop tags from cleanedModelName
            for (const auto &tag : vrTokens)
            {
                size_t pos = 0;
                std::string cleanedLower = cleanedModelName;
                std::transform(cleanedLower.begin(), cleanedLower.end(), cleanedLower.begin(), ::tolower);
                while ((pos = cleanedLower.find(tag, pos)) != std::string::npos)
                {
                    cleanedModelName.erase(pos, tag.length());
                    cleanedLower.erase(pos, tag.length());
                }
            }
            for (const auto &tag : dtTokens)
            {
                size_t pos = 0;
                std::string cleanedLower = cleanedModelName;
                std::transform(cleanedLower.begin(), cleanedLower.end(), cleanedLower.begin(), ::tolower);
                while ((pos = cleanedLower.find(tag, pos)) != std::string::npos)
                {
                    cleanedModelName.erase(pos, tag.length());
                    cleanedLower.erase(pos, tag.length());
                }
            }

            // Trim again after removing tags
            while (!cleanedModelName.empty() && std::isspace(cleanedModelName.back()))
            {
                cleanedModelName.pop_back();
            }

            // Replace aliases
            if (cfg.contains("Aliases") && cfg["Aliases"].is_object())
            {
                for (auto it = cfg["Aliases"].begin(); it != cfg["Aliases"].end(); ++it)
                {
                    std::string targetModel = it.key();
                    if (it.value().is_array())
                    {
                        for (auto &aliasVal : it.value())
                        {
                            std::string alias = aliasVal.get<std::string>();
                            if (cleanedModelName.find(alias) != std::string::npos)
                            {
                                cleanedModelName = targetModel;
                                break;
                            }
                        }
                    }
                }
            }

            std::string tagType = "UNKNOWN";
            for (auto &t : vrTokens)
                if (lo.find(t) != std::string::npos)
                {
                    tagType = "VR";
                    break;
                }
            if (tagType == "UNKNOWN")
                for (auto &t : dtTokens)
                    if (lo.find(t) != std::string::npos)
                    {
                        tagType = "NO VR";
                        break;
                    }

            // Detect if source folder is inside a "Mobile" subfolder
            bool isMobile = false;
            for (auto p = folder; p != modelRoot && p.has_parent_path() && p != p.root_path(); p = p.parent_path())
            {
                std::string dirName = p.filename().string();
                std::string dirLower = dirName;
                std::transform(dirLower.begin(), dirLower.end(), dirLower.begin(), ::tolower);
                if (dirLower == "mobile")
                {
                    isMobile = true;
                    break;
                }
            }

            // Build destination: To Process / CleanModel / VR|NO VR / [Mobile/]
            fs::path dstDir = TO_PROCESS / cleanedModelName / tagType;
            if (isMobile)
                dstDir = dstDir / "Mobile";
            fs::create_directories(dstDir);

            // Extract site from [brackets]
            std::string site;
            auto lb = rawModelName.find('[');
            auto rb = rawModelName.find(']', lb + 1);
            if (lb != std::string::npos && rb != std::string::npos)
                site = rawModelName.substr(lb + 1, rb - lb - 1);

            auto dst = dstDir / "0.mkv";
            int attempt = 0;
            while (fs::exists(dst))
            {
                std::error_code ec_eq;
                if (fs::equivalent(src, dst, ec_eq))
                    break;

                ++attempt;
                std::string siteTag = site.empty() ? "" : (" [" + site + "]");
                std::string base = (attempt == 1)
                                       ? ("0" + siteTag + ".mkv")
                                       : ("0" + siteTag + " (" + std::to_string(attempt) + ").mkv");
                dst = dstDir / base;
            }

            if (!fs::exists(dst))
            {
                std::error_code ec;
                fs::create_symlink(src, dst, ec);
                if (ec)
                    fs::copy_file(src, dst, ec);
            }
        }
        catch (...)
        {
        }
    }

    // ── PTS repair (re-encode with CUDA + sequential frame timestamps) ────────

    bool repairTimestamps(const fs::path &folder, const fs::path &src, const fs::path &dst,
                          NoteCb note, GuiCb gui)
    {
        auto sig = probeSignature(src, folder);
        int fps = std::max((int)std::round(sig.vFps), 0);
        if (fps == 0)
            fps = DEFAULT_TARGET_FPS;
        bool hasAudio = sig.hasA;

        int64_t srcSize = 0;
        try
        {
            srcSize = (int64_t)fs::file_size(folder / src.filename());
        }
        catch (...)
        {
        }

        // Sequential frame numbering: frame N gets PTS = N/fps.
        // Every decoded frame gets a unique sequential timestamp — no freeze frames.
        // Combined with -vsync cfr -r fps this is safe: frames are already at the
        // right rate so cfr never needs to duplicate.
        bool fullRange = sig.vPix.rfind("yuvj", 0) == 0;
        std::string vf = "setpts=N/" + std::to_string(fps) + "/TB";
        if (fullRange)
            vf += ",scale=in_range=full:out_range=tv";
        vf += ",format=yuv420p";
        std::string af = "asetpts=N/SR/TB";

        bool cudaMode = (g_encoderMode.load(std::memory_order_relaxed) == EncoderMode::CUDA);
        bool cuda = cudaMode && hasCudaHwaccel();

        double srcDur = std::max(ffprobeBestDuration(src, folder), 1.0);
        ProgressCb prog = nullptr;
        if (gui)
        {
            prog = [&, srcDur](double outTime, int64_t written, int64_t target)
            {
                float pct = 100.0f * std::min(1.0f, (float)(outTime / srcDur));
                int64_t tgt = target > 0 ? target : srcSize;
                gui(pct, 0, written, tgt);
            };
        }

        std::vector<std::string> errs;
        int hwFailCount = 0;
        constexpr int MAX_HW_FAILURES = 2; // bail to CPU after 2 HW encoder failures

        auto encoders = encoderArgsetsFast();
        for (auto &enc : encoders)
        {
            // ── Short-circuit: after MAX_HW_FAILURES hardware failures, skip ──
            // Broken .ts files crash most HW decoders/encoders.  Don't waste
            // minutes trying every single one — jump to CPU fallback fast.
            bool isSoftware = (enc.label.find("libx264") != std::string::npos ||
                               enc.label.find("libx265") != std::string::npos);
            if (!isSoftware && hwFailCount >= MAX_HW_FAILURES)
                continue;

            bool isNvenc = (enc.label.find("nvenc") != std::string::npos);

            // For NVENC encoders: try CUDA decode first, then CPU decode + NVENC.
            // Broken .ts files can crash the CUDA *decoder* while the NVENC *encoder* is fine.
            int passes = (cuda && isNvenc) ? 2 : 1;
            for (int pass = 0; pass < passes; ++pass)
            {
                bool useCudaDec = (pass == 0 && cuda && isNvenc);
                try
                {
                    if (note)
                        note("repair PTS [" + enc.label + (useCudaDec ? " +CUDA" : "") + "] ~" + std::to_string(fps) + "fps");

                    // Do NOT use huge -analyzeduration/-probesize here.
                    // These files have broken PTS — deep probing corrupt packets hangs/crashes FFmpeg
                    // before the encoder even starts, killing every encoder in the ladder.
                    std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                                    "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err"};
                    if (useCudaDec)
                        cmd.insert(cmd.end(), {"-hwaccel", "cuda"});
                    cmd.insert(cmd.end(), {"-i", "./" + src.filename().string()});

                    if (hasAudio)
                    {
                        cmd.insert(cmd.end(), {"-map", "0:v:0?", "-map", "0:a:0?"});
                    }
                    else
                    {
                        std::string durStr = std::to_string(std::max(ffprobeBestDuration(src, folder), 0.1));
                        cmd.insert(cmd.end(), {"-f", "lavfi", "-t", durStr, "-i",
                                               "anullsrc=r=" + std::to_string(TARGET_AUDIO_SR) + ":cl=stereo",
                                               "-map", "0:v:0?", "-map", "1:a:0"});
                    }

                    cmd.insert(cmd.end(), {"-vf", vf, "-af", af, "-vsync", "cfr", "-r", std::to_string(fps)});
                    cmd.insert(cmd.end(), enc.args.begin(), enc.args.end());
                    cmd.insert(cmd.end(), {"-c:a", aacEncoderName(), "-ar", std::to_string(TARGET_AUDIO_SR),
                                           "-ac", std::to_string(TARGET_AUDIO_CH)});
                    cmd.insert(cmd.end(), {"-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                                           "./" + dst.filename().string()});

                    runFfmpeg(cmd, folder, "repair PTS [" + enc.label + "]", prog, srcSize, note);
                    if (note)
                        note("repair PTS OK [" + enc.label + (useCudaDec ? " +CUDA" : "") + "]");
                    return true;
                }
                catch (const PipelineError &e)
                {
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    std::string tag = "[" + enc.label + (useCudaDec ? " +CUDA" : "") + "]";
                    errs.push_back(tag + " " + e.detail.substr(0, 300));
                    if (!isSoftware)
                        ++hwFailCount;
                    if (note)
                    {
                        if (useCudaDec && passes > 1)
                            note("CUDA decode failed — retrying " + enc.label + " with CPU decode");
                        else
                            note("repair PTS " + tag + " failed: " + e.detail.substr(0, 200));
                    }
                }
            }
        }

        // ── Fallback: try one bare NVENC command before CPU (only if we didn't already exhaust HW) ──
        if (hwFailCount < MAX_HW_FAILURES)
        {
            auto have = detectEncoders();
            const char *nvencFallbacks[] = {"h264_nvenc"}; // only try ONE bare fallback
            for (auto &encName : nvencFallbacks)
            {
                if (!have.count(encName) || !have[encName])
                    continue;
                try
                {
                    if (note)
                        note("repair PTS fallback [" + std::string(encName) + " bare]");
                    std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                                    "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err",
                                                    "-i", "./" + src.filename().string(),
                                                    "-map", "0:v:0?", "-map", "0:a:0?",
                                                    "-vf", vf,
                                                    "-af", af,
                                                    "-vsync", "cfr", "-r", std::to_string(fps),
                                                    "-c:v", encName, "-preset", "p4", "-rc", "vbr", "-cq", "22",
                                                    "-c:a", aacEncoderName(), "-ar", std::to_string(TARGET_AUDIO_SR),
                                                    "-ac", std::to_string(TARGET_AUDIO_CH),
                                                    "-avoid_negative_ts", "make_zero",
                                                    "./" + dst.filename().string()};
                    runFfmpeg(cmd, folder, "repair PTS [" + std::string(encName) + " bare]", prog, srcSize, note);
                    if (note)
                        note("repair PTS OK [" + std::string(encName) + " bare]");
                    return true;
                }
                catch (const PipelineError &e)
                {
                    std::error_code ec;
                    fs::remove(folder / dst.filename(), ec);
                    errs.push_back("[" + std::string(encName) + " bare] " + e.detail.substr(0, 300));
                    if (note)
                        note("repair PTS [" + std::string(encName) + " bare] failed: " + e.detail.substr(0, 200));
                }
            }
        }

        // CPU fallback: libx264 ultrafast — maximum speed
        try
        {
            if (note)
                note("repair PTS fallback [libx264_safe]");
            std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
                                            "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err",
                                            "-threads", "0",
                                            "-i", "./" + src.filename().string(),
                                            "-map", "0:v:0?", "-map", "0:a:0?",
                                            "-vf", vf,
                                            "-af", af,
                                            "-vsync", "cfr", "-r", std::to_string(fps),
                                            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
                                            "-c:a", "aac", "-ar", std::to_string(TARGET_AUDIO_SR),
                                            "-ac", std::to_string(TARGET_AUDIO_CH),
                                            "-avoid_negative_ts", "make_zero",
                                            "./" + dst.filename().string()};
            runFfmpeg(cmd, folder, "repair PTS [libx264_safe]", prog, srcSize, note);
            if (note)
                note("repair PTS OK [libx264_safe]");
            return true;
        }
        catch (const PipelineError &e)
        {
            errs.push_back("[libx264_safe] " + e.detail.substr(0, 300));
            if (note)
                note("repair PTS [libx264_safe] failed: " + e.detail.substr(0, 200));
        }

        // Absolute last resort: no filters at all
        try
        {
            if (note)
                note("repair PTS last-resort [bare]");
            std::vector<std::string> cmd = {g_ffmpeg, "-y", "-hide_banner",
                                            "-fflags", "+genpts+discardcorrupt", "-err_detect", "+ignore_err",
                                            "-threads", "0",
                                            "-i", "./" + src.filename().string(),
                                            "-c:v", "libx264", "-preset", "ultrafast", "-crf", "23",
                                            "-c:a", "aac", "-ar", std::to_string(TARGET_AUDIO_SR),
                                            "-ac", std::to_string(TARGET_AUDIO_CH),
                                            "./" + dst.filename().string()};
            runFfmpeg(cmd, folder, "repair PTS [bare]", prog, srcSize, note);
            if (note)
                note("repair PTS OK [bare fallback]");
            return true;
        }
        catch (const PipelineError &e)
        {
            errs.push_back("[bare] " + e.detail.substr(0, 300));
            if (note)
            {
                std::string all;
                for (auto &s : errs)
                    all += "  " + s + "\n";
                note("repair PTS FAILED — all encoders failed:\n" + all);
            }
        }
        return false;
    }

    // ── Smart PTS-aware .ts converter ───────────────────────────────────────────
    //  - Checks eligibility (real MPEG-TS, big enough to warrant scan)
    //  - Probes PTS gaps — if clean, returns false (let normal path handle it)
    //  - If broken: re-encodes .ts → mkvOut with CUDA + N/fps/TB, returns true

    bool fixTsPts(const fs::path &folder, const fs::path &tsFile, const fs::path &mkvOut,
                  NoteCb note, GuiCb gui)
    {
        auto full = folder / tsFile.filename();
        if (!fs::exists(full))
            return false;

        // ── Eligibility gate ────────────────────────────────────────────────
        // Only real MPEG-TS video files need PTS scanning
        if (!isMpegTsVideo(full))
            return false;

        // Skip PTS probe on tiny files — fast enough to just streamcopy
        int64_t fileSize = 0;
        try
        {
            fileSize = (int64_t)fs::file_size(full);
        }
        catch (...)
        {
        }
        if (fileSize < PTS_CHECK_MIN_BYTES)
            return false;

        // ── Probe PTS ───────────────────────────────────────────────────────
        if (note)
            note("scanning PTS: " + tsFile.filename().string() + " (" + humanBytes(fileSize) + ")");

        double maxJump = detectMaxPtsJump(full, folder);
        if (maxJump <= PTS_JUMP_THRESHOLD_SEC)
        {
            if (note)
                note("PTS clean (max gap " + humanSecs(maxJump) + ") — normal path");
            return false; // clean — let caller do regular streamcopy
        }

        // ── Broken PTS — re-encode .ts → .mkv with sequential timestamps ───
        if (note)
            note("PTS BROKEN (max gap " + humanSecs(maxJump) + ") in " + tsFile.filename().string() + " -> re-encode to " + mkvOut.filename().string());

        if (repairTimestamps(folder, tsFile.filename(), mkvOut.filename(), note, gui))
        {
            if (note)
                note("PTS repair OK: " + mkvOut.filename().string());
            return true; // we handled the conversion
        }

        if (note)
            note("PTS repair FAILED for " + tsFile.filename().string() + " — falling back to normal path");
        std::error_code ec;
        fs::remove(folder / mkvOut.filename(), ec);
        return false; // repair failed — let normal path try
    }

    // repairPtsIfNeeded removed — .ts files are fixed at source by fixTsPts.
    // The merged 0.mkv is NEVER re-encoded post-hoc.

    // ── Main merge pipeline ─────────────────────────────────────────────────────

    void mergeFolder(const fs::path &folder, GuiCb gui, const nlohmann::json &cfg,
                     bool mkLinks, NoteCb note, MetricCb metric,
                     bool fixPts)
    {
        purgeTemp(folder);
        bool conversionFailed = false;

        // Phase 1: Convert .tmp.ts / .tmp.mp4 to .mkv
        for (auto &pattern : {"*.tmp.ts", "*.tmp.mp4"})
        {
            // Collect matching files first (avoid modifying dir while iterating)
            std::vector<fs::path> tmps;
            try
            {
                for (auto &e : fs::directory_iterator(folder))
                {
                    auto name = e.path().filename().string();
                    bool match = (std::string(pattern) == "*.tmp.ts") ? name.ends_with(".tmp.ts") : name.ends_with(".tmp.mp4");
                    if (e.is_regular_file() && match)
                        tmps.push_back(e.path());
                }
            }
            catch (...)
            {
            }

            for (size_t idx = 0; idx < tmps.size(); ++idx)
            {
                auto &t = tmps[idx];
                try
                {
                    if (note)
                        note("convert [" + std::to_string(idx + 1) + "/" + std::to_string(tmps.size()) + "] " + t.filename().string());
                    auto stem = t.stem().string();
                    // Remove .tmp from stem (e.g. "file.tmp" -> "file")
                    if (stem.size() > 4 && stem.substr(stem.size() - 4) == ".tmp")
                        stem = stem.substr(0, stem.size() - 4);
                    auto clean = folder / (stem + ".mkv");

                    bool isTs = (t.extension() == ".ts" || t.filename().string().ends_with(".tmp.ts"));

                    if (isTs && fixPts)
                    {
                        // Smart pre-process: if PTS is broken, re-encode .ts → .mkv directly
                        if (fixTsPts(folder, t.filename(), clean.filename(), note, gui))
                        {
                            // fixTsPts handled the conversion — remove source and move on
                            std::error_code ec;
                            fs::remove(t, ec);
                        }
                        else
                        {
                            // PTS is clean (or file too small to scan) — normal streamcopy path
                            try
                            {
                                tsStreamcopyPostprocess(folder, t.filename(), clean.filename(), note, gui);
                                auto vr = validateAgainstSource(folder, t.filename(), clean.filename(), true, true);
                                if (!vr.ok)
                                {
                                    auto [ok2, met2] = runPassesWithValidate(folder, t.filename(), clean.filename(), true, note, metric, "convert", gui);
                                    if (!ok2)
                                    {
                                        double srcDur = met2.srcDur > 0 ? met2.srcDur : ffprobeBestDuration(t.filename(), folder);
                                        double outDur = met2.outDur > 0 ? met2.outDur : (fs::exists(clean) ? ffprobeBestDuration(clean.filename(), folder) : 0.0);
                                        bool needSalvage = (srcDur > 0 && (srcDur - outDur) >= SALVAGE_MIN_DELTA_SEC && outDur < srcDur * SALVAGE_TRIG_RATIO) || shouldSalvage(met2, true);
                                        if (needSalvage)
                                        {
                                            if (note)
                                                note("convert short -> salvage");
                                            std::error_code ec;
                                            fs::remove(clean, ec);
                                            salvageReencode(folder, t.filename(), clean.filename(), true, note, metric, "convert-salvage", gui);
                                        }
                                        else
                                        {
                                            if (note)
                                                note("convert FAILED [" + t.filename().string() + "]");
                                            std::error_code ec;
                                            fs::remove(clean, ec);
                                            // Auto-delete weak/failed .ts under threshold (only if confirmed MPEG-TS video)
                                            int64_t tsSize = 0;
                                            try
                                            {
                                                tsSize = (int64_t)fs::file_size(t);
                                            }
                                            catch (...)
                                            {
                                            }
                                            if (!isMpegTsVideo(t))
                                            {
                                                if (note)
                                                    note("SKIP non-video .ts (not MPEG-TS): " + t.filename().string());
                                            }
                                            else if (tsSize > 0 && tsSize < FAILED_TS_DELETE_MAX_BYTES)
                                            {
                                                fs::remove(t, ec);
                                                if (note)
                                                    note("deleted failed .ts (" + humanBytes(tsSize) + " < 100 MiB): " + t.filename().string());
                                            }
                                            else if (note)
                                            {
                                                note("keeping failed .ts (" + humanBytes(tsSize) + " >= 100 MiB): " + t.filename().string());
                                            }
                                            // Don't mark as failed — file removed from list, continue with rest
                                            continue;
                                        }
                                    }
                                }
                                std::error_code ec;
                                fs::remove(t, ec);
                            }
                            catch (const std::exception &ex)
                            {
                                if (g_stopRequested.load())
                                    throw; // don't swallow stop
                                if (note)
                                    note(std::string("convert ERROR: ") + ex.what());
                                std::error_code ec;
                                fs::remove(clean, ec);
                                // Auto-delete weak/failed .ts under threshold (only if confirmed MPEG-TS video)
                                int64_t tsSize = 0;
                                try
                                {
                                    tsSize = (int64_t)fs::file_size(t);
                                }
                                catch (...)
                                {
                                }
                                if (!isMpegTsVideo(t))
                                {
                                    if (note)
                                        note("SKIP non-video .ts (not MPEG-TS): " + t.filename().string());
                                }
                                else if (tsSize > 0 && tsSize < FAILED_TS_DELETE_MAX_BYTES)
                                {
                                    fs::remove(t, ec);
                                    if (note)
                                        note("deleted failed .ts (" + humanBytes(tsSize) + " < 100 MiB): " + t.filename().string());
                                }
                                // Don't mark as failed — file removed from list, continue with rest
                                continue;
                            }
                        } // end else (normal path)
                    }
                    else
                    {
                        auto [ok, met] = runPassesWithValidate(folder, t.filename(), clean.filename(), false, note, metric, "convert", gui);
                        if (!ok)
                        {
                            double srcDur = met.srcDur > 0 ? met.srcDur : ffprobeBestDuration(t.filename(), folder);
                            double outDur = met.outDur > 0 ? met.outDur : (fs::exists(clean) ? ffprobeBestDuration(clean.filename(), folder) : 0.0);
                            bool needSalvage = (srcDur > 0 && (srcDur - outDur) >= SALVAGE_MIN_DELTA_SEC && outDur < srcDur * SALVAGE_TRIG_RATIO) || shouldSalvage(met, false);
                            if (needSalvage)
                            {
                                if (note)
                                    note("convert short -> salvage");
                                std::error_code ec;
                                fs::remove(clean, ec);
                                salvageReencode(folder, t.filename(), clean.filename(), false, note, metric, "convert-salvage", gui);
                            }
                            else
                            {
                                throw PipelineError(folder, "convert-validate", "copy-remux failed and salvage not triggered");
                            }
                        }
                        std::error_code ec;
                        fs::remove(t, ec);
                    }
                }
                catch (const std::exception &ex)
                {
                    if (g_stopRequested.load())
                        throw; // don't swallow stop
                    if (note)
                        note(std::string("conversion aborted: ") + ex.what());
                    // Auto-delete small failed .ts/.tmp.ts under threshold (only if confirmed MPEG-TS video)
                    int64_t tsSize = 0;
                    try
                    {
                        tsSize = (int64_t)fs::file_size(t);
                    }
                    catch (...)
                    {
                    }
                    auto tname = t.filename().string();
                    bool isTsFile = tname.ends_with(".ts") || tname.ends_with(".tmp.ts");
                    if (isTsFile && !isMpegTsVideo(t))
                    {
                        if (note)
                            note("SKIP non-video .ts (not MPEG-TS): " + tname);
                    }
                    else if (isTsFile && tsSize > 0 && tsSize < FAILED_TS_DELETE_MAX_BYTES)
                    {
                        std::error_code ec;
                        fs::remove(t, ec);
                        if (note)
                            note("deleted failed .ts (" + humanBytes(tsSize) + " < 100 MiB): " + tname);
                    }
                    // Don't mark as failed — file removed from list, continue with rest
                }
            }
        }

        if (conversionFailed)
        {
            if (gui)
                gui(100, 0, 0, 0);
            throw PipelineError(folder, "conversion", "One or more files failed to convert");
        }

        // Phase 2: Collect media, remux TS → MKV, concat
        auto mergedMkv = folder / "0.mkv";
        std::error_code ec2;
        fs::remove(folder / "0.mp4", ec2);

        std::vector<fs::path> media;
        try
        {
            for (auto &e : fs::directory_iterator(folder))
            {
                if (!e.is_regular_file())
                    continue;
                auto ext = e.path().extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (isMediaExt(ext) && e.path().filename() != "0.mkv")
                    media.push_back(e.path());
            }
        }
        catch (...)
        {
        }

        // Sort naturally
        std::sort(media.begin(), media.end(), [](const fs::path &a, const fs::path &b)
                  { return natLess(a.filename().string(), b.filename().string()); });

        // Filter readable
        std::vector<fs::path> good;
        for (auto &f : media)
        {
            if (ffprobeOk(f, folder))
                good.push_back(f);
            else
            {
                auto ext = f.extension().string();
                std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                int64_t fSize = 0;
                try
                {
                    fSize = (int64_t)fs::file_size(f);
                }
                catch (...)
                {
                }
                if (ext == ".ts" && !isMpegTsVideo(f))
                {
                    // Not an MPEG-TS video file (e.g. TypeScript) — never delete, just skip
                    if (note)
                        note("SKIP non-video .ts (not MPEG-TS): " + f.filename().string());
                }
                else if ((ext == ".ts") && fSize > 0 && fSize < FAILED_TS_DELETE_MAX_BYTES)
                {
                    if (note)
                        note("delete unreadable .ts (" + humanBytes(fSize) + "): " + f.filename().string());
                    std::error_code ec;
                    fs::remove(f, ec);
                }
                else
                {
                    if (note)
                        note("delete unreadable " + f.filename().string() + " (" + humanBytes(fSize) + ")");
                    std::error_code ec;
                    fs::remove(f, ec);
                }
            }
        }

        if (!fs::exists(mergedMkv) && good.empty())
        {
            if (note)
                note("nothing to do");
            if (gui)
                gui(100, 0, 0, 0);
            return;
        }

        if (fs::exists(mergedMkv) && good.empty())
        {
            if (note)
                note("only symlink pass");
            if (mkLinks)
                makeSymlink(folder, cfg);
            if (gui)
            {
                auto sz = (int64_t)fs::file_size(mergedMkv);
                gui(100, 0, sz, sz);
            }
            return;
        }

        // Single file shortcut
        if (!fs::exists(mergedMkv) && good.size() == 1)
        {
            auto only = good[0];
            auto ext = only.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

            if (ext == ".ts")
            {
                auto rem = only;
                rem.replace_extension(".remux.mkv");

                // Smart pre-process: if PTS is broken, re-encode directly
                if (fixPts && fixTsPts(folder, only.filename(), rem.filename(), note, gui))
                {
                    if (note)
                        note("single .ts PTS-repaired -> finalizing");
                }
                else
                {
                    // PTS is clean — normal streamcopy path
                    tsStreamcopyPostprocess(folder, only.filename(), rem.filename(), note, gui);
                    auto vr = validateAgainstSource(folder, only.filename(), rem.filename(), true, true);
                    if (!vr.ok)
                    {
                        auto [ok2, met2] = runPassesWithValidate(folder, only.filename(), rem.filename(), true, note, metric, "remux", gui);
                        if (!ok2)
                        {
                            double srcDur = met2.srcDur > 0 ? met2.srcDur : ffprobeBestDuration(only.filename(), folder);
                            double outDur = met2.outDur > 0 ? met2.outDur : (fs::exists(rem) ? ffprobeBestDuration(rem.filename(), folder) : 0.0);
                            bool needSalvage = (srcDur > 0 && (srcDur - outDur) >= SALVAGE_MIN_DELTA_SEC && outDur < srcDur * SALVAGE_TRIG_RATIO) || shouldSalvage(met2, true);
                            if (needSalvage)
                            {
                                if (note)
                                    note("single remux short -> salvage");
                                std::error_code ec;
                                fs::remove(rem, ec);
                                salvageReencode(folder, only.filename(), rem.filename(), true, note, metric, "remux-salvage", gui);
                            }
                            else
                            {
                                throw PipelineError(folder, "single-remux", "copy-remux failed and salvage not triggered");
                            }
                        }
                    }
                }
                fs::rename(rem, mergedMkv);
                // fixTsPts already handled PTS repair on the .ts — no post-merge repair needed
                if (mkLinks)
                    makeSymlink(folder, cfg);
                if (DELETE_TS_AFTER_REMUX)
                {
                    std::error_code ec;
                    fs::remove(only, ec);
                }
                return;
            }
            else
            {
                if (ext == ".mkv")
                {
                    fs::rename(only, mergedMkv);
                }
                else
                {
                    std::vector<std::string> cmd = {g_ffmpeg, "-y", "-i", "./" + only.filename().string(),
                                                    "-map", "0", "-c", "copy", "-copyinkf",
                                                    "-avoid_negative_ts", "make_zero", "-reset_timestamps", "1",
                                                    "-muxpreload", "0", "-muxdelay", "0", "-max_interleave_delta", "0", "./0.mkv"};
                    runFfmpeg(cmd, folder, "finalize -> MKV", nullptr, 0, note);
                    std::error_code ec;
                    fs::remove(only, ec);
                }
                if (mkLinks)
                    makeSymlink(folder, cfg);
                return;
            }
        }

        // Phase 3: Remux all TS parts
        int tsCount = 0, tsTotal = 0;
        for (auto &p : good)
        {
            auto ext2 = p.extension().string();
            std::transform(ext2.begin(), ext2.end(), ext2.begin(), ::tolower);
            if (ext2 == ".ts")
                ++tsTotal;
        }
        if (note && tsTotal > 0)
            note("remux " + std::to_string(tsTotal) + " TS part" + (tsTotal > 1 ? "s" : "") + " -> MKV (" + std::to_string(good.size()) + " total files)");
        else if (note)
            note("no TS parts to remux (" + std::to_string(good.size()) + " files ready)");

        std::vector<fs::path> parts;
        for (auto &p : good)
        {
            auto ext = p.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext != ".ts")
            {
                parts.push_back(p);
                continue;
            }

            ++tsCount;
            auto rem = p;
            rem.replace_extension(".remux.mkv");

            if (note)
                note("remux TS [" + std::to_string(tsCount) + "/" + std::to_string(tsTotal) + "] " + p.filename().string());

            // Smart pre-process: if PTS is broken, re-encode directly
            if (fixPts && fixTsPts(folder, p.filename(), rem.filename(), note, gui))
            {
                // fixTsPts handled the conversion
            }
            else
            {
                // PTS is clean — normal streamcopy path
                tsStreamcopyPostprocess(folder, p.filename(), rem.filename(), note, gui);
                auto vr = validateAgainstSource(folder, p.filename(), rem.filename(), true, true);
                if (!vr.ok)
                {
                    auto [ok2, met2] = runPassesWithValidate(folder, p.filename(), rem.filename(), true, note, metric, "remux", gui);
                    if (!ok2)
                    {
                        double srcDur = met2.srcDur > 0 ? met2.srcDur : ffprobeBestDuration(p.filename(), folder);
                        double outDur = met2.outDur > 0 ? met2.outDur : (fs::exists(rem) ? ffprobeBestDuration(rem.filename(), folder) : 0.0);
                        bool needSalvage = (srcDur > 0 && (srcDur - outDur) >= SALVAGE_MIN_DELTA_SEC && outDur < srcDur * SALVAGE_TRIG_RATIO) || shouldSalvage(met2, true);
                        if (needSalvage)
                        {
                            if (note)
                                note("remux short -> salvage");
                            std::error_code ec;
                            fs::remove(rem, ec);
                            salvageReencode(folder, p.filename(), rem.filename(), true, note, metric, "remux-salvage", gui);
                        }
                        else
                        {
                            throw PipelineError(folder, "remux-validate", "copy-remux failed and salvage not triggered");
                        }
                    }
                }
            }
            if (DELETE_TS_AFTER_REMUX)
            {
                std::error_code ec;
                fs::remove(p, ec);
            }
            parts.push_back(rem);
        }

        // Phase 4: Concat
        auto concatTxt = writeConcat(folder, parts, fs::exists(mergedMkv));
        if (note)
            note("concat " + std::to_string(parts.size()) + " parts -> MKV");

        double totalDur = (fs::exists(mergedMkv) ? ffprobeBestDuration(mergedMkv, folder) : 0.0);
        int64_t targetBytes = (fs::exists(mergedMkv) ? (int64_t)fs::file_size(mergedMkv) : 0);
        for (auto &p : parts)
        {
            totalDur += ffprobeBestDuration(p, folder);
            try
            {
                targetBytes += (int64_t)fs::file_size(p);
            }
            catch (...)
            {
            }
        }
        if (metric)
            metric("total", targetBytes, 0, totalDur, 0,
                   std::to_string(parts.size()) + " parts");
        if (gui)
            gui(0, 0, 0, targetBytes);

        auto startTime = std::chrono::steady_clock::now();
        int64_t lastWritten = 0;

        ProgressCb prog = [&](double sec, int64_t bw, int64_t tgt)
        {
            lastWritten = bw > 0 ? bw : lastWritten;
            if (!gui)
                return;

            // Dual progress: time-based and bytes-based, use whichever is better
            float pctTime = (totalDur > 0 && sec > 0) ? (float)(sec / totalDur * 100.0) : 0.0f;
            float pctBytes = (targetBytes > 0 && lastWritten > 0) ? (float)((double)lastWritten / targetBytes * 100.0) : 0.0f;
            float pct = std::max(pctTime, pctBytes);
            pct = std::clamp(pct, 0.0f, 99.9f); // Don't show 100% until commit phase

            auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();
            float eta = 0;
            if (pctTime > 1.0f && sec > 0)
            {
                // Time-based ETA
                double speed = (elapsed > 0) ? sec / elapsed : 0;
                eta = (speed > 0) ? (float)((totalDur - sec) / speed) : 0;
            }
            else if (pctBytes > 1.0f && lastWritten > 0)
            {
                // Bytes-based ETA fallback
                double bps = (elapsed > 0) ? lastWritten / elapsed : 0;
                eta = (bps > 0) ? (float)((targetBytes - lastWritten) / bps) : 0;
            }

            gui(pct, eta, lastWritten, tgt > 0 ? tgt : targetBytes);
        };

        std::vector<fs::path> usedFiles;
        try
        {
            concatCopyMkv(folder, concatTxt, prog, targetBytes, note);
            usedFiles = parts;
        }
        catch (const StopRequested &)
        {
            throw; // propagate stop immediately — never fall into re-encode
        }
        catch (const PipelineError &)
        {
            if (g_stopRequested.load())
                throw StopRequested();
            if (note)
                note("concat copy failed -> normalize");
            auto norm = normalizeParts(parts, folder, note);
            auto concatTxt2 = writeConcat(folder, norm, fs::exists(mergedMkv), "concat_list_norm.txt");
            try
            {
                concatCopyMkv(folder, concatTxt2, prog, targetBytes, note, "concat copy (normalized) -> MKV");
                usedFiles.insert(usedFiles.end(), norm.begin(), norm.end());
                usedFiles.insert(usedFiles.end(), parts.begin(), parts.end());
            }
            catch (const StopRequested &)
            {
                throw; // propagate stop immediately
            }
            catch (const PipelineError &)
            {
                if (g_stopRequested.load())
                    throw StopRequested();
                concatReencodeFilter(norm, folder, note, prog);
                usedFiles.insert(usedFiles.end(), norm.begin(), norm.end());
                usedFiles.insert(usedFiles.end(), parts.begin(), parts.end());
            }
        }

        // Phase 5: Validate merged output (fast check only)
        auto tmp = folder / "0~merge.mkv";
        if (note)
            note("validating merged output...");

        // Quick size-based validation - skip expensive duration probing
        int64_t outSize = 0;
        try
        {
            outSize = (int64_t)fs::file_size(tmp);
        }
        catch (...)
        {
        }

        // If output file exists and has reasonable size (>80% of expected), accept it
        bool quickValid = (outSize > 0) && (targetBytes <= 0 || outSize >= targetBytes * 0.80);

        if (!quickValid && fs::exists(tmp))
        {
            // Only do full validation if quick check failed
            auto vr = validateMerge(folder, tmp, targetBytes, totalDur, true);
            quickValid = vr.ok;
        }

        if (!quickValid && fs::exists(tmp))
        {
            // Last resort: accept if file exists and is non-trivial
            quickValid = (outSize > 1024 * 1024); // >1MB
            if (quickValid && note)
                note("accepting merge output (size-based validation)");
        }

        if (!quickValid)
        {
            auto tmp2 = folder / ".merge2.mkv";
            try
            {
                mkvStreamcopyPostprocess(folder, tmp.filename(), tmp2.filename(), note, gui);
                auto vr2 = validateMerge(folder, tmp2, targetBytes, totalDur, true);
                if (!vr2.ok)
                {
                    auto vr3 = validateMerge(folder, tmp2, targetBytes, totalDur, true, true);
                    if (!vr3.ok)
                    {
                        std::error_code ec;
                        fs::remove(tmp, ec);
                        fs::remove(tmp2, ec);
                        fs::remove(folder / "concat_list.txt", ec);
                        fs::remove(folder / "concat_list_norm.txt", ec);
                        throw PipelineError(folder, "merge-validate", vr3.msg.empty() ? vr2.msg : vr3.msg);
                    }
                }
                fs::rename(tmp2, tmp);
            }
            catch (const PipelineError &)
            {
                std::error_code ec;
                for (auto &j : {"0~merge.mkv", ".merge2.mkv", "concat_list.txt", "concat_list_norm.txt"})
                    fs::remove(folder / j, ec);
                throw;
            }
        }

        // Phase 6: Commit
        if (note)
            note("commit merged MKV");
        {
            std::error_code ec;
            fs::remove(mergedMkv, ec);
        }
        fs::rename(tmp, mergedMkv);

        // Cleanup
        std::error_code ec;
        for (auto &j : {"concat_list.txt", "concat_list_norm.txt", ".merge2.mkv"})
            fs::remove(folder / j, ec);
        try
        {
            for (auto &e : fs::directory_iterator(folder))
                if (e.path().filename().string().ends_with(".norm.mkv"))
                    fs::remove(e.path(), ec);
        }
        catch (...)
        {
        }

        for (auto &f : usedFiles)
        {
            try
            {
                if (fs::exists(f) && fs::is_regular_file(f) && f.filename() != "0.mkv")
                    fs::remove(f, ec);
            }
            catch (...)
            {
            }
        }

        // Phase 7: finalize
        // Individual .ts parts were already PTS-fixed by fixTsPts before concat.
        // Concat segment boundaries naturally have PTS jumps — that's expected, not corruption.

        if (mkLinks)
            makeSymlink(folder, cfg);
    }

} // namespace sh
