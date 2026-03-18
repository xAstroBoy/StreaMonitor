// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — CLI Entry Point
// Interactive command-line monitoring and control
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "config/config.h"
#include "core/site_plugin.h"
#include "core/bot_manager.h"
#include "net/http_client.h"
#include "web/web_server.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <csignal>
#include <thread>
#include <chrono>
#include <iomanip>

// Include all site plugins so they auto-register
#include "sites/chaturbate.h"
#include "sites/camsoda.h"
#include "sites/bongacams.h"
#include "sites/stripchat.h"
#include "sites/cam4.h"
#include "sites/cherrytv.h"
#include "sites/dreamcam.h"
#include "sites/flirt4free.h"
#include "sites/xlovecam.h"
#include "sites/fanslylive.h"
#include "sites/myfreecams.h"
#include "sites/streamate.h"
#include "sites/camscom.h"
#include "sites/amateurtv.h"
#include "sites/manyvids.h"
#include "sites/sexchathu.h"
#include "sites/stripchatvr.h"
#include "sites/dreamcamvr.h"

extern "C"
{
#include <libavformat/avformat.h>
}

static std::atomic<bool> g_shutdown{false};
static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_logRingBuffer;

static void signalHandler(int sig)
{
    std::cout << "\nReceived signal, shutting down...\n";
    g_shutdown.store(true);
}

static void initLogging()
{
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("[%H:%M:%S] [%^%l%$] [%n] %v");

    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "streamonitor.log", 10 * 1024 * 1024, 3);
    file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

    g_logRingBuffer = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(500);
    g_logRingBuffer->set_pattern("[%H:%M:%S] [%l] %v");

    auto logger = std::make_shared<spdlog::logger>("sm",
                                                   spdlog::sinks_init_list{console, file, g_logRingBuffer});
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// ─────────────────────────────────────────────────────────────────
// ANSI colors
// ─────────────────────────────────────────────────────────────────
namespace ansi
{
    const char *reset = "\033[0m";
    const char *red = "\033[31m";
    const char *green = "\033[32m";
    const char *yellow = "\033[33m";
    const char *blue = "\033[34m";
    const char *magenta = "\033[35m";
    const char *cyan = "\033[36m";
    const char *white = "\033[37m";
    const char *gray = "\033[90m";
    const char *bold = "\033[1m";
}

static const char *statusColorAnsi(sm::Status s)
{
    switch (s)
    {
    case sm::Status::Public:
        return ansi::green;
    case sm::Status::Private:
        return ansi::magenta;
    case sm::Status::Offline:
        return ansi::yellow;
    case sm::Status::LongOffline:
        return ansi::gray;
    case sm::Status::Online:
        return ansi::cyan;
    case sm::Status::Error:
        return ansi::red;
    case sm::Status::RateLimit:
        return ansi::red;
    case sm::Status::NotExist:
        return ansi::red;
    case sm::Status::NotRunning:
        return ansi::gray;
    default:
        return ansi::white;
    }
}

static std::string formatBytes(uint64_t bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    double size = (double)bytes;
    while (size >= 1024.0 && unit < 4)
    {
        size /= 1024.0;
        unit++;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "%.1f %s", size, units[unit]);
    return buf;
}

static std::vector<std::string> split(const std::string &s)
{
    std::vector<std::string> tokens;
    std::istringstream iss(s);
    std::string tok;
    while (iss >> tok)
        tokens.push_back(tok);
    return tokens;
}

