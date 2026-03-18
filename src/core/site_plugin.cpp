// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Site plugin base class implementation
// Faithful port of Python bot.py — all logic replicated:
//   - File naming: sidecar checking, zero-byte cleanup, first-gap
//   - Resolution: config-driven, min(w,h) for portrait
//   - State machine: error reset on non-error, auto-remove,
//     download loop re-check, consecutive error tracking
//   - Post-download: temp file cleanup, HTML sniffing
// ─────────────────────────────────────────────────────────────────

#include "core/site_plugin.h"
#include "core/bot_manager.h"
#include "gui/imgui_log_sink.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace sm
{

    std::mutex SitePlugin::fileNumberMutex_;
    BotManager *SitePlugin::manager_ = nullptr;

    // ─────────────────────────────────────────────────────────────────
    // SiteRegistry (singleton)
    // ─────────────────────────────────────────────────────────────────
    SiteRegistry &SiteRegistry::instance()
    {
        static SiteRegistry reg;
        return reg;
    }

    void SiteRegistry::registerSite(const std::string &siteName, const std::string &siteSlug,
                                    FactoryFn factory)
    {
        SiteInfo info{siteName, siteSlug, std::move(factory)};
        sites_[siteName] = info;
        slugToName_[siteSlug] = siteName;
        spdlog::debug("Registered site: {} [{}]", siteName, siteSlug);
    }

    std::unique_ptr<SitePlugin> SiteRegistry::create(const std::string &siteName,
                                                     const std::string &username) const
    {
        auto it = sites_.find(siteName);
        if (it == sites_.end())
        {
            // Try slug lookup
            auto slug_it = slugToName_.find(siteName);
            if (slug_it != slugToName_.end())
                it = sites_.find(slug_it->second);
        }
        if (it == sites_.end())
            return nullptr;
        return it->second.factory(username);
    }

    std::vector<std::string> SiteRegistry::siteNames() const
    {
        std::vector<std::string> names;
        for (const auto &[name, _] : sites_)
            names.push_back(name);
        return names;
    }

    std::string SiteRegistry::slugToName(const std::string &slug) const
    {
        auto it = slugToName_.find(slug);
        return (it != slugToName_.end()) ? it->second : "";
    }

    std::string SiteRegistry::nameToSlug(const std::string &name) const
    {
        auto it = sites_.find(name);
        return (it != sites_.end()) ? it->second.slug : "";
    }

    bool SiteRegistry::hasSite(const std::string &name) const
    {
        return sites_.count(name) > 0 || slugToName_.count(name) > 0;
    }

    // ─────────────────────────────────────────────────────────────────
    // Constructor / Destructor
    // ─────────────────────────────────────────────────────────────────
    SitePlugin::SitePlugin(const std::string &siteName, const std::string &siteSlug,
                           const std::string &username)
        : siteName_(siteName), siteSlug_(siteSlug), username_(username)
    {
        std::string logName = username + " [" + siteSlug + "]";

        // Build sink list: console + ImGui GUI sink (if available)
        auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        std::vector<spdlog::sink_ptr> sinks = {consoleSink};
        if (auto guiSink = ImGuiLogSink::instance())
            sinks.push_back(guiSink);

        // Unique registry name (pointer suffix) — logName used for display
        std::string registryName = logName + "_" +
                                   std::to_string(reinterpret_cast<uintptr_t>(this));
        logger_ = std::make_shared<spdlog::logger>(registryName, sinks.begin(), sinks.end());
        logger_->set_level(spdlog::level::info);
        logger_->set_pattern("[%H:%M:%S] [" + logName + "] %v");
        spdlog::register_logger(logger_);

        state_.username = username;
        state_.siteName = siteName;
        state_.siteSlug = siteSlug;
        state_.startTime = Clock::now();
    }

    SitePlugin::~SitePlugin()
    {
        requestQuit();
        if (thread_ && thread_->joinable())
        {
            thread_->request_stop();
            cancelToken_.cancel();
            thread_->join();
        }
        spdlog::drop(logger_->name());
    }

    // ─────────────────────────────────────────────────────────────────
    // Lifecycle
    // ─────────────────────────────────────────────────────────────────

    void SitePlugin::configure(const AppConfig &config)
    {
        config_ = &config;
        http_.setDefaultUserAgent(config.userAgent);
        http_.setDefaultTimeout(config.httpTimeoutSec);
        http_.setVerifySsl(config.verifySsl);

        // Apply proxy from pool (first available)
        if (config.proxyEnabled && !config.proxies.empty())
        {
            proxyPool_.setProxies(config.proxies);
            proxyPool_.setMaxFailuresBeforeDisable(config.proxyMaxFailures);
            proxyPool_.setDisableDurationSec(config.proxyDisableSec);
            proxyPool_.setAutoDisable(config.proxyAutoDisable);

            if (auto proxy = proxyPool_.getNext())
            {
                http_.setProxy(*proxy);
                currentProxyUrl_ = proxy->url;
            }
        }

        std::lock_guard lock(stateMutex_);
        state_.websiteUrl = getWebsiteUrl();
    }

    void SitePlugin::start(const AppConfig &config)
    {
        if (running_.load())
            return;

        config_ = &config;
        running_.store(true);
        quitting_.store(false);
        cancelToken_.reset();

        {
            std::lock_guard lock(stateMutex_);
            state_.running = true;
            state_.quitting = false;
            state_.consecutiveErrors = 0;
            state_.websiteUrl = getWebsiteUrl();
        }

        http_.setDefaultUserAgent(config.userAgent);
        http_.setDefaultTimeout(config.httpTimeoutSec);
        http_.setVerifySsl(config.verifySsl);

        // Apply proxy from pool
        if (config.proxyEnabled && !config.proxies.empty())
        {
            proxyPool_.setProxies(config.proxies);
            proxyPool_.setMaxFailuresBeforeDisable(config.proxyMaxFailures);
            proxyPool_.setDisableDurationSec(config.proxyDisableSec);
            proxyPool_.setAutoDisable(config.proxyAutoDisable);

            if (auto proxy = proxyPool_.getNext())
            {
                http_.setProxy(*proxy);
                currentProxyUrl_ = proxy->url;
            }
        }

        thread_ = std::make_unique<std::jthread>([this, &config](std::stop_token)
                                                 { threadFunc(config); });

        logger_->info("Started monitoring");
    }

    void SitePlugin::stop()
    {
        running_.store(false);
        cancelToken_.cancel();
        sleepCv_.notify_all(); // Wake sleepInterruptible immediately

        StateChangeCallback cb;
        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.status = Status::NotRunning;
            state_.recording = false;
            cb = stateCallback_; // copy under lock
        }

        if (cb)
            cb(state_);
        logger_->info("Stopped monitoring");
    }

    void SitePlugin::requestQuit()
    {
        quitting_.store(true);
        running_.store(false);
        cancelToken_.cancel();
        sleepCv_.notify_all(); // Wake sleepInterruptible immediately
    }

    bool SitePlugin::isRunning() const { return running_.load(); }

    bool SitePlugin::isAlive() const
    {
        return thread_ && thread_->joinable();
    }

    // ─────────────────────────────────────────────────────────────────
    // State access
    // ─────────────────────────────────────────────────────────────────
    BotState SitePlugin::getState() const
    {
        std::lock_guard lock(stateMutex_);
        return state_;
    }

    Status SitePlugin::getStatus() const
    {
        std::lock_guard lock(stateMutex_);
        return state_.status;
    }

    void SitePlugin::setStateCallback(StateChangeCallback cb)
    {
        std::lock_guard lock(stateMutex_);
        stateCallback_ = std::move(cb);
    }

    void SitePlugin::setState(Status status)
    {
        // Refresh preview URL on each status update (some sites update per-status-check)
        auto preview = getPreviewUrl();

        std::lock_guard lock(stateMutex_);
        bool statusChanged = (state_.status != status);
        state_.prevStatus = state_.status;
        state_.status = status;
        // Only update lastStatusChange when status actually changes
        if (statusChanged)
            state_.lastStatusChange = Clock::now();
        // Preserve local captured preview while recording; only overwrite
        // from site preview URL when we don't already have a local file path.
        bool hasLocalPreview = false;
#ifdef _WIN32
        hasLocalPreview = (state_.previewUrl.size() > 2 && state_.previewUrl[1] == ':');
#else
        hasLocalPreview = (!state_.previewUrl.empty() && state_.previewUrl[0] == '/');
#endif
        bool previewChanged = false;
        if (!preview.empty() && !(state_.recording && hasLocalPreview))
        {
            previewChanged = (state_.previewUrl != preview);
            state_.previewUrl = preview;
        }
        // Only fire callback when something actually changed — avoids
        // spamming glfwPostEmptyEvent when 20 bots check status every 5s
        if ((statusChanged || previewChanged) && stateCallback_)
            stateCallback_(state_);
    }

    void SitePlugin::setRecording(bool rec)
    {
        std::lock_guard lock(stateMutex_);
        state_.recording = rec;
        if (stateCallback_)
            stateCallback_(state_);
    }

    void SitePlugin::setMobile(bool mobile)
    {
        std::lock_guard lock(stateMutex_);
        state_.mobile = mobile;
    }

    void SitePlugin::setGender(Gender g)
    {
        std::lock_guard lock(stateMutex_);
        state_.gender = g;
    }

    void SitePlugin::setCountry(const std::string &c)
    {
        std::lock_guard lock(stateMutex_);
        state_.country = c;
    }

    void SitePlugin::setRoomId(const std::string &rid)
    {
        std::lock_guard lock(stateMutex_);
        state_.roomId = rid;
    }

    void SitePlugin::setUsername(const std::string &newUsername)
    {
        if (newUsername.empty() || newUsername == username_)
            return;
        logger_->info("Username updated: {} -> {}", username_, newUsername);
        username_ = newUsername;
        std::lock_guard lock(stateMutex_);
        state_.username = newUsername;
    }

    void SitePlugin::setLastError(const std::string &err, int httpCode)
    {
        std::lock_guard lock(stateMutex_);
        state_.lastError = err;
        state_.lastHttpCode = httpCode;
    }

    void SitePlugin::setLastApiResponse(const std::string &json)
    {
        std::lock_guard lock(stateMutex_);
        state_.lastApiResponse = json;
    }

    void SitePlugin::setRecordingResolution(int width, int height)
    {
        std::lock_guard lock(stateMutex_);
        state_.recordingStats.recordingWidth = width;
        state_.recordingStats.recordingHeight = height;
    }

    void SitePlugin::forceResync()
    {
        resyncPending_.store(true);
        cancelToken_.cancel(); // Wake up any sleeping operation
        sleepCv_.notify_all(); // Wake sleepInterruptible immediately
        logger_->info("Force resync requested");
    }

    // ─────────────────────────────────────────────────────────────────
    // Resolution selection (faithful port of Python
    // getWantedResolutionPlaylist)
    //
    // Python logic:
    //   resolution_diff = min(w,h) - WANTED_RESOLUTION  (handles portrait)
    //   Sort by abs(resolution_diff)
    //   Then apply WANTED_RESOLUTION_PREFERENCE:
    //     exact: only if diff==0
    //     closest: first in sorted list
    //     exact_or_least_higher: first with diff>=0
    //     exact_or_highest_lower: first with diff<=0
    //   Use urljoin(url, selected_url) to resolve relative
    // ─────────────────────────────────────────────────────────────────
    std::string SitePlugin::selectResolution(const std::string &masterUrl)
    {
        auto resp = http_.get(masterUrl, 15);
        if (!resp.ok())
        {
            logger_->warn("Failed to fetch master playlist: HTTP {}", resp.statusCode);
            return masterUrl;
        }

        if (!M3U8Parser::isMasterPlaylist(resp.body))
            return masterUrl;

        auto master = M3U8Parser::parseMaster(resp.body, masterUrl);
        if (master.variants.empty())
        {
            logger_->warn("No variants found in master playlist");
            return masterUrl;
        }

        for (const auto &v : master.variants)
        {
            std::string audioInfo = v.audioGroupId.empty() ? "" : " audio=" + v.audioGroupId;
            logger_->debug("  Variant: {}x{} @ {} bps{}", v.width, v.height, v.bandwidth, audioInfo);
        }

        if (master.hasSplitAudio())
            logger_->info("Detected split audio/video playlist ({} audio renditions)", master.audioRenditions.size());

        // Use config values (not hardcoded!)
        int wantedRes = config_ ? config_->wantedResolution : 99999;
        ResolutionPref pref = config_ ? config_->resolutionPref : ResolutionPref::Closest;

        // Python uses min(w,h) - WANTED_RESOLUTION for portrait handling
        struct VariantWithDiff
        {
            const HLSVariant *v;
            int diff;
        };
        std::vector<VariantWithDiff> withDiff;
        for (const auto &v : master.variants)
        {
            int minDim = (v.width < v.height) ? v.width : v.height;
            if (minDim == 0)
                minDim = v.height; // fallback
            withDiff.push_back({&v, minDim - wantedRes});
        }

        // Sort by abs(diff)
        std::sort(withDiff.begin(), withDiff.end(),
                  [](const VariantWithDiff &a, const VariantWithDiff &b)
                  {
                      return std::abs(a.diff) < std::abs(b.diff);
                  });

        const HLSVariant *selected = nullptr;

        switch (pref)
        {
        case ResolutionPref::Exact:
            if (!withDiff.empty() && withDiff[0].diff == 0)
                selected = withDiff[0].v;
            break;

        case ResolutionPref::Closest:
            if (!withDiff.empty())
                selected = withDiff[0].v;
            break;

        case ResolutionPref::ExactOrLeastHigher:
            for (const auto &vd : withDiff)
            {
                if (vd.diff >= 0)
                {
                    selected = vd.v;
                    break;
                }
            }
            break;

        case ResolutionPref::ExactOrHighestLower:
            for (const auto &vd : withDiff)
            {
                if (vd.diff <= 0)
                {
                    selected = vd.v;
                    break;
                }
            }
            break;
        }

        if (!selected)
        {
            logger_->error("Couldn't select a resolution");
            return masterUrl;
        }

        logger_->info("Selected quality: {}x{}", selected->width, selected->height);

        // Store selected resolution for stats reporting
        setRecordingResolution(selected->width, selected->height);

        // If the master has split audio/video (LLHLS format), build a filtered
        // master playlist and write it to a temp file so ffmpeg can read both
        // audio and video chunklists and sync them via EXT-X-PROGRAM-DATE-TIME.
        if (master.hasSplitAudio() && !selected->audioGroupId.empty())
        {
            std::string filteredContent = M3U8Parser::buildFilteredMaster(master, *selected, masterUrl);
            logger_->debug("Built filtered master playlist for split audio:\n{}", filteredContent);

            // Write to a temp file that ffmpeg can read
            auto tempDir = std::filesystem::temp_directory_path() / "streamonitor";
            std::filesystem::create_directories(tempDir);
            auto tempFile = tempDir / (username() + "_" + siteSlug() + "_master.m3u8");
            {
                std::ofstream ofs(tempFile, std::ios::trunc);
                if (ofs.good())
                {
                    ofs << filteredContent;
                    ofs.close();
                    logger_->info("Wrote filtered master playlist to {}", tempFile.string());
                    return tempFile.string();
                }
                else
                {
                    logger_->warn("Failed to write filtered master, falling back to variant URL");
                }
            }
        }

        // Normal case: return the selected variant's URL directly
        return M3U8Parser::resolveUrl(masterUrl, selected->url);
    }

    // ─────────────────────────────────────────────────────────────────
    // Output folder path (Python: outputFolder property)
    // ─────────────────────────────────────────────────────────────────
    std::filesystem::path SitePlugin::getOutputFolder(const AppConfig &config) const
    {
        return config.downloadsDir / (username_ + " [" + siteSlug_ + "]");
    }

    // ─────────────────────────────────────────────────────────────────
    // Output filename generation — faithful port of Python
    // genOutFilename with sidecar checking, zero-byte cleanup,
    // first-gap algorithm
    // ─────────────────────────────────────────────────────────────────
    std::string SitePlugin::generateOutputPath(const AppConfig &config)
    {
        std::lock_guard lock(fileNumberMutex_);

        auto dir = config.downloadsDir / (username_ + " [" + siteSlug_ + "]");

        // Mobile subfolder (Python: outputFolder property)
        if (isMobile())
            dir /= "Mobile";

        fs::create_directories(dir);

        const auto &fmt = getFormatInfo(config.container);
        std::string ext = fmt.extension; // e.g. ".mkv"

        // ── Pass 1: Delete zero-byte final files (Python pre-scan) ──
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(dir, ec))
        {
            if (!entry.is_regular_file())
                continue;
            auto entryExt = entry.path().extension().string();
            std::transform(entryExt.begin(), entryExt.end(), entryExt.begin(), ::tolower);
            if (entryExt != ext)
                continue;
            try
            {
                if (fs::file_size(entry.path()) == 0)
                {
                    fs::remove(entry.path(), ec);
                    logger_->debug("Deleted zero-byte file: {}", entry.path().filename().string());
                }
            }
            catch (...)
            {
            }
        }

        // ── Sidecar file candidates for a given final path ──────────
        auto sidecarsFor = [](const fs::path &finalPath) -> std::vector<fs::path>
        {
            auto stem = finalPath.parent_path() / finalPath.stem();
            auto stemStr = stem.string();
            return {
                fs::path(stemStr + ".tmp.ts"),
                fs::path(stemStr + ".ts.tmp"),
                fs::path(stemStr + ".segment.tmp"),
                fs::path(stemStr + ".part"),
                fs::path(stemStr + ".tmp"),
            };
        };

        // ── Pass 2: Find first available gap (Python while True loop)
        int n = 1;
        while (true)
        {
            auto candidate = dir / (std::to_string(n) + ext);

            // If candidate file exists...
            if (fs::exists(candidate, ec))
            {
                try
                {
                    if (fs::file_size(candidate, ec) > 0)
                    {
                        n++;
                        continue; // slot occupied by valid file
                    }
                    // Zero-byte → remove and reuse
                    fs::remove(candidate, ec);
                    logger_->debug("Removed zero-byte file during numbering: {}{}", n, ext);
                }
                catch (...)
                {
                    n++;
                    continue;
                }
            }

            // Check sidecar files — if any non-zero sidecar exists,
            // this slot is "in use" by another download
            bool blocked = false;
            for (const auto &sidecar : sidecarsFor(candidate))
            {
                if (fs::exists(sidecar, ec))
                {
                    try
                    {
                        if (fs::file_size(sidecar, ec) == 0)
                        {
                            fs::remove(sidecar, ec);
                            logger_->debug("Removed zero-byte sidecar: {}",
                                           sidecar.filename().string());
                        }
                        else
                        {
                            blocked = true;
                            break;
                        }
                    }
                    catch (...)
                    {
                        blocked = true;
                        break;
                    }
                }
            }

            if (!blocked)
            {
                // Found available slot
                {
                    std::lock_guard slock(stateMutex_);
                    state_.currentFile = candidate.string();
                    state_.fileCount = n;
                }
                return candidate.string();
            }

            n++;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Post-download cleanup — faithful port of Python
    // _post_download_cleanup: temp file removal, HTML sniffing,
    // zero-byte check
    // ─────────────────────────────────────────────────────────────────
    bool SitePlugin::postDownloadCleanup(const std::string &finalPath, bool ok)
    {
        try
        {
            std::error_code ec;

            // Check for .tmp.ts as actual output (HLS downloads)
            std::string actualFile = finalPath;
            auto stem = fs::path(finalPath).parent_path() / fs::path(finalPath).stem();
            std::string tmpTsFile = stem.string() + ".tmp.ts";

            if (fs::exists(tmpTsFile, ec) && !fs::exists(finalPath, ec))
                actualFile = tmpTsFile;

            // If ok but file is zero or missing → fail
            if (ok)
            {
                bool isMissing = !fs::exists(actualFile, ec) || fs::file_size(actualFile, ec) == 0;
                if (isMissing)
                {
                    logger_->error("Output file is 0 KB or missing: {}", actualFile);
                    if (fs::exists(actualFile, ec))
                    {
                        fs::remove(actualFile, ec);
                        logger_->info("Removed zero-byte output file");
                    }
                    ok = false;
                }
            }

            // On failure: clean up temp/sidecar files
            if (!ok)
            {
                // Remove actual file if zero-byte
                if (fs::exists(actualFile, ec))
                {
                    if (fs::file_size(actualFile, ec) == 0)
                    {
                        fs::remove(actualFile, ec);
                        logger_->info("Removed failed output file");
                    }
                }

                // Clean sidecar temp files
                auto stemStr = stem.string();
                std::vector<std::string> tempCandidates = {
                    stemStr + ".tmp.ts",
                    stemStr + ".ts.tmp",
                    stemStr + ".segment.tmp",
                    stemStr + ".part",
                    stemStr + ".tmp",
                    (fs::path(finalPath).parent_path() / "ffmpeg2pass-0.log").string(),
                    (fs::path(finalPath).parent_path() / "ffmpeg2pass-0.log.mbtree").string(),
                };

                for (const auto &tmp : tempCandidates)
                {
                    if (!fs::exists(tmp, ec))
                        continue;

                    try
                    {
                        bool shouldDelete = false;

                        // Sniff first 512 bytes
                        std::ifstream f(tmp, std::ios::binary);
                        if (f)
                        {
                            char buf[512] = {};
                            f.read(buf, sizeof(buf));
                            auto bytesRead = f.gcount();

                            if (bytesRead == 0)
                            {
                                shouldDelete = true;
                                logger_->debug("Temp file is empty: {}",
                                               fs::path(tmp).filename().string());
                            }
                            else
                            {
                                // HTML detection (Python: <html or <!doctype)
                                std::string sniff(buf, bytesRead);
                                std::transform(sniff.begin(), sniff.end(), sniff.begin(), ::tolower);
                                if (sniff.find("<html") != std::string::npos ||
                                    sniff.find("<!doctype") != std::string::npos)
                                {
                                    shouldDelete = true;
                                    logger_->warn("Temp file contains HTML: {}",
                                                  fs::path(tmp).filename().string());
                                }
                            }
                        }
                        else
                        {
                            shouldDelete = true;
                        }

                        if (shouldDelete)
                        {
                            fs::remove(tmp, ec);
                            logger_->debug("Cleaned up temp file: {}",
                                           fs::path(tmp).filename().string());
                        }
                    }
                    catch (const std::exception &e)
                    {
                        logger_->debug("Failed to clean temp file {}: {}", tmp, e.what());
                    }
                }
            }
        }
        catch (const std::exception &e)
        {
            logger_->warn("Error in post-download cleanup: {}", e.what());
        }

        return ok;
    }

    // ─────────────────────────────────────────────────────────────────
    // Auto-remove model (Python: Bot.auto_remove_model)
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::autoRemoveModel(const std::string &reason)
    {
        if (manager_)
        {
            logger_->warn("Auto-removing {} model [{} ] {}", reason, siteSlug_, username_);
            manager_->removeBot(username_, siteName_);
            logger_->info("Successfully auto-removed {} model", reason);
        }
        else
        {
            logger_->warn("Model {} - {} (no manager for auto-removal)",
                          username_, reason);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Thread function — THE STATE MACHINE
    // Faithful port of Python bot.py run() method
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::threadFunc(const AppConfig &config)
    {
        logger_->info("Thread started for {}", username_);

        int offlineTime = 0; // cumulative offline seconds (Python: offline_time)

        while (running_.load() && !quitting_.load())
        {
            // ── Max consecutive error check (Python: _max_consecutive_errors)
            {
                bool errorLimitReached = false;
                {
                    std::lock_guard lock(stateMutex_);
                    if (state_.consecutiveErrors >= maxConsecutiveErrors_)
                    {
                        logger_->warn("Hit {} consecutive errors, backing off for {}s before retrying",
                                      maxConsecutiveErrors_, sleepOnLongOffline_);
                        state_.consecutiveErrors = 0;
                        errorLimitReached = true;
                    }
                }
                if (errorLimitReached)
                {
                    // Python: sleeps sleep_on_long_offline (15s), continues
                    // NOT setting RateLimit status like before
                    sleepInterruptible(sleepOnLongOffline_);
                    continue;
                }
            }

            // ── Check status ────────────────────────────────────────
            Status status;
            try
            {
                status = checkStatus();
            }
            catch (const std::exception &e)
            {
                logger_->error("Exception in checkStatus: {}", e.what());
                status = Status::Error;
            }

            // Log status changes (Python: if self.sc != self.previous_status)
            Status prevStatus;
            {
                std::lock_guard lock(stateMutex_);
                prevStatus = state_.status;
            }
            setState(status);

            if (status != prevStatus)
            {
                logger_->info("Status: {} → {}", statusToString(prevStatus),
                              statusToString(status));
            }

            // ── State machine dispatch ──────────────────────────────

            if (status == Status::Error)
            {
                // Python: self._consecutive_errors += 1; self._sleep(self.sleep_on_error)
                {
                    std::lock_guard lock(stateMutex_);
                    state_.consecutiveErrors++;
                }
                sleepInterruptible(sleepOnError_);
                continue;
            }

            if (status == Status::ConnectionError)
            {
                // Connection error: network/DNS/timeout failure — NOT a rate limit!
                // Track as consecutive error and sleep briefly before retry
                {
                    std::lock_guard lock(stateMutex_);
                    state_.consecutiveErrors++;
                }
                logger_->debug("Connection error, sleeping {}s", sleepOnError_);
                sleepInterruptible(sleepOnError_);
                continue;
            }

            // Reset consecutive errors on ANY non-error status
            // (Python: if self.sc != Status.ERROR: self._consecutive_errors = 0)
            {
                std::lock_guard lock(stateMutex_);
                state_.consecutiveErrors = 0;
            }

            if (status == Status::NotExist)
            {
                // Python: auto-removes, sets running=False, breaks
                logger_->error("User {} does not exist - auto-removing", username_);
                autoRemoveModel("non-existent");
                running_.store(false);
                break;
            }

            if (status == Status::Deleted)
            {
                // Python: auto-removes, sets running=False, breaks
                logger_->error("Model account {} has been DELETED - auto-removing", username_);
                autoRemoveModel("deleted");
                running_.store(false);
                break;
            }

            if (status == Status::Cloudflare)
            {
                // Python: self._sleep(self.sleep_on_ratelimit)
                logger_->error("Cloudflare challenge detected");
                sleepInterruptible(sleepOnRateLimit_);
                continue;
            }

            if (status == Status::RateLimit)
            {
                // Python: self._sleep(self.sleep_on_ratelimit)
                logger_->warn("Rate limited");
                sleepInterruptible(sleepOnRateLimit_);
                continue;
            }

            if (status == Status::Offline)
            {
                // Python: offline_time += self.sleep_on_offline
                offlineTime += sleepOnOffline_;
                int sleepTime = (offlineTime > longOfflineTimeout_)
                                    ? sleepOnLongOffline_
                                    : sleepOnOffline_;
                sleepInterruptible(sleepTime);
                continue;
            }

            if (status == Status::LongOffline)
            {
                // Python: self._sleep(self.sleep_on_long_offline)
                sleepInterruptible(sleepOnLongOffline_);
                continue;
            }

            if (status == Status::Online)
            {
                // Python: offline_time = 0; self._sleep(self.sleep_on_private)
                offlineTime = 0;
                sleepInterruptible(sleepOnPrivate_);
                continue;
            }

            if (status == Status::Private)
            {
                // Python: offline_time = 0; self._sleep(self.sleep_on_private)
                offlineTime = 0;
                sleepInterruptible(sleepOnPrivate_);
                continue;
            }

            if (status == Status::Restricted)
            {
                // Python: falls through to default error handler
                sleepInterruptible(sleepOnError_);
                continue;
            }

            if (status == Status::Public)
            {
                // Python: offline_time = 0; download loop
                offlineTime = 0;
                downloadLoop(config);
                continue;
            }

            // Unknown or unhandled status
            sleepInterruptible(sleepOnError_);
        }

        // Cleanup
        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.recording = false;
            state_.status = Status::NotRunning;
        }
        if (stateCallback_)
            stateCallback_(state_);
        logger_->info("Thread exiting for {}", username_);
    }

    // ─────────────────────────────────────────────────────────────────
    // Download loop — faithful port of Python PUBLIC handler
    //
    // Python logic:
    //   while running:
    //     re-check status
    //     if not PUBLIC → break
    //     success = _download_once()
    //     if not success:
    //       sleep(sleep_on_error)  ← NOT 5s!
    //       re-check status before retry
    //       if still PUBLIC → log "still live, retrying"
    //       else → break
    //     else:
    //       log "checking if still live"
    //       sleep(2)
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::downloadLoop(const AppConfig &config)
    {
        logger_->info("Entering download loop");

        while (running_.load() && !quitting_.load())
        {
            // Re-verify status (Python: current_status = self.getStatus())
            Status currentStatus;
            try
            {
                currentStatus = checkStatus();
            }
            catch (...)
            {
                currentStatus = Status::Error;
            }

            if (currentStatus != Status::Public)
            {
                setState(currentStatus);
                logger_->info("No longer public ({}), exiting download loop",
                              statusToString(currentStatus));
                break;
            }

            // Attempt download
            bool success = downloadOnce(config);

            if (!running_.load() || quitting_.load())
                break;

            if (!success)
            {
                // Python: self._sleep(self.sleep_on_error) — 20s, not 5s!
                sleepInterruptible(sleepOnError_);

                // Python: re-check status before retrying
                try
                {
                    Status postFailStatus = checkStatus();
                    setState(postFailStatus);
                    if (postFailStatus != Status::Public)
                    {
                        logger_->info("No longer public after failed download, exiting");
                        break;
                    }
                    logger_->info("Stream still live, retrying download...");
                }
                catch (...)
                {
                    setState(Status::Error);
                    break;
                }
            }
            else
            {
                // Python: "Checking if stream is still live..."; sleep(2)
                logger_->info("Checking if stream is still live...");
                sleepInterruptible(2);
            }
        }

        setRecording(false);
    }

    // ─────────────────────────────────────────────────────────────────
    // Single download attempt — with post-download cleanup
    // ─────────────────────────────────────────────────────────────────
    bool SitePlugin::downloadOnce(const AppConfig &config)
    {
        std::string videoUrl;
        try
        {
            videoUrl = getVideoUrl();
        }
        catch (const std::exception &e)
        {
            logger_->error("Failed to get video URL: {}", e.what());
            return false;
        }

        if (videoUrl.empty())
        {
            logger_->warn("Empty video URL");
            return false;
        }

        std::string outputPath = generateOutputPath(config);
        logger_->info("Started downloading show → {}", outputPath);

        setRecording(true);

        HLSRecorder recorder(config);
        recorder.setLogger(logger_);

        recorder.setProgressCallback([this](const RecordingProgress &prog)
                                     {
            // Update stats silently — do NOT fire stateCallback here.
            // The GUI refreshes bot states every 2s, which is fast enough
            // to show updated size/speed. Firing stateCallback on every
            // progress update (every 100 packets) would call
            // glfwPostEmptyEvent, keeping the GUI pinned at 30fps.
            std::lock_guard lock(stateMutex_);
            state_.recordingStats.bytesWritten = prog.bytesWritten;
            state_.recordingStats.currentSpeed = prog.speed; });

        // Set pause/resume callback — when the stream ends (model goes
        // private/offline), keep the output file open and poll for the
        // model to come back. Avoids creating a new file each time.
        bool pauseNotified = false;
        recorder.setPauseResumeCallback([this, &config, &pauseNotified]() -> PauseResumeResult
                                        {
            if (!running_.load() || quitting_.load() || cancelToken_.isCancelled())
                return {PauseAction::Stop, ""};

            Status status;
            try
            {
                status = checkStatus();
            }
            catch (const std::exception &e)
            {
                logger_->debug("Pause check error: {}", e.what());
                return {PauseAction::Wait, ""};
            }

            // Update visible state so UI reflects current status
            setState(status);

            if (!pauseNotified)
            {
                pauseNotified = true;
                setRecording(false);
                logger_->info("Recording paused — waiting for model to return");
            }

            if (status == Status::Public)
            {
                // Model is back! Get fresh video URL
                try
                {
                    std::string url = getVideoUrl();
                    if (!url.empty())
                    {
                        logger_->info("Model is back — resuming recording");
                        setRecording(true);
                        pauseNotified = false;
                        return {PauseAction::Resume, url};
                    }
                }
                catch (const std::exception &e)
                {
                    logger_->debug("Resume URL error: {}", e.what());
                }
                return {PauseAction::Wait, ""};
            }

            // Keep waiting for recoverable statuses
            if (status == Status::Private || status == Status::Online ||
                status == Status::Offline || status == Status::LongOffline ||
                status == Status::RateLimit || status == Status::Cloudflare ||
                status == Status::Error || status == Status::ConnectionError ||
                status == Status::Restricted)
            {
                return {PauseAction::Wait, ""};
            }

            // Stop permanently only for terminal statuses.
            if (status == Status::NotExist || status == Status::Deleted)
                return {PauseAction::Stop, ""};

            // Unknown/unexpected status: keep waiting to preserve append behavior.
            return {PauseAction::Wait, ""}; });

        cancelToken_.reset();

        // Set resolution change callback — when the stream's resolution
        // changes (e.g. model switches mobile↔desktop), close the current
        // file and start a new one in the appropriate subfolder.
        // This detects mobile at the FFmpeg level (portrait = height > width)
        // instead of relying on the SC API which can be slow.
        recorder.setResolutionChangeCallback([this, &config](const ResolutionInfo &ri) -> std::string
                                             {
            logger_->info("Resolution change detected: {}x{} (mobile={})",
                          ri.width, ri.height, ri.isMobile);

            // Update mobile state from the actual video resolution — faster
            // and more reliable than waiting for the next SC API response.
            setMobile(ri.isMobile);

            // Generate a new output path (picks up the Mobile subfolder change)
            return generateOutputPath(config); });

        auto result = recorder.record(videoUrl, outputPath, cancelToken_, config.userAgent);

        setRecording(false);

        bool ok = result.success;

        // Post-download cleanup (Python: _post_download_cleanup)
        ok = postDownloadCleanup(outputPath, ok);

        // Update total bytes
        if (ok)
        {
            std::error_code ec;
            if (fs::exists(outputPath, ec))
            {
                auto fileSize = fs::file_size(outputPath, ec);
                logger_->info("Recording ended successfully: {} ({} bytes)",
                              outputPath, fileSize);
                std::lock_guard lock(stateMutex_);
                state_.totalBytes += fileSize;
            }
        }
        else
        {
            logger_->warn("Recording failed");
        }

        return ok;
    }

    // ─────────────────────────────────────────────────────────────────
    // Interruptible sleep (also interrupted by resync request)
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::sleepInterruptible(int seconds)
    {
        std::unique_lock lock(sleepMutex_);
        sleepCv_.wait_for(lock, std::chrono::seconds(seconds), [this]()
                          { return !running_.load() || quitting_.load() || resyncPending_.load(); });

        if (resyncPending_.load())
        {
            resyncPending_.store(false);
            cancelToken_.reset();
            logger_->info("Sleep interrupted by resync request");
        }
    }

} // namespace sm
