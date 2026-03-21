// StripHelper C++ — ImGui GUI implementation
#include "app.h"
#include "pipeline.h"
#include "subprocess.h"
#include "shell_integration.h"
#include "utils/thumbnail_generator.h"
#include <imgui.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <cstring>
#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>

namespace sh
{

    // ── Helpers ─────────────────────────────────────────────────────────────────

    static ImU32 statusColor(RowStatus s)
    {
        switch (s)
        {
        case RowStatus::Pending:
            return IM_COL32(140, 140, 140, 255);
        case RowStatus::Queued:
            return IM_COL32(180, 180, 80, 255);
        case RowStatus::Working:
            return IM_COL32(80, 160, 255, 255);
        case RowStatus::Done:
            return IM_COL32(80, 220, 80, 255);
        case RowStatus::Error:
            return IM_COL32(240, 80, 80, 255);
        case RowStatus::Skipped:
            return IM_COL32(180, 130, 60, 255);
        }
        return IM_COL32(255, 255, 255, 255);
    }

    const char *App::statusLabel(RowStatus s)
    {
        switch (s)
        {
        case RowStatus::Pending:
            return "Pending";
        case RowStatus::Queued:
            return "Queued";
        case RowStatus::Working:
            return "Working";
        case RowStatus::Done:
            return "Done";
        case RowStatus::Error:
            return "Error";
        case RowStatus::Skipped:
            return "Skipped";
        }
        return "?";
    }

    void App::addLog(const std::string &line)
    {
        std::lock_guard lk(mtx_);
        log_.push_back(line);
        if (log_.size() > 500)
            log_.pop_front();
    }

    // ── Ctor ────────────────────────────────────────────────────────────────────

    App::App()
    {
        // Fill path buffers with current defaults so settings form is never blank
        std::strncpy(defaultPathBuf_, TO_PROCESS.string().c_str(), sizeof(defaultPathBuf_) - 1);
        std::strncpy(configPathBuf_, CONFIG_PATH.string().c_str(), sizeof(configPathBuf_) - 1);

        // Load persistent settings (may overwrite the defaults above)
        loadSettings();

        // Load config.json for symlink metadata
        try
        {
            if (fs::exists(CONFIG_PATH))
            {
                std::ifstream f(CONFIG_PATH);
                cfg_ = nlohmann::json::parse(f, nullptr, false, true);
            }
        }
        catch (...)
        {
        }

        // Default input path (if not set by settings)
        if (pathBuf_[0] == '\0')
        {
            auto p = TO_PROCESS.string();
            std::strncpy(pathBuf_, p.c_str(), sizeof(pathBuf_) - 1);
        }
    }

    void App::setPath(const std::string &p)
    {
        std::strncpy(pathBuf_, p.c_str(), sizeof(pathBuf_) - 1);
    }

    // ── Settings persistence ────────────────────────────────────────────────────

    void App::loadSettings()
    {
        try
        {
            if (!fs::exists(SETTINGS_PATH))
                return;
            std::ifstream f(SETTINGS_PATH);
            auto j = nlohmann::json::parse(f, nullptr, false, true);
            if (j.is_discarded())
                return;

            if (j.contains("threads"))
                threads_ = j["threads"].get<int>();
            if (j.contains("symlinks"))
                mkLinks_ = j["symlinks"].get<bool>();
            if (j.contains("repairPts"))
                repairPts_ = j["repairPts"].get<bool>();
            if (j.contains("deleteTs"))
                deleteTs_ = j["deleteTs"].get<bool>();
            if (j.contains("failedTsMaxMB"))
                failedTsMaxMB_ = j["failedTsMaxMB"].get<int>();
            if (j.contains("targetFps"))
                targetFps_ = j["targetFps"].get<int>();
            if (j.contains("audioSampleRate"))
                audioSampleRate_ = j["audioSampleRate"].get<int>();
            if (j.contains("audioChannels"))
                audioChannels_ = j["audioChannels"].get<int>();
            if (j.contains("capMaxW"))
                capMaxW_ = j["capMaxW"].get<int>();
            if (j.contains("capMaxH"))
                capMaxH_ = j["capMaxH"].get<int>();
            if (j.contains("defaultPath"))
            {
                auto s = j["defaultPath"].get<std::string>();
                std::strncpy(defaultPathBuf_, s.c_str(), sizeof(defaultPathBuf_) - 1);
                // Also set as initial path if we have one
                if (s.length() > 0 && pathBuf_[0] == '\0')
                    std::strncpy(pathBuf_, s.c_str(), sizeof(pathBuf_) - 1);
            }
            if (j.contains("configPath"))
            {
                auto s = j["configPath"].get<std::string>();
                std::strncpy(configPathBuf_, s.c_str(), sizeof(configPathBuf_) - 1);
                if (!s.empty())
                    CONFIG_PATH = s;
            }
            if (j.contains("toProcessPath"))
            {
                auto s = j["toProcessPath"].get<std::string>();
                if (!s.empty())
                    TO_PROCESS = s;
            }
            if (j.contains("thumbnailEnabled"))
                thumbnailEnabled_ = j["thumbnailEnabled"].get<bool>();
            if (j.contains("thumbnailWidth"))
                thumbnailWidth_ = j["thumbnailWidth"].get<int>();
            if (j.contains("thumbnailColumns"))
                thumbnailColumns_ = j["thumbnailColumns"].get<int>();
            if (j.contains("thumbnailRows"))
                thumbnailRows_ = j["thumbnailRows"].get<int>();

            // Clamp
            threads_ = std::clamp(threads_, 1, 32);
            failedTsMaxMB_ = std::clamp(failedTsMaxMB_, 0, 10000);
        }
        catch (...)
        {
        }
    }

