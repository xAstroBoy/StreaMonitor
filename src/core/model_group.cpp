// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Model Group implementation
// Cross-register cycling engine: one thread, many pairings
// ─────────────────────────────────────────────────────────────────

#include "core/model_group.h"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // Constructor / Destructor
    // ─────────────────────────────────────────────────────────────────
    ModelGroup::ModelGroup(const std::string &groupName)
        : groupName_(groupName)
    {
        state_.groupName = groupName;
    }

    ModelGroup::~ModelGroup()
    {
        requestQuit();
        if (thread_ && thread_->joinable())
        {
            thread_->request_stop();
            cancelToken_.cancel();
            thread_->join();
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Pairing management
    // ─────────────────────────────────────────────────────────────────
    bool ModelGroup::addPairing(const std::string &site, const std::string &username)
    {
        // If the cycling thread is running, stop it first so we don't
        // mutate pairings_ while the thread is iterating. (Fixes #48)
        bool wasRunning = running_.load();
        if (wasRunning)
            stop();

        // Don't add dupes
        for (const auto &p : pairings_)
        {
            if (p.site == site && p.username == username)
                return false;
        }

        auto plugin = SiteRegistry::instance().create(site, username);
        if (!plugin)
        {
            spdlog::error("[Group:{}] Unknown site '{}' for pairing '{}'",
                          groupName_, site, username);
            return false;
        }

        GroupPairing pairing;
        pairing.site = site;
        pairing.username = username;
        pairing.plugin = std::move(plugin);
        pairings_.push_back(std::move(pairing));

        updateGroupState();
        spdlog::info("[Group:{}] Added pairing: {} [{}]", groupName_, username, site);

        // Restart cycling if it was running before
        if (wasRunning && lastConfig_)
            start(*lastConfig_);

        return true;
    }

    bool ModelGroup::removePairing(const std::string &site, const std::string &username)
    {
        // If the cycling thread is running, stop it first so we don't
        // mutate pairings_ while the thread is iterating. (Fixes #48)
        bool wasRunning = running_.load();
        if (wasRunning)
            stop();

        auto it = std::find_if(pairings_.begin(), pairings_.end(),
                               [&](const GroupPairing &p)
                               {
                                   return p.site == site && p.username == username;
                               });
        if (it == pairings_.end())
            return false;

        if (it->plugin->isRunning())
            it->plugin->stop();

        pairings_.erase(it);
        updateGroupState();

        // Restart cycling if it was running before and pairings remain
        if (wasRunning && lastConfig_ && !pairings_.empty())
            start(*lastConfig_);

        return true;
    }

    bool ModelGroup::setPrimaryPairing(size_t index)
    {
        // If the cycling thread is running, stop it first so we don't
        // mutate pairings_ while the thread is iterating. (Fixes #48)
        bool wasRunning = running_.load();
        if (wasRunning)
            stop();
        if (index == 0 || index >= pairings_.size())
            return false;
        auto pairing = std::move(pairings_[index]);
        pairings_.erase(pairings_.begin() + index);
        pairings_.insert(pairings_.begin(), std::move(pairing));
        updateGroupState();
        spdlog::info("[Group:{}] Set primary: {} [{}]", groupName_,
                     pairings_[0].username, pairings_[0].site);

        // Restart cycling if it was running before
        if (wasRunning && lastConfig_)
            start(*lastConfig_);

        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────
    void ModelGroup::start(const AppConfig &config)
    {
        if (running_.load())
            return;

        if (pairings_.empty())
        {
            spdlog::warn("[Group:{}] No pairings, nothing to start", groupName_);
            return;
        }

        // Configure all plugins (HTTP setup, no thread)
        for (auto &p : pairings_)
            p.plugin->configure(config);

        lastConfig_ = &config; // remember for restart after pairing changes

        running_.store(true);
        quitting_.store(false);
        cancelToken_.reset();

        {
            std::lock_guard lock(stateMutex_);
            state_.running = true;
        }

        thread_ = std::make_unique<std::jthread>([this, &config](std::stop_token)
                                                 { threadFunc(config); });

        spdlog::info("[Group:{}] Started with {} pairings", groupName_, pairings_.size());
    }

    void ModelGroup::stop()
    {
        running_.store(false);
        cancelToken_.cancel();
        sleepCv_.notify_all(); // Instant wake from sleepInterruptible

        // Wait for the cycling thread to fully finish before touching state.
        // Without this, the thread continues accessing pairings_/state_ after
        // stop() returns, causing use-after-free on destruction. (Fixes #48)
        if (thread_ && thread_->joinable())
        {
            thread_->request_stop();
            thread_->join();
        }
        thread_.reset();

        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.recording = false;
            state_.activeStatus = Status::NotRunning;
            state_.activePairingIdx = -1;
        }

        emitStateChange();

        spdlog::info("[Group:{}] Stopped", groupName_);
    }

    void ModelGroup::requestQuit()
    {
        quitting_.store(true);
        running_.store(false);
        cancelToken_.cancel();
        sleepCv_.notify_all(); // Instant wake from sleepInterruptible
    }

    bool ModelGroup::isRunning() const { return running_.load(); }

    // ─────────────────────────────────────────────────────────────────
    // State access
    // ─────────────────────────────────────────────────────────────────
    ModelGroupState ModelGroup::getState() const
    {
        std::lock_guard lock(stateMutex_);
        return state_;
    }

    void ModelGroup::setStateCallback(GroupStateCallback cb)
    {
        std::lock_guard lock(stateMutex_);
        stateCallback_ = std::move(cb);
    }

    size_t ModelGroup::pairingCount() const
    {
        std::lock_guard lock(pairingsMutex_);
        return pairings_.size();
    }

    std::vector<std::pair<std::string, std::string>> ModelGroup::getPairings() const
    {
        std::lock_guard lock(pairingsMutex_);
        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(pairings_.size());
        for (const auto &p : pairings_)
            result.emplace_back(p.site, p.username);
        return result;
    }

    void ModelGroup::updateGroupState()
    {
        std::lock_guard pLock(pairingsMutex_);
        std::lock_guard sLock(stateMutex_);
        state_.pairings.clear();
        for (const auto &p : pairings_)
        {
            ModelGroupState::PairingState ps;
            ps.site = p.site;
            ps.username = p.username;
            ps.lastStatus = p.lastStatus;
            ps.mobile = p.lastMobile;
            state_.pairings.push_back(std::move(ps));
        }
    }

    void ModelGroup::emitStateChange()
    {
        GroupStateCallback cb;
        ModelGroupState stateCopy;
        {
            std::lock_guard lock(stateMutex_);
            cb = stateCallback_;
            stateCopy = state_;
        }
        if (cb)
            cb(stateCopy);
    }

    // ─────────────────────────────────────────────────────────────────
    // THE CYCLING THREAD
    //
    // Algorithm:
    //   while running:
    //     for each pairing:
    //       check status (no delay between pairings)
    //       if PUBLIC → download from this one
    //       if anything else → move to next pairing immediately
    //     end for
    //     if nothing was live → sleep(sleepAllOffline_)
    //   end while
    // ─────────────────────────────────────────────────────────────────
    void ModelGroup::threadFunc(const AppConfig &config)
    {
        try
        {
            {
                std::lock_guard pLock(pairingsMutex_);
                spdlog::info("[Group:{}] Cycling thread started ({} pairings)",
                             groupName_, pairings_.size());
            }

            while (running_.load() && !quitting_.load())
            {
                bool anyLive = false;

                size_t numPairings;
                {
                    std::lock_guard pLock(pairingsMutex_);
                    numPairings = pairings_.size();
                }
                for (size_t i = 0; i < numPairings && running_.load() && !quitting_.load(); i++)
                {
                    auto &pairing = pairings_[i];

                    // Update active pairing indicator
                    {
                        std::lock_guard lock(stateMutex_);
                        state_.activePairingIdx = (int)i;
                        state_.activeSite = pairing.site;
                        state_.activeUsername = pairing.username;
                    }

                    // Check status — NO DELAY between pairings
                    Status status;
                    try
                    {
                        status = pairing.plugin->checkStatus();
                    }
                    catch (const std::exception &e)
                    {
                        spdlog::error("[Group:{}] {} [{}] checkStatus exception: {}",
                                      groupName_, pairing.username, pairing.site, e.what());
                        status = Status::RateLimit;
                    }

                    {
                        std::lock_guard pLock(pairingsMutex_);
                        pairing.lastStatus = status;
                        // Use API mobile hint for dual-recording trigger.
                        // Actual mobile detection is from stream resolution.
                        pairing.lastMobile = pairing.plugin->apiMobileHint();
                    }

                    // Update state for GUI
                    {
                        std::lock_guard lock(stateMutex_);
                        state_.activeStatus = status;
                        state_.activeMobile = pairing.lastMobile;
                        if (i < state_.pairings.size())
                        {
                            state_.pairings[i].lastStatus = status;
                            state_.pairings[i].mobile = pairing.lastMobile;
                        }
                    }
                    emitStateChange();

                    spdlog::debug("[Group:{}] {} [{}] → {}",
                                  groupName_, pairing.username, pairing.site,
                                  statusToString(status));

                    // ── PUBLIC → download from this pairing ─────────────
                    if (status == Status::Public)
                    {
                        anyLive = true;

                        // ── Mobile dual-recording ───────────────────────
                        // If this pairing's API reports mobile AND non-VR,
                        // check all other non-VR pairings — if any are also
                        // PUBLIC, download from them in parallel to capture
                        // both camera views (mobile + desktop).
                        // VR pairings are ALWAYS left alone — never
                        // participate in dual-recording.
                        // NOTE: We use apiMobileHint() (from the site API)
                        // to TRIGGER dual-recording because we need to know
                        // before the stream opens.  The actual output FOLDER
                        // (PC vs Mobile/) is determined by stream resolution
                        // in the recorder's first-open detection.
                        bool mobileMulti = pairing.lastMobile && !isVrSlug(pairing.site);
                        std::vector<std::unique_ptr<std::jthread>> parallelThreads;
                        std::vector<std::unique_ptr<CancellationToken>> parallelTokens;
                        bool startedAsMobile = pairing.lastMobile; // API hint at launch time

                        if (mobileMulti)
                        {
                            spdlog::info("[Group:{}] {} [{}] is MOBILE — checking other pairings for dual recording",
                                         groupName_, pairing.username, pairing.site);

                            // Snapshot pairing count under lock (same pattern as outer loop)
                            const size_t dualCount = numPairings; // already snapshotted above
                            for (size_t j = 0; j < dualCount && running_.load() && !quitting_.load(); j++)
                            {
                                if (j == i)
                                    continue;
                                auto &other = pairings_[j];
                                if (isVrSlug(other.site))
                                    continue; // VR always independent

                                Status otherStatus;
                                try
                                {
                                    otherStatus = other.plugin->checkStatus();
                                }
                                catch (...)
                                {
                                    continue;
                                }
                                {
                                    std::lock_guard pLock(pairingsMutex_);
                                    other.lastStatus = otherStatus;
                                    other.lastMobile = other.plugin->apiMobileHint();
                                }

                                // Update GUI state for this pairing
                                {
                                    std::lock_guard lock(stateMutex_);
                                    if (j < state_.pairings.size())
                                    {
                                        state_.pairings[j].lastStatus = otherStatus;
                                        state_.pairings[j].mobile = other.lastMobile;
                                    }
                                }

                                if (otherStatus == Status::Public)
                                {
                                    spdlog::info("[Group:{}] {} [{}] also PUBLIC — starting parallel download",
                                                 groupName_, other.username, other.site);

                                    auto token = std::make_unique<CancellationToken>();
                                    auto *tokenPtr = token.get();
                                    size_t idx = j;
                                    parallelTokens.push_back(std::move(token));
                                    parallelThreads.push_back(std::make_unique<std::jthread>(
                                        [this, idx, &config, tokenPtr]()
                                        {
                                            downloadFromWithToken(pairings_[idx], config, *tokenPtr);
                                        }));
                                }
                            }
                        }

                        spdlog::info("[Group:{}] {} [{}] is LIVE{} — downloading",
                                     groupName_, pairing.username, pairing.site,
                                     mobileMulti ? " (MOBILE, dual-recording)" : "");

                        {
                            std::lock_guard lock(stateMutex_);
                            state_.recording = true;
                        }
                        emitStateChange();

                        // Download from this pairing (blocks until stream ends or error)
                        downloadFrom(pairing, config);

                        {
                            std::lock_guard lock(stateMutex_);
                            state_.recording = false;
                        }
                        emitStateChange();

                        // ── Mobile→PC transition ────────────────────────
                        // After download ends, re-check mobile status. If the
                        // stream started as mobile but is now PC (landscape),
                        // cancel all parallel recorders — we only need the
                        // primary. The primary stays as favorite (index 0).
                        if (startedAsMobile && !parallelThreads.empty())
                        {
                            bool nowMobile = false;
                            try
                            {
                                nowMobile = pairing.plugin->isMobile();
                            }
                            catch (...)
                            {
                            }

                            if (!nowMobile)
                            {
                                spdlog::info("[Group:{}] {} [{}] switched from MOBILE to PC — "
                                             "cancelling {} parallel recorder(s), keeping primary",
                                             groupName_, pairing.username, pairing.site,
                                             parallelThreads.size());
                                for (auto &t : parallelTokens)
                                    t->cancel();
                            }
                        }

                        // Wait for parallel downloads to finish.
                        // If group is being stopped, cancel the parallel tokens first.
                        if (!parallelThreads.empty())
                        {
                            if (!running_.load() || quitting_.load())
                            {
                                for (auto &t : parallelTokens)
                                    t->cancel();
                            }
                            spdlog::info("[Group:{}] Waiting for {} parallel download(s) to finish",
                                         groupName_, parallelThreads.size());
                            parallelThreads.clear(); // join all
                            parallelTokens.clear();
                        }

                        // Brief pause after download, then continue cycling
                        sleepInterruptible(sleepAfterDownload_.load());
                        break; // restart cycle from pairing[0]
                    }

                    // ── NOTEXIST / DELETED → skip this pairing ──────────
                    if (status == Status::NotExist || status == Status::Deleted)
                    {
                        spdlog::warn("[Group:{}] {} [{}] does not exist, skipping in cycle",
                                     groupName_, pairing.username, pairing.site);
                        continue;
                    }

                    // ── RATELIMIT → tiny pause then continue ────────────
                    if (status == Status::RateLimit || status == Status::Cloudflare)
                    {
                        sleepInterruptible(2); // brief backoff, then next pairing
                        continue;
                    }

                    // ── OFFLINE / PRIVATE / anything else → next pairing immediately
                    // No delay! That's the whole point.
                }

                // ── All pairings checked, none were live ────────────────
                if (!anyLive && running_.load() && !quitting_.load())
                {
                    {
                        std::lock_guard lock(stateMutex_);
                        state_.activePairingIdx = -1;
                        state_.activeStatus = Status::Offline;
                    }
                    emitStateChange();

                    const int offlineSleep = sleepAllOffline_.load();
                    spdlog::debug("[Group:{}] All pairings offline, sleeping {}s",
                                  groupName_, offlineSleep);
                    sleepInterruptible(offlineSleep);
                }
            }

            // Cleanup
            {
                std::lock_guard lock(stateMutex_);
                state_.running = false;
                state_.recording = false;
                state_.activeStatus = Status::NotRunning;
                state_.activePairingIdx = -1;
            }
            emitStateChange();

            spdlog::info("[Group:{}] Cycling thread exiting", groupName_);
        }
        catch (const std::exception &e)
        {
            spdlog::error("[Group:{}] FATAL thread exception: {}", groupName_, e.what());
        }
        catch (...)
        {
            spdlog::error("[Group:{}] FATAL unknown thread exception", groupName_);
        }

        // Ensure state is cleaned up even after exception
        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.recording = false;
            state_.activeStatus = Status::NotRunning;
            state_.activePairingIdx = -1;
        }
        running_.store(false);
    }

    // ─────────────────────────────────────────────────────────────────
    // Download from a specific pairing
    // ─────────────────────────────────────────────────────────────────
    bool ModelGroup::downloadFrom(GroupPairing &pairing, const AppConfig &config)
    {
        cancelToken_.reset();
        return downloadFromWithToken(pairing, config, cancelToken_);
    }

    bool ModelGroup::downloadFromWithToken(GroupPairing &pairing, const AppConfig &config,
                                           CancellationToken &token)
    {
        // Get video URL
        std::string videoUrl;
        try
        {
            videoUrl = pairing.plugin->getVideoUrl();
        }
        catch (const std::exception &e)
        {
            spdlog::error("[Group:{}] {} [{}] getVideoUrl failed: {}",
                          groupName_, pairing.username, pairing.site, e.what());
            return false;
        }

        if (videoUrl.empty())
        {
            spdlog::warn("[Group:{}] {} [{}] empty video URL",
                         groupName_, pairing.username, pairing.site);
            return false;
        }

        // Generate output path: downloads/<username> [<slug>]/<number>.<ext>
        std::string slug = SiteRegistry::instance().nameToSlug(pairing.site);
        if (slug.empty())
            slug = pairing.site;

        // Helper: generate next output path for a given base dir
        auto generateNextPath = [&](bool mobile) -> std::string
        {
            auto baseDir = config.downloadsDir /
                           (pairing.username + " [" + slug + "]");
            if (mobile)
                baseDir /= "Mobile";
            std::filesystem::create_directories(baseDir);

            const auto &fmt2 = getFormatInfo(config.container);
            int num = 1;
            std::error_code ec2;
            for (const auto &entry : std::filesystem::directory_iterator(baseDir, ec2))
            {
                if (!entry.is_regular_file())
                    continue;
                auto name = entry.path().stem().string();
                try
                {
                    int n = std::stoi(name);
                    if (n >= num)
                        num = n + 1;
                }
                catch (...)
                {
                }
            }
            return (baseDir / (std::to_string(num) + fmt2.extension)).string();
        };

        // Start ALL recordings in the non-mobile (PC) folder.
        // The API's isMobile flag tells us the broadcaster's DEVICE,
        // not the stream orientation — ALL sites report the same flag.
        // The resolution change callback will detect portrait (h > w)
        // on the first packet and move the mobile view to Mobile/.
        // VR sites also always start here (VR is never mobile).
        std::string outputPath = generateNextPath(false);
        spdlog::info("[Group:{}] Recording {} [{}] to: {}",
                     groupName_, pairing.username, pairing.site, outputPath);

        // Record using HLS recorder
        HLSRecorder recorder(config);

        // Resolution change callback — when stream resolution changes
        // (model switched mobile↔desktop), close current file and start
        // a new one in the appropriate folder.
        // VR sites are NEVER mobile — ignore portrait detection for them.
        recorder.setResolutionChangeCallback([&](const ResolutionInfo &ri) -> std::string
                                             {
            bool mobile = ri.isMobile && !isVrSlug(pairing.site);
            spdlog::info("[Group:{}] Resolution {}: {}x{} (mobile={}, via {})",
                         groupName_,
                         ri.source == ResolutionInfo::Source::MasterPlaylist
                             ? "from master playlist" : "change",
                         ri.width, ri.height, mobile,
                         ri.source == ResolutionInfo::Source::MasterPlaylist
                             ? "master-playlist" : "codec-params");
            return generateNextPath(mobile); });

        // Pass masterUrl for SegmentFeeder orientation monitoring
        std::string mUrl = pairing.plugin->masterUrl();
        auto result = recorder.record(videoUrl, outputPath, token,
                                      config.userAgent, "", {}, {}, mUrl);

        // Post-recording validation
        std::error_code ec;
        if (std::filesystem::exists(outputPath, ec))
        {
            auto fileSize = std::filesystem::file_size(outputPath, ec);
            if (fileSize == 0)
            {
                std::filesystem::remove(outputPath, ec);
                spdlog::warn("[Group:{}] Removed zero-byte output", groupName_);
                return false;
            }
            spdlog::info("[Group:{}] Recording complete: {} ({} bytes)",
                         groupName_, outputPath, fileSize);
        }

        return result.success;
    }

    // ─────────────────────────────────────────────────────────────────
    // Interruptible sleep
    // ─────────────────────────────────────────────────────────────────
    void ModelGroup::sleepInterruptible(int seconds)
    {
        std::unique_lock lock(sleepMutex_);
        sleepCv_.wait_for(lock, std::chrono::seconds(seconds),
                          [this]
                          { return !running_.load() || quitting_.load(); });
    }

} // namespace sm
