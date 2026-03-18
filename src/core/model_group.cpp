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
        return true;
    }

    bool ModelGroup::removePairing(const std::string &site, const std::string &username)
    {
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
        return true;
    }

    bool ModelGroup::setPrimaryPairing(size_t index)
    {
        if (index == 0 || index >= pairings_.size())
            return false;
        auto pairing = std::move(pairings_[index]);
        pairings_.erase(pairings_.begin() + index);
        pairings_.insert(pairings_.begin(), std::move(pairing));
        updateGroupState();
        spdlog::info("[Group:{}] Set primary: {} [{}]", groupName_,
                     pairings_[0].username, pairings_[0].site);
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

        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.recording = false;
            state_.activeStatus = Status::NotRunning;
            state_.activePairingIdx = -1;
        }

        if (stateCallback_)
            stateCallback_(state_);

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
        stateCallback_ = std::move(cb);
    }

    size_t ModelGroup::pairingCount() const
    {
        return pairings_.size();
    }

    std::vector<std::pair<std::string, std::string>> ModelGroup::getPairings() const
    {
        std::vector<std::pair<std::string, std::string>> result;
        result.reserve(pairings_.size());
        for (const auto &p : pairings_)
            result.emplace_back(p.site, p.username);
        return result;
    }

    void ModelGroup::updateGroupState()
    {
        std::lock_guard lock(stateMutex_);
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
        spdlog::info("[Group:{}] Cycling thread started ({} pairings)",
                     groupName_, pairings_.size());

        while (running_.load() && !quitting_.load())
        {
            bool anyLive = false;

            for (size_t i = 0; i < pairings_.size() && running_.load() && !quitting_.load(); i++)
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

                pairing.lastStatus = status;
                pairing.lastMobile = pairing.plugin->isMobile();

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
                if (stateCallback_)
                    stateCallback_(state_);

                spdlog::debug("[Group:{}] {} [{}] → {}",
                              groupName_, pairing.username, pairing.site,
                              statusToString(status));

                // ── PUBLIC → download from this pairing ─────────────
                if (status == Status::Public)
                {
                    anyLive = true;
                    spdlog::info("[Group:{}] {} [{}] is LIVE — downloading",
                                 groupName_, pairing.username, pairing.site);

                    {
                        std::lock_guard lock(stateMutex_);
                        state_.recording = true;
                    }
                    if (stateCallback_)
                        stateCallback_(state_);

                    // Download (blocks until stream ends or error)
                    downloadFrom(pairing, config);

                    {
                        std::lock_guard lock(stateMutex_);
                        state_.recording = false;
                    }
                    if (stateCallback_)
                        stateCallback_(state_);

                    // Brief pause after download, then continue cycling
                    sleepInterruptible(sleepAfterDownload_);
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
                if (stateCallback_)
                    stateCallback_(state_);

                spdlog::debug("[Group:{}] All pairings offline, sleeping {}s",
                              groupName_, sleepAllOffline_);
                sleepInterruptible(sleepAllOffline_);
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
        if (stateCallback_)
            stateCallback_(state_);

        spdlog::info("[Group:{}] Cycling thread exiting", groupName_);
    }

    // ─────────────────────────────────────────────────────────────────
    // Download from a specific pairing
    // ─────────────────────────────────────────────────────────────────
    bool ModelGroup::downloadFrom(GroupPairing &pairing, const AppConfig &config)
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

        std::string outputPath = generateNextPath(pairing.plugin->isMobile());
        spdlog::info("[Group:{}] Recording {} [{}] to: {}",
                     groupName_, pairing.username, pairing.site, outputPath);

        // Record using HLS recorder
        HLSRecorder recorder(config);

        // Resolution change callback — when stream resolution changes
        // (model switched mobile↔desktop), close current file and start
        // a new one in the appropriate folder.
        recorder.setResolutionChangeCallback([&](const ResolutionInfo &ri) -> std::string
                                             {
            spdlog::info("[Group:{}] Resolution change: {}x{} (mobile={})",
                         groupName_, ri.width, ri.height, ri.isMobile);
            return generateNextPath(ri.isMobile); });

        cancelToken_.reset();
        auto result = recorder.record(videoUrl, outputPath, cancelToken_, config.userAgent);

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
