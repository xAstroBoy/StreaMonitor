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

            // TWO-PHASE SCAN: Do NOT call hasCoverArt() here!
            // Phase 1 (scan) = fast filesystem walk only: check .jpg existence + EBML magic.
            // Phase 2 (worker) = hasCoverArt() is called lazily before thumbnail generation.
            // This keeps scan nearly instant even for 2000+ files.
            ve.hasCoverEmbed = false;
            ve.coverProbed = false;

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
            if (ve.hasThumb)
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

        // Finalize — set counters so render() picks them up
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
        int count = 0;
        int64_t totalB = 0;
        {
            std::lock_guard lock(videosMutex_);
            for (auto &v : videos_)
            {
                if (!v.processed)
                {
                    count++;
                    totalB += v.fileSize;
                }
            }
        }
        totalToProcess_.store(count);
        totalBytes_.store(totalB);

        if (count == 0)
        {
            addLog("[INFO] Nothing to process — all videos done");
            return;
        }

        int numThreads = std::clamp(threadCount.load(), 1, 64);
        addLog("[INFO] Processing " + std::to_string(count) +
               " videos with " + std::to_string(numThreads) + " threads...");

        working_.store(true);
        startTime_ = std::chrono::steady_clock::now();
        etaLastTime_ = startTime_;

        // Launch worker threads
        activeWorkers_.store(numThreads);
        workers_.clear();
        for (int t = 0; t < numThreads; t++)
            workers_.emplace_back([this]()
                                  { workerFunc(); });
    }

    void App::workerFunc()
    {
        // Build sorted work queue: ALL unprocessed, sorted by file size ascending (smallest first)
        std::vector<std::pair<int64_t, size_t>> sizeIdx;
        {
            std::lock_guard lock(videosMutex_);
            for (size_t i = 0; i < videos_.size(); i++)
            {
                if (!videos_[i].processed)
                    sizeIdx.push_back({videos_[i].fileSize, i});
            }
        }
        std::sort(sizeIdx.begin(), sizeIdx.end()); // ascending by file size

        std::vector<size_t> indices;
        indices.reserve(sizeIdx.size());
        for (auto &[sz, idx] : sizeIdx)
            indices.push_back(idx);

        // ═══════════════════════════════════════════════════════════════
        // PIPELINE PER VIDEO:
        //   1. Not MKV? → Remux to .mkv  (stream copy, fixes ALL playback)
        //      Fake MKV? → Remux in-place (stream copy, fixes ALL playback)
        //   2. Is MKV → Timestamps broken? → Remux in-place (fixes playback)
        //   3. Has thumbnail? → No → Generate contact sheet
        //   4. Embed cover art in MKV (if not already embedded)
        //   5. Is VR? → Has fisheye 180° SBS metadata? → No → Inject it!
        // ═══════════════════════════════════════════════════════════════
        while (!cancelWork_.load())
        {
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

            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_ = fs::path(videoStr).filename().string();
            }

            auto logCb = [this](const std::string &msg)
            { addLog("  " + msg); };

            addLog("[INFO] Processing: " + fs::path(videoStr).filename().string() +
                   " (" + formatSize(fsize) + ")");

            bool tsFixed = false;
            bool wasRemuxed = false;

            // ── STEP 1: Ensure real Matroska container ──────────────────
            // Not MKV? → Remux automatically to .mkv (stream copy, fixes ALL playback)
            // Fake MKV (MP4 inside)? → Remux in-place (stream copy, fixes ALL playback)
            // Real MKV? → Already good, continue to timestamp check
            if (origContainer != ContainerType::RealMKV)
            {
                addLog("[INFO] Remuxing to MKV: " + fs::path(videoStr).filename().string());
                std::string mkvPath = sm::ensureRealMKV(videoStr, logCb);
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
                    addLog("[ERROR] " + fs::path(videoStr).filename().string() +
                           " — remux failed, cleaned up temp files");
                    processedCount_.fetch_add(1);
                    bytesProcessed_.fetch_add(fsize);
                    continue;
                }
                wasRemuxed = true;
                tsFixed = true; // remux inherently fixes timestamps (DTS/PTS normalised)
                remuxedCount_.fetch_add(1);

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
            if (!wasRemuxed)
            {
                if (sm::hasTimestampIssues(videoStr, logCb))
                {
                    addLog("[INFO] Timestamps broken → remuxing to fix: " +
                           fs::path(videoStr).filename().string());
                    if (sm::fixTimestamps(videoStr, logCb))
                    {
                        tsFixed = true;
                        wasRemuxed = true;
                        remuxedCount_.fetch_add(1);
                    }
                    else
                    {
                        addLog("[WARNING] TS fix failed: " + fs::path(videoStr).filename().string());
                    }
                }
            }

            // ── STEP 3: Has thumbnail? → No → Generate it! ─────────────
            // Check embedded cover art first (deferred probe)
            if (!alreadyHasJpg)
            {
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
                               fs::path(videoStr).filename().string());
                    }
                }
            }

            // Generate contact sheet if no thumbnail exists
            bool genOk = alreadyHasJpg;
            if (!alreadyHasJpg)
            {
                addLog("[INFO] Generating thumbnail: " + fs::path(videoStr).filename().string());
                genOk = sm::generateContactSheet(videoStr, thumbStr, tc, logCb);
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
                    addLog("[ERROR] Thumbnail failed: " + fs::path(videoStr).filename().string());
                }
            }

            // ── STEP 4: Embed cover art + clean up external .jpg ────────
            if (genOk && doEmbed && !anyError)
            {
                bool wasEmbedded = sm::hasCoverArt(videoStr);
                if (!wasEmbedded && fs::exists(thumbStr))
                {
                    addLog("[INFO] Embedding cover art: " + fs::path(videoStr).filename().string());
                    if (sm::embedThumbnailInMKV(videoStr, thumbStr, logCb))
                    {
                        embeddedCount_.fetch_add(1);
                        wasEmbedded = true;
                    }
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
            if (sm::isVRFromPath(origVideoStr))
            {
                addLog("[INFO] VR content → ensuring fisheye 180° SBS metadata: " +
                       fs::path(videoStr).filename().string());
                if (sm::injectVRSpatialMetadata(videoStr, logCb))
                    vrCount_.fetch_add(1);
            }

            // Mark processed
            {
                std::lock_guard lock(videosMutex_);
                videos_[idx].processed = true;
            }

            processedCount_.fetch_add(1);
            bytesProcessed_.fetch_add(fsize);
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
            ImGui::BeginChild("##Header", {w, ImGui::GetFrameHeight() + 16}, false);
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
            ImGui::BeginChild("##Toolbar", {w, ImGui::GetFrameHeight() + 14}, false);
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

            // Show live scan progress
            if (isScanning)
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "Scanning... %d files found", scanProgress_.load());
            }

            if (isWorking)
            {
                ImGui::SameLine();
                int done = processedCount_.load();
                int total = totalToProcess_.load();
                float frac = total > 0 ? (float)done / total : 0.0f;
                ImGui::SetNextItemWidth(120);
                ImGui::ProgressBar(frac, {120, 0}, "");
                ImGui::SameLine();
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
                        ImGui::SameLine();
                        ImGui::TextColored(COL_DIM, "(%s/%s @ %s/s)",
                                           formatSize(doneBytes).c_str(),
                                           formatSize(totalB).c_str(),
                                           formatSize((int64_t)bps).c_str());
                    }
                    else
                    {
                        ImGui::TextColored(COL_DIM, "ETA ...");
                    }
                }

                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel"))
                    cancelWork_.store(true);
            }

            ImGui::SameLine(w - 250);
            ImGui::Checkbox("Hide Done", &hideFinished_);

            ImGui::SameLine(w - 100);
            if (ImGui::Button("Settings", {90, 0}))
                showSettings_ = !showSettings_;

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Processing stats (visible during generation) ────────────
        if (working_.load())
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.08f, 0.12f, 1.0f));
            float statsH = ImGui::GetFrameHeight() * 2 + 14;
            ImGui::BeginChild("##ProcessingStats", {w, statsH}, false);
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

        ImGui::BeginChild("##Content", {w, contentH}, false);
        {
            float tableH = showLog_ ? contentH * splitRatio_ : contentH;
            float logH = showLog_ ? contentH - tableH - 4 : 0;

            // Table panel
            ImGui::BeginChild("##TablePanel", {w, tableH}, false);
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

                ImGui::BeginChild("##LogPanel", {w, logH}, false);
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
            {
                std::lock_guard lock(videosMutex_);
                for (auto &v : videos_)
                {
                    countAll++;
                    if (v.failed)
                        countFailed++;
                    else if (v.processed || v.hasThumb)
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

        // Snapshot under lock — COPY strings to avoid dangling pointers
        // (scan thread may reallocate videos_ vector while we render)
        struct RowSnap
        {
            std::string rel;
            ContainerType container;
            int64_t fileSize;
            bool hasThumb;
            bool hasCoverEmbed;
            bool processed;
            bool failed;
            bool tsFixed;
            bool remuxed;

            // Sort key helpers
            int containerKey() const { return (int)container; }
            int thumbKey() const { return hasCoverEmbed ? 2 : (hasThumb ? 1 : 0); }
            int statusKey() const
            {
                if (failed)
                    return 0;
                if (processed)
                    return 1;
                if (!hasThumb)
                    return 2;
                return 3; // skipped
            }
        };
        static thread_local std::vector<RowSnap> rows;
        rows.clear();
        {
            std::lock_guard lock(videosMutex_);
            rows.reserve(videos_.size());
            for (auto &v : videos_)
            {
                // Tab-based filtering
                bool isFailed = v.failed;
                bool isDone = !v.failed && (v.processed ||
                                            (v.hasThumb && v.container == ContainerType::RealMKV));
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
                                v.hasCoverEmbed, v.processed, v.failed, v.tsFixed, v.remuxed});
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
                                ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SortTristate;

        if (ImGui::BeginTable("##Videos", 5, flags, ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_DefaultSort, 4.0f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_PreferSortDescending, 0.7f);
            ImGui::TableSetupColumn("Container", ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
            ImGui::TableSetupColumn("Thumbnail", ImGuiTableColumnFlags_PreferSortDescending, 0.8f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_PreferSortDescending, 1.2f);
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

                    // File column
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(r.rel.c_str());

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
                        DrawBadge("Failed", COL_BADGE_ERR);
                    else if (r.processed)
                    {
                        DrawBadge("Done", COL_BADGE_OK);
                        if (r.remuxed)
                            DrawBadge("Remuxed", IM_COL32(100, 60, 200, 255));
                        if (r.tsFixed)
                            DrawBadge("TS Fixed", IM_COL32(60, 130, 200, 255));
                    }
                    else if (r.hasThumb && r.container == ContainerType::RealMKV)
                        DrawBadge("Skipped", COL_BADGE_SKIP);
                    else
                        DrawBadge("Pending", COL_BADGE_PEND);
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

        std::lock_guard lock(logMtx_);
        ImGuiListClipper clipper;
        clipper.Begin((int)log_.size());
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
            {
                const auto &line = log_[i];
                ImVec4 col = COL_TEXT;
                if (line.find("[ERROR]") != std::string::npos)
                    col = COL_RED;
                else if (line.find("[WARNING]") != std::string::npos)
                    col = COL_YELLOW;
                else if (line.find("[OK]") != std::string::npos)
                    col = COL_GREEN;
                else if (line.find("[INFO]") != std::string::npos)
                    col = COL_DIM;

                ImGui::TextColored(col, "%s", line.c_str());
            }
        }

        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);

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
            if (ImGui::SliderInt("##Threads", &threads, 1, 64))
                threadCount.store(threads);

            // Show note that changes apply immediately
            if (working_.load())
            {
                ImGui::Spacing();
                ImGui::TextColored(COL_GREEN, "Settings apply IMMEDIATELY to pending files!");
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
