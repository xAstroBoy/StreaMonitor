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

namespace tt
{

    struct VideoEntry
    {
        std::filesystem::path videoPath;
        std::filesystem::path thumbPath; // expected thumbnail path
        bool hasThumb = false;           // already has a thumbnail
        bool processed = false;          // generated this run
        bool failed = false;             // generation failed
        std::string errorMsg;
    };

    class App
    {
    public:
        App();
        ~App();

        // Called every frame from the main loop
        void render();

        // Settings (public for CLI overrides)
        int thumbnailWidth = 3840;
        int thumbnailColumns = 4;
        int thumbnailRows = 4;
        bool embedInVideo = true;
        int threadCount = 4;

    private:
        // Render sections
        void renderTable();
        void renderLogPanel();
        void renderSettingsPopup();

        // Actions
        void startScan();
        void startGeneration();
        void workerFunc();
        void addLog(const std::string &line);

        // Root directory to scan
        char rootDir_[1024] = {};

        // Discovered video files
        std::vector<VideoEntry> videos_;
        mutable std::mutex videosMutex_;

        // Log
        std::deque<std::string> log_;
        std::mutex logMtx_;
        bool autoScroll_ = true;

        // State
        bool scanned_ = false;
        bool showSettings_ = false;
        bool showLog_ = true;
        float splitRatio_ = 0.65f;

        // Worker threads (parallel processing)
        std::vector<std::jthread> workers_;
        std::atomic<bool> working_{false};
        std::atomic<bool> cancelWork_{false};
        std::atomic<int> processedCount_{0};
        std::atomic<int> totalToProcess_{0};
        std::atomic<int> nextIdx_{0};
        std::atomic<int> generated_{0};
        std::atomic<int> errors_{0};

        std::string currentFile_;
        std::mutex currentFileMutex_;

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
