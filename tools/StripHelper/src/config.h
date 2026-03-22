#pragma once
// StripHelper C++ — configuration constants
// Direct port of Python striphelper/config.py

#include <string>
#include <filesystem>
#include <cstdlib>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <regex>
#include <vector>
#include <fstream>
#include <windows.h>

namespace sh
{
    namespace fs = std::filesystem;

    // ── FFmpeg / FFprobe / MKVToolNix paths ─────────────────────────────────────
    inline std::string g_ffmpeg = "ffmpeg";
    inline std::string g_ffprobe = "ffprobe";
    inline std::string g_mkvpropedit = "mkvpropedit";

    // ── Validation thresholds ───────────────────────────────────────────────────
    // NOTE: For MKV concat output, be more lenient - container duration metadata
    // may differ slightly from sum of inputs due to timestamp normalization.
    constexpr double MIN_SIZE_RATIO_GENERIC = 0.90;  // Allow 10% variance (was 0.985)
    constexpr double MIN_DUR_RATIO_GENERIC = 0.90;   // Allow 10% variance (was 0.990)
    constexpr double MIN_SIZE_RATIO_TS2MUX = 0.85;   // Allow 15% variance (was 0.940)
    constexpr double MIN_DUR_RATIO_TS2MUX = 0.90;    // Allow 10% variance (was 0.985)

    constexpr double TAIL_TOL_SEC_TS2MUX = 600.0;    // 10 min tolerance (was 360s)
    constexpr double TAIL_TOL_SEC_GENERIC = 300.0;   // 5 min tolerance (was 180s)
    constexpr double TAIL_TOL_SEC_MERGE = 600.0;     // 10 min for merge (was 180s)

    constexpr double SALVAGE_TRIG_RATIO = 0.985;
    constexpr double SALVAGE_MIN_DELTA_SEC = 60.0;
    constexpr bool SALVAGE_IGNORE_SIZE = true;

    constexpr bool SALVAGE_TRUST_ON_SRC_PKT_ZERO = true;
    constexpr double SALVAGE_MIN_ABS_OK_SEC = 30.0;

    // ── PTS continuity ──────────────────────────────────────────────────────────
    constexpr double PTS_JUMP_THRESHOLD_SEC = 2.0; // max allowed gap between consecutive video packets

    // Smart detection: only probe PTS on .ts files larger than this (small files are fast to just remux)
    constexpr int64_t PTS_CHECK_MIN_BYTES = 5 * 1024 * 1024; // 5 MiB — skip PTS scan for tiny clips

    // ── Audio / Video defaults (mutable at runtime via settings / CLI) ─────────
    inline int TARGET_AUDIO_SR = 48000;
    inline int TARGET_AUDIO_CH = 2;
    inline int DEFAULT_TARGET_FPS = 30;

    // ── Behavior (mutable at runtime via settings / CLI) ────────────────────────
    inline bool DELETE_TS_AFTER_REMUX = true;
    inline int64_t FAILED_TS_DELETE_MAX_BYTES = 100 * 1024 * 1024; // 100 MiB — auto-delete failed .ts below this

    // ── Paths (defaults — overridden at runtime by initPaths + settings.json) ────
    inline fs::path CONFIG_PATH = R"(F:\config.json)";
    inline fs::path TO_PROCESS = R"(F:\To Process)";
    inline fs::path SETTINGS_PATH; // resolved at runtime next to the exe

    // ── Runtime path resolution ─────────────────────────────────────────────────
    inline fs::path getExeDir()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path();
    }

    inline void initPaths()
    {
        if (auto *p = std::getenv("FFMPEG"))
            g_ffmpeg = p;
        if (auto *p = std::getenv("FFPROBE"))
            g_ffprobe = p;
        if (auto *p = std::getenv("MKVPROPEDIT"))
            g_mkvpropedit = p;

        // Auto-detect mkvpropedit if not in PATH
        // Check common install locations on Windows
#ifdef _WIN32
        if (g_mkvpropedit == "mkvpropedit")
        {
            // Test if bare command works (in PATH)
            DWORD attr = GetFileAttributesA("mkvpropedit.exe");
            bool inPath = false;
            {
                char buf[MAX_PATH];
                inPath = SearchPathA(nullptr, "mkvpropedit.exe", nullptr, MAX_PATH, buf, nullptr) > 0;
            }
            if (!inPath)
            {
                // Check common install locations
                const char *candidates[] = {
                    R"(C:\Program Files\MKVToolNix\mkvpropedit.exe)",
                    R"(C:\Program Files (x86)\MKVToolNix\mkvpropedit.exe)",
                };
                for (auto *c : candidates)
                {
                    if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES)
                    {
                        g_mkvpropedit = c;
                        break;
                    }
                }
            }
        }
