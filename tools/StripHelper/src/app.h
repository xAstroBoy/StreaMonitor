// StripHelper C++ — Application GUI declarations
#pragma once
#include "config.h"
#include "pipeline.h"
#include "shell_integration.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <deque>
#include <filesystem>
#include <functional>
#include <nlohmann/json.hpp>

struct ImFont;

namespace sh
{

    enum class RowStatus
    {
        Pending,
        Queued,
        Working,
        Done,
        Error,
        Skipped
    };

    struct FolderRow
    {
        fs::path absPath;
        std::string relPath; // display path relative to root
        RowStatus status = RowStatus::Pending;
        std::string stage;
        float pct = 0.0f;
        float eta = 0.0f; // seconds remaining
        int64_t written = 0;
        int64_t target = 0;
        std::string durInfo;
        std::string sizeInfo;
        std::string info; // note / error / result
    };

    class App
    {
    public:
        App();
        ~App();
        void render(); // called each ImGui frame
        bool wantQuit() const { return quit_; }
        bool isProcessing() const { return running_.load(); }
        void setPath(const std::string &p); // set initial folder path
        void setSymlinks(bool v) { mkLinks_ = v; }
        void setAutoStart(bool v) { autoStart_ = v; }

        // CLI setters — all configurable options can be overridden
        void setThreads(int v) { threads_ = std::clamp(v, 1, 32); }
        void setRepairPts(bool v) { repairPts_ = v; }
        void setDeleteTs(bool v) { deleteTs_ = v; }
        void setFailedTsMaxMB(int v) { failedTsMaxMB_ = std::clamp(v, 0, 10000); }
        void setTargetFps(int v) { targetFps_ = std::clamp(v, 1, 120); }
        void setAudioSampleRate(int v) { audioSampleRate_ = std::clamp(v, 8000, 192000); }
        void setAudioChannels(int v) { audioChannels_ = std::clamp(v, 1, 8); }
        void setThumbnailEnabled(bool v) { thumbnailEnabled_ = v; }
        void setThumbnailWidth(int v) { thumbnailWidth_ = std::clamp(v, 320, 3840); }
        void setThumbnailColumns(int v) { thumbnailColumns_ = std::clamp(v, 1, 10); }
        void setThumbnailRows(int v) { thumbnailRows_ = std::clamp(v, 1, 10); }

        // Push member settings → config.h runtime globals (call after CLI overrides)
        void syncSettingsToGlobals();

    private:
        // GUI sections
        void renderTopBar();
        void renderTable();
        void renderBottomBar();
        void renderLogPanel();
        void renderSettingsPopup();

        // Settings tab renderers
        void renderSettingsGeneral();
        void renderSettingsIO();
        void renderSettingsVideo();
        void renderSettingsPaths();
        void renderSettingsThumbnails();
        void renderSettingsShellIntegration();
        void renderSettingsImport();

        // Settings persistence
        void loadSettings();
        void saveSettings();
        void importSmConfig(const std::string &path);

        // Actions
        void browseFolder();
        void startProcessing();
        void stopProcessing();

        // Worker
        void workerMain();
        void processOne(int idx);

        // State — protected by mtx_
        std::mutex mtx_;
        std::vector<FolderRow> rows_;
        std::deque<std::string> log_; // rolling log lines (max 500)
        char pathBuf_[1024] = {};
        int threads_ = 4;
        bool mkLinks_ = false;
        bool started_ = false;
        bool quit_ = false;
        bool autoStart_ = false; // auto-run on first frame
        bool autoClose_ = false; // auto-close window when processing finishes
        bool showSettings_ = false;

        // Configurable thresholds (persisted in settings.json)
        bool repairPts_ = true; // run PTS repair on .ts files
        bool useCuda_ = false;  // encoder mode: false=CPU, true=CUDA
        bool deleteTs_ = DELETE_TS_AFTER_REMUX;
        int failedTsMaxMB_ = (int)(FAILED_TS_DELETE_MAX_BYTES / (1024 * 1024));
        int targetFps_ = DEFAULT_TARGET_FPS;
        int audioSampleRate_ = TARGET_AUDIO_SR;
        int audioChannels_ = TARGET_AUDIO_CH;
        char defaultPathBuf_[1024] = {};
        char configPathBuf_[1024] = {};

        // Thumbnail generation
        bool thumbnailEnabled_ = true;
        int thumbnailWidth_ = 1280;
        int thumbnailColumns_ = 4;
        int thumbnailRows_ = 4;

        std::atomic<int> doneCount_{0};
        std::atomic<int> errCount_{0};
        std::atomic<int> totalCount_{0};
        std::atomic<bool> running_{false};
        std::atomic<bool> stopReq_{false};

        // Worker threads
        std::vector<std::thread> workers_;
        std::atomic<int> nextIdx_{0};

        // Config from config.json (for symlinks)
        nlohmann::json cfg_;

        // Helpers
        void addLog(const std::string &line);
        static const char *statusLabel(RowStatus s);

        // Log panel state
        bool autoScroll_ = true;
        bool showLog_ = true;
        float logHeight_ = 180.0f;
        float splitRatio_ = 0.65f; // splitter: table / log ratio
    };

} // namespace sh
