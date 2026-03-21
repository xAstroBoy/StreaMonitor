// ThumbnailTool — App implementation (SM-themed, multithreaded)
#include "app.h"
#include "utils/thumbnail_generator.h"

#include <imgui.h>
#include <algorithm>
#include <filesystem>
#include <cstring>

namespace fs = std::filesystem;

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

    // ── Constructor / Destructor ────────────────────────────────────

    App::App()
    {
        std::string defaultDir = "F:\\StripChat";
        std::strncpy(rootDir_, defaultDir.c_str(), sizeof(rootDir_) - 1);
        addLog("[INFO] ThumbnailTool ready — set root directory and click Scan");
    }

    App::~App()
    {
        cancelWork_.store(true);
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

    // ── Scan ────────────────────────────────────────────────────────

    void App::startScan()
    {
        {
            std::lock_guard lock(videosMutex_);
            videos_.clear();
        }
        totalVideos_ = 0;
        withThumb_ = 0;
        withoutThumb_ = 0;
        generated_.store(0);
        errors_.store(0);

        fs::path root(rootDir_);
        if (!fs::is_directory(root))
        {
            addLog("[ERROR] Not a valid directory: " + std::string(rootDir_));
            return;
        }

        addLog("[INFO] Scanning " + root.string() + " ...");

        int count = 0;
        for (auto &entry : fs::recursive_directory_iterator(root,
                                                            fs::directory_options::skip_permission_denied))
        {
            if (!entry.is_regular_file())
                continue;
            if (!isVideoFile(entry.path()))
                continue;

            VideoEntry ve;
            ve.videoPath = entry.path();
            ve.thumbPath = thumbPathForVideo(entry.path());
            ve.hasThumb = fs::exists(ve.thumbPath);

            count++;
            if (ve.hasThumb)
                withThumb_++;
            else
                withoutThumb_++;

            std::lock_guard lock(videosMutex_);
            videos_.push_back(std::move(ve));
        }

        totalVideos_ = count;
        scanned_ = true;
        addLog("[OK] Found " + std::to_string(count) + " videos (" +
               std::to_string(withThumb_) + " with thumbnails, " +
               std::to_string(withoutThumb_) + " missing)");
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
        nextIdx_.store(0);

        int count = 0;
        {
            std::lock_guard lock(videosMutex_);
            for (auto &v : videos_)
            {
                if (!v.hasThumb && !v.processed)
                    count++;
            }
        }
        totalToProcess_.store(count);

        if (count == 0)
        {
            addLog("[INFO] Nothing to generate — all videos have thumbnails");
            return;
        }

        addLog("[INFO] Starting generation of " + std::to_string(count) +
               " thumbnails with " + std::to_string(threadCount) + " threads...");

        working_.store(true);

        // Launch worker threads
        int numThreads = std::clamp(threadCount, 1, 16);
        workers_.clear();
        for (int t = 0; t < numThreads; t++)
            workers_.emplace_back([this]()
                                  { workerFunc(); });
    }

    void App::workerFunc()
    {
        sm::ThumbnailConfig tc;
        tc.width = thumbnailWidth;
        tc.columns = thumbnailColumns;
        tc.rows = thumbnailRows;

        // Build index list of work items
        std::vector<size_t> indices;
        {
            std::lock_guard lock(videosMutex_);
            for (size_t i = 0; i < videos_.size(); i++)
            {
                if (!videos_[i].hasThumb && !videos_[i].processed)
                    indices.push_back(i);
            }
        }

        // Work-stealing loop: each thread grabs the next unprocessed index
        while (!cancelWork_.load())
        {
            int myIdx = nextIdx_.fetch_add(1);
            if (myIdx >= (int)indices.size())
                break;

            size_t idx = indices[myIdx];
            std::string videoStr, thumbStr;
            {
                std::lock_guard lock(videosMutex_);
                videoStr = videos_[idx].videoPath.string();
                thumbStr = videos_[idx].thumbPath.string();
            }

            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_ = fs::path(videoStr).filename().string();
            }

            addLog("[INFO] Generating: " + fs::path(videoStr).filename().string());

            auto logCb = [this](const std::string &msg)
            { addLog("  " + msg); };
            bool ok = sm::generateContactSheet(videoStr, thumbStr, tc, logCb);

            {
                std::lock_guard lock(videosMutex_);
                videos_[idx].processed = true;
                if (ok)
                {
                    videos_[idx].hasThumb = true;
                    generated_.fetch_add(1);

                    // Embed in MKV if enabled
                    if (embedInVideo)
                        sm::embedThumbnailInMKV(videoStr, thumbStr, logCb);
                }
                else
                {
                    videos_[idx].failed = true;
                    videos_[idx].errorMsg = "Generation failed";
                    errors_.fetch_add(1);
                    addLog("[ERROR] Failed: " + fs::path(videoStr).filename().string());
                }
            }

            processedCount_.fetch_add(1);
        }

        // Last worker to finish marks generation complete
        if (processedCount_.load() >= totalToProcess_.load())
        {
            working_.store(false);
            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_.clear();
            }
            addLog("[OK] Generation complete — " + std::to_string(generated_.load()) +
                   " generated, " + std::to_string(errors_.load()) + " errors");
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
            ImGui::InputTextWithHint("##RootDir", "Root directory (e.g. F:\\StripChat)",
                                     rootDir_, sizeof(rootDir_));
            ImGui::SameLine();
            if (ImGui::Button("Scan", {80, 0}))
                startScan();
            ImGui::SameLine();

            bool busy = working_.load();
            if (busy)
                ImGui::BeginDisabled();
            if (ImGui::Button("Generate", {90, 0}))
                startGeneration();
            if (busy)
                ImGui::EndDisabled();

            if (busy)
            {
                ImGui::SameLine();
                int done = processedCount_.load();
                int total = totalToProcess_.load();
                float frac = total > 0 ? (float)done / total : 0.0f;
                ImGui::SetNextItemWidth(120);
                ImGui::ProgressBar(frac, {120, 0}, "");
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "%d/%d", done, total);
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel"))
                    cancelWork_.store(true);
            }

            ImGui::SameLine(w - 100);
            if (ImGui::Button("Settings", {90, 0}))
                showSettings_ = !showSettings_;

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
            if (scanned_)
            {
                ImGui::TextColored(COL_DIM, "Videos: %d", totalVideos_);
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
        if (!scanned_)
        {
            ImGui::SetCursorPos({20, 40});
            ImGui::TextColored(COL_DIM, "Enter a root directory and click Scan to discover videos.");
            return;
        }

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_Resizable;

        if (ImGui::BeginTable("##Videos", 3, flags, ImGui::GetContentRegionAvail()))
        {
            ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_DefaultSort, 5.0f);
            ImGui::TableSetupColumn("Thumbnail", 0, 1.0f);
            ImGui::TableSetupColumn("Status", 0, 1.2f);
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableHeadersRow();

            std::lock_guard lock(videosMutex_);
            for (size_t i = 0; i < videos_.size(); i++)
            {
                const auto &v = videos_[i];
                ImGui::TableNextRow();

                // File column
                ImGui::TableNextColumn();
                auto relPath = fs::relative(v.videoPath, fs::path(rootDir_));
                ImGui::TextUnformatted(relPath.string().c_str());

                // Thumbnail column
                ImGui::TableNextColumn();
                if (v.hasThumb)
                    DrawBadge("Yes", COL_BADGE_OK);
                else
                    DrawBadge("No", COL_BADGE_SKIP);

                // Status column
                ImGui::TableNextColumn();
                if (v.failed)
                    DrawBadge("Failed", COL_BADGE_ERR);
                else if (v.processed && v.hasThumb)
                    DrawBadge("Done", COL_BADGE_OK);
                else if (!v.hasThumb)
                    DrawBadge("Pending", COL_BADGE_PEND);
                else
                    DrawBadge("Skipped", COL_BADGE_SKIP);
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
        ImGui::SetNextWindowSize({420, 380}, ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                {0.5f, 0.5f});

        if (ImGui::Begin("Thumbnail Settings", &showSettings_, ImGuiWindowFlags_NoCollapse))
        {
            // Section: Thumbnail Grid
            ImGui::SeparatorText("Thumbnail Grid");

            ImGui::Text("Image Width (px)");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##Width", &thumbnailWidth, 1280, 7680, "%d px");

            ImGui::Spacing();
            ImGui::Text("Grid Columns");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##Cols", &thumbnailColumns, 2, 8);

            ImGui::Spacing();
            ImGui::Text("Grid Rows");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##Rows", &thumbnailRows, 2, 12);

            // Section: Processing
            ImGui::Spacing();
            ImGui::SeparatorText("Processing");

            ImGui::Text("Worker Threads");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderInt("##Threads", &threadCount, 1, 16);

            ImGui::Spacing();
            ImGui::Checkbox("Embed thumbnail in MKV (requires mkvpropedit)", &embedInVideo);

            // Info
            ImGui::Spacing();
            ImGui::SeparatorText("Info");
            ImGui::TextColored(COL_DIM, "Output: %dx%d contact sheet per video",
                               thumbnailColumns, thumbnailRows);
            ImGui::TextColored(COL_DIM, "Width: %d px (adaptive: scales up for high-res sources)",
                               thumbnailWidth);

            ImGui::Spacing();
            if (ImGui::Button("Close", {120, 0}))
                showSettings_ = false;
        }
        ImGui::End();
    }

    // Static member definition
    constexpr const char *App::kVideoExts[];

} // namespace tt
