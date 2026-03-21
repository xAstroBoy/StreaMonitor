// StripHelper C++ — Win32 subprocess management
// Port of striphelper/ffmpeg.py — now with native FFmpeg fast-path

#include "subprocess.h"
#include "native_ffmpeg.h"
#include <windows.h>
#include <sstream>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cmath>
#include <set>
#include <mutex>

namespace sh
{

    // ── Global stop mechanism ────────────────────────────────────────────────────

    std::atomic<bool> g_stopRequested{false};
    static std::mutex g_procMtx;
    static std::set<HANDLE> g_activeProcs;

    void requestGlobalStop()
    {
        g_stopRequested = true;
        killAllFfmpeg();
    }

    void resetGlobalStop()
    {
        g_stopRequested = false;
    }

    void killAllFfmpeg()
    {
        std::lock_guard<std::mutex> lk(g_procMtx);
        for (auto h : g_activeProcs)
            TerminateProcess(h, 1);
    }

    static void trackProcess(HANDLE h)
    {
        std::lock_guard<std::mutex> lk(g_procMtx);
        g_activeProcs.insert(h);
    }

    static void untrackProcess(HANDLE h)
    {
        std::lock_guard<std::mutex> lk(g_procMtx);
        g_activeProcs.erase(h);
    }

    // ── Command line building ───────────────────────────────────────────────────

