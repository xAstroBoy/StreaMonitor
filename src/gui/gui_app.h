#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — ImGui GUI Application (Full Feature)
// Settings page, disk usage, cross-register, animations, preview
// ─────────────────────────────────────────────────────────────────

#include "core/bot_manager.h"
#include "config/config.h"
#include "gui/image_cache.h"
#include "gui/imgui_log_sink.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <atomic>

struct GLFWwindow;

namespace sm
{

    // ── Log entry for the log viewer ────────────────────────────────
    struct LogEntry
    {
        std::string message;
        std::string level;  // "info", "warn", "error", "debug"
        std::string source; // bot name or "system"
        TimePoint time;
    };

    // ── GUI Application ─────────────────────────────────────────────
    class GuiApp
    {
    public:
        GuiApp(AppConfig &config, ModelConfigStore &configStore, BotManager &manager,
               std::shared_ptr<ImGuiLogSink> logSink = nullptr);
        ~GuiApp();

        // Run the main loop (blocks until window is closed)
        int run();

        // Add log entry (thread-safe, called from any thread)
        void addLog(const std::string &level, const std::string &source,
                    const std::string &message);

    private:
        // ── Initialization ──────────────────────────────────────────
        bool initWindow();
        void initImGui();
        void applyTheme();
        void cleanup();

        // ── Main frame rendering ────────────────────────────────────
        void renderFrame();
        void renderMenuBar();
        void renderToolbar();
        void renderModelTable();
        void renderLogPanel();
        void renderStatusBar();

        // ── Dialogs & Windows ───────────────────────────────────────
        void renderSettingsWindow();
        void renderAddModelDialog();
        void renderAboutWindow();
        void renderFFmpegMonitor();
        void renderDiskUsagePanel();
        void renderCrossRegisterWindow();
        void renderBotDetailPanel();
        void renderEditModelDialog();

        // ── Settings sub-panels ─────────────────────────────────────
        void renderSettingsGeneral();
        void renderSettingsRecording();
        void renderSettingsFFmpeg();
        void renderSettingsNetwork();
        void renderSettingsSites();
        void renderSettingsAdvanced();

        // ── Helpers ─────────────────────────────────────────────────
        void refreshBotStates();
        ImVec4 statusColor(Status status) const;
        const char *statusIcon(Status status) const;
        std::string formatBytes(uint64_t bytes) const;
        std::string formatDuration(double seconds) const;
        std::string formatTimeAgo(TimePoint tp) const;
        std::string formatBytesHuman(uint64_t bytes) const;
        float getStatusPulse(Status status) const;

        // ── State ───────────────────────────────────────────────────
        AppConfig &config_;
        ModelConfigStore &configStore_;
        BotManager &manager_;
        GLFWwindow *window_ = nullptr;

        // UI state — windows/panels
        bool showSettings_ = false;
        bool showAddModel_ = false;
        bool showAbout_ = false;
        bool showFFmpegMon_ = false;
        bool showLogPanel_ = true;
        bool showDiskUsage_ = false;
        bool showCrossRegister_ = false;
        bool showBotDetail_ = false;
        bool autoScroll_ = true;
        int selectedBot_ = -1;
        float splitRatio_ = 0.65f;

        // Pending async operations (move / resync run on background thread)
        std::optional<std::pair<std::string, std::string>> pendingMoveBot_;
        std::optional<std::pair<std::string, std::string>> pendingResyncBot_;
        std::mutex asyncResultMutex_; // protects moveResyncResult_
        std::string moveResyncResult_;
        std::atomic<bool> moveResyncDone_{false};

        // Settings tab
        int settingsTab_ = 0;

        // Add model dialog state
        char addUsername_[256] = {};
        char addUrlInput_[512] = {}; // URL paste input
        int addSiteIdx_ = 0;
        bool addAlsoCounterpart_ = false; // SC<->SCVR cross-add checkbox

        // Edit model popup state
        bool showEditModel_ = false;
        char editModelUsername_[256] = {};
        int editModelSiteIdx_ = 0;
        std::string editModelOrigUser_;
        std::string editModelOrigSite_;

        // Search/filter
        char searchBuf_[256] = {};
        int filterStatus_ = 0; // 0 = all
        int filterSite_ = 0;   // 0 = all

        // Multi-select for bulk actions
        std::unordered_set<std::string> selectedRows_; // keys = lowercase usernames

        // Cross-register dialog
        char crossGroupName_[128] = {};
        char crossUsername_[256] = {};
        int crossSiteIdx_ = 0;
        std::string focusCrossRegisterGroup_; // scroll to this group when opening window
        char bulkGroupName_[128] = {};        // "Create Group" popup from bulk selection