// ─────────────────────────────────────────────────────────────────
// Status display
// ─────────────────────────────────────────────────────────────────
static void printStatus(sm::BotManager &manager)
{
    auto states = manager.getAllStates();

    std::cout << "\n"
              << ansi::bold << ansi::cyan
              << "╔══════════════════════════════════════════════════════════════════╗\n"
              << "║                     StreaMonitor v2.0                           ║\n"
              << "╚══════════════════════════════════════════════════════════════════╝\n"
              << ansi::reset;

    // Header
    std::cout << ansi::bold
              << std::left << std::setw(22) << "  Username"
              << std::setw(14) << "Site"
              << std::setw(14) << "Status"
              << std::setw(10) << "Running"
              << std::setw(10) << "REC"
              << std::setw(12) << "Size"
              << ansi::reset << "\n";
    std::cout << "  " << std::string(80, '-') << "\n";

    size_t online = 0, recording = 0;
    for (const auto &st : states)
    {
        const char *col = statusColorAnsi(st.status);

        std::cout << "  "
                  << std::left << std::setw(22) << st.username
                  << std::setw(14) << st.siteSlug
                  << col << std::setw(14) << sm::statusToString(st.status) << ansi::reset
                  << std::setw(10) << (st.running ? "yes" : "no");

        if (st.recording)
        {
            std::cout << ansi::red << std::setw(10) << "REC" << ansi::reset;
            uint64_t bytes = st.recordingStats.bytesWritten;
            std::cout << std::setw(12) << formatBytes(bytes);
            recording++;
        }
        else
        {
            std::cout << std::setw(10) << "-"
                      << std::setw(12) << "-";
        }

        if (st.status == sm::Status::Public || st.status == sm::Status::Online)
            online++;

        std::cout << "\n";
    }

    std::cout << "  " << std::string(80, '-') << "\n";
    std::cout << ansi::bold
              << "  Total: " << states.size()
              << " | " << ansi::green << "Online: " << online << ansi::reset
              << ansi::bold << " | " << ansi::red << "Recording: " << recording
              << ansi::reset << "\n\n";
}

static void printHelp()
{
    std::cout << ansi::bold << ansi::cyan << "\nAvailable Commands:\n"
              << ansi::reset
              << "  " << ansi::bold << "add" << ansi::reset << " <username> <site>   - Add a model to monitor\n"
              << "  " << ansi::bold << "remove" << ansi::reset << " <username>        - Remove a model\n"
              << "  " << ansi::bold << "start" << ansi::reset << " <username|*>       - Start monitoring\n"
              << "  " << ansi::bold << "stop" << ansi::reset << " <username|*>        - Stop monitoring\n"
              << "  " << ansi::bold << "restart" << ansi::reset << " <username>       - Restart a bot\n"
              << "  " << ansi::bold << "move" << ansi::reset << " <username|*>        - Move files to unprocessed folder\n"
              << "  " << ansi::bold << "resync" << ansi::reset << " <username|*>      - Force status recheck & restart\n"
              << "  " << ansi::bold << "status" << ansi::reset << "                   - Show status table\n"
              << "  " << ansi::bold << "sites" << ansi::reset << "                    - List available sites\n"
              << "  " << ansi::bold << "save" << ansi::reset << "                     - Save config\n"
              << "  " << ansi::bold << "web" << ansi::reset << "                      - Toggle web dashboard / show URL\n"
              << "  " << ansi::bold << "cls" << ansi::reset << "                      - Clear screen\n"
              << "  " << ansi::bold << "quit" << ansi::reset << "                     - Stop all and exit\n"
              << "  " << ansi::bold << "help" << ansi::reset << "                     - Show this help\n\n";
}

