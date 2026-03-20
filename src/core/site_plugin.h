#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Site plugin base class
// Thread-based model monitor with state machine lifecycle
// Faithful port of Python bot.py — every feature replicated
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "config/config.h"
#include "net/http_client.h"
#include "net/proxy_pool.h"
#include "net/m3u8_parser.h"
#include "downloaders/hls_recorder.h"
#include <string>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <map>
#include <set>
#include <deque>
#include <chrono>
#include <spdlog/spdlog.h>

namespace sm
{

    // Forward
    class SitePlugin;
    class BotManager;

    // ── Site plugin registry ────────────────────────────────────────
    class SiteRegistry
    {
    public:
        using FactoryFn = std::function<std::unique_ptr<SitePlugin>(const std::string &username)>;

        static SiteRegistry &instance();

        void registerSite(const std::string &siteName, const std::string &siteSlug,
                          FactoryFn factory);

        std::unique_ptr<SitePlugin> create(const std::string &siteName,
                                           const std::string &username) const;

        std::vector<std::string> siteNames() const;
        std::string slugToName(const std::string &slug) const;
        std::string nameToSlug(const std::string &name) const;
        bool hasSite(const std::string &name) const;

    private:
        struct SiteInfo
        {
            std::string name;
            std::string slug;
            FactoryFn factory;
        };
        std::map<std::string, SiteInfo> sites_;
        std::map<std::string, std::string> slugToName_;
    };

// ── Auto-registration macro ─────────────────────────────────────
#define REGISTER_SITE(ClassName) \
    static bool _reg_##ClassName = [] {                                       \
        sm::SiteRegistry::instance().registerSite(                            \
            ClassName::kSiteName, ClassName::kSiteSlug,                       \
            [](const std::string& username) -> std::unique_ptr<sm::SitePlugin> { \
                return std::make_unique<ClassName>(username);                  \
            });                                                               \
        return true; }()

    // ── Bot state (thread-safe observable) ──────────────────────────
    struct BotState
    {
        std::string username;
        std::string siteName;
        std::string siteSlug;
        Status status = Status::NotRunning;
        Status prevStatus = Status::Unknown;
        bool running = false;
        bool recording = false;
        bool quitting = false;
        bool mobile = false;
        Gender gender = Gender::Unknown;
        std::string country;
        std::string websiteUrl;
        std::string previewUrl; // thumbnail / snapshot URL
        std::string groupName;
        std::string roomId; // for RoomIdBot sites (StripChat, F4F, SexChatHU, FanslyLive)
        int consecutiveErrors = 0;
        int fileCount = 0;
        uint64_t totalBytes = 0;
        std::string currentFile;
        TimePoint lastStatusChange;
        TimePoint startTime;
        RecordingStats recordingStats;

        // Error/debug info for inspection
        std::string lastError;       // Last error message
        std::string lastApiResponse; // Last API JSON response (for debugging)
        int lastHttpCode = 0;        // Last HTTP status code
    };

    using StateChangeCallback = std::function<void(const BotState &)>;

    // ── Site Plugin base class ──────────────────────────────────────
    class SitePlugin
    {
    public:
        SitePlugin(const std::string &siteName, const std::string &siteSlug,
                   const std::string &username);
        virtual ~SitePlugin();

        SitePlugin(const SitePlugin &) = delete;
        SitePlugin &operator=(const SitePlugin &) = delete;

        // ── Lifecycle ───────────────────────────────────────────────
        void configure(const AppConfig &config); // HTTP setup only (no thread)
        void start(const AppConfig &config);
        void stop();
        void requestQuit();
        bool isRunning() const;
        bool isAlive() const;

        // ── State access (thread-safe) ──────────────────────────────
        BotState getState() const;
        Status getStatus() const;
        void setStateCallback(StateChangeCallback cb);

        // ── Identity ────────────────────────────────────────────────
        const std::string &username() const { return username_; }
        const std::string &siteName() const { return siteName_; }
        const std::string &siteSlug() const { return siteSlug_; }
        std::string id() const { return username_ + "_" + siteSlug_; }

        // ── Config ──────────────────────────────────────────────────
        void setGender(Gender g);
        void setCountry(const std::string &c);
        void setRoomId(const std::string &rid);
        void setUsername(const std::string &newUsername); // for redirect updates

        // ── Output folder ───────────────────────────────────────────
        std::filesystem::path getOutputFolder(const AppConfig &config) const;

        // ── Force resync (immediate status check) ───────────────────
        void forceResync();

        // ── Continuous preview (in-memory, pushed by recorder) ────────
        // The recorder pushes RGBA frames at ~15fps automatically.
        // GUI calls consumePreview() to get the latest frame if new.
        // MJPEG endpoint calls waitForPreview() to block until next frame.
        bool consumePreview(PreviewFrame &out, uint64_t &lastVersion);
        bool waitForPreview(PreviewFrame &out, uint64_t &lastVersion, int timeoutMs);

        // ── Live audio (pushed by recorder, consumed by GUI audio player) ──
        // Callback receives interleaved f32 stereo PCM at 48 kHz.
        using AudioDataCallback = std::function<void(const float *samples, size_t frameCount)>;
        void setAudioDataCallback(AudioDataCallback cb);
        void clearAudioDataCallback();

        // ── Override these in site plugins ──────────────────────────
        virtual Status checkStatus() = 0;
        virtual std::string getVideoUrl() = 0;
        virtual std::string getWebsiteUrl() const = 0;
        virtual std::pair<std::string, std::vector<std::string>> getSiteColor() const
        {
            return {"white", {}};
        }

        // ── Preview / thumbnail URL ──────────────────────────────
        virtual std::string getPreviewUrl() const { return ""; }

        // ── Mobile detection ────────────────────────────────────────
        // Returns the stream-detected mobile state.  This is set by the
        // recorder's resolution callback when it first sees the actual
        // video dimensions (portrait = mobile).  Always starts false and
        // is updated automatically — NEVER trust the site API.
        virtual bool isMobile() const;

        // API mobile hint — sites (like StripChat) may report isMobile
        // from the broadcaster's device.  This is ONLY used as a trigger
        // hint for cross-register dual-recording (model_group), NOT for
        // output folder decisions.  Default: false.
        virtual bool apiMobileHint() const { return false; }

        // ── Master playlist URL (for SegmentFeeder monitoring) ───────
        // Stored by selectResolution() or site-specific getVideoUrl().
        // Used by the recorder to periodically re-fetch the master m3u8
        // and detect orientation changes from RESOLUTION= tags.
        std::string masterUrl() const;
        void setMasterUrl(const std::string &url);

        // ── Bulk update support ─────────────────────────────────────
        virtual bool supportsBulkUpdate() const { return false; }

        // ── Manager for auto-removal ────────────────────────────────
        static void setManager(BotManager *mgr) { manager_ = mgr; }

    protected:
        HttpClient &http() { return http_; }

        // Select best resolution from master playlist (uses config)
        std::string selectResolution(const std::string &masterUrl);

        // State helpers
        void setState(Status status);
        void setRecording(bool rec);
        void setMobile(bool mobile);
        void setLastError(const std::string &err, int httpCode = 0);
        void setLastApiResponse(const std::string &json);
        void setRecordingResolution(int width, int height);

        // Config pointer (set by start/configure)
        const AppConfig *config_ = nullptr;

        // Logging
        std::shared_ptr<spdlog::logger> logger_;

        // Sleep settings (match Python bot.py defaults exactly)
        int sleepOnPrivate_ = 5;
        int sleepOnOffline_ = 5;
        int sleepOnLongOffline_ = 15;
        int sleepOnError_ = 20;
        int sleepOnRateLimit_ = 180;
        int longOfflineTimeout_ = 180;
        int maxConsecutiveErrors_ = 20;

    private:
        void threadFunc(const AppConfig &config);
        void downloadLoop(const AppConfig &config);
        bool downloadOnce(const AppConfig &config);
        bool postDownloadCleanup(const std::string &finalPath, bool ok);
        void sleepInterruptible(int seconds);
        std::string generateOutputPath(const AppConfig &config);
        void autoRemoveModel(const std::string &reason);

        std::string siteName_;
        std::string siteSlug_;
        std::string username_;

        std::unique_ptr<std::jthread> thread_;
        std::atomic<bool> running_{false};
        std::atomic<bool> quitting_{false};
        std::atomic<bool> resyncPending_{false}; // Force immediate status check
        std::atomic<bool> streamMobile_{false};  // Stream-detected portrait (h > w)
        CancellationToken cancelToken_;
        std::string lastMasterUrl_; // Master playlist URL for feeder monitoring
        mutable std::mutex masterUrlMutex_;

        // Condition variable for efficient sleeping (replaces 1-second polling)
        std::mutex sleepMutex_;
        std::condition_variable sleepCv_;

        mutable std::mutex stateMutex_;
        BotState state_;
        StateChangeCallback stateCallback_;

        HttpClient http_;

        // Continuous preview state (pushed by recorder, consumed by GUI/web)
        std::mutex previewMutex_;
        std::condition_variable previewCv_;
        PreviewFrame pendingPreview_;           // currently-displayed frame
        std::deque<PreviewFrame> previewQueue_; // buffered frames for smooth playback
        uint64_t previewVersion_ = 0;
        std::chrono::steady_clock::time_point lastPreviewPumpTime_{};
        static constexpr size_t kMaxPreviewQueue = 90; // ~3 s at 30 fps
        void pumpPreviewQueue_();                      // caller must hold previewMutex_

        // Audio forwarding callback (set by GUI, called by recorder thread)
        std::mutex audioMutex_;
        AudioDataCallback audioDataCb_;

        // Proxy pool for round-robin proxy selection
        ProxyPool proxyPool_;
        std::string currentProxyUrl_; // Currently assigned proxy URL

        static std::mutex fileNumberMutex_;
        static BotManager *manager_;
    };

} // namespace sm
