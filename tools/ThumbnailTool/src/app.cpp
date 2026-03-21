// ThumbnailTool — App implementation
#include "app.h"
#include "utils/thumbnail_generator.h"

#include <imgui.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

namespace tt
{

    App::App() = default;

    App::~App()
    {
        cancelWork_.store(true);
        if (worker_ && worker_->joinable())
            worker_->join();
    }

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

    // ── Scan ────────────────────────────────────────────────────────

    void App::startScan()
    {
        std::lock_guard lock(videosMutex_);
        videos_.clear();
        totalVideos_ = 0;
        withThumb_ = 0;
        withoutThumb_ = 0;
        generated_ = 0;
        errors_ = 0;

        fs::path root(rootDir_);
        if (!fs::is_directory(root))
            return;

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

            totalVideos_++;
            if (ve.hasThumb)
                withThumb_++;
            else
                withoutThumb_++;

            videos_.push_back(std::move(ve));
        }

        scanned_ = true;
    }

    // ── Generation worker ───────────────────────────────────────────

    void App::startGeneration()
    {
        if (working_.load())
            return;

        cancelWork_.store(false);
        processedCount_.store(0);
        generated_ = 0;
        errors_ = 0;

        // Count how many need thumbnails
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
            return;

        showProgress_ = true;
        working_.store(true);
        worker_ = std::make_unique<std::jthread>([this]()
                                                 { workerFunc(); });
    }

    void App::workerFunc()
    {
        sm::ThumbnailConfig tc;
        tc.width = thumbnailWidth;
        tc.columns = thumbnailColumns;
        tc.rows = thumbnailRows;

        std::vector<size_t> indices;
        {
            std::lock_guard lock(videosMutex_);
            for (size_t i = 0; i < videos_.size(); i++)
            {
                if (!videos_[i].hasThumb && !videos_[i].processed)
                    indices.push_back(i);
            }
        }

        for (size_t idx : indices)
        {
            if (cancelWork_.load())
                break;

            std::string videoStr, thumbStr;
            {
                std::lock_guard lock(videosMutex_);
                videoStr = videos_[idx].videoPath.string();
                thumbStr = videos_[idx].thumbPath.string();
            }

            {
                std::lock_guard lock(currentFileMutex_);
                currentFile_ = videoStr;
            }

            bool ok = sm::generateContactSheet(videoStr, thumbStr, tc);

            {
                std::lock_guard lock(videosMutex_);
                videos_[idx].processed = true;
                if (ok)
                {
                    videos_[idx].hasThumb = true;
                    generated_++;
                }
                else
                {
                    videos_[idx].failed = true;
                    videos_[idx].errorMsg = "Generation failed";
                    errors_++;
                }
            }

            processedCount_.fetch_add(1);
        }

        working_.store(false);
        {
            std::lock_guard lock(currentFileMutex_);
            currentFile_.clear();
        }
    }

    // ── Render ──────────────────────────────────────────────────────

    void App::render()
    {
        renderMainPanel();

        if (showSettings_)
            renderSettingsPopup();
        if (showProgress_)
            renderProgressPopup();
    }

    void App::renderMainPanel()
    {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("ThumbnailTool", nullptr,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar |
                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::Text("Batch Video Thumbnail Generator");
        ImGui::Separator();
        ImGui::Spacing();

        // Root directory input
        ImGui::Text("Root Directory:");
        ImGui::SetNextItemWidth(-200.0f);
        ImGui::InputText("##RootDir", rootDir_, sizeof(rootDir_));
        ImGui::SameLine();
        if (ImGui::Button("Scan", {90, 0}))
            startScan();
        ImGui::SameLine();
        if (ImGui::Button("Settings", {90, 0}))
            showSettings_ = true;

        ImGui::Spacing();

        if (scanned_)
        {
            // Stats bar
            ImGui::TextColored(ImVec4(0.4f, 0.7f, 1.0f, 1.0f),
                               "Total: %d videos  |  With thumbnail: %d  |  Missing: %d",
                               totalVideos_, withThumb_, withoutThumb_);
            if (generated_ > 0 || errors_ > 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "  Generated: %d", generated_);
                if (errors_ > 0)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "  Errors: %d", errors_);
                }
            }

            ImGui::Spacing();

            // Generate button
            bool busy = working_.load();
            if (busy)
                ImGui::BeginDisabled();
            if (ImGui::Button("Generate Missing Thumbnails", {250, 32}))
                startGeneration();
            if (busy)
                ImGui::EndDisabled();

            if (busy)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Working... %d / %d",
                                   processedCount_.load(), totalToProcess_.load());
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Video list table
            if (ImGui::BeginTable("##Videos", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0, -1)))
            {
                ImGui::TableSetupColumn("File", 0, 4.0f);
                ImGui::TableSetupColumn("Thumbnail", 0, 1.0f);
                ImGui::TableSetupColumn("Status", 0, 1.2f);
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableHeadersRow();

                std::lock_guard lock(videosMutex_);
                for (size_t i = 0; i < videos_.size(); i++)
                {
                    const auto &v = videos_[i];
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    // Show relative path from root
                    auto relPath = fs::relative(v.videoPath, fs::path(rootDir_));
                    ImGui::TextUnformatted(relPath.string().c_str());

                    ImGui::TableNextColumn();
                    if (v.hasThumb)
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Yes");
                    else
                        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No");

                    ImGui::TableNextColumn();
                    if (v.failed)
                        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Failed");
                    else if (v.processed)
                        ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f), "Done");
                    else if (!v.hasThumb)
                        ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Pending");
                    else
                        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Skipped");
                }
                ImGui::EndTable();
            }
        }
        else
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "Enter a root directory above and click Scan to discover videos.");
        }

        ImGui::End();
    }

    void App::renderProgressPopup()
    {
        ImGui::SetNextWindowSize(ImVec2(500, 160), ImGuiCond_Appearing);
        if (ImGui::Begin("Generation Progress", &showProgress_,
                         ImGuiWindowFlags_NoCollapse))
        {
            int done = processedCount_.load();
            int total = totalToProcess_.load();
            float frac = total > 0 ? static_cast<float>(done) / total : 0.0f;

            ImGui::Text("Processing: %d / %d", done, total);
            ImGui::ProgressBar(frac, ImVec2(-1, 20));

            {
                std::lock_guard lock(currentFileMutex_);
                if (!currentFile_.empty())
                    ImGui::TextWrapped("Current: %s", currentFile_.c_str());
            }

            ImGui::Spacing();
            if (working_.load())
            {
                if (ImGui::Button("Cancel", {120, 0}))
                    cancelWork_.store(true);
            }
            else
            {
                ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.4f, 1.0f),
                                   "Complete! Generated: %d  Errors: %d", generated_, errors_);
                if (ImGui::Button("Close", {120, 0}))
                    showProgress_ = false;
            }
        }
        ImGui::End();
    }

    void App::renderSettingsPopup()
    {
        ImGui::SetNextWindowSize(ImVec2(400, 250), ImGuiCond_Appearing);
        if (ImGui::Begin("Thumbnail Settings", &showSettings_,
                         ImGuiWindowFlags_NoCollapse))
        {
            ImGui::Text("Thumbnail Width (px)");
            ImGui::SliderInt("##Width", &thumbnailWidth, 320, 1920);

            ImGui::Spacing();
            ImGui::Text("Grid Columns");
            ImGui::SliderInt("##Cols", &thumbnailColumns, 2, 8);

            ImGui::Spacing();
            ImGui::Text("Grid Rows");
            ImGui::SliderInt("##Rows", &thumbnailRows, 2, 12);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                               "Output: %dx%d contact sheet per video",
                               thumbnailColumns, thumbnailRows);

            ImGui::Spacing();
            if (ImGui::Button("Close", {120, 0}))
                showSettings_ = false;
        }
        ImGui::End();
    }

    // Static member definition
    constexpr const char *App::kVideoExts[];

} // namespace tt
