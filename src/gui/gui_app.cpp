// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — ImGui GUI Implementation (Full Feature)
// Dashboard with settings, disk usage, cross-register,
// animations, FFmpeg monitor, bot detail panel
// ─────────────────────────────────────────────────────────────────

#include "gui/gui_app.h"
#include "net/proxy_pool.h"
#include "net/mouflon.h"
#include "web/web_server.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <thread>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <Windows.h>
#include <shellapi.h>
#include "resources/resource.h"

// ── Win32 subclass to intercept minimize BEFORE GLFW processes it ────
// GLFW's iconify callback fires AFTER the minimize animation, causing
// a brief taskbar flash. Subclassing catches SC_MINIMIZE immediately.
static WNDPROC g_origWndProc = nullptr;
static sm::GuiApp *g_guiAppForSubclass = nullptr;

static LRESULT CALLBACK minimizeSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_MINIMIZE)
    {
        auto *self = g_guiAppForSubclass;
        if (self && self->shouldMinimizeToTray())
        {
            // Disable multi-viewport BEFORE hiding so ImGui pulls
            // all secondary OS windows back into the main viewport.
            // Then hide the main window — nothing remains on screen.
            ImGuiIO &io = ImGui::GetIO();
            io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
            ShowWindow(hwnd, SW_HIDE);
            self->setMinimizedToTray(true);
            return 0; // Swallow the message
        }
    }
    return CallWindowProcW(g_origWndProc, hwnd, msg, wp, lp);
}

#else
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // Safe detached thread helper — wraps lambda in try/catch to
    // prevent std::terminate() from unhandled exceptions on
    // background threads (the #1 cause of silent crashes).
    // ─────────────────────────────────────────────────────────────────
    template <typename Fn>
    static void safeDetach(Fn &&fn, const char *ctx = "background")
    {
        std::thread([f = std::forward<Fn>(fn), ctx]()
                    {
            try
            {
                f();
            }
            catch (const std::exception &e)
            {
                spdlog::error("[{}] thread exception: {}", ctx, e.what());
            }
            catch (...)
            {
                spdlog::error("[{}] unknown thread exception", ctx);
            } })
            .detach();
    }

    // ─────────────────────────────────────────────────────────────────
    // Color palette
    // ─────────────────────────────────────────────────────────────────
    static ImVec4 COL_BG_DARK = {0.06f, 0.06f, 0.08f, 1.0f};
    static ImVec4 COL_BG_PANEL = {0.10f, 0.10f, 0.12f, 1.0f};
    static ImVec4 COL_ACCENT = {0.35f, 0.55f, 1.00f, 1.0f};
    static ImVec4 COL_ACCENT_HOVER = {0.45f, 0.65f, 1.00f, 1.0f};
    static ImVec4 COL_ACCENT_DIM = {0.20f, 0.35f, 0.70f, 1.0f};
    static ImVec4 COL_TEXT = {0.92f, 0.92f, 0.94f, 1.0f};
    static ImVec4 COL_TEXT_DIM = {0.50f, 0.50f, 0.55f, 1.0f};
    static ImVec4 COL_GREEN = {0.25f, 0.85f, 0.35f, 1.0f};
    static ImVec4 COL_RED = {0.95f, 0.25f, 0.25f, 1.0f};
    static ImVec4 COL_YELLOW = {0.95f, 0.80f, 0.15f, 1.0f};
    static ImVec4 COL_ORANGE = {0.95f, 0.55f, 0.15f, 1.0f};
    static ImVec4 COL_MAGENTA = {0.85f, 0.35f, 0.85f, 1.0f};
    static ImVec4 COL_CYAN = {0.30f, 0.85f, 0.85f, 1.0f};
    static ImVec4 COL_RECORDING = {1.00f, 0.15f, 0.15f, 1.0f};

    // ─────────────────────────────────────────────────────────────────
    // Platform helpers
    // ─────────────────────────────────────────────────────────────────

    /// Open a folder in the native file manager
    static void openFolderInExplorer(const std::filesystem::path &folder)
    {
        if (!std::filesystem::exists(folder))
        {
            spdlog::warn("Folder does not exist: {}", folder.string());
            return;
        }
#ifdef _WIN32
        ShellExecuteA(nullptr, "explore", folder.string().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        std::string cmd = "open \"" + folder.string() + "\"";
        system(cmd.c_str());
#else
        std::string cmd = "xdg-open \"" + folder.string() + "\"";
        system(cmd.c_str());
#endif
    }

    /// Open a URL in the default browser
    static void openUrlInBrowser(const std::string &url)
    {
        if (url.empty())
            return;
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        std::string cmd = "open \"" + url + "\"";
        system(cmd.c_str());
#else
        std::string cmd = "xdg-open \"" + url + "\"";
        system(cmd.c_str());
#endif
    }

    /// Copy text to clipboard via ImGui
    static void copyToClipboard(const std::string &text)
    {
        ImGui::SetClipboardText(text.c_str());
    }

    /// Strip whitespace from a string
    static std::string stripWhitespace(const std::string &str)
    {
        std::string result;
        result.reserve(str.size());
        for (char c : str)
            if (!std::isspace(static_cast<unsigned char>(c)))
                result += c;
        return result;
    }

    /// Try to parse a site URL and extract (siteName, username).
    /// Returns true on success, filling outSite and outUsername.
    /// Supports URLs like:
    ///   https://stripchat.com/username
    ///   https://chaturbate.com/username/
    ///   https://www.camsoda.com/username
    ///   etc.
    static bool parseSiteUrl(const std::string &input, std::string &outSite, std::string &outUsername)
    {
        // Must start with http:// or https://
        std::string url = input;
        // Trim whitespace
        while (!url.empty() && std::isspace(static_cast<unsigned char>(url.front())))
            url.erase(url.begin());
        while (!url.empty() && std::isspace(static_cast<unsigned char>(url.back())))
            url.pop_back();

        if (url.size() < 10)
            return false;

        // Strip protocol
        std::string lower = url;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        size_t protoEnd = lower.find("://");
        if (protoEnd == std::string::npos)
            return false;
        std::string rest = url.substr(protoEnd + 3); // after ://

        // Split host/path
        auto slashPos = rest.find('/');
        if (slashPos == std::string::npos || slashPos == rest.size() - 1)
            return false;
        std::string host = rest.substr(0, slashPos);
        std::string path = rest.substr(slashPos + 1);

        // Remove trailing slash
        while (!path.empty() && path.back() == '/')
            path.pop_back();
        if (path.empty())
            return false;

        // Remove www. prefix from host
        std::string hostLower = host;
        std::transform(hostLower.begin(), hostLower.end(), hostLower.begin(), ::tolower);
        if (hostLower.substr(0, 4) == "www.")
            hostLower = hostLower.substr(4);

        // Map domains to site names
        // Extract the last path segment as username (handle paths like /cam/username or /live/username)
        auto lastSlash = path.rfind('/');
        std::string username = (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
        // Remove query string if present
        auto qpos = username.find('?');
        if (qpos != std::string::npos)
            username = username.substr(0, qpos);
        auto hpos = username.find('#');
        if (hpos != std::string::npos)
            username = username.substr(0, hpos);

        if (username.empty())
            return false;

        // Domain → Site name mapping
        struct DomainMap
        {
            const char *domain;
            const char *site;
        };
        static const DomainMap domains[] = {
            {"stripchat.com", "StripChat"},
            {"vr.stripchat.com", "StripChatVR"},
            {"chaturbate.com", "Chaturbate"},
            {"en.chaturbate.com", "Chaturbate"},
            {"de.chaturbate.com", "Chaturbate"},
            {"bongacams.com", "BongaCams"},
            {"de.bongacams.net", "BongaCams"},
            {"bongacams.net", "BongaCams"},
            {"camsoda.com", "CamSoda"},
            {"cam4.com", "Cam4"},
            {"hu.cam4.com", "Cam4"},
            {"cams.com", "Cams.com"},
            {"cherry.tv", "CherryTV"},
            {"dreamcamtrue.com", "DreamCam"},
            {"fansly.com", "FanslyLive"},
            {"flirt4free.com", "Flirt4Free"},
            {"manyvids.com", "ManyVids"},
            {"myfreecams.com", "MyFreeCams"},
            {"sexchat.hu", "SexChatHU"},
            {"streamate.com", "StreaMate"},
            {"pornhublive.com", "StreaMate"},
            {"xlovecam.com", "XLoveCam"},
            {"amateur.tv", "AmateurTV"},
        };

        // Check vr.stripchat.com first (before generic stripchat.com)
        for (const auto &dm : domains)
        {
            if (hostLower == dm.domain)
            {
                outSite = dm.site;
                outUsername = username;
                return true;
            }
        }

        // Also try suffix match (e.g. "de.bongacams.net" matches "bongacams.net")
        for (const auto &dm : domains)
        {
            std::string suffix = std::string(".") + dm.domain;
            if (hostLower.size() > suffix.size() &&
                hostLower.substr(hostLower.size() - suffix.size()) == suffix)
            {
                outSite = dm.site;
                outUsername = username;
                return true;
            }
        }

        return false;
    }

    /// Detect LAN IP address for intranet access
    static std::string detectLanIP()
    {
#ifdef _WIN32
        ULONG bufLen = 15000;
        std::vector<BYTE> buf(bufLen);
        auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
        DWORD ret = GetAdaptersAddresses(AF_INET,
                                         GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
                                         nullptr, pAddr, &bufLen);
        if (ret == ERROR_BUFFER_OVERFLOW)
        {
            buf.resize(bufLen);
            pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
            ret = GetAdaptersAddresses(AF_INET,
                                       GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_INCLUDE_GATEWAYS,
                                       nullptr, pAddr, &bufLen);
        }
        if (ret == NO_ERROR)
        {
            std::string ipWithGateway;
            std::string fallbackIp;

            for (auto a = pAddr; a; a = a->Next)
            {
                if (a->OperStatus != IfOperStatusUp)
                    continue;
                if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                    continue;

                // Check if this adapter has a non-zero gateway
                bool hasGateway = false;
                for (auto gw = a->FirstGatewayAddress; gw; gw = gw->Next)
                {
                    if (gw->Address.lpSockaddr->sa_family == AF_INET)
                    {
                        auto gwSa = reinterpret_cast<sockaddr_in *>(gw->Address.lpSockaddr);
                        if (gwSa->sin_addr.s_addr != 0)
                        {
                            hasGateway = true;
                            break;
                        }
                    }
                }

                for (auto u = a->FirstUnicastAddress; u; u = u->Next)
                {
                    if (u->Address.lpSockaddr->sa_family != AF_INET)
                        continue;
                    auto sa = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    std::string s(ip);
                    if (s == "127.0.0.1" || s.substr(0, 4) == "169.")
                        continue;

                    if (hasGateway && ipWithGateway.empty())
                        ipWithGateway = s;
                    else if (fallbackIp.empty())
                        fallbackIp = s;
                }
            }

            if (!ipWithGateway.empty())
                return ipWithGateway;
            if (!fallbackIp.empty())
                return fallbackIp;
        }
#else
        struct ifaddrs *ifaddr;
        if (getifaddrs(&ifaddr) == 0)
        {
            for (auto ifa = ifaddr; ifa; ifa = ifa->ifa_next)
            {
                if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                auto sa = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                std::string s(ip);
                if (s != "127.0.0.1" && s.substr(0, 4) != "169.")
                {
                    freeifaddrs(ifaddr);
                    return s;
                }
            }
            freeifaddrs(ifaddr);
        }
#endif
        return "127.0.0.1";
    }

    // ─────────────────────────────────────────────────────────────────
    // JSON Tree Viewer — renders a collapsible, color-coded JSON tree
    // ─────────────────────────────────────────────────────────────────
    static const ImVec4 kJsonKeyColor = {0.55f, 0.82f, 1.00f, 1.0f};    // light blue
    static const ImVec4 kJsonStringColor = {0.80f, 0.95f, 0.55f, 1.0f}; // green
    static const ImVec4 kJsonNumberColor = {0.95f, 0.70f, 0.40f, 1.0f}; // orange
    static const ImVec4 kJsonBoolColor = {0.95f, 0.55f, 0.85f, 1.0f};   // pink/magenta
    static const ImVec4 kJsonNullColor = {0.55f, 0.55f, 0.60f, 1.0f};   // gray
    static const ImVec4 kJsonBraceColor = {0.60f, 0.60f, 0.65f, 1.0f};  // dim gray

    static void renderJsonValue(const nlohmann::json &j, const std::string &key, bool isArrayElem, int depth);

    static void renderJsonInline(const nlohmann::json &j)
    {
        if (j.is_string())
        {
            std::string val = "\"" + j.get<std::string>() + "\"";
            ImGui::TextColored(kJsonStringColor, "%s", val.c_str());
        }
        else if (j.is_number_integer())
        {
            ImGui::TextColored(kJsonNumberColor, "%lld", (long long)j.get<int64_t>());
        }
        else if (j.is_number_float())
        {
            ImGui::TextColored(kJsonNumberColor, "%g", j.get<double>());
        }
        else if (j.is_boolean())
        {
            ImGui::TextColored(kJsonBoolColor, "%s", j.get<bool>() ? "true" : "false");
        }
        else if (j.is_null())
        {
            ImGui::TextColored(kJsonNullColor, "null");
        }
    }

    static void renderJsonValue(const nlohmann::json &j, const std::string &key, bool isArrayElem, int depth)
    {
        if (j.is_object() || j.is_array())
        {
            // Collapsible node
            bool isObj = j.is_object();
            std::string sizeHint = isObj
                                       ? " {" + std::to_string(j.size()) + " keys}"
                                       : " [" + std::to_string(j.size()) + " items]";

            // Use tree node with a unique ID
            ImGui::PushID(key.c_str());
            ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
            if (depth < 1)
                flags |= ImGuiTreeNodeFlags_DefaultOpen;
            // Small items (<=4 entries) auto-expand at shallow depth
            if (j.size() <= 4 && depth < 3)
                flags |= ImGuiTreeNodeFlags_DefaultOpen;

            // Build the tree label inline
            std::string nodeLabel;
            if (isArrayElem)
                nodeLabel = "[" + key + "]";
            else if (!key.empty())
                nodeLabel = key;
            else
                nodeLabel = isObj ? "Object" : "Array";
            nodeLabel += sizeHint;

            bool open = ImGui::TreeNodeEx(nodeLabel.c_str(), flags);

            if (open)
            {
                if (isObj)
                {
                    for (auto it = j.begin(); it != j.end(); ++it)
                        renderJsonValue(it.value(), it.key(), false, depth + 1);
                }
                else
                {
                    int idx = 0;
                    for (const auto &elem : j)
                    {
                        renderJsonValue(elem, std::to_string(idx), true, depth + 1);
                        idx++;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        else
        {
            // Leaf value — show key: value on one line
            ImGui::Bullet();
            if (!key.empty())
            {
                if (isArrayElem)
                    ImGui::TextColored(kJsonBraceColor, "[%s]", key.c_str());
                else
                    ImGui::TextColored(kJsonKeyColor, "%s:", key.c_str());
                ImGui::SameLine(0, 6);
            }
            renderJsonInline(j);
        }
    }

    static void renderJsonTree(const std::string &jsonStr, const char *searchBuf)
    {
        nlohmann::json parsed;
        try
        {
            parsed = nlohmann::json::parse(jsonStr);
        }
        catch (...)
        {
            // Not valid JSON — fall back to raw text
            ImGui::TextColored(COL_TEXT_DIM, "%s", jsonStr.c_str());
            return;
        }

        // If user typed a search query, show matching keys/values highlighted
        if (searchBuf && searchBuf[0] != '\0')
        {
            std::string query(searchBuf);
            std::transform(query.begin(), query.end(), query.begin(), ::tolower);

            // Flatten JSON and filter matches
            std::function<void(const nlohmann::json &, const std::string &)> searchTree;
            searchTree = [&](const nlohmann::json &node, const std::string &path)
            {
                if (node.is_object())
                {
                    for (auto it = node.begin(); it != node.end(); ++it)
                    {
                        std::string k = it.key();
                        std::string kLower = k;
                        std::transform(kLower.begin(), kLower.end(), kLower.begin(), ::tolower);
                        std::string childPath = path.empty() ? k : path + "." + k;

                        if (kLower.find(query) != std::string::npos)
                        {
                            ImGui::TextColored(kJsonKeyColor, "%s:", childPath.c_str());
                            ImGui::SameLine(0, 4);
                            if (it.value().is_primitive())
                                renderJsonInline(it.value());
                            else
                                ImGui::TextColored(kJsonBraceColor, "(%zu items)",
                                                   it.value().size());
                        }
                        else if (it.value().is_primitive())
                        {
                            std::string valStr = it.value().dump();
                            std::string valLower = valStr;
                            std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::tolower);
                            if (valLower.find(query) != std::string::npos)
                            {
                                ImGui::TextColored(kJsonKeyColor, "%s:", childPath.c_str());
                                ImGui::SameLine(0, 4);
                                renderJsonInline(it.value());
                            }
                        }
                        else
                        {
                            searchTree(it.value(), childPath);
                        }
                    }
                }
                else if (node.is_array())
                {
                    int idx = 0;
                    for (const auto &elem : node)
                    {
                        std::string childPath = path + "[" + std::to_string(idx) + "]";
                        if (elem.is_primitive())
                        {
                            std::string valStr = elem.dump();
                            std::string valLower = valStr;
                            std::transform(valLower.begin(), valLower.end(), valLower.begin(), ::tolower);
                            if (valLower.find(query) != std::string::npos)
                            {
                                ImGui::TextColored(kJsonBraceColor, "%s:", childPath.c_str());
                                ImGui::SameLine(0, 4);
                                renderJsonInline(elem);
                            }
                        }
                        else
                        {
                            searchTree(elem, childPath);
                        }
                        idx++;
                    }
                }
            };
            searchTree(parsed, "");
        }
        else
        {
            renderJsonValue(parsed, "", false, 0);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Constructor / Destructor
    // ─────────────────────────────────────────────────────────────────
    GuiApp::GuiApp(AppConfig &config, ModelConfigStore &configStore, BotManager &manager,
                   std::shared_ptr<ImGuiLogSink> logSink, WebServer *webServer)
        : config_(config), configStore_(configStore), manager_(manager),
          logSink_(std::move(logSink)), webServer_(webServer)
    {
        // Detect LAN URL once at startup
        cachedLanUrl_ = "http://" + detectLanIP() + ":" + std::to_string(config.webPort);

        // Clear preview cache on startup to avoid stale thumbnails
        {
            auto cachePath = std::filesystem::current_path() / ".cache" / "previews";
            std::error_code ec;
            if (std::filesystem::exists(cachePath, ec))
            {
                std::filesystem::remove_all(cachePath, ec);
                spdlog::info("Cleared preview cache: {}", cachePath.string());
            }
            std::filesystem::create_directories(cachePath, ec);
        }

        // Initialize settings edit state from config
        std::strncpy(editDownloadDir_, config.downloadsDir.string().c_str(), sizeof(editDownloadDir_) - 1);
        editResolution_ = config.wantedResolution;
        editPort_ = config.webPort;
        if (!config.ffmpegPath.empty())
            std::strncpy(editFfmpegPath_, config.ffmpegPath.string().c_str(), sizeof(editFfmpegPath_) - 1);
        std::strncpy(editFilenameFormat_, config.filenameFormat.c_str(), sizeof(editFilenameFormat_) - 1);

        // Stripchat spy private
        editSpyPrivateEnabled_ = config.spyPrivateEnabled;
        if (!config.stripchatCookies.empty())
            std::strncpy(editStripchatCookies_, config.stripchatCookies.c_str(), sizeof(editStripchatCookies_) - 1);

        switch (config.container)
        {
        case ContainerFormat::MKV:
            editContainerFmt_ = 0;
            break;
        case ContainerFormat::MP4:
            editContainerFmt_ = 1;
            break;
        case ContainerFormat::TS:
            editContainerFmt_ = 2;
            break;
        }

        // Initialize proxy edit state (use first proxy if available)
        editProxyEnabled_ = config.proxyEnabled;
        if (!config.proxies.empty())
        {
            const auto &firstProxy = config.proxies[0];
            std::strncpy(editProxyUrl_, firstProxy.url.c_str(), sizeof(editProxyUrl_) - 1);

            switch (firstProxy.type)
            {
            case ProxyType::HTTP:
                editProxyType_ = 0;
                break;
            case ProxyType::HTTPS:
                editProxyType_ = 1;
                break;
            case ProxyType::SOCKS4:
                editProxyType_ = 2;
                break;
            case ProxyType::SOCKS4A:
                editProxyType_ = 3;
                break;
            case ProxyType::SOCKS5:
                editProxyType_ = 4;
                break;
            case ProxyType::SOCKS5H:
                editProxyType_ = 5;
                break;
            default:
                editProxyType_ = 0;
                break;
            }
        }

        // Initialize encoding config edit state from config
        editEnableCuda_ = config.encoding.enableCuda;
        editCrf_ = config.encoding.crf;
        editAudioBitrate_ = config.encoding.audioBitrate;
        editCopyAudio_ = config.encoding.copyAudio;
        editMaxWidth_ = config.encoding.maxWidth;
        editMaxHeight_ = config.encoding.maxHeight;
        editEncoderThreads_ = config.encoding.threads;

        // Map encoder type enum to combo index
        switch (config.encoding.encoder)
        {
        case EncoderType::Copy:
            editEncoderType_ = 0;
            break;
        case EncoderType::X265:
            editEncoderType_ = 1;
            break;
        case EncoderType::X264:
            editEncoderType_ = 2;
            break;
        case EncoderType::NVENC_HEVC:
            editEncoderType_ = 3;
            break;
        case EncoderType::NVENC_H264:
            editEncoderType_ = 4;
            break;
        default:
            editEncoderType_ = 1;
            break;
        }

        // Map preset string to combo index
        const char *presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                 "medium", "slow", "slower", "veryslow", "placebo"};
        editPresetIdx_ = 5; // default to "medium"
        for (int i = 0; i < 10; ++i)
        {
            if (config.encoding.preset == presets[i])
            {
                editPresetIdx_ = i;
                break;
            }
        }

        // Initialize FFmpeg tuning edit state from config
        editFfmpegLiveLastSegments_ = config.ffmpeg.liveLastSegments;
        editFfmpegRwTimeoutSec_ = config.ffmpeg.rwTimeoutSec;
        editFfmpegSocketTimeoutSec_ = config.ffmpeg.socketTimeoutSec;
        editFfmpegReconnectDelayMax_ = config.ffmpeg.reconnectDelayMax;
        editFfmpegMaxRestarts_ = config.ffmpeg.maxRestarts;
        editFfmpegGracefulQuitTimeoutSec_ = config.ffmpeg.gracefulQuitTimeoutSec;
        editFfmpegStartupGraceSec_ = config.ffmpeg.startupGraceSec;
        editFfmpegSuspectStallSec_ = config.ffmpeg.suspectStallSec;
        editFfmpegStallSameTimeSec_ = config.ffmpeg.stallSameTimeSec;
        editFfmpegSpeedLowThreshold_ = config.ffmpeg.speedLowThreshold;
        editFfmpegSpeedLowSustainSec_ = config.ffmpeg.speedLowSustainSec;
        editFfmpegMaxSingleLagSec_ = config.ffmpeg.maxSingleLagSec;
        editFfmpegMaxConsecSkipLines_ = config.ffmpeg.maxConsecSkipLines;
        editFfmpegFallbackNoStderrSec_ = config.ffmpeg.fallbackNoStderrSec;
        editFfmpegFallbackNoOutputSec_ = config.ffmpeg.fallbackNoOutputSec;
        editFfmpegCooldownAfterStalls_ = config.ffmpeg.cooldownAfterStalls;
        editFfmpegCooldownSleepSec_ = config.ffmpeg.cooldownSleepSec;
        editFfmpegPlaylistProbeIntervalSec_ = config.ffmpeg.playlistProbeIntervalSec;
        std::strncpy(editFfmpegProbeSize_, config.ffmpeg.probeSize.c_str(), sizeof(editFfmpegProbeSize_) - 1);
        std::strncpy(editFfmpegAnalyzeDuration_, config.ffmpeg.analyzeDuration.c_str(), sizeof(editFfmpegAnalyzeDuration_) - 1);

        // System tray / behavior settings
        editMinimizeToTray_ = config.minimizeToTray;
        editEnablePreviewCapture_ = config.enablePreviewCapture;
#ifdef _WIN32
        editAutoStart_ = SystemTray::isAutoStartEnabled();
#endif

        // Web server edit state from config
        editWebEnabled_ = config.webEnabled;
        std::strncpy(editWebHost_, config.webHost.c_str(), sizeof(editWebHost_) - 1);
        editWebPort_ = config.webPort;
        std::strncpy(editWebUsername_, config.webUsername.c_str(), sizeof(editWebUsername_) - 1);
        std::strncpy(editWebPassword_, config.webPassword.c_str(), sizeof(editWebPassword_) - 1);
        std::memset(editWebNewPassword_, 0, sizeof(editWebNewPassword_));
    }

    GuiApp::~GuiApp()
    {
        cleanup();
    }

    // ─────────────────────────────────────────────────────────────────
    // Window initialization
    // ─────────────────────────────────────────────────────────────────
    bool GuiApp::initWindow()
    {
        if (!glfwInit())
        {
            spdlog::error("Failed to initialize GLFW");
            return false;
        }

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

        window_ = glfwCreateWindow(1600, 900,
                                   "StreaMonitor v2.0 - Stream Monitor & Recorder",
                                   nullptr, nullptr);
        if (!window_)
        {
            spdlog::error("Failed to create window");
            glfwTerminate();
            return false;
        }

        glfwMakeContextCurrent(window_);
        glfwSwapInterval(0); // Disable VSync — we use our own frame limiter

        // ── Apply dark title bar & custom border via DWM ─────────
#ifdef _WIN32
        {
            HWND hwnd = glfwGetWin32Window(window_);
            // Enable dark mode title bar (Win10 1809+ / Win11)
            BOOL useDarkMode = TRUE;
            ::DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                                    &useDarkMode, sizeof(useDarkMode));
            // Custom border color — match our dark theme
            COLORREF borderColor = RGB(25, 25, 35); // subtle dark blue-grey
            ::DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */,
                                    &borderColor, sizeof(borderColor));
            // Custom caption (title bar background) color
            COLORREF captionColor = RGB(15, 15, 20); // near-black matching COL_BG_DARK
            ::DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */,
                                    &captionColor, sizeof(captionColor));
            // Custom title text color
            COLORREF textColor = RGB(180, 180, 200); // subtle light text
            ::DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */,
                                    &textColor, sizeof(textColor));
        }
#endif

        // Set window icon (embedded RGBA pixel data)
        {
#include "resources/icon_data.h"
            GLFWimage icons[2];
            icons[0].width = kIconWidth;
            icons[0].height = kIconHeight;
            icons[0].pixels = const_cast<unsigned char *>(kIconPixels);
            icons[1].width = kIconSmallWidth;
            icons[1].height = kIconSmallHeight;
            icons[1].pixels = const_cast<unsigned char *>(kIconSmallPixels);
            glfwSetWindowIcon(window_, 2, icons);
        }

        // Register input callbacks for idle detection
        glfwSetWindowUserPointer(window_, this);
        glfwSetCursorPosCallback(window_, glfwCursorPosCallback);
        glfwSetMouseButtonCallback(window_, glfwMouseButtonCallback);
        glfwSetScrollCallback(window_, glfwScrollCallback);
        glfwSetKeyCallback(window_, glfwKeyCallback);
        glfwSetCharCallback(window_, glfwCharCallback);
        glfwSetWindowFocusCallback(window_, glfwWindowFocusCallback);

        // ── Close callback — set shutdown flag first ────────────
        glfwSetWindowCloseCallback(window_, [](GLFWwindow *w)
                                   {
                                       auto *self = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
                                       if (self)
                                           self->shuttingDown_.store(true);
                                       glfwSetWindowShouldClose(w, GLFW_TRUE); });

        // ── Win32 subclass: intercept minimize BEFORE GLFW processes it ─
        // GLFW's iconify callback fires AFTER the minimize animation,
        // causing the window to briefly flash on the taskbar. Subclassing
        // catches WM_SYSCOMMAND/SC_MINIMIZE and hides instantly.
#ifdef _WIN32
        {
            HWND hwnd = glfwGetWin32Window(window_);
            g_guiAppForSubclass = this;
            g_origWndProc = reinterpret_cast<WNDPROC>(
                SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                                  reinterpret_cast<LONG_PTR>(minimizeSubclassProc)));
        }
#endif

        return true;
    }

    void GuiApp::initImGui()
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();

        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Allow windows outside main viewport
        io.IniFilename = "imgui_layout.ini";

        // ── DPI-aware font scaling ──
        float xscale = 1.0f, yscale = 1.0f;
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor)
            glfwGetMonitorContentScale(monitor, &xscale, &yscale);
        dpiScale_ = std::max(xscale, yscale);
        if (dpiScale_ < 1.0f)
            dpiScale_ = 1.0f;

        // Fallback: if DPI reports 1.0 but monitor is high-res, auto-scale
        if (dpiScale_ <= 1.05f && monitor)
        {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            if (mode && mode->width >= 2560)
                dpiScale_ = std::max(dpiScale_, (float)mode->width / 1920.0f);
        }

        // Scale font size for the monitor DPI
        float fontSize = 16.0f * dpiScale_;
        ImFontConfig fontCfg;
        fontCfg.SizePixels = fontSize;
        fontCfg.OversampleH = 2;
        fontCfg.OversampleV = 1;

        // Try to load a system font that supports Unicode symbols (★ ✔ etc.)
        // Fall back to ProggyClean default if unavailable.
        bool fontLoaded = false;
