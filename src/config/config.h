#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Application configuration
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "net/proxy_pool.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

namespace sm
{

    struct AppConfig
    {
        // Paths
        std::filesystem::path downloadsDir = "downloads";
        std::filesystem::path configFile = "config.json";
        std::filesystem::path ffmpegPath = "ffmpeg";

        // Recording
        ContainerFormat container = ContainerFormat::MKV;
        int wantedResolution = 99999;
        ResolutionPref resolutionPref = ResolutionPref::Closest;
        bool ffmpegReadRate = false;
        int segmentTimeSec = 0; // 0 = no segmenting

        // Output filename format — supported tokens:
        //   {n}     = sequential number (1, 2, 3...)
        //   {model} = model username
        //   {site}  = site slug (SC, CB, BC, etc.)
        //   {date}  = date as YYYY-MM-DD
        //   {time}  = time as HH-MM-SS
        //   {datetime} = YYYYMMDD-HHMMSS (Python SM style)
        // Example: "{model}-{datetime}" → "username-20250710-143022.mkv"
        std::string filenameFormat = "{model}_{site}_{datetime}";

        // Output folder name format (Issue #2) — supported tokens:
        //   {model} = model username
        //   {site}  = site slug (SC, CB, BC, etc.)
        // Example: "{model} [{site}]" → "username [SC]" (default)
        //          "{model}" → "username" (no site suffix)
        std::string folderFormat = "{model} [{site}]";

        // Recording mode: Continuous (single file), ChunkedBySize, ChunkedByDuration
        // 0 = Continuous (default), 1 = Chunked by file size, 2 = Chunked by duration
        int recordingMode = 0;
        int chunkSizeMB = 500;     // max MB per file (ChunkedBySize mode)
        int chunkDurationMin = 60; // max minutes per file (ChunkedByDuration mode)

        // Network
        std::string userAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0";
        bool verifySsl = true;
        int httpTimeoutSec = 30;

        // ── Proxy pool (multiple proxies with round-robin) ──────────
        bool proxyEnabled = false;
        std::vector<ProxyEntry> proxies; // Multiple proxies for round-robin
        int proxyMaxFailures = 5;        // Failures before temp-disable
        int proxyDisableSec = 60;        // Seconds to disable after max failures
        bool proxyAutoDisable = true;    // Auto-disable failing proxies

        // Web server
        bool webEnabled = true;
        std::string webHost = "0.0.0.0"; // 0.0.0.0 = listen on all interfaces (WiFi intranet)
        int webPort = 5000;
        std::string webUsername = "admin";
        std::string webPassword = "admin";
        std::string webStaticDir = "web"; // directory containing static Next.js export

        // Disk
        float minFreeDiskPercent = 5.0f;

        // Behavior
        bool minimizeToTray = true;    // Minimize to system tray instead of taskbar
        bool autoStartOnLogin = false; // Register in Windows startup

        // Debug
        bool debug = false;

        // Logging level: "debug", "info", "warn", "error" (default: info)
        std::string logLevel = "info";

        // Model auto-removal behavior
        // When false (default), offline/non-existent/deleted models are
        // NOT auto-removed — they simply stop and can be restarted later.
        // When true, non-existent or deleted models are automatically removed
        // from the config (legacy Python SM behavior).
        bool autoRemoveNonExistent = false;

        // Stripchat spy/private recording (Issue #8)
        // When enabled and model goes Private, attempt to record using
        // authenticated access. Requires valid stripchatCookies.
        bool spyPrivateEnabled = false;
        std::string stripchatCookies; // Browser cookies for Stripchat auth (e.g. "session_id=abc123; ...")

        // Preview capture (decoded video frames for GUI + web preview)
        bool enablePreviewCapture = true;

        // Thumbnail contact sheet generation (after each recording)
        bool thumbnailEnabled = true;
        int thumbnailWidth = 1280;
        int thumbnailColumns = 4;
        int thumbnailRows = 4;

        // Per-site options (site slug → options)
        struct SiteOptions
        {
            bool enabled = true; // whether this site is active
            int quality = 0;     // 0 = use global resolution, >0 = override
        };
        std::map<std::string, SiteOptions> siteOptions;

        // Per-site VR defaults (site slug → VRConfig)
        // Models on VR sites automatically get these unless overridden
        std::map<std::string, VRConfig> siteVRDefaults;

        // Encoding settings (transcoding vs stream copy)
        EncodingConfig encoding; // Default: x265 MKV, CUDA off

        // FFmpeg tuning
        struct FFmpegTuning
        {
            int liveLastSegments = 3;
            int rwTimeoutSec = 5;
            int socketTimeoutSec = 5;
            int reconnectDelayMax = 10;
            int maxRestarts = 15; // bot layer re-checks status between download attempts
            int gracefulQuitTimeoutSec = 6;
            int startupGraceSec = 10;
            int suspectStallSec = 25;
            int stallSameTimeSec = 20;
            float speedLowThreshold = 0.80f;
            int speedLowSustainSec = 25;
            int maxSingleLagSec = 12;
            int maxConsecSkipLines = 5;
            int fallbackNoStderrSec = 35;
            int fallbackNoOutputSec = 35;
            int cooldownAfterStalls = 5;
            int cooldownSleepSec = 15;
            int playlistProbeIntervalSec = 8;
            std::string probeSize = "4M";
            std::string analyzeDuration = "10000000";
        } ffmpeg;

        // Load / Save
        void loadFromFile(const std::filesystem::path &path);
        void saveToFile(const std::filesystem::path &path) const;
        void loadFromEnv();
    };

    // ── Model list config (config.json) ─────────────────────────────
    class ModelConfigStore
    {
    public:
        void load(const std::filesystem::path &path);
        void save(const std::filesystem::path &path) const;
        void save() const; // saves to last loaded path

        void add(const ModelConfig &model);
        bool remove(const std::string &username, const std::string &siteslug = "");
        void updateStatus(const std::string &username, const std::string &siteslug,
                          Status status, bool recording);
        void updateRunning(const std::string &username, const std::string &siteslug,
                           bool running);
        void setCrossRegisterGroup(const std::string &username, const std::string &site,
                                   const std::string &groupName);

        std::vector<ModelConfig> getAll() const;
        std::optional<ModelConfig> find(const std::string &username,
                                        const std::string &siteslug) const;
        size_t count() const;

    private:
        mutable std::mutex mutex_;
        std::vector<ModelConfig> models_;
        std::filesystem::path lastPath_;
    };

    // JSON serialization
    void to_json(nlohmann::json &j, const ModelConfig &m);
    void from_json(const nlohmann::json &j, ModelConfig &m);

    // Build folder name from format string (Issue #2)
    // Tokens: {model} = username, {site} = site slug
    std::string buildFolderName(const std::string &format, const std::string &username,
                                const std::string &siteSlug);

} // namespace sm