#endif

        // Settings file lives next to the exe
        SETTINGS_PATH = getExeDir() / "settings.json";

        // Allow env overrides for default paths
        if (auto *p = std::getenv("STRIPHELPER_CONFIG"))
            CONFIG_PATH = p;
        if (auto *p = std::getenv("STRIPHELPER_TO_PROCESS"))
            TO_PROCESS = p;
    }

    // ── Helpers ─────────────────────────────────────────────────────────────────
    inline bool isMediaExt(const std::string &ext)
    {
        auto lo = ext;
        std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
        return lo == ".ts" || lo == ".mp4" || lo == ".mkv";
    }

    // Check if a .ts file is actually an MPEG-TS transport stream (not TypeScript etc.)
    // MPEG-TS packets start with sync byte 0x47 and are 188/192/204 bytes long.
    // We verify by finding two consecutive sync bytes at the expected packet spacing.
    inline bool isMpegTsVideo(const fs::path &fp)
    {
        try
        {
            std::ifstream f(fp, std::ios::binary);
            if (!f)
                return false;

            char buf[1024];
            f.read(buf, sizeof(buf));
            auto bytesRead = f.gcount();
            if (bytesRead < 4)
                return false;

            constexpr int packetSizes[] = {188, 192, 204};

            // Scan for first 0x47 sync byte, then confirm with a second one
            for (int64_t i = 0; i < bytesRead; ++i)
            {
                if (static_cast<unsigned char>(buf[i]) != 0x47)
                    continue;
                for (int ps : packetSizes)
                {
                    if (i + ps < bytesRead && static_cast<unsigned char>(buf[i + ps]) == 0x47)
                        return true;
                }
                // If we're near EOF and can't verify second packet, first 0x47 at offset 0 is good enough
                if (i == 0 && bytesRead < 189)
                    return true;
            }
            return false;
        }
        catch (...)
        {
            return false; // Can't read → not confirmed as video → don't delete
        }
    }

    inline std::string humanBytes(int64_t n)
    {
        const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
        double v = static_cast<double>(std::max<int64_t>(n, 0));
        int i = 0;
        while (v >= 1024.0 && i < 5)
        {
            v /= 1024.0;
            ++i;
        }
        std::ostringstream os;
        os << std::fixed << std::setprecision(1) << v << " " << units[i];
        return os.str();
    }

    inline std::string humanSecs(double s)
    {
        if (s < 0)
            s = 0;
        int total = static_cast<int>(s);
        int h = total / 3600;
        int m = (total % 3600) / 60;
        int sec = total % 60;
        std::ostringstream os;
        os << std::setfill('0') << std::setw(2) << h << ":"
           << std::setfill('0') << std::setw(2) << m << ":"
           << std::setfill('0') << std::setw(2) << sec;
        return os.str();
    }

    inline std::string humanEta(double secs)
    {
        int s = std::max(0, static_cast<int>(secs));
        int h = s / 3600;
        s %= 3600;
        int m = s / 60;
        s %= 60;
        std::ostringstream os;
        os << std::setfill('0') << std::setw(2) << h << ":"
           << std::setfill('0') << std::setw(2) << m << ":"
           << std::setfill('0') << std::setw(2) << s;
        return os.str();
    }

    // Natural sort key (for filename ordering like 1.ts, 2.ts, 10.ts)
    inline std::vector<std::pair<bool, std::string>> natkey(const std::string &s)
    {
        std::vector<std::pair<bool, std::string>> parts;
        std::regex re(R"((\d+)|(\D+))");
        auto begin = std::sregex_iterator(s.begin(), s.end(), re);
        auto end = std::sregex_iterator();
        for (auto it = begin; it != end; ++it)
        {
            auto m = *it;
            bool isNum = m[1].matched;
            std::string val = m[0].str();
            if (!isNum)
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
            parts.push_back({isNum, val});
        }
        return parts;
    }

    inline bool natLess(const std::string &a, const std::string &b)
    {
        auto ka = natkey(a);
        auto kb = natkey(b);
        size_t n = std::min(ka.size(), kb.size());
        for (size_t i = 0; i < n; ++i)
        {
            if (ka[i].first && kb[i].first)
            {
                // both numeric: compare as integers
                long long na = 0, nb = 0;
                try
                {
                    na = std::stoll(ka[i].second);
                }
                catch (...)
                {
                }
                try
                {
                    nb = std::stoll(kb[i].second);
                }
                catch (...)
                {
                }
                if (na != nb)
                    return na < nb;
            }
            else
            {
                if (ka[i].second != kb[i].second)
                    return ka[i].second < kb[i].second;
            }
        }
        return ka.size() < kb.size();
    }

} // namespace sh
