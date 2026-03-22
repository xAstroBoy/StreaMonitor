// StripHelper C++ — ImGui GUI implementation (polished, matches StreaMonitor)
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
#include <cmath>
#include <windows.h>
#include <shobjidl.h>
#include <shlwapi.h>

namespace sh
{

    // ── Color palette (matches StreaMonitor exactly) ────────────────────────────

    static constexpr ImVec4 COL_ACCENT = {0.35f, 0.55f, 1.00f, 1.0f};
    static constexpr ImVec4 COL_ACCENT_HOVER = {0.45f, 0.65f, 1.00f, 1.0f};
    static constexpr ImVec4 COL_TEXT = {0.92f, 0.92f, 0.94f, 1.0f};
    static constexpr ImVec4 COL_TEXT_DIM = {0.50f, 0.50f, 0.55f, 1.0f};
    static constexpr ImVec4 COL_GREEN = {0.25f, 0.85f, 0.35f, 1.0f};
    static constexpr ImVec4 COL_RED = {0.95f, 0.25f, 0.25f, 1.0f};
    static constexpr ImVec4 COL_YELLOW = {0.95f, 0.80f, 0.15f, 1.0f};
    static constexpr ImVec4 COL_CYAN = {0.30f, 0.85f, 0.85f, 1.0f};
    static constexpr ImVec4 COL_ORANGE = {0.95f, 0.55f, 0.15f, 1.0f};
    static constexpr ImVec4 COL_BG_PANEL = {0.10f, 0.10f, 0.12f, 1.0f};

    // ── Status colors ───────────────────────────────────────────────────────────

    static ImVec4 statusColorVec(RowStatus s)
    {
        switch (s)
        {
        case RowStatus::Pending:
            return COL_TEXT_DIM;
        case RowStatus::Queued:
            return COL_YELLOW;
        case RowStatus::Working:
            return COL_CYAN;
        case RowStatus::Done:
            return COL_GREEN;
        case RowStatus::Error:
            return COL_RED;
        case RowStatus::Skipped:
            return COL_ORANGE;
        }
        return COL_TEXT;
    }