    void App::saveSettings()
    {
        try
        {
            nlohmann::json j;
            j["threads"] = threads_;
            j["symlinks"] = mkLinks_;
            j["repairPts"] = repairPts_;
            j["deleteTs"] = deleteTs_;
            j["failedTsMaxMB"] = failedTsMaxMB_;
            j["targetFps"] = targetFps_;
            j["audioSampleRate"] = audioSampleRate_;
            j["audioChannels"] = audioChannels_;
            j["capMaxW"] = capMaxW_;
            j["capMaxH"] = capMaxH_;
            j["defaultPath"] = std::string(defaultPathBuf_);
            j["configPath"] = std::string(configPathBuf_);
            j["toProcessPath"] = TO_PROCESS.string();
            j["thumbnailEnabled"] = thumbnailEnabled_;
            j["thumbnailWidth"] = thumbnailWidth_;
            j["thumbnailColumns"] = thumbnailColumns_;
            j["thumbnailRows"] = thumbnailRows_;

            fs::create_directories(SETTINGS_PATH.parent_path());
            std::ofstream f(SETTINGS_PATH);
            f << j.dump(2);
            addLog("Settings saved.");
        }
        catch (const std::exception &e)
        {
            addLog(std::string("Failed to save settings: ") + e.what());
        }
    }

    // ── Top bar ─────────────────────────────────────────────────────────────────

