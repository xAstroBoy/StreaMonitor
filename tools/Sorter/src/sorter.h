#pragma once
// Sorter — File sorting engine
// Ports NewSorter.py logic to C++.
// Reads config.json for VR/Desktop tags, aliases, phone clip patterns.
// Sorts files from F:\Unsorted → F:\StripChat with model/year/month/VR structure.
// Creates symlinks for New Videos, Mobile, VR, Normal views.

#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <functional>

namespace sorter
{
    namespace fs = std::filesystem;

    // ── Configuration loaded from config.json ──────────────────────

    struct SorterConfig
    {
        std::vector<std::string> vrTags;                         // e.g. "[SCVR]", "[DCVR]"
        std::vector<std::string> desktopTags;                    // e.g. "[BC]", "[SC]", ...
        std::map<std::string, std::vector<std::string>> aliases; // model → [alias1, alias2, ...]
        std::vector<std::string> phoneClips;                     // e.g. "-clip", "-phoneclip"

        // Directories
        std::string configPath = "F:\\config.json";
        std::string unsortedDir = "F:\\Unsorted";
        std::string sortedDir = "F:\\StripChat";
        std::string newVideosDir = "F:\\New Videos";
        std::string mobileDir = "F:\\Stripchat Mobile";
        std::string vrDir = "F:\\Stripchat VR";
        std::string normalDir = "F:\\Stripchat Normal";

        // Options
        bool autoSortOnStartup = false;
        bool createSymlinks = true;
    };

    // ── Sort result for a single file ──────────────────────────────

    struct SortResult
    {
        fs::path sourcePath;
        fs::path destPath;
        std::string modelName;
        std::string tag;        // e.g. "[SC]"
        std::string folderType; // "VR" or "NO VR"
        bool isMobile = false;
        bool success = false;
        bool skipped = false; // already exists at destination
        std::string error;
    };

    // ── Public API ─────────────────────────────────────────────────

    // Load config.json
    bool loadConfig(const std::string &path, SorterConfig &cfg, std::string &error);

    // Save config (directories + options only — VR/Desktop/Aliases stay in config.json)
    bool saveAppConfig(const std::string &path, const SorterConfig &cfg, std::string &error);
    bool loadAppConfig(const std::string &path, SorterConfig &cfg);

    // Extract model name from a folder/path string, resolving aliases
    std::string extractModelName(const std::string &folderName, const SorterConfig &cfg, bool removeTags = true);

    // Find model name in a full path (looks for "ModelName [Tag]" pattern)
    std::string findModelName(const std::string &path, const SorterConfig &cfg, bool cleaned = false);

    // Extract tag like "[SC]" from a string
    std::string extractTag(const std::string &str);

    // Identify VR vs Desktop by tag
    std::string identifyFolderType(const std::string &str, const SorterConfig &cfg);

    // Sort all files from unsorted directory
    // logCb is called for each action. Returns list of results.
    using LogCallback = std::function<void(const std::string &)>;
    std::vector<SortResult> sortFiles(const SorterConfig &cfg, LogCallback logCb = nullptr);

    // Create symlinks for Mobile, VR, Normal views from the sorted directory
    void createViewSymlinks(const SorterConfig &cfg, LogCallback logCb = nullptr);

} // namespace sorter
