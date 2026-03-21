// Sorter — File sorting engine implementation
// Direct C++ port of NewSorter.py with overwrite protection + improvements.

#include "sorter.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <ctime>
#include <array>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sorter
{
    using json = nlohmann::json;

    // ── Month names ────────────────────────────────────────────────

    static const std::array<const char *, 12> kMonths = {
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};

    // ── Config loading ─────────────────────────────────────────────

    bool loadConfig(const std::string &path, SorterConfig &cfg, std::string &error)
    {
        try
        {
            std::ifstream f(path);
            if (!f.is_open())
            {
                error = "Cannot open config: " + path;
                return false;
            }

            json j;
            f >> j;

            // VR tags
            cfg.vrTags.clear();
            if (j.contains("VR") && j["VR"].is_array())
            {
                for (auto &v : j["VR"])
                    cfg.vrTags.push_back(v.get<std::string>());
            }

            // Desktop tags
            cfg.desktopTags.clear();
            if (j.contains("Desktop") && j["Desktop"].is_array())
            {
                for (auto &v : j["Desktop"])
                    cfg.desktopTags.push_back(v.get<std::string>());
            }

            // Aliases
            cfg.aliases.clear();
            if (j.contains("Aliases") && j["Aliases"].is_object())
            {
                for (auto &[key, val] : j["Aliases"].items())
                {
                    std::vector<std::string> aliasList;
                    if (val.is_array())
                    {
                        for (auto &a : val)
                            aliasList.push_back(a.get<std::string>());
                    }
                    cfg.aliases[key] = std::move(aliasList);
                }
            }

            // Phone clips
            cfg.phoneClips.clear();
            if (j.contains("PhoneClips") && j["PhoneClips"].is_array())
            {
                for (auto &v : j["PhoneClips"])
                    cfg.phoneClips.push_back(v.get<std::string>());
            }

            return true;
        }
        catch (const std::exception &e)
        {
            error = "JSON parse error: " + std::string(e.what());
            return false;
        }
    }

    // ── App config (directories + options, saved next to exe) ──────

    bool saveAppConfig(const std::string &path, const SorterConfig &cfg, std::string &error)
    {
        try
        {
            json j;
            j["configPath"] = cfg.configPath;
            j["unsortedDir"] = cfg.unsortedDir;
            j["sortedDir"] = cfg.sortedDir;
            j["newVideosDir"] = cfg.newVideosDir;
            j["mobileDir"] = cfg.mobileDir;
            j["vrDir"] = cfg.vrDir;
            j["normalDir"] = cfg.normalDir;
            j["autoSortOnStartup"] = cfg.autoSortOnStartup;
            j["createSymlinks"] = cfg.createSymlinks;
            std::ofstream f(path);
            f << j.dump(2);
            return true;
        }
        catch (const std::exception &e)
        {
            error = e.what();
            return false;
        }
    }

    bool loadAppConfig(const std::string &path, SorterConfig &cfg)
    {
        try
        {
            std::ifstream f(path);
            if (!f.is_open())
                return false;
            json j;
            f >> j;
            if (j.contains("configPath"))
                cfg.configPath = j["configPath"].get<std::string>();
            if (j.contains("unsortedDir"))
                cfg.unsortedDir = j["unsortedDir"].get<std::string>();
            if (j.contains("sortedDir"))
                cfg.sortedDir = j["sortedDir"].get<std::string>();
            if (j.contains("newVideosDir"))
                cfg.newVideosDir = j["newVideosDir"].get<std::string>();
            if (j.contains("mobileDir"))
                cfg.mobileDir = j["mobileDir"].get<std::string>();
            if (j.contains("vrDir"))
                cfg.vrDir = j["vrDir"].get<std::string>();
            if (j.contains("normalDir"))
                cfg.normalDir = j["normalDir"].get<std::string>();
            if (j.contains("autoSortOnStartup"))
                cfg.autoSortOnStartup = j["autoSortOnStartup"].get<bool>();
            if (j.contains("createSymlinks"))
                cfg.createSymlinks = j["createSymlinks"].get<bool>();
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    // ── Name extraction ────────────────────────────────────────────

    std::string extractModelName(const std::string &folderName, const SorterConfig &cfg, bool removeTags)
    {
        std::string result = folderName;

        // Remove VR and Desktop tags
        if (removeTags)
        {
            for (auto &tag : cfg.vrTags)
            {
                auto pos = result.find(tag);
                if (pos != std::string::npos)
                    result.erase(pos, tag.size());
            }
            for (auto &tag : cfg.desktopTags)
            {
                auto pos = result.find(tag);
                if (pos != std::string::npos)
                    result.erase(pos, tag.size());
            }
            // Trim whitespace
            while (!result.empty() && result.back() == ' ')
                result.pop_back();
            while (!result.empty() && result.front() == ' ')
                result.erase(result.begin());
        }

        // Replace aliases
        for (auto &[model, aliasList] : cfg.aliases)
        {
            for (auto &alias : aliasList)
            {
                if (result.find(alias) != std::string::npos)
                {
                    auto pos = result.find(alias);
                    result.replace(pos, alias.size(), model);
                    return result; // only first match
                }
            }
        }

        return result;
    }

    std::string findModelName(const std::string &path, const SorterConfig &cfg, bool cleaned)
    {
        // Find pattern "ModelName [Tag]" in the path
        std::regex re(R"(([^\\\/]+?\[.*?\]))");
        std::smatch match;
        if (std::regex_search(path, match, re))
        {
            std::string modelName = match[1].str();
            if (cleaned)
                return extractModelName(modelName, cfg, true);
            return modelName;
        }
        return "";
    }

    std::string extractTag(const std::string &str)
    {
        std::regex re(R"(\[[^\]]+\])");
        std::smatch match;
        if (std::regex_search(str, match, re))
            return match[0].str();
        return "";
    }

    std::string identifyFolderType(const std::string &str, const SorterConfig &cfg)
    {
        for (auto &tag : cfg.vrTags)
            if (str.find(tag) != std::string::npos)
                return "VR";
        for (auto &tag : cfg.desktopTags)
            if (str.find(tag) != std::string::npos)
                return "NO VR";
        return "";
    }

    // ── File sorting ───────────────────────────────────────────────

    static bool isVideoExt(const fs::path &p)
    {
        auto ext = p.extension().string();
        for (auto &c : ext)
            c = (char)std::tolower((unsigned char)c);
        return ext == ".mp4" || ext == ".mkv" || ext == ".ts" || ext == ".avi";
    }

    static void sortRecursive(const fs::path &dir, const SorterConfig &cfg,
                              std::vector<SortResult> &results, LogCallback logCb)
    {
        if (!fs::exists(dir))
            return;

        for (auto &entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied))
        {
            if (entry.is_directory())
            {
                sortRecursive(entry.path(), cfg, results, logCb);
                continue;
            }

            if (!entry.is_regular_file() || !isVideoExt(entry.path()))
                continue;

            SortResult sr;
            sr.sourcePath = entry.path();

            std::string fileName = entry.path().filename().string();
            std::string parentName = entry.path().parent_path().filename().string();
            sr.isMobile = (parentName == "Mobile");

            // Find model name from path
            std::string pathStr = entry.path().string();
            std::string modelCleaned = findModelName(pathStr, cfg, true);
            std::string modelRaw = findModelName(pathStr, cfg, false);

            if (modelCleaned.empty())
            {
                sr.error = "Cannot extract model name";
                sr.success = false;
                results.push_back(std::move(sr));
                if (logCb)
                    logCb("[WARNING] Cannot extract model name: " + fileName);
                continue;
            }

            sr.modelName = modelCleaned;
            sr.tag = extractTag(modelRaw);

            // Identify VR vs Desktop
            sr.folderType = identifyFolderType(modelRaw, cfg);
            if (sr.folderType.empty())
            {
                sr.error = "Cannot identify folder type (VR/Desktop)";
                sr.success = false;
                results.push_back(std::move(sr));
                if (logCb)
                    logCb("[WARNING] Cannot identify folder type: " + fileName);
                continue;
            }

            // Parse date from filename: DD-MM-YYYY-...
            std::string baseName = entry.path().stem().string();
            int day = 0, month = 0, year = 0;
            if (sscanf(baseName.c_str(), "%d-%d-%d", &day, &month, &year) != 3 ||
                month < 1 || month > 12 || day < 1 || day > 31 || year < 2000)
            {
                sr.error = "Filename doesn't match DD-MM-YYYY format";
                sr.success = false;
                results.push_back(std::move(sr));
                if (logCb)
                    logCb("[WARNING] Bad date format: " + fileName);
                continue;
            }

            std::string monthName = kMonths[month - 1];
            std::string yearStr = std::to_string(year);

            // Build target path: SortedDir/ModelName/Year/Month/VR|NO VR/[Mobile/]day.ext
            auto ext = entry.path().extension().string();
            fs::path basePath = fs::path(cfg.sortedDir) / modelCleaned / yearStr / monthName / sr.folderType;

            fs::path targetPath;
            if (sr.isMobile)
                targetPath = basePath / "Mobile" / (std::to_string(day) + ext);
            else
                targetPath = basePath / (std::to_string(day) + ext);

            // ── OVERWRITE PROTECTION ──
            // If target already exists, try appending the tag
            if (fs::exists(targetPath))
            {
                std::string newName = std::to_string(day) + " " + sr.tag + ext;
                if (sr.isMobile)
                    targetPath = basePath / "Mobile" / newName;
                else
                    targetPath = basePath / newName;

                // If STILL exists, skip entirely — don't overwrite!
                if (fs::exists(targetPath))
                {
                    sr.destPath = targetPath;
                    sr.skipped = true;
                    sr.success = false;
                    sr.error = "Already exists at destination";
                    results.push_back(std::move(sr));
                    if (logCb)
                        logCb("[SKIP] Already sorted: " + fileName + " -> " + targetPath.string());
                    continue;
                }
            }

            sr.destPath = targetPath;

            // Create directories and move
            try
            {
                fs::create_directories(targetPath.parent_path());
                fs::rename(entry.path(), targetPath);
                sr.success = true;
                if (logCb)
                    logCb("[OK] " + fileName + " -> " + targetPath.string());
            }
            catch (const std::exception &e)
            {
                // fs::rename fails across drives — fall back to copy+delete
                try
                {
                    fs::copy_file(entry.path(), targetPath, fs::copy_options::none);
                    fs::remove(entry.path());
                    sr.success = true;
                    if (logCb)
                        logCb("[OK] " + fileName + " -> " + targetPath.string() + " (copied)");
                }
                catch (const std::exception &e2)
                {
                    sr.error = e2.what();
                    sr.success = false;
                    if (logCb)
                        logCb("[ERROR] " + fileName + ": " + e2.what());
                }
            }

            results.push_back(std::move(sr));

            // Clean up empty parent directories
            if (sr.success)
            {
                fs::path parent = entry.path().parent_path();
                fs::path unsorted(cfg.unsortedDir);
                while (parent != unsorted && parent.has_parent_path())
                {
                    try
                    {
                        if (fs::is_empty(parent))
                        {
                            fs::remove(parent);
                            if (logCb)
                                logCb("[INFO] Removed empty dir: " + parent.string());
                        }
                        else
                            break;
                    }
                    catch (...)
                    {
                        break;
                    }
                    parent = parent.parent_path();
                }
            }

            // Create symlink in New Videos
            if (sr.success && cfg.createSymlinks)
            {
                try
                {
                    // Replace sorted dir prefix with new videos dir prefix
                    std::string destStr = targetPath.string();
                    std::string sortedPrefix = cfg.sortedDir;
                    if (destStr.substr(0, sortedPrefix.size()) == sortedPrefix)
                    {
                        fs::path symlinkPath = fs::path(cfg.newVideosDir) /
                                               destStr.substr(sortedPrefix.size() + 1);
                        fs::create_directories(symlinkPath.parent_path());
                        if (!fs::exists(symlinkPath))
                            fs::create_symlink(targetPath, symlinkPath);
                    }
                }
                catch (const std::exception &e)
                {
                    if (logCb)
                        logCb("[WARNING] Symlink failed: " + std::string(e.what()));
                }
            }
        }
    }

    std::vector<SortResult> sortFiles(const SorterConfig &cfg, LogCallback logCb)
    {
        std::vector<SortResult> results;

        if (!fs::is_directory(cfg.unsortedDir))
        {
            if (logCb)
                logCb("[ERROR] Unsorted directory not found: " + cfg.unsortedDir);
            return results;
        }

        if (logCb)
            logCb("[INFO] Sorting files from " + cfg.unsortedDir + " ...");

        sortRecursive(fs::path(cfg.unsortedDir), cfg, results, logCb);

        if (logCb)
        {
            int ok = 0, fail = 0, skip = 0;
            for (auto &r : results)
            {
                if (r.success)
                    ok++;
                else if (r.skipped)
                    skip++;
                else
                    fail++;
            }
            logCb("[OK] Sort complete — " + std::to_string(ok) + " moved, " +
                  std::to_string(skip) + " skipped, " + std::to_string(fail) + " failed");
        }

        return results;
    }

    // ── View symlinks ──────────────────────────────────────────────

    static void createSymlinksForView(const fs::path &sortedDir, const fs::path &viewDir,
                                      const std::string &subdir, LogCallback logCb)
    {
        if (!fs::is_directory(sortedDir))
            return;
        fs::create_directories(viewDir);

        for (auto &model : fs::directory_iterator(sortedDir))
        {
            if (!model.is_directory())
                continue;
            for (auto &year : fs::directory_iterator(model.path()))
            {
                if (!year.is_directory())
                    continue;
                for (auto &month : fs::directory_iterator(year.path()))
                {
                    if (!month.is_directory())
                        continue;
                    fs::path target = month.path() / subdir;
                    if (!fs::is_directory(target))
                        continue;
                    for (auto &f : fs::directory_iterator(target))
                    {
                        if (!f.is_regular_file())
                            continue;
                        auto ext = f.path().extension().string();
                        for (auto &c : ext)
                            c = (char)std::tolower((unsigned char)c);
                        if (ext != ".mkv" && ext != ".mp4")
                            continue;

                        fs::path dst = viewDir / model.path().filename() /
                                       year.path().filename() / month.path().filename() /
                                       f.path().filename();
                        try
                        {
                            fs::create_directories(dst.parent_path());
                            if (!fs::exists(dst))
                                fs::create_symlink(f.path(), dst);
                        }
                        catch (...)
                        {
                        }
                    }
                }
            }
        }
    }

    void createViewSymlinks(const SorterConfig &cfg, LogCallback logCb)
    {
        fs::path sorted(cfg.sortedDir);

        // Clear and recreate symlink directories
        auto clearAndCreate = [&](const std::string &dir)
        {
            if (fs::exists(dir))
            {
                try
                {
                    fs::remove_all(dir);
                }
                catch (...)
                {
                }
            }
            fs::create_directories(dir);
        };

        if (logCb)
            logCb("[INFO] Creating view symlinks...");

        clearAndCreate(cfg.mobileDir);
        clearAndCreate(cfg.vrDir);
        clearAndCreate(cfg.normalDir);

        // Mobile: NO VR/Mobile/
        // Need special handling — traverse into NO VR/Mobile subdirectory
        if (fs::is_directory(sorted))
        {
            for (auto &model : fs::directory_iterator(sorted))
            {
                if (!model.is_directory())
                    continue;
                for (auto &year : fs::directory_iterator(model.path()))
                {
                    if (!year.is_directory())
                        continue;
                    for (auto &month : fs::directory_iterator(year.path()))
                    {
                        if (!month.is_directory())
                            continue;
                        fs::path mobileSubdir = month.path() / "NO VR" / "Mobile";
                        if (!fs::is_directory(mobileSubdir))
                            continue;
                        for (auto &f : fs::directory_iterator(mobileSubdir))
                        {
                            if (!f.is_regular_file())
                                continue;
                            fs::path dst = fs::path(cfg.mobileDir) / model.path().filename() /
                                           year.path().filename() / month.path().filename() /
                                           f.path().filename();
                            try
                            {
                                fs::create_directories(dst.parent_path());
                                if (!fs::exists(dst))
                                    fs::create_symlink(f.path(), dst);
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }
            }
        }

        // VR
        createSymlinksForView(sorted, fs::path(cfg.vrDir), "VR", logCb);

        // Normal (NO VR, excluding Mobile subfolder — handled by direct file listing)
        if (fs::is_directory(sorted))
        {
            fs::create_directories(cfg.normalDir);
            for (auto &model : fs::directory_iterator(sorted))
            {
                if (!model.is_directory())
                    continue;
                for (auto &year : fs::directory_iterator(model.path()))
                {
                    if (!year.is_directory())
                        continue;
                    for (auto &month : fs::directory_iterator(year.path()))
                    {
                        if (!month.is_directory())
                            continue;
                        fs::path noVr = month.path() / "NO VR";
                        if (!fs::is_directory(noVr))
                            continue;
                        for (auto &f : fs::directory_iterator(noVr))
                        {
                            if (!f.is_regular_file())
                                continue;
                            auto ext = f.path().extension().string();
                            for (auto &c : ext)
                                c = (char)std::tolower((unsigned char)c);
                            if (ext != ".mkv" && ext != ".mp4")
                                continue;

                            fs::path dst = fs::path(cfg.normalDir) / model.path().filename() /
                                           year.path().filename() / month.path().filename() /
                                           f.path().filename();
                            try
                            {
                                fs::create_directories(dst.parent_path());
                                if (!fs::exists(dst))
                                    fs::create_symlink(f.path(), dst);
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                }
            }
        }

        if (logCb)
            logCb("[OK] View symlinks created");
    }

} // namespace sorter