    void App::browseFolder()
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog *pDlg = nullptr;
        HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void **)&pDlg);
        if (SUCCEEDED(hr))
        {
            DWORD opts = 0;
            pDlg->GetOptions(&opts);
            pDlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            pDlg->SetTitle(L"Select folder to process");
            if (SUCCEEDED(pDlg->Show(nullptr)))
            {
                IShellItem *pItem = nullptr;
                if (SUCCEEDED(pDlg->GetResult(&pItem)))
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                    {
                        char narrow[1024];
                        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
                        std::strncpy(pathBuf_, narrow, sizeof(pathBuf_) - 1);
                        CoTaskMemFree(path);
                    }
                    pItem->Release();
                }
            }
            pDlg->Release();
        }
        CoUninitialize();
    }

    void App::renderTopBar()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));

        // Settings button FIRST (right side), then the rest fills remaining space
        // Calculate how much space the right-side controls need:
        // Browse(90) + gap + threads(60) + "threads" text(~50) + gap + Symlinks(~80) + gap + Start/Stop(80) + gap + Settings(50) + gaps
        const float rightControlsW = 90 + 60 + 55 + 85 + 80 + 50 + 8 * 6; // ~468

        ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - rightControlsW));
        ImGui::InputText("##path", pathBuf_, sizeof(pathBuf_));
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(90, 0)))
            browseFolder();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(60);
        ImGui::InputInt("##threads", &threads_, 0, 0);
        threads_ = std::clamp(threads_, 1, 32);
        ImGui::SameLine();
        ImGui::TextUnformatted("threads");
        ImGui::SameLine();
        ImGui::Checkbox("Symlinks", &mkLinks_);
        ImGui::SameLine();
        ImGui::Checkbox("PTS fix", &repairPts_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Repair broken PTS in .ts files (slow).\nUncheck to skip for faster processing.");
        ImGui::SameLine();

        bool busy = running_.load();
        if (!busy)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
            if (ImGui::Button("Start", ImVec2(80, 0)))
                startProcessing();
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.75f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button("Stop", ImVec2(80, 0)))
                stopProcessing();
            ImGui::PopStyleColor(2);
        }

        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.35f, 0.35f, 0.42f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.45f, 0.45f, 0.55f, 1.0f));
        if (ImGui::Button("Settings", ImVec2(80, 0)))
            showSettings_ = true;
        ImGui::PopStyleColor(2);

        ImGui::PopStyleVar();
    }

    // ── Table ───────────────────────────────────────────────────────────────────

    void App::renderTable()
    {
        float logH = showLog_ ? logHeight_ : 0;
        ImVec2 avail = ImGui::GetContentRegionAvail();
        avail.y -= (logH + 50); // leave room for bottom bar + log

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Sortable | ImGuiTableFlags_SizingStretchProp;

        if (!ImGui::BeginTable("##folders", 9, flags, avail))
            return;

        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.30f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Written", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch, 0.20f);
        ImGui::TableHeadersRow();

        std::lock_guard lk(mtx_);
        ImGuiListClipper clipper;
        clipper.Begin((int)rows_.size());
        while (clipper.Step())
        {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
            {
                auto &r = rows_[i];
                ImGui::TableNextRow();
                ImU32 col = statusColor(r.status);

                // Folder
                ImGui::TableNextColumn();
                ImGui::PushStyleColor(ImGuiCol_Text, col);
                ImGui::TextUnformatted(r.relPath.c_str());

                // Status
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(statusLabel(r.status));

                // Stage
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.stage.c_str());

                // Progress bar
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Working || r.status == RowStatus::Done)
                {
                    float frac = r.pct / 100.0f;
                    char overlay[16];
                    snprintf(overlay, sizeof(overlay), "%.0f%%", r.pct);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(
                                                                      r.status == RowStatus::Done ? 0.3f : 0.2f,
                                                                      r.status == RowStatus::Done ? 0.8f : 0.5f,
                                                                      r.status == RowStatus::Done ? 0.3f : 0.9f, 1.0f));
                    ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
                    ImGui::PopStyleColor();
                }

                // ETA
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Working && r.eta > 0)
                    ImGui::Text("%s", humanEta(r.eta).c_str());

                // Written
                ImGui::TableNextColumn();
                if (r.written > 0)
                    ImGui::Text("%s", humanBytes(r.written).c_str());

                // Target
                ImGui::TableNextColumn();
                if (r.target > 0)
                    ImGui::Text("%s", humanBytes(r.target).c_str());

                // Duration
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.durInfo.c_str());

                // Info
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", r.info.c_str());

                ImGui::PopStyleColor(); // text color
            }
        }
        ImGui::EndTable();
    }

    // ── Bottom bar ──────────────────────────────────────────────────────────────

    void App::renderBottomBar()
    {
        int done = doneCount_.load();
        int errs = errCount_.load();
        int total = totalCount_.load();

        // Overall progress
        float frac = total > 0 ? (float)done / (float)total : 0.0f;
        char overlay[64];
        snprintf(overlay, sizeof(overlay), "%d / %d done   (%d errors)", done, total, errs);
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 0.3f, 1.0f));
        ImGui::ProgressBar(frac, ImVec2(-1, 20), overlay);
        ImGui::PopStyleColor();
    }

    // ── Log panel ───────────────────────────────────────────────────────────────

    void App::renderLogPanel()
    {
        if (ImGui::CollapsingHeader("Log", ImGuiTreeNodeFlags_DefaultOpen))
        {
            showLog_ = true;
            ImGui::BeginChild("##logscroll", ImVec2(0, logHeight_), ImGuiChildFlags_Borders);
            std::lock_guard lk(mtx_);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
            for (auto &line : log_)
                ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
            if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
                ImGui::SetScrollHereY(1.0f);
            ImGui::EndChild();
        }
        else
        {
            showLog_ = false;
        }
    }

    // ── Settings popup (tabbed like StreaMonitor) ─────────────────────────────

    void App::renderSettingsPopup()
    {
        if (!showSettings_)
            return;

        ImGui::SetNextWindowSize(ImVec2(620, 520), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 7));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));

        if (ImGui::Begin("Settings", &showSettings_, ImGuiWindowFlags_NoCollapse))
        {
            if (ImGui::BeginTabBar("##SettingsTabs"))
            {
                if (ImGui::BeginTabItem("General"))
                {
                    renderSettingsGeneral();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Video / Audio"))
                {
                    renderSettingsVideo();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Paths"))
                {
                    renderSettingsPaths();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Thumbnails"))
                {
                    renderSettingsThumbnails();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Shell Integration"))
                {
                    renderSettingsShellIntegration();
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Import"))
                {
                    renderSettingsImport();
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // ── Save / Cancel buttons ───────────────────────────────
            float buttonW = 140;
            float buttonH = 34;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float totalW = buttonW * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetContentRegionAvail().x - totalW) * 0.5f + ImGui::GetCursorPosX());

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.65f, 0.20f, 1.0f));
            if (ImGui::Button("Apply & Save", ImVec2(buttonW, buttonH)))
            {
                saveSettings();
                showSettings_ = false;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(buttonW, buttonH)))
            {
                loadSettings(); // revert changes
                showSettings_ = false;
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }

    void App::renderSettingsGeneral()
    {
        const float labelW = 260.0f;
        const float inputW = -1.0f;

        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Worker threads");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##threads", &threads_, 1, 4);
        threads_ = std::clamp(threads_, 1, 32);

        ImGui::Spacing();
        ImGui::Checkbox("Create symlinks by default", &mkLinks_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Create symlinks in the output folder\nfor models found in config.json.");

        ImGui::Spacing();
        ImGui::SeparatorText("File Cleanup");
        ImGui::Spacing();

        ImGui::Checkbox("Repair .ts PTS timestamps (slower, more reliable)", &repairPts_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Re-encode .ts files with broken PTS.\nDisable to skip — much faster but\nmay cause A/V sync issues.");

        ImGui::Checkbox("Delete .ts source after successful remux", &deleteTs_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Removes original .ts files after they've been\nsuccessfully remuxed to .mkv.");

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Auto-delete failed .ts max (MB)");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Failed .ts files smaller than this are auto-deleted.\nSet 0 to disable.");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##failedTsMB", &failedTsMaxMB_, 10, 50);
        failedTsMaxMB_ = std::clamp(failedTsMaxMB_, 0, 10000);
    }

    void App::renderSettingsVideo()
    {
        const float labelW = 260.0f;
        const float inputW = -1.0f;

        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Target FPS");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Target frame rate for re-encode operations.\nVideos with higher FPS will be capped.");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##fps", &targetFps_, 1, 5);
        targetFps_ = std::clamp(targetFps_, 1, 120);

        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Audio sample rate (Hz)");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##srate", &audioSampleRate_, 100, 1000);
        audioSampleRate_ = std::clamp(audioSampleRate_, 8000, 192000);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Audio channels");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##ach", &audioChannels_, 1, 1);
        audioChannels_ = std::clamp(audioChannels_, 1, 8);

        ImGui::Spacing();
        ImGui::SeparatorText("Resolution Limits");
        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Max width");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Videos wider than this will be scaled down\nduring re-encode. Set 3840 for 4K.");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##maxW", &capMaxW_, 100, 320);
        capMaxW_ = std::clamp(capMaxW_, 320, 7680);

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Max height");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputInt("##maxH", &capMaxH_, 100, 240);
        capMaxH_ = std::clamp(capMaxH_, 240, 4320);
    }

    void App::renderSettingsPaths()
    {
        const float labelW = 260.0f;
        const float inputW = -1.0f;

        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Default folder");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Default \"To Process\" folder.\nUsed when no path is specified.");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##defpath", defaultPathBuf_, sizeof(defaultPathBuf_));

        ImGui::Spacing();

        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Config JSON path");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Path to StreaMonitor config.json.\nUsed for symlink metadata and model info.");
        ImGui::SameLine(labelW);
        ImGui::SetNextItemWidth(inputW);
        ImGui::InputText("##cfgpath", configPathBuf_, sizeof(configPathBuf_));

        ImGui::Spacing();
        ImGui::SeparatorText("Current Paths");
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Settings file:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", SETTINGS_PATH.string().c_str());

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "FFmpeg:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", g_ffmpeg.c_str());

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "FFprobe:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", g_ffprobe.c_str());
    }

    void App::renderSettingsThumbnails()
    {
        const float labelW = 260.0f;
        const float inputW = -1.0f;

        ImGui::Spacing();

        ImGui::Checkbox("Generate thumbnail contact sheet after merge", &thumbnailEnabled_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Creates a contact sheet (.thumb.jpg) next to\nthe final 0.mkv after successful merge.");

        if (thumbnailEnabled_)
        {
            ImGui::Spacing();

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Width (px)");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputInt("##thumbW", &thumbnailWidth_, 10, 100);
            thumbnailWidth_ = std::clamp(thumbnailWidth_, 320, 3840);

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Columns");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputInt("##thumbCols", &thumbnailColumns_, 1, 1);
            thumbnailColumns_ = std::clamp(thumbnailColumns_, 1, 10);

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Rows");
            ImGui::SameLine(labelW);
            ImGui::SetNextItemWidth(inputW);
            ImGui::InputInt("##thumbRows", &thumbnailRows_, 1, 1);
            thumbnailRows_ = std::clamp(thumbnailRows_, 1, 10);

            ImGui::Spacing();
            int total = thumbnailColumns_ * thumbnailRows_;
            ImGui::TextColored(ImVec4(0.5f, 0.7f, 1.0f, 1.0f), "Grid: %dx%d = %d frames", thumbnailColumns_, thumbnailRows_, total);
        }
    }

    void App::renderSettingsShellIntegration()
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Add a \"Process with StripHelper\" option to the right-click context menu for folders in Windows Explorer.");
        ImGui::Spacing();

        bool installed = sh::isShellMenuInstalled();

        if (installed)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Status: Installed");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.20f, 1.0f));
            if (ImGui::Button("Uninstall Context Menu", ImVec2(-1, 34)))
            {
                sh::uninstallShellMenu();
                addLog("Context menu uninstalled.");
            }
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Status: Not installed");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            if (ImGui::Button("Install Context Menu", ImVec2(-1, 34)))
            {
                sh::installShellMenu();
                addLog("Context menu installed.");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Uses per-user registry (HKCU) — no admin required.");
    }

    void App::renderSettingsImport()
    {
        ImGui::Spacing();
        ImGui::TextWrapped("Import settings from a StreaMonitor config.json file. This will read model lists, download paths, and other relevant settings.");
        ImGui::Spacing();
        ImGui::SeparatorText("Import StreaMonitor Config");
        ImGui::Spacing();

        static char importPathBuf[1024] = {};
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Config file:");
        ImGui::SameLine(120);
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100);
        ImGui::InputText("##importPath", importPathBuf, sizeof(importPathBuf));
        ImGui::SameLine();
        if (ImGui::Button("Browse##imp", ImVec2(-1, 0)))
        {
            // Open file dialog
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            IFileOpenDialog *pDlg = nullptr;
            HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_IFileOpenDialog, (void **)&pDlg);
            if (SUCCEEDED(hr))
            {
                COMDLG_FILTERSPEC filters[] = {{L"JSON Files", L"*.json"}, {L"All Files", L"*.*"}};
                pDlg->SetFileTypes(2, filters);
                pDlg->SetTitle(L"Select StreaMonitor config.json");
                if (SUCCEEDED(pDlg->Show(nullptr)))
                {
                    IShellItem *pItem = nullptr;
                    if (SUCCEEDED(pDlg->GetResult(&pItem)))
                    {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(pItem->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                        {
                            char narrow[1024];
                            WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
                            std::strncpy(importPathBuf, narrow, sizeof(importPathBuf) - 1);
                            CoTaskMemFree(path);
                        }
                        pItem->Release();
                    }
                }
                pDlg->Release();
            }
            CoUninitialize();
        }

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.40f, 0.65f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.25f, 0.50f, 0.75f, 1.0f));
        if (ImGui::Button("Import Config", ImVec2(-1, 34)))
        {
            std::string path(importPathBuf);
            if (!path.empty())
            {
                importSmConfig(path);
            }
            else
            {
                addLog("Please specify a config file path.");
            }
        }
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::SeparatorText("What gets imported");
        ImGui::Spacing();
        ImGui::BulletText("Download directory (used as default folder)");
        ImGui::BulletText("Config file path for symlink metadata");
        ImGui::BulletText("Thumbnail settings (if present)");
        ImGui::BulletText("FFmpeg path overrides (if present)");
    }

    void App::importSmConfig(const std::string &path)
    {
        try
        {
            if (!fs::exists(path))
            {
                addLog("Import failed: file not found: " + path);
                return;
            }
            std::ifstream f(path);
            auto j = nlohmann::json::parse(f, nullptr, false, true);
            if (j.is_discarded())
            {
                addLog("Import failed: invalid JSON in " + path);
                return;
            }

            int imported = 0;

            // Download directory → default path
            if (j.contains("downloadDir") && j["downloadDir"].is_string())
            {
                auto dir = j["downloadDir"].get<std::string>();
                std::strncpy(defaultPathBuf_, dir.c_str(), sizeof(defaultPathBuf_) - 1);
                addLog("Imported downloadDir: " + dir);
                imported++;
            }

            // Config path: remember where this config is
            std::strncpy(configPathBuf_, path.c_str(), sizeof(configPathBuf_) - 1);
            CONFIG_PATH = path;
            addLog("Set config path: " + path);
            imported++;

            // Thumbnail settings
            if (j.contains("thumbnailWidth") && j["thumbnailWidth"].is_number())
            {
                thumbnailWidth_ = j["thumbnailWidth"].get<int>();
                imported++;
            }
            if (j.contains("thumbnailColumns") && j["thumbnailColumns"].is_number())
            {
                thumbnailColumns_ = j["thumbnailColumns"].get<int>();
                imported++;
            }
            if (j.contains("thumbnailRows") && j["thumbnailRows"].is_number())
            {
                thumbnailRows_ = j["thumbnailRows"].get<int>();
                imported++;
            }

            // FFmpeg path
            if (j.contains("ffmpegPath") && j["ffmpegPath"].is_string())
            {
                g_ffmpeg = j["ffmpegPath"].get<std::string>();
                addLog("Imported ffmpeg path: " + g_ffmpeg);
                imported++;
            }
            if (j.contains("ffprobePath") && j["ffprobePath"].is_string())
            {
                g_ffprobe = j["ffprobePath"].get<std::string>();
                addLog("Imported ffprobe path: " + g_ffprobe);
                imported++;
            }

            // Reload config.json for symlink data
            try
            {
                std::ifstream cf(CONFIG_PATH);
                cfg_ = nlohmann::json::parse(cf, nullptr, false, true);
            }
            catch (...)
            {
            }

            addLog("Import complete: " + std::to_string(imported) + " settings imported.");
            saveSettings();
        }
        catch (const std::exception &e)
        {
            addLog("Import failed: " + std::string(e.what()));
        }
    }

    // ── Main render ─────────────────────────────────────────────────────────────

    void App::render()
    {
        // Auto-start on first frame when launched from CLI with a folder path
        if (autoStart_ && !started_ && !running_.load())
        {
            autoStart_ = false;
            startProcessing();
        }

        // Full-window ImGui panel
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::Begin("##main", nullptr, wf);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));

        // Title
        ImGui::TextColored(ImVec4(0.4f, 0.75f, 1.0f, 1.0f), "StripHelper");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), " — Remux & Merge Pipeline");
        ImGui::Separator();

        renderTopBar();
        ImGui::Separator();
        renderTable();
        renderBottomBar();
        renderLogPanel();

        ImGui::PopStyleVar();
        ImGui::End();

        // Settings popup (rendered outside the main fullscreen window)
        renderSettingsPopup();
    }

    // ── Processing ──────────────────────────────────────────────────────────────

    void App::startProcessing()
    {
        if (running_.load())
            return;
        fs::path root(pathBuf_);
        if (!fs::is_directory(root))
        {
            addLog("ERROR: Not a valid directory: " + std::string(pathBuf_));
            return;
        }

        // Auto-persist current settings (threads, symlinks, etc.)
        saveSettings();

        // Reset so To Process gets wiped fresh for this batch
        if (mkLinks_)
            resetToProcessPurge();

        addLog("Scanning for work folders in " + root.string() + " ...");

        auto folders = findWorkFolders(root);
        if (folders.empty())
        {
            addLog("No work folders found.");
            return;
        }

        {
            std::lock_guard lk(mtx_);
            rows_.clear();
            log_.clear();
            for (auto &f : folders)
            {
                FolderRow r;
                r.absPath = f;
                r.relPath = fs::relative(f, root).string();
                rows_.push_back(std::move(r));
            }
        }

        totalCount_ = (int)folders.size();
        doneCount_ = 0;
        errCount_ = 0;
        nextIdx_ = 0;
        stopReq_ = false;
        resetGlobalStop(); // clear stop flag from previous run
        running_ = true;
        started_ = true;

        addLog("Found " + std::to_string(folders.size()) + " folders. Starting " + std::to_string(threads_) + " workers...");

        // Launch worker threads
        workers_.clear();
        int t = std::min(threads_, (int)folders.size());
        for (int i = 0; i < t; ++i)
            workers_.emplace_back(&App::workerMain, this);
    }

    void App::stopProcessing()
    {
        stopReq_ = true;
        requestGlobalStop(); // kill all active FFmpeg/FFprobe processes NOW
        addLog("Stop requested — killing active processes...");
        // Detach workers — they'll throw StopRequested and exit quickly now
        for (auto &w : workers_)
        {
            if (w.joinable())
                w.detach();
        }
        workers_.clear();
    }

    void App::workerMain()
    {
        while (!stopReq_.load())
        {
            int idx = nextIdx_.fetch_add(1);
            if (idx >= (int)totalCount_.load())
                break;
            processOne(idx);
        }

        // Check if we're the last worker
        int done = doneCount_.load() + errCount_.load();
        int total = totalCount_.load();
        if (done >= total || stopReq_.load())
        {
            bool expected = true;
            if (running_.compare_exchange_strong(expected, false))
            {
                addLog("=== Processing complete ===");
            }
        }
    }

    void App::processOne(int idx)
    {
        fs::path folder;
        {
            std::lock_guard lk(mtx_);
            if (idx >= (int)rows_.size())
                return;
            rows_[idx].status = RowStatus::Working;
            rows_[idx].stage = "starting";
            folder = rows_[idx].absPath;
        }

        // Track .ts processing stats for the Info column
        auto tsTotal = std::make_shared<int>(0);
        auto tsDone = std::make_shared<int>(0);
        auto ptsFixes = std::make_shared<int>(0);

        auto noteCb = [this, idx, tsTotal, tsDone, ptsFixes](const std::string &msg)
        {
            addLog("[" + std::to_string(idx) + "] " + msg);
            std::lock_guard lk(mtx_);
            if (idx >= (int)rows_.size())
                return;
            rows_[idx].stage = msg;

            // ── Parse notes to populate Info column live ──
            // Detect total TS count: "remux 19 TS parts -> MKV" or "remux 19 TS part"
            if (msg.find("TS part") != std::string::npos)
            {
                auto pos = msg.find("remux ");
                if (pos != std::string::npos)
                {
                    try
                    {
                        *tsTotal = std::stoi(msg.substr(pos + 6));
                    }
                    catch (...)
                    {
                    }
                }
            }
            // Detect convert count: "convert [1/5] file.tmp.ts"
            if (*tsTotal == 0 && msg.find("convert [") != std::string::npos)
            {
                auto sl = msg.find('/');
                auto rb = msg.find(']', sl != std::string::npos ? sl : 0);
                if (sl != std::string::npos && rb != std::string::npos)
                {
                    try
                    {
                        *tsTotal = std::stoi(msg.substr(sl + 1, rb - sl - 1));
                    }
                    catch (...)
                    {
                    }
                }
            }
            // Track PTS broken detections
            if (msg.find("PTS BROKEN") != std::string::npos ||
                msg.find("PTS discontinuity") != std::string::npos)
                ++(*ptsFixes);
            // Track completions
            if (msg.find("PTS repair OK") != std::string::npos ||
                msg.find("PTS clean") != std::string::npos ||
                (msg.find("ts streamcopy") != std::string::npos && msg.find(" OK") != std::string::npos) ||
                (msg.find("remux") != std::string::npos && msg.find(" OK") != std::string::npos && msg.find("pass") != std::string::npos))
                ++(*tsDone);

            // Update Info column with live .ts progress
            if (*tsTotal > 0)
            {
                std::string info = std::to_string(*tsDone) + "/" + std::to_string(*tsTotal) + " .ts done";
                if (*ptsFixes > 0)
                    info += " (" + std::to_string(*ptsFixes) + " PTS fix)";
                rows_[idx].info = info;
            }
        };

        auto guiCb = [this, idx](float pct, float eta, int64_t written, int64_t target)
        {
            std::lock_guard lk(mtx_);
            if (idx < (int)rows_.size())
            {
                rows_[idx].pct = pct;
                rows_[idx].eta = eta;
                rows_[idx].written = written;
                rows_[idx].target = target;
            }
        };

        auto metricCb = [this, idx](const std::string &label, int64_t srcSz, int64_t outSz,
                                    double srcDur, double outDur, const std::string &detail)
        {
            addLog("[" + std::to_string(idx) + "] metric " + label + ": " + humanBytes(srcSz) + "->" + humanBytes(outSz) + "  " + humanSecs(srcDur) + "->" + humanSecs(outDur) + "  " + detail);
            std::lock_guard lk(mtx_);
            if (idx < (int)rows_.size())
            {
                // For "total" metric, show total duration; for others show src->out
                if (label == "total")
                    rows_[idx].durInfo = humanSecs(srcDur);
                else
                    rows_[idx].durInfo = humanSecs(srcDur) + " -> " + humanSecs(outDur);
                rows_[idx].sizeInfo = humanBytes(srcSz) + " -> " + humanBytes(outSz);
            }
        };

        try
        {
            mergeFolder(folder, guiCb, cfg_, mkLinks_, noteCb, metricCb, repairPts_);

            // Generate thumbnail contact sheet after successful merge
            if (thumbnailEnabled_)
            {
                auto merged = folder / "0.mkv";
                if (fs::exists(merged))
                {
                    try
                    {
                        sm::ThumbnailConfig tc;
                        tc.width = thumbnailWidth_;
                        tc.columns = thumbnailColumns_;
                        tc.rows = thumbnailRows_;
                        auto thumbPath = merged;
                        thumbPath.replace_extension(".thumb.jpg");

                        noteCb("generating thumbnail...");

                        sm::generateContactSheet(merged.string(), thumbPath.string(), tc,
                                                 [this](const std::string &msg)
                                                 { addLog("[thumb] " + msg); });

                        if (fs::exists(thumbPath))
                            noteCb("thumbnail saved: " + thumbPath.filename().string());
                    }
                    catch (const std::exception &e)
                    {
                        addLog("[thumb] WARNING: " + std::string(e.what()));
                    }
                }
            }

            {
                std::lock_guard lk(mtx_);
                if (idx < (int)rows_.size())
                {
                    rows_[idx].status = RowStatus::Done;
                    rows_[idx].pct = 100.0f;
                    rows_[idx].eta = 0;
                    rows_[idx].info = "OK";
                }
            }
            doneCount_++;
        }
        catch (const StopRequested &)
        {
            // Stop requested — clean up temp files, don't count as error
            purgeTemp(folder);
            {
                std::lock_guard lk(mtx_);
                if (idx < (int)rows_.size())
                {
                    rows_[idx].status = RowStatus::Pending;
                    rows_[idx].stage = "stopped";
                    rows_[idx].pct = 0;
                    rows_[idx].eta = 0;
                }
            }
            return; // don't increment done or error count
        }
        catch (const PipelineError &e)
        {
            {
                std::lock_guard lk(mtx_);
                if (idx < (int)rows_.size())
                {
                    rows_[idx].status = RowStatus::Error;
                    rows_[idx].info = e.stage + ": " + e.detail.substr(0, 200);
                }
            }
            addLog("[" + std::to_string(idx) + "] ERROR: " + e.stage + " " + e.detail.substr(0, 200));
            errCount_++;
        }
        catch (const std::exception &e)
        {
            {
                std::lock_guard lk(mtx_);
                if (idx < (int)rows_.size())
                {
                    rows_[idx].status = RowStatus::Error;
                    rows_[idx].info = e.what();
                }
            }
            addLog("[" + std::to_string(idx) + "] ERROR: " + std::string(e.what()));
            errCount_++;
        }
    }

} // namespace sh
