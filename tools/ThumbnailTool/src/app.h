#pragma once
// ThumbnailTool — Batch thumbnail generator for video files
// Recursively scans a directory, finds videos without thumbnails, generates them.

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
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

        // Settings
        int thumbnailWidth = 640;
        int thumbnailColumns = 4;
        int thumbnailRows = 6;

    private:
        void renderMainPanel();
        void renderProgressPopup();
        void renderSettingsPopup();

        void startScan();
        void startGeneration();
        void workerFunc();

        // Root directory to scan
        char rootDir_[1024] = "F:\\StripChat";

        // Discovered video files
        std::vector<VideoEntry> videos_;
        mutable std::mutex videosMutex_;

        // State
        bool scanned_ = false;
        bool showSettings_ = false;
        bool showProgress_ = false;

        // Worker thread for generation
        std::unique_ptr<std::jthread> worker_;
        std::atomic<bool> working_{false};
        std::atomic<bool> cancelWork_{false};
        std::atomic<int> processedCount_{0};
        std::atomic<int> totalToProcess_{0};
        std::string currentFile_;
        std::mutex currentFileMutex_;

        // Stats
        int totalVideos_ = 0;
        int withThumb_ = 0;
        int withoutThumb_ = 0;
        int generated_ = 0;
        int errors_ = 0;

    public:
        // Video extensions to look for
        static constexpr const char *kVideoExts[] = {
            ".mp4", ".mkv", ".ts", ".avi", ".mov", ".flv", ".wmv", ".webm", ".m4v"};
    };

} // namespace tt
