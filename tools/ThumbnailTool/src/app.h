#pragma once
// ThumbnailTool — Batch thumbnail generator for video files
// Recursively scans a directory, finds videos without thumbnails,
// generates high-res contact sheets and embeds them as MKV cover art.

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <filesystem>
#include <chrono>
#include <array>

namespace tt
{

    enum class ContainerType : uint8_t
    {
        RealMKV = 0, // .mkv with real EBML header — embed-ready
        FakeMKV = 1, // .mkv but actually MP4 inside — needs remux
        Other = 2,   // .mp4 / .ts / etc — needs remux to MKV
    };

    // Per-thread progress tracking
    struct ThreadProgress
    {
        std::atomic<bool> active{false};
        std::string filePath;
        std::string action;                              // "Remuxing", "Generating", "Embedding", "VR Metadata", etc.
        std::string subAction;                           // more detail like frame count, percentage
        int64_t fileSize = 0;                            // size of current file being processed
        std::atomic<int64_t> bytesProcessed{0};          // bytes processed so far (for I/O speed)
        std::chrono::steady_clock::time_point startTime; // when this file started
        std::mutex mtx;
    };

    // Max threads for progress tracking
    constexpr int kMaxThreads = 32;

    // Tab filter for the status pages
    enum class StatusTab : int
    {
        All = 0,
        InProgress, // NEW: shows what each thread is currently processing
        Pending,
        Done,
        Failed,
    };

    struct VideoEntry
    {
        std::filesystem::path videoPath;
        std::filesystem::path thumbPath; // expected thumbnail path
        std::string relDisplay;          // cached relative path string for rendering
        ContainerType container = ContainerType::Other;
        int64_t fileSize = 0;       // file size in bytes (for sorting + display)
        bool hasThumb = false;      // .jpg file exists (or embedded cover = treated as thumb)
        bool hasCoverEmbed = false; // MKV has embedded cover art (detected during scan)
        bool coverProbed = false;   // true once hasCoverArt() has been called (deferred)
        bool hasTag = false;        // has THUMBNAILED metadata tag (definitive processed marker)
        bool tsFixed = false;       // timestamps were fixed this run
        bool remuxed = false;       // was remuxed to real MKV this run
        bool processed = false;     // processed this run
        bool failed = false;        // processing failed
        bool regenQueued = false;   // queued for thumbnail regeneration
        std::string errorMsg;
    };

    class App
    {
    public:
        App();
        ~App();

        // Called every frame from the main loop
        void render();

        // Settings (public for CLI overrides) - atomic for live updates during processing
        std::atomic<int> thumbnailWidth{3840};
        std::atomic<int> thumbnailColumns{4};
        std::atomic<int> thumbnailRows{4};
        std::atomic<bool> embedInVideo{true};
        std::atomic<int> threadCount{4};

    private:
        // Render sections
        void renderTable();
        void renderLogPanel();
        void renderSettingsPopup();

        // Actions
        void startScan();  // launches scanWorker on background thread
        void scanWorker(const std::string &rootStr); // actual scan logic (runs off GUI thread)
        void startGeneration();
        void workerFunc(int threadIdx);
        void addLog(const std::string &line);

        // Config persistence
        void loadConfig();
        void saveConfig();
        std::filesystem::path configPath() const;

        // Root directory to scan
        char rootDir_[1024] = {};

        // Discovered video files
        std::vector<VideoEntry> videos_;
        mutable std::mutex videosMutex_;

        // Log
        std::deque<std::string> log_;
        std::mutex logMtx_;
        bool autoScroll_ = true;
        char logFilter_[128] = {}; // text search filter for log panel
        int logLevelFilter_ = 0;   // 0=All, 1=INFO+, 2=WARN+, 3=ERROR only

        // State
        bool scanned_ = false;
        bool showSettings_ = false;
        bool showLog_ = true;
        bool hideFinished_ = false;
        float splitRatio_ = 0.65f;
        StatusTab currentTab_ = StatusTab::All;

        // Scan thread (runs off GUI thread)
        std::jthread scanThread_;
        std::atomic<bool> scanning_{false};
        std::atomic<int> scanProgress_{0}; // live file count during scan

        // Worker threads (parallel processing)
        std::vector<std::jthread> workers_;
        std::atomic<bool> working_{false};
        std::atomic<bool> cancelWork_{false};
        std::atomic<int> processedCount_{0};
        std::atomic<int> totalToProcess_{0};
        std::atomic<int> nextIdx_{0};
        std::atomic<int> generated_{0};
        std::atomic<int> errors_{0};
        std::atomic<int> remuxedCount_{0};
        std::atomic<int> embeddedCount_{0};
        std::atomic<int> vrCount_{0};
        std::atomic<int> activeWorkers_{0};
        std::atomic<int64_t> bytesProcessed_{0}; // bytes of completed files (for ETA)
        std::atomic<int64_t> totalBytes_{0};     // total bytes to process
        std::atomic<int> lastThreadCount_{0};    // track thread count changes
        std::mutex workerMutex_;                 // protect worker spawning

        std::string currentFile_;
        std::mutex currentFileMutex_;
        std::chrono::steady_clock::time_point startTime_;

        // Per-thread progress tracking (array for lock-free access by thread index)
        std::array<ThreadProgress, kMaxThreads> threadProgress_;

        // ETA rolling window (accessed only from render thread)
        int64_t etaLastBytes_ = 0;
        std::chrono::steady_clock::time_point etaLastTime_;
        double etaRollingBps_ = 0;

        // Stats (from scan)
        int totalVideos_ = 0;
        int withThumb_ = 0;
        int withoutThumb_ = 0;

    public:
        // Video extensions to look for
        static constexpr const char *kVideoExts[] = {
            ".mp4", ".mkv", ".ts", ".avi", ".mov", ".flv", ".wmv", ".webm", ".m4v"};
    };

} // namespace tt
