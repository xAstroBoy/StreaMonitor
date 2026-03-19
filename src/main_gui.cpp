// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — GUI Entry Point
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include "config/config.h"
#include "core/site_plugin.h"
#include "core/bot_manager.h"
#include "gui/gui_app.h"
#include "gui/imgui_log_sink.h"
#include "net/http_client.h"
#include "web/web_server.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/ringbuffer_sink.h>

#include <iostream>
#include <csignal>

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

static void signalHandler(int sig)
{
    spdlog::info("Received signal {}, shutting down...", sig);
    g_shutdown.store(true);
}

static std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> g_logRingBuffer;

static std::shared_ptr<sm::ImGuiLogSink> initLogging()
{
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("[%H:%M:%S] [%^%l%$] [%n] %v");

    auto file = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "streamonitor.log", 10 * 1024 * 1024, 3);
    file->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v");

    // ImGui log panel sink — buffers messages for the GUI to drain
    auto guiSink = std::make_shared<sm::ImGuiLogSink>();
    sm::ImGuiLogSink::setInstance(guiSink);

    // Ring buffer for the web dashboard /api/logs endpoint
    g_logRingBuffer = std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(1000);
    g_logRingBuffer->set_pattern("[%H:%M:%S] [%^%l%$] [%n] %v");

    auto logger = std::make_shared<spdlog::logger>("sm",
                                                   spdlog::sinks_init_list{console, file, guiSink, g_logRingBuffer});
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);

    spdlog::info("StreaMonitor v2.0 starting (GUI mode)");
    return guiSink;
}

int guiMain(int argc, char **argv)
{
    // Signal handlers
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    auto guiLogSink = initLogging();

    // Initialize FFmpeg
    avformat_network_init();

    // Initialize HTTP globally
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

    // Preview is on-demand only (CDN URL loaded when user opens bot detail panel)

    sm::ModelConfigStore configStore;
    std::filesystem::path configPath = "config.json";
    if (std::filesystem::exists(configPath))
    {
        configStore.load(configPath);
        spdlog::info("Loaded {} model configs", configStore.getAll().size());
    }

    // Print registered sites
    auto &registry = sm::SiteRegistry::instance();
    spdlog::info("Registered sites:");
    for (const auto &name : registry.siteNames())
    {
        spdlog::info("  - {}", name);
    }

    // Create bot manager
    sm::BotManager manager(config, configStore);

    // Load bots and rebuild cross-register groups from config
    manager.loadFromConfig();

    // Start all bots that were enabled
    manager.startAll();

    // Start cross-register groups
    manager.startAllGroups();

    // Start web server if enabled
    std::unique_ptr<sm::WebServer> webServer;
    // Always create the WebServer so the GUI can start/stop it on demand
    webServer = std::make_unique<sm::WebServer>(manager, config, configStore);
    webServer->setLogRingBuffer(g_logRingBuffer);
    if (config.webEnabled)
    {
        if (webServer->start())
        {
            spdlog::info("Web dashboard: {} | {}", webServer->getLocalUrl(), webServer->getNetworkUrl());
        }
    }

    // Run GUI — pass web server pointer so settings can control it
    sm::GuiApp gui(config, configStore, manager, guiLogSink, webServer.get());
    int exitCode = gui.run();

    // Cleanup — IMPORTANT: join all recording threads BEFORE cleaning up
    // FFmpeg/HTTP libraries, otherwise av_write_trailer won't complete
    // and video files will be left without duration metadata.
    spdlog::info("Shutting down...");
    if (webServer)
        webServer->stop();

    // Save config BEFORE shutdown so running bots keep running=true
    // (they should auto-start again on next launch).
    manager.saveConfig();

    manager.shutdown(); // stops all groups + bots, joins all threads (writes trailers)
    sm::HttpClient::globalCleanup();
    avformat_network_deinit();

    spdlog::info("Goodbye!");
    return exitCode;
}