#ifdef _WIN32
        {
            static const ImWchar glyphRanges[] = {
                0x0020,
                0x00FF, // Basic Latin + Latin Supplement
                0x2600,
                0x26FF, // Miscellaneous Symbols (★ etc.)
                0x2700,
                0x27BF, // Dingbats (✔ etc.)
                0x25A0,
                0x25FF, // Geometric Shapes (▶ etc.)
                0,      // terminator
            };
            const char *systemFonts[] = {
                "C:\\Windows\\Fonts\\segoeui.ttf",
                "C:\\Windows\\Fonts\\arial.ttf",
                "C:\\Windows\\Fonts\\consola.ttf",
            };
            for (const char *fontPath : systemFonts)
            {
                if (std::filesystem::exists(fontPath))
                {
                    fontCfg.GlyphRanges = glyphRanges;
                    ImFont *f = io.Fonts->AddFontFromFileTTF(fontPath, fontSize, &fontCfg);
                    if (f)
                    {
                        fontLoaded = true;
                        break;
                    }
                }
            }
        }
#endif
        if (!fontLoaded)
            io.Fonts->AddFontDefault(&fontCfg);
        io.FontGlobalScale = 1.0f; // already scaled via SizePixels

        // When viewports are enabled, tweak window rounding/bg for platform windows
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
        {
            ImGuiStyle &style = ImGui::GetStyle();
            style.WindowRounding = 0.0f;
            style.Colors[ImGuiCol_WindowBg].w = 1.0f; // Ensure opaque bg for detached windows
        }

        ImGui_ImplGlfw_InitForOpenGL(window_, true);
        ImGui_ImplOpenGL3_Init("#version 330");

        applyTheme();
    }

    void GuiApp::applyTheme()
    {
        ImGuiStyle &style = ImGui::GetStyle();
        ImVec4 *colors = style.Colors;

        colors[ImGuiCol_WindowBg] = COL_BG_DARK;
        colors[ImGuiCol_ChildBg] = COL_BG_PANEL;
        colors[ImGuiCol_PopupBg] = {0.08f, 0.08f, 0.10f, 0.96f};
        colors[ImGuiCol_Border] = {0.18f, 0.18f, 0.20f, 0.50f};
        colors[ImGuiCol_FrameBg] = {0.14f, 0.14f, 0.16f, 1.0f};
        colors[ImGuiCol_FrameBgHovered] = {0.20f, 0.20f, 0.24f, 1.0f};
        colors[ImGuiCol_FrameBgActive] = {0.26f, 0.26f, 0.30f, 1.0f};
        colors[ImGuiCol_TitleBg] = {0.08f, 0.08f, 0.10f, 1.0f};
        colors[ImGuiCol_TitleBgActive] = {0.12f, 0.12f, 0.14f, 1.0f};
        colors[ImGuiCol_MenuBarBg] = {0.08f, 0.08f, 0.10f, 1.0f};
        colors[ImGuiCol_Header] = {0.18f, 0.18f, 0.22f, 1.0f};
        colors[ImGuiCol_HeaderHovered] = COL_ACCENT;
        colors[ImGuiCol_HeaderActive] = COL_ACCENT_HOVER;
        colors[ImGuiCol_Button] = {0.18f, 0.18f, 0.22f, 1.0f};
        colors[ImGuiCol_ButtonHovered] = COL_ACCENT;
        colors[ImGuiCol_ButtonActive] = COL_ACCENT_HOVER;
        colors[ImGuiCol_Tab] = {0.12f, 0.12f, 0.14f, 1.0f};
        colors[ImGuiCol_TabHovered] = COL_ACCENT;
        colors[ImGuiCol_TabSelected] = {0.22f, 0.35f, 0.60f, 1.0f};
        colors[ImGuiCol_ScrollbarBg] = {0.06f, 0.06f, 0.08f, 0.5f};
        colors[ImGuiCol_ScrollbarGrab] = {0.28f, 0.28f, 0.32f, 1.0f};
        colors[ImGuiCol_TableHeaderBg] = {0.12f, 0.12f, 0.14f, 1.0f};
        colors[ImGuiCol_TableBorderStrong] = {0.18f, 0.18f, 0.20f, 1.0f};
        colors[ImGuiCol_TableBorderLight] = {0.14f, 0.14f, 0.16f, 1.0f};
        colors[ImGuiCol_TableRowBg] = {0.00f, 0.00f, 0.00f, 0.00f};
        colors[ImGuiCol_TableRowBgAlt] = {0.08f, 0.08f, 0.10f, 0.40f};
        colors[ImGuiCol_Text] = COL_TEXT;
        colors[ImGuiCol_TextDisabled] = COL_TEXT_DIM;
        colors[ImGuiCol_SeparatorHovered] = COL_ACCENT;
        colors[ImGuiCol_SeparatorActive] = COL_ACCENT_HOVER;
        colors[ImGuiCol_ResizeGrip] = {0.20f, 0.20f, 0.24f, 0.5f};
        colors[ImGuiCol_ResizeGripHovered] = COL_ACCENT;
        colors[ImGuiCol_ResizeGripActive] = COL_ACCENT_HOVER;

        float s = dpiScale_;
        style.WindowRounding = 6.0f * s;
        style.FrameRounding = 4.0f * s;
        style.GrabRounding = 4.0f * s;
        style.TabRounding = 4.0f * s;
        style.ScrollbarRounding = 4.0f * s;
        style.ChildRounding = 4.0f * s;
        style.PopupRounding = 4.0f * s;
        style.WindowPadding = {10 * s, 10 * s};
        style.FramePadding = {8 * s, 5 * s};
        style.ItemSpacing = {8 * s, 6 * s};
        style.ScrollbarSize = 14.0f * s;
        style.GrabMinSize = 12.0f * s;
        style.WindowBorderSize = 1.0f;
        style.ChildBorderSize = 0.0f;
        style.TabBorderSize = 0.0f;
        style.ScaleAllSizes(1.0f); // apply internally
    }

    void GuiApp::cleanup()
    {
        // Stop audio playback
        if (!audioStreamKey_.empty())
        {
            auto sep = audioStreamKey_.find('|');
            if (sep != std::string::npos)
                manager_.clearAudioDataCallback(
                    audioStreamKey_.substr(0, sep),
                    audioStreamKey_.substr(sep + 1));
            audioPlayer_.stop();
            audioStreamKey_.clear();
        }

        // Free preview texture
        if (detailPreviewTex_)
        {
            glDeleteTextures(1, &detailPreviewTex_);
            detailPreviewTex_ = 0;
        }

        if (window_)
        {
            ImGui_ImplOpenGL3_Shutdown();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window_);
            glfwTerminate();
            window_ = nullptr;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Main run loop
    // ─────────────────────────────────────────────────────────────────
    int GuiApp::run()
    {
        if (!initWindow())
            return 1;
        initImGui();

        // Register event callback — when any bot status changes, wake the
        // GUI from its idle sleep so the UI updates immediately.
        manager_.setEventCallback([this](const ManagerEvent &)
                                  {
                                      if (shuttingDown_.load())
                                          return; // Don't touch GLFW after close requested
                                      guiDirty_.store(true);
                                      glfwPostEmptyEvent(); // Wake glfwWaitEventsTimeout
                                  });

        // ── Adaptive frame rate ─────────────────────────────────────
        // Preview: 60 fps (Stream View / Bot Detail open with live video)
        // Active:  30 fps (user interacting / recent state changes)
        // Idle:     4 fps (nothing happening — minimal CPU usage)
        constexpr double kPreviewFrameTime = 1.0 / 60.0; // 16ms
        constexpr double kActiveFrameTime = 1.0 / 30.0;  // 33ms
        constexpr double kIdleFrameTime = 1.0 / 4.0;     // 250ms
        constexpr double kIdleTimeout = 2.0;             // seconds before going idle

        lastInputTime_ = glfwGetTime();

#ifdef _WIN32
        // ── Initialize system tray icon ─────────────────────────────
        tray_.init(window_);

        // Build tray context menu (rebuilt each frame for live status)
        auto buildTrayMenu = [this]()
        {
            std::vector<TrayMenuItem> items;

            // Status summary
            int totalBots = 0, recording = 0, online = 0, errors = 0;
            for (auto &s : cachedStates_)
            {
                totalBots++;
                if (s.recording)
                    recording++;
                if (s.status == Status::Public || s.status == Status::Online)
                    online++;
                if (s.status == Status::Error || s.status == Status::ConnectionError)
                    errors++;
            }

            items.push_back({"StreaMonitor v2.0", false});
            items.push_back({"", true, true}); // separator
            items.push_back({"Recording: " + std::to_string(recording) + " / Online: " + std::to_string(online), false});
            items.push_back({"Total: " + std::to_string(totalBots) + " models", false});
            if (errors > 0)
                items.push_back({"Errors: " + std::to_string(errors), false});
            items.push_back({"", true, true}); // separator
            items.push_back({"Show Window"});
            items.push_back({"Start All"});
            items.push_back({"Stop All"});
            items.push_back({"", true, true}); // separator
            items.push_back({"Quit"});

            tray_.setMenuItems(items, [this, errors](int idx)
                               {
                // Indices depend on whether "Errors" line is shown
                int showIdx = errors > 0 ? 6 : 5;
                int startIdx = showIdx + 1;
                int stopIdx = showIdx + 2;
                int quitIdx = showIdx + 4;

                if (idx == showIdx)
                {
                    tray_.clearRestore();
                    // Re-enable multi-viewport before showing
                    ImGuiIO &io = ImGui::GetIO();
                    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
                    HWND hwnd = glfwGetWin32Window(window_);
                    ShowWindow(hwnd, SW_SHOW);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    minimizedToTray_ = false;
                }
                else if (idx == startIdx)
                {
                    safeDetach([this]() {
                        manager_.startAllBots();
                        manager_.startAllGroups();
                    }, "tray-start-all");
                }
                else if (idx == stopIdx)
                {
                    safeDetach([this]() {
                        manager_.stopAll();
                        manager_.stopAllGroups();
                    }, "tray-stop-all");
                }
                else if (idx == quitIdx)
                {
                    shuttingDown_.store(true);
                    glfwSetWindowShouldClose(window_, GLFW_TRUE);
                } });
        };
        buildTrayMenu(); // initial build
#endif

        while (!glfwWindowShouldClose(window_))
        {
            try
            {
                // Determine frame rate tier
                double glfwNow = glfwGetTime();
                bool isActive = (glfwNow - lastInputTime_) < kIdleTimeout || guiDirty_.exchange(false);

                double targetFrameTime;
                if (showStreamView_ || showBotDetail_)
                    targetFrameTime = kPreviewFrameTime; // 60 fps for live video
                else if (isActive)
                    targetFrameTime = kActiveFrameTime; // 30 fps for interaction
                else
                    targetFrameTime = kIdleFrameTime; //  4 fps when idle

                // Sleep the thread until an OS event or the timeout — sole rate limiter.
                // Background threads call glfwPostEmptyEvent() to wake us.
                glfwWaitEventsTimeout(targetFrameTime);

#ifdef _WIN32
                // ── System tray message pump ────────────────────────────
                tray_.pollEvents();

                // Handle restore from tray (left-click on tray icon)
                if (tray_.wantsRestore())
                {
                    tray_.clearRestore();
                    // Re-enable multi-viewport before showing
                    ImGuiIO &io = ImGui::GetIO();
                    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
                    HWND hwnd = glfwGetWin32Window(window_);
                    ShowWindow(hwnd, SW_SHOW);
                    ShowWindow(hwnd, SW_RESTORE);
                    SetForegroundWindow(hwnd);
                    minimizedToTray_ = false;
                    markActive();
                }

                // Update tray tooltip with live status
                {
                    int rec = 0;
                    for (auto &s : cachedStates_)
                        if (s.recording)
                            rec++;
                    std::wstring tip = L"StreaMonitor";
                    if (rec > 0)
                        tip += L" - Recording " + std::to_wstring(rec) + L" model(s)";
                    tray_.setTooltip(tip);
                }

                // Rebuild tray menu periodically (state changes)
                buildTrayMenu();
#endif

                auto now = Clock::now();
                animTime_ = std::chrono::duration<float>(now.time_since_epoch()).count();

                // Refresh bot states periodically (state changes push via events,
                // this is just a safety net)
                if (std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - lastRefresh_)
                        .count() > 2000)
                {
                    refreshBotStates();
                    lastRefresh_ = now;
                }

                // Refresh disk usage every 60 seconds (recursive dir scan is expensive)
                if (std::chrono::duration_cast<std::chrono::seconds>(
                        now - lastDiskRefresh_)
                        .count() > 60)
                {
                    cachedDiskUsage_ = manager_.getDiskUsage();
                    lastDiskRefresh_ = now;
                }

                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplGlfw_NewFrame();
                ImGui::NewFrame();

                renderFrame();

                ImGui::Render();
                int displayW, displayH;
                glfwGetFramebufferSize(window_, &displayW, &displayH);
                glViewport(0, 0, displayW, displayH);
                glClearColor(COL_BG_DARK.x, COL_BG_DARK.y, COL_BG_DARK.z, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

                glfwSwapBuffers(window_);

                // ── Multi-viewport: update & render platform windows ────
                // This allows ImGui windows (settings, popups, etc.) to be
                // dragged outside the main application window as real OS windows.
                ImGuiIO &io = ImGui::GetIO();
                if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
                {
                    GLFWwindow *backup = glfwGetCurrentContext();
                    ImGui::UpdatePlatformWindows();
                    ImGui::RenderPlatformWindowsDefault();
                    glfwMakeContextCurrent(backup);

#ifdef _WIN32
                    // Set the program icon on all viewport windows (popups, etc.)
                    // so they show our icon in the taskbar instead of the default.
                    static HICON hIconBig = nullptr;
                    static HICON hIconSmall = nullptr;
                    if (!hIconBig)
                    {
                        HINSTANCE hInst = ::GetModuleHandle(nullptr);
                        hIconBig = ::LoadIcon(hInst, MAKEINTRESOURCE(IDI_ICON1));
                        hIconSmall = (HICON)::LoadImage(hInst, MAKEINTRESOURCE(IDI_ICON1),
                                                        IMAGE_ICON,
                                                        ::GetSystemMetrics(SM_CXSMICON),
                                                        ::GetSystemMetrics(SM_CYSMICON),
                                                        LR_DEFAULTCOLOR);
                    }
                    if (hIconBig)
                    {
                        ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
                        for (int i = 1; i < pio.Viewports.Size; i++) // skip 0 = main viewport
                        {
                            HWND hwnd = (HWND)pio.Viewports[i]->PlatformHandleRaw;
                            if (hwnd && ::SendMessage(hwnd, WM_GETICON, ICON_BIG, 0) != (LPARAM)hIconBig)
                            {
                                ::SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIconBig);
                                ::SendMessage(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(hIconSmall ? hIconSmall : hIconBig));
                            }
                        }
                    }
#endif
                }
                // No secondary sleep — glfwWaitEventsTimeout at top of loop
                // is the sole rate limiter.
            }
            catch (const std::exception &e)
            {
                spdlog::error("GUI main loop exception: {}", e.what());
                // Continue running despite error
            }
            catch (...)
            {
                spdlog::error("Unknown GUI main loop exception");
                // Continue running despite error
            }
        }

        // ── Orderly shutdown — prevent callbacks touching GLFW ──
        shuttingDown_.store(true);
        manager_.setEventCallback(nullptr);

#ifdef _WIN32
        tray_.shutdown();
#endif

        cleanup();
        return 0;
    }

    // ─────────────────────────────────────────────────────────────────
    // Frame rendering
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderFrame()
    {
        // Upload any decoded preview images to GL textures
        imageCache_.uploadPending();

        ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::SetNextWindowViewport(viewport->ID);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus |
                                 ImGuiWindowFlags_MenuBar;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::Begin("##MainWindow", nullptr, flags);
        ImGui::PopStyleVar(2);

        renderMenuBar();
        renderToolbar();

        // Main content area split: top = table, bottom = log
        float availH = ImGui::GetContentRegionAvail().y - 28.0f * dpiScale_; // Reserve for status bar
        float tableH = showLogPanel_ ? availH * splitRatio_ : availH;

        if (ImGui::BeginChild("##ModelTable", {0, tableH}, ImGuiChildFlags_None))
            renderModelTable();
        ImGui::EndChild();

        if (showLogPanel_)
        {
            // Splitter bar
            ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.15f, 0.18f, 1.0f});
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, COL_ACCENT);
            ImGui::Button("##Splitter", {-1, 4});
            if (ImGui::IsItemActive())
            {
                splitRatio_ += ImGui::GetIO().MouseDelta.y / availH;
                splitRatio_ = std::clamp(splitRatio_, 0.2f, 0.9f);
            }
            ImGui::PopStyleColor(2);

            float logH = ImGui::GetContentRegionAvail().y - 28.0f * dpiScale_;
            if (logH < 40.0f)
                logH = 40.0f;
            if (ImGui::BeginChild("##LogPanel", {0, logH}, ImGuiChildFlags_None))
                renderLogPanel();
            ImGui::EndChild();
        }

        ImGui::End(); // MainWindow

        // Status bar at bottom
        renderStatusBar();

        // Modal / popup windows
        if (showSettings_)
            renderSettingsWindow();
        if (showAddModel_)
            renderAddModelDialog();
        if (showAbout_)
            renderAboutWindow();
        if (showFFmpegMon_)
            renderFFmpegMonitor();
        if (showDiskUsage_)
            renderDiskUsagePanel();
        if (showCrossRegister_)
            renderCrossRegisterWindow();
        if (showBotDetail_ && selectedBot_ >= 0 && selectedBot_ < (int)cachedStates_.size())
            renderBotDetailPanel();
        if (showEditModel_)
            renderEditModelDialog();
        if (showStreamView_)
            renderStreamViewWindow();
        renderAddToGroupPopup();

        // ── Async move / resync operations ─────────────────────────
        if (pendingMoveBot_.has_value())
        {
            auto [user, site] = *pendingMoveBot_;
            pendingMoveBot_.reset();
            safeDetach([this, user, site]()
                       {
                auto result = manager_.moveFilesToUnprocessed(user, site);
                {
                    std::lock_guard lk(asyncResultMutex_);
                    moveResyncResult_ = "[" + site + "] " + user + ": " + result.message;
                }
                moveResyncDone_.store(true); }, "move-bot");
        }

        if (pendingResyncBot_.has_value())
        {
            auto [user, site] = *pendingResyncBot_;
            pendingResyncBot_.reset();
            safeDetach([this, user, site]()
                       {
                auto result = manager_.resyncBot(user, site);
                {
                    std::lock_guard lk(asyncResultMutex_);
                    moveResyncResult_ = "[" + site + "] " + user + ": " + result;
                }
                moveResyncDone_.store(true); }, "resync-bot");
        }

        // Show result toast
        if (moveResyncDone_.load())
        {
            moveResyncDone_.store(false);
            std::string msg;
            {
                std::lock_guard lk(asyncResultMutex_);
                msg = moveResyncResult_;
            }
            addLog("info", "system", msg);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Menu bar
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderMenuBar()
    {
        if (!ImGui::BeginMenuBar())
            return;

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Add Model...", "Ctrl+N"))
                showAddModel_ = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Save Config", "Ctrl+S"))
                manager_.saveConfig();
            ImGui::Separator();
            if (ImGui::MenuItem("Exit", "Alt+F4"))
            {
                shuttingDown_.store(true);
                glfwSetWindowShouldClose(window_, GLFW_TRUE);
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Log Panel", "Ctrl+L", &showLogPanel_);
            ImGui::MenuItem("Stream View", nullptr, &showStreamView_);
            ImGui::MenuItem("Disk Usage", "Ctrl+D", &showDiskUsage_);
            ImGui::MenuItem("FFmpeg Monitor", nullptr, &showFFmpegMon_);
            ImGui::MenuItem("Cross-Register", nullptr, &showCrossRegister_);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Control"))
        {
            if (ImGui::MenuItem("Start All"))
            {
                safeDetach([this]()
                           {
                    manager_.startAllBots();
                    manager_.startAllGroups(); }, "menu-start-all");
            }
            if (ImGui::MenuItem("Stop All"))
            {
                safeDetach([this]()
                           {
                    manager_.stopAll();
                    manager_.stopAllGroups(); }, "menu-stop-all");
            }
            if (ImGui::MenuItem("Resync All"))
            {
                safeDetach([this]()
                           {
                    auto result = manager_.resyncAll();
                    addLog("info", "system", result); }, "menu-resync-all");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Settings...", "Ctrl+,"))
                showSettings_ = true;
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About StreaMonitor"))
                showAbout_ = true;
            ImGui::EndMenu();
        }

        // Right-aligned live stats
        float textWidth = ImGui::CalcTextSize("REC: 00 | Online: 00 | Total: 000").x;
        ImGui::SameLine(ImGui::GetWindowWidth() - textWidth - 20);

        size_t recCount = manager_.recordingCount();
        size_t onCount = manager_.onlineCount();
        size_t totalCount = manager_.botCount();

        if (recCount > 0)
        {
            // Pulsating red for recording indicator
            float pulse = (std::sin(animTime_ * 3.0f) + 1.0f) * 0.5f;
            ImVec4 recCol = {1.0f, 0.1f + pulse * 0.2f, 0.1f + pulse * 0.2f, 1.0f};
            ImGui::TextColored(recCol, "REC: %zu", recCount);
        }
        else
        {
            ImGui::TextColored(COL_TEXT_DIM, "REC: 0");
        }

        ImGui::SameLine();
        ImGui::TextColored(COL_GREEN, "| Online: %zu", onCount);
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "| Total: %zu", totalCount);

        ImGui::EndMenuBar();
    }

    // ─────────────────────────────────────────────────────────────────
    // Toolbar
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderToolbar()
    {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {12 * dpiScale_, 6 * dpiScale_});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {6 * dpiScale_, 6 * dpiScale_});

        ImGui::Spacing();

        // Add button (green)
        ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.45f, 0.15f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f, 0.55f, 0.20f, 1.0f});
        if (ImGui::Button(" + Add Model "))
            showAddModel_ = true;
        ImGui::PopStyleColor(2);

        ImGui::SameLine();
        if (ImGui::Button(" Start All "))
        {
            safeDetach([this]()
                       {
                manager_.startAllBots();
                manager_.startAllGroups(); }, "toolbar-start-all");
        }
        ImGui::SameLine();
        if (ImGui::Button(" Stop All "))
        {
            safeDetach([this]()
                       {
                manager_.stopAll();
                manager_.stopAllGroups(); }, "toolbar-stop-all");
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.45f, 0.30f, 0.10f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.55f, 0.38f, 0.15f, 1.0f});
        if (ImGui::Button(" Move to Unprocessed "))
        {
            safeDetach([this]()
                       { manager_.moveAllFilesToUnprocessed(); }, "toolbar-move-all");
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.10f, 0.45f, 0.55f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.15f, 0.55f, 0.65f, 1.0f});
        if (ImGui::Button(" Resync All "))
        {
            safeDetach([this]()
                       { manager_.resyncAll(); }, "toolbar-resync-all");
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "(auto-saved)");

        // Right side: search bar + filter
        float searchFilterWidth = (200 + 150 + 12) * dpiScale_;
        float rightX = ImGui::GetWindowWidth() - searchFilterWidth;
        float curX = ImGui::GetCursorPosX();
        if (rightX > curX + 20 * dpiScale_)
            ImGui::SameLine(rightX);
        else
            ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * dpiScale_);
        ImGui::InputTextWithHint("##Search", "Search models...", searchBuf_, sizeof(searchBuf_));

        ImGui::SameLine();
        ImGui::SetNextItemWidth(150 * dpiScale_);
        const char *statusFilters[] = {"All Status", "Public", "Recording", "Private",
                                       "Offline", "Error", "Not Running"};
        ImGui::Combo("##StatusFilter", &filterStatus_, statusFilters,
                     IM_ARRAYSIZE(statusFilters));

        ImGui::PopStyleVar(2);
        ImGui::Separator();
    }

    // ─────────────────────────────────────────────────────────────────
    // Model table — grouped by username, with multi-select & bulk actions
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderModelTable()
    {
        float s = dpiScale_;

        // ── Build grouped rows by username ─────────────────────────
        struct GroupedRow
        {
            std::string username;
            std::string key; // lowercase
            std::vector<int> indices;
            std::string sitesStr;
            std::string groupName; // cross-register group name (if any)
            Status bestStatus = Status::NotRunning;
            bool anyRecording = false;
            bool anyRunning = false;
            bool anyMobile = false;
            uint64_t combinedSize = 0;
            double maxSpeed = 0.0;
            TimePoint earliestStart{};
            std::string primaryUrl;
            int primaryIdx = -1;
            int maxResW = 0;
            int maxResH = 0;
        };

        auto statusPrio = [](Status st) -> int
        {
            switch (st)
            {
            case Status::Public:
            case Status::Online:
                return 6;
            case Status::Private:
            case Status::Restricted:
                return 5;
            case Status::Offline:
                return 3;
            case Status::LongOffline:
                return 2;
            case Status::Error:
            case Status::RateLimit:
            case Status::Cloudflare:
                return 1;
            case Status::NotExist:
            case Status::Deleted:
                return 0;
            default:
                return -1;
            }
        };

        // ── Pre-compute cross-register group lookup ────────────────
        // Map (siteName, username) → group name for fast lookup
        std::unordered_map<std::string, std::string> botToGroup;
        {
            auto crGroups = manager_.getCrossRegisterGroups();
            for (const auto &cg : crGroups)
            {
                for (const auto &[site, user] : cg.members)
                {
                    std::string bkey = user + "\t" + site;
                    botToGroup[bkey] = cg.groupName;
                }
            }
        }

        std::unordered_map<std::string, GroupedRow> groupMap;
        for (int i = 0; i < (int)cachedStates_.size(); i++)
        {
            const auto &bot = cachedStates_[i];

            // Determine grouping key: cross-register group name, or lowercase username
            std::string crGroupName;
            {
                std::string bkey = bot.username + "\t" + bot.siteName;
                auto it = botToGroup.find(bkey);
                if (it != botToGroup.end())
                    crGroupName = it->second;
            }

            std::string key;
            if (!crGroupName.empty())
            {
                // Cross-register: group by group name (prefix to avoid collision with usernames)
                key = "\x01_crg_" + crGroupName;
            }
            else
            {
                key = bot.username;
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            }

            auto &grp = groupMap[key];
            if (grp.indices.empty())
            {
                grp.username = crGroupName.empty() ? bot.username : crGroupName;
                grp.key = key;
                grp.bestStatus = bot.status;
                grp.primaryUrl = bot.websiteUrl;
                grp.primaryIdx = i;
                grp.earliestStart = bot.startTime;
                grp.groupName = crGroupName;
            }
            grp.indices.push_back(i);
            if (!grp.sitesStr.empty())
                grp.sitesStr += ", ";
            // For cross-register groups, show username [Site] per member
            if (!crGroupName.empty())
                grp.sitesStr += bot.username + " [" + bot.siteName + "]";
            else
                grp.sitesStr += bot.siteName;
            grp.combinedSize += bot.totalBytes + bot.recordingStats.bytesWritten;
            if (bot.recording)
            {
                grp.anyRecording = true;
                grp.maxSpeed = std::max(grp.maxSpeed, bot.recordingStats.currentSpeed);
                if (bot.recordingStats.recordingWidth > grp.maxResW)
                {
                    grp.maxResW = bot.recordingStats.recordingWidth;
                    grp.maxResH = bot.recordingStats.recordingHeight;
                }
            }
            if (bot.running)
                grp.anyRunning = true;
            if (bot.mobile)
                grp.anyMobile = true;
            if (statusPrio(bot.status) > statusPrio(grp.bestStatus))
                grp.bestStatus = bot.status;
            if (bot.running && bot.startTime.time_since_epoch().count() > 0)
            {
                if (grp.earliestStart.time_since_epoch().count() == 0 ||
                    bot.startTime < grp.earliestStart)
                    grp.earliestStart = bot.startTime;
            }
            if (bot.recording && !cachedStates_[grp.primaryIdx].recording)
                grp.primaryIdx = i;
            if (grp.primaryUrl.empty() && !bot.websiteUrl.empty())
                grp.primaryUrl = bot.websiteUrl;
        }

        // ── Overlay ModelGroupState onto cross-register rows ──
        // Individual bots in a cross-register group are NOT started as
        // individual bots — the ModelGroup cycling engine manages them.
        // So we pull running/status/recording from the group state.
        {
            auto groupStates = manager_.getAllGroupStates();
            for (auto &[key, grp] : groupMap)
            {
                if (grp.groupName.empty())
                    continue;
                for (const auto &gs : groupStates)
                {
                    if (gs.groupName != grp.groupName)
                        continue;
                    grp.anyRunning = gs.running;
                    grp.anyRecording = gs.recording;
                    if (gs.running)
                    {
                        grp.bestStatus = gs.activeStatus;
                        grp.anyMobile = gs.activeMobile;
                        // Use best per-pairing status if group is cycling
                        for (const auto &ps : gs.pairings)
                        {
                            if (statusPrio(ps.lastStatus) > statusPrio(grp.bestStatus))
                                grp.bestStatus = ps.lastStatus;
                            if (ps.mobile)
                                grp.anyMobile = true;
                        }
                    }
                    break;
                }
            }
        }

        std::vector<GroupedRow> rows;
        rows.reserve(groupMap.size());
        for (auto &[_, grp] : groupMap)
            rows.push_back(std::move(grp));
        std::sort(rows.begin(), rows.end(),
                  [](const GroupedRow &a, const GroupedRow &b)
                  { return a.username < b.username; });

        std::string searchStr(searchBuf_);
        std::transform(searchStr.begin(), searchStr.end(), searchStr.begin(), ::tolower);

        // ── Filter rows ────────────────────────────────────────────
        std::vector<GroupedRow *> visibleRows;
        for (auto &grp : rows)
        {
            if (!searchStr.empty())
            {
                std::string lowerName = grp.key;
                std::string lowerSites = grp.sitesStr;
                std::transform(lowerSites.begin(), lowerSites.end(), lowerSites.begin(), ::tolower);
                std::string lowerGroup = grp.groupName;
                std::transform(lowerGroup.begin(), lowerGroup.end(), lowerGroup.begin(), ::tolower);
                if (lowerName.find(searchStr) == std::string::npos &&
                    lowerSites.find(searchStr) == std::string::npos &&
                    lowerGroup.find(searchStr) == std::string::npos)
                    continue;
            }
            if (filterStatus_ > 0)
            {
                bool match = false;
                for (int idx : grp.indices)
                {
                    const auto &b = cachedStates_[idx];
                    switch (filterStatus_)
                    {
                    case 1:
                        match |= (b.status == Status::Public);
                        break;
                    case 2:
                        match |= b.recording;
                        break;
                    case 3:
                        match |= (b.status == Status::Private);
                        break;
                    case 4:
                        match |= (b.status == Status::Offline || b.status == Status::LongOffline);
                        break;
                    case 5:
                        match |= (b.status == Status::Error);
                        break;
                    case 6:
                        match |= (b.status == Status::NotRunning);
                        break;
                    }
                }
                if (!match)
                    continue;
            }
            visibleRows.push_back(&grp);
        }

        // ── Bulk action bar (shown when anything is selected) ──────
        if (!selectedRows_.empty())
        {
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {10 * s, 5 * s});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.12f, 0.12f, 0.16f, 1.0f});
            ImGui::BeginChild("##BulkBar", {0, 38 * s}, ImGuiChildFlags_None);

            ImGui::TextColored(COL_ACCENT, "%d selected", (int)selectedRows_.size());
            ImGui::SameLine(0, 16 * s);

            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.15f, 0.40f, 0.15f, 1.0f});
            if (ImGui::Button("Start Selected"))
            {
                // Offload to background thread — each startBot locks mutex + writes config
                std::vector<std::string> groupNames;
                std::vector<std::pair<std::string, std::string>> bots;
                for (auto &grp : rows)
                {
                    if (!selectedRows_.count(grp.key))
                        continue;
                    if (!grp.groupName.empty())
                        groupNames.push_back(grp.groupName);
                    else
                        for (int idx : grp.indices)
                            bots.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                }
                safeDetach([this, groupNames = std::move(groupNames), bots = std::move(bots)]()
                           {
                    for (auto &gn : groupNames)
                        manager_.startGroup(gn);
                    for (auto &[u, s] : bots)
                        manager_.startBot(u, s); }, "start-selected");
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 4 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.50f, 0.15f, 0.15f, 1.0f});
            if (ImGui::Button("Stop Selected"))
            {
                // Offload to background thread — each stopBot locks mutex + writes config
                std::vector<std::string> groupNames;
                std::vector<std::pair<std::string, std::string>> bots;
                for (auto &grp : rows)
                {
                    if (!selectedRows_.count(grp.key))
                        continue;
                    if (!grp.groupName.empty())
                        groupNames.push_back(grp.groupName);
                    else
                        for (int idx : grp.indices)
                            bots.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                }
                safeDetach([this, groupNames = std::move(groupNames), bots = std::move(bots)]()
                           {
                    for (auto &gn : groupNames)
                        manager_.stopGroup(gn);
                    for (auto &[u, s] : bots)
                        manager_.stopBot(u, s); }, "stop-selected");
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 4 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.35f, 0.35f, 0.15f, 1.0f});
            if (ImGui::Button("Restart Selected"))
            {
                // Collect targets and offload to background thread to avoid freezing UI
                // (each stopBot acquires mutex + does disk I/O via configStore_.save)
                std::vector<std::string> groupNames;
                std::vector<std::pair<std::string, std::string>> bots;
                for (auto &grp : rows)
                {
                    if (!selectedRows_.count(grp.key))
                        continue;
                    if (!grp.groupName.empty())
                        groupNames.push_back(grp.groupName);
                    else
                        for (int idx : grp.indices)
                            bots.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                }
                safeDetach([this, groupNames = std::move(groupNames), bots = std::move(bots)]()
                           {
                    for (auto &gn : groupNames)
                    {
                        manager_.stopGroup(gn);
                        std::this_thread::sleep_for(std::chrono::milliseconds(500));
                        manager_.startGroup(gn);
                    }
                    for (auto &[user, site] : bots)
                        manager_.restartBot(user, site); }, "restart-selected");
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 4 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.10f, 0.45f, 0.55f, 1.0f});
            if (ImGui::Button("Resync Selected"))
            {
                // Force resync all selected bots (immediate status check)
                std::vector<std::pair<std::string, std::string>> targets;
                for (auto &grp : rows)
                {
                    if (!selectedRows_.count(grp.key))
                        continue;
                    for (int idx : grp.indices)
                        targets.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteSlug);
                }
                if (!targets.empty())
                {
                    safeDetach([this, targets = std::move(targets)]()
                               {
                        int ok = 0, fail = 0;
                        for (auto &[user, site] : targets)
                        {
                            auto result = manager_.resyncBot(user, site);
                            if (result.find("Resynced") != std::string::npos)
                                ++ok;
                            else
                                ++fail;
                        }
                        std::string msg = "Resync: " + std::to_string(ok) + " resynced";
                        if (fail > 0)
                            msg += ", " + std::to_string(fail) + " failed";
                        {
                            std::lock_guard lk(asyncResultMutex_);
                            moveResyncResult_ = std::move(msg);
                        }
                        moveResyncDone_.store(true); }, "resync-selected");
                }
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 12 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55f, 0.10f, 0.10f, 1.0f});
            if (ImGui::Button("Remove Selected"))
            {
                for (auto &grp : rows)
                    if (selectedRows_.count(grp.key))
                        for (int idx : grp.indices)
                            manager_.removeBot(cachedStates_[idx].username, cachedStates_[idx].siteName);
                selectedRows_.clear();
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 4 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.45f, 0.30f, 0.10f, 1.0f});
            if (ImGui::Button("Move to Unprocessed"))
            {
                // Collect selected bots and move their files on a background thread
                std::vector<std::pair<std::string, std::string>> targets;
                for (auto &grp : rows)
                {
                    if (!selectedRows_.count(grp.key))
                        continue;
                    for (int idx : grp.indices)
                        targets.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteSlug);
                }
                if (!targets.empty())
                {
                    safeDetach([this, targets = std::move(targets)]()
                               {
                        std::string summary;
                        int ok = 0, fail = 0;
                        for (auto &[user, site] : targets)
                        {
                            auto result = manager_.moveFilesToUnprocessed(user, site);
                            if (result.success)
                                ++ok;
                            else
                                ++fail;
                        }
                        std::string msg = "Bulk move: " + std::to_string(ok) + " moved";
                        if (fail > 0)
                            msg += ", " + std::to_string(fail) + " had no files";
                        {
                            std::lock_guard lk(asyncResultMutex_);
                            moveResyncResult_ = std::move(msg);
                        }
                        moveResyncDone_.store(true); }, "move-selected");
                }
            }
            ImGui::PopStyleColor();

            ImGui::SameLine(0, 12 * s);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.20f, 0.25f, 0.45f, 1.0f});
            if (ImGui::Button("Create Group"))
                ImGui::OpenPopup("CreateGroupFromSelection");
            ImGui::PopStyleColor();

            // Popup for group name input
            if (ImGui::BeginPopup("CreateGroupFromSelection"))
            {
                ImGui::Text("Group Name:");
                ImGui::SetNextItemWidth(200 * s);
                bool enter = ImGui::InputText("##NewGrpName", bulkGroupName_, sizeof(bulkGroupName_),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::SameLine();
                if ((ImGui::Button("OK") || enter) && std::strlen(bulkGroupName_) > 0)
                {
                    // Collect selected members
                    std::vector<std::pair<std::string, std::string>> members;
                    for (auto &grp : rows)
                    {
                        if (!selectedRows_.count(grp.key))
                            continue;
                        for (int idx : grp.indices)
                        {
                            const auto &b = cachedStates_[idx];
                            members.emplace_back(b.siteName, b.username);
                        }
                    }
                    manager_.createCrossRegisterGroup(bulkGroupName_, members);
                    std::memset(bulkGroupName_, 0, sizeof(bulkGroupName_));
                    selectedRows_.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::SameLine(0, 12 * s);
            if (ImGui::Button("Clear Selection"))
                selectedRows_.clear();

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        // ── Table ──────────────────────────────────────────────────
        ImGuiTableFlags tableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_BordersOuter | ImGuiTableFlags_BordersInnerV |
                                     ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp |
                                     ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate |
                                     ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

        if (!ImGui::BeginTable("##Models", 11, tableFlags))
            return;

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("##Chk", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide | ImGuiTableColumnFlags_NoReorder, 28.0f * s);
        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize | ImGuiTableColumnFlags_NoSort | ImGuiTableColumnFlags_NoHide, 110.0f * s);
        ImGui::TableSetupColumn("Username", ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_NoHide, 3.5f);
        ImGui::TableSetupColumn("Sites", 0, 2.0f);
        ImGui::TableSetupColumn("Group", 0, 2.0f);
        ImGui::TableSetupColumn("URL", 0, 4.0f);
        ImGui::TableSetupColumn("Recording", 0, 1.0f);
        ImGui::TableSetupColumn("Resolution", 0, 1.5f);
        ImGui::TableSetupColumn("Size", 0, 1.2f);
        ImGui::TableSetupColumn("Speed", 0, 1.0f);
        ImGui::TableSetupColumn("Uptime", 0, 1.0f);

        // Header row — with Select All checkbox
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
        // Checkbox header — select/deselect all
        ImGui::TableSetColumnIndex(0);
        {
            // Center checkbox using absolute position in column
            float colW = ImGui::GetColumnWidth();
            float cbW = ImGui::GetFrameHeight();
            float pad = (colW - cbW) * 0.5f;
            ImGui::SetCursorPosX(pad > 0.0f ? pad : 0.0f);

            bool allSelected = !visibleRows.empty();
            bool anySelected = false;
            for (auto *rp : visibleRows)
            {
                if (selectedRows_.count(rp->key))
                    anySelected = true;
                else
                    allSelected = false;
            }
            bool mixed = anySelected && !allSelected;
            if (mixed)
            {
                // Show as checked; clicking will deselect all
                bool tmp = true;
                if (ImGui::Checkbox("##SelectAll", &tmp))
                    selectedRows_.clear();
            }
            else
            {
                if (ImGui::Checkbox("##SelectAll", &allSelected))
                {
                    if (allSelected)
                        for (auto *rp : visibleRows)
                            selectedRows_.insert(rp->key);
                    else
                        selectedRows_.clear();
                }
            }
        }
        // Rest of headers
        for (int col = 1; col < 11; col++)
        {
            ImGui::TableSetColumnIndex(col);
            ImGui::TableHeader(ImGui::TableGetColumnName(col));
        }

        // ── Sort by column headers ─────────────────────────────────
        // NOTE: visibleRows is rebuilt from scratch every frame, so we
        // must always apply the sort (not just when SpecsDirty).
        if (ImGuiTableSortSpecs *sortSpecs = ImGui::TableGetSortSpecs())
        {
            if (sortSpecs->SpecsDirty)
                sortSpecs->SpecsDirty = false;

            if (sortSpecs->SpecsCount > 0)
            {
                const auto &spec = sortSpecs->Specs[0];
                bool asc = (spec.SortDirection == ImGuiSortDirection_Ascending);
                std::sort(visibleRows.begin(), visibleRows.end(),
                          [&](const GroupedRow *a, const GroupedRow *b) -> bool
                          {
                              int cmp = 0;
                              switch (spec.ColumnIndex)
                              {
                              case 2: // Username
                              {
                                  std::string la = a->username, lb = b->username;
                                  std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                                  std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                                  cmp = la.compare(lb);
                                  break;
                              }
                              case 3: // Sites
                                  cmp = a->sitesStr.compare(b->sitesStr);
                                  break;
                              case 4: // Group
                                  cmp = a->groupName.compare(b->groupName);
                                  break;
                              case 5: // URL
                                  cmp = a->primaryUrl.compare(b->primaryUrl);
                                  break;
                              case 6: // Recording
                                  cmp = (int)a->anyRecording - (int)b->anyRecording;
                                  break;
                              case 7: // Resolution
                              {
                                  int aRes = a->maxResW * a->maxResH;
                                  int bRes = b->maxResW * b->maxResH;
                                  cmp = (aRes < bRes) ? -1 : (aRes > bRes) ? 1
                                                                           : 0;
                                  break;
                              }
                              case 8: // Size
                                  cmp = (a->combinedSize < b->combinedSize) ? -1 : (a->combinedSize > b->combinedSize) ? 1
                                                                                                                       : 0;
                                  break;
                              case 9: // Speed
                                  cmp = (a->maxSpeed < b->maxSpeed) ? -1 : (a->maxSpeed > b->maxSpeed) ? 1
                                                                                                       : 0;
                                  break;
                              case 10: // Uptime
                                  cmp = (a->earliestStart < b->earliestStart) ? -1 : (a->earliestStart > b->earliestStart) ? 1
                                                                                                                           : 0;
                                  break;
                              default:
                                  break;
                              }
                              return asc ? (cmp < 0) : (cmp > 0);
                          });
            }
            else
            {
                // No sort active (user cleared sort) — default to alphabetical by username
                std::sort(visibleRows.begin(), visibleRows.end(),
                          [](const GroupedRow *a, const GroupedRow *b) -> bool
                          {
                              std::string la = a->username, lb = b->username;
                              std::transform(la.begin(), la.end(), la.begin(), ::tolower);
                              std::transform(lb.begin(), lb.end(), lb.begin(), ::tolower);
                              return la < lb;
                          });
            }
        }

        for (auto *rp : visibleRows)
        {
            auto &grp = *rp;
            bool isMultiSite = grp.indices.size() > 1;
            ImGui::TableNextRow();
            ImGui::PushID(grp.primaryIdx);

            // ── Checkbox ────────────────────────────────────────────
            ImGui::TableNextColumn();
            {
                // Center checkbox using absolute position in column
                float colW = ImGui::GetColumnWidth();
                float cbW = ImGui::GetFrameHeight(); // checkbox is square
                float pad = (colW - cbW) * 0.5f;
                ImGui::SetCursorPosX(pad > 0.0f ? pad : 0.0f);
                bool checked = selectedRows_.count(grp.key) > 0;
                if (ImGui::Checkbox("##sel", &checked))
                {
                    if (checked)
                        selectedRows_.insert(grp.key);
                    else
                        selectedRows_.erase(grp.key);
                }
            }

            // ── Status (readable text) ──────────────────────────────
            ImGui::TableNextColumn();
            {
                ImVec4 col = statusColor(grp.bestStatus);
                if (grp.anyRecording)
                {
                    float pulse = (std::sin(animTime_ * 4.0f) + 1.0f) * 0.5f;
                    col = {1.0f, 0.1f + pulse * 0.3f, 0.1f + pulse * 0.1f, 1.0f};
                }
                ImGui::TextColored(col, "%s", statusToString(grp.bestStatus));
                if (grp.anyMobile)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(COL_ORANGE, "(M)");
                }
            }

            // ── Username (TreeNode for multi-site, Selectable for single) ──
            ImGui::TableNextColumn();
            bool treeOpen = false;
            {
                bool selected = selectedRows_.count(grp.key) > 0;

                if (isMultiSite)
                {
                    ImGuiTreeNodeFlags nodeFlags = ImGuiTreeNodeFlags_DefaultOpen |
                                                   ImGuiTreeNodeFlags_SpanAvailWidth |
                                                   ImGuiTreeNodeFlags_OpenOnArrow;
                    if (selected)
                        nodeFlags |= ImGuiTreeNodeFlags_Selected;
                    treeOpen = ImGui::TreeNodeEx(grp.username.c_str(), nodeFlags);
                    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
                    {
                        selectedBot_ = grp.primaryIdx;
                        if (ImGui::IsMouseDoubleClicked(0))
                            showBotDetail_ = true;

                        // Ctrl+click: toggle this row in selection
                        // Plain click: select only this row
                        if (ImGui::GetIO().KeyCtrl)
                        {
                            if (selectedRows_.count(grp.key))
                                selectedRows_.erase(grp.key);
                            else
                                selectedRows_.insert(grp.key);
                        }
                        else
                        {
                            selectedRows_.clear();
                            selectedRows_.insert(grp.key);
                        }
                    }
                }
                else
                {
                    if (ImGui::Selectable(grp.username.c_str(), selected,
                                          ImGuiSelectableFlags_AllowDoubleClick |
                                              ImGuiSelectableFlags_SpanAllColumns))
                    {
                        selectedBot_ = grp.primaryIdx;
                        if (ImGui::IsMouseDoubleClicked(0))
                            showBotDetail_ = true;

                        // Ctrl+click: toggle this row in selection
                        // Plain click: select only this row
                        if (ImGui::GetIO().KeyCtrl)
                        {
                            if (selectedRows_.count(grp.key))
                                selectedRows_.erase(grp.key);
                            else
                                selectedRows_.insert(grp.key);
                        }
                        else
                        {
                            selectedRows_.clear();
                            selectedRows_.insert(grp.key);
                        }
                    }
                }
            }

            // ── Right-click context menu (with per-site actions) ────
            if (ImGui::BeginPopupContextItem("##BotCtx"))
            {
                selectedBot_ = grp.primaryIdx;

                ImGui::TextColored(COL_ACCENT, "%s", grp.username.c_str());
                ImGui::SameLine();
                ImGui::TextColored(COL_TEXT_DIM, "(%s)", grp.sitesStr.c_str());
                ImGui::Separator();

                // ── Start / Stop / Restart ──────────────────────────
                if (!grp.groupName.empty())
                {
                    // Cross-register group — use group start/stop (cycling engine)
                    if (grp.anyRunning)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                        if (ImGui::MenuItem("Stop Group"))
                            manager_.stopGroup(grp.groupName);
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_GREEN);
                        if (ImGui::MenuItem("Start Group"))
                            manager_.startGroup(grp.groupName);
                        ImGui::PopStyleColor();
                    }
                }
                else if (grp.indices.size() == 1)
                {
                    int idx = grp.indices[0];
                    const auto &b = cachedStates_[idx];
                    if (b.running)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                        if (ImGui::MenuItem("Stop"))
                        {
                            std::string u = b.username, s = b.siteName;
                            safeDetach([this, u, s]()
                                       { manager_.stopBot(u, s); }, "ctx-stop-bot");
                        }
                        ImGui::PopStyleColor();
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_YELLOW);
                        if (ImGui::MenuItem("Restart"))
                        {
                            std::string u = b.username, s = b.siteName;
                            safeDetach([this, u, s]()
                                       { manager_.restartBot(u, s); }, "ctx-restart-bot");
                        }
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_GREEN);
                        if (ImGui::MenuItem("Start"))
                            manager_.startBot(b.username, b.siteName);
                        ImGui::PopStyleColor();
                    }
                }
                else
                {
                    // Multi-site: per-site entries + All options
                    for (int idx : grp.indices)
                    {
                        const auto &b = cachedStates_[idx];
                        if (b.running)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                            if (ImGui::MenuItem(("Stop  [" + b.siteName + "]").c_str()))
                            {
                                std::string u = b.username, s = b.siteName;
                                safeDetach([this, u, s]()
                                           { manager_.stopBot(u, s); }, "ctx-stop-site");
                            }
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, COL_GREEN);
                            if (ImGui::MenuItem(("Start [" + b.siteName + "]").c_str()))
                                manager_.startBot(b.username, b.siteName);
                            ImGui::PopStyleColor();
                        }
                    }
                    ImGui::Separator();
                    if (grp.anyRunning)
                    {
                        if (ImGui::MenuItem("Stop All"))
                        {
                            std::vector<std::pair<std::string, std::string>> targets;
                            for (int idx : grp.indices)
                                targets.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                            safeDetach([this, targets = std::move(targets)]()
                                       {
                                for (auto& [u, s] : targets)
                                    manager_.stopBot(u, s); }, "ctx-stop-all");
                        }
                        if (ImGui::MenuItem("Restart All"))
                        {
                            std::vector<std::pair<std::string, std::string>> targets;
                            for (int idx : grp.indices)
                                targets.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                            safeDetach([this, targets = std::move(targets)]()
                                       {
                                for (auto& [u, s] : targets)
                                    manager_.restartBot(u, s); }, "ctx-restart-all");
                        }
                    }
                    else
                    {
                        if (ImGui::MenuItem("Start All"))
                        {
                            std::vector<std::pair<std::string, std::string>> targets;
                            for (int idx : grp.indices)
                                targets.emplace_back(cachedStates_[idx].username, cachedStates_[idx].siteName);
                            safeDetach([this, targets = std::move(targets)]()
                                       {
                                for (auto& [u, s] : targets)
                                    manager_.startBot(u, s); }, "ctx-start-all");
                        }
                    }
                }

                ImGui::Separator();

                if (ImGui::MenuItem("Open in Browser") && !grp.primaryUrl.empty())
                    openUrlInBrowser(grp.primaryUrl);

                if (ImGui::MenuItem("Copy URL") && !grp.primaryUrl.empty())
                    copyToClipboard(grp.primaryUrl);

                ImGui::Separator();

                if (ImGui::MenuItem("Open Downloads Folder"))
                {
                    auto &b = cachedStates_[grp.primaryIdx];
                    auto folder = config_.downloadsDir / (b.username + " [" + b.siteSlug + "]");
                    if (std::filesystem::exists(folder))
                        openFolderInExplorer(folder);
                    else
                        openFolderInExplorer(config_.downloadsDir);
                }

                if (ImGui::MenuItem("Details..."))
                    showBotDetail_ = true;

                ImGui::Separator();

                if (ImGui::MenuItem("Move to Unprocessed"))
                    pendingMoveBot_ = {cachedStates_[grp.primaryIdx].username,
                                       cachedStates_[grp.primaryIdx].siteSlug};

                if (ImGui::MenuItem("Resync Status"))
                    pendingResyncBot_ = {cachedStates_[grp.primaryIdx].username,
                                         cachedStates_[grp.primaryIdx].siteSlug};

                ImGui::Separator();

                // ── Cross-Register Group ────────────────────────────
                if (!grp.groupName.empty())
                {
                    if (ImGui::MenuItem("Edit Group..."))
                    {
                        focusCrossRegisterGroup_ = grp.groupName;
                        showCrossRegister_ = true;
                    }
                    if (ImGui::MenuItem("Break from Group"))
                    {
                        // Stop the group cycling first, then disband the entire group
                        std::string gname = grp.groupName;
                        safeDetach([this, gname]()
                                   {
                            manager_.stopGroup(gname);
                            manager_.removeCrossRegisterGroup(gname); }, "break-group");
                    }
                }
                else if (grp.indices.size() > 1)
                {
                    // Auto-grouped row (same username, multiple sites) — offer
                    // to create a cross-register group from the non-VR members.
                    int nonVrCount = 0;
                    for (int idx : grp.indices)
                    {
                        if (!isVrSlug(cachedStates_[idx].siteSlug))
                            nonVrCount++;
                    }

                    if (nonVrCount >= 2)
                    {
                        if (ImGui::MenuItem("Create Cross-Register Group"))
                        {
                            // Collect non-VR members
                            std::vector<std::pair<std::string, std::string>> members;
                            for (int idx : grp.indices)
                            {
                                const auto &b = cachedStates_[idx];
                                if (!isVrSlug(b.siteSlug))
                                    members.emplace_back(b.siteSlug, b.username);
                            }
                            // Use the username as the group name
                            std::string gname = grp.username;
                            // Stop individual bots first (group will cycle them)
                            for (auto &[site, user] : members)
                                manager_.stopBot(user, site);
                            manager_.createCrossRegisterGroup(gname, members);
                            // Auto-start the new group
                            manager_.startGroup(gname);
                        }
                    }

                    if (ImGui::MenuItem("Add to Group..."))
                    {
                        // Open add-to-group popup with bot context
                        addToGroupBotUser_ = grp.username;
                        addToGroupBotSite_ = cachedStates_[grp.primaryIdx].siteName;
                        // Pre-select existing group that matches this username
                        addToGroupSelectedIdx_ = -1;
                        std::memset(addToGroupNewName_, 0, sizeof(addToGroupNewName_));
                        auto existingGroups = manager_.getCrossRegisterGroups();
                        for (int gi2 = 0; gi2 < (int)existingGroups.size(); gi2++)
                        {
                            // Match by group name == username (case-insensitive)
                            std::string gnLower = existingGroups[gi2].groupName;
                            std::string unLower = grp.username;
                            std::transform(gnLower.begin(), gnLower.end(), gnLower.begin(), ::tolower);
                            std::transform(unLower.begin(), unLower.end(), unLower.begin(), ::tolower);
                            if (gnLower == unLower)
                            {
                                addToGroupSelectedIdx_ = gi2;
                                break;
                            }
                        }
                        showAddToGroupPopup_ = true;
                    }
                }
                else
                {
                    if (ImGui::MenuItem("Add to Group..."))
                    {
                        // Open add-to-group popup with bot context
                        addToGroupBotUser_ = cachedStates_[grp.primaryIdx].username;
                        addToGroupBotSite_ = cachedStates_[grp.primaryIdx].siteName;
                        addToGroupSelectedIdx_ = -1;
                        std::memset(addToGroupNewName_, 0, sizeof(addToGroupNewName_));
                        // Try to pre-select an existing group that matches this username
                        auto existingGroups = manager_.getCrossRegisterGroups();
                        for (int gi2 = 0; gi2 < (int)existingGroups.size(); gi2++)
                        {
                            std::string gnLower = existingGroups[gi2].groupName;
                            std::string unLower = grp.username;
                            std::transform(gnLower.begin(), gnLower.end(), gnLower.begin(), ::tolower);
                            std::transform(unLower.begin(), unLower.end(), unLower.begin(), ::tolower);
                            if (gnLower == unLower)
                            {
                                addToGroupSelectedIdx_ = gi2;
                                break;
                            }
                            // Also check if username appears in any group member
                            for (const auto &[msite, muser] : existingGroups[gi2].members)
                            {
                                std::string muLower = muser;
                                std::transform(muLower.begin(), muLower.end(), muLower.begin(), ::tolower);
                                if (muLower == unLower)
                                {
                                    addToGroupSelectedIdx_ = gi2;
                                    break;
                                }
                            }
                            if (addToGroupSelectedIdx_ >= 0)
                                break;
                        }
                        showAddToGroupPopup_ = true;
                    }
                }

                ImGui::Separator();

                // ── Edit model entry ────────────────────────────────
                if (grp.indices.size() == 1)
                {
                    if (ImGui::MenuItem("Edit..."))
                    {
                        const auto &b = cachedStates_[grp.indices[0]];
                        editModelOrigUser_ = b.username;
                        editModelOrigSite_ = b.siteName;
                        std::strncpy(editModelUsername_, b.username.c_str(), sizeof(editModelUsername_) - 1);
                        editModelUsername_[sizeof(editModelUsername_) - 1] = '\0';
                        // Find site index
                        auto allSites = manager_.availableSites();
                        editModelSiteIdx_ = 0;
                        for (int si = 0; si < (int)allSites.size(); si++)
                        {
                            if (allSites[si] == b.siteName)
                            {
                                editModelSiteIdx_ = si;
                                break;
                            }
                        }
                        showEditModel_ = true;
                    }
                }
                else
                {
                    if (ImGui::BeginMenu("Edit..."))
                    {
                        for (int idx : grp.indices)
                        {
                            const auto &b = cachedStates_[idx];
                            if (ImGui::MenuItem((b.username + " [" + b.siteName + "]").c_str()))
                            {
                                editModelOrigUser_ = b.username;
                                editModelOrigSite_ = b.siteName;
                                std::strncpy(editModelUsername_, b.username.c_str(), sizeof(editModelUsername_) - 1);
                                editModelUsername_[sizeof(editModelUsername_) - 1] = '\0';
                                auto allSites = manager_.availableSites();
                                editModelSiteIdx_ = 0;
                                for (int si = 0; si < (int)allSites.size(); si++)
                                {
                                    if (allSites[si] == b.siteName)
                                    {
                                        editModelSiteIdx_ = si;
                                        break;
                                    }
                                }
                                showEditModel_ = true;
                            }
                        }
                        ImGui::EndMenu();
                    }
                }

                ImGui::Separator();

                // ── Remove (per-site or direct) ────────────────────
                if (grp.indices.size() == 1)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                    if (ImGui::MenuItem("Remove"))
                        manager_.removeBot(cachedStates_[grp.indices[0]].username,
                                           cachedStates_[grp.indices[0]].siteName);
                    ImGui::PopStyleColor();
                }
                else
                {
                    if (ImGui::BeginMenu("Remove..."))
                    {
                        for (int idx : grp.indices)
                        {
                            const auto &b = cachedStates_[idx];
                            if (ImGui::MenuItem((b.siteName + " (" + b.siteSlug + ")").c_str()))
                                manager_.removeBot(b.username, b.siteName);
                        }
                        ImGui::Separator();
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                        if (ImGui::MenuItem("Remove All"))
                            for (int idx : grp.indices)
                                manager_.removeBot(cachedStates_[idx].username, cachedStates_[idx].siteName);
                        ImGui::PopStyleColor();
                        ImGui::EndMenu();
                    }
                }

                ImGui::EndPopup();
            }

            // ── Sites ───────────────────────────────────────────────
            ImGui::TableNextColumn();
            ImGui::TextColored(COL_ACCENT, "%s", grp.sitesStr.c_str());

            // ── Group (cross-register group name) ───────────────────
            ImGui::TableNextColumn();
            if (!grp.groupName.empty())
                ImGui::TextColored(COL_YELLOW, "%s", grp.groupName.c_str());
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── URL (model website — clickable, right-click to copy) ──
            ImGui::TableNextColumn();
            if (!grp.primaryUrl.empty())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT_DIM);
                // Truncate display to fit column, but full URL in tooltip
                ImGui::TextUnformatted(grp.primaryUrl.c_str());
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("%s\nClick = open  |  Right-click = copy", grp.primaryUrl.c_str());
                }
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                    openUrlInBrowser(grp.primaryUrl);
                if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                {
                    copyToClipboard(grp.primaryUrl);
                    addLog("info", "system", "Copied URL: " + grp.primaryUrl);
                }
            }
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── Recording ───────────────────────────────────────────
            ImGui::TableNextColumn();
            if (grp.anyRecording)
            {
                float pulse = (std::sin(animTime_ * 3.0f) + 1.0f) * 0.5f;
                ImVec4 recCol = {1.0f, 0.2f + pulse * 0.2f, 0.2f, 1.0f};
                ImGui::TextColored(recCol, "REC");
            }
            else
            {
                ImGui::TextColored(COL_TEXT_DIM, "--");
            }

            // ── Resolution ──────────────────────────────────────────
            ImGui::TableNextColumn();
            if (grp.maxResW > 0 && grp.maxResH > 0)
                ImGui::Text("%dx%d", grp.maxResW, grp.maxResH);
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── Size ────────────────────────────────────────────────
            ImGui::TableNextColumn();
            if (grp.combinedSize > 0)
                ImGui::Text("%s", formatBytesHuman(grp.combinedSize).c_str());
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── Speed ───────────────────────────────────────────────
            ImGui::TableNextColumn();
            if (grp.anyRecording && grp.maxSpeed > 0)
                ImGui::Text("%.1fx", grp.maxSpeed);
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── Uptime ──────────────────────────────────────────────
            ImGui::TableNextColumn();
            if (grp.anyRunning && grp.earliestStart.time_since_epoch().count() > 0)
                ImGui::Text("%s", formatTimeAgo(grp.earliestStart).c_str());
            else
                ImGui::TextColored(COL_TEXT_DIM, "--");

            // ── Expandable sub-rows for multi-site groups ───────────
            if (isMultiSite && treeOpen)
            {
                for (int idx : grp.indices)
                {
                    const auto &b = cachedStates_[idx];
                    ImGui::TableNextRow();
                    ImGui::PushID(idx + 100000);

                    // Checkbox for sub-row
                    ImGui::TableNextColumn();
                    {
                        // Center checkbox using absolute position (ignore tree indent)
                        float colW = ImGui::GetColumnWidth();
                        float cbW = ImGui::GetFrameHeight();
                        float colStart = ImGui::GetCursorScreenPos().x - ImGui::GetCursorPosX();
                        float pad = (colW - cbW) * 0.5f;
                        ImGui::SetCursorPosX(pad > 0.0f ? pad : 0.0f);
                        std::string subKey = b.username + "\t" + b.siteName;
                        std::transform(subKey.begin(), subKey.end(), subKey.begin(), ::tolower);
                        bool subChecked = selectedRows_.count(subKey) > 0;
                        if (ImGui::Checkbox("##subsel", &subChecked))
                        {
                            if (subChecked)
                                selectedRows_.insert(subKey);
                            else
                                selectedRows_.erase(subKey);
                        }
                    }

                    // Status (readable text)
                    ImGui::TableNextColumn();
                    {
                        ImVec4 col = statusColor(b.status);
                        if (b.recording)
                        {
                            float pulse = (std::sin(animTime_ * 4.0f) + 1.0f) * 0.5f;
                            col = {1.0f, 0.1f + pulse * 0.3f, 0.1f + pulse * 0.1f, 1.0f};
                        }
                        ImGui::TextColored(col, "%s", statusToString(b.status));
                        if (b.mobile)
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(COL_ORANGE, "(M)");
                        }
                    }

                    // Username (indented, shows site tag)
                    ImGui::TableNextColumn();
                    {
                        ImGui::TreePush("sub");
                        std::string subKey2 = b.username + "\t" + b.siteName;
                        std::transform(subKey2.begin(), subKey2.end(), subKey2.begin(), ::tolower);
                        bool subSel = selectedRows_.count(subKey2) > 0;
                        std::string label = b.username + "  [" + b.siteSlug + "]";
                        if (ImGui::Selectable(label.c_str(), subSel,
                                              ImGuiSelectableFlags_AllowDoubleClick |
                                                  ImGuiSelectableFlags_SpanAllColumns))
                        {
                            selectedBot_ = idx;
                            if (ImGui::IsMouseDoubleClicked(0))
                                showBotDetail_ = true;

                            // Ctrl+click: toggle in selection. Plain click: select only this.
                            if (ImGui::GetIO().KeyCtrl)
                            {
                                if (selectedRows_.count(subKey2))
                                    selectedRows_.erase(subKey2);
                                else
                                    selectedRows_.insert(subKey2);
                            }
                            else
                            {
                                selectedRows_.clear();
                                selectedRows_.insert(subKey2);
                            }
                        }
                        ImGui::TreePop();
                    }

                    // Context menu for sub-row
                    if (ImGui::BeginPopupContextItem(("ctx_sub_" + b.username + "_" + b.siteName).c_str()))
                    {
                        if (b.running)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                            if (ImGui::MenuItem("Stop"))
                            {
                                std::string u = b.username, s = b.siteName;
                                safeDetach([this, u, s]()
                                           { manager_.stopBot(u, s); }, "sub-ctx-stop");
                            }
                            ImGui::PopStyleColor();
                            ImGui::PushStyleColor(ImGuiCol_Text, COL_YELLOW);
                            if (ImGui::MenuItem("Restart"))
                            {
                                std::string u = b.username, s = b.siteName;
                                safeDetach([this, u, s]()
                                           { manager_.restartBot(u, s); }, "sub-ctx-restart");
                            }
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, COL_GREEN);
                            if (ImGui::MenuItem("Start"))
                                manager_.startBot(b.username, b.siteName);
                            ImGui::PopStyleColor();
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Details..."))
                        {
                            selectedBot_ = idx;
                            showBotDetail_ = true;
                        }
                        if (ImGui::MenuItem("Edit..."))
                        {
                            editModelOrigUser_ = b.username;
                            editModelOrigSite_ = b.siteName;
                            std::strncpy(editModelUsername_, b.username.c_str(), sizeof(editModelUsername_) - 1);
                            editModelUsername_[sizeof(editModelUsername_) - 1] = '\0';
                            auto allSites = manager_.availableSites();
                            editModelSiteIdx_ = 0;
                            for (int si = 0; si < (int)allSites.size(); si++)
                                if (allSites[si] == b.siteName)
                                {
                                    editModelSiteIdx_ = si;
                                    break;
                                }
                            showEditModel_ = true;
                        }
                        ImGui::Separator();
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                        if (ImGui::MenuItem("Remove"))
                            manager_.removeBot(b.username, b.siteName);
                        ImGui::PopStyleColor();
                        ImGui::EndPopup();
                    }

                    // Site
                    ImGui::TableNextColumn();
                    ImGui::TextColored(COL_ACCENT, "%s", b.siteName.c_str());

                    // Group
                    ImGui::TableNextColumn();
                    ImGui::TextColored(COL_TEXT_DIM, "--");

                    // URL
                    ImGui::TableNextColumn();
                    if (!b.websiteUrl.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT_DIM);
                        ImGui::TextUnformatted(b.websiteUrl.c_str());
                        ImGui::PopStyleColor();
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                            ImGui::SetTooltip("%s", b.websiteUrl.c_str());
                        }
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
                            openUrlInBrowser(b.websiteUrl);
                    }
                    else
                        ImGui::TextColored(COL_TEXT_DIM, "--");

                    // Recording
                    ImGui::TableNextColumn();
                    if (b.recording)
                    {
                        float pulse = (std::sin(animTime_ * 3.0f) + 1.0f) * 0.5f;
                        ImVec4 recCol = {1.0f, 0.2f + pulse * 0.2f, 0.2f, 1.0f};
                        ImGui::TextColored(recCol, "REC");
                    }
                    else
                        ImGui::TextColored(COL_TEXT_DIM, "--");

                    // Resolution
                    ImGui::TableNextColumn();
                    if (b.recordingStats.recordingWidth > 0 && b.recordingStats.recordingHeight > 0)
                        ImGui::Text("%dx%d", b.recordingStats.recordingWidth, b.recordingStats.recordingHeight);
                    else
                        ImGui::TextColored(COL_TEXT_DIM, "--");

                    // Size (completed + in-progress, same as parent row)
                    ImGui::TableNextColumn();
                    {
                        uint64_t subSize = b.totalBytes + b.recordingStats.bytesWritten;
                        if (subSize > 0)
                            ImGui::Text("%s", formatBytesHuman(subSize).c_str());
                        else
                            ImGui::TextColored(COL_TEXT_DIM, "--");
                    }

                    // Speed
                    ImGui::TableNextColumn();
                    if (b.recording && b.recordingStats.currentSpeed > 0)
                        ImGui::Text("%.1fx", b.recordingStats.currentSpeed);
                    else
                        ImGui::TextColored(COL_TEXT_DIM, "--");

                    // Uptime
                    ImGui::TableNextColumn();
                    if (b.running && b.startTime.time_since_epoch().count() > 0)
                        ImGui::Text("%s", formatTimeAgo(b.startTime).c_str());
                    else
                        ImGui::TextColored(COL_TEXT_DIM, "--");

                    ImGui::PopID();
                }
                ImGui::TreePop(); // close the parent TreeNode
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    // ─────────────────────────────────────────────────────────────────
    // Log panel
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderLogPanel()
    {
        // Drain spdlog sink into our log entries each frame
        if (logSink_)
        {
            auto entries = logSink_->drain();
            if (!entries.empty())
            {
                std::lock_guard lock(logMutex_);
                for (auto &e : entries)
                {
                    logEntries_.push_back({std::move(e.message), std::move(e.level),
                                           std::move(e.source), e.time});
                    if (logEntries_.size() > kMaxLogEntries)
                        logEntries_.pop_front();
                }
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12 * dpiScale_, 6 * dpiScale_});

        // Header: label + level filter toggles + right-aligned controls
        ImGui::Text("Log");
        ImGui::SameLine();
        ImGui::Spacing();
        ImGui::SameLine();

        // Level filter toggle buttons (colored)
        auto levelToggle = [&](const char *label, bool &flag, ImVec4 onCol)
        {
            ImGui::SameLine();
            if (flag)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, onCol);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {onCol.x * 1.2f, onCol.y * 1.2f, onCol.z * 1.2f, 1.0f});
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.20f, 0.20f, 0.22f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.30f, 0.30f, 0.33f, 1.0f});
            }
            if (ImGui::SmallButton(label))
                flag = !flag;
            ImGui::PopStyleColor(2);
        };
        levelToggle("Debug", logShowDebug_, {0.40f, 0.40f, 0.45f, 1.0f});
        levelToggle("Info", logShowInfo_, {0.15f, 0.55f, 0.25f, 1.0f});
        levelToggle("Warn", logShowWarn_, {0.65f, 0.55f, 0.10f, 1.0f});
        levelToggle("Error", logShowError_, {0.65f, 0.15f, 0.15f, 1.0f});

        ImGui::SameLine();
        ImGui::SetNextItemWidth(120 * dpiScale_);
        ImGui::InputTextWithHint("##LogSource", "Source filter", logSourceFilter_, sizeof(logSourceFilter_));

        // Right-aligned: auto-scroll, copy, clear
        float clearBtnWidth = ImGui::CalcTextSize("Clear").x + ImGui::GetStyle().FramePadding.x * 4;
        float copyBtnWidth = ImGui::CalcTextSize("Copy Logs").x + ImGui::GetStyle().FramePadding.x * 4;
        float checkboxWidth = ImGui::CalcTextSize("Auto-scroll").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetFrameHeight();
        float spacing = ImGui::GetStyle().ItemSpacing.x;
        float rightEdge = ImGui::GetContentRegionAvail().x;
        float controlsStartX = ImGui::GetCursorPosX() + rightEdge - clearBtnWidth - copyBtnWidth - checkboxWidth - spacing * 2;

        ImGui::SameLine(controlsStartX);
        ImGui::Checkbox("Auto-scroll", &autoScroll_);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy Logs"))
        {
            std::lock_guard lock(logMutex_);
            std::string allLogs;
            allLogs.reserve(logEntries_.size() * 80);
            for (const auto &entry : logEntries_)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                   entry.time.time_since_epoch())
                                   .count();
                int hours = (int)(elapsed / 3600) % 24;
                int mins = (int)(elapsed / 60) % 60;
                int secs = (int)elapsed % 60;
                char timeBuf[16];
                snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d]", hours, mins, secs);
                allLogs += timeBuf;
                allLogs += " [" + entry.source + "] " + entry.message + "\n";
            }
            copyToClipboard(allLogs);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
        {
            std::lock_guard lock(logMutex_);
            logEntries_.clear();
        }

        ImGui::Separator();

        // Log content (filtered)
        if (ImGui::BeginChild("##LogContent", {0, 0}, ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar))
        {
            std::lock_guard lock(logMutex_);

            // Check if we're at the bottom BEFORE rendering new content
            float scrollY = ImGui::GetScrollY();
            float scrollMax = ImGui::GetScrollMaxY();
            bool wasAtBottom = (scrollMax <= 0.0f) || (scrollY >= scrollMax - 4.0f);

            // Build filtered index list
            std::string srcFilter(logSourceFilter_);
            std::transform(srcFilter.begin(), srcFilter.end(), srcFilter.begin(), ::tolower);

            std::vector<int> filtered;
            filtered.reserve(logEntries_.size());
            for (int i = 0; i < (int)logEntries_.size(); i++)
            {
                const auto &entry = logEntries_[i];
                // Level filter
                if (entry.level == "debug" && !logShowDebug_)
                    continue;
                if (entry.level == "info" && !logShowInfo_)
                    continue;
                if (entry.level == "warn" && !logShowWarn_)
                    continue;
                if (entry.level == "error" && !logShowError_)
                    continue;
                // Source filter
                if (!srcFilter.empty())
                {
                    std::string src = entry.source;
                    std::transform(src.begin(), src.end(), src.begin(), ::tolower);
                    if (src.find(srcFilter) == std::string::npos)
                    {
                        std::string msg = entry.message;
                        std::transform(msg.begin(), msg.end(), msg.begin(), ::tolower);
                        if (msg.find(srcFilter) == std::string::npos)
                            continue;
                    }
                }
                filtered.push_back(i);
            }

            ImGuiListClipper clipper;
            clipper.Begin((int)filtered.size());

            while (clipper.Step())
            {
                for (int fi = clipper.DisplayStart; fi < clipper.DisplayEnd; fi++)
                {
                    const auto &entry = logEntries_[filtered[fi]];

                    // Level color
                    ImVec4 col = COL_TEXT;
                    if (entry.level == "error")
                        col = COL_RED;
                    else if (entry.level == "warn")
                        col = COL_YELLOW;
                    else if (entry.level == "debug")
                        col = COL_TEXT_DIM;
                    else if (entry.level == "info")
                        col = COL_GREEN;

                    // Timestamp
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                       entry.time.time_since_epoch())
                                       .count();
                    int hours = (int)(elapsed / 3600) % 24;
                    int mins = (int)(elapsed / 60) % 60;
                    int secs = (int)elapsed % 60;

                    ImGui::PushID(filtered[fi]);
                    ImGui::TextColored(COL_TEXT_DIM, "[%02d:%02d:%02d]", hours, mins, secs);
                    ImGui::SameLine();
                    ImGui::TextColored(COL_ACCENT_DIM, "[%s]", entry.source.c_str());
                    ImGui::SameLine();
                    ImGui::TextColored(col, "%s", entry.message.c_str());

                    // Right-click to copy individual log entry (especially useful for errors)
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                    {
                        char timeBuf[16];
                        snprintf(timeBuf, sizeof(timeBuf), "[%02d:%02d:%02d]", hours, mins, secs);
                        std::string full = std::string(timeBuf) + " [" + entry.source + "] " + entry.message;
                        copyToClipboard(full);
                    }
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Right-click to copy this line");
                    ImGui::PopID();
                }
            }

            // Auto-scroll: if enabled and we were at the bottom, stay at bottom
            if (autoScroll_ && wasAtBottom)
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }

    // ─────────────────────────────────────────────────────────────────
    // Status bar
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderStatusBar()
    {
        ImGuiViewport *viewport = ImGui::GetMainViewport();
        float statusH = 28.0f * dpiScale_;
        ImGui::SetNextWindowPos({viewport->WorkPos.x,
                                 viewport->WorkPos.y + viewport->WorkSize.y - statusH});
        ImGui::SetNextWindowSize({viewport->WorkSize.x, statusH});

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                 ImGuiWindowFlags_NoDocking;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {10, 4});
        ImGui::PushStyleColor(ImGuiCol_WindowBg, {0.06f, 0.06f, 0.08f, 1.0f});
        ImGui::Begin("##StatusBar", nullptr, flags);

        // Left: disk usage summary
        ImGui::TextColored(COL_TEXT_DIM, "Disk: %s free | Downloads: %s (%d files)",
                           formatBytesHuman(cachedDiskUsage_.freeBytes).c_str(),
                           formatBytesHuman(cachedDiskUsage_.downloadDirBytes).c_str(),
                           cachedDiskUsage_.fileCount);

        // Log toggle button — shows error count when hidden
        ImGui::SameLine();
        {
            int errorCount = 0;
            {
                std::lock_guard lock(logMutex_);
                for (const auto &e : logEntries_)
                    if (e.level == "error")
                        errorCount++;
            }
            if (!showLogPanel_ && errorCount > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.5f, 0.15f, 0.15f, 1.0f});
                std::string lbl = "Logs (" + std::to_string(errorCount) + " errors)";
                if (ImGui::SmallButton(lbl.c_str()))
                    showLogPanel_ = true;
                ImGui::PopStyleColor();
            }
            else
            {
                if (ImGui::SmallButton(showLogPanel_ ? "Hide Logs" : "Show Logs"))
                    showLogPanel_ = !showLogPanel_;
            }
        }

        // Center: local web dashboard URL (clickable to copy)
        {
            const std::string &localUrl = cachedLanUrl_;
            std::string label = "Web: " + localUrl;
            float labelW = ImGui::CalcTextSize(label.c_str()).x;
            float centerX = (ImGui::GetWindowWidth() - labelW) * 0.5f;
            // Ensure it doesn't overlap with disk info or version
            ImGui::SameLine(centerX);
            ImGui::PushStyleColor(ImGuiCol_Text, COL_ACCENT);
            if (ImGui::SmallButton(label.c_str()))
            {
                copyToClipboard(localUrl);
                addLog("info", "system", "Copied web URL: " + localUrl);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Click to copy web dashboard URL");
        }

        // Right: version
        float versionWidth = ImGui::CalcTextSize("StreaMonitor v2.0 C++").x;
        ImGui::SameLine(ImGui::GetWindowWidth() - versionWidth - 20);
        ImGui::TextColored(COL_TEXT_DIM, "StreaMonitor v2.0 C++");

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
    }

    // ─────────────────────────────────────────────────────────────────
    // Settings window
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderSettingsWindow()
    {
        ImGui::SetNextWindowSize({600 * dpiScale_, 500 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({400 * dpiScale_, 300 * dpiScale_}, {900 * dpiScale_, 800 * dpiScale_});
        if (!ImGui::Begin("Settings", &showSettings_))
        {
            ImGui::End();
            return;
        }

        // Tab bar for settings categories (with right-click to hide/show)
        if (ImGui::BeginTabBar("##SettingsTabs"))
        {
            // Right-click on tab bar background for visibility menu
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
                ImGui::OpenPopup("##TabVisibility");
            if (ImGui::BeginPopup("##TabVisibility"))
            {
                ImGui::TextColored(COL_ACCENT, "Show/Hide Tabs");
                ImGui::Separator();
                ImGui::MenuItem("General", nullptr, &showTabGeneral_);
                ImGui::MenuItem("Recording", nullptr, &showTabRecording_);
                ImGui::MenuItem("FFmpeg", nullptr, &showTabFFmpeg_);
                ImGui::MenuItem("Network", nullptr, &showTabNetwork_);
                ImGui::MenuItem("Web Server", nullptr, &showTabWebServer_);
                ImGui::MenuItem("Sites", nullptr, &showTabSites_);
                ImGui::MenuItem("Advanced", nullptr, &showTabAdvanced_);
                ImGui::EndPopup();
            }

            if (showTabGeneral_ && ImGui::BeginTabItem("General"))
            {
                renderSettingsGeneral();
                ImGui::EndTabItem();
            }
            if (showTabRecording_ && ImGui::BeginTabItem("Recording"))
            {
                renderSettingsRecording();
                ImGui::EndTabItem();
            }
            if (showTabFFmpeg_ && ImGui::BeginTabItem("FFmpeg"))
            {
                renderSettingsFFmpeg();
                ImGui::EndTabItem();
            }
            if (showTabNetwork_ && ImGui::BeginTabItem("Network"))
            {
                renderSettingsNetwork();
                ImGui::EndTabItem();
            }
            if (showTabWebServer_ && ImGui::BeginTabItem("Web Server"))
            {
                renderSettingsWeb();
                ImGui::EndTabItem();
            }
            if (showTabSites_ && ImGui::BeginTabItem("Sites"))
            {
                renderSettingsSites();
                ImGui::EndTabItem();
            }
            if (showTabAdvanced_ && ImGui::BeginTabItem("Advanced"))
            {
                renderSettingsAdvanced();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::Spacing();

        // Apply / Cancel buttons
        if (ImGui::Button("Apply & Save", {140, 0}))
        {
            // Apply settings to config
            config_.downloadsDir = editDownloadDir_;
            config_.wantedResolution = editResolution_;
            config_.webPort = editPort_;
            config_.ffmpegPath = editFfmpegPath_;
            config_.filenameFormat = editFilenameFormat_;

            // Stripchat spy private
            config_.spyPrivateEnabled = editSpyPrivateEnabled_;
            config_.stripchatCookies = editStripchatCookies_;

            switch (editContainerFmt_)
            {
            case 0:
                config_.container = ContainerFormat::MKV;
                break;
            case 1:
                config_.container = ContainerFormat::MP4;
                break;
            case 2:
                config_.container = ContainerFormat::TS;
                break;
            }

            // Proxy settings
            config_.proxyEnabled = editProxyEnabled_;

            // Map proxy type index to ProxyType enum
            const ProxyType types[] = {ProxyType::HTTP, ProxyType::HTTPS, ProxyType::SOCKS4,
                                       ProxyType::SOCKS4A, ProxyType::SOCKS5, ProxyType::SOCKS5H};
            ProxyType selectedType = (editProxyType_ >= 0 && editProxyType_ < 6)
                                         ? types[editProxyType_]
                                         : ProxyType::None;

            // Update or create proxy entry
            std::string proxyUrlStr(editProxyUrl_);
            if (!proxyUrlStr.empty())
            {
                if (config_.proxies.empty())
                {
                    // Create new proxy entry
                    ProxyEntry entry;
                    entry.url = proxyUrlStr;
                    entry.type = selectedType;
                    entry.enabled = true;
                    entry.rolling = false;
                    config_.proxies.push_back(entry);
                }
                else
                {
                    // Update first proxy entry
                    config_.proxies[0].url = proxyUrlStr;
                    config_.proxies[0].type = selectedType;
                }
            }

            // Encoding config
            const EncoderType encoderTypes[] = {EncoderType::Copy, EncoderType::X265, EncoderType::X264,
                                                EncoderType::NVENC_HEVC, EncoderType::NVENC_H264};
            config_.encoding.encoder = (editEncoderType_ >= 0 && editEncoderType_ < 5)
                                           ? encoderTypes[editEncoderType_]
                                           : EncoderType::X265;
            config_.encoding.enableCuda = editEnableCuda_;
            config_.encoding.crf = editCrf_;
            const char *presets[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                     "medium", "slow", "slower", "veryslow", "placebo"};
            config_.encoding.preset = (editPresetIdx_ >= 0 && editPresetIdx_ < 10)
                                          ? presets[editPresetIdx_]
                                          : "medium";
            config_.encoding.audioBitrate = editAudioBitrate_;
            config_.encoding.copyAudio = editCopyAudio_;
            config_.encoding.maxWidth = editMaxWidth_;
            config_.encoding.maxHeight = editMaxHeight_;
            config_.encoding.threads = editEncoderThreads_;

            // FFmpeg tuning settings
            config_.ffmpeg.liveLastSegments = editFfmpegLiveLastSegments_;
            config_.ffmpeg.rwTimeoutSec = editFfmpegRwTimeoutSec_;
            config_.ffmpeg.socketTimeoutSec = editFfmpegSocketTimeoutSec_;
            config_.ffmpeg.reconnectDelayMax = editFfmpegReconnectDelayMax_;
            config_.ffmpeg.maxRestarts = editFfmpegMaxRestarts_;
            config_.ffmpeg.gracefulQuitTimeoutSec = editFfmpegGracefulQuitTimeoutSec_;
            config_.ffmpeg.startupGraceSec = editFfmpegStartupGraceSec_;
            config_.ffmpeg.suspectStallSec = editFfmpegSuspectStallSec_;
            config_.ffmpeg.stallSameTimeSec = editFfmpegStallSameTimeSec_;
            config_.ffmpeg.speedLowThreshold = editFfmpegSpeedLowThreshold_;
            config_.ffmpeg.speedLowSustainSec = editFfmpegSpeedLowSustainSec_;
            config_.ffmpeg.maxSingleLagSec = editFfmpegMaxSingleLagSec_;
            config_.ffmpeg.maxConsecSkipLines = editFfmpegMaxConsecSkipLines_;
            config_.ffmpeg.fallbackNoStderrSec = editFfmpegFallbackNoStderrSec_;
            config_.ffmpeg.fallbackNoOutputSec = editFfmpegFallbackNoOutputSec_;
            config_.ffmpeg.cooldownAfterStalls = editFfmpegCooldownAfterStalls_;
            config_.ffmpeg.cooldownSleepSec = editFfmpegCooldownSleepSec_;
            config_.ffmpeg.playlistProbeIntervalSec = editFfmpegPlaylistProbeIntervalSec_;
            config_.ffmpeg.probeSize = editFfmpegProbeSize_;
            config_.ffmpeg.analyzeDuration = editFfmpegAnalyzeDuration_;

            // System tray & auto-start
            config_.minimizeToTray = editMinimizeToTray_;
            config_.autoStartOnLogin = editAutoStart_;
            config_.enablePreviewCapture = editEnablePreviewCapture_;
#ifdef _WIN32
            SystemTray::setAutoStart(editAutoStart_);
#endif

            // Web server settings — detect if restart is needed
            {
                bool needRestart = false;
                bool wasEnabled = config_.webEnabled;
                std::string oldHost = config_.webHost;
                int oldPort = config_.webPort;

                config_.webEnabled = editWebEnabled_;
                config_.webHost = editWebHost_;
                config_.webPort = editWebPort_;
                config_.webUsername = editWebUsername_;

                // Update password only if new password field is non-empty
                if (std::strlen(editWebNewPassword_) > 0)
                {
                    config_.webPassword = editWebNewPassword_;
                    std::strncpy(editWebPassword_, editWebNewPassword_, sizeof(editWebPassword_) - 1);
                    std::memset(editWebNewPassword_, 0, sizeof(editWebNewPassword_));
                    addLog("info", "system", "Web credentials updated");
                }

                if (oldHost != config_.webHost || oldPort != config_.webPort)
                    needRestart = true;

                if (webServer_)
                {
                    if (!editWebEnabled_ && webServer_->isRunning())
                    {
                        webServer_->stop();
                        addLog("info", "system", "Web server stopped");
                    }
                    else if (editWebEnabled_ && !webServer_->isRunning())
                    {
                        webServer_->start();
                        addLog("info", "system", "Web server started on " + config_.webHost + ":" + std::to_string(config_.webPort));
                    }
                    else if (editWebEnabled_ && needRestart && webServer_->isRunning())
                    {
                        webServer_->stop();
                        webServer_->start();
                        addLog("info", "system", "Web server restarted on " + config_.webHost + ":" + std::to_string(config_.webPort));
                    }
                }
                // Update cached LAN URL
                cachedLanUrl_ = "http://" + detectLanIP() + ":" + std::to_string(config_.webPort);
            }

            // Auto-save to disk
            config_.saveToFile("app_config.json");
            manager_.saveConfig();
            editDirtyFlag_ = false;
            addLog("info", "system", "Settings applied & saved to disk");
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", {100, 0}))
            showSettings_ = false;

        ImGui::End();
    }

    void GuiApp::renderSettingsGeneral()
    {
        ImGui::Spacing();
        ImGui::Text("Download Directory");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##DownloadDir", editDownloadDir_, sizeof(editDownloadDir_)))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "Environment: STRMNTR_DOWNLOAD_DIR");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Container Format");
        const char *formats[] = {"MKV (Matroska)", "MP4 (MPEG-4)", "TS (MPEG-TS)"};
        if (ImGui::Combo("##ContainerFmt", &editContainerFmt_, formats, IM_ARRAYSIZE(formats)))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "MKV is recommended for reliability (default)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Web Interface Port");
        if (ImGui::InputInt("##Port", &editPort_))
        {
            editPort_ = std::clamp(editPort_, 1024, 65535);
            editDirtyFlag_ = true;
        }
        ImGui::TextColored(COL_TEXT_DIM, "Environment: STRMNTR_HTTP_PORT (default: 5000)");
    }

    void GuiApp::renderSettingsRecording()
    {
        ImGui::Spacing();
        ImGui::Text("Target Resolution");
        if (ImGui::InputInt("##Resolution", &editResolution_))
        {
            editResolution_ = std::clamp(editResolution_, 144, 99999);
            editDirtyFlag_ = true;
        }
        ImGui::TextColored(COL_TEXT_DIM, "99999 = highest available (default)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Filename Format");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##FilenameFormat", editFilenameFormat_, sizeof(editFilenameFormat_)))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "Tokens: {n} = sequential number, {model} = username");
        ImGui::TextColored(COL_TEXT_DIM, "{site} = site slug, {date} = YYYYMMDD, {time} = HHMMSS");
        ImGui::TextColored(COL_TEXT_DIM, "{datetime} = YYYYMMDD_HHMMSS   (default: {n})");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Sleep Timers (seconds)");
        static int sleepOnline = 5, sleepOffline = 15, sleepPrivate = 5,
                   sleepError = 30, sleepRateLimit = 120;
        ImGui::SliderInt("After Online Check", &sleepOnline, 1, 60);
        ImGui::SliderInt("After Offline", &sleepOffline, 5, 120);
        ImGui::SliderInt("After Private", &sleepPrivate, 5, 60);
        ImGui::SliderInt("After Error", &sleepError, 10, 300);
        ImGui::SliderInt("After Rate Limit", &sleepRateLimit, 30, 600);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Error Handling");
        static int maxErrors = 20;
        ImGui::SliderInt("Max Consecutive Errors", &maxErrors, 5, 500);
        ImGui::TextColored(COL_TEXT_DIM, "Bot stops after this many consecutive errors");
    }

    void GuiApp::renderSettingsFFmpeg()
    {
        ImGui::Spacing();
        ImGui::Text("FFmpeg Binary Path");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##FfmpegPath", editFfmpegPath_, sizeof(editFfmpegPath_)))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "Leave empty to use system FFmpeg (libav* API)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Encoding Settings ──────────────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Encoding Settings");
        ImGui::Spacing();

        ImGui::Text("Video Encoder");
        const char *encoderNames[] = {"Copy (No Re-encode)", "x265 (HEVC Software)", "x264 (H.264 Software)",
                                      "NVENC HEVC (NVIDIA GPU)", "NVENC H.264 (NVIDIA GPU)"};
        if (ImGui::Combo("##Encoder", &editEncoderType_, encoderNames, IM_ARRAYSIZE(encoderNames)))
            editDirtyFlag_ = true;

        // Show encoding options only if not stream copy
        if (editEncoderType_ != 0)
        {
            ImGui::Spacing();
            ImGui::Text("Quality (CRF)");
            if (ImGui::SliderInt("##CRF", &editCrf_, 0, 51, "%d"))
                editDirtyFlag_ = true;
            ImGui::TextColored(COL_TEXT_DIM, "Lower = better quality, larger file (18-28 typical)");

            ImGui::Spacing();
            ImGui::Text("Encoding Preset");
            const char *presetNames[] = {"ultrafast", "superfast", "veryfast", "faster", "fast",
                                         "medium", "slow", "slower", "veryslow", "placebo"};
            if (ImGui::Combo("##Preset", &editPresetIdx_, presetNames, IM_ARRAYSIZE(presetNames)))
                editDirtyFlag_ = true;
            ImGui::TextColored(COL_TEXT_DIM, "Slower = better compression, longer encode time");

            ImGui::Spacing();
            ImGui::Text("Encoder Threads");
            if (ImGui::SliderInt("##Threads", &editEncoderThreads_, 0, 32, editEncoderThreads_ == 0 ? "Auto" : "%d"))
                editDirtyFlag_ = true;

            ImGui::Spacing();
            if (ImGui::Checkbox("Enable CUDA Acceleration", &editEnableCuda_))
                editDirtyFlag_ = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Audio Settings ──────────────────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Audio Settings");
        ImGui::Spacing();

        if (ImGui::Checkbox("Copy Audio (No Re-encode)", &editCopyAudio_))
            editDirtyFlag_ = true;

        if (!editCopyAudio_)
        {
            ImGui::Spacing();
            ImGui::Text("Audio Bitrate (kbps)");
            if (ImGui::SliderInt("##AudioBitrate", &editAudioBitrate_, 64, 320, "%d kbps"))
                editDirtyFlag_ = true;
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Resolution Limits ──────────────────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Output Resolution Limits");
        ImGui::Spacing();

        ImGui::Text("Max Width");
        if (ImGui::InputInt("##MaxWidth", &editMaxWidth_))
        {
            editMaxWidth_ = std::max(0, editMaxWidth_);
            editDirtyFlag_ = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "0 = keep original");

        ImGui::Text("Max Height");
        if (ImGui::InputInt("##MaxHeight", &editMaxHeight_))
        {
            editMaxHeight_ = std::max(0, editMaxHeight_);
            editDirtyFlag_ = true;
        }
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "0 = keep original");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── FFmpeg Tuning (Advanced) ──────────────────────────────────
        if (ImGui::CollapsingHeader("Advanced FFmpeg Tuning", ImGuiTreeNodeFlags_None))
        {
            ImGui::Indent();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Probe & Analysis");
            ImGui::Text("Probe Size");
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("##ProbeSize", editFfmpegProbeSize_, sizeof(editFfmpegProbeSize_)))
                editDirtyFlag_ = true;
            ImGui::SameLine();
            ImGui::TextColored(COL_TEXT_DIM, "e.g. 4M, 8M");

            ImGui::Text("Analyze Duration");
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputText("##AnalyzeDuration", editFfmpegAnalyzeDuration_, sizeof(editFfmpegAnalyzeDuration_)))
                editDirtyFlag_ = true;
            ImGui::SameLine();
            ImGui::TextColored(COL_TEXT_DIM, "microseconds (10000000 = 10s)");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "HLS Settings");
            if (ImGui::SliderInt("Live Last Segments", &editFfmpegLiveLastSegments_, 1, 10))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Playlist Probe Interval (sec)", &editFfmpegPlaylistProbeIntervalSec_, 1, 30))
                editDirtyFlag_ = true;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Timeouts");
            if (ImGui::SliderInt("R/W Timeout (sec)", &editFfmpegRwTimeoutSec_, 1, 30))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Socket Timeout (sec)", &editFfmpegSocketTimeoutSec_, 1, 30))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Reconnect Delay Max (sec)", &editFfmpegReconnectDelayMax_, 1, 60))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Graceful Quit Timeout (sec)", &editFfmpegGracefulQuitTimeoutSec_, 1, 30))
                editDirtyFlag_ = true;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Stall Detection");
            if (ImGui::SliderInt("Startup Grace (sec)", &editFfmpegStartupGraceSec_, 1, 60))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Suspect Stall (sec)", &editFfmpegSuspectStallSec_, 5, 120))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Stall Same Time (sec)", &editFfmpegStallSameTimeSec_, 5, 120))
                editDirtyFlag_ = true;
            if (ImGui::SliderFloat("Speed Low Threshold", &editFfmpegSpeedLowThreshold_, 0.1f, 1.0f, "%.2f"))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Speed Low Sustain (sec)", &editFfmpegSpeedLowSustainSec_, 5, 120))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Max Single Lag (sec)", &editFfmpegMaxSingleLagSec_, 1, 60))
                editDirtyFlag_ = true;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Recovery & Restarts");
            if (ImGui::SliderInt("Max Restarts", &editFfmpegMaxRestarts_, 1, 50))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Max Consecutive Skip Lines", &editFfmpegMaxConsecSkipLines_, 1, 20))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Fallback No Stderr (sec)", &editFfmpegFallbackNoStderrSec_, 10, 120))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Fallback No Output (sec)", &editFfmpegFallbackNoOutputSec_, 10, 120))
                editDirtyFlag_ = true;

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Cooldown");
            if (ImGui::SliderInt("Cooldown After Stalls", &editFfmpegCooldownAfterStalls_, 1, 20))
                editDirtyFlag_ = true;
            if (ImGui::SliderInt("Cooldown Sleep (sec)", &editFfmpegCooldownSleepSec_, 5, 120))
                editDirtyFlag_ = true;

            ImGui::Unindent();
        }
    }

    void GuiApp::renderSettingsNetwork()
    {
        ImGui::Spacing();
        ImGui::Text("HTTP Client Settings");

        static int requestTimeout = 30;
        ImGui::SliderInt("Request Timeout (sec)", &requestTimeout, 5, 120);

        static int maxRetries = 3;
        ImGui::SliderInt("Max Retries", &maxRetries, 0, 10);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Rate Limiting");
        static float rateLimit = 10.0f;
        ImGui::SliderFloat("Requests/second", &rateLimit, 1.0f, 50.0f, "%.1f");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("User Agent");
        static char userAgent[512] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64)";
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##UserAgent", userAgent, sizeof(userAgent));

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Proxy ──────────────────────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Proxy Settings");
        ImGui::Spacing();

        if (ImGui::Checkbox("Enable Proxy", &editProxyEnabled_))
            editDirtyFlag_ = true;

        if (editProxyEnabled_)
        {
            ImGui::Text("Proxy Type");
            const char *proxyTypes[] = {"HTTP", "HTTPS", "SOCKS4", "SOCKS4A", "SOCKS5", "SOCKS5H (remote DNS)"};
            if (ImGui::Combo("##ProxyType", &editProxyType_, proxyTypes, IM_ARRAYSIZE(proxyTypes)))
                editDirtyFlag_ = true;

            ImGui::Text("Proxy URL");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##ProxyUrl", editProxyUrl_, sizeof(editProxyUrl_)))
                editDirtyFlag_ = true;
            ImGui::TextColored(COL_TEXT_DIM, "Format: host:port  or  user:pass@host:port");
            ImGui::TextColored(COL_TEXT_DIM, "Environment: STRMNTR_PROXY (e.g. socks5://127.0.0.1:9050)");
        }
    }

    void GuiApp::renderSettingsWeb()
    {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Web Dashboard Server");
        ImGui::Spacing();

        // Enable/disable toggle
        if (ImGui::Checkbox("Enable Web Server", &editWebEnabled_))
            editDirtyFlag_ = true;

        bool serverRunning = webServer_ && webServer_->isRunning();
        ImGui::SameLine();
        if (serverRunning)
            ImGui::TextColored(COL_GREEN, "(Running)");
        else
            ImGui::TextColored(COL_RED, "(Stopped)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Bind address & port
        ImGui::TextColored(COL_ACCENT, "Binding");
        ImGui::Spacing();

        ImGui::Text("Host / Bind Address");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputTextWithHint("##WebHost", "0.0.0.0 (all interfaces)", editWebHost_, sizeof(editWebHost_)))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "0.0.0.0 = listen on all interfaces (required for LAN access)");
        ImGui::TextColored(COL_TEXT_DIM, "127.0.0.1 = localhost only");

        ImGui::Spacing();
        ImGui::Text("Port");
        if (ImGui::InputInt("##WebPort", &editWebPort_))
        {
            if (editWebPort_ < 1)
                editWebPort_ = 1;
            if (editWebPort_ > 65535)
                editWebPort_ = 65535;
            editDirtyFlag_ = true;
        }

        // Show current URLs
        if (serverRunning)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(COL_ACCENT, "Access URLs");
            ImGui::Spacing();

            std::string localUrl = webServer_->getLocalUrl();
            std::string netUrl = webServer_->getNetworkUrl();

            ImGui::Text("Local:   ");
            ImGui::SameLine();
            ImGui::TextColored(COL_CYAN, "%s", localUrl.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open##local"))
                openUrlInBrowser(localUrl);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##local"))
                copyToClipboard(localUrl);

            ImGui::Text("Network: ");
            ImGui::SameLine();
            ImGui::TextColored(COL_CYAN, "%s", netUrl.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open##net"))
                openUrlInBrowser(netUrl);
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy##net"))
                copyToClipboard(netUrl);

            ImGui::TextColored(COL_TEXT_DIM, "Use the Network URL from any device (PC, phone, tablet)");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Credentials
        ImGui::TextColored(COL_ACCENT, "Login Credentials");
        ImGui::Spacing();

        ImGui::Text("Username");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##WebUser", editWebUsername_, sizeof(editWebUsername_)))
            editDirtyFlag_ = true;

        ImGui::Spacing();
        ImGui::Text("Current Password");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##WebPassCurrent", editWebPassword_, sizeof(editWebPassword_),
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::TextColored(COL_TEXT_DIM, "(Read-only — enter new password below to change)");

        ImGui::Spacing();
        ImGui::Text("New Password (leave empty to keep current)");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##WebPassNew", editWebNewPassword_, sizeof(editWebNewPassword_),
                             ImGuiInputTextFlags_Password))
            editDirtyFlag_ = true;

        ImGui::Spacing();
        ImGui::Spacing();

        // Quick start/stop buttons
        if (webServer_)
        {
            if (serverRunning)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Stop Web Server Now", {-1, 0}))
                {
                    webServer_->stop();
                    addLog("info", "system", "Web server stopped");
                }
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.55f, 0.25f, 1.0f));
                if (ImGui::Button("Start Web Server Now", {-1, 0}))
                {
                    config_.webHost = editWebHost_;
                    config_.webPort = editWebPort_;
                    if (webServer_->start())
                    {
                        addLog("info", "system", "Web server started on " + config_.webHost + ":" + std::to_string(config_.webPort));
                        cachedLanUrl_ = "http://" + detectLanIP() + ":" + std::to_string(config_.webPort);
                    }
                    else
                    {
                        addLog("error", "system", "Failed to start web server on " + config_.webHost + ":" + std::to_string(config_.webPort));
                    }
                }
                ImGui::PopStyleColor();
            }
        }
        else
        {
            ImGui::TextColored(COL_TEXT_DIM, "Web server not available (not initialized)");
        }

