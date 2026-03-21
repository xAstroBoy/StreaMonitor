// Sorter — App implementation (ImGui GUI for file sorting)
#include "app.h"
#include <imgui.h>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace sorter
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
        loadSettings();

        // Load config.json
        std::string err;
        configLoaded_ = loadConfig(cfg_.configPath, cfg_, err);
        if (!configLoaded_)
            configError_ = err;
        else
            addLog("[INFO] Config loaded: " + cfg_.configPath +
                   " (" + std::to_string(cfg_.vrTags.size()) + " VR tags, " +
                   std::to_string(cfg_.desktopTags.size()) + " Desktop tags, " +
                   std::to_string(cfg_.aliases.size()) + " aliases)");

        addLog("[INFO] Sorter ready — click Sort to begin");

        // Auto-sort on startup if configured
        if (cfg_.autoSortOnStartup && configLoaded_)
        {
            addLog("[INFO] Auto-sort on startup enabled");
            startSort();
        }
    }

    App::~App()
    {
        if (sortThread_.joinable())
            sortThread_.join();
        if (symlinkThread_.joinable())
            symlinkThread_.join();
    }

    void App::addLog(const std::string &line)
    {
        std::lock_guard lock(logMtx_);
        log_.push_back(line);
        while (log_.size() > 5000)
            log_.pop_front();
    }

    // ── Config ──────────────────────────────────────────────────────

    fs::path App::appConfigPath() const
    {
#ifdef _WIN32
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return fs::path(buf).parent_path() / "sorter_config.json";
#else
        return fs::current_path() / "sorter_config.json";
#endif
    }

    void App::loadSettings()
    {
        loadAppConfig(appConfigPath().string(), cfg_);
    }

    void App::saveSettings()
    {
        std::string err;
        if (saveAppConfig(appConfigPath().string(), cfg_, err))
            addLog("[INFO] Settings saved to " + appConfigPath().string());
        else
            addLog("[ERROR] Save failed: " + err);
    }

    // ── Sort ────────────────────────────────────────────────────────

    void App::startSort()
    {
        if (sorting_.load() || creatingSymlinks_.load())
            return;
        if (!configLoaded_)
        {
            addLog("[ERROR] Cannot sort — config not loaded");
            return;
        }

        if (sortThread_.joinable())
            sortThread_.join();

        {
            std::lock_guard lock(resultsMutex_);
            results_.clear();
        }
        sorting_.store(true);
        sortThread_ = std::jthread([this]()
                                   { sortWorker(); });
    }

    void App::sortWorker()
    {
        auto logCb = [this](const std::string &msg)
        { addLog(msg); };

        auto res = sortFiles(cfg_, logCb);

        {
            std::lock_guard lock(resultsMutex_);
            results_ = std::move(res);
        }
        sorting_.store(false);
    }

    void App::startSymlinks()
    {
        if (sorting_.load() || creatingSymlinks_.load())
            return;
        if (!configLoaded_)
        {
            addLog("[ERROR] Cannot create symlinks — config not loaded");
            return;
        }

        if (symlinkThread_.joinable())
            symlinkThread_.join();

        creatingSymlinks_.store(true);
        symlinkThread_ = std::jthread([this]()
                                      { symlinkWorker(); });
    }

    void App::symlinkWorker()
    {
        auto logCb = [this](const std::string &msg)
        { addLog(msg); };

        createViewSymlinks(cfg_, logCb);
        creatingSymlinks_.store(false);
    }

    // ── Render ──────────────────────────────────────────────────────

    void App::render()
    {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##Sorter", nullptr,
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
            ImGui::Text("Sorter");
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextColored(COL_DIM, " — Video File Organizer");
            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Toolbar ─────────────────────────────────────────────────
        {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.08f, 0.10f, 1.0f));
            ImGui::BeginChild("##Toolbar", {w, ImGui::GetFrameHeight() + 14}, false);
            ImGui::SetCursorPos({12, 6});

            bool isBusy = sorting_.load() || creatingSymlinks_.load();

            if (isBusy)
                ImGui::BeginDisabled();

            if (ImGui::Button("Sort Files", {100, 0}))
                startSort();
            ImGui::SameLine();

            if (ImGui::Button("Create Symlinks", {130, 0}))
                startSymlinks();

            if (isBusy)
                ImGui::EndDisabled();

            if (sorting_.load())
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "Sorting...");
            }
            else if (creatingSymlinks_.load())
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_YELLOW, "Creating symlinks...");
            }

            if (!configLoaded_)
            {
                ImGui::SameLine();
                ImGui::TextColored(COL_RED, "Config error: %s", configError_.c_str());
            }

            ImGui::SameLine(w - 100);
            if (ImGui::Button("Settings", {90, 0}))
                showSettings_ = !showSettings_;

            ImGui::EndChild();
            ImGui::PopStyleColor();
        }

        // ── Content area (results table + log) ──────────────────────
        float contentH = h - ImGui::GetCursorPosY() - statusH;
        if (contentH < 100)
            contentH = 100;

        float tableH = contentH * 0.5f;
        float logH = contentH - tableH - 4;

        // Results table
        ImGui::BeginChild("##ResultsPanel", {w, tableH}, false);
        {
            // Snapshot results under lock
            struct RowSnap
            {
                std::string source;
                std::string dest;
                std::string model;
                std::string tag;
                std::string folderType;
                bool mobile;
                bool success;
                bool skipped;
                std::string error;
            };
            static std::vector<RowSnap> rows;
            rows.clear();
            {
                std::lock_guard lock(resultsMutex_);
                rows.reserve(results_.size());
                for (auto &r : results_)
                {
                    rows.push_back({
                        r.sourcePath.filename().string(),
                        r.destPath.empty() ? "" : r.destPath.string(),
                        r.modelName,
                        r.tag,
                        r.folderType,
                        r.isMobile,
                        r.success,
                        r.skipped,
                        r.error,
                    });
                }
            }

            if (rows.empty())
            {
                ImGui::SetCursorPos({20, 20});
                ImGui::TextColored(COL_DIM, "No sort results yet. Click Sort Files to begin.");
            }
            else
            {
                ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp |
                                        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable;

                if (ImGui::BeginTable("##Results", 5, flags, ImGui::GetContentRegionAvail()))
                {
                    ImGui::TableSetupColumn("File", ImGuiTableColumnFlags_DefaultSort, 3.0f);
                    ImGui::TableSetupColumn("Model", 0, 1.5f);
                    ImGui::TableSetupColumn("Type", 0, 0.8f);
                    ImGui::TableSetupColumn("Status", 0, 0.8f);
                    ImGui::TableSetupColumn("Destination", 0, 3.0f);
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableHeadersRow();

                    ImGuiListClipper clipper;
                    clipper.Begin((int)rows.size());
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++)
                        {
                            auto &r = rows[i];
                            ImGui::TableNextRow();

                            // File
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(r.source.c_str());

                            // Model
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted(r.model.c_str());

                            // Type
                            ImGui::TableNextColumn();
                            if (r.folderType == "VR")
                                DrawBadge("VR", IM_COL32(140, 50, 200, 255));
                            else if (r.mobile)
                                DrawBadge("Mobile", IM_COL32(50, 130, 200, 255));
                            else if (!r.folderType.empty())
                                DrawBadge("Desktop", COL_BADGE_OK);

                            // Status
                            ImGui::TableNextColumn();
                            if (r.success)
                                DrawBadge("Moved", COL_BADGE_OK);
                            else if (r.skipped)
                                DrawBadge("Skipped", COL_BADGE_SKIP);
                            else
                                DrawBadge("Failed", COL_BADGE_ERR);

                            // Destination
                            ImGui::TableNextColumn();
                            if (!r.dest.empty())
                                ImGui::TextUnformatted(r.dest.c_str());
                            else if (!r.error.empty())
                                ImGui::TextColored(COL_RED, "%s", r.error.c_str());
                        }
                    }
                    ImGui::EndTable();
                }
            }
        }
        ImGui::EndChild();

        // Splitter
        ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.15f, 0.18f, 1.0f});
        ImGui::Button("##Splitter", {w, 4});
        ImGui::PopStyleColor();

        // Log panel
        ImGui::BeginChild("##LogPanel", {w, logH}, false);
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
                    else if (line.find("[WARNING]") != std::string::npos || line.find("[SKIP]") != std::string::npos)
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
        ImGui::EndChild();

        // ── Status bar ──────────────────────────────────────────────
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                pos, {pos.x + w, pos.y + statusH}, COL_STATUS_BG);

            ImGui::SetCursorScreenPos({pos.x + 12, pos.y + 2});

            int ok = 0, fail = 0, skip = 0;
            {
                std::lock_guard lock(resultsMutex_);
                for (auto &r : results_)
                {
                    if (r.success)
                        ok++;
                    else if (r.skipped)
                        skip++;
                    else
                        fail++;
                }
            }

            if (sorting_.load())
            {
                ImGui::TextColored(COL_YELLOW, "Sorting files...");
            }
            else if (creatingSymlinks_.load())
            {
                ImGui::TextColored(COL_YELLOW, "Creating symlinks...");
            }
            else if (ok + fail + skip > 0)
            {
                ImGui::TextColored(COL_GREEN, "Moved: %d", ok);
                ImGui::SameLine(0, 16);
                ImGui::TextColored(COL_DIM, "Skipped: %d", skip);
                if (fail > 0)
                {
                    ImGui::SameLine(0, 16);
                    ImGui::TextColored(COL_RED, "Failed: %d", fail);
                }
            }
            else
            {
                ImGui::TextColored(COL_DIM, "Ready");
            }

            ImGui::SameLine(w - 200);
            ImGui::TextColored(COL_DIM, "Source: %s", cfg_.unsortedDir.c_str());
        }

        ImGui::End();

        // Settings popup
        if (showSettings_)
            renderSettingsPopup();
    }

    // ── Settings ────────────────────────────────────────────────────

    void App::renderSettingsPopup()
    {
        ImGui::SetNextWindowSize({520, 560}, ImGuiCond_Appearing);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                {0.5f, 0.5f});

        if (ImGui::Begin("Sorter Settings", &showSettings_, ImGuiWindowFlags_NoCollapse))
        {
            // Config path
            ImGui::SeparatorText("Config Source");
            static char cfgBuf[512] = {};
            static bool cfgInit = false;
            if (!cfgInit)
            {
                std::strncpy(cfgBuf, cfg_.configPath.c_str(), sizeof(cfgBuf) - 1);
                cfgInit = true;
            }
            ImGui::Text("config.json Path");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##CfgPath", cfgBuf, sizeof(cfgBuf)))
                cfg_.configPath = cfgBuf;

            if (ImGui::Button("Reload Config"))
            {
                cfg_.configPath = cfgBuf;
                std::string err;
                configLoaded_ = loadConfig(cfg_.configPath, cfg_, err);
                configError_ = configLoaded_ ? "" : err;
                if (configLoaded_)
                    addLog("[INFO] Config reloaded successfully");
                else
                    addLog("[ERROR] " + err);
            }

            // Directories
            ImGui::Spacing();
            ImGui::SeparatorText("Directories");

            auto dirInput = [](const char *label, std::string &val)
            {
                static char buf[512];
                std::strncpy(buf, val.c_str(), sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = 0;
                ImGui::Text("%s", label);
                ImGui::SetNextItemWidth(-1);
                std::string id = std::string("##") + label;
                if (ImGui::InputText(id.c_str(), buf, sizeof(buf)))
                    val = buf;
            };

            dirInput("Unsorted Directory", cfg_.unsortedDir);
            dirInput("Sorted Directory", cfg_.sortedDir);
            dirInput("New Videos Directory", cfg_.newVideosDir);
            dirInput("Mobile Symlinks", cfg_.mobileDir);
            dirInput("VR Symlinks", cfg_.vrDir);
            dirInput("Normal Symlinks", cfg_.normalDir);

            // Options
            ImGui::Spacing();
            ImGui::SeparatorText("Options");
            ImGui::Checkbox("Auto-sort on startup", &cfg_.autoSortOnStartup);
            ImGui::Checkbox("Create symlinks after sorting", &cfg_.createSymlinks);

            // Shell integration
            ImGui::Spacing();
            ImGui::SeparatorText("Shell Integration");
#ifdef _WIN32
            {
                HKEY hKey = nullptr;
                bool installed = (RegOpenKeyExA(HKEY_CURRENT_USER,
                                                "Software\\Classes\\Directory\\shell\\Sorter",
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
                                      "Software\\Classes\\Directory\\shell\\Sorter\\command");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\Background\\shell\\Sorter\\command");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\shell\\Sorter");
                        RegDeleteKeyA(HKEY_CURRENT_USER,
                                      "Software\\Classes\\Directory\\Background\\shell\\Sorter");
                        addLog("[INFO] Context menu uninstalled");
                    }
                }
                else
                {
                    ImGui::TextColored(COL_DIM, "Context menu: Not installed");
                    ImGui::SameLine();
                    if (ImGui::Button("Install##Shell"))
                    {
                        char exeBuf[MAX_PATH] = {};
                        GetModuleFileNameA(nullptr, exeBuf, MAX_PATH);
                        std::string exePath(exeBuf);

                        const char *keyPaths[] = {
                            "Software\\Classes\\Directory\\shell\\Sorter",
                            "Software\\Classes\\Directory\\Background\\shell\\Sorter",
                        };
                        const char *args[] = {"%1", "%V"};

                        for (int i = 0; i < 2; i++)
                        {
                            HKEY hk = nullptr;
                            RegCreateKeyExA(HKEY_CURRENT_USER, keyPaths[i], 0, nullptr,
                                            REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hk, nullptr);
                            if (hk)
                            {
                                const char *text = "Sort with Sorter";
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

            // Info
            ImGui::Spacing();
            ImGui::SeparatorText("Config Info");
            if (configLoaded_)
            {
                ImGui::TextColored(COL_DIM, "VR tags: %d", (int)cfg_.vrTags.size());
                ImGui::TextColored(COL_DIM, "Desktop tags: %d", (int)cfg_.desktopTags.size());
                ImGui::TextColored(COL_DIM, "Aliases: %d", (int)cfg_.aliases.size());
                ImGui::TextColored(COL_DIM, "Phone clip patterns: %d", (int)cfg_.phoneClips.size());
            }
            else
            {
                ImGui::TextColored(COL_RED, "Config not loaded: %s", configError_.c_str());
            }

            // Save / Close
            ImGui::Spacing();
            if (ImGui::Button("Save Settings", {140, 0}))
                saveSettings();
            ImGui::SameLine();
            if (ImGui::Button("Close", {120, 0}))
                showSettings_ = false;
        }
        ImGui::End();
    }

} // namespace sorter
