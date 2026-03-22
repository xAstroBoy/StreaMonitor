// ThumbnailTool — App implementation (SM-themed, multithreaded)
#include "app.h"
#include "utils/thumbnail_generator.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace tt
{
    // ── Color palette (matches StreaMonitor) ────────────────────────
    static const ImVec4 COL_ACCENT = {0.35f, 0.55f, 1.00f, 1.0f};
    static const ImVec4 COL_GREEN = {0.30f, 0.90f, 0.40f, 1.0f};
    static const ImVec4 COL_RED = {1.00f, 0.35f, 0.35f, 1.0f};
    static const ImVec4 COL_YELLOW = {1.00f, 0.80f, 0.20f, 1.0f};
    static const ImVec4 COL_DIM = {0.50f, 0.50f, 0.55f, 1.0f};
    static const ImVec4 COL_TEXT = {0.92f, 0.92f, 0.94f, 1.0f};
    static const ImU32 COL_BADGE_OK = IM_COL32(50, 180, 80, 255);
    static const ImU32 COL_BADGE_ERR = IM_COL32(220, 60, 60, 255);
    static const ImU32 COL_BADGE_PEND = IM_COL32(200, 170, 40, 255);
    static const ImU32 COL_BADGE_SKIP = IM_COL32(90, 90, 100, 255);
    static const ImU32 COL_STATUS_BG = IM_COL32(18, 18, 24, 255);

    // ── Helpers ─────────────────────────────────────────────────────

    static bool isVideoFile(const fs::path &p)
    {
        auto ext = p.extension().string();
        for (auto &c : ext)
            c = static_cast<char>(std::tolower(c));
        for (auto *ve : App::kVideoExts)
            if (ext == ve)
                return true;
        return false;
    }

    static fs::path thumbPathForVideo(const fs::path &video)
    {
        auto p = video;
        p.replace_extension(".jpg");
        return p;
    }

    static void DrawBadge(const char *text, ImU32 col)
    {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImVec2 size = ImGui::CalcTextSize(text);
        float pad = 6.0f;
        ImVec2 mn = {pos.x, pos.y + 1};
        ImVec2 mx = {pos.x + size.x + pad * 2, pos.y + size.y + pad};
        ImGui::GetWindowDrawList()->AddRectFilled(mn, mx, col, 4.0f);
        ImGui::SetCursorScreenPos({mn.x + pad, mn.y + pad * 0.35f});
        ImGui::TextUnformatted(text);
        ImGui::SameLine(0, 8);
    }

    static std::string formatSize(int64_t bytes)
    {
        char buf[32];
        if (bytes <= 0)
            return "0 B";
        if (bytes >= 1073741824LL)
            snprintf(buf, sizeof(buf), "%.2f GB", bytes / 1073741824.0);
        else if (bytes >= 1048576LL)
            snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
        else
            snprintf(buf, sizeof(buf), "%lld KB", (long long)(bytes / 1024));
        return buf;
    }

    // ── Constructor / Destructor ────────────────────────────────────

    App::App()
    {
        std::string defaultDir = "F:\\StripChat";
        std::strncpy(rootDir_, defaultDir.c_str(), sizeof(rootDir_) - 1);
        loadConfig();
        addLog("[INFO] ThumbnailTool ready — set root directory and click Scan");
    }

    App::~App()
    {
        cancelWork_.store(true);
        if (scanThread_.joinable())
            scanThread_.join();
        for (auto &w : workers_)
            if (w.joinable())
                w.join();
        workers_.clear();
    }

    void App::addLog(const std::string &line)
    {
        std::lock_guard lock(logMtx_);
        log_.push_back(line);
        while (log_.size() > 2000)
            log_.pop_front();
    }

    // ── Config Persistence ──────────────────────────────────────────

    fs::path App::configPath() const
    {
#ifdef _WIN32
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path() / "thumbnail_config.json";
#else
        return fs::current_path() / "thumbnail_config.json";
#endif
    }

    void App::loadConfig()
    {
        auto cfgPath = configPath();
        if (!fs::exists(cfgPath))
            return;
        try
        {
            std::ifstream f(cfgPath);
            json j;
            f >> j;
            if (j.contains("rootDir"))
            {
                std::string rd = j["rootDir"].get<std::string>();
                std::strncpy(rootDir_, rd.c_str(), sizeof(rootDir_) - 1);
            }
            if (j.contains("thumbnailWidth"))
                thumbnailWidth.store(j["thumbnailWidth"].get<int>());
            if (j.contains("thumbnailColumns"))
                thumbnailColumns.store(j["thumbnailColumns"].get<int>());
            if (j.contains("thumbnailRows"))
                thumbnailRows.store(j["thumbnailRows"].get<int>());
            if (j.contains("threadCount"))
                threadCount.store(j["threadCount"].get<int>());
            if (j.contains("embedInVideo"))
                embedInVideo.store(j["embedInVideo"].get<bool>());
            if (j.contains("hideFinished"))
                hideFinished_ = j["hideFinished"].get<bool>();
            if (j.contains("showLog"))
                showLog_ = j["showLog"].get<bool>();
            addLog("[INFO] Loaded settings from " + cfgPath.string());
        }
        catch (const std::exception &e)
        {
            addLog("[WARNING] Failed to load config: " + std::string(e.what()));
        }
    }

    void App::saveConfig()
    {
        auto cfgPath = configPath();
        try
        {
            json j;
            j["rootDir"] = std::string(rootDir_);
            j["thumbnailWidth"] = thumbnailWidth.load();
            j["thumbnailColumns"] = thumbnailColumns.load();
            j["thumbnailRows"] = thumbnailRows.load();
            j["threadCount"] = threadCount.load();
            j["embedInVideo"] = embedInVideo.load();
            j["hideFinished"] = hideFinished_;
            j["showLog"] = showLog_;
            std::ofstream f(cfgPath);
            f << j.dump(2);
            addLog("[INFO] Settings saved to " + cfgPath.string());
        }
        catch (const std::exception &e)
        {
            addLog("[ERROR] Failed to save config: " + std::string(e.what()));
        }
    }

    // ── Scan ────────────────────────────────────────────────────────

    void App::startScan()
    {
        // Don't allow overlapping scans or scan while generating
        if (scanning_.load() || working_.load())
            return;

        // Wait for any previous scan thread to finish
        if (scanThread_.joinable())
            scanThread_.join();

        // Reset state on GUI thread (lightweight, no I/O)
        {
            std::lock_guard lock(videosMutex_);
            videos_.clear();
        }
        totalVideos_ = 0;
        withThumb_ = 0;
        withoutThumb_ = 0;
        generated_.store(0);
        errors_.store(0);
        scanProgress_.store(0);
        scanned_ = false;

        // Copy root dir so the thread has its own string
        std::string rootStr(rootDir_);

        fs::path root(rootStr);
        if (!fs::is_directory(root))
        {
            addLog("[ERROR] Not a valid directory: " + rootStr);
            return;
        }

        addLog("[INFO] Scanning " + root.string() + " ...");
        scanning_.store(true);

        // Launch background thread — GUI stays responsive
        scanThread_ = std::jthread([this, rootStr]()
                                   { scanWorker(); });
    }

    void App::scanWorker()
    {
        fs::path root(rootDir_);

        int count = 0;
        int wThumb = 0;
        int woThumb = 0;

        for (auto &entry : fs::recursive_directory_iterator(root,
                                                            fs::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file())
                continue;
            if (!isVideoFile(entry.path()))
                continue;

            // Skip temp remux files
            auto fname = entry.path().stem().string();
            if (fname.size() > 8 && fname.substr(fname.size() - 8) == "~remuxed")
                continue;
            auto fnameExt = entry.path().filename().string();
            if (fnameExt.find(".remux.mkv") != std::string::npos)
                continue;

            VideoEntry ve;
            ve.videoPath = entry.path();
            ve.thumbPath = thumbPathForVideo(entry.path());
            ve.relDisplay = fs::relative(entry.path(), root).string();
            ve.hasThumb = fs::exists(ve.thumbPath);

            // Verify actual container type — don't trust the extension!
            auto ext = entry.path().extension().string();
            for (auto &c : ext)
                c = (char)std::tolower((unsigned char)c);
            if (ext == ".mkv")
            {
                ve.container = sm::isRealMatroska(entry.path().string())
                                   ? ContainerType::RealMKV
                                   : ContainerType::FakeMKV;
            }
            else
            {
                ve.container = ContainerType::Other;
            }

            // For Real MKV files, check if cover art is already embedded
            // This is fast (metadata only) and needed to detect already-processed files
            if (ve.container == ContainerType::RealMKV)
            {
                ve.hasCoverEmbed = sm::hasCoverArt(entry.path().string());
                ve.coverProbed = true;
                // Check for the definitive THUMBNAILED metadata tag
                ve.hasTag = sm::hasProcessedTag(entry.path().string());
            }
            else
            {
                ve.hasCoverEmbed = false;
                ve.coverProbed = false;
                ve.hasTag = false;
            }

            // Collect file size (fast — cached in directory entry on Windows)
            try
            {
                ve.fileSize = (int64_t)entry.file_size();
            }
            catch (...)
            {
                ve.fileSize = 0;
            }

            count++;
            // Count as "with thumbnail" if the processed tag is present, or sidecar .jpg exists, or cover is embedded
            if (ve.hasTag || ve.hasThumb || ve.hasCoverEmbed)
                wThumb++;
            else
                woThumb++;

            // Push into shared list (GUI reads this for live table updates)
            {
                std::lock_guard lock(videosMutex_);
                videos_.push_back(std::move(ve));
            }

            // Update live progress for the toolbar
            scanProgress_.store(count);
        }

        // Finalize — sort: MKV first, then by filesize descending (matches work queue order)
        {
            std::lock_guard lock(videosMutex_);
            std::sort(videos_.begin(), videos_.end(),
                      [](const VideoEntry &a, const VideoEntry &b)
                      {
                          if (a.container != b.container)
                              return (int)a.container < (int)b.container; // MKV first
                          return a.fileSize > b.fileSize; // largest first
                      });
        }

        // Set counters so render() picks them up
        totalVideos_ = count;
        withThumb_ = wThumb;
        withoutThumb_ = woThumb;
        scanned_ = true;
        scanning_.store(false);

        addLog("[OK] Found " + std::to_string(count) + " videos (" +
               std::to_string(wThumb) + " with thumbnails, " +
               std::to_string(woThumb) + " missing)");
    }

    // ── Generation (multi-threaded) ─────────────────────────────────

    void App::startGeneration()
    {
        if (working_.load())
            return;

        cancelWork_.store(false);
        processedCount_.store(0);
        generated_.store(0);
        errors_.store(0);
        remuxedCount_.store(0);
        embeddedCount_.store(0);
        vrCount_.store(0);
        nextIdx_.store(0);
        bytesProcessed_.store(0);
        etaLastBytes_ = 0;
        etaRollingBps_ = 0;

        // Count ALL unprocessed videos and calculate total bytes
        // Skip files that are already done (processed this run, OR have the THUMBNAILED metadata tag)
        int count = 0;
        int64_t totalB = 0;
        int skipped = 0;
        {
            std::lock_guard lock(videosMutex_);
            for (auto &v : videos_)
            {
                // Skip if already processed this run
                if (v.processed)
                    continue;
                // Skip if has the THUMBNAILED metadata tag (processed by a previous run)
                // UNLESS it's been queued for regeneration
                if (v.hasTag && !v.regenQueued)
                {
                    v.processed = true; // mark so UI shows as Done
                    skipped++;
                    continue;
                }
                count++;
                totalB += v.fileSize;
            }
        }
        totalToProcess_.store(count);
        totalBytes_.store(totalB);

        if (skipped > 0)
            addLog("[INFO] Skipped " + std::to_string(skipped) +
                   " already-processed files (THUMBNAILED tag found)");

        if (count == 0)
        {
            addLog("[INFO] Nothing to process — all videos done");
            return;
        }

        int numThreads = std::clamp(threadCount.load(), 1, kMaxThreads);
        addLog("[INFO] Processing " + std::to_string(count) +
               " videos with " + std::to_string(numThreads) + " threads...");

        working_.store(true);
        startTime_ = std::chrono::steady_clock::now();
        etaLastTime_ = startTime_;

        // Clear thread progress
        for (int i = 0; i < kMaxThreads; i++)
        {
            threadProgress_[i].active.store(false);
            std::lock_guard lock(threadProgress_[i].mtx);
            threadProgress_[i].filePath.clear();
            threadProgress_[i].action.clear();
            threadProgress_[i].subAction.clear();
            threadProgress_[i].fileSize = 0;
        }

        // Launch worker threads with their thread index
        activeWorkers_.store(numThreads);
        lastThreadCount_.store(numThreads);
        {
            std::lock_guard lock(workerMutex_);
            workers_.clear();
            for (int t = 0; t < numThreads; t++)
                workers_.emplace_back([this, t]()
                                      { workerFunc(t); });
        }
    }

    void App::workerFunc(int threadIdx)
    {
        // Build sorted work queue: MKV first (no remux needed = stable I/O),
        // then other containers. Within each group, sort by file size DESCENDING
        // (largest first → better thread utilization, finishes big files early,
        // avoids the "one huge file left at the end" stall).
        struct WorkItem
        {
            size_t idx;
            int64_t fileSize;
            ContainerType container;
        };
        std::vector<WorkItem> workItems;
        {
            std::lock_guard lock(videosMutex_);
            for (size_t i = 0; i < videos_.size(); i++)
            {
                auto &v = videos_[i];
                // Skip already processed
                if (v.processed)
                    continue;
                // Skip if has processed tag (unless queued for regen)
                if (v.hasTag && !v.regenQueued)
                    continue;
                workItems.push_back({i, v.fileSize, v.container});
            }
        }
        // Sort: RealMKV first (container=0), then FakeMKV (1), then Other (2).
        // Within each group, largest files first (descending by size).
        std::sort(workItems.begin(), workItems.end(),
                  [](const WorkItem &a, const WorkItem &b)
                  {
                      if (a.container != b.container)
                          return (int)a.container < (int)b.container; // MKV first
                      return a.fileSize > b.fileSize; // largest first
                  });

        std::vector<size_t> indices;
        indices.reserve(workItems.size());
        for (auto &wi : workItems)
            indices.push_back(wi.idx);

        // Helper to update thread progress
        auto setProgress = [this, threadIdx](const std::string &file, const std::string &action, const std::string &sub = "", int64_t fsize = 0)
        {
            std::lock_guard lock(threadProgress_[threadIdx].mtx);
            threadProgress_[threadIdx].filePath = file;
            threadProgress_[threadIdx].action = action;
            threadProgress_[threadIdx].subAction = sub;
            if (fsize > 0)
            {
                threadProgress_[threadIdx].fileSize = fsize;
                threadProgress_[threadIdx].startTime = std::chrono::steady_clock::now();
            }
        };

        // Mark thread as active
        threadProgress_[threadIdx].active.store(true);

        // ═══════════════════════════════════════════════════════════════
        // PIPELINE PER VIDEO:
        //   1. Not MKV? → Remux to .mkv  (stream copy, fixes ALL playback)
        //      Fake MKV? → Remux in-place (stream copy, fixes ALL playback)
        //   2. Is MKV → Timestamps broken? → Remux in-place (fixes playback)
        //   3. Has thumbnail? → No → Generate contact sheet
        //   4. Embed cover art in MKV (if not already embedded)
        //   5. Is VR? → Has fisheye 180° SBS metadata? → No → Inject it!
        // ═══════════════════════════════════════════════════════════════

        // Helper to check cancel and exit early
        auto shouldCancel = [this]()
        { return cancelWork_.load(); };

        // Helper to check if this thread should exit (thread count reduced)
        auto shouldExitThread = [this, threadIdx]()
        {
            return threadIdx >= threadCount.load();
        };

        while (!shouldCancel())
        {
            // Check if thread count was reduced - if so, exit gracefully BETWEEN files
            if (shouldExitThread())
            {
                addLog("[INFO] Thread " + std::to_string(threadIdx + 1) + " exiting (thread count reduced)");
                break;
            }

            int myIdx = nextIdx_.fetch_add(1);
            if (myIdx >= (int)indices.size())
                break;

            // Read settings FRESH for each file (allows live config changes)
            sm::ThumbnailConfig tc;
            tc.width = thumbnailWidth.load();
            tc.columns = thumbnailColumns.load();
            tc.rows = thumbnailRows.load();
            bool doEmbed = embedInVideo.load();

            size_t idx = indices[myIdx];
            std::string videoStr, thumbStr, origVideoStr;
            int64_t fsize = 0;
            bool alreadyHasJpg = false;
            ContainerType origContainer;
            {
                std::lock_guard lock(videosMutex_);
                videoStr = videos_[idx].videoPath.string();
                thumbStr = videos_[idx].thumbPath.string();
                alreadyHasJpg = videos_[idx].hasThumb;
                fsize = videos_[idx].fileSize;
                origContainer = videos_[idx].container;
            }
            origVideoStr = videoStr; // keep original path for VR folder detection

            // Update thread progress
            setProgress(videoStr, "Starting", formatSize(fsize), fsize);

            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_ = videoStr;
            }

            auto logCb = [this](const std::string &msg)
            { addLog("  " + msg); };

            // Progress callback for I/O tracking during remux
            auto progressCb = [this, threadIdx](int64_t bytesWritten)
            { threadProgress_[threadIdx].bytesProcessed.store(bytesWritten); };

            addLog("[INFO] Processing: " + videoStr +
                   " (" + formatSize(fsize) + ")");

            bool tsFixed = false;
            bool wasRemuxed = false;

            // ── STEP 1: Ensure real Matroska container ──────────────────
            // Not MKV? → Remux automatically to .mkv (stream copy, fixes ALL playback)
            // Fake MKV (MP4 inside)? → Remux in-place (stream copy, fixes ALL playback)
            // Real MKV? → Already good, continue to timestamp check
            if (shouldCancel())
                break; // Early exit check
            if (origContainer != ContainerType::RealMKV)
            {
                setProgress(videoStr, "Remuxing", "Converting to MKV...");
                threadProgress_[threadIdx].bytesProcessed.store(0); // Reset for new remux
                addLog("[INFO] Remuxing to MKV: " + videoStr);
                std::string mkvPath = sm::ensureRealMKV(videoStr, logCb, shouldCancel, progressCb);
                if (mkvPath.empty())
                {
                    // Clean up orphaned temp and thumbnail files
                    {
                        std::error_code ec;
                        auto stem = fs::path(videoStr).stem().string();
                        auto dir = fs::path(videoStr).parent_path();
                        fs::path tmpPath = dir / (stem + "~remuxed.mkv");
                        if (fs::exists(tmpPath))
                            fs::remove(tmpPath, ec);
                        if (fs::exists(thumbStr))
                            fs::remove(thumbStr, ec);
                    }
                    {
                        std::lock_guard lock(videosMutex_);
                        videos_[idx].processed = true;
                        videos_[idx].failed = true;
                        videos_[idx].errorMsg = "Remux failed";
                    }
                    errors_.fetch_add(1);
                    addLog("[ERROR] " + videoStr +
                           " — remux failed, cleaned up temp files");
                    processedCount_.fetch_add(1);
                    bytesProcessed_.fetch_add(fsize);
                    continue;
                }
                wasRemuxed = true;
                tsFixed = true; // remux inherently fixes timestamps (DTS/PTS normalised)
                remuxedCount_.fetch_add(1);
                setProgress(videoStr, "Remuxed", "Done");

                // Update paths if extension changed (e.g. .mp4 → .mkv)
                if (mkvPath != videoStr)
                {
                    auto newThumb = thumbPathForVideo(fs::path(mkvPath));
                    std::lock_guard lock(videosMutex_);
                    videos_[idx].videoPath = mkvPath;
                    videos_[idx].thumbPath = newThumb;
                    videos_[idx].container = ContainerType::RealMKV;
                    videos_[idx].remuxed = true;
                    videoStr = mkvPath;
                    thumbStr = newThumb.string();
                }
                else
                {
                    std::lock_guard lock(videosMutex_);
                    videos_[idx].container = ContainerType::RealMKV;
                    videos_[idx].remuxed = true;
                }
            }

            // ── STEP 2: Is MKV → Timestamps/frames broken? → Remux ─────
            // Only check if we didn't just remux (fresh remux = already fixed)
            if (shouldCancel())
                break; // Early exit check
            if (!wasRemuxed)
            {
                setProgress(videoStr, "Checking", "Analyzing timestamps...");
                if (sm::hasTimestampIssues(videoStr, logCb))
                {
                    setProgress(videoStr, "Fixing TS", "Remuxing to fix timestamps...");
                    threadProgress_[threadIdx].bytesProcessed.store(0); // Reset for new remux
                    addLog("[INFO] Timestamps broken → remuxing to fix: " +
                           videoStr);
                    if (sm::fixTimestamps(videoStr, logCb, shouldCancel, progressCb))
                    {
                        tsFixed = true;
                        wasRemuxed = true;
                        remuxedCount_.fetch_add(1);
                    }
                    else
                    {
                        addLog("[WARNING] TS fix failed: " + videoStr);
                    }
                }
            }

            // ── STEP 3: Has thumbnail? → No → Generate it! ─────────────
            // Check embedded cover art first (deferred probe)
            // If regenQueued, force regeneration regardless of existing cover
            if (shouldCancel())
                break; // Early exit check

            bool isRegen = false;
            bool fileHasTag = false;
            {
                std::lock_guard lock(videosMutex_);
                isRegen = videos_[idx].regenQueued;
                fileHasTag = videos_[idx].hasTag;
            }

            // If file is NOT tagged, treat it as needing full reprocessing:
            // regenerate thumbnail + re-embed + tag. Having an existing cover
            // without the THUMBNAILED tag means it was never fully processed.
            if (!fileHasTag && !isRegen)
            {
                isRegen = true;
                if (alreadyHasJpg)
                    addLog("[INFO] Not tagged — reprocessing: " + videoStr);
            }

            if (isRegen)
            {
                // Force regeneration — pretend there's no thumbnail
                alreadyHasJpg = false;
                if (videos_[idx].regenQueued)
                    addLog("[INFO] Regenerating thumbnail (user request): " + videoStr);
            }
            else if (!alreadyHasJpg)
            {
                setProgress(videoStr, "Probing", "Checking for cover art...");
                bool probed = false;
                {
                    std::lock_guard lock(videosMutex_);
                    probed = videos_[idx].coverProbed;
                }
                if (!probed)
                {
                    bool hasCover = sm::hasCoverArt(videoStr);
                    std::lock_guard lock(videosMutex_);
                    videos_[idx].coverProbed = true;
                    videos_[idx].hasCoverEmbed = hasCover;
                    if (hasCover)
                    {
                        videos_[idx].hasThumb = true;
                        alreadyHasJpg = true;
                        addLog("[INFO] Already has cover art: " +
                               videoStr);
                    }
                }
            }

            // Generate contact sheet if no thumbnail exists
            bool genOk = alreadyHasJpg;
            if (!alreadyHasJpg)
            {
                setProgress(videoStr, "Generating", "Creating contact sheet...");
                addLog("[INFO] Generating thumbnail: " + videoStr);
                genOk = sm::generateContactSheet(videoStr, thumbStr, tc, logCb, shouldCancel);
            }

            bool anyError = false;
            {
                std::lock_guard lock(videosMutex_);
                videos_[idx].tsFixed = tsFixed;
                if (genOk)
                {
                    videos_[idx].hasThumb = true;
                    if (!alreadyHasJpg)
                        generated_.fetch_add(1);
                }
                else if (!alreadyHasJpg)
                {
                    videos_[idx].failed = true;
                    videos_[idx].errorMsg = "Thumbnail generation failed";
                    errors_.fetch_add(1);
                    anyError = true;
                    addLog("[ERROR] Thumbnail failed: " + videoStr);
                }
            }

            // ── STEP 4: Embed cover art + clean up external .jpg ────────
            if (shouldCancel())
                break; // Early exit check
            if (genOk && doEmbed && !anyError)
            {
                setProgress(videoStr, "Embedding", "Checking cover art...");
                bool wasEmbedded = sm::hasCoverArt(videoStr);

                // For regeneration, force re-embed even if cover exists
                if (isRegen && wasEmbedded && fs::exists(thumbStr))
                {
                    setProgress(videoStr, "Embedding", "Replacing cover art...");
                    addLog("[INFO] Replacing cover art: " + videoStr);
                    if (sm::embedThumbnailInMKV(videoStr, thumbStr, logCb, "", true))
                    {
                        embeddedCount_.fetch_add(1);
                        wasEmbedded = true;
                    }
                }
                else if (!wasEmbedded && fs::exists(thumbStr))
                {
                    setProgress(videoStr, "Embedding", "Adding cover to MKV...");
                    addLog("[INFO] Embedding cover art: " + videoStr);
                    if (sm::embedThumbnailInMKV(videoStr, thumbStr, logCb, "", false))
                    {
                        embeddedCount_.fetch_add(1);
                        wasEmbedded = true;
                    }
                }
                else if (wasEmbedded && !isRegen)
                {
                    setProgress(videoStr, "Fix Meta", "Updating DLNA metadata...");
                    // Fix existing cover attachment metadata for DLNA compatibility
                    sm::fixCoverAttachmentMetadata(videoStr, logCb);
                }
                // Cover art is inside the MKV — remove the external .jpg
                if (wasEmbedded && fs::exists(thumbStr))
                {
                    std::error_code ec;
                    fs::remove(thumbStr, ec);
                    if (!ec)
                        addLog("[INFO] Cleaned up external .jpg (embedded in MKV)");
                }
            }

            // ── STEP 5: Is VR? → Has 180° SBS metadata? → Set it! ──────
            if (shouldCancel())
                break; // Early exit check
            if (sm::isVRFromPath(origVideoStr))
            {
                setProgress(videoStr, "VR Meta", "Injecting spatial metadata...");
                addLog("[INFO] VR content → ensuring fisheye 180° SBS metadata: " +
                       videoStr);
                if (sm::injectVRSpatialMetadata(videoStr, logCb))
                    vrCount_.fetch_add(1);
            }

            // ── STEP 6: Write THUMBNAILED metadata tag ──────────────────
            if (!shouldCancel() && !anyError)
            {
                setProgress(videoStr, "Tagging", "Writing metadata tag...");
                sm::writeProcessedTag(videoStr, logCb);
            }

            // Mark processed
            setProgress(videoStr, "Done", "");
            {
                std::lock_guard lock(videosMutex_);
                videos_[idx].processed = true;
                videos_[idx].hasTag = true;
                videos_[idx].regenQueued = false;
            }

            processedCount_.fetch_add(1);
            bytesProcessed_.fetch_add(fsize);
        }

        // Mark this thread as inactive
        threadProgress_[threadIdx].active.store(false);
        {
            std::lock_guard lock(threadProgress_[threadIdx].mtx);
            threadProgress_[threadIdx].filePath.clear();
            threadProgress_[threadIdx].action.clear();
            threadProgress_[threadIdx].subAction.clear();
            threadProgress_[threadIdx].fileSize = 0;
        }

        // Last worker to exit handles completion/cleanup
        int rem = activeWorkers_.fetch_sub(1) - 1;
        if (rem == 0)
        {
            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_.clear();
            }

            if (cancelWork_.load())
            {
                // Cancel cleanup: delete orphaned .jpg and ~remuxed.mkv
                int cleaned = 0;
                {
                    std::lock_guard lock(videosMutex_);
                    for (auto &v : videos_)
                    {
                        if (v.processed && !v.failed)
                            continue; // successfully completed

                        // Delete orphaned .jpg
                        if (fs::exists(v.thumbPath))
                        {
                            std::error_code ec;
                            fs::remove(v.thumbPath, ec);
                            if (!ec)
                                cleaned++;
                        }

                        // Delete orphaned ~remuxed.mkv temp files
                        auto stem = v.videoPath.stem().string();
                        auto dir = v.videoPath.parent_path();
                        fs::path tmpPath = dir / (stem + "~remuxed.mkv");
                        if (fs::exists(tmpPath))
                        {
                            std::error_code ec;
                            fs::remove(tmpPath, ec);
                            if (!ec)
                                cleaned++;
                        }
                    }
                }
                addLog("[WARNING] Cancelled — cleaned up " +
                       std::to_string(cleaned) + " orphaned files");
            }
            else
            {
                addLog("[OK] Complete — " + std::to_string(generated_.load()) +
                       " thumbs, " + std::to_string(remuxedCount_.load()) +
                       " remuxed, " + std::to_string(embeddedCount_.load()) +
                       " embedded, " + std::to_string(vrCount_.load()) +
                       " VR, " + std::to_string(errors_.load()) + " errors");
            }

            working_.store(false);
        }
    }

    // ── Render (SM-style fullscreen layout) ────────────────────────

    void App::render()
    {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##ThumbnailTool", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();

        float w = ImGui::GetContentRegionAvail().x;
        float h = ImGui::GetContentRegionAvail().y;
        float statusH = ImGui::GetFrameHeight() + 4;

        // ── Header bar ──────────────────────────────────────────────
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.05f, 0.05f, 0.07f, 1.0f));
            ImGui::BeginChild("##Header", {w, ImGui::GetFrameHeight() + 16}, false,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SetCursorPos({12, 8});
            ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
            ImGui::Text("ThumbnailTool");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, " — Batch Video Thumbnail Generator");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Toolbar ─────────────────────────────────────────────────
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
            ImGui::BeginChild("##Toolbar", {w, ImGui::GetFrameHeight() + 14}, false,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SetCursorPos({12, 6});

            ImGui::SetNextItemWidth(w * 0.45f);
            bool isScanning = scanning_.load();
            bool isWorking = working_.load();

            if (isScanning || isWorking)
                ImGui::BeginDisabled();
            ImGui::InputTextWithHint("##RootDir", "Root directory (e.g. F:\\StripChat)",
                                     rootDir_, sizeof(rootDir_));
            if (isScanning || isWorking)
                ImGui::EndDisabled();

            ImGui::SameLine();

            if (isScanning)
                ImGui::BeginDisabled();
            if (ImGui::Button("Scan", {80, 0}))
                startScan();
            if (isScanning)
                ImGui::EndDisabled();

            ImGui::SameLine();

            if (isWorking || isScanning)
                ImGui::BeginDisabled();
            if (ImGui::Button("Generate", {90, 0}))
                startGeneration();
            if (isWorking || isScanning)
                ImGui::EndDisabled();

            ImGui::SameLine();

            // Regen All — forces regeneration of ALL thumbnails (ignores existing)
            if (isWorking || isScanning)
                ImGui::BeginDisabled();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.3f, 0.1f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.7f, 0.4f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.9f, 0.5f, 0.2f, 1.0f));
            if (ImGui::Button("Regen All", {90, 0}))
            {
                // Mark ALL files as needing regeneration
                {
                    std::lock_guard lock(videosMutex_);
                    for (auto &v : videos_)
                        v.regenQueued = true;
                }
                addLog("[INFO] Queued ALL files for thumbnail regeneration");
                startGeneration();
            }
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Regenerate thumbnails for ALL files (ignores existing)");
            if (isWorking || isScanning)
                ImGui::EndDisabled();

            // Show live scan progress
            if (isScanning)
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "Scanning... %d files found", scanProgress_.load());
            }

            // Right-aligned controls: STOP (when working), Hide Done, Settings
            // Calculate right side width dynamically
            float rightControlsWidth = 0;
            float stopBtnW = 70;
            float hideDoneW = ImGui::CalcTextSize("Hide Done").x + 30; // checkbox + padding
            float settingsBtnW = 90;
            float spacing = 12;

            if (isWorking)
                rightControlsWidth = stopBtnW + spacing + hideDoneW + spacing + settingsBtnW + 24;
            else
                rightControlsWidth = hideDoneW + spacing + settingsBtnW + 24;

            // Progress info (left side, after scan status)
            if (isWorking)
            {
                ImGui::SameLine();
                int done = processedCount_.load();
                int total = totalToProcess_.load();
                float frac = total > 0 ? (float)done / total : 0.0f;

                // Compact progress display
                float availForProgress = w - ImGui::GetCursorPosX() - rightControlsWidth - 20;
                if (availForProgress > 300)
                {
                    ImGui::SetNextItemWidth(100);
                    ImGui::ProgressBar(frac, {100, 0}, "");
                    ImGui::SameLine();
                }
                ImGui::TextColored(COL_YELLOW, "%d/%d", done, total);

                // ETA calculation (rolling window for stable estimates)
                ImGui::SameLine();
                {
                    int64_t doneBytes = bytesProcessed_.load();
                    int64_t totalB = totalBytes_.load();
                    if (doneBytes > 0 && totalB > 0)
                    {
                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - startTime_).count();

                        // Rolling window: update speed every 5 seconds
                        double sinceSnap = std::chrono::duration<double>(now - etaLastTime_).count();
                        if (sinceSnap >= 5.0 || etaRollingBps_ <= 0)
                        {
                            int64_t delta = doneBytes - etaLastBytes_;
                            double newBps = delta / std::max(sinceSnap, 0.001);
                            // Smooth: 70% new + 30% old to prevent jumps
                            etaRollingBps_ = (etaRollingBps_ > 0)
                                                 ? etaRollingBps_ * 0.3 + newBps * 0.7
                                                 : newBps;
                            etaLastBytes_ = doneBytes;
                            etaLastTime_ = now;
                        }

                        double bps = etaRollingBps_ > 0 ? etaRollingBps_ : (doneBytes / std::max(elapsed, 0.001));
                        int64_t remainingBytes = totalB - doneBytes;
                        int etaSec = (int)(remainingBytes / std::max(bps, 1.0));
                        int etaH = etaSec / 3600;
                        int etaM = (etaSec % 3600) / 60;
                        int etaS = etaSec % 60;
                        if (etaH > 0)
                            ImGui::TextColored(COL_DIM, "ETA %dh%02dm%02ds", etaH, etaM, etaS);
                        else if (etaM > 0)
                            ImGui::TextColored(COL_DIM, "ETA %dm%02ds", etaM, etaS);
                        else
                            ImGui::TextColored(COL_DIM, "ETA %ds", etaS);

                        // Only show size info if there's enough space
                        if (availForProgress > 500)
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(COL_DIM, "(%s/%s)",
                                               formatSize(doneBytes).c_str(),
                                               formatSize(totalB).c_str());
                        }
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "ETA ...");
                    }
                }
            }

            // Right-aligned section
            ImGui::SameLine(w - rightControlsWidth);

            if (isWorking)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.2f, 0.2f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1.0f, 0.3f, 0.3f, 1.0f));
                if (ImGui::Button("STOP", {stopBtnW, 0}))
                    cancelWork_.store(true);
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, spacing);
            }

            ImGui::Checkbox("Hide Done", &hideFinished_);
            ImGui::SameLine(0, spacing);

            if (ImGui::Button("Settings", {settingsBtnW, 0}))
                showSettings_ = !showSettings_;

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Processing stats (visible during generation) ────────────
        if (working_.load())
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.08f, 0.12f, 1.0f));
            float statsH = ImGui::GetFrameHeight() * 2 + 14;
            ImGui::BeginChild("##ProcessingStats", {w, statsH}, false,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImGui::SetCursorPos({12, 4});

            // Load atomic values for display
            int dispCols = thumbnailColumns.load();
            int dispRows = thumbnailRows.load();
            int dispWidth = thumbnailWidth.load();
            bool dispEmbed = embedInVideo.load();

            // Row 1: Processing info + current file
            ImGui::TextColored(COL_ACCENT, "PROCESSING");
            ImGui::SameLine(0, 12);
            ImGui::TextColored(COL_DIM, "|");
            ImGui::SameLine(0, 12);
            ImGui::TextColored(COL_TEXT, "%d threads", activeWorkers_.load());
            ImGui::SameLine(0, 12);
            ImGui::TextColored(COL_DIM, "|");
            ImGui::SameLine(0, 12);
            ImGui::TextColored(COL_TEXT, "%dx%d @ %dpx", dispCols, dispRows, dispWidth);
            ImGui::SameLine(0, 12);
            ImGui::TextColored(COL_DIM, "|");
            ImGui::SameLine(0, 12);
            ImGui::TextColored(dispEmbed ? COL_GREEN : COL_DIM, "Embed: %s", dispEmbed ? "ON" : "OFF");

            // Current file
            {
                std::lock_guard lock(currentFileMutex_);
                if (!currentFile_.empty())
                {
                    ImGui::SameLine(0, 24);
                    ImGui::TextColored(COL_YELLOW, ">>>");
                    ImGui::SameLine();
                    ImGui::TextUnformatted(currentFile_.c_str());
                }
            }

            // Row 2: Live stat counters
            ImGui::SetCursorPos({12, ImGui::GetFrameHeight() + 8});
            int rmx = remuxedCount_.load();
            int gen = generated_.load();
            int emb = embeddedCount_.load();
            int vr = vrCount_.load();
            int err = errors_.load();
            int done = processedCount_.load();
            int total = totalToProcess_.load();

            ImGui::TextColored(COL_TEXT, "Progress: %d/%d", done, total);
            ImGui::SameLine(0, 20);

            if (rmx > 0)
            {
                ImGui::TextColored(COL_GREEN, "Remuxed: %d", rmx);
                ImGui::SameLine(0, 16);
            }
            if (gen > 0)
            {
                ImGui::TextColored(COL_GREEN, "Thumbs: %d", gen);
                ImGui::SameLine(0, 16);
            }
            if (emb > 0)
            {
                ImGui::TextColored(COL_GREEN, "Embedded: %d", emb);
                ImGui::SameLine(0, 16);
            }
            if (vr > 0)
            {
                ImGui::TextColored(COL_ACCENT, "VR: %d", vr);
                ImGui::SameLine(0, 16);
            }
            if (err > 0)
            {
                ImGui::TextColored(COL_RED, "Errors: %d", err);
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Content area (table + log with splitter) ────────────────
        float contentH = h - ImGui::GetCursorPosY() - statusH;
        if (contentH < 100)
            contentH = 100;

        ImGui::BeginChild("##Content", {w, contentH}, false,
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            float tableH = showLog_ ? contentH * splitRatio_ : contentH;
            float logH = showLog_ ? contentH - tableH - 4 : 0;

            // Table panel
            ImGui::BeginChild("##TablePanel", {w, tableH}, false,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            renderTable();
            ImGui::EndChild();

            // Splitter + log panel
            if (showLog_)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.15f, 0.18f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.25f, 0.40f, 0.80f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, {0.30f, 0.50f, 1.00f, 1.0f});
                ImGui::Button("##Splitter", {w, 4});
                if (ImGui::IsItemActive())
                {
                    float delta = ImGui::GetIO().MouseDelta.y;
                    float newRatio = splitRatio_ + delta / contentH;
                    splitRatio_ = std::clamp(newRatio, 0.2f, 0.9f);
                }
                ImGui::PopStyleColor(3);

                ImGui::BeginChild("##LogPanel", {w, logH}, false,
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                renderLogPanel();
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();

        // ── Status bar ──────────────────────────────────────────────
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                pos, {pos.x + w, pos.y + statusH}, COL_STATUS_BG);

            ImGui::SetCursorScreenPos({pos.x + 12, pos.y + 2});
            if (scanning_.load())
            {
                ImGui::TextColored(COL_YELLOW, "Scanning... %d files found", scanProgress_.load());
            }
            else if (scanned_)
            {
                int64_t totalSz = 0;
                {
                    std::lock_guard lock(videosMutex_);
                    for (auto &v : videos_)
                        totalSz += v.fileSize;
                }
                ImGui::TextColored(COL_DIM, "Videos: %d (%s)", totalVideos_, formatSize(totalSz).c_str());
                ImGui::SameLine(0, 16);
                ImGui::TextColored(COL_GREEN, "Thumbs: %d", withThumb_ + generated_.load());
                ImGui::SameLine(0, 16);
                ImGui::TextColored(COL_YELLOW, "Missing: %d",
                                   std::max(0, withoutThumb_ - generated_.load()));
                if (errors_.load() > 0)
                {
                    ImGui::SameLine(0, 16);
                    ImGui::TextColored(COL_RED, "Errors: %d", errors_.load());
                }
            }
            else
            {
                ImGui::TextColored(COL_DIM, "Ready — scan a directory to begin");
            }

            ImGui::SameLine(w - 80);
            if (ImGui::SmallButton(showLog_ ? "Hide Log" : "Show Log"))
                showLog_ = !showLog_;
        }

        ImGui::End();

        // Settings popup (separate window)
        if (showSettings_)
            renderSettingsPopup();
    }

    // ── Table ───────────────────────────────────────────────────────

    void App::renderTable()
    {
        // Show table as soon as we have data — even while scanning
        bool hasData = false;
        {
            std::lock_guard lock(videosMutex_);
            hasData = !videos_.empty();
        }

        if (!hasData)
        {
            ImGui::SetCursorPos({20, 40});
            if (scanning_.load())
                ImGui::TextColored(COL_YELLOW, "Scanning for videos...");
            else if (scanned_)
                ImGui::TextColored(COL_DIM, "No videos found in the scanned directory.");
            else
                ImGui::TextColored(COL_DIM, "Enter a root directory and click Scan to discover videos.");
            return;
        }

        // ── Status tabs ─────────────────────────────────────────────
        if (ImGui::BeginTabBar("##StatusTabs", ImGuiTabBarFlags_None))
        {
            // Count items per category for badge display
            int countAll = 0, countPending = 0, countDone = 0, countFailed = 0;
            int countInProgress = activeWorkers_.load();
            {
                std::lock_guard lock(videosMutex_);
                for (auto &v : videos_)
                {
                    countAll++;
                    if (v.failed)
                        countFailed++;
                    else if (v.processed || v.hasTag || v.hasThumb)
                        countDone++;
                    else
                        countPending++;
                }
            }

            char tabLabel[64];

            snprintf(tabLabel, sizeof(tabLabel), " All (%d) ###TabAll", countAll);
            if (ImGui::BeginTabItem(tabLabel))
            {
                currentTab_ = StatusTab::All;
                ImGui::EndTabItem();
            }

            // In Progress tab - show what workers are doing
            snprintf(tabLabel, sizeof(tabLabel), " In Progress (%d) ###TabInProgress", countInProgress);
            if (ImGui::BeginTabItem(tabLabel))
            {
                currentTab_ = StatusTab::InProgress;
                ImGui::EndTabItem();
            }

            snprintf(tabLabel, sizeof(tabLabel), " Pending (%d) ###TabPending", countPending);
            if (ImGui::BeginTabItem(tabLabel))
            {
                currentTab_ = StatusTab::Pending;
                ImGui::EndTabItem();
            }

            snprintf(tabLabel, sizeof(tabLabel), " Done (%d) ###TabDone", countDone);
            if (ImGui::BeginTabItem(tabLabel))
            {
                currentTab_ = StatusTab::Done;
                ImGui::EndTabItem();
            }

            snprintf(tabLabel, sizeof(tabLabel), " Failed (%d) ###TabFailed", countFailed);
            if (ImGui::BeginTabItem(tabLabel))
            {
                currentTab_ = StatusTab::Failed;
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        // ── "In Progress" tab shows thread activity ─────────────────
        if (currentTab_ == StatusTab::InProgress)
        {
            int numThreads = threadCount.load();
            auto now = std::chrono::steady_clock::now();

            ImGuiTableFlags tFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX;

            if (ImGui::BeginTable("##InProgressTable", 9, tFlags, ImGui::GetContentRegionAvail()))
            {
                // Dynamic layout: fixed columns for compact data, stretch columns for text
                ImGui::TableSetupColumn("##", ImGuiTableColumnFlags_WidthFixed, 30.0f);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch, 2.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Written", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Speed", ImGuiTableColumnFlags_WidthFixed, 110.0f);
                ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("Elapsed", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                for (int t = 0; t < numThreads && t < kMaxThreads; t++)
                {
                    bool isActive = threadProgress_[t].active.load();
                    std::string filePath, action, subAction;
                    int64_t fsize = 0;
                    int64_t bytesProcessed = 0;
                    double elapsed = 0;

                    {
                        std::lock_guard lock(threadProgress_[t].mtx);
                        filePath = threadProgress_[t].filePath;
                        action = threadProgress_[t].action;
                        subAction = threadProgress_[t].subAction;
                        fsize = threadProgress_[t].fileSize;
                        bytesProcessed = threadProgress_[t].bytesProcessed.load();
                        if (isActive && fsize > 0)
                            elapsed = std::chrono::duration<double>(now - threadProgress_[t].startTime).count();
                    }

                    // Calculate I/O speed and ETA
                    double speedMBps = 0.0;
                    double etaSec = 0.0;
                    if (elapsed > 0.5 && bytesProcessed > 0)
                    {
                        speedMBps = (double)bytesProcessed / (1024.0 * 1024.0) / elapsed;
                        if (fsize > bytesProcessed && speedMBps > 0.01)
                        {
                            double remaining = (double)(fsize - bytesProcessed) / (1024.0 * 1024.0);
                            etaSec = remaining / speedMBps;
                        }
                    }

                    ImGui::TableNextRow();

                    // Thread # column — compact colored indicator
                    ImGui::TableNextColumn();
                    if (isActive)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_GREEN);
                        ImGui::Text(" %d", t + 1);
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, " %d", t + 1);
                    }

                    // Action column — color-coded
                    ImGui::TableNextColumn();
                    if (isActive && !action.empty())
                    {
                        ImVec4 actionColor = COL_TEXT;
                        if (action == "Remuxing" || action == "Fixing TS")
                            actionColor = COL_YELLOW;
                        else if (action == "Generating")
                            actionColor = COL_ACCENT;
                        else if (action == "Embedding" || action == "Fix Meta")
                            actionColor = COL_GREEN;
                        else if (action == "VR Meta")
                            actionColor = ImVec4(0.7f, 0.5f, 1.0f, 1.0f);
                        else if (action == "Probing" || action == "Checking" || action == "Starting")
                            actionColor = COL_DIM;
                        else if (action == "Done")
                            actionColor = COL_GREEN;
                        ImGui::TextColored(actionColor, "%s", action.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "Idle");
                    }

                    // Detail column
                    ImGui::TableNextColumn();
                    if (isActive && !subAction.empty())
                        ImGui::TextUnformatted(subAction.c_str());
                    else
                        ImGui::TextColored(COL_DIM, "-");

                    // Size column
                    ImGui::TableNextColumn();
                    if (isActive && fsize > 0)
                        ImGui::TextUnformatted(formatSize(fsize).c_str());
                    else
                        ImGui::TextColored(COL_DIM, "-");

                    // Written column (live output size during remux)
                    ImGui::TableNextColumn();
                    if (isActive && bytesProcessed > 0)
                    {
                        // Show percentage of original when both are known
                        if (fsize > 0)
                        {
                            int pct = (int)((double)bytesProcessed * 100.0 / (double)fsize);
                            ImGui::Text("%s (%d%%)", formatSize(bytesProcessed).c_str(), pct);
                        }
                        else
                        {
                            ImGui::TextUnformatted(formatSize(bytesProcessed).c_str());
                        }
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "-");
                    }

                    // Speed column (MB/s)
                    ImGui::TableNextColumn();
                    if (isActive && speedMBps > 0.01)
                    {
                        if (speedMBps >= 100)
                            ImGui::TextColored(COL_GREEN, "%.0f MB/s", speedMBps);
                        else if (speedMBps >= 10)
                            ImGui::TextColored(COL_TEXT, "%.1f MB/s", speedMBps);
                        else
                            ImGui::TextColored(COL_YELLOW, "%.2f MB/s", speedMBps);
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "-");
                    }

                    // ETA column
                    ImGui::TableNextColumn();
                    if (isActive && etaSec > 0.5)
                    {
                        int es = (int)etaSec;
                        if (es >= 60)
                            ImGui::TextColored(COL_YELLOW, "%dm%02ds", es / 60, es % 60);
                        else
                            ImGui::Text("%ds", es);
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "-");
                    }

                    // Elapsed column
                    ImGui::TableNextColumn();
                    if (isActive && elapsed > 0)
                    {
                        int es = (int)elapsed;
                        if (es >= 60)
                            ImGui::TextColored(COL_YELLOW, "%dm%02ds", es / 60, es % 60);
                        else
                            ImGui::Text("%ds", es);
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "-");
                    }

                    // File column — relative path for meaningful context
                    ImGui::TableNextColumn();
                    if (isActive && !filePath.empty())
                    {
                        // Show relative path from root dir (same as Done/All tabs)
                        std::string display;
                        try
                        {
                            display = fs::relative(fs::path(filePath), fs::path(rootDir_)).string();
                        }
                        catch (...)
                        {
                            display = fs::path(filePath).filename().string();
                        }
                        ImGui::TextUnformatted(display.c_str());
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("%s", filePath.c_str());
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "Waiting for work...");
                    }
                }

                ImGui::EndTable();
            }
            return; // Don't render main file table for In Progress tab
        }

        // Snapshot under lock — COPY strings to avoid dangling pointers
        // (scan thread may reallocate videos_ vector while we render)
        struct RowSnap
        {
            std::string rel;
            ContainerType container;
            int64_t fileSize;
            bool hasThumb;
            bool hasCoverEmbed;
            bool hasTag;
            bool processed;
            bool failed;
            bool tsFixed;
            bool remuxed;
            size_t videoIdx; // index into videos_ for context menu actions

            // Sort key helpers
            int containerKey() const { return (int)container; }
            int thumbKey() const { return hasCoverEmbed ? 2 : (hasThumb ? 1 : 0); }
            int statusKey() const
            {
                if (failed)
                    return 0;
                if (processed || hasTag)
                    return 1;
                // No tag = pending, regardless of existing cover/thumb
                return 2;
            }
        };
        static thread_local std::vector<RowSnap> rows;
        rows.clear();
        {
            std::lock_guard lock(videosMutex_);
            rows.reserve(videos_.size());
            for (size_t vi = 0; vi < videos_.size(); vi++)
            {
                auto &v = videos_[vi];
                // Tab-based filtering
                bool isFailed = v.failed;
                // Done = processed this session OR has THUMBNAILED metadata tag
                // Files with existing cover but NO tag are Pending (need full reprocessing)
                bool isDone = !v.failed && (v.processed || v.hasTag);
                bool isPending = !isFailed && !isDone;

                switch (currentTab_)
                {
                case StatusTab::Pending:
                    if (!isPending)
                        continue;
                    break;
                case StatusTab::Done:
                    if (!isDone)
                        continue;
                    break;
                case StatusTab::Failed:
                    if (!isFailed)
                        continue;
                    break;
                default: // All
                    // Also apply "Hide Done" filter when on All tab
                    if (hideFinished_ && isDone)
                        continue;
                    break;
                }

                rows.push_back({v.relDisplay, v.container, v.fileSize, v.hasThumb,
                                v.hasCoverEmbed, v.hasTag, v.processed, v.failed, v.tsFixed, v.remuxed, vi});
            }
        }

        // Show filtered count when hiding finished on All tab
        if (hideFinished_ && currentTab_ == StatusTab::All)
        {
            int totalVids = 0;
            {
                std::lock_guard lock(videosMutex_);
                totalVids = (int)videos_.size();
            }
            int hidden = totalVids - (int)rows.size();
            if (hidden > 0)
            {
                ImGui::SetCursorPos({ImGui::GetCursorPosX() + 12, ImGui::GetCursorPosY() + 4});
                ImGui::TextColored(COL_DIM, "(%d finished files hidden)", hidden);
            }
        }

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SortTristate;

        if (ImGui::BeginTable("##Videos", 5, flags, ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_DefaultSort, 5.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.6f);
            ImGui::TableSetupColumn("Container", ImGuiTableColumnFlags_PreferSortDescending, 0.6f);
            ImGui::TableSetupColumn("Thumbnail", ImGuiTableColumnFlags_PreferSortDescending, 0.7f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_PreferSortDescending, 1.8f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            // Apply sort if user clicked a column header
            if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs())
            {
                if (sortSpecs->SpecsCount > 0)
                {
                    auto &spec = sortSpecs->Specs[0];
                    bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                    int col = spec.ColumnIndex;

                    std::sort(rows.begin(), rows.end(),
                              [col, asc](const RowSnap &a, const RowSnap &b)
                              {
                                  int cmp = 0;
                                  switch (col)
                                  {
                                  case 0: // File — lexicographic
                                      cmp = a.rel.compare(b.rel);
                                      break;
                                  case 1: // Size
                                      cmp = (a.fileSize < b.fileSize) ? -1 : (a.fileSize > b.fileSize ? 1 : 0);
                                      break;
                                  case 2: // Container
                                      cmp = a.containerKey() - b.containerKey();
                                      break;
                                  case 3: // Thumbnail
                                      cmp = a.thumbKey() - b.thumbKey();
                                      break;
                                  case 4: // Status
                                      cmp = a.statusKey() - b.statusKey();
                                      break;
                                  }
                                  return asc ? (cmp < 0) : (cmp > 0);
                              });
                    sortSpecs->SpecsDirty = false;
                }
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)rows.size());
            while (clipper.Step())
            {
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                {
                    auto &r = rows[i];
                    ImGui::TableNextRow();
                    ImGui::PushID(i);

                    // File column — selectable for context menu
                    ImGui::TableNextColumn();
                    ImGui::Selectable(r.rel.c_str(), false,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                          ImGuiSelectableFlags_AllowOverlap);

                    // Right-click context menu on the row
                    if (ImGui::BeginPopupContextItem("##RowCtx"))
                    {
                        ImGui::TextColored(COL_DIM, "%s", r.rel.c_str());
                        ImGui::Separator();

                        if (ImGui::MenuItem("Regenerate Thumbnail"))
                        {
                            std::lock_guard lock(videosMutex_);
                            if (r.videoIdx < videos_.size())
                            {
                                videos_[r.videoIdx].regenQueued = true;
                                videos_[r.videoIdx].processed = false;
                                videos_[r.videoIdx].failed = false;
                                videos_[r.videoIdx].errorMsg.clear();
                            }
                            addLog("[INFO] Queued for regeneration: " + r.rel);
                        }

                        if (ImGui::MenuItem("Copy Path"))
                        {
                            std::string fullPath;
                            {
                                std::lock_guard lock(videosMutex_);
                                if (r.videoIdx < videos_.size())
                                    fullPath = videos_[r.videoIdx].videoPath.string();
                            }
                            if (!fullPath.empty())
                                ImGui::SetClipboardText(fullPath.c_str());
                        }

                        if (ImGui::MenuItem("Open Folder"))
                        {
                            std::string folder;
                            {
                                std::lock_guard lock(videosMutex_);
                                if (r.videoIdx < videos_.size())
                                    folder = videos_[r.videoIdx].videoPath.parent_path().string();
                            }
#ifdef _WIN32
                            if (!folder.empty())
                                ShellExecuteA(nullptr, "open", folder.c_str(), nullptr, nullptr, SW_SHOW);
#endif
                        }

                        ImGui::EndPopup();
                    }

                    // Size column
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(formatSize(r.fileSize).c_str());

                    // Container column
                    ImGui::TableNextColumn();
                    switch (r.container)
                    {
                    case ContainerType::RealMKV:
                        DrawBadge("MKV", COL_BADGE_OK);
                        break;
                    case ContainerType::FakeMKV:
                        DrawBadge("Fake MKV", COL_BADGE_PEND);
                        break;
                    default:
                        DrawBadge("Other", COL_BADGE_SKIP);
                        break;
                    }

                    // Thumbnail column
                    ImGui::TableNextColumn();
                    if (r.hasCoverEmbed)
                        DrawBadge("Embedded", COL_BADGE_OK);
                    else if (r.hasThumb)
                        DrawBadge("Yes", COL_BADGE_OK);
                    else
                        DrawBadge("No", COL_BADGE_SKIP);

                    // Status column
                    ImGui::TableNextColumn();
                    if (r.failed)
                    {
                        DrawBadge("Failed", COL_BADGE_ERR);
                    }
                    else if (r.processed || r.hasTag)
                    {
                        DrawBadge("Done", COL_BADGE_OK);
                        if (r.hasTag)
                            DrawBadge("Tagged", IM_COL32(60, 160, 160, 255));
                        if (r.remuxed)
                            DrawBadge("Remuxed", IM_COL32(100, 60, 200, 255));
                        if (r.tsFixed)
                            DrawBadge("TS Fixed", IM_COL32(60, 130, 200, 255));
                    }
                    else
                    {
                        DrawBadge("Pending", COL_BADGE_PEND);
                    }

                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
    }

    // ── Log panel ───────────────────────────────────────────────────

    void App::renderLogPanel()
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.04f, 0.04f, 0.06f, 1.0f));
        ImGui::BeginChild("##Log", ImGui::GetContentRegionAvail(), false);

        // ── Log toolbar (filter + level buttons + copy) ─────────────
        {
            float w = ImGui::GetContentRegionAvail().x;
            ImGui::SetCursorPos({6, 4});

            // Level filter buttons
            auto levelBtn = [this](const char *label, int level, ImVec4 col)
            {
                bool active = (logLevelFilter_ == level);
                if (active)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(col.x * 0.5f, col.y * 0.5f, col.z * 0.5f, 0.8f));
                else
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.12f, 0.15f, 1.0f));
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {6, 2});
                if (ImGui::SmallButton(label))
                    logLevelFilter_ = level;
                ImGui::PopStyleVar();
                ImGui::PopStyleColor();
                ImGui::SameLine(0, 4);
            };

            levelBtn("All", 0, COL_TEXT);
            levelBtn("Info", 1, COL_DIM);
            levelBtn("Warn", 2, COL_YELLOW);
            levelBtn("Err", 3, COL_RED);

            ImGui::SameLine(0, 12);
            ImGui::SetNextItemWidth(std::min(w * 0.4f, 300.0f));
            ImGui::InputTextWithHint("##LogFilter", "Filter logs...", logFilter_, sizeof(logFilter_));

            ImGui::SameLine(0, 8);

            // Copy visible logs button
            if (ImGui::SmallButton("Copy"))
            {
                std::string allText;
                std::lock_guard lock(logMtx_);
                std::string filterStr(logFilter_);
                for (auto &c : filterStr)
                    c = (char)std::tolower((unsigned char)c);

                for (auto &line : log_)
                {
                    // Level filter
                    if (logLevelFilter_ == 1 && line.find("[INFO]") == std::string::npos &&
                        line.find("[OK]") == std::string::npos &&
                        line.find("[WARNING]") == std::string::npos &&
                        line.find("[ERROR]") == std::string::npos)
                        continue;
                    if (logLevelFilter_ == 2 && line.find("[WARNING]") == std::string::npos &&
                        line.find("[ERROR]") == std::string::npos)
                        continue;
                    if (logLevelFilter_ == 3 && line.find("[ERROR]") == std::string::npos)
                        continue;

                    // Text filter
                    if (!filterStr.empty())
                    {
                        std::string lower = line;
                        for (auto &c : lower)
                            c = (char)std::tolower((unsigned char)c);
                        if (lower.find(filterStr) == std::string::npos)
                            continue;
                    }
                    allText += line;
                    allText += '\n';
                }
                if (!allText.empty())
                    ImGui::SetClipboardText(allText.c_str());
            }

            ImGui::SameLine(0, 8);
            if (ImGui::SmallButton("Clear"))
            {
                std::lock_guard lock(logMtx_);
                log_.clear();
            }

            ImGui::SameLine(0, 8);
            ImGui::Checkbox("Auto-scroll", &autoScroll_);
        }

        ImGui::Spacing();

        // ── Log content ─────────────────────────────────────────────
        ImGui::BeginChild("##LogContent", ImGui::GetContentRegionAvail(), false);
        {
            std::lock_guard lock(logMtx_);

            // Build filtered view
            std::string filterStr(logFilter_);
            for (auto &c : filterStr)
                c = (char)std::tolower((unsigned char)c);

            // We need to collect filtered indices for the clipper
            static thread_local std::vector<int> filteredIdx;
            filteredIdx.clear();
            filteredIdx.reserve(log_.size());

            for (int i = 0; i < (int)log_.size(); i++)
            {
                auto &line = log_[i];

                // Level filter
                if (logLevelFilter_ == 1 && line.find("[INFO]") == std::string::npos &&
                    line.find("[OK]") == std::string::npos &&
                    line.find("[WARNING]") == std::string::npos &&
                    line.find("[ERROR]") == std::string::npos)
                    continue;
                if (logLevelFilter_ == 2 && line.find("[WARNING]") == std::string::npos &&
                    line.find("[ERROR]") == std::string::npos)
                    continue;
                if (logLevelFilter_ == 3 && line.find("[ERROR]") == std::string::npos)
                    continue;

                // Text filter
                if (!filterStr.empty())
                {
                    std::string lower = line;
                    for (auto &c : lower)
                        c = (char)std::tolower((unsigned char)c);
                    if (lower.find(filterStr) == std::string::npos)
                        continue;
                }

                filteredIdx.push_back(i);
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)filteredIdx.size());
            while (clipper.Step())
            {
                for (int fi = clipper.DisplayStart; fi < clipper.DisplayEnd; fi++)
                {
                    auto &line = log_[filteredIdx[fi]];
                    ImVec4 col = COL_TEXT;
                    if (line.find("[ERROR]") != std::string::npos)
                        col = COL_RED;
                    else if (line.find("[WARNING]") != std::string::npos)
                        col = COL_YELLOW;
                    else if (line.find("[OK]") != std::string::npos)
                        col = COL_GREEN;
                    else if (line.find("[INFO]") != std::string::npos)
                        col = COL_DIM;

                    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                    ImGui::TextColored(col, "%s", line.c_str());
                    ImGui::PopTextWrapPos();

                    // Right-click to copy individual line
                    if (ImGui::IsItemHovered() && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
                        ImGui::SetClipboardText(line.c_str());
                }
            }

            if (autoScroll_)
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ── Settings popup ──────────────────────────────────────────────

    void App::renderSettingsPopup()
    {
        ImGui::SetNextWindowSize({460, 560}, ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                {0.5f, 0.5f});

        if (ImGui::Begin("Thumbnail Settings", &showSettings_, ImGuiWindowFlags_NoCollapse))
        {
            // Load current values from atomics
            int width = thumbnailWidth.load();
            int cols = thumbnailColumns.load();
            int rows = thumbnailRows.load();
            int threads = threadCount.load();
            bool embed = embedInVideo.load();

            // Section: Thumbnail Grid
            ImGui::SeparatorText("Thumbnail Grid");

            ImGui::Text("Image Width (px)");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##Width", &width, 1280, 7680, "%d px"))
                thumbnailWidth.store(width);

            ImGui::Spacing();
            ImGui::Text("Grid Columns");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##Cols", &cols, 2, 8))
                thumbnailColumns.store(cols);

            ImGui::Spacing();
            ImGui::Text("Grid Rows");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##Rows", &rows, 2, 12))
                thumbnailRows.store(rows);

            // Section: Processing
            ImGui::Spacing();
            ImGui::SeparatorText("Processing");

            ImGui::Text("Worker Threads");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##Threads", &threads, 1, kMaxThreads))
            {
                int oldCount = threadCount.exchange(threads);

                // If increasing threads while working, spawn new workers immediately
                if (working_.load() && threads > oldCount)
                {
                    std::lock_guard lock(workerMutex_);
                    for (int t = oldCount; t < threads && t < kMaxThreads; t++)
                    {
                        // Only spawn if this slot isn't already active
                        if (!threadProgress_[t].active.load())
                        {
                            activeWorkers_.fetch_add(1);
                            workers_.emplace_back([this, t]()
                                                  { workerFunc(t); });
                            addLog("[INFO] Spawned new worker thread " + std::to_string(t + 1));
                        }
                    }
                }
            }

            // Show note that changes apply immediately
            if (working_.load())
            {
                ImGui::Spacing();
                ImGui::TextColored(COL_GREEN, "Thread changes apply IMMEDIATELY!");
                ImGui::TextColored(COL_DIM, "Increase: new workers spawn instantly");
                ImGui::TextColored(COL_DIM, "Decrease: workers finish current file then exit");
            }

            ImGui::Spacing();
            if (ImGui::Checkbox("Embed thumbnail in MKV (requires mkvpropedit)", &embed))
                embedInVideo.store(embed);

            // Section: Shell Integration
            ImGui::Spacing();
            ImGui::SeparatorText("Shell Integration");
#ifdef _WIN32
            {
                // Check if context menu is installed
                HKEY hKey = nullptr;
                bool installed = (RegOpenKeyExA(HKEY_CURRENT_USER,
                                                "Software\\Classes\\Directory\\shell\\ThumbnailTool",
                                                0, KEY_READ, &hKey) == ERROR_SUCCESS);
                if (hKey)
                    RegCloseKey(hKey);

                if (installed)
                {
                    ImGui::TextColored(COL_GREEN, "Context menu: Installed");
                    ImGui::SameLine();
                    if (ImGui::Button("Uninstall##Shell"))
                    {
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\shell\\ThumbnailTool\\command");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\Background\\shell\\ThumbnailTool\\command");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\shell\\ThumbnailTool");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\Background\\shell\\ThumbnailTool");
                        addLog("[INFO] Context menu uninstalled");
                    }
                }
                else
                {
                    ImGui::TextColored(COL_DIM, "Context menu: Not installed");
                    ImGui::SameLine();
                    if (ImGui::Button("Install##Shell"))
                    {
                        // Get exe path
                        char exeBuf[MAX_PATH] = {};
                        GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
                        std::string exePath(exeBuf);

                        // Register for folder right-click
                        const char *keyPaths[] = {
                            "Software\\Classes\\Directory\\shell\\ThumbnailTool",
                            "Software\\Classes\\Directory\\Background\\shell\\ThumbnailTool",
                        };
                        const char *args[] = {"%1", "%V"};

                        for (int i = 0; i < 2; i++)
                        {
                            HKEY hk = nullptr;
                            RegCreateKeyExA(HKEY_CURRENT_USER, keyPaths[i], 0, nullptr,
                                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr);
                            if (hk)
                            {
                                const char *text = "Generate Thumbnails";
                                RegSetValueExA(hk, nullptr, 0, REG_SZ,
                                               (const BYTE *)text, (DWORD)(strlen(text) + 1));
                                RegSetValueExA(hk, "Icon", 0, REG_SZ,
                                               (const BYTE *)exePath.c_str(), (DWORD)(exePath.size() + 1));
                                RegCloseKey(hk);
                            }

                            std::string cmdKey = std::string(keyPaths[i]) + "\\command";
                            RegCreateKeyExA(HKEY_CURRENT_USER, cmdKey.c_str(), 0, nullptr,
                                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr);
                            if (hk)
                            {
                                std::string cmd = "\"" + exePath + "\" \"" + args[i] + "\"";
                                RegSetValueExA(hk, nullptr, 0, REG_SZ,
                                               (const BYTE *)cmd.c_str(), (DWORD)(cmd.size() + 1));
                                RegCloseKey(hk);
                            }
                        }
                        addLog("[INFO] Context menu installed");
                    }
                }
            }
#else
            ImGui::TextColored(COL_DIM, "Shell integration is Windows-only");
#endif

            // Section: Config
            ImGui::Spacing();
            ImGui::SeparatorText("Configuration");

            if (ImGui::Button("Save Settings", {140, 0}))
                saveConfig();

            ImGui::SameLine();
            if (ImGui::Button("Reload Settings", {140, 0}))
                loadConfig();

            // Info
            ImGui::Spacing();
            ImGui::SeparatorText("Info");
            ImGui::TextColored(COL_DIM, "Output: %dx%d contact sheet per video",
                               cols, rows);
            ImGui::TextColored(COL_DIM, "Width: %d px (adaptive: scales up for high-res sources)",
                               width);

            ImGui::Spacing();
            if (ImGui::Button("Close", {120, 0}))
                showSettings_ = false;
        }
        ImGui::End();
    }

} // namespace tt
