// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Bot Manager implementation
// ─────────────────────────────────────────────────────────────────

#include "core/bot_manager.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <algorithm>
#include <filesystem>
#include <set>
#include <unordered_map>

namespace sm
{

    BotManager::BotManager(AppConfig &config, ModelConfigStore &configStore)
        : config_(config), configStore_(configStore)
    {
    }

    BotManager::~BotManager()
    {
        shutdown();
    }

    // ─────────────────────────────────────────────────────────────────
    // Initialization
    // ─────────────────────────────────────────────────────────────────
    void BotManager::loadFromConfig()
    {
        std::lock_guard lock(mutex_);

        auto models = configStore_.getAll();
        spdlog::info("Loading {} models from config", models.size());

        for (const auto &model : models)
        {
            auto plugin = SiteRegistry::instance().create(model.site, model.username);
            if (!plugin)
            {
                spdlog::warn("Unknown site '{}' for model '{}', skipping",
                             model.site, model.username);
                continue;
            }

            if (model.gender != Gender::Unknown)
                plugin->setGender(model.gender);
            if (!model.country.empty())
                plugin->setCountry(model.country);

            // Configure HTTP + populate websiteUrl for table display
            plugin->configure(config_);
            plugin->setStateCallback([this](const BotState &state)
                                     { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });

            BotEntry entry;
            entry.plugin = std::move(plugin);
            entry.autoStart = model.running;
            bots_.push_back(std::move(entry));
        }

        spdlog::info("Created {} bots", bots_.size());

        // Rebuild cross-register groups from per-model config entries
        {
            std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> groupMembers;
            for (const auto &model : models)
            {
                if (!model.crossRegisterGroup.empty())
                    groupMembers[model.crossRegisterGroup].emplace_back(model.site, model.username);
            }
            for (auto &[name, members] : groupMembers)
            {
                CrossRegisterGroup grp;
                grp.groupName = name;
                grp.members = std::move(members);
                crossRegisterGroups_.push_back(std::move(grp));

                // Create ModelGroup for cycling
                auto mg = std::make_unique<ModelGroup>(name);
                for (const auto &[site, user] : crossRegisterGroups_.back().members)
                    mg->addPairing(site, user);
                groups_.push_back(std::move(mg));
            }
            if (!crossRegisterGroups_.empty())
                spdlog::info("Rebuilt {} cross-register groups from config", crossRegisterGroups_.size());
        }
    }

    void BotManager::startAll()
    {
        std::lock_guard lock(mutex_);
        int started = 0;
        for (auto &entry : bots_)
        {
            if (entry.autoStart && !entry.plugin->isRunning())
            {
                entry.plugin->setStateCallback([this](const BotState &state)
                                               { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });
                entry.plugin->start(config_);
                started++;
            }
        }
        spdlog::info("Started {} bots", started);
    }

    void BotManager::startAllBots()
    {
        std::lock_guard lock(mutex_);
        int started = 0;
        for (auto &entry : bots_)
        {
            if (!entry.plugin->isRunning())
            {
                entry.plugin->setStateCallback([this](const BotState &state)
                                               { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });
                entry.plugin->start(config_);
                started++;
            }
        }
        // Persist running=true for all bots
        for (const auto &entry : bots_)
        {
            auto state = entry.plugin->getState();
            configStore_.updateRunning(state.username, state.siteSlug, true);
        }
        configStore_.save();
        spdlog::info("Started all {} bots", started);
    }

    void BotManager::stopAll()
    {
        std::lock_guard lock(mutex_);
        for (auto &entry : bots_)
        {
            if (entry.plugin->isRunning())
                entry.plugin->stop();
        }
        // Persist running=false for all bots
        for (const auto &entry : bots_)
        {
            auto state = entry.plugin->getState();
            configStore_.updateRunning(state.username, state.siteSlug, false);
        }
        configStore_.save();
        spdlog::info("Stopped all bots");
    }