    static ImU32 statusColor(RowStatus s)
    {
        auto c = statusColorVec(s);
        return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255), 255);
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

    void App::syncSettingsToGlobals()
    {
        // Push App member variables → config.h inline globals
        // so pipeline.cpp (which reads globals directly) uses the saved values.
        DEFAULT_TARGET_FPS = targetFps_;
        TARGET_AUDIO_SR = audioSampleRate_;
        TARGET_AUDIO_CH = audioChannels_;
        DELETE_TS_AFTER_REMUX = deleteTs_;
        FAILED_TS_DELETE_MAX_BYTES = static_cast<int64_t>(failedTsMaxMB_) * 1024 * 1024;
    }

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
            if (j.contains("autoClose"))
                autoClose_ = j["autoClose"].get<bool>();
            if (j.contains("useCuda"))
                useCuda_ = j["useCuda"].get<bool>();

            // Apply encoder mode to global
            g_encoderMode.store(useCuda_ ? EncoderMode::CUDA : EncoderMode::CPU,
                                std::memory_order_relaxed);

            // Clamp
            threads_ = std::clamp(threads_, 1, 32);
            failedTsMaxMB_ = std::clamp(failedTsMaxMB_, 0, 10000);

            // Push loaded values into config.h globals so pipeline.cpp sees them
            syncSettingsToGlobals();
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
            j["defaultPath"] = std::string(defaultPathBuf_);
            j["configPath"] = std::string(configPathBuf_);
            j["toProcessPath"] = TO_PROCESS.string();
            j["thumbnailEnabled"] = thumbnailEnabled_;
            j["thumbnailWidth"] = thumbnailWidth_;
            j["thumbnailColumns"] = thumbnailColumns_;
            j["thumbnailRows"] = thumbnailRows_;
            j["autoClose"] = autoClose_;
            j["useCuda"] = useCuda_;

            fs::create_directories(SETTINGS_PATH.parent_path());
            std::ofstream f(SETTINGS_PATH);
            f << j.dump(2);
            addLog("Settings saved.");

            // Push saved values into config.h globals so pipeline.cpp sees them
            syncSettingsToGlobals();
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
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12, 6));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 6));

        ImGui::Spacing();

        // ── Calculate right-side width dynamically ────────────────────
        const auto &sty = ImGui::GetStyle();
        float sp = sty.ItemSpacing.x;                                   // default SameLine gap
        float fp2 = sty.FramePadding.x * 2.0f;                          // button horizontal padding
        float cbPad = ImGui::GetFrameHeight() + sty.ItemInnerSpacing.x; // checkbox square + inner gap

        // Measure the encoder toggle text dynamically
        const char *encLabel = useCuda_ ? " CUDA " : " CPU ";
        float rightW = sp + (ImGui::CalcTextSize(" Browse ").x + fp2)        // Browse button
                       + sp + 55.0f                                          // thread input
                       + sp + ImGui::CalcTextSize("threads").x               // "threads" label
                       + 16.0f + (cbPad + ImGui::CalcTextSize("Symlinks").x) // Symlinks checkbox
                       + 12.0f + (cbPad + ImGui::CalcTextSize("PTS fix").x)  // PTS fix checkbox
                       + 12.0f + (ImGui::CalcTextSize(encLabel).x + fp2)     // CPU/CUDA toggle
                       + 16.0f + 80.0f                                       // Start/Stop button
                       + 8.0f + ImGui::GetFrameHeight()                      // Settings gear button
                       + sp;                                                 // trailing pad

        // ── Path input (stretches to fill) ──────────────────────────
        ImGui::SetNextItemWidth(std::max(200.0f, ImGui::GetContentRegionAvail().x - rightW));
        ImGui::InputTextWithHint("##path", "Folder to process...", pathBuf_, sizeof(pathBuf_));

        // ── Browse button ───────────────────────────────────────────
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.18f, 0.18f, 0.22f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
        if (ImGui::Button(" Browse ", ImVec2(0, 0)))
            browseFolder();
        ImGui::PopStyleColor(2);

        // ── Thread count ────────────────────────────────────────────
        ImGui::SameLine();
        ImGui::SetNextItemWidth(55);
        ImGui::InputInt("##threads", &threads_, 0, 0);
        threads_ = std::clamp(threads_, 1, 32);
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "threads");

        // ── Checkboxes ──────────────────────────────────────────────
        ImGui::SameLine(0, 16);
        ImGui::Checkbox("Symlinks", &mkLinks_);
        ImGui::SameLine(0, 12);
        ImGui::Checkbox("PTS fix", &repairPts_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Repair broken PTS in .ts files (slower).\nUncheck to skip for faster processing.");

        // ── CPU / CUDA toggle ───────────────────────────────────────
        ImGui::SameLine(0, 12);
        if (useCuda_)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.50f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.60f, 0.20f, 1.0f));
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.18f, 0.18f, 0.22f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
        }
        if (ImGui::Button(useCuda_ ? " CUDA " : " CPU ", ImVec2(0, 0)))
        {
            useCuda_ = !useCuda_;
            g_encoderMode.store(useCuda_ ? EncoderMode::CUDA : EncoderMode::CPU,
                                std::memory_order_relaxed);
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Encoder: %s\nClick to switch to %s.",
                              useCuda_ ? "CUDA (NVENC GPU)" : "CPU (libx264/x265)",
                              useCuda_ ? "CPU" : "CUDA");

        // ── Start / Stop (green / red accent) ───────────────────────
        ImGui::SameLine(0, 16);
        bool busy = running_.load();
        if (!busy)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            if (ImGui::Button(" Start ", ImVec2(80, 0)))
                startProcessing();
            ImGui::PopStyleColor(2);
        }
        else
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.12f, 0.12f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.70f, 0.18f, 0.18f, 1.0f));
            if (ImGui::Button(" Stop ", ImVec2(80, 0)))
                stopProcessing();
            ImGui::PopStyleColor(2);
        }

        // ── Settings (gear button) ──────────────────────────────────
        ImGui::SameLine(0, 8);
        {
            float h = ImGui::GetFrameHeight();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.18f, 0.18f, 0.22f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
            if (ImGui::Button("\xE2\x9A\x99", ImVec2(h, h))) // UTF-8 gear ⚙
                showSettings_ = true;
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Settings");
        }

        ImGui::PopStyleVar(2);
    }

    // ── Table ───────────────────────────────────────────────────────────────────

    void App::renderTable()
    {
        ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                ImGuiTableFlags_Resizable |
                                ImGuiTableFlags_ScrollX |
                                ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Sortable |
                                ImGuiTableFlags_SizingStretchProp |
                                ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_PadOuterX;

        if (!ImGui::BeginTable("##folders", 9, flags, ImVec2(0, 0)))
            return;

        // Styled header
        ImGui::TableSetupScrollFreeze(1, 1);
        ImGui::TableSetupColumn("Folder", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Stage", ImGuiTableColumnFlags_WidthStretch, 0.18f);
        ImGui::TableSetupColumn("Progress", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("ETA", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Written", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Info", ImGuiTableColumnFlags_WidthStretch, 0.22f);
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
                ImVec4 col = statusColorVec(r.status);

                // ── Folder name ─────────────────────────────────────
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(r.relPath.c_str());

                // ── Status badge (colored pill) ─────────────────────
                ImGui::TableNextColumn();
                {
                    const char *label = statusLabel(r.status);
                    ImVec2 textSz = ImGui::CalcTextSize(label);
                    float padX = 8.0f, padY = 2.0f;
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    ImVec4 bgCol = col;
                    bgCol.w = 0.20f; // subtle background

                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImVec2 pMin = {cursor.x, cursor.y + 1};
                    ImVec2 pMax = {cursor.x + textSz.x + padX * 2, cursor.y + textSz.y + padY * 2 + 1};
                    dl->AddRectFilled(pMin, pMax, ImGui::ColorConvertFloat4ToU32(bgCol), 4.0f);

                    ImGui::SetCursorScreenPos({cursor.x + padX, cursor.y + padY + 1});
                    ImGui::TextColored(col, "%s", label);
                }

                // ── Stage ───────────────────────────────────────────
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Working)
                    ImGui::TextColored(COL_CYAN, "%s", r.stage.c_str());
                else
                    ImGui::TextColored(COL_TEXT_DIM, "%s", r.stage.c_str());

                // ── Progress bar (colored by status) ────────────────
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Working || r.status == RowStatus::Done)
                {
                    float frac = r.pct / 100.0f;
                    char overlay[16];
                    snprintf(overlay, sizeof(overlay), "%.0f%%", r.pct);

                    ImVec4 barCol = r.status == RowStatus::Done
                                        ? ImVec4(0.20f, 0.65f, 0.30f, 1.0f)
                                        : ImVec4(0.25f, 0.50f, 0.85f, 1.0f);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
                    ImGui::ProgressBar(frac, ImVec2(-1, 18), overlay);
                    ImGui::PopStyleColor();
                }

                // ── ETA ─────────────────────────────────────────────
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Working && r.eta > 0)
                    ImGui::TextColored(COL_TEXT_DIM, "%s", humanEta(r.eta).c_str());

                // ── Written ─────────────────────────────────────────
                ImGui::TableNextColumn();
                if (r.written > 0)
                    ImGui::TextColored(COL_TEXT, "%s", humanBytes(r.written).c_str());

                // ── Target ──────────────────────────────────────────
                ImGui::TableNextColumn();
                if (r.target > 0)
                    ImGui::TextColored(COL_TEXT, "%s", humanBytes(r.target).c_str());

                // ── Duration ────────────────────────────────────────
                ImGui::TableNextColumn();
                if (!r.durInfo.empty())
                    ImGui::TextColored(COL_ACCENT, "%s", r.durInfo.c_str());

                // ── Info ────────────────────────────────────────────
                ImGui::TableNextColumn();
                if (r.status == RowStatus::Error)
                    ImGui::TextColored(COL_RED, "%s", r.info.c_str());
                else if (r.status == RowStatus::Done)
                    ImGui::TextColored(COL_GREEN, "%s", r.info.c_str());
                else
                    ImGui::TextWrapped("%s", r.info.c_str());
            }
        }
        ImGui::EndTable();
    }

    // ── Bottom bar ──────────────────────────────────────────────────────────────

    void App::renderBottomBar()
    {
        // No-op: status is shown in the status bar at the bottom of the frame
    }

    // ── Log panel (SM-style with colored text) ──────────────────────────────────

    void App::renderLogPanel()
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, COL_BG_PANEL);
        ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None);

        std::lock_guard lk(mtx_);
        for (auto &line : log_)
        {
            // Color-code log lines by content
            if (line.find("ERROR") != std::string::npos || line.find("FAIL") != std::string::npos)
                ImGui::TextColored(COL_RED, "%s", line.c_str());
            else if (line.find("WARNING") != std::string::npos || line.find("WARN") != std::string::npos)
                ImGui::TextColored(COL_YELLOW, "%s", line.c_str());
            else if (line.find("OK") != std::string::npos || line.find("complete") != std::string::npos || line.find("Done") != std::string::npos || line.find("saved") != std::string::npos)
                ImGui::TextColored(COL_GREEN, "%s", line.c_str());
            else if (line.find("===") != std::string::npos || line.find("Starting") != std::string::npos || line.find("Found") != std::string::npos)
                ImGui::TextColored(COL_ACCENT, "%s", line.c_str());
            else
                ImGui::TextColored(COL_TEXT_DIM, "%s", line.c_str());
        }
        if (autoScroll_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20)
            ImGui::SetScrollHereY(1.0f);

        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    // ── Settings popup (tabbed like StreaMonitor) ─────────────────────────────

    void App::renderSettingsPopup()
    {
        if (!showSettings_)
            return;

        ImGui::SetNextWindowSize(ImVec2(700, 580), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(550, 400), ImVec2(1200, 900));
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
                if (ImGui::BeginTabItem("I/O"))
                {
                    renderSettingsIO();
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

        ImGui::Checkbox("Auto-close when finished", &autoClose_);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Automatically close the window once\nall folders have been processed.");

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

    void App::renderSettingsIO()
    {
        ImGui::Spacing();
        ImGui::SeparatorText("Encoder Mode");
        ImGui::Spacing();

        ImGui::TextWrapped("Choose the encoder used for re-encode operations "
                           "(PTS repair, salvage, concat re-encode). "
                           "Stream-copy operations (normal concat/remux) are always lossless and unaffected.");
        ImGui::Spacing();

        int mode = useCuda_ ? 1 : 0;
        if (ImGui::RadioButton("CPU  (libx264 / libx265)", &mode, 0))
        {
            useCuda_ = false;
            g_encoderMode.store(EncoderMode::CPU, std::memory_order_relaxed);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use software encoding (CPU).\nUniversally compatible, no driver needed.\nSlower but always works.");
        if (ImGui::RadioButton("CUDA  (NVENC h264/hevc/av1)", &mode, 1))
        {
            useCuda_ = true;
            g_encoderMode.store(EncoderMode::CUDA, std::memory_order_relaxed);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use NVIDIA GPU hardware encoding.\nMuch faster, requires an NVIDIA GPU.\nFalls back to CPU if GPU fails.");

        ImGui::Spacing();
        ImGui::TextColored(COL_TEXT_DIM, "Current: %s", useCuda_ ? "CUDA (NVENC GPU)" : "CPU (libx264/x265)");
        ImGui::TextColored(COL_TEXT_DIM, "Tip: You can also toggle this with the CPU/CUDA button in the top bar.");

        ImGui::Spacing();
        ImGui::SeparatorText("Concat Strategy");
        ImGui::Spacing();

        ImGui::TextWrapped("Concatenation always uses stream-copy (no re-encoding) by default. "
                           "Re-encoding only happens as a last resort when files have "
                           "incompatible codecs or broken timestamps that prevent direct copy.");
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

        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "mkvpropedit:");
        ImGui::SameLine();
        ImGui::TextWrapped("%s", g_mkvpropedit.c_str());
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
        ImGui::TextWrapped("Add a \"Process with StripHelper (Symlinks)\" option to the right-click context menu in Windows Explorer.");
        ImGui::TextWrapped("Works both when right-clicking ON a folder and when right-clicking inside a folder (background).");
        ImGui::Spacing();

        bool installed = sh::isShellMenuInstalled();

        if (installed)
        {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Context Menu: Installed");
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
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "Context Menu: Not installed");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.20f, 1.0f));
            if (ImGui::Button("Install Context Menu", ImVec2(-1, 34)))
            {
                sh::installShellMenu();
                addLog("Context menu installed (with --symlinks).");
            }
            ImGui::PopStyleColor(2);
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f), "Uses per-user registry (HKCU) \xe2\x80\x94 no admin required.");

        // ── Legacy cleanup ──────────────────────────────────────
        bool hasLegacy = sh::isLegacyMenuInstalled();
        if (hasLegacy)
        {
            ImGui::Spacing();
            ImGui::SeparatorText("Legacy Cleanup");
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Old \"MergeAllFilesSymlink\" entry found (from Python installer)");
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.35f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.45f, 0.15f, 1.0f));
            if (ImGui::Button("Remove Legacy Entry", ImVec2(-1, 34)))
            {
                if (sh::uninstallLegacyMenu())
                    addLog("Legacy MergeAllFilesSymlink entry removed.");
                else
                    addLog("WARNING: Could not remove legacy entry (may need admin).");
            }
            ImGui::PopStyleColor(2);
        }
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

        // ── Full-window panel (matches StreaMonitor layout) ─────────
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::SetNextWindowViewport(vp->ID);

        ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar |
                              ImGuiWindowFlags_NoResize |
                              ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoCollapse |
                              ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##main", nullptr, wf);
        ImGui::PopStyleVar(2);

        // ── Header bar ──────────────────────────────────────────────
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
        ImGui::BeginChild("##header", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
        {
            // Title + version
            ImGui::TextColored(COL_ACCENT, "StripHelper");
            ImGui::SameLine();
            ImGui::TextColored(COL_TEXT_DIM, " \xE2\x80\x94 Remux & Merge Pipeline");

            // Right-aligned: progress summary
            int done = doneCount_.load();
            int errs = errCount_.load();
            int total = totalCount_.load();
            if (total > 0)
            {
                char summary[128];
                snprintf(summary, sizeof(summary), "%d / %d done  (%d errors)", done, total, errs);
                float textW = ImGui::CalcTextSize(summary).x;
                ImGui::SameLine(ImGui::GetWindowWidth() - textW - 16);
                if (errs > 0)
                    ImGui::TextColored(COL_RED, "%s", summary);
                else if (done >= total)
                    ImGui::TextColored(COL_GREEN, "%s", summary);
                else
                    ImGui::TextColored(COL_TEXT_DIM, "%s", summary);
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::Separator();

        // ── Toolbar ─────────────────────────────────────────────────
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
        ImGui::BeginChild("##toolbar", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY);
        renderTopBar();
        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::Separator();

        // ── Content area: Table + Log with draggable splitter ───────
        float availH = ImGui::GetContentRegionAvail().y - 28.0f; // reserve for status bar
        float tableH = showLog_ ? availH * splitRatio_ : availH;

        // Table
        if (ImGui::BeginChild("##TableArea", ImVec2(0, tableH), ImGuiChildFlags_None))
            renderTable();
        ImGui::EndChild();

        if (showLog_)
        {
            // ── Draggable splitter bar ──────────────────────────────
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.15f, 0.15f, 0.18f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
            ImGui::Button("##Splitter", ImVec2(-1, 4));
            if (ImGui::IsItemActive())
            {
                splitRatio_ += ImGui::GetIO().MouseDelta.y / availH;
                splitRatio_ = std::clamp(splitRatio_, 0.2f, 0.9f);
            }
            ImGui::PopStyleColor(2);

            // Log panel fills remaining space above status bar
            float logH = ImGui::GetContentRegionAvail().y - 28.0f;
            if (logH < 40.0f)
                logH = 40.0f;
            if (ImGui::BeginChild("##LogArea", ImVec2(0, logH), ImGuiChildFlags_None))
                renderLogPanel();
            ImGui::EndChild();
        }

        ImGui::End(); // ##main

        // ── Status bar (fixed at bottom, like StreaMonitor) ─────────
        {
            ImGuiViewport *svp = ImGui::GetMainViewport();
            float barH = 28.0f;
            ImGui::SetNextWindowPos(ImVec2(svp->WorkPos.x, svp->WorkPos.y + svp->WorkSize.y - barH));
            ImGui::SetNextWindowSize(ImVec2(svp->WorkSize.x, barH));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 4));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.08f, 0.08f, 0.10f, 1.0f});

            ImGui::Begin("##StatusBar", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBringToFrontOnFocus);

            int done = doneCount_.load();
            int errs = errCount_.load();
            int total = totalCount_.load();

            // Left: progress
            if (total > 0)
            {
                float frac = (float)done / (float)total;
                char pctStr[32];
                snprintf(pctStr, sizeof(pctStr), "%.0f%%", frac * 100.0f);
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.20f, 0.55f, 0.30f, 1.0f));
                ImGui::ProgressBar(frac, ImVec2(200, 16), pctStr);
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            if (running_.load())
            {
                // Pulsating dot
                float pulse = (std::sin((float)ImGui::GetTime() * 4.0f) + 1.0f) * 0.5f;
                ImVec4 dotCol = {1.0f, 0.2f + pulse * 0.3f, 0.2f + pulse * 0.3f, 1.0f};
                ImGui::TextColored(dotCol, "\xE2\x97\x8F"); // ●
                ImGui::SameLine(0, 4);
                ImGui::TextColored(COL_TEXT, "Processing...");
            }
            else if (total > 0 && done >= total)
            {
                ImGui::TextColored(COL_GREEN, "\xE2\x9C\x93 Complete");
            }
            else
            {
                ImGui::TextColored(COL_TEXT_DIM, "Ready");
            }

            // Right: toggle log
            float toggleW = ImGui::CalcTextSize(showLog_ ? "Hide Log" : "Show Log").x + 16;
            ImGui::SameLine(ImGui::GetWindowWidth() - toggleW - 12);
            if (ImGui::SmallButton(showLog_ ? "Hide Log" : "Show Log"))
                showLog_ = !showLog_;

            ImGui::End();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

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

    App::~App()
    {
        // Ensure all workers are stopped and joined before members are destroyed
        stopReq_ = true;
        requestGlobalStop();
        for (auto &w : workers_)
        {
            if (w.joinable())
                w.join();
        }
        workers_.clear();
    }

    void App::stopProcessing()
    {
        stopReq_ = true;
        requestGlobalStop(); // kill all active FFmpeg/FFprobe processes NOW
        addLog("Stop requested — killing active processes...");
        // Join workers — they exit quickly because stopReq_ is set
        // and requestGlobalStop() kills all active FFmpeg/FFprobe processes.
        for (auto &w : workers_)
        {
            if (w.joinable())
                w.join();
        }
        workers_.clear();
        running_ = false;
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

        // Check if all items are finished (done + error covers every claimed index)
        // Use nextIdx_ to know how many items were claimed — items still pending
        // after stop don't increment done/error, but nextIdx_ tracks them.
        int claimed = std::min(nextIdx_.load(), totalCount_.load());
        int finished = doneCount_.load() + errCount_.load();
        bool allDone = (finished >= claimed) || stopReq_.load();
        if (allDone)
        {
            bool expected = true;
            if (running_.compare_exchange_strong(expected, false))
            {
                if (!stopReq_.load())
                {
                    addLog("=== Processing complete ===");
                    if (autoClose_)
                    {
                        addLog("Auto-close enabled — exiting...");
                        quit_ = true;
                    }
                }
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

            // ── Post-merge pipeline (matches ThumbnailTool) ──────────────
            auto merged = folder / "0.mkv";
            if (fs::exists(merged))
            {
                auto logCb = [this](const std::string &msg)
                { addLog("[post] " + msg); };

                // 1. VR spatial metadata injection (before thumbnail so frames show VR)
                try
                {
                    if (sm::isVRFromPath(folder.string()))
                    {
                        noteCb("injecting VR 180° SBS metadata...");
                        sm::injectVRSpatialMetadata(merged.string(), logCb, g_mkvpropedit);
                    }
                }
                catch (const std::exception &e)
                {
                    addLog("[post] VR metadata WARNING: " + std::string(e.what()));
                }

                // 2. Thumbnail: clean orphans → generate → embed → delete
                if (thumbnailEnabled_)
                {
                    try
                    {
                        // Clean up orphan .thumb.jpg files from per-segment recording
                        for (auto &entry : fs::directory_iterator(folder))
                        {
                            if (!entry.is_regular_file())
                                continue;
                            auto fname = entry.path().filename().string();
                            if (fname.size() > 10 && fname.find(".thumb.jpg") != std::string::npos)
                            {
                                std::error_code ec;
                                fs::remove(entry.path(), ec);
                                if (!ec)
                                    addLog("[thumb] cleaned orphan: " + fname);
                            }
                        }

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

                        // Embed thumbnail as cover art inside the MKV
                        if (fs::exists(thumbPath))
                        {
                            sm::embedThumbnailInMKV(merged.string(), thumbPath.string(), logCb, g_mkvpropedit, true);

                            // Delete .thumb.jpg after successful embed
                            std::error_code ec;
                            fs::remove(thumbPath, ec);
                            if (!ec)
                                noteCb("thumbnail embedded & cleaned up");
                            else
                                noteCb("thumbnail embedded (cleanup failed: " + ec.message() + ")");
                        }

                        // Fix DLNA cover attachment metadata
                        sm::fixCoverAttachmentMetadata(merged.string(), logCb, g_mkvpropedit);
                    }
                    catch (const std::exception &e)
                    {
                        addLog("[thumb] WARNING: " + std::string(e.what()));
                    }
                }

                // 3. Write THUMBNAILED=done tag (marks file as fully processed)
                try
                {
                    sm::writeProcessedTag(merged.string(), logCb, g_mkvpropedit);
                }
                catch (const std::exception &e)
                {
                    addLog("[post] tag WARNING: " + std::string(e.what()));
                }
            }

            // Clean up empty subfolders (Mobile, etc.)
            try
            {
                // Iterate in reverse depth order so children are removed before parents
                std::vector<fs::path> emptyDirs;
                for (auto &entry : fs::recursive_directory_iterator(folder, fs::directory_options::skip_permission_denied))
                {
                    if (entry.is_directory())
                        emptyDirs.push_back(entry.path());
                }
                // Sort deepest first
                std::sort(emptyDirs.begin(), emptyDirs.end(),
                          [](const fs::path &a, const fs::path &b)
                          {
                              return a.string().size() > b.string().size();
                          });
                for (auto &d : emptyDirs)
                {
                    if (fs::is_empty(d))
                    {
                        std::error_code ec;
                        fs::remove(d, ec);
                        if (!ec)
                            addLog("[" + std::to_string(idx) + "] removed empty dir: " + d.filename().string());
                    }
                }
            }
            catch (...)
            {
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