    static std::wstring toWide(const std::string &s)
    {
        if (s.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
        return w;
    }

    static std::wstring quoteArg(const std::wstring &a)
    {
        if (a.empty())
            return L"\"\"";
        bool needsQuote = false;
        for (auto c : a)
            if (c == L' ' || c == L'\t' || c == L'"' || c == L'&' || c == L'|' || c == L'(' || c == L')')
            {
                needsQuote = true;
                break;
            }
        if (!needsQuote)
            return a;
        std::wstring r = L"\"";
        for (auto c : a)
        {
            if (c == L'"')
                r += L"\\\"";
            else
                r += c;
        }
        r += L"\"";
        return r;
    }

    static std::wstring buildCmdLine(const std::vector<std::string> &args)
    {
        std::wstring line;
        for (size_t i = 0; i < args.size(); ++i)
        {
            if (i > 0)
                line += L' ';
            line += quoteArg(toWide(args[i]));
        }
        return line;
    }

    // ── Low-level process runner ────────────────────────────────────────────────

    RunResult runProcess(const std::vector<std::string> &args, const fs::path &cwd, bool captureStdout)
    {
        RunResult result;

        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE hReadPipe = INVALID_HANDLE_VALUE, hWritePipe = INVALID_HANDLE_VALUE;
        CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        // NUL handle for the uncaptured stream
        HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = INVALID_HANDLE_VALUE;
        if (captureStdout)
        {
            si.hStdOutput = hWritePipe;
            si.hStdError = hNul;
        }
        else
        {
            si.hStdOutput = hNul;
            si.hStdError = hWritePipe;
        }

        PROCESS_INFORMATION pi = {};
        auto cmdLine = buildCmdLine(args);
        std::wstring cwdW = cwd.wstring();

        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, cwdW.c_str(), &si, &pi);
        CloseHandle(hWritePipe);
        CloseHandle(hNul);

        if (!ok)
        {
            CloseHandle(hReadPipe);
            result.exitCode = -1;
            result.output = "CreateProcess failed";
            return result;
        }

        // Read captured output
        std::string captured;
        char buf[8192];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr) && bytesRead > 0)
        {
            buf[bytesRead] = 0;
            captured += buf;
        }
        CloseHandle(hReadPipe);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        result.exitCode = static_cast<int>(exitCode);
        result.output = std::move(captured);
        return result;
    }

    // ── FFmpeg runner with progress ─────────────────────────────────────────────

    void runFfmpeg(const std::vector<std::string> &cmd, const fs::path &cwd,
                   const std::string &stage, ProgressCb prog,
                   int64_t targetBytes, NoteCb note)
    {
        auto fullCmd = cmd;
        if (prog)
        {
            // Insert -progress pipe:2 -nostats after the first arg (ffmpeg path)
            fullCmd.insert(fullCmd.begin() + 1, {"-progress", "pipe:2", "-nostats"});
        }
        if (note)
            note(stage);

        // Build command line, set up pipes
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE hReadPipe, hWritePipe;
        CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        si.hStdInput = INVALID_HANDLE_VALUE;
        si.hStdOutput = hNul;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi = {};
        auto cmdLine = buildCmdLine(fullCmd);
        std::wstring cwdW = cwd.wstring();

        BOOL ok = CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW, nullptr, cwdW.c_str(), &si, &pi);
        CloseHandle(hWritePipe);
        CloseHandle(hNul);

        if (!ok)
        {
            CloseHandle(hReadPipe);
            if (note)
                note(stage + " failed (CreateProcess)");
            throw PipelineError(cwd, stage, "Failed to launch FFmpeg");
        }

        // Track process so killAllFfmpeg() can terminate it on Stop
        trackProcess(pi.hProcess);

        // Read stderr and parse progress
        std::string stderrBuf;
        double outTime = 0.0;
        int64_t bytesWritten = 0;

        // Non-blocking read loop
        char buf[4096];
        DWORD bytesRead;
        while (true)
        {
            BOOL readOk = ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
            if (!readOk || bytesRead == 0)
                break;
            buf[bytesRead] = 0;
            stderrBuf += buf;

            if (prog)
            {
                // Parse any complete lines
                size_t pos;
                while ((pos = stderrBuf.find('\n')) != std::string::npos)
                {
                    std::string line = stderrBuf.substr(0, pos);
                    stderrBuf.erase(0, pos + 1);
                    // Trim
                    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                        line.pop_back();

                    if (line.rfind("out_time_ms=", 0) == 0)
                    {
                        try
                        {
                            outTime = std::stod(line.substr(12)) / 1'000'000.0;
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (line.rfind("total_size=", 0) == 0)
                    {
                        try
                        {
                            bytesWritten = std::stoll(line.substr(11));
                        }
                        catch (...)
                        {
                        }
                    }
                    else if (line == "progress=end")
                    {
                        break;
                    }
                    prog(outTime, bytesWritten, targetBytes);
                }
            }
        }
        CloseHandle(hReadPipe);

        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD exitCode = 0;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        untrackProcess(pi.hProcess);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        // If stop was requested, throw immediately — don't treat as a normal error
        if (g_stopRequested.load())
            throw StopRequested();

        if (exitCode != 0)
        {
            if (note)
                note(stage + " failed");
            throw PipelineError(cwd, stage, stderrBuf.empty() ? ("ffmpeg exited " + std::to_string(exitCode)) : stderrBuf.substr(0, 800));
        }
        if (note)
            note(stage + " OK");
    }

    // ── FFprobe helpers ─────────────────────────────────────────────────────────

    static double ffprobeNum(const fs::path &fp, const fs::path &cwd, const std::vector<std::string> &extra)
    {
        std::vector<std::string> args = {g_ffprobe, "-v", "error"};
        args.insert(args.end(), extra.begin(), extra.end());
        args.push_back(fp.filename().string());
        auto r = runProcess(args, cwd, true);
        if (r.exitCode != 0)
            return 0.0;
        auto s = r.output;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        // Take first line
        auto nl = s.find('\n');
        if (nl != std::string::npos)
            s = s.substr(0, nl);
        while (!s.empty() && (s.back() == '\r'))
            s.pop_back();
        if (s.empty() || s == "N/A")
            return 0.0;
        try
        {
            return std::stod(s);
        }
        catch (...)
        {
            return 0.0;
        }
    }

    bool ffprobeOk(const fs::path &fp, const fs::path &cwd)
    {
        // Native fast-path: ~10ms vs ~200ms subprocess
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        if (nativeProbeOk(full))
            return true;
        // Fall back to subprocess
        auto r = runProcess({g_ffprobe, "-v", "error", "-i", fp.filename().string()}, cwd, false);
        return r.exitCode == 0;
    }

    double ffprobeFormatDuration(const fs::path &fp, const fs::path &cwd)
    {
        // Native fast-path
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        double d = nativeProbeDuration(full);
        if (d > 0)
            return d;
        return ffprobeNum(fp, cwd, {"-show_entries", "format=duration", "-of", "default=nw=1:nk=1"});
    }

    double ffprobeStreamDuration(const fs::path &fp, const fs::path &cwd, const std::string &sel)
    {
        return ffprobeNum(fp, cwd, {"-select_streams", sel, "-show_entries", "stream=duration", "-of", "default=nw=1:nk=1"});
    }

    double ffprobeBestDuration(const fs::path &fp, const fs::path &cwd)
    {
        // Native fast-path: single avformat_open_input vs 3 subprocess calls
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        double d = nativeProbeDuration(full);
        if (d > 0)
            return d;
        // Fall back to subprocess multi-query
        double fmt = ffprobeFormatDuration(fp, cwd);
        double vd = ffprobeStreamDuration(fp, cwd, "v:0");
        double ad = ffprobeStreamDuration(fp, cwd, "a:0");
        return std::max({fmt, vd, ad});
    }

    double ffprobeLastPacketPts(const fs::path &fp, const fs::path &cwd)
    {
        try
        {
            auto fname = fp.filename().string();
            auto vr = runProcess({g_ffprobe, "-v", "error", "-select_streams", "v:0",
                                  "-show_packets", "-show_entries", "packet=pts_time",
                                  "-of", "csv=p=0", "-read_intervals", "%+#9999999", fname},
                                 cwd, true);
            auto ar = runProcess({g_ffprobe, "-v", "error", "-select_streams", "a:0",
                                  "-show_packets", "-show_entries", "packet=pts_time",
                                  "-of", "csv=p=0", "-read_intervals", "%+#9999999", fname},
                                 cwd, true);

            auto lastLine = [](const std::string &text) -> double
            {
                auto lines = text;
                while (!lines.empty() && (lines.back() == '\n' || lines.back() == '\r'))
                    lines.pop_back();
                auto pos = lines.rfind('\n');
                std::string last = (pos != std::string::npos) ? lines.substr(pos + 1) : lines;
                while (!last.empty() && last.back() == '\r')
                    last.pop_back();
                try
                {
                    return std::stod(last);
                }
                catch (...)
                {
                    return 0.0;
                }
            };
            return std::max(lastLine(vr.output), lastLine(ar.output));
        }
        catch (...)
        {
            return 0.0;
        }
    }

    double ffprobeFramesDuration(const fs::path &fp, const fs::path &cwd)
    {
        auto fname = fp.filename().string();
        // avg_frame_rate
        auto r1 = runProcess({g_ffprobe, "-v", "error", "-select_streams", "v:0",
                              "-show_entries", "stream=avg_frame_rate", "-of", "default=nw=1:nk=1", fname},
                             cwd, true);
        std::string afr = r1.output;
        while (!afr.empty() && (afr.back() == '\n' || afr.back() == '\r'))
            afr.pop_back();
        double fps = 0.0;
        auto sl = afr.find('/');
        if (sl != std::string::npos)
        {
            try
            {
                double num = std::stod(afr.substr(0, sl));
                double den = std::stod(afr.substr(sl + 1));
                fps = (den > 0) ? num / den : 0.0;
            }
            catch (...)
            {
            }
        }

        // nb_read_frames (slow — decodes)
        auto r2 = runProcess({g_ffprobe, "-v", "error", "-count_frames", "1",
                              "-select_streams", "v:0", "-show_entries", "stream=nb_read_frames",
                              "-of", "default=nw=1:nk=1", fname},
                             cwd, true);
        std::string nb = r2.output;
        while (!nb.empty() && (nb.back() == '\n' || nb.back() == '\r'))
            nb.pop_back();
        int frames = 0;
        try
        {
            frames = std::stoi(nb);
        }
        catch (...)
        {
        }

        if (fps > 0.0 && frames > 0)
            return static_cast<double>(frames) / fps;
        return 0.0;
    }

    nlohmann::json ffprobeJsonFull(const fs::path &fp, const fs::path &cwd)
    {
        auto r = runProcess({g_ffprobe, "-v", "error", "-show_streams", "-show_format",
                             "-print_format", "json", fp.filename().string()},
                            cwd, true);
        if (r.exitCode != 0 || r.output.empty())
            return {};
        try
        {
            return nlohmann::json::parse(r.output);
        }
        catch (...)
        {
            return {};
        }
    }

    static double parseFpsRatio(const std::string &s)
    {
        if (s.empty() || s == "0/0")
            return 0.0;
        auto sl = s.find('/');
        if (sl != std::string::npos)
        {
            try
            {
                double a = std::stod(s.substr(0, sl));
                double b = std::stod(s.substr(sl + 1));
                return (b > 0) ? a / b : 0.0;
            }
            catch (...)
            {
                return 0.0;
            }
        }
        try
        {
            return std::stod(s);
        }
        catch (...)
        {
            return 0.0;
        }
    }

    ProbeSig probeSignature(const fs::path &fp, const fs::path &cwd)
    {
        // Native fast-path: direct libavformat probe
        ProbeSig sig;
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        auto np = nativeProbe(full);
        if (np.hasVideo || np.hasAudio)
        {
            sig.hasV = np.hasVideo;
            sig.hasA = np.hasAudio;
            sig.vCodec = np.videoCodec;
            sig.vW = np.width;
            sig.vH = np.height;
            sig.vPix = np.pixFmt;
            sig.vFps = np.fps;
            sig.aCodec = np.audioCodec;
            sig.aSr = np.sampleRate;
            sig.aCh = np.channels;
            return sig;
        }

        // Fall back to subprocess ffprobe JSON
        auto j = ffprobeJsonFull(fp, cwd);
        for (auto &s : j.value("streams", nlohmann::json::array()))
        {
            auto ct = s.value("codec_type", "");
            if (ct == "video" && !sig.hasV)
            {
                sig.hasV = true;
                sig.vCodec = s.value("codec_name", "");
                sig.vW = s.value("width", 0);
                sig.vH = s.value("height", 0);
                sig.vPix = s.value("pix_fmt", "");
                std::string fpsStr = s.value("avg_frame_rate", s.value("r_frame_rate", ""));
                sig.vFps = parseFpsRatio(fpsStr);
            }
            else if (ct == "audio" && !sig.hasA)
            {
                sig.hasA = true;
                sig.aCodec = s.value("codec_name", "");
                auto srStr = s.value("sample_rate", "0");
                try
                {
                    sig.aSr = std::stoi(srStr);
                }
                catch (...)
                {
                }
                sig.aCh = s.value("channels", 0);
                sig.aLayout = s.value("channel_layout", "");
            }
        }
        return sig;
    }

    // ── Encoder detection ───────────────────────────────────────────────────────

    static std::map<std::string, bool> g_encoderCache;
    static int g_aacMf = -1; // -1=unknown, 0=no, 1=yes

    std::map<std::string, bool> detectEncoders()
    {
        if (!g_encoderCache.empty())
            return g_encoderCache;

        // CRITICAL: -encoders outputs to STDOUT, so captureStdout must be TRUE
        auto r = runProcess({g_ffmpeg, "-hide_banner", "-encoders"}, fs::current_path(), true);
        std::string text = r.output;

        auto has = [&](const std::string &name) -> bool
        {
            // look for e.g. " h264_nvenc " in the encoders list
            return text.find(name) != std::string::npos;
        };

        g_encoderCache["h264_nvenc"] = has("h264_nvenc");
        g_encoderCache["hevc_nvenc"] = has("hevc_nvenc");
        g_encoderCache["av1_nvenc"] = has("av1_nvenc");
        g_encoderCache["h264_qsv"] = has("h264_qsv");
        g_encoderCache["hevc_qsv"] = has("hevc_qsv");
        g_encoderCache["av1_qsv"] = has("av1_qsv");
        g_encoderCache["h264_amf"] = has("h264_amf");
        g_encoderCache["hevc_amf"] = has("hevc_amf");
        g_encoderCache["av1_amf"] = has("av1_amf");
        g_encoderCache["libx264"] = has("libx264");
        g_encoderCache["libx265"] = has("libx265");
        g_aacMf = has("aac_mf") ? 1 : 0;

        return g_encoderCache;
    }

    bool hasAacMf()
    {
        if (g_aacMf < 0)
            detectEncoders();
        return g_aacMf == 1;
    }

    std::string aacEncoderName()
    {
        return hasAacMf() ? "aac_mf" : "aac";
    }

    // ── Hardware acceleration detection ─────────────────────────────────────────

    bool hasCudaHwaccel()
    {
        static int cached = -1;
        if (cached >= 0)
            return cached == 1;
        auto r = runProcess({g_ffmpeg, "-hide_banner", "-hwaccels"}, fs::current_path(), false);
        cached = (r.output.find("cuda") != std::string::npos) ? 1 : 0;
        return cached == 1;
    }

    // ── PTS continuity analysis ─────────────────────────────────────────────────

    double detectMaxPtsJump(const fs::path &fp, const fs::path &cwd)
    {
        // Native fast-path: direct packet reading, no subprocess
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        double nativeResult = nativeDetectMaxPtsJump(full);
        if (nativeResult > 0)
            return nativeResult;

        // Fall back to subprocess-based analysis
        double duration = ffprobeBestDuration(fp, cwd);
        if (duration <= 0.0)
            duration = 600.0; // fallback assumption

        // Build -read_intervals string: "0%+3,30%+3,60%+3,..."
        // Sample a 3-second window every 30 seconds, plus the last 10 seconds.
        constexpr double windowSec = 3.0;
        constexpr double stepSec = 30.0;
        std::string intervals;
        for (double t = 0.0; t < duration; t += stepSec)
        {
            if (!intervals.empty())
                intervals += ',';
            // Format: start%+duration  (% prefix = seconds from start)
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f%%+%.1f", t, windowSec);
            intervals += buf;
        }
        // Always sample the tail (last 10 seconds)
        if (duration > 15.0)
        {
            if (!intervals.empty())
                intervals += ',';
            char buf[64];
            snprintf(buf, sizeof(buf), "%.1f%%+10", std::max(0.0, duration - 10.0));
            intervals += buf;
        }

        auto r = runProcess({g_ffprobe, "-v", "error", "-select_streams", "v:0",
                             "-show_entries", "packet=pts_time", "-of", "csv=p=0",
                             "-read_intervals", intervals,
                             fp.filename().string()},
                            cwd, true);
        if (r.exitCode != 0)
            return 0.0;

        double maxJump = 0.0;
        double prev = -1.0;
        std::istringstream ss(r.output);
        std::string line;
        while (std::getline(ss, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
            if (line.empty() || line == "N/A")
                continue;
            try
            {
                double pts = std::stod(line);
                if (prev >= 0.0)
                {
                    double delta = pts - prev;
                    if (delta > maxJump)
                        maxJump = delta;
                }
                prev = pts;
            }
            catch (...)
            {
            }
        }
        return maxJump;
    }

} // namespace sh