    void BotManager::shutdown()
    {
        if (isShutdown_)
            return;
        isShutdown_ = true;
        spdlog::info("Shutting down bot manager...");
        {
            std::lock_guard lock(mutex_);
            // Stop all groups first
            for (auto &g : groups_)
                g->requestQuit();
            for (auto &entry : bots_)
                entry.plugin->requestQuit();
        }
        // Give recording threads time to write trailers
        std::this_thread::sleep_for(std::chrono::seconds(4));
        {
            std::lock_guard lock(mutex_);
            groups_.clear();
            bots_.clear(); // destructors join threads
        }
        spdlog::info("Bot manager shut down");
    }

    // ─────────────────────────────────────────────────────────────────
    // Bot management
    // ─────────────────────────────────────────────────────────────────
    bool BotManager::addBot(const std::string &username, const std::string &site,
                            bool autoStart)
    {
        std::lock_guard lock(mutex_);

        // Check for duplicate
        if (findBot(username, site))
        {
            spdlog::warn("Bot already exists: {} [{}]", username, site);
            return false;
        }

        auto plugin = SiteRegistry::instance().create(site, username);
        if (!plugin)
        {
            spdlog::error("Unknown site: {}", site);
            return false;
        }

        plugin->setStateCallback([this](const BotState &state)
                                 { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });

        // Add to config
        ModelConfig mc;
        mc.site = site;
        mc.username = username;
        mc.running = autoStart;
        configStore_.add(mc);
        configStore_.save();

        BotEntry entry;
        entry.plugin = std::move(plugin);
        entry.autoStart = autoStart;

        if (autoStart)
            entry.plugin->start(config_);

        bots_.push_back(std::move(entry));
        emitEvent(ManagerEvent::BotAdded, username + "_" + SiteRegistry::instance().nameToSlug(site), username);

        return true;
    }

    bool BotManager::removeBot(const std::string &username, const std::string &site)
    {
        std::lock_guard lock(mutex_);

        auto it = std::find_if(bots_.begin(), bots_.end(),
                               [&](const BotEntry &e)
                               {
                                   return e.plugin->username() == username &&
                                          (site.empty() || e.plugin->siteName() == site ||
                                           e.plugin->siteSlug() == site);
                               });

        if (it == bots_.end())
            return false;

        it->plugin->requestQuit();
        std::string slug = it->plugin->siteSlug();
        bots_.erase(it);

        configStore_.remove(username, site);
        configStore_.save();

        emitEvent(ManagerEvent::BotRemoved, username + "_" + slug, username);
        return true;
    }

    bool BotManager::startBot(const std::string &username, const std::string &site)
    {
        std::lock_guard lock(mutex_);
        auto *bot = findBot(username, site);
        if (!bot || bot->isRunning())
            return false;

        bot->setStateCallback([this](const BotState &state)
                              { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });
        bot->start(config_);

        // Persist running state so bot auto-starts on next launch
        std::string slug = bot->siteSlug();
        configStore_.updateRunning(username, slug, true);
        configStore_.updateStatus(username, slug, bot->getState().status, false);
        configStore_.save();

        return true;
    }

    bool BotManager::stopBot(const std::string &username, const std::string &site)
    {
        std::lock_guard lock(mutex_);
        auto *bot = findBot(username, site);
        if (!bot || !bot->isRunning())
            return false;
        bot->stop();

        // Persist stopped state so bot does NOT auto-start on next launch
        std::string slug = bot->siteSlug();
        configStore_.updateRunning(username, slug, false);
        configStore_.updateStatus(username, slug, bot->getState().status, false);
        configStore_.save();

        return true;
    }