#ifdef _WIN32
        ImGui::Spacing();
        ImGui::TextColored(COL_TEXT_DIM, "Note: If LAN access doesn't work, check Windows Firewall.");
        ImGui::TextColored(COL_TEXT_DIM, "Allow this app through: Windows Security > Firewall > Allow an app");
#endif
    }

    void GuiApp::renderSettingsAdvanced()
    {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Preview Capture");
        ImGui::Spacing();

        if (ImGui::Checkbox("Enable Live Preview", &editEnablePreviewCapture_))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "Decode video frames for GUI/web preview thumbnails and live stream viewing.");
        ImGui::TextColored(COL_TEXT_DIM, "Disable to save CPU — recording is unaffected.");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Logging");
        static int logLevel = 1; // 0=debug, 1=info, 2=warn, 3=error
        const char *levels[] = {"Debug", "Info", "Warning", "Error"};
        ImGui::Combo("Log Level", &logLevel, levels, IM_ARRAYSIZE(levels));

        static int maxLogEntries = 10000;
        ImGui::SliderInt("Max Log Entries", &maxLogEntries, 1000, 100000);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Disk Space Watchdog");
        static bool enableWatchdog = true;
        ImGui::Checkbox("Enable Disk Space Monitoring", &enableWatchdog);
        static int minFreeGb = 5;
        ImGui::SliderInt("Min Free Space (GB)", &minFreeGb, 1, 100);
        ImGui::TextColored(COL_TEXT_DIM, "Pause recordings when free space drops below threshold");

