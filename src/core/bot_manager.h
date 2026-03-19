#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Bot Manager
// Central orchestrator for all model bots
// ─────────────────────────────────────────────────────────────────

#include "core/site_plugin.h"
#include "core/model_group.h"
#include "config/config.h"
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <unordered_map>

namespace sm
{

    // ── Manager event ───────────────────────────────────────────────
    struct ManagerEvent
    {
        enum Type
        {
            BotAdded,
            BotRemoved,
            BotStatusChanged,
            BotRecordingStarted,
            BotRecordingStopped,
            ConfigSaved,
            Error
        };
        Type type;
        std::string botId;
        std::string message;
    };
    using ManagerEventCallback = std::function<void(const ManagerEvent &)>;

    // ── Bot Manager ─────────────────────────────────────────────────
    class BotManager
    {
    public:
        explicit BotManager(AppConfig &config, ModelConfigStore &configStore);
        ~BotManager();

        // Non-copyable
        BotManager(const BotManager &) = delete;
        BotManager &operator=(const BotManager &) = delete;

        // ── Initialization ──────────────────────────────────────────
        void loadFromConfig(); // creates bots from config store
        void startAll();       // start bots marked as running (for launch)
        void startAllBots();   // start ALL bots and persist running=true
        void stopAll();        // stop all bots and persist running=false
        void shutdown();       // graceful shutdown, cancel all recordings

        // ── Bot management ──────────────────────────────────────────
        bool addBot(const std::string &username, const std::string &site,
                    bool autoStart = true);
        bool removeBot(const std::string &username, const std::string &site = "");
        bool startBot(const std::string &username, const std::string &site = "");
        bool stopBot(const std::string &username, const std::string &site = "");
        bool restartBot(const std::string &username, const std::string &site = "");
        bool editBot(const std::string &oldUsername, const std::string &oldSite,
                     const std::string &newUsername, const std::string &newSite);

        // ── File / state operations (Python parity) ─────────────────
        struct MoveResult
        {
            bool success = false;
            std::string message;
        };
        MoveResult moveFilesToUnprocessed(const std::string &username,
                                          const std::string &site = "");
        MoveResult moveAllFilesToUnprocessed();
        std::string resyncBot(const std::string &username,
                              const std::string &site = "");
        std::string resyncAll();

        // ── Queries ─────────────────────────────────────────────────
        std::vector<BotState> getAllStates() const;
        // Non-blocking version: returns nullopt if mutex is busy (GUI won't freeze)
        std::optional<std::vector<BotState>> tryGetAllStates() const;
        std::optional<BotState> getBotState(const std::string &username,
                                            const std::string &site = "") const;
        size_t botCount() const;
        size_t recordingCount() const;
        size_t onlineCount() const;

        // ── Continuous preview (forwarded to SitePlugin) ─────────────
        bool consumePreview(const std::string &username, const std::string &site,
                            PreviewFrame &out, uint64_t &lastVersion);
        bool waitForPreview(const std::string &username, const std::string &site,
                            PreviewFrame &out, uint64_t &lastVersion, int timeoutMs);

        // ── Audio forwarding (forwarded to SitePlugin) ───────────────
        using AudioDataCallback = std::function<void(const float *samples, size_t frameCount)>;
        void setAudioDataCallback(const std::string &username, const std::string &site,
                                  AudioDataCallback cb);
        void clearAudioDataCallback(const std::string &username, const std::string &site);

        // ── Events ──────────────────────────────────────────────────
        void setEventCallback(ManagerEventCallback cb);

        // ── Config persistence ──────────────────────────────────────
        void saveConfig();

        // ── Available sites ─────────────────────────────────────────
        std::vector<std::string> availableSites() const;

        // ── Cross-register groups (cycling multi-site tracking) ───────
        bool createCrossRegisterGroup(const std::string &groupName,
                                      const std::vector<std::pair<std::string, std::string>> &members);
        bool removeCrossRegisterGroup(const std::string &groupName);
        bool addToCrossRegisterGroup(const std::string &groupName,
                                     const std::string &site, const std::string &username);
        bool removeFromCrossRegisterGroup(const std::string &groupName,
                                          const std::string &site, const std::string &username);
        bool setPrimaryPairing(const std::string &groupName, size_t pairingIndex);
        std::vector<CrossRegisterGroup> getCrossRegisterGroups() const;
        std::optional<CrossRegisterGroup> getGroupForBot(const std::string &username,
                                                         const std::string &site) const;

        // Start/stop a group's cycling thread
        bool startGroup(const std::string &groupName);
        bool stopGroup(const std::string &groupName);
        void startAllGroups();
        void stopAllGroups();

        // Get group states for GUI
        std::vector<ModelGroupState> getAllGroupStates() const;

        // ── Disk usage ──────────────────────────────────────────────
        struct DiskUsageInfo
        {
            uint64_t totalBytes = 0;
            uint64_t freeBytes = 0;
            uint64_t downloadDirBytes = 0;
            int fileCount = 0;
        };
        DiskUsageInfo getDiskUsage() const;

    private:
        struct BotEntry
        {
            std::unique_ptr<SitePlugin> plugin;
            bool autoStart = true;
        };

        SitePlugin *findBot(const std::string &username, const std::string &site = "");
        const SitePlugin *findBot(const std::string &username, const std::string &site = "") const;
        void emitEvent(ManagerEvent::Type type, const std::string &botId,
                       const std::string &msg = "");

        AppConfig &config_;
        ModelConfigStore &configStore_;
        mutable std::mutex mutex_;
        std::vector<BotEntry> bots_;
        std::vector<CrossRegisterGroup> crossRegisterGroups_;
        std::vector<std::unique_ptr<ModelGroup>> groups_;
        std::mutex eventMutex_; // protects eventCb_
        ManagerEventCallback eventCb_;
        bool isShutdown_ = false;
    };

} // namespace sm