    bool BotManager::editBot(const std::string &oldUsername, const std::string &oldSite,
                             const std::string &newUsername, const std::string &newSite)
    {
        std::lock_guard lock(mutex_);

        // Find existing bot
        auto it = std::find_if(bots_.begin(), bots_.end(),
                               [&](const BotEntry &e)
                               {
                                   return e.plugin->username() == oldUsername &&
                                          (oldSite.empty() || e.plugin->siteName() == oldSite ||
                                           e.plugin->siteSlug() == oldSite);
                               });
        if (it == bots_.end())
            return false;

        bool wasRunning = it->plugin->isRunning();
        std::string oldSlug = it->plugin->siteSlug();

        // If site changed, we need to recreate the plugin entirely
        bool siteChanged = !newSite.empty() && newSite != it->plugin->siteName() && newSite != it->plugin->siteSlug();
        bool usernameChanged = !newUsername.empty() && newUsername != oldUsername;

        if (siteChanged)
        {
            // Stop old bot
            if (wasRunning)
                it->plugin->requestQuit();

            // Remove old config entry
            configStore_.remove(oldUsername, oldSlug);

            // Erase old bot
            bots_.erase(it);

            // Create new bot with new site
            std::string finalUser = usernameChanged ? newUsername : oldUsername;
            auto plugin = SiteRegistry::instance().create(newSite, finalUser);
            if (!plugin)
                return false;

            plugin->setStateCallback([this](const BotState &state)
                                     { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });

            ModelConfig mc;
            mc.site = newSite;
            mc.username = finalUser;
            mc.running = wasRunning;
            configStore_.add(mc);
            configStore_.save();

            BotEntry entry;
            entry.plugin = std::move(plugin);
            entry.autoStart = wasRunning;
            if (wasRunning)
                entry.plugin->start(config_);
            bots_.push_back(std::move(entry));
        }
        else if (usernameChanged)
        {
            // Same site, just update username
            if (wasRunning)
                it->plugin->stop();

            configStore_.remove(oldUsername, oldSlug);

            // Create new plugin with new username (simpler than trying to rename in-place)
            auto plugin = SiteRegistry::instance().create(it->plugin->siteName(), newUsername);
            if (!plugin)
                return false;

            plugin->setStateCallback([this](const BotState &state)
                                     { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });

            ModelConfig mc;
            mc.site = plugin->siteName();
            mc.username = newUsername;
            mc.running = wasRunning;
            configStore_.add(mc);
            configStore_.save();

            BotEntry newEntry;
            newEntry.plugin = std::move(plugin);
            newEntry.autoStart = wasRunning;
            if (wasRunning)
                newEntry.plugin->start(config_);

            *it = std::move(newEntry);
        }
        else
        {
            return false; // Nothing changed
        }

        emitEvent(ManagerEvent::BotStatusChanged, newUsername + "_" + SiteRegistry::instance().nameToSlug(newSite.empty() ? oldSite : newSite));
        return true;
    }

