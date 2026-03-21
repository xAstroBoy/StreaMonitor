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
#include "utils/thumbnail_generator.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <regex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>

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
        logger_->set_level(spdlog::get_level()); // inherit global log level
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

        // Wait for the recording thread to fully finish (writes trailer, closes file)
        if (thread_ && thread_->joinable())
        {
            thread_->request_stop();
            thread_->join();
        }
        thread_.reset();

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
        std::lock_guard lock(stateMutex_);
        bool statusChanged = (state_.status != status);
        state_.prevStatus = state_.status;
        state_.status = status;
        // Only update lastStatusChange when status actually changes
        if (statusChanged)
            state_.lastStatusChange = Clock::now();
        // Preview comes from the actual stream (captured by HLS recorder).
        // We no longer use site API preview URLs.
        if (statusChanged && stateCallback_)
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
        streamMobile_.store(mobile, std::memory_order_relaxed);
        std::lock_guard lock(stateMutex_);
        state_.mobile = mobile;
    }

    bool SitePlugin::isMobile() const
    {
        return streamMobile_.load(std::memory_order_relaxed);
    }

    std::string SitePlugin::masterUrl() const
    {
        std::lock_guard lock(masterUrlMutex_);
        return lastMasterUrl_;
    }

    void SitePlugin::setMasterUrl(const std::string &url)
    {
        std::lock_guard lock(masterUrlMutex_);
        lastMasterUrl_ = url;
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
    // Continuous preview — recorder pushes, GUI/MJPEG consumes
    // ─────────────────────────────────────────────────────────────────
    bool SitePlugin::consumePreview(PreviewFrame &out, uint64_t &lastVersion)
    {
        std::lock_guard lock(previewMutex_);
        pumpPreviewQueue_();
        if (previewVersion_ <= lastVersion || pendingPreview_.empty())
            return false;
        // Copy (not move) — multiple consumers may read independently
        out.pixels = pendingPreview_.pixels;
        out.width = pendingPreview_.width;
        out.height = pendingPreview_.height;
        lastVersion = previewVersion_;
        return true;
    }

    bool SitePlugin::waitForPreview(PreviewFrame &out, uint64_t &lastVersion, int timeoutMs)
    {
        using namespace std::chrono;
        auto deadline = steady_clock::now() + milliseconds(timeoutMs);

        std::unique_lock lock(previewMutex_);
        while (steady_clock::now() < deadline)
        {
            pumpPreviewQueue_();
            if (previewVersion_ > lastVersion && !pendingPreview_.empty())
            {
                out.pixels = pendingPreview_.pixels;
                out.width = pendingPreview_.width;
                out.height = pendingPreview_.height;
                lastVersion = previewVersion_;
                return true;
            }
            // Wait briefly for more data (matches pump interval)
            previewCv_.wait_for(lock, milliseconds(33));
        }
        return false;
    }

    // ─────────────────────────────────────────────────────────────────
    // Pump: pop one frame from the queue into pendingPreview_ at an
    // adaptive rate.  Faster when the queue is full (catch up), slower
    // when nearly empty (stretch remaining frames to avoid gaps).
    // Caller MUST hold previewMutex_.
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::pumpPreviewQueue_()
    {
        if (previewQueue_.empty())
            return;

        using namespace std::chrono;
        auto now = steady_clock::now();

        // First frame — deliver immediately
        if (lastPreviewPumpTime_.time_since_epoch().count() == 0)
        {
            pendingPreview_ = std::move(previewQueue_.front());
            previewQueue_.pop_front();
            previewVersion_++;
            lastPreviewPumpTime_ = now;
            return;
        }

        // Adaptive interval based on queue depth:
        //   > 90 frames  →  16 ms (62 fps)  catch up fast
        //   > 60          →  22 ms (45 fps)
        //   > 30          →  33 ms (30 fps)  normal rate
        //   > 10          →  40 ms (25 fps)  conserve
        //   ≤ 10          →  50 ms (20 fps)  stretch
        int64_t intervalMs;
        size_t qs = previewQueue_.size();
        if (qs > 90)
            intervalMs = 16;
        else if (qs > 60)
            intervalMs = 22;
        else if (qs > 30)
            intervalMs = 33;
        else if (qs > 10)
            intervalMs = 40;
        else
            intervalMs = 50;

        auto elapsed = duration_cast<milliseconds>(now - lastPreviewPumpTime_).count();
        if (elapsed < intervalMs)
            return;

        pendingPreview_ = std::move(previewQueue_.front());
        previewQueue_.pop_front();
        previewVersion_++;
        lastPreviewPumpTime_ = now;
    }

    // ─────────────────────────────────────────────────────────────────
    // Audio data callback — set/clear by GUI audio player
    // ─────────────────────────────────────────────────────────────────
    void SitePlugin::setAudioDataCallback(AudioDataCallback cb)
    {
        std::lock_guard lock(audioMutex_);
        audioDataCb_ = std::move(cb);
    }

    void SitePlugin::clearAudioDataCallback()
    {
        std::lock_guard lock(audioMutex_);
        audioDataCb_ = nullptr;
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
        // Store master URL for SegmentFeeder orientation monitoring
        setMasterUrl(masterUrl);

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
    // Output filename generation — supports configurable format (Issue #44)
    // Tokens: {n} = sequential number, {model} = username, {site} = site slug,
    //         {date} = YYYY-MM-DD, {time} = HH-MM-SS, {datetime} = YYYYMMDD-HHMMSS
    // Legacy default "{n}" preserves backward compat (sidecar checking, zero-byte cleanup)
    // ─────────────────────────────────────────────────────────────────

    // Helper: replace all occurrences of a token in a string
    static std::string replaceAll(std::string str, const std::string &token, const std::string &value)
    {
        size_t pos = 0;
        while ((pos = str.find(token, pos)) != std::string::npos)
        {
            str.replace(pos, token.length(), value);
            pos += value.length();
        }
        return str;
    }

    // Helper: expand format tokens (except {n}) with current time and model info
    static std::string expandFormatTokens(const std::string &fmt,
                                          const std::string &username,
                                          const std::string &siteSlug)
    {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        std::tm tm_now{};
#ifdef _WIN32
        localtime_s(&tm_now, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_now);
#endif
        // Format date/time strings
        char dateBuf[16], timeBuf[16], datetimeBuf[32];
        std::strftime(dateBuf, sizeof(dateBuf), "%Y-%m-%d", &tm_now);
        std::strftime(timeBuf, sizeof(timeBuf), "%H-%M-%S", &tm_now);
        std::strftime(datetimeBuf, sizeof(datetimeBuf), "%Y%m%d-%H%M%S", &tm_now);

        std::string result = fmt;
        result = replaceAll(result, "{model}", username);
        result = replaceAll(result, "{site}", siteSlug);
        result = replaceAll(result, "{datetime}", std::string(datetimeBuf));
        result = replaceAll(result, "{date}", std::string(dateBuf));
        result = replaceAll(result, "{time}", std::string(timeBuf));
        return result;
    }

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

        // Determine filename format (default: "{n}" for legacy behavior)
        std::string nameFormat = config.filenameFormat.empty() ? "{n}" : config.filenameFormat;

        // Check if the format uses sequential numbering
        bool usesSequentialNumber = (nameFormat.find("{n}") != std::string::npos);

        if (usesSequentialNumber)
        {
            // ── Sequential numbering mode: gap-finding with sidecar checks ──
            // Expand non-{n} tokens first
            std::string expandedBase = expandFormatTokens(nameFormat, username_, siteSlug_);

            int n = 1;
            while (true)
            {
                std::string filename = replaceAll(expandedBase, "{n}", std::to_string(n));
                auto candidate = dir / (filename + ext);

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

                // Check sidecar files
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
                    std::lock_guard slock(stateMutex_);
                    state_.currentFile = candidate.string();
                    state_.fileCount = n;
                    return candidate.string();
                }

                n++;
            }
        }
        else
        {
            // ── Timestamp/custom mode: unique filename per recording ──
            // Expand all tokens (no {n} present)
            std::string filename = expandFormatTokens(nameFormat, username_, siteSlug_);
            auto candidate = dir / (filename + ext);

            // If file already exists (unlikely with timestamps but handle it),
            // append a counter suffix
            if (fs::exists(candidate, ec))
            {
                int suffix = 1;
                while (true)
                {
                    auto suffixed = dir / (filename + "_" + std::to_string(suffix) + ext);
                    if (!fs::exists(suffixed, ec))
                    {
                        candidate = suffixed;
                        break;
                    }
                    suffix++;
                }
            }

            {
                std::lock_guard slock(stateMutex_);
                state_.currentFile = candidate.string();
                state_.fileCount = 0; // Not using sequential numbering
            }
            return candidate.string();
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

        // Generate thumbnail contact sheet if enabled and download succeeded
        if (ok && config_)
        {
            try
            {
                if (config_->thumbnailEnabled)
                {
                    // Find the actual output file
                    std::string videoFile = finalPath;
                    auto stem = fs::path(finalPath).parent_path() / fs::path(finalPath).stem();
                    std::string tmpTsFile = stem.string() + ".tmp.ts";
                    if (!fs::exists(videoFile) && fs::exists(tmpTsFile))
                        videoFile = tmpTsFile;

                    if (fs::exists(videoFile) && fs::file_size(videoFile) > 0)
                    {
                        auto thumbPath = fs::path(videoFile);
                        thumbPath.replace_extension(".thumb.jpg");

                        sm::ThumbnailConfig tcfg;
                        tcfg.width = config_->thumbnailWidth;
                        tcfg.columns = config_->thumbnailColumns;
                        tcfg.rows = config_->thumbnailRows;

                        auto logCb = [this](const std::string &msg)
                        {
                            logger_->info("{}", msg);
                        };

                        if (sm::generateContactSheet(videoFile, thumbPath.string(), tcfg, logCb))
                        {
                            logger_->info("Contact sheet saved: {}", thumbPath.filename().string());
                            // Embed as cover art inside the MKV (in-place)
                            sm::embedThumbnailInMKV(videoFile, thumbPath.string(), logCb);
                        }
                    }
                }
            }
            catch (const std::exception &e)
            {
                logger_->debug("Thumbnail generation failed: {}", e.what());
            }
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
        try
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
                    if (config.autoRemoveNonExistent)
                    {
                        logger_->error("User {} does not exist - auto-removing", username_);
                        autoRemoveModel("non-existent");
                    }
                    else
                    {
                        logger_->warn("User {} appears non-existent — stopping bot (will retry on manual start)", username_);
                    }
                    running_.store(false);
                    break;
                }

                if (status == Status::Deleted)
                {
                    if (config.autoRemoveNonExistent)
                    {
                        logger_->error("Model account {} has been DELETED - auto-removing", username_);
                        autoRemoveModel("deleted");
                    }
                    else
                    {
                        logger_->warn("Model {} appears deleted — stopping bot (will retry on manual start)", username_);
                    }
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
        catch (const std::exception &e)
        {
            logger_->error("FATAL thread exception for {}: {}", username_, e.what());
        }
        catch (...)
        {
            logger_->error("FATAL unknown thread exception for {}", username_);
        }

        // Ensure state cleanup even after exception
        {
            std::lock_guard lock(stateMutex_);
            state_.running = false;
            state_.recording = false;
            state_.status = Status::NotRunning;
        }
        running_.store(false);
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
            chunkReached_.store(false);
            bool success = downloadOnce(config);

            if (!running_.load() || quitting_.load())
                break;

            // If a chunk boundary was hit, immediately start the next file
            // without sleeping or re-checking status — the model is still live.
            if (chunkReached_.load())
            {
                chunkReached_.store(false);
                logger_->info("Chunk boundary reached — starting next file");
                continue; // go straight to next downloadOnce
            }

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

        // ── Continuous preview from the live stream ─────────────────
        // Recorder pushes EVERY decoded RGBA frame.  We queue them
        // and a pump function drains the queue at a steady rate so
        // the GUI sees smooth video instead of bursty segment dumps.
        // Only enabled when enablePreviewCapture is true (saves CPU).
        if (config.enablePreviewCapture)
        {
            recorder.setPreviewDataCallback([this](std::vector<uint8_t> rgba, int w, int h)
                                            {
                std::lock_guard lock(previewMutex_);
                PreviewFrame f;
                f.pixels = std::move(rgba);
                f.width  = w;
                f.height = h;
                previewQueue_.push_back(std::move(f));
                // Cap queue so memory doesn't grow unbounded
                while (previewQueue_.size() > kMaxPreviewQueue)
                    previewQueue_.pop_front();
                previewCv_.notify_all(); });
        }

        // ── Audio forwarding ────────────────────────────────────────
        // Recorder pushes f32 stereo 48kHz PCM. Forward to whoever
        // registered the audio callback (typically the GUI AudioPlayer).
        recorder.setAudioDataCallback([this](const float *samples, size_t frameCount)
                                      {
            std::lock_guard lock(audioMutex_);
            if (audioDataCb_)
                audioDataCb_(samples, frameCount); });

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

        // ── Status check for SegmentFeeder early abort ────────────
        // When consecutive segment downloads fail, the feeder calls
        // this to check if the model went private/offline so it can
        // abort immediately instead of grinding through 30 errors.
        recorder.setStatusCheckCallback([this]() -> Status
                                        { return checkStatus(); });

        cancelToken_.reset();
        chunkReached_.store(false);

        // ── Chunk-limit tracking ────────────────────────────────────
        // For chunked recording modes, we monitor progress and cancel
        // the token when the size/duration limit is reached.
        auto chunkStartTime = std::chrono::steady_clock::now();
        recorder.setProgressCallback([this, &config, chunkStartTime](const RecordingProgress &prog)
                                     {
            // Update stats silently (same as before)
            {
                std::lock_guard lock(stateMutex_);
                state_.recordingStats.bytesWritten = prog.bytesWritten;
                state_.recordingStats.currentSpeed = prog.speed;
            }

            // Check chunk limits
            if (config.recordingMode == 1)
            {
                // Chunked by file size
                uint64_t limitBytes = static_cast<uint64_t>(config.chunkSizeMB) * 1024ULL * 1024ULL;
                if (prog.bytesWritten >= limitBytes)
                {
                    chunkReached_.store(true);
                    cancelToken_.cancel();
                }
            }
            else if (config.recordingMode == 2)
            {
                // Chunked by duration
                auto elapsed = std::chrono::steady_clock::now() - chunkStartTime;
                auto elapsedMin = std::chrono::duration_cast<std::chrono::minutes>(elapsed).count();
                if (elapsedMin >= config.chunkDurationMin)
                {
                    chunkReached_.store(true);
                    cancelToken_.cancel();
                }
            } });
        // changes (e.g. model switches mobile↔desktop), close the current
        // file and start a new one in the appropriate subfolder.
        // Mobile detection is ALWAYS from the actual stream resolution
        // (portrait = isPortraitStream: h > w, ratio < 0.85).
        // The site API's isMobile flag is NEVER trusted for this.
        recorder.setResolutionChangeCallback([this, &config](const ResolutionInfo &ri) -> std::string
                                             {
            // VR sites (slug ends with "VR") are NEVER mobile — ignore
            // portrait detection for them.
            bool vrSite = siteSlug_.size() >= 2 &&
                          siteSlug_.compare(siteSlug_.size() - 2, 2, "VR") == 0;
            bool mobile = ri.isMobile && !vrSite;

            logger_->info("Resolution: {}x{} → mobile={} (detected via {})",
                          ri.width, ri.height, mobile,
                          ri.source == ResolutionInfo::Source::MasterPlaylist
                              ? "master-playlist" : "codec-params");

            // Update mobile state from the actual video resolution.
            // This is the ONLY source of truth for mobile detection.
            setMobile(mobile);

            // Generate a new output path (picks up the Mobile subfolder change)
            return generateOutputPath(config); });

        // Pass masterUrl for SegmentFeeder orientation monitoring.
        // The feeder will periodically re-fetch the master m3u8 and
        // detect portrait↔landscape changes from RESOLUTION= tags.
        auto result = recorder.record(videoUrl, outputPath, cancelToken_,
                                      config.userAgent, "", {}, {}, masterUrl());

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
