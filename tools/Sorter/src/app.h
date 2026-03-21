#pragma once
// Sorter — ImGui application class
// Provides a GUI for sorting model video files by date/tag/VR/Mobile.

#include "sorter.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <thread>
#include <filesystem>

namespace sorter
{

    class App
    {
    public:
        App();
        ~App();

        // Called every frame from the main loop
        void render();

    private:
        void renderSettingsPopup();
        void addLog(const std::string &line);

        // Actions
        void startSort();
        void sortWorker();
        void startSymlinks();
        void symlinkWorker();

        // Config
        void loadSettings();
        void saveSettings();
        fs::path appConfigPath() const;

        SorterConfig cfg_;
        bool configLoaded_ = false;
        std::string configError_;

        // Sort results
        std::vector<SortResult> results_;
        std::mutex resultsMutex_;

        // Log
        std::deque<std::string> log_;
        std::mutex logMtx_;
        bool autoScroll_ = true;

        // State
        bool showSettings_ = false;
        std::jthread sortThread_;
        std::atomic<bool> sorting_{false};
        std::jthread symlinkThread_;
        std::atomic<bool> creatingSymlinks_{false};
    };

} // namespace sorter