    bool BotManager::restartBot(const std::string &username, const std::string &site)
    {
        stopBot(username, site);
        // Use a detached thread so we don't block the caller (GUI thread)
        std::string u = username;
        std::string s = site;
        std::thread([this, u, s]()
                    {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            startBot(u, s); })
            .detach();
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Move files to unprocessed (Python do_move / _move_streamer_files)
    // ─────────────────────────────────────────────────────────────────
    BotManager::MoveResult BotManager::moveFilesToUnprocessed(
        const std::string &username, const std::string &site)
    {
        namespace fs = std::filesystem;
        std::error_code ec;

        SitePlugin *bot = nullptr;
        bool wasRunning = false;
        bool wasRecording = false;
        fs::path sourceFolder;
        std::string slug;

        {
            std::lock_guard lock(mutex_);
            bot = findBot(username, site);
            if (!bot)
                return {false, "Bot not found"};

            auto state = bot->getState();
            wasRunning = state.running;
            wasRecording = state.recording;
            slug = bot->siteSlug();
            sourceFolder = bot->getOutputFolder(config_);
        }

        // Check source folder exists and has files
        if (!fs::exists(sourceFolder, ec) || !fs::is_directory(sourceFolder, ec))
            return {false, "No download folder found"};

        bool empty = true;
        for (const auto &entry : fs::directory_iterator(sourceFolder, ec))
        {
            (void)entry;
            empty = false;
            break;
        }
        if (empty)
            return {false, "No files to move"};

        // Stop recording if active
        if (wasRecording)
        {
            spdlog::info("Stopping {} [{}] for file move...", username, slug);
            stopBot(username, slug);

            // Wait for recording to actually stop (30s timeout)
            for (int i = 0; i < 30; i++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto st = getBotState(username, slug);
                if (!st || !st->recording)
                    break;
                if (i % 5 == 4)
                    spdlog::info("Waiting for {} to stop recording... ({}s)", username, i + 1);
            }

            // Check if still recording after timeout
            auto st = getBotState(username, slug);
            if (st && st->recording)
            {
                if (wasRunning)
                    startBot(username, slug);
                return {false, fmt::format("Timeout waiting for recording to stop (30s)")};
            }
        }
        else if (wasRunning)
        {
            // Stop even if not recording so we don't race with new recordings
            stopBot(username, slug);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Destination: ../unprocessed/{username} [{slug}]/
        auto unprocessedDir = config_.downloadsDir.parent_path() / "unprocessed";
        fs::create_directories(unprocessedDir, ec);

        std::string folderName = username + " [" + slug + "]";
        auto destFolder = unprocessedDir / folderName;

        // Check for file conflicts if dest exists
        if (fs::exists(destFolder, ec) && fs::is_directory(destFolder, ec))
        {
            std::set<std::string> existing, source;
            for (const auto &e : fs::directory_iterator(destFolder, ec))
                existing.insert(e.path().filename().string());
            for (const auto &e : fs::directory_iterator(sourceFolder, ec))
                source.insert(e.path().filename().string());

            std::vector<std::string> conflicts;
            for (const auto &f : source)
            {
                if (existing.count(f))
                    conflicts.push_back(f);
            }

            if (!conflicts.empty())
            {
                if (wasRunning)
                    startBot(username, slug);
                std::string preview;
                for (size_t i = 0; i < std::min(conflicts.size(), size_t(3)); i++)
                {
                    if (!preview.empty())
                        preview += ", ";
                    preview += conflicts[i];
                }
                if (conflicts.size() > 3)
                    preview += "...";
                return {false, fmt::format("Conflicts detected: {}", preview)};
            }
        }

        // Move files
        try
        {
            if (!fs::exists(destFolder, ec))
            {
                // Move entire folder
                fs::rename(sourceFolder, destFolder, ec);
                if (ec)
                {
                    // Cross-device? Fall back to copy + remove
                    fs::copy(sourceFolder, destFolder,
                             fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
                    if (!ec)
                        fs::remove_all(sourceFolder, ec);
                }
            }
            else
            {
                // Move individual files into existing dest
                for (const auto &entry : fs::directory_iterator(sourceFolder, ec))
                {
                    auto dst = destFolder / entry.path().filename();
                    fs::rename(entry.path(), dst, ec);
                    if (ec)
                    {
                        fs::copy_file(entry.path(), dst,
                                      fs::copy_options::overwrite_existing, ec);
                        if (!ec)
                            fs::remove(entry.path(), ec);
                    }
                }

                // Remove empty source folder
                if (fs::exists(sourceFolder, ec) && fs::is_empty(sourceFolder, ec))
                {
                    fs::remove(sourceFolder, ec);
                    spdlog::info("Removed empty folder: {}", sourceFolder.string());
                }
            }
        }
        catch (const std::exception &e)
        {
            spdlog::error("Error moving files for {}: {}", username, e.what());
            if (wasRunning)
                startBot(username, slug);
            return {false, fmt::format("Error: {}", e.what())};
        }

        // Restart if was running
        if (wasRunning)
        {
            spdlog::info("Restarting {} [{}] after file move...", username, slug);
            startBot(username, slug);
        }

        return {true, "Files moved successfully"};
    }

    BotManager::MoveResult BotManager::moveAllFilesToUnprocessed()
    {
        auto states = getAllStates();
        int moved = 0, failed = 0;
        std::string details;

        for (const auto &st : states)
        {
            auto result = moveFilesToUnprocessed(st.username, st.siteSlug);
            if (result.success)
            {
                moved++;
                details += fmt::format("  [{}] {}: {}\n", st.siteSlug, st.username, result.message);
            }
            else if (result.message != "No download folder found" &&
                     result.message != "No files to move")
            {
                failed++;
                details += fmt::format("  [{}] {}: {}\n", st.siteSlug, st.username, result.message);
            }
        }

        if (moved == 0 && failed == 0)
            return {true, "No files to move"};

        return {failed == 0,
                fmt::format("Moved files for {} bot(s){}\n{}",
                            moved,
                            failed > 0 ? fmt::format(", {} failed", failed) : "",
                            details)};
    }

    // ─────────────────────────────────────────────────────────────────
    // Resync (Python do_resync / _resync_streamer)
    // Force status recheck and restart downloads
    // ─────────────────────────────────────────────────────────────────
    std::string BotManager::resyncBot(const std::string &username,
                                      const std::string &site)
    {
        SitePlugin *bot = nullptr;
        std::string slug;
        bool wasRecording = false;

        {
            std::lock_guard lock(mutex_);
            bot = findBot(username, site);
            if (!bot)
                return "Bot not found";
            slug = bot->siteSlug();
            wasRecording = bot->getState().recording;
        }

        // Stop if recording
        if (wasRecording)
        {
            spdlog::info("Force stopping {} [{}] for resync...", username, slug);
            stopBot(username, slug);

            for (int i = 0; i < 15; i++)
            {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                auto st = getBotState(username, slug);
                if (!st || !st->recording)
                    break;
            }

            auto st = getBotState(username, slug);
            if (st && st->recording)
                return "Timeout stopping recording for resync";
        }
        else
        {
            stopBot(username, slug);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }

        // Force a fresh status check by restarting the bot
        // The bot thread will call checkStatus() on startup
        spdlog::info("Resyncing {} [{}]...", username, slug);
        startBot(username, slug);

        // Wait a moment for the status check
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto st = getBotState(username, slug);
        if (!st)
            return "Resynced (bot state unavailable)";

        std::string statusStr = statusToString(st->status);
        if (st->status == Status::Public)
            return fmt::format("Resynced and started download ({})", statusStr);
        else
            return fmt::format("Resynced - status: {}", statusStr);
    }

    std::string BotManager::resyncAll()
    {
        auto states = getAllStates();
        int resynced = 0;
        std::string details;

        for (const auto &st : states)
        {
            auto result = resyncBot(st.username, st.siteSlug);
            details += fmt::format("  [{}] {}: {}\n", st.siteSlug, st.username, result);
            if (result.find("Resynced") != std::string::npos)
                resynced++;
        }

        return fmt::format("Resync complete ({}/{} successful)\n{}", resynced, states.size(), details);
    }

    // ─────────────────────────────────────────────────────────────────
    // Queries
    // ─────────────────────────────────────────────────────────────────
    std::vector<BotState> BotManager::getAllStates() const
    {
        std::lock_guard lock(mutex_);
        std::vector<BotState> states;
        states.reserve(bots_.size());
        for (const auto &entry : bots_)
            states.push_back(entry.plugin->getState());
        return states;
    }

    std::optional<std::vector<BotState>> BotManager::tryGetAllStates() const
    {
        std::unique_lock lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock())
            return std::nullopt; // Mutex busy — skip this refresh
        std::vector<BotState> states;
        states.reserve(bots_.size());
        for (const auto &entry : bots_)
            states.push_back(entry.plugin->getState());
        return states;
    }

    std::optional<BotState> BotManager::getBotState(const std::string &username,
                                                    const std::string &site) const
    {
        std::lock_guard lock(mutex_);
        const auto *bot = findBot(username, site);
        if (!bot)
            return std::nullopt;
        return bot->getState();
    }

    size_t BotManager::botCount() const
    {
        std::lock_guard lock(mutex_);
        return bots_.size();
    }

    size_t BotManager::recordingCount() const
    {
        std::lock_guard lock(mutex_);
        return std::count_if(bots_.begin(), bots_.end(),
                             [](const BotEntry &e)
                             { return e.plugin->getState().recording; });
    }

    size_t BotManager::onlineCount() const
    {
        std::lock_guard lock(mutex_);
        return std::count_if(bots_.begin(), bots_.end(),
                             [](const BotEntry &e)
                             {
                                 auto s = e.plugin->getStatus();
                                 return s == Status::Public || s == Status::Online;
                             });
    }

    // ─────────────────────────────────────────────────────────────────
    // Events
    // ─────────────────────────────────────────────────────────────────
    void BotManager::setEventCallback(ManagerEventCallback cb)
    {
        std::lock_guard lock(eventMutex_);
        eventCb_ = std::move(cb);
    }

    void BotManager::emitEvent(ManagerEvent::Type type, const std::string &botId,
                               const std::string &msg)
    {
        ManagerEventCallback cb;
        {
            std::lock_guard lock(eventMutex_);
            cb = eventCb_; // copy under lock
        }
        if (cb)
            cb({type, botId, msg}); // invoke outside lock
    }

    // ─────────────────────────────────────────────────────────────────
    // Config
    // ─────────────────────────────────────────────────────────────────
    void BotManager::saveConfig()
    {
        std::lock_guard lock(mutex_);
        for (const auto &entry : bots_)
        {
            auto state = entry.plugin->getState();
            configStore_.updateStatus(state.username, state.siteSlug,
                                      state.status, state.recording);
            configStore_.updateRunning(state.username, state.siteSlug,
                                       entry.plugin->isRunning());
        }
        configStore_.save();
    }

    std::vector<std::string> BotManager::availableSites() const
    {
        return SiteRegistry::instance().siteNames();
    }

    // ─────────────────────────────────────────────────────────────────
    // Cross-register system
    // ─────────────────────────────────────────────────────────────────
    bool BotManager::createCrossRegisterGroup(
        const std::string &groupName,
        const std::vector<std::pair<std::string, std::string>> &members)
    {
        std::lock_guard lock(mutex_);

        // Check if group already exists
        for (const auto &g : crossRegisterGroups_)
        {
            if (g.groupName == groupName)
                return false;
        }

        CrossRegisterGroup group;
        group.groupName = groupName;
        group.members = members;

        // Update ModelConfig for each member — set crossRegisterGroup field
        for (const auto &[site, user] : members)
            configStore_.setCrossRegisterGroup(user, site, groupName);

        crossRegisterGroups_.push_back(std::move(group));

        // Create the actual ModelGroup with its cycling thread
        auto mg = std::make_unique<ModelGroup>(groupName);
        for (const auto &[site, user] : members)
            mg->addPairing(site, user);
        groups_.push_back(std::move(mg));

        emitEvent(ManagerEvent::ConfigSaved, groupName, "Cross-register group created");
        configStore_.save();
        return true;
    }

    bool BotManager::removeCrossRegisterGroup(const std::string &groupName)
    {
        std::lock_guard lock(mutex_);
        auto it = std::find_if(crossRegisterGroups_.begin(), crossRegisterGroups_.end(),
                               [&](const CrossRegisterGroup &g)
                               { return g.groupName == groupName; });
        if (it == crossRegisterGroups_.end())
            return false;

        // Clear group reference from member configs
        for (const auto &[site, user] : it->members)
            configStore_.setCrossRegisterGroup(user, site, "");

        crossRegisterGroups_.erase(it);

        // Remove the ModelGroup object
        auto git = std::find_if(groups_.begin(), groups_.end(),
                                [&](const auto &g)
                                { return g->groupName() == groupName; });
        if (git != groups_.end())
        {
            (*git)->requestQuit();
            groups_.erase(git);
        }

        configStore_.save();
        return true;
    }

    bool BotManager::addToCrossRegisterGroup(
        const std::string &groupName,
        const std::string &site, const std::string &username)
    {
        std::lock_guard lock(mutex_);
        for (auto &g : crossRegisterGroups_)
        {
            if (g.groupName == groupName)
            {
                // Check if already a member
                for (const auto &[s, u] : g.members)
                {
                    if (s == site && u == username)
                        return false;
                }
                g.members.emplace_back(site, username);

                // Ensure the model exists as a bot entry + config
                if (!findBot(username, site))
                {
                    auto plugin = SiteRegistry::instance().create(site, username);
                    if (plugin)
                    {
                        plugin->setStateCallback([this](const BotState &state)
                                                 { emitEvent(ManagerEvent::BotStatusChanged, state.username + "_" + state.siteSlug); });

                        // Add to config store if not already there
                        if (!configStore_.find(username, site).has_value())
                        {
                            ModelConfig mc;
                            mc.site = site;
                            mc.username = username;
                            mc.running = false;
                            mc.crossRegisterGroup = groupName;
                            configStore_.add(mc);
                        }

                        BotEntry entry;
                        entry.plugin = std::move(plugin);
                        entry.autoStart = false;
                        bots_.push_back(std::move(entry));
                        spdlog::info("Created bot entry for cross-register member {} [{}]", username, site);
                    }
                    else
                    {
                        spdlog::warn("Unknown site '{}' for cross-register member '{}'", site, username);
                    }
                }

                // Set group reference on model config and save
                configStore_.setCrossRegisterGroup(username, site, groupName);

                // Sync with ModelGroup
                for (auto &mg : groups_)
                {
                    if (mg->groupName() == groupName)
                    {
                        mg->addPairing(site, username);
                        break;
                    }
                }
                configStore_.save();
                return true;
            }
        }
        return false;
    }

    bool BotManager::removeFromCrossRegisterGroup(
        const std::string &groupName,
        const std::string &site, const std::string &username)
    {
        std::lock_guard lock(mutex_);
        for (auto &g : crossRegisterGroups_)
        {
            if (g.groupName == groupName)
            {
                auto it = std::find_if(g.members.begin(), g.members.end(),
                                       [&](const auto &p)
                                       { return p.first == site && p.second == username; });
                if (it != g.members.end())
                {
                    // Clear group reference on model config
                    configStore_.setCrossRegisterGroup(it->second, it->first, "");
                    g.members.erase(it);

                    // Sync with ModelGroup
                    for (auto &mg : groups_)
                    {
                        if (mg->groupName() == groupName)
                        {
                            mg->removePairing(site, username);
                            break;
                        }
                    }
                    configStore_.save();
                    return true;
                }
            }
        }
        return false;
    }

    std::vector<CrossRegisterGroup> BotManager::getCrossRegisterGroups() const
    {
        std::lock_guard lock(mutex_);
        return crossRegisterGroups_;
    }

    bool BotManager::setPrimaryPairing(const std::string &groupName, size_t pairingIndex)
    {
        std::lock_guard lock(mutex_);
        for (auto &g : crossRegisterGroups_)
        {
            if (g.groupName == groupName && pairingIndex > 0 && pairingIndex < g.members.size())
            {
                // Move the selected pairing to the front
                auto pairing = g.members[pairingIndex];
                g.members.erase(g.members.begin() + pairingIndex);
                g.members.insert(g.members.begin(), pairing);

                // Sync ordering with ModelGroup
                for (auto &mg : groups_)
                {
                    if (mg->groupName() == groupName)
                    {
                        mg->setPrimaryPairing(pairingIndex);
                        break;
                    }
                }
                configStore_.save();
                return true;
            }
        }
        return false;
    }

    std::optional<CrossRegisterGroup> BotManager::getGroupForBot(
        const std::string &username, const std::string &site) const
    {
        std::lock_guard lock(mutex_);
        for (const auto &g : crossRegisterGroups_)
        {
            for (const auto &[s, u] : g.members)
            {
                if (u == username && (site.empty() || s == site))
                    return g;
            }
        }
        return std::nullopt;
    }

    // ─────────────────────────────────────────────────────────────────
    // Group lifecycle (cycling threads)
    // ─────────────────────────────────────────────────────────────────
    bool BotManager::startGroup(const std::string &groupName)
    {
        std::lock_guard lock(mutex_);
        for (auto &g : groups_)
        {
            if (g->groupName() == groupName && !g->isRunning())
            {
                g->setStateCallback([this, groupName](const ModelGroupState &)
                                    { emitEvent(ManagerEvent::BotStatusChanged, "group:" + groupName); });
                g->start(config_);
                return true;
            }
        }
        return false;
    }

    bool BotManager::stopGroup(const std::string &groupName)
    {
        std::lock_guard lock(mutex_);
        for (auto &g : groups_)
        {
            if (g->groupName() == groupName && g->isRunning())
            {
                g->stop();
                return true;
            }
        }
        return false;
    }

    void BotManager::startAllGroups()
    {
        std::lock_guard lock(mutex_);
        for (auto &g : groups_)
        {
            if (!g->isRunning() && g->pairingCount() > 0)
            {
                g->setStateCallback([this, name = g->groupName()](const ModelGroupState &)
                                    { emitEvent(ManagerEvent::BotStatusChanged, "group:" + name); });
                g->start(config_);
            }
        }
    }

    void BotManager::stopAllGroups()
    {
        std::lock_guard lock(mutex_);
        for (auto &g : groups_)
        {
            if (g->isRunning())
                g->stop();
        }
    }

    std::vector<ModelGroupState> BotManager::getAllGroupStates() const
    {
        std::lock_guard lock(mutex_);
        std::vector<ModelGroupState> states;
        states.reserve(groups_.size());
        for (const auto &g : groups_)
            states.push_back(g->getState());
        return states;
    }

    // ─────────────────────────────────────────────────────────────────
    // Disk usage
    // ─────────────────────────────────────────────────────────────────
    BotManager::DiskUsageInfo BotManager::getDiskUsage() const
    {
        DiskUsageInfo info;
        std::error_code ec;

        std::filesystem::path downloadDir = config_.downloadsDir;

        // Get filesystem space info
        auto spaceInfo = std::filesystem::space(downloadDir, ec);
        if (!ec)
        {
            info.totalBytes = spaceInfo.capacity;
            info.freeBytes = spaceInfo.available;
        }

        // Calculate download directory size
        if (std::filesystem::exists(downloadDir, ec))
        {
            for (const auto &entry : std::filesystem::recursive_directory_iterator(downloadDir, ec))
            {
                if (entry.is_regular_file(ec))
                {
                    info.downloadDirBytes += entry.file_size(ec);
                    info.fileCount++;
                }
            }
        }

        return info;
    }

    // ─────────────────────────────────────────────────────────────────
    // Continuous preview forwarding
    // ─────────────────────────────────────────────────────────────────
    bool BotManager::consumePreview(const std::string &username, const std::string &site,
                                    PreviewFrame &out, uint64_t &lastVersion)
    {
        std::lock_guard lock(mutex_);
        auto *bot = findBot(username, site);
        if (!bot)
            return false;
        return bot->consumePreview(out, lastVersion);
    }

    bool BotManager::waitForPreview(const std::string &username, const std::string &site,
                                    PreviewFrame &out, uint64_t &lastVersion, int timeoutMs)
    {
        // Find the bot under lock, then release lock before waiting
        SitePlugin *bot = nullptr;
        {
            std::lock_guard lock(mutex_);
            bot = findBot(username, site);
        }
        if (!bot)
            return false;
        return bot->waitForPreview(out, lastVersion, timeoutMs);
    }

    // ─────────────────────────────────────────────────────────────────
    // Audio callback forwarding
    // ─────────────────────────────────────────────────────────────────
    void BotManager::setAudioDataCallback(const std::string &username, const std::string &site,
                                          AudioDataCallback cb)
    {
        std::lock_guard lock(mutex_);
        auto *bot = findBot(username, site);
        if (bot)
            bot->setAudioDataCallback(std::move(cb));
    }

    void BotManager::clearAudioDataCallback(const std::string &username, const std::string &site)
    {
        std::lock_guard lock(mutex_);
        auto *bot = findBot(username, site);
        if (bot)
            bot->clearAudioDataCallback();
    }

    // ─────────────────────────────────────────────────────────────────
    // Internal find
    // ─────────────────────────────────────────────────────────────────
    SitePlugin *BotManager::findBot(const std::string &username, const std::string &site)
    {
        for (auto &entry : bots_)
        {
            if (entry.plugin->username() == username &&
                (site.empty() || entry.plugin->siteName() == site ||
                 entry.plugin->siteSlug() == site))
                return entry.plugin.get();
        }
        return nullptr;
    }

    const SitePlugin *BotManager::findBot(const std::string &username,
                                          const std::string &site) const
    {
        for (const auto &entry : bots_)
        {
            if (entry.plugin->username() == username &&
                (site.empty() || entry.plugin->siteName() == site ||
                 entry.plugin->siteSlug() == site))
                return entry.plugin.get();
        }
        return nullptr;
    }

} // namespace sm
