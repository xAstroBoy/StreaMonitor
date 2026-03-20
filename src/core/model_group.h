#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Model Group (Cross-Register Cycling Engine)
//
// Groups multiple site/username pairings for the same model.
// ONE thread per group cycles through pairings:
//   check pairing[0] → if public → download
//                     → if offline → immediately check pairing[1]
//   ...
//   if ALL offline → sleep → restart cycle
//
// Mobile dual-recording: when the API reports a mobile broadcaster,
// ALL non-VR public pairings are recorded in parallel to capture
// both camera views (mobile portrait + desktop landscape).
// The API hint (apiMobileHint) TRIGGERS dual-recording, but the
// actual output FOLDER (PC vs Mobile/) is determined exclusively
// by stream resolution (isPortraitStream: h > w, ratio < 0.85).
// The site API is NEVER trusted for folder decisions.
// VR pairings are ALWAYS independent — never mobile, never parallel.
// ─────────────────────────────────────────────────────────────────

#include "core/site_plugin.h"
#include "config/config.h"
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>

// ── VR slug detection (used by ModelGroup + GUI) ────────────────
inline bool isVrSlug(const std::string &siteOrSlug)
{
    // VR slugs end with "VR" (e.g. "SCVR", "DCVR")
    // VR site names end with "VR" too (e.g. "StripChatVR", "DreamCamVR")
    return siteOrSlug.size() >= 2 &&
           siteOrSlug[siteOrSlug.size() - 2] == 'V' &&
           siteOrSlug[siteOrSlug.size() - 1] == 'R';
}

namespace sm
{

    // ── Pairing: one site + username combo ───────────────────────────
    struct GroupPairing
    {
        std::string site;     // site name (e.g. "StripChat")
        std::string username; // username on that site
        std::unique_ptr<SitePlugin> plugin;
        Status lastStatus = Status::Offline;
        bool lastMobile = false;
    };

    // ── Group state (for GUI observation) ───────────────────────────
    struct ModelGroupState
    {
        std::string groupName;
        bool running = false;
        bool recording = false;
        int activePairingIdx = -1; // which pairing is currently being checked/downloaded
        std::string activeSite;
        std::string activeUsername;
        Status activeStatus = Status::NotRunning;
        bool activeMobile = false;

        struct PairingState
        {
            std::string site;
            std::string username;
            Status lastStatus = Status::Offline;
            bool mobile = false;
        };
        std::vector<PairingState> pairings;
    };

    using GroupStateCallback = std::function<void(const ModelGroupState &)>;

    // ── Model Group ─────────────────────────────────────────────────
    class ModelGroup
    {
    public:
        explicit ModelGroup(const std::string &groupName);
        ~ModelGroup();

        // Non-copyable
        ModelGroup(const ModelGroup &) = delete;
        ModelGroup &operator=(const ModelGroup &) = delete;

        // ── Setup ───────────────────────────────────────────────────
        // Add a pairing. Must be called before start().
        // Creates the SitePlugin internally from the registry.
        bool addPairing(const std::string &site, const std::string &username);
        bool removePairing(const std::string &site, const std::string &username);
        bool setPrimaryPairing(size_t index);

        // ── Lifecycle ───────────────────────────────────────────────
        void start(const AppConfig &config);
        void stop();
        void requestQuit();
        bool isRunning() const;

        // ── State ───────────────────────────────────────────────────
        ModelGroupState getState() const;
        void setStateCallback(GroupStateCallback cb);
        const std::string &groupName() const { return groupName_; }

        // ── Access pairings (for display) ───────────────────────────
        size_t pairingCount() const;
        std::vector<std::pair<std::string, std::string>> getPairings() const;

        // ── Config ──────────────────────────────────────────────────
        void setSleepAllOffline(int seconds) { sleepAllOffline_ = seconds; }
        void setSleepAfterDownload(int seconds) { sleepAfterDownload_ = seconds; }

    private:
        void threadFunc(const AppConfig &config);
        bool downloadFrom(GroupPairing &pairing, const AppConfig &config);
        bool downloadFromWithToken(GroupPairing &pairing, const AppConfig &config,
                                   CancellationToken &token);
        void sleepInterruptible(int seconds);
        void updateGroupState();

        std::string groupName_;
        std::vector<GroupPairing> pairings_;

        std::unique_ptr<std::jthread> thread_;
        std::atomic<bool> running_{false};
        std::atomic<bool> quitting_{false};
        CancellationToken cancelToken_;
        std::mutex sleepMutex_;
        std::condition_variable sleepCv_;

        mutable std::mutex stateMutex_;
        ModelGroupState state_;
        GroupStateCallback stateCallback_;

        // Tuning
        int sleepAllOffline_ = 10;   // seconds to sleep when all pairings are offline
        int sleepAfterDownload_ = 2; // brief pause after a download ends before re-cycling
    };

} // namespace sm