        // Site filter for searchable combo popups
        char siteFilterBuf_[128] = {};

        // JSON search in bot detail panel
        char jsonSearchBuf_[128] = {};
        bool jsonShowRaw_ = false;

        // Cached bot states
        std::vector<BotState> cachedStates_;
        TimePoint lastRefresh_;

        // Cached disk usage
        BotManager::DiskUsageInfo cachedDiskUsage_;
        TimePoint lastDiskRefresh_;

        // Animation state
        float animTime_ = 0.0f;
        std::unordered_map<std::string, float> botAnimAlpha_; // per-bot fade animations

        // Preview thumbnail cache
        ImageCache imageCache_;

        // DPI scale factor (detected from monitor)
        float dpiScale_ = 1.0f;

        // Cached LAN URL for web dashboard (detected once at startup)
        std::string cachedLanUrl_;

        // Log buffer
        std::shared_ptr<ImGuiLogSink> logSink_;
        mutable std::mutex logMutex_;
        std::deque<LogEntry> logEntries_;
        static constexpr size_t kMaxLogEntries = 10000;

        // Settings edit state (mirrors AppConfig for editing)
        char editDownloadDir_[512] = {};
        int editResolution_ = 99999;
        int editContainerFmt_ = 0; // 0=MKV, 1=MP4, 2=TS
        int editPort_ = 5000;
        char editFfmpegPath_[512] = {};
        bool editDirtyFlag_ = false;

        // Encoding config edit state (mirrors EncodingConfig)
        int editEncoderType_ = 1; // 0=Copy, 1=X265, 2=X264, 3=NVENC_HEVC, 4=NVENC_H264
        bool editEnableCuda_ = false;
        int editCrf_ = 23;           // 0-51, lower=better quality
        int editPresetIdx_ = 5;      // preset index (ultrafast..placebo)
        int editAudioBitrate_ = 128; // kbps
        bool editCopyAudio_ = true;
        int editMaxWidth_ = 0;
        int editMaxHeight_ = 0;
        int editEncoderThreads_ = 0;

        // FFmpeg tuning edit state (mirrors AppConfig::FFmpegTuning)
        int editFfmpegLiveLastSegments_ = 3;
        int editFfmpegRwTimeoutSec_ = 5;
        int editFfmpegSocketTimeoutSec_ = 5;
        int editFfmpegReconnectDelayMax_ = 10;
        int editFfmpegMaxRestarts_ = 15;
        int editFfmpegGracefulQuitTimeoutSec_ = 6;
        int editFfmpegStartupGraceSec_ = 10;
        int editFfmpegSuspectStallSec_ = 25;
        int editFfmpegStallSameTimeSec_ = 20;
        float editFfmpegSpeedLowThreshold_ = 0.80f;
        int editFfmpegSpeedLowSustainSec_ = 25;
        int editFfmpegMaxSingleLagSec_ = 12;
        int editFfmpegMaxConsecSkipLines_ = 5;
        int editFfmpegFallbackNoStderrSec_ = 35;
        int editFfmpegFallbackNoOutputSec_ = 35;
        int editFfmpegCooldownAfterStalls_ = 5;
        int editFfmpegCooldownSleepSec_ = 15;
        int editFfmpegPlaylistProbeIntervalSec_ = 8;
        char editFfmpegProbeSize_[32] = "4M";
        char editFfmpegAnalyzeDuration_[32] = "10000000";

        // Proxy edit state
        bool editProxyEnabled_ = false;
        int editProxyType_ = 0; // 0=HTTP, 1=HTTPS, 2=SOCKS4, 3=SOCKS4A, 4=SOCKS5, 5=SOCKS5H
        char editProxyUrl_[512] = {};

        // ── Adaptive frame rate (CPU idle optimization) ─────────────
        double lastInputTime_ = 0.0;          // glfwGetTime() of last user input
        std::atomic<bool> guiDirty_{true};     // set by bot threads to wake GUI
        static void glfwCursorPosCallback(GLFWwindow *w, double, double);
        static void glfwMouseButtonCallback(GLFWwindow *w, int, int, int);
        static void glfwScrollCallback(GLFWwindow *w, double, double);
        static void glfwKeyCallback(GLFWwindow *w, int, int, int, int);
        static void glfwCharCallback(GLFWwindow *w, unsigned int);
        static void glfwWindowFocusCallback(GLFWwindow *w, int);
        void markActive();
    };

} // namespace sm