// ─────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────
int cliMain(int argc, char **argv)
{
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    initLogging();
    spdlog::info("StreaMonitor v2.0 starting (CLI mode)");

    avformat_network_init();
    sm::HttpClient::globalInit();

    // Load config
    sm::AppConfig config;
    config.loadFromEnv();

    // Load persisted app settings (overrides env where set)
    std::filesystem::path appConfigPath = "app_config.json";
    if (std::filesystem::exists(appConfigPath))
    {
        config.loadFromFile(appConfigPath);
        spdlog::info("Loaded app settings from {}", appConfigPath.string());
    }

    sm::ModelConfigStore configStore;
    std::filesystem::path configPath = "config.json";
    if (std::filesystem::exists(configPath))
    {
        configStore.load(configPath);
        spdlog::info("Loaded {} model configs", configStore.getAll().size());
    }

    // Print sites
    auto &registry = sm::SiteRegistry::instance();
    std::cout << ansi::cyan << "Registered sites: " << ansi::reset;
    for (const auto &name : registry.siteNames())
    {
        std::cout << name << " ";
    }
    std::cout << "\n";

    // Create manager
    sm::BotManager manager(config, configStore);

    // Load bots from config
    for (const auto &mc : configStore.getAll())
    {
        try
        {
            manager.addBot(mc.username, mc.site);
        }
        catch (const std::exception &e)
        {
            spdlog::warn("Skip {}: {}", mc.username, e.what());
        }
    }

    // Auto-start
    bool autostart = true;
    bool webOverride = false;
    bool webEnabled = config.webEnabled;
    for (int i = 1; i < argc; i++)
    {
        std::string arg(argv[i]);
        if (arg == "--no-autostart")
            autostart = false;
        else if (arg == "--web")
        {
            webEnabled = true;
            webOverride = true;
        }
        else if (arg == "--no-web")
        {
            webEnabled = false;
            webOverride = true;
        }
        else if (arg == "--port" && i + 1 < argc)
        {
            config.webPort = std::stoi(argv[++i]);
        }
        else if (arg == "--host" && i + 1 < argc)
        {
            config.webHost = argv[++i];
        }
        else if (arg == "--help" || arg == "-h")
        {
            std::cout << "StreaMonitor v2.0 - Stream Monitor & Recorder\n\n"
                      << "Usage: StreaMonitor --cli [options]\n\n"
                      << "Options:\n"
                      << "  --cli              Run in headless CLI mode (default: GUI)\n"
                      << "  --web              Enable web dashboard (default: on)\n"
                      << "  --no-web           Disable web dashboard\n"
                      << "  --port <port>      Web server port (default: 5000)\n"
                      << "  --host <host>      Web server host (default: 0.0.0.0)\n"
                      << "  --no-autostart     Don't auto-start monitoring\n"
                      << "  -h, --help         Show this help\n";
            return 0;
        }
    }

    if (autostart)
        manager.startAll();

    // Start web server if enabled
    std::unique_ptr<sm::WebServer> webServer;
    if (webEnabled)
    {
        webServer = std::make_unique<sm::WebServer>(manager, config, configStore);
        webServer->setLogRingBuffer(g_logRingBuffer);
        if (webServer->start())
        {
            std::cout << ansi::green << "🌐 Web dashboard running:\n"
                      << "   Local:   " << webServer->getLocalUrl() << "\n"
                      << "   Network: " << webServer->getNetworkUrl() << "\n"
                      << ansi::reset;
        }
        else
        {
            std::cout << ansi::red << "Failed to start web server\n"
                      << ansi::reset;
        }
    }
    else
    {
        std::cout << ansi::yellow << "Web dashboard disabled (use --web to enable)\n"
                  << ansi::reset;
    }

    printHelp();
    printStatus(manager);

    // Command loop
    std::string line;
    while (!g_shutdown.load())
    {
        std::cout << ansi::bold << ansi::cyan << "sm> " << ansi::reset;
        std::cout.flush();

        if (!std::getline(std::cin, line))
            break;
        if (line.empty())
            continue;

        auto tokens = split(line);
        if (tokens.empty())
            continue;

        const std::string &cmd = tokens[0];

        if (cmd == "quit" || cmd == "exit" || cmd == "q")
        {
            break;
        }
        else if (cmd == "help" || cmd == "h" || cmd == "?")
        {
            printHelp();
        }
        else if (cmd == "status" || cmd == "s")
        {
            printStatus(manager);
        }
        else if (cmd == "sites")
        {
            std::cout << "Available sites:\n";
            for (const auto &name : registry.siteNames())
            {
                std::cout << "  - " << name << "\n";
            }
        }
        else if (cmd == "add")
        {
            if (tokens.size() < 3)
            {
                std::cout << ansi::red << "Usage: add <username> <site>\n"
                          << ansi::reset;
            }
            else
            {
                // SC* alias: adds both SC (StripChat) and SCVR (StripChatVR)
                std::string siteArg = tokens[2];
                std::string siteLower = siteArg;
                std::transform(siteLower.begin(), siteLower.end(), siteLower.begin(), ::tolower);

                std::vector<std::string> sitesToAdd;
                if (siteLower == "sc*")
                {
                    sitesToAdd = {"SC", "SCVR"};
                    std::cout << ansi::cyan << "SC* combo: adding both SC and SCVR\n"
                              << ansi::reset;
                }
                else
                {
                    sitesToAdd = {siteArg};
                }

                for (const auto &site : sitesToAdd)
                {
                    try
                    {
                        manager.addBot(tokens[1], site);
                        std::cout << ansi::green << "Added " << tokens[1]
                                  << " [" << site << "]\n"
                                  << ansi::reset;
                    }
                    catch (const std::exception &e)
                    {
                        std::cout << ansi::red << "Error adding [" << site << "]: " << e.what() << "\n"
                                  << ansi::reset;
                    }
                }
            }
        }
        else if (cmd == "remove" || cmd == "rm")
        {
            if (tokens.size() < 2)
            {
                std::cout << ansi::red << "Usage: remove <username>\n"
                          << ansi::reset;
            }
            else
            {
                // Find the site for this username
                auto states = manager.getAllStates();
                bool found = false;
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        manager.removeBot(tokens[1], st.siteSlug);
                        std::cout << ansi::yellow << "Removed " << tokens[1] << "\n"
                                  << ansi::reset;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    std::cout << ansi::red << "Not found: " << tokens[1] << "\n"
                              << ansi::reset;
            }
        }
        else if (cmd == "start")
        {
            if (tokens.size() < 2 || tokens[1] == "*")
            {
                manager.startAllBots();
                std::cout << ansi::green << "Started all bots\n"
                          << ansi::reset;
            }
            else
            {
                auto states = manager.getAllStates();
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        manager.startBot(tokens[1], st.siteSlug);
                        std::cout << ansi::green << "Started " << tokens[1] << "\n"
                                  << ansi::reset;
                        break;
                    }
                }
            }
        }
        else if (cmd == "stop")
        {
            if (tokens.size() < 2 || tokens[1] == "*")
            {
                manager.stopAll();
                std::cout << ansi::yellow << "Stopped all bots\n"
                          << ansi::reset;
            }
            else
            {
                auto states = manager.getAllStates();
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        manager.stopBot(tokens[1], st.siteSlug);
                        std::cout << ansi::yellow << "Stopped " << tokens[1] << "\n"
                                  << ansi::reset;
                        break;
                    }
                }
            }
        }
        else if (cmd == "restart")
        {
            if (tokens.size() < 2)
            {
                std::cout << ansi::red << "Usage: restart <username>\n"
                          << ansi::reset;
            }
            else
            {
                auto states = manager.getAllStates();
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        manager.restartBot(tokens[1], st.siteSlug);
                        std::cout << ansi::cyan << "Restarted " << tokens[1] << "\n"
                                  << ansi::reset;
                        break;
                    }
                }
            }
        }
        else if (cmd == "move")
        {
            if (tokens.size() < 2)
            {
                std::cout << ansi::red << "Usage: move <username|*>\n"
                          << ansi::reset;
            }
            else if (tokens[1] == "*")
            {
                std::cout << ansi::cyan << "Moving all files to unprocessed...\n"
                          << ansi::reset;
                auto result = manager.moveAllFilesToUnprocessed();
                if (result.success)
                    std::cout << ansi::green << result.message << ansi::reset;
                else
                    std::cout << ansi::yellow << result.message << ansi::reset;
            }
            else
            {
                auto states = manager.getAllStates();
                bool found = false;
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        std::cout << ansi::cyan << "Moving files for " << tokens[1] << "...\n"
                                  << ansi::reset;
                        auto result = manager.moveFilesToUnprocessed(tokens[1], st.siteSlug);
                        if (result.success)
                            std::cout << ansi::green << "[" << st.siteSlug << "] "
                                      << tokens[1] << ": " << result.message << "\n"
                                      << ansi::reset;
                        else
                            std::cout << ansi::red << "[" << st.siteSlug << "] "
                                      << tokens[1] << ": " << result.message << "\n"
                                      << ansi::reset;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    std::cout << ansi::red << "Not found: " << tokens[1] << "\n"
                              << ansi::reset;
            }
        }
        else if (cmd == "resync")
        {
            if (tokens.size() < 2)
            {
                std::cout << ansi::red << "Usage: resync <username|*>\n"
                          << ansi::reset;
            }
            else if (tokens[1] == "*")
            {
                std::cout << ansi::cyan << "Resyncing all bots...\n"
                          << ansi::reset;
                auto result = manager.resyncAll();
                std::cout << ansi::green << result << ansi::reset;
            }
            else
            {
                auto states = manager.getAllStates();
                bool found = false;
                for (const auto &st : states)
                {
                    if (st.username == tokens[1])
                    {
                        std::cout << ansi::cyan << "Resyncing " << tokens[1] << "...\n"
                                  << ansi::reset;
                        auto result = manager.resyncBot(tokens[1], st.siteSlug);
                        std::cout << ansi::green << "[" << st.siteSlug << "] "
                                  << tokens[1] << ": " << result << "\n"
                                  << ansi::reset;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    std::cout << ansi::red << "Not found: " << tokens[1] << "\n"
                              << ansi::reset;
            }
        }
        else if (cmd == "cls" || cmd == "clear")
        {
#ifdef _WIN32
            system("cls");
#else
            system("clear");
#endif
        }
        else if (cmd == "save")
        {
            manager.saveConfig();
            std::cout << ansi::green << "Config saved\n"
                      << ansi::reset;
        }
        else if (cmd == "web")
        {
            if (webServer && webServer->isRunning())
            {
                if (tokens.size() > 1 && tokens[1] == "off")
                {
                    webServer->stop();
                    std::cout << ansi::yellow << "Web dashboard stopped\n"
                              << ansi::reset;
                }
                else
                {
                    std::cout << ansi::green << "🌐 Web dashboard is running:\n"
                              << "   Local:   " << webServer->getLocalUrl() << "\n"
                              << "   Network: " << webServer->getNetworkUrl() << "\n"
                              << ansi::reset;
                }
            }
            else
            {
                if (tokens.size() > 1 && tokens[1] == "off")
                {
                    std::cout << ansi::gray << "Web dashboard is already off\n"
                              << ansi::reset;
                }
                else
                {
                    if (!webServer)
                    {
                        webServer = std::make_unique<sm::WebServer>(manager, config, configStore);
                        webServer->setLogRingBuffer(g_logRingBuffer);
                    }
                    if (webServer->start())
                    {
                        std::cout << ansi::green << "🌐 Web dashboard started:\n"
                                  << "   Local:   " << webServer->getLocalUrl() << "\n"
                                  << "   Network: " << webServer->getNetworkUrl() << "\n"
                                  << ansi::reset;
                    }
                    else
                    {
                        std::cout << ansi::red << "Failed to start web server\n"
                                  << ansi::reset;
                    }
                }
            }
        }
        else
        {
            std::cout << ansi::red << "Unknown command: " << cmd
                      << " (type 'help')\n"
                      << ansi::reset;
        }
    }

    // Shutdown — join all recording threads BEFORE library cleanup
    // so av_write_trailer can finalize video files with proper duration
    std::cout << ansi::yellow << "\nStopping all bots..." << ansi::reset << std::endl;
    if (webServer)
        webServer->stop();
    manager.saveConfig();
    manager.shutdown(); // stops all groups + bots, joins threads (writes trailers)

    sm::HttpClient::globalCleanup();
    avformat_network_deinit();

    std::cout << ansi::green << "Goodbye!\n"
              << ansi::reset;
    return 0;
}