#ifdef _WIN32
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("System Tray");
        if (ImGui::Checkbox("Minimize to System Tray", &editMinimizeToTray_))
            editDirtyFlag_ = true;
        ImGui::TextColored(COL_TEXT_DIM, "When minimized, hide window to system tray instead of taskbar");

        if (ImGui::Checkbox("Start with Windows", &editAutoStart_))
            editDirtyFlag_ = true;
        if (SystemTray::isAutoStartEnabled())
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.0f), "Currently registered in Windows startup");
        else
            ImGui::TextColored(COL_TEXT_DIM, "Add to Windows startup (HKCU\\Run registry key)");
#endif
    }

    void GuiApp::renderSettingsSites()
    {
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Per-Site Configuration");
        ImGui::TextColored(COL_TEXT_DIM,
                           "Enable/disable specific sites and configure site-specific settings");
        ImGui::Spacing();

        auto sites = manager_.availableSites();
        for (size_t i = 0; i < sites.size(); i++)
        {
            ImGui::PushID((int)i);
            bool enabled = true; // TODO: persist per-site enable
            ImGui::Checkbox(sites[i].c_str(), &enabled);
            ImGui::PopID();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Stripchat Mouflon Keys ──────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Stripchat — Mouflon Decryption Keys");
        ImGui::TextColored(COL_TEXT_DIM,
                           "Keys used for decrypting Stripchat HLS playlists (pkey → pdkey)");
        ImGui::Spacing();

        auto &mouflon = MouflonKeys::instance();
        auto keys = mouflon.getKeys();

        if (keys.empty())
        {
            ImGui::TextColored(COL_YELLOW, "No mouflon keys loaded. Keys are auto-extracted from Doppio JS at startup.");
        }
        else
        {
            // Show keys in a table
            if (ImGui::BeginTable("##MouflonKeys", 3,
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            {
                ImGui::TableSetupColumn("Public Key (pkey)", 0, 2.0f);
                ImGui::TableSetupColumn("Decryption Key (pdkey)", 0, 2.0f);
                ImGui::TableSetupColumn("##Actions", ImGuiTableColumnFlags_WidthFixed, 60.0f * dpiScale_);
                ImGui::TableHeadersRow();

                std::string keyToRemove;
                int row = 0;
                for (const auto &[pkey, pdkey] : keys)
                {
                    ImGui::PushID(row++);
                    ImGui::TableNextRow();

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(pkey.c_str());
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        copyToClipboard(pkey);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Right-click to copy");

                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(pdkey.c_str());
                    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        copyToClipboard(pdkey);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Right-click to copy");

                    ImGui::TableNextColumn();
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.50f, 0.12f, 0.12f, 1.0f});
                    if (ImGui::SmallButton("Remove"))
                        keyToRemove = pkey;
                    ImGui::PopStyleColor();

                    ImGui::PopID();
                }
                ImGui::EndTable();

                // Deferred removal (can't modify while iterating)
                if (!keyToRemove.empty())
                {
                    mouflon.removeKey(keyToRemove);
                    addLog("info", "system", "Removed mouflon key: " + keyToRemove);
                }
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(COL_TEXT_DIM, "Add a new key pair:");

        static char newPkey[128] = {};
        static char newPdkey[128] = {};
        ImGui::SetNextItemWidth(200 * dpiScale_);
        ImGui::InputTextWithHint("##NewPkey", "pkey", newPkey, sizeof(newPkey));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(200 * dpiScale_);
        ImGui::InputTextWithHint("##NewPdkey", "pdkey", newPdkey, sizeof(newPdkey));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.15f, 0.45f, 0.15f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.20f, 0.55f, 0.20f, 1.0f});
        if (ImGui::Button("Add Key") && std::strlen(newPkey) > 0 && std::strlen(newPdkey) > 0)
        {
            mouflon.addKey(newPkey, newPdkey);
            addLog("info", "system",
                   "Added mouflon key: " + std::string(newPkey) + " → " + std::string(newPdkey));
            std::memset(newPkey, 0, sizeof(newPkey));
            std::memset(newPdkey, 0, sizeof(newPdkey));
        }
        ImGui::PopStyleColor(2);

        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.10f, 0.45f, 0.55f, 1.0f});
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4{0.15f, 0.55f, 0.65f, 1.0f});
        if (ImGui::Button("Re-extract from Doppio JS"))
        {
            addLog("info", "system", "Re-extracting mouflon keys...");
            safeDetach([this]()
                       {
                auto &mk = MouflonKeys::instance();
                HttpClient http;
                http.setDefaultUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36");
                mk.reinitialize(http);
                addLog("info", "system",
                       "Mouflon key re-extraction complete (" +
                           std::to_string(mk.getKeys().size()) + " keys)"); }, "mouflon-reextract");
        }
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::TextColored(COL_TEXT_DIM, "Fetch latest keys from Stripchat's Doppio JS");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // ── Stripchat Spy Private ──────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Stripchat — Spy on Private Shows");
        ImGui::TextColored(COL_TEXT_DIM,
                           "Attempt to spy-record when a model enters a private show.");
        ImGui::TextColored(COL_TEXT_DIM,
                           "Requires Stripchat account cookies and enough tokens.");
        ImGui::Spacing();

        if (ImGui::Checkbox("Enable Spy on Private", &editSpyPrivateEnabled_))
            editDirtyFlag_ = true;

        if (editSpyPrivateEnabled_)
        {
            ImGui::Spacing();
            ImGui::Text("Stripchat Cookies");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputTextMultiline("##StripchatCookies", editStripchatCookies_,
                                          sizeof(editStripchatCookies_),
                                          ImVec2(-1, 60 * dpiScale_)))
                editDirtyFlag_ = true;
            ImGui::TextColored(COL_TEXT_DIM,
                               "Paste your browser cookies (e.g. session_id=abc; auth_token=xyz)");
            ImGui::TextColored(COL_TEXT_DIM,
                               "Get them from browser DevTools → Application → Cookies → stripchat.com");
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(COL_TEXT_DIM, "Note: Disabling a site will stop all bots on that site");
    }

    // ─────────────────────────────────────────────────────────────────
    // Add model dialog
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderAddModelDialog()
    {
        ImGui::SetNextWindowSize({500 * dpiScale_, 340 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({350 * dpiScale_, 250 * dpiScale_}, {700 * dpiScale_, 500 * dpiScale_});
        if (!ImGui::Begin("Add Model", &showAddModel_))
        {
            ImGui::End();
            return;
        }

        auto sites = manager_.availableSites();

        // ── URL Paste shortcut ──────────────────────────────────────
        ImGui::TextColored(COL_ACCENT, "Paste Site URL (auto-detect):");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##AddUrl", addUrlInput_, sizeof(addUrlInput_),
                             ImGuiInputTextFlags_EnterReturnsTrue))
        {
            std::string parsedSite, parsedUser;
            if (parseSiteUrl(addUrlInput_, parsedSite, parsedUser))
            {
                // Strip spaces from username
                parsedUser = stripWhitespace(parsedUser);
                std::strncpy(addUsername_, parsedUser.c_str(), sizeof(addUsername_) - 1);
                addUsername_[sizeof(addUsername_) - 1] = '\0';
                // Find site index
                for (int i = 0; i < (int)sites.size(); i++)
                {
                    if (sites[i] == parsedSite)
                    {
                        addSiteIdx_ = i;
                        break;
                    }
                }
                // Auto-add — only the detected site (no automatic counterpart)
                manager_.addBot(parsedUser, parsedSite, true);
                addLog("info", "system", "Added from URL: " + parsedUser + " on " + parsedSite);
                std::memset(addUrlInput_, 0, sizeof(addUrlInput_));
                std::memset(addUsername_, 0, sizeof(addUsername_));
                addAlsoCounterpart_ = false;
                showAddModel_ = false;
            }
            else
            {
                addLog("warn", "system", "Could not parse URL: " + std::string(addUrlInput_));
            }
        }
        ImGui::TextColored(COL_TEXT_DIM, "e.g. https://stripchat.com/username or https://chaturbate.com/name");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(COL_ACCENT, "Or enter manually:");
        ImGui::Spacing();

        ImGui::Text("Username:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##AddUser", addUsername_, sizeof(addUsername_));

        ImGui::Spacing();
        ImGui::Text("Site:");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##AddSite",
                              addSiteIdx_ < (int)sites.size() ? sites[addSiteIdx_].c_str() : ""))
        {
            // Search filter at top of combo
            ImGui::SetNextItemWidth(-1);
            if (ImGui::IsWindowAppearing())
            {
                ImGui::SetKeyboardFocusHere();
                std::memset(siteFilterBuf_, 0, sizeof(siteFilterBuf_));
            }
            ImGui::InputTextWithHint("##SiteFilter", "Type to filter...", siteFilterBuf_, sizeof(siteFilterBuf_));
            ImGui::Separator();

            std::string filter(siteFilterBuf_);
            std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

            for (int i = 0; i < (int)sites.size(); i++)
            {
                if (!filter.empty())
                {
                    std::string lower = sites[i];
                    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                    if (lower.find(filter) == std::string::npos)
                        continue;
                }
                bool selected = (addSiteIdx_ == i);
                if (ImGui::Selectable(sites[i].c_str(), selected))
                {
                    addSiteIdx_ = i;
                    addAlsoCounterpart_ = false;
                }
            }
            ImGui::EndCombo();
        }

        // StripChat <-> StripChatVR cross-add checkbox
        if (addSiteIdx_ < (int)sites.size())
        {
            std::string chosen = sites[addSiteIdx_];
            if (chosen == "StripChat")
            {
                ImGui::Spacing();
                ImGui::Checkbox("Also add StripChatVR", &addAlsoCounterpart_);
            }
            else if (chosen == "StripChatVR")
            {
                ImGui::Spacing();
                ImGui::Checkbox("Also add StripChat", &addAlsoCounterpart_);
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool canAdd = std::strlen(addUsername_) > 0 && addSiteIdx_ < (int)sites.size();
        if (!canAdd)
            ImGui::BeginDisabled();
        if (ImGui::Button("Add & Start", {-1, 0}))
        {
            std::string chosenSite = sites[addSiteIdx_];
            // Strip spaces from username (sites don't use spaces in usernames)
            std::string cleanUsername = stripWhitespace(addUsername_);
            if (cleanUsername.empty())
            {
                ImGui::End();
                return;
            }
            std::vector<std::string> sitesToAdd;
            sitesToAdd.push_back(chosenSite);

            // Add counterpart if checkbox was checked
            if (addAlsoCounterpart_)
            {
                if (chosenSite == "StripChat")
                    sitesToAdd.push_back("StripChatVR");
                else if (chosenSite == "StripChatVR")
                    sitesToAdd.push_back("StripChat");
            }

            for (const auto &site : sitesToAdd)
            {
                manager_.addBot(cleanUsername, site, true);
                addLog("info", "system", std::string("Added & saved: ") + cleanUsername + " on " + site);
            }
            std::memset(addUsername_, 0, sizeof(addUsername_));
            std::memset(addUrlInput_, 0, sizeof(addUrlInput_));
            addAlsoCounterpart_ = false;
            showAddModel_ = false;
        }
        if (!canAdd)
            ImGui::EndDisabled();

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // About window
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderAboutWindow()
    {
        ImGui::SetNextWindowSize({400 * dpiScale_, 300 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({300 * dpiScale_, 200 * dpiScale_}, {800 * dpiScale_, 600 * dpiScale_});
        if (!ImGui::Begin("About StreaMonitor", &showAbout_))
        {
            ImGui::End();
            return;
        }

        ImGui::TextColored(COL_ACCENT, "StreaMonitor v2.0");
        ImGui::Text("C++ Rewrite with Native FFmpeg & ImGui");
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Features:");
        ImGui::BulletText("18 site plugins (all Python sites ported)");
        ImGui::BulletText("Native FFmpeg API - no subprocess spawning");
        ImGui::BulletText("ImGui GUI with dark theme");
        ImGui::BulletText("Thread-per-model architecture (std::jthread)");
        ImGui::BulletText("Cross-register multi-site model tracking");
        ImGui::BulletText("Live disk usage monitoring");
        ImGui::BulletText("VR stream support (StripChatVR, DreamCamVR)");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextColored(COL_TEXT_DIM, "Built with C++20, CMake, vcpkg");
        ImGui::TextColored(COL_TEXT_DIM, "Libraries: libcurl, nlohmann/json, spdlog, ImGui, GLFW");

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // FFmpeg monitor
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderFFmpegMonitor()
    {
        ImGui::SetNextWindowSize({500 * dpiScale_, 400 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({350 * dpiScale_, 250 * dpiScale_}, {900 * dpiScale_, 700 * dpiScale_});
        if (!ImGui::Begin("FFmpeg Monitor", &showFFmpegMon_))
        {
            ImGui::End();
            return;
        }

        int activeRecordings = 0;
        uint64_t totalBytesWritten = 0;
        uint32_t totalSegments = 0;
        uint32_t totalStalls = 0;

        for (const auto &bot : cachedStates_)
        {
            if (bot.recording)
            {
                activeRecordings++;
                totalBytesWritten += bot.recordingStats.bytesWritten;
                totalSegments += bot.recordingStats.segmentsRecorded;
                totalStalls += bot.recordingStats.stallsDetected;
            }
        }

        ImGui::Text("Active Recordings: %d", activeRecordings);
        ImGui::Text("Total Data Written: %s", formatBytesHuman(totalBytesWritten).c_str());
        ImGui::Text("Total Segments: %u", totalSegments);
        ImGui::Text("Total Stalls Detected: %u", totalStalls);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Per-recording details
        if (ImGui::BeginTable("##RecDetails", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV))
        {
            ImGui::TableSetupColumn("Model");
            ImGui::TableSetupColumn("File");
            ImGui::TableSetupColumn("Size");
            ImGui::TableSetupColumn("Segments");
            ImGui::TableSetupColumn("Speed");
            ImGui::TableHeadersRow();

            for (const auto &bot : cachedStates_)
            {
                if (!bot.recording)
                    continue;

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s [%s]", bot.username.c_str(), bot.siteSlug.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(COL_TEXT_DIM, "%s", bot.recordingStats.currentFile.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%s", formatBytesHuman(bot.recordingStats.bytesWritten).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%u", bot.recordingStats.segmentsRecorded);
                ImGui::TableNextColumn();
                ImGui::Text("%.1fx", bot.recordingStats.currentSpeed);
            }

            ImGui::EndTable();
        }

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Disk usage panel
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderDiskUsagePanel()
    {
        ImGui::SetNextWindowSize({400 * dpiScale_, 250 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({300 * dpiScale_, 180 * dpiScale_}, {700 * dpiScale_, 500 * dpiScale_});
        if (!ImGui::Begin("Disk Usage", &showDiskUsage_))
        {
            ImGui::End();
            return;
        }

        auto &du = cachedDiskUsage_;

        // Disk space bar
        float usedRatio = (du.totalBytes > 0)
                              ? 1.0f - (float)du.freeBytes / (float)du.totalBytes
                              : 0.0f;

        ImGui::Text("Disk Space");
        ImGui::ProgressBar(usedRatio, {-1, 20},
                           (std::to_string((int)(usedRatio * 100)) + "% used").c_str());

        ImGui::Spacing();
        ImGui::Text("Total:       %s", formatBytesHuman(du.totalBytes).c_str());
        ImGui::Text("Free:        %s", formatBytesHuman(du.freeBytes).c_str());
        ImGui::Text("Used:        %s", formatBytesHuman(du.totalBytes - du.freeBytes).c_str());

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Downloads Directory");
        ImGui::Text("Size:        %s", formatBytesHuman(du.downloadDirBytes).c_str());
        ImGui::Text("Files:       %d", du.fileCount);
        ImGui::Text("Path:        %s", config_.downloadsDir.string().c_str());

        // Warning if low on space
        if (du.freeBytes > 0 && du.freeBytes < 5ULL * 1024 * 1024 * 1024)
        {
            ImGui::Spacing();
            ImGui::TextColored(COL_RED, "WARNING: Low disk space! Consider freeing up space.");
        }

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Cross-register window
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderCrossRegisterWindow()
    {
        ImGui::SetNextWindowSize({600 * dpiScale_, 450 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({400 * dpiScale_, 300 * dpiScale_}, {900 * dpiScale_, 700 * dpiScale_});
        if (!ImGui::Begin("Cross-Register Groups", &showCrossRegister_))
        {
            ImGui::End();
            return;
        }

        ImGui::TextColored(COL_ACCENT, "Cross-Register Groups — Cycling Engine");
        ImGui::TextColored(COL_TEXT_DIM,
                           "Same person, different sites/usernames. One thread cycles through");
        ImGui::TextColored(COL_TEXT_DIM,
                           "all pairings without delay — sleeps only when ALL are offline.");
        ImGui::Spacing();

        // Create new group
        ImGui::Text("New Group:");
        {
            float createBtnW = ImGui::CalcTextSize(" Create ").x + ImGui::GetStyle().FramePadding.x * 2;
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - createBtnW - ImGui::GetStyle().ItemSpacing.x);
        }
        ImGui::InputText("##GroupName", crossGroupName_, sizeof(crossGroupName_));
        ImGui::SameLine();
        if (ImGui::Button("Create") && std::strlen(crossGroupName_) > 0)
        {
            manager_.createCrossRegisterGroup(crossGroupName_, {});
            std::memset(crossGroupName_, 0, sizeof(crossGroupName_));
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // List existing groups with cycling status
        auto groups = manager_.getCrossRegisterGroups();
        auto groupStates = manager_.getAllGroupStates();
        for (size_t gi = 0; gi < groups.size(); gi++)
        {
            auto &group = groups[gi];
            ImGui::PushID((int)gi);

            // Find matching group state
            const ModelGroupState *gs = nullptr;
            for (const auto &s : groupStates)
            {
                if (s.groupName == group.groupName)
                {
                    gs = &s;
                    break;
                }
            }

            // Header with status indicator
            std::string headerLabel = group.groupName;
            if (gs && gs->running)
            {
                if (gs->recording)
                    headerLabel += "  [REC: " + gs->activeUsername + " on " + gs->activeSite + "]";
                else if (gs->activePairingIdx >= 0)
                    headerLabel += "  [Checking: " + gs->activeUsername + " on " + gs->activeSite + "]";
                else
                    headerLabel += "  [All Offline — Sleeping]";
            }
            else
            {
                headerLabel += "  [Stopped]";
            }

            if (ImGui::CollapsingHeader(headerLabel.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            {
                // Auto-scroll to focused group from right-click "Edit Group"
                if (!focusCrossRegisterGroup_.empty() && group.groupName == focusCrossRegisterGroup_)
                {
                    ImGui::SetScrollHereY(0.0f);
                    focusCrossRegisterGroup_.clear();
                }
                // Start/Stop buttons for the group
                if (gs && gs->running)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.50f, 0.15f, 0.15f, 1.0f});
                    if (ImGui::SmallButton("Stop Group"))
                        manager_.stopGroup(group.groupName);
                    ImGui::PopStyleColor();
                }
                else
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.40f, 0.15f, 1.0f});
                    if (ImGui::SmallButton("Start Group"))
                        manager_.startGroup(group.groupName);
                    ImGui::PopStyleColor();
                }
                ImGui::SameLine();
                ImGui::TextColored(COL_TEXT_DIM, "(%zu pairings)", group.members.size());

                // Pairing list with per-pairing status
                for (size_t mi = 0; mi < group.members.size(); mi++)
                {
                    auto &[site, user] = group.members[mi];

                    // Show pairing status from group state
                    ImVec4 pairingCol = COL_TEXT_DIM;
                    const char *pairingStatus = "—";
                    bool pairingMobile = false;
                    if (gs && mi < gs->pairings.size())
                    {
                        pairingCol = statusColor(gs->pairings[mi].lastStatus);
                        pairingStatus = statusToString(gs->pairings[mi].lastStatus);
                        pairingMobile = gs->pairings[mi].mobile;
                    }

                    // Active pairing gets a highlight marker
                    bool isActive = gs && gs->activePairingIdx == (int)mi && gs->running;
                    if (isActive)
                    {
                        float pulse = (std::sin(animTime_ * 3.0f) + 1.0f) * 0.5f;
                        ImGui::TextColored({0.3f + pulse * 0.3f, 0.8f, 0.3f + pulse * 0.3f, 1.0f}, ">");
                    }
                    else
                    {
                        ImGui::TextColored({0.2f, 0.2f, 0.2f, 1.0f}, " ");
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(pairingCol, "%s", pairingStatus);
                    ImGui::SameLine();
                    ImGui::Text("%s [%s]", user.c_str(), site.c_str());
                    if (pairingMobile)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(COL_ORANGE, "(Mobile)");
                    }
                    ImGui::SameLine();
                    ImGui::PushID((int)mi);
                    if (mi == 0)
                    {
                        // Primary indicator (first pairing = primary)
                        ImGui::TextColored(COL_YELLOW, "[Primary]");
                        ImGui::SameLine();
                    }
                    else
                    {
                        // Button to set as primary (move to front)
                        ImGui::PushStyleColor(ImGuiCol_Button, {0.25f, 0.25f, 0.12f, 1.0f});
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.40f, 0.40f, 0.15f, 1.0f});
                        if (ImGui::SmallButton("Set as primary"))
                            manager_.setPrimaryPairing(group.groupName, mi);
                        ImGui::PopStyleColor(2);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Set as primary (checked first)");
                        ImGui::SameLine();
                    }
                    if (ImGui::SmallButton("Remove"))
                        manager_.removeFromCrossRegisterGroup(group.groupName, site, user);
                    ImGui::PopID();
                }

                // Add member — expanded inputs (two rows to avoid overlap)
                auto sites = manager_.availableSites();
                float addAvail = ImGui::GetContentRegionAvail().x;
                float spacing = ImGui::GetStyle().ItemSpacing.x;

                // Row 1: Username + Site combo
                float inputW = addAvail * 0.50f - spacing;
                float comboW = addAvail * 0.50f - spacing;

                ImGui::SetNextItemWidth(inputW);
                ImGui::InputTextWithHint("##AddUser", "Username", crossUsername_, sizeof(crossUsername_));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(comboW);
                if (ImGui::BeginCombo("##AddSite",
                                      crossSiteIdx_ < (int)sites.size() ? sites[crossSiteIdx_].c_str() : ""))
                {
                    // Search filter at top
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::IsWindowAppearing())
                    {
                        ImGui::SetKeyboardFocusHere();
                        std::memset(siteFilterBuf_, 0, sizeof(siteFilterBuf_));
                    }
                    ImGui::InputTextWithHint("##SiteFilter", "Type to filter...", siteFilterBuf_, sizeof(siteFilterBuf_));
                    ImGui::Separator();

                    std::string filter(siteFilterBuf_);
                    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

                    for (int i = 0; i < (int)sites.size(); i++)
                    {
                        if (!filter.empty())
                        {
                            std::string lower = sites[i];
                            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                            if (lower.find(filter) == std::string::npos)
                                continue;
                        }
                        if (ImGui::Selectable(sites[i].c_str(), crossSiteIdx_ == i))
                            crossSiteIdx_ = i;
                    }
                    ImGui::EndCombo();
                }
                // Row 2: Add + Delete buttons on their own line
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.40f, 0.15f, 1.0f});
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f, 0.55f, 0.20f, 1.0f});
                    bool canAdd = std::strlen(crossUsername_) > 0 && crossSiteIdx_ < (int)sites.size();
                    if (!canAdd)
                        ImGui::BeginDisabled();
                    if (ImGui::SmallButton("Add Member"))
                    {
                        manager_.addToCrossRegisterGroup(
                            group.groupName, sites[crossSiteIdx_], crossUsername_);
                        std::memset(crossUsername_, 0, sizeof(crossUsername_));
                    }
                    if (!canAdd)
                        ImGui::EndDisabled();
                    ImGui::PopStyleColor(2);
                }

                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, {0.50f, 0.15f, 0.15f, 1.0f});
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.65f, 0.20f, 0.20f, 1.0f});
                if (ImGui::SmallButton("Delete Group"))
                    manager_.removeCrossRegisterGroup(group.groupName);
                ImGui::PopStyleColor(2);
                ImGui::Spacing();
            }

            ImGui::PopID();
        }

        if (groups.empty())
            ImGui::TextColored(COL_TEXT_DIM, "No cross-register groups defined.");

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Add to Group popup (proper ImGui modal popup)
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderAddToGroupPopup()
    {
        if (!showAddToGroupPopup_)
            return;

        ImGui::SetNextWindowSize({400 * dpiScale_, 340 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({300 * dpiScale_, 240 * dpiScale_}, {600 * dpiScale_, 500 * dpiScale_});
        if (ImGui::Begin("Add to Group", &showAddToGroupPopup_,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse))
        {
            ImGui::Text("Add  %s [%s]  to a cross-register group:",
                        addToGroupBotUser_.c_str(), addToGroupBotSite_.c_str());
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            auto groups = manager_.getCrossRegisterGroups();

            // List existing groups as radio buttons
            ImGui::TextColored(COL_ACCENT, "Existing Groups:");
            for (int gi = 0; gi < (int)groups.size(); gi++)
            {
                std::string label = groups[gi].groupName + " (" +
                                    std::to_string(groups[gi].members.size()) + " members)";
                if (ImGui::RadioButton(label.c_str(), addToGroupSelectedIdx_ == gi))
                    addToGroupSelectedIdx_ = gi;
            }

            if (groups.empty())
                ImGui::TextColored(COL_TEXT_DIM, "  No groups yet");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // Option to create a new group
            if (ImGui::RadioButton("Create new group:", addToGroupSelectedIdx_ == -1))
                addToGroupSelectedIdx_ = -1;
            ImGui::SameLine();
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            ImGui::InputTextWithHint("##NewGroupName", "Group name...",
                                     addToGroupNewName_, sizeof(addToGroupNewName_));

            ImGui::Spacing();
            ImGui::Spacing();

            // Buttons — properly sized side by side
            float btnW = 120 * dpiScale_;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float totalW = btnW * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalW) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.55f, 0.25f, 1.0f});
            if (ImGui::Button("Add", {btnW, 0}))
            {
                if (addToGroupSelectedIdx_ >= 0 && addToGroupSelectedIdx_ < (int)groups.size())
                {
                    // Add to existing group
                    manager_.addToCrossRegisterGroup(
                        groups[addToGroupSelectedIdx_].groupName,
                        addToGroupBotSite_, addToGroupBotUser_);
                    addLog("info", "system", "Added " + addToGroupBotUser_ + " to group " + groups[addToGroupSelectedIdx_].groupName);
                }
                else if (std::strlen(addToGroupNewName_) > 0)
                {
                    // Create new group with this bot as first member
                    std::vector<std::pair<std::string, std::string>> members = {
                        {addToGroupBotSite_, addToGroupBotUser_}};
                    manager_.createCrossRegisterGroup(addToGroupNewName_, members);
                    addLog("info", "system", "Created group " + std::string(addToGroupNewName_) + " with " + addToGroupBotUser_);
                }
                showAddToGroupPopup_ = false;
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            if (ImGui::Button("Cancel", {btnW, 0}))
                showAddToGroupPopup_ = false;
        }
        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Stream View window (live stream from recording bot)
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderStreamViewWindow()
    {
        ImGui::SetNextWindowSize({640 * dpiScale_, 400 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({400 * dpiScale_, 280 * dpiScale_}, {1200 * dpiScale_, 900 * dpiScale_});
        if (!ImGui::Begin("Stream View", &showStreamView_))
        {
            // Window collapsed or hidden — stop audio if playing
            if (!audioStreamKey_.empty())
            {
                auto sep = audioStreamKey_.find('|');
                if (sep != std::string::npos)
                    manager_.clearAudioDataCallback(
                        audioStreamKey_.substr(0, sep),
                        audioStreamKey_.substr(sep + 1));
                audioPlayer_.stop();
                audioStreamKey_.clear();
            }
            ImGui::End();
            return;
        }

        // Find the first recording bot (or use selected bot)
        int viewIdx = -1;
        if (selectedBot_ >= 0 && selectedBot_ < (int)cachedStates_.size() &&
            cachedStates_[selectedBot_].recording)
        {
            viewIdx = selectedBot_;
        }
        else
        {
            // Auto-pick first recording bot
            for (int i = 0; i < (int)cachedStates_.size(); i++)
            {
                if (cachedStates_[i].recording)
                {
                    viewIdx = i;
                    break;
                }
            }
        }

        if (viewIdx < 0)
        {
            // No recording — stop audio
            if (!audioStreamKey_.empty())
            {
                auto sep = audioStreamKey_.find('|');
                if (sep != std::string::npos)
                    manager_.clearAudioDataCallback(
                        audioStreamKey_.substr(0, sep),
                        audioStreamKey_.substr(sep + 1));
                audioPlayer_.stop();
                audioStreamKey_.clear();
            }

            float w = ImGui::GetContentRegionAvail().x;
            float h = ImGui::GetContentRegionAvail().y;
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, {pos.x + w, pos.y + h}, IM_COL32(15, 15, 20, 255));
            ImVec2 textSz = ImGui::CalcTextSize("No active recordings");
            ImVec2 textPos = {pos.x + (w - textSz.x) * 0.5f, pos.y + (h - textSz.y) * 0.5f};
            dl->AddText(textPos, IM_COL32(120, 120, 140, 255), "No active recordings");
            ImGui::Dummy({w, h});
            ImGui::End();
            return;
        }

        const auto &bot = cachedStates_[viewIdx];

        // Header: bot name + resolution
        ImGui::TextColored(COL_ACCENT, "%s [%s]", bot.username.c_str(), bot.siteName.c_str());
        ImGui::SameLine();
        if (bot.recordingStats.recordingWidth > 0)
            ImGui::TextColored(COL_TEXT_DIM, "  %dx%d  %.1fx",
                               bot.recordingStats.recordingWidth,
                               bot.recordingStats.recordingHeight,
                               bot.recordingStats.currentSpeed);

        // Bot selector for switching between recording bots
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 200 * dpiScale_);
        ImGui::SetNextItemWidth(200 * dpiScale_);
        if (ImGui::BeginCombo("##StreamBot", (bot.username + " [" + bot.siteName + "]").c_str()))
        {
            for (int i = 0; i < (int)cachedStates_.size(); i++)
            {
                if (!cachedStates_[i].recording)
                    continue;
                bool sel = (viewIdx == i);
                std::string label = cachedStates_[i].username + " [" + cachedStates_[i].siteName + "]";
                if (ImGui::Selectable(label.c_str(), sel))
                    selectedBot_ = i;
            }
            ImGui::EndCombo();
        }

        // ── Audio controls ──────────────────────────────────────────
        {
            // Mute toggle
            if (audioMuted_)
            {
                if (ImGui::Button("\xf0\x9f\x94\x87##Mute")) // 🔇
                    audioMuted_ = false;
            }
            else
            {
                if (ImGui::Button("\xf0\x9f\x94\x8a##Mute")) // 🔊
                    audioMuted_ = true;
            }
            ImGui::SameLine();

            // Volume slider
            ImGui::SetNextItemWidth(120 * dpiScale_);
            if (ImGui::SliderFloat("##Volume", &audioVolume_, 0.0f, 1.0f, "Vol %.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp))
            {
                audioPlayer_.setVolume(audioMuted_ ? 0.0f : audioVolume_);
            }
            // Also apply mute state every frame
            audioPlayer_.setVolume(audioMuted_ ? 0.0f : audioVolume_);
        }

        // Determine the key for preview
        std::string previewKey = bot.username + "|" + bot.siteName;

        // If we switched to a different bot, discard the old texture & rewire audio
        if (previewKey != detailPreviewKey_)
        {
            if (detailPreviewTex_)
            {
                glDeleteTextures(1, &detailPreviewTex_);
                detailPreviewTex_ = 0;
            }
            detailPreviewW_ = detailPreviewH_ = 0;
            detailPreviewKey_ = previewKey;
            previewVersion_ = 0; // reset version to get first frame immediately

            // Unwire old audio
            if (!audioStreamKey_.empty())
            {
                auto sep = audioStreamKey_.find('|');
                if (sep != std::string::npos)
                    manager_.clearAudioDataCallback(
                        audioStreamKey_.substr(0, sep),
                        audioStreamKey_.substr(sep + 1));
                audioPlayer_.stop();
            }

            // Wire new audio
            audioStreamKey_ = previewKey;
            audioPlayer_.init(48000, 2);
            audioPlayer_.flush(); // clear stale samples from previous stream
            audioPlayer_.setVolume(audioMuted_ ? 0.0f : audioVolume_);
            audioPlayer_.start();
            manager_.setAudioDataCallback(bot.username, bot.siteName,
                                          [this](const float *samples, size_t frameCount)
                                          {
                                              audioPlayer_.pushSamples(samples, frameCount);
                                          });
        }

        // Consume latest frame from continuous preview stream
        PreviewFrame frame;
        if (manager_.consumePreview(bot.username, bot.siteName, frame, previewVersion_))
        {
            if (detailPreviewTex_ == 0)
            {
                glGenTextures(1, &detailPreviewTex_);
                glBindTexture(GL_TEXTURE_2D, detailPreviewTex_);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            else
            {
                glBindTexture(GL_TEXTURE_2D, detailPreviewTex_);
            }

            // Fast path: if dimensions unchanged, update in-place (no realloc)
            if (frame.width == detailPreviewW_ && frame.height == detailPreviewH_)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels.data());
            }
            else
            {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels.data());
            }
            glBindTexture(GL_TEXTURE_2D, 0);

            detailPreviewW_ = frame.width;
            detailPreviewH_ = frame.height;
        }

        // Render stream view — fill available window space
        float availW = ImGui::GetContentRegionAvail().x;
        float availH = ImGui::GetContentRegionAvail().y;

        if (detailPreviewTex_ != 0 && detailPreviewW_ > 0 && detailPreviewH_ > 0)
        {
            float aspect = static_cast<float>(detailPreviewH_) / detailPreviewW_;
            float imgW = availW;
            float imgH = imgW * aspect;
            if (imgH > availH)
            {
                imgH = availH;
                imgW = imgH / aspect;
            }
            // Center the image
            float offsetX = (availW - imgW) * 0.5f;
            if (offsetX > 0)
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
            ImGui::Image((ImTextureID)(intptr_t)detailPreviewTex_, {imgW, imgH});
        }
        else
        {
            ImVec2 pos = ImGui::GetCursorScreenPos();
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(pos, {pos.x + availW, pos.y + availH},
                              IM_COL32(15, 15, 20, 255));
            const char *waitLabel = "Waiting for stream frame...";
            ImVec2 textSz = ImGui::CalcTextSize(waitLabel);
            ImVec2 textPos = {pos.x + (availW - textSz.x) * 0.5f,
                              pos.y + (availH - textSz.y) * 0.5f};
            dl->AddText(textPos, IM_COL32(120, 120, 140, 255), waitLabel);
            ImGui::Dummy({availW, availH});
        }

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Bot detail panel
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderBotDetailPanel()
    {
        const auto &bot = cachedStates_[selectedBot_];
        std::string title = bot.username + " [" + bot.siteName + "]###BotDetail";

        ImGui::SetNextWindowSize({540 * dpiScale_, 500 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({400 * dpiScale_, 350 * dpiScale_}, {800 * dpiScale_, 700 * dpiScale_});
        if (!ImGui::Begin(title.c_str(), &showBotDetail_))
        {
            ImGui::End();
            return;
        }

        // Header
        ImGui::TextColored(COL_ACCENT, "%s", bot.username.c_str());
        ImGui::SameLine();
        ImGui::TextColored(statusColor(bot.status), "(%s)", statusToString(bot.status));

        // Preview thumbnail — collapsible, in-memory, direct GL texture
        if (ImGui::CollapsingHeader("Live Preview", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Spacing();
            float avail = ImGui::GetContentRegionAvail().x;
            float imgW = std::min(avail, 320.0f * dpiScale_);

            // Determine the key for this bot
            std::string previewKey = bot.username + "|" + bot.siteName;

            // If we switched to a different bot, discard the old texture
            if (previewKey != detailPreviewKey_)
            {
                if (detailPreviewTex_)
                {
                    glDeleteTextures(1, &detailPreviewTex_);
                    detailPreviewTex_ = 0;
                }
                detailPreviewW_ = detailPreviewH_ = 0;
                detailPreviewKey_ = previewKey;
                previewVersion_ = 0; // reset version to get first frame immediately
            }

            // Consume latest frame from continuous preview stream
            PreviewFrame frame;
            if (manager_.consumePreview(bot.username, bot.siteName, frame, previewVersion_))
            {
                // Upload RGBA pixels directly to a GL texture
                if (detailPreviewTex_ == 0)
                {
                    glGenTextures(1, &detailPreviewTex_);
                    glBindTexture(GL_TEXTURE_2D, detailPreviewTex_);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
                else
                {
                    glBindTexture(GL_TEXTURE_2D, detailPreviewTex_);
                }

                if (frame.width == detailPreviewW_ && frame.height == detailPreviewH_)
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
                                    GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels.data());
                else
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame.width, frame.height, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, frame.pixels.data());
                glBindTexture(GL_TEXTURE_2D, 0);

                detailPreviewW_ = frame.width;
                detailPreviewH_ = frame.height;
            }

            // Render the texture or a placeholder
            if (detailPreviewTex_ != 0 && detailPreviewW_ > 0 && detailPreviewH_ > 0)
            {
                // Maintain aspect ratio
                float aspect = static_cast<float>(detailPreviewH_) / detailPreviewW_;
                float imgH = imgW * aspect;
                ImGui::Image((ImTextureID)(intptr_t)detailPreviewTex_, {imgW, imgH});

                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Live preview (%dx%d)", detailPreviewW_, detailPreviewH_);
            }
            else
            {
                float imgH = imgW * 0.5625f; // 16:9 placeholder
                ImVec2 pos = ImGui::GetCursorScreenPos();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddRectFilled(pos, {pos.x + imgW, pos.y + imgH},
                                  IM_COL32(20, 20, 28, 255));
                dl->AddRect(pos, {pos.x + imgW, pos.y + imgH},
                            IM_COL32(60, 60, 70, 255));

                const char *label = bot.recording
                                        ? "Requesting preview..."
                                        : "Preview available when recording";
                ImVec2 textSz = ImGui::CalcTextSize(label);
                ImVec2 textPos = {pos.x + (imgW - textSz.x) * 0.5f,
                                  pos.y + (imgH - textSz.y) * 0.5f};
                dl->AddText(textPos, IM_COL32(120, 120, 140, 255), label);

                ImGui::Dummy({imgW, imgH});
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Info grid
        ImGui::Text("Site:            %s (%s)", bot.siteName.c_str(), bot.siteSlug.c_str());
        ImGui::Text("Website:         %s", bot.websiteUrl.c_str());
        ImGui::TextColored(statusColor(bot.status), "Status:          %s", statusToString(bot.status));
        ImGui::Text("Gender:          %s", genderToString(bot.gender));
        if (!bot.country.empty())
            ImGui::Text("Country:         %s", bot.country.c_str());
        ImGui::Text("Running:         %s", bot.running ? "Yes" : "No");
        ImGui::Text("Recording:       %s", bot.recording ? "Yes" : "No");
        if (bot.recording && bot.recordingStats.recordingWidth > 0)
            ImGui::Text("Resolution:      %dx%d", bot.recordingStats.recordingWidth, bot.recordingStats.recordingHeight);
        else if (bot.recordingStats.recordingWidth > 0)
            ImGui::TextColored(COL_TEXT_DIM, "Last Resolution: %dx%d", bot.recordingStats.recordingWidth, bot.recordingStats.recordingHeight);
        if (bot.mobile)
            ImGui::TextColored(COL_ORANGE, "Mobile:          Yes (broadcasting from phone)");
        ImGui::Text("Errors:          %d", bot.consecutiveErrors);
        ImGui::Text("Files:           %d", bot.fileCount);
        ImGui::Text("Total Data:      %s", formatBytesHuman(bot.totalBytes).c_str());

        if (bot.recording)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(COL_GREEN, "Recording Details");
            ImGui::Text("Current File:    %s", bot.recordingStats.currentFile.c_str());
            ImGui::Text("Written:         %s", formatBytesHuman(bot.recordingStats.bytesWritten).c_str());
            ImGui::Text("Segments:        %u", bot.recordingStats.segmentsRecorded);
            ImGui::Text("Speed:           %.1fx", bot.recordingStats.currentSpeed);
            ImGui::Text("Stalls:          %u", bot.recordingStats.stallsDetected);
        }

        // Cross-register group
        auto group = manager_.getGroupForBot(bot.username, bot.siteName);
        if (group)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(COL_CYAN, "Cross-Register: %s", group->groupName.c_str());
            for (const auto &[site, user] : group->members)
            {
                if (user != bot.username || site != bot.siteName)
                    ImGui::BulletText("%s [%s]", user.c_str(), site.c_str());
            }
        }

        // Error/Debug info section
        if (!bot.lastError.empty() || !bot.lastApiResponse.empty() || bot.lastHttpCode != 0)
        {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            ImGui::TextColored(COL_ORANGE, "Debug Info");

            if (bot.lastHttpCode != 0)
                ImGui::Text("Last HTTP Code:  %d", bot.lastHttpCode);

            if (!bot.lastError.empty())
            {
                ImGui::Text("Last Error:");
                ImGui::PushStyleColor(ImGuiCol_Text, COL_RED);
                ImGui::TextWrapped("%s", bot.lastError.c_str());
                ImGui::PopStyleColor();
                if (ImGui::SmallButton("Copy Error"))
                    copyToClipboard(bot.lastError);
            }

            if (!bot.lastApiResponse.empty())
            {
                ImGui::Spacing();
                ImGui::Text("Last API Response:");

                // Search bar + view toggle
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 140 * dpiScale_);
                ImGui::InputTextWithHint("##JsonSearch", "Search keys/values...",
                                         jsonSearchBuf_, sizeof(jsonSearchBuf_));
                ImGui::SameLine();
                ImGui::Checkbox("Raw", &jsonShowRaw_);
                ImGui::SameLine();
                if (ImGui::SmallButton("Copy JSON"))
                    copyToClipboard(bot.lastApiResponse);

                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 6));
                ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4{0.07f, 0.07f, 0.09f, 1.0f});
                ImGui::BeginChild("##ApiJson", ImVec2(0, 350 * dpiScale_), true,
                                  ImGuiWindowFlags_HorizontalScrollbar);

                if (jsonShowRaw_)
                {
                    // Pretty-printed raw text
                    try
                    {
                        auto parsed = nlohmann::json::parse(bot.lastApiResponse);
                        std::string pretty = parsed.dump(2);
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_DIM);
                        ImGui::TextUnformatted(pretty.c_str());
                        ImGui::PopStyleColor();
                    }
                    catch (...)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, COL_TEXT_DIM);
                        ImGui::TextUnformatted(bot.lastApiResponse.c_str());
                        ImGui::PopStyleColor();
                    }
                }
                else
                {
                    // Interactive tree viewer
                    renderJsonTree(bot.lastApiResponse, jsonSearchBuf_);
                }

                ImGui::EndChild();
                ImGui::PopStyleColor();
                ImGui::PopStyleVar();
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // URL buttons — auto-sized to fill width
        if (!bot.websiteUrl.empty())
        {
            float avail = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float btnW = (avail - spacing * 2.0f) / 3.0f;

            ImGui::PushStyleColor(ImGuiCol_Button, {0.20f, 0.35f, 0.60f, 1.0f});
            if (ImGui::Button("Copy URL", {btnW, 0}))
                copyToClipboard(bot.websiteUrl);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Open in Browser", {btnW, 0}))
                openUrlInBrowser(bot.websiteUrl);
            ImGui::SameLine();
            if (ImGui::Button("Open Folder", {btnW, 0}))
            {
                auto folder = config_.downloadsDir / (bot.username + " [" + bot.siteSlug + "]");
                if (std::filesystem::exists(folder))
                    openFolderInExplorer(folder);
                else
                    openFolderInExplorer(config_.downloadsDir);
            }
        }

        ImGui::Spacing();

        // Action buttons — auto-sized to fill width
        {
            float avail = ImGui::GetContentRegionAvail().x;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float btnW = (avail - spacing * 2.0f) / 3.0f;

            if (bot.running)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.5f, 0.15f, 0.15f, 1.0f});
                if (ImGui::Button("Stop", {btnW, 0}))
                {
                    std::string u = bot.username, s = bot.siteName;
                    safeDetach([this, u, s]()
                               { manager_.stopBot(u, s); }, "detail-stop");
                }
                ImGui::PopStyleColor();
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.40f, 0.15f, 1.0f});
                if (ImGui::Button("Start", {btnW, 0}))
                    manager_.startBot(bot.username, bot.siteName);
                ImGui::PopStyleColor();
            }
            ImGui::SameLine();
            if (ImGui::Button("Restart", {btnW, 0}))
            {
                std::string u = bot.username, s = bot.siteName;
                safeDetach([this, u, s]()
                           { manager_.restartBot(u, s); }, "detail-restart");
            }
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Button, {0.5f, 0.15f, 0.15f, 1.0f});
            if (ImGui::Button("Remove", {btnW, 0}))
            {
                manager_.removeBot(bot.username, bot.siteName);
                showBotDetail_ = false;
            }
            ImGui::PopStyleColor();
        }

        // Resync button (full width)
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, {0.10f, 0.45f, 0.55f, 1.0f});
        if (ImGui::Button("Force Resync (Check Status Now)", {-1, 0}))
        {
            std::string user = bot.username;
            std::string site = bot.siteSlug;
            pendingResyncBot_ = std::make_pair(user, site);
        }
        ImGui::PopStyleColor();

        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Edit Model Dialog
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::renderEditModelDialog()
    {
        if (!showEditModel_)
            return;

        ImGui::SetNextWindowSize({420 * dpiScale_, 200 * dpiScale_}, ImGuiCond_Appearing);
        ImGui::SetNextWindowSizeConstraints({300 * dpiScale_, 150 * dpiScale_}, {600 * dpiScale_, 400 * dpiScale_});
        if (ImGui::Begin("Edit Model##EditModel", &showEditModel_,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking))
        {
            ImGui::Text("Editing:  %s  [%s]", editModelOrigUser_.c_str(), editModelOrigSite_.c_str());
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Username:");
            ImGui::SameLine(110 * dpiScale_);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##editUser", editModelUsername_, sizeof(editModelUsername_));

            ImGui::Text("Site:");
            ImGui::SameLine(110 * dpiScale_);
            ImGui::SetNextItemWidth(-1);
            auto sites = manager_.availableSites();
            if (ImGui::BeginCombo("##editSite", editModelSiteIdx_ < (int)sites.size()
                                                    ? sites[editModelSiteIdx_].c_str()
                                                    : ""))
            {
                for (int i = 0; i < (int)sites.size(); i++)
                {
                    bool sel = (i == editModelSiteIdx_);
                    if (ImGui::Selectable(sites[i].c_str(), sel))
                        editModelSiteIdx_ = i;
                    if (sel)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            float btnW = 100 * dpiScale_;
            float spacing = ImGui::GetStyle().ItemSpacing.x;
            float totalW = btnW * 2 + spacing;
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - totalW) * 0.5f);

            ImGui::PushStyleColor(ImGuiCol_Button, {0.15f, 0.55f, 0.25f, 1.0f});
            if (ImGui::Button("Apply", {btnW, 0}))
            {
                std::string newUser = stripWhitespace(editModelUsername_);
                std::string newSite = (editModelSiteIdx_ < (int)sites.size())
                                          ? sites[editModelSiteIdx_]
                                          : editModelOrigSite_;
                if (!newUser.empty())
                {
                    manager_.editBot(editModelOrigUser_, editModelOrigSite_, newUser, newSite);
                    addLog("info", "system", "Edited model: " + editModelOrigUser_ + " [" + editModelOrigSite_ + "] -> " + newUser + " [" + newSite + "]");
                    showEditModel_ = false;
                }
            }
            ImGui::PopStyleColor();

            ImGui::SameLine();
            if (ImGui::Button("Cancel", {btnW, 0}))
                showEditModel_ = false;
        }
        ImGui::End();
    }

    // ─────────────────────────────────────────────────────────────────
    // Adaptive frame rate — GLFW input callbacks
    // These mark the GUI as "active" so we render at 30fps instead of
    // 4fps idle.  ImGui_ImplGlfw already installs its own callbacks
    // via ImGui_ImplGlfw_InitForOpenGL, but GLFW supports chaining:
    // we call ImGui's handlers from ours.
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::markActive()
    {
        lastInputTime_ = glfwGetTime();
    }

    void GuiApp::glfwCursorPosCallback(GLFWwindow *w, double, double)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    void GuiApp::glfwMouseButtonCallback(GLFWwindow *w, int, int, int)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    void GuiApp::glfwScrollCallback(GLFWwindow *w, double, double)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    void GuiApp::glfwKeyCallback(GLFWwindow *w, int, int, int, int)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    void GuiApp::glfwCharCallback(GLFWwindow *w, unsigned int)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    void GuiApp::glfwWindowFocusCallback(GLFWwindow *w, int)
    {
        auto *app = static_cast<GuiApp *>(glfwGetWindowUserPointer(w));
        if (app)
            app->markActive();
    }

    // ─────────────────────────────────────────────────────────────────
    // Helpers
    // ─────────────────────────────────────────────────────────────────
    void GuiApp::refreshBotStates()
    {
        // Non-blocking: skip refresh if manager mutex is held by a
        // background operation (file move, bulk stop, etc.).
        // The GUI keeps the previous cachedStates_ and retries next cycle.
        auto states = manager_.tryGetAllStates();
        if (states)
            cachedStates_ = std::move(*states);
    }

    void GuiApp::addLog(const std::string &level, const std::string &source,
                        const std::string &message)
    {
        std::lock_guard lock(logMutex_);
        logEntries_.push_back({message, level, source, Clock::now()});
        if (logEntries_.size() > kMaxLogEntries)
            logEntries_.pop_front();
    }

    ImVec4 GuiApp::statusColor(Status status) const
    {
        switch (status)
        {
        case Status::Public:
        case Status::Online:
            return COL_GREEN;
        case Status::Private:
        case Status::Restricted:
            return COL_MAGENTA;
        case Status::Offline:
            return COL_TEXT_DIM;
        case Status::LongOffline:
            return {0.35f, 0.35f, 0.40f, 1.0f};
        case Status::Error:
            return COL_RED;
        case Status::RateLimit:
        case Status::Cloudflare:
            return COL_ORANGE;
        case Status::NotExist:
        case Status::Deleted:
            return COL_RED;
        case Status::NotRunning:
            return {0.40f, 0.40f, 0.45f, 1.0f};
        default:
            return COL_YELLOW;
        }
    }

    const char *GuiApp::statusIcon(Status status) const
    {
        switch (status)
        {
        case Status::Public:
        case Status::Online:
            return "[LIVE]";
        case Status::Private:
            return "[PRV]";
        case Status::Restricted:
            return "[RST]";
        case Status::Offline:
        case Status::LongOffline:
            return "[OFF]";
        case Status::Error:
            return "[ERR]";
        case Status::RateLimit:
            return "[LIM]";
        case Status::NotExist:
        case Status::Deleted:
            return "[DEL]";
        case Status::NotRunning:
            return "[---]";
        case Status::Cloudflare:
            return "[CF]";
        default:
            return "[???]";
        }
    }

    std::string GuiApp::formatBytes(uint64_t bytes) const
    {
        return formatBytesHuman(bytes);
    }

    std::string GuiApp::formatBytesHuman(uint64_t bytes) const
    {
        const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
        double size = (double)bytes;
        int unit = 0;
        while (size >= 1024.0 && unit < 5)
        {
            size /= 1024.0;
            unit++;
        }

        std::ostringstream oss;
        if (unit == 0)
            oss << bytes << " B";
        else
            oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
        return oss.str();
    }

    std::string GuiApp::formatDuration(double seconds) const
    {
        int h = (int)seconds / 3600;
        int m = ((int)seconds % 3600) / 60;
        int s = (int)seconds % 60;

        std::ostringstream oss;
        if (h > 0)
            oss << h << "h " << m << "m";
        else if (m > 0)
            oss << m << "m " << s << "s";
        else
            oss << s << "s";
        return oss.str();
    }

    std::string GuiApp::formatTimeAgo(TimePoint tp) const
    {
        auto now = Clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - tp).count();
        return formatDuration((double)elapsed);
    }

    float GuiApp::getStatusPulse(Status status) const
    {
        if (status == Status::Public || status == Status::Online)
            return (std::sin(animTime_ * 2.0f) + 1.0f) * 0.5f * 0.15f;
        return 0.0f;
    }

} // namespace sm
