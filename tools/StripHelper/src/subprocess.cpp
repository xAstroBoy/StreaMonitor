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

    // ── FFprobe helpers — fully native (no subprocess fallback) ────────────────

    bool ffprobeOk(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeProbeOk(full);
    }

    double ffprobeFormatDuration(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeProbeDuration(full);
    }

    double ffprobeStreamDuration(const fs::path &fp, const fs::path &cwd, const std::string &sel)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeStreamDuration(full, sel);
    }

    double ffprobeBestDuration(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        double d = nativeProbeDuration(full);
        if (d > 0)
            return d;
        // Also check individual streams
        double vd = nativeStreamDuration(full, "v:0");
        double ad = nativeStreamDuration(full, "a:0");
        return std::max({d, vd, ad});
    }

    double ffprobeLastPacketPts(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeLastPacketPts(full);
    }

    double ffprobeFramesDuration(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeFrameCount(full);
    }

    ProbeSig probeSignature(const fs::path &fp, const fs::path &cwd)
    {
        ProbeSig sig;
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        auto np = nativeProbe(full);

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

    // ── Encoder detection — fully native (avcodec_find_encoder_by_name) ────────

    static std::mutex g_encoderMtx;
    static std::map<std::string, bool> g_encoderCache;
    static int g_aacMf = -1; // -1=unknown, 0=no, 1=yes

    std::map<std::string, bool> detectEncoders()
    {
        std::lock_guard lk(g_encoderMtx);
        if (!g_encoderCache.empty())
            return g_encoderCache;

        // Direct codec registry lookup — no subprocess needed
        const char *names[] = {
            "h264_nvenc", "hevc_nvenc", "av1_nvenc",
            "h264_qsv", "hevc_qsv", "av1_qsv",
            "h264_amf", "hevc_amf", "av1_amf",
            "libx264", "libx265"};
        for (auto *n : names)
            g_encoderCache[n] = nativeEncoderAvailable(n);

        g_aacMf = nativeEncoderAvailable("aac_mf") ? 1 : 0;

        return g_encoderCache;
    }

    bool hasAacMf()
    {
        detectEncoders();
        std::lock_guard lk(g_encoderMtx);
        return g_aacMf == 1;
    }

    std::string aacEncoderName()
    {
        return hasAacMf() ? "aac_mf" : "aac";
    }

    // ── Hardware acceleration detection — native ────────────────────────────────

    bool hasCudaHwaccel()
    {
        return nativeCudaAvailable();
    }

    // ── PTS continuity analysis — fully native ────────────────────────────────

    double detectMaxPtsJump(const fs::path &fp, const fs::path &cwd)
    {
        fs::path full = fp.is_absolute() ? fp : cwd / fp.filename();
        return nativeDetectMaxPtsJump(full);
    }

} // namespace sh
