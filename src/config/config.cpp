#include "config/config.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <cstdlib>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // JSON serialization for ModelConfig
    // ─────────────────────────────────────────────────────────────────
    void to_json(nlohmann::json &j, const ModelConfig &m)
    {
        j = nlohmann::json{
            {"site", m.site},
            {"username", m.username},
            {"running", m.running},
            {"status", statusToString(m.lastStatus)},
            {"recording", m.recording}};
        if (m.gender != Gender::Unknown)
            j["gender"] = static_cast<int>(m.gender);
        if (!m.country.empty())
            j["country"] = m.country;
        if (!m.crossRegisterGroup.empty())
            j["crossRegisterGroup"] = m.crossRegisterGroup;
        // VR config (only persist if it's a VR model)
        if (m.vrConfig.isVR())
        {
            j["vr"] = {
                {"projection", vrProjectionToString(m.vrConfig.projection)},
                {"stereoMode", vrStereoModeToString(m.vrConfig.stereoMode)},
                {"fov", m.vrConfig.fov},
                {"embedMetadata", m.vrConfig.embedMetadata}};
            if (!m.vrConfig.customTags.empty())
                j["vr"]["customTags"] = m.vrConfig.customTags;
        }
    }

    static Status parseStatusString(const std::string &s)
    {
        if (s == "PUBLIC")
            return Status::Public;
        if (s == "PRIVATE")
            return Status::Private;
        if (s == "OFFLINE")
            return Status::Offline;
        if (s == "ONLINE")
            return Status::Online;
        if (s == "ERROR")
            return Status::Error;
        if (s == "CONNECTION_ERROR" || s == "CONNECTIONERROR")
            return Status::ConnectionError;
        if (s == "RATELIMIT")
            return Status::RateLimit;
        if (s == "NOTEXIST")
            return Status::NotExist;
        if (s == "DELETED")
            return Status::Deleted;
        if (s == "RESTRICTED")
            return Status::Restricted;
        if (s == "CLOUDFLARE")
            return Status::Cloudflare;
        if (s == "LONG_OFFLINE")
            return Status::LongOffline;
        return Status::Unknown;
    }

    void from_json(const nlohmann::json &j, ModelConfig &m)
    {
        j.at("site").get_to(m.site);
        j.at("username").get_to(m.username);
        m.running = j.value("running", true);
        m.recording = j.value("recording", false);
        m.lastStatus = parseStatusString(j.value("status", "OFFLINE"));
        if (j.contains("gender"))
            m.gender = static_cast<Gender>(j["gender"].get<int>());
        if (j.contains("country"))
            j.at("country").get_to(m.country);
        if (j.contains("crossRegisterGroup"))
            j.at("crossRegisterGroup").get_to(m.crossRegisterGroup);
        // VR config
        if (j.contains("vr") && j["vr"].is_object())
        {
            auto &vr = j["vr"];
            m.vrConfig.projection = parseVRProjection(vr.value("projection", "none"));
            m.vrConfig.stereoMode = parseVRStereoMode(vr.value("stereoMode", "mono"));
            m.vrConfig.fov = vr.value("fov", 180);
            m.vrConfig.embedMetadata = vr.value("embedMetadata", true);
            if (vr.contains("customTags") && vr["customTags"].is_object())
            {
                m.vrConfig.customTags = vr["customTags"].get<std::map<std::string, std::string>>();
            }
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // ModelConfigStore
    // ─────────────────────────────────────────────────────────────────
    void ModelConfigStore::load(const std::filesystem::path &path)
    {
        std::lock_guard lock(mutex_);
        lastPath_ = path;
        models_.clear();

        if (!std::filesystem::exists(path))
        {
            spdlog::info("Config file not found, starting fresh: {}", path.string());
            return;
        }

        try
        {
            std::ifstream f(path);
            auto j = nlohmann::json::parse(f);
            if (j.is_array())
            {
                models_ = j.get<std::vector<ModelConfig>>();
            }
            spdlog::info("Loaded {} models from config", models_.size());
        }
        catch (const std::exception &e)
        {
            spdlog::error("Failed to load config: {}", e.what());
        }
    }

    void ModelConfigStore::save(const std::filesystem::path &path) const
    {
        std::lock_guard lock(mutex_);
        try
        {
            nlohmann::json j = models_;
            std::ofstream f(path);
            f << j.dump(2);
            spdlog::debug("Saved {} models to config", models_.size());
        }
        catch (const std::exception &e)
        {
            spdlog::error("Failed to save config: {}", e.what());
        }
    }

    void ModelConfigStore::save() const
    {
        if (!lastPath_.empty())
            save(lastPath_);
    }

    void ModelConfigStore::add(const ModelConfig &model)
    {
        std::lock_guard lock(mutex_);
        // Check for duplicate
        for (const auto &m : models_)
        {
            if (m.username == model.username && m.site == model.site)
                return;
        }
        models_.push_back(model);
        spdlog::info("Added model: {} [{}]", model.username, model.site);
    }

    bool ModelConfigStore::remove(const std::string &username, const std::string &siteslug)
    {
        std::lock_guard lock(mutex_);
        auto it = std::remove_if(models_.begin(), models_.end(),
                                 [&](const ModelConfig &m)
                                 {
                                     if (siteslug.empty())
                                         return m.username == username;
                                     return m.username == username && m.site == siteslug;
                                 });
        if (it != models_.end())
        {
            models_.erase(it, models_.end());
            return true;
        }
        return false;
    }

    void ModelConfigStore::updateStatus(const std::string &username, const std::string &siteslug,
                                        Status status, bool recording)
    {
        std::lock_guard lock(mutex_);
        for (auto &m : models_)
        {
            if (m.username == username && (siteslug.empty() || m.site == siteslug))
            {
                m.lastStatus = status;
                m.recording = recording;
            }
        }
    }

    std::vector<ModelConfig> ModelConfigStore::getAll() const
    {
        std::lock_guard lock(mutex_);
        return models_;
    }

    void ModelConfigStore::setCrossRegisterGroup(const std::string &username, const std::string &site,
                                                 const std::string &groupName)
    {
        std::lock_guard lock(mutex_);
        for (auto &m : models_)
        {
            if (m.username == username && (site.empty() || m.site == site))
                m.crossRegisterGroup = groupName;
        }
    }

    std::optional<ModelConfig> ModelConfigStore::find(const std::string &username,
                                                      const std::string &siteslug) const
    {
        std::lock_guard lock(mutex_);
        for (const auto &m : models_)
        {
            if (m.username == username && (siteslug.empty() || m.site == siteslug))
                return m;
        }
        return std::nullopt;
    }

    size_t ModelConfigStore::count() const
    {
        std::lock_guard lock(mutex_);
        return models_.size();
    }

    // ─────────────────────────────────────────────────────────────────
    // AppConfig
    // ─────────────────────────────────────────────────────────────────
    static std::string getEnv(const char *name, const std::string &defaultVal = "")
    {
        const char *val = std::getenv(name);
        return val ? std::string(val) : defaultVal;
    }

    static int getEnvInt(const char *name, int defaultVal)
    {
        const char *val = std::getenv(name);
        if (!val)
            return defaultVal;
        try
        {
            return std::stoi(val);
        }
        catch (...)
        {
            return defaultVal;
        }
    }

    static float getEnvFloat(const char *name, float defaultVal)
    {
        const char *val = std::getenv(name);
        if (!val)
            return defaultVal;
        try
        {
            return std::stof(val);
        }
        catch (...)
        {
            return defaultVal;
        }
    }

    static bool getEnvBool(const char *name, bool defaultVal)
    {
        const char *val = std::getenv(name);
        if (!val)
            return defaultVal;
        std::string s(val);
        return s == "1" || s == "true" || s == "True" || s == "yes";
    }

    void AppConfig::loadFromEnv()
    {
        downloadsDir = getEnv("STRMNTR_DOWNLOAD_DIR", downloadsDir.string());
        container = parseContainerFormat(getEnv("STRMNTR_CONTAINER", "mkv"));
        wantedResolution = getEnvInt("STRMNTR_RESOLUTION", wantedResolution);
        ffmpegReadRate = getEnvBool("STRMNTR_FFMPEG_READRATE", ffmpegReadRate);
        ffmpegPath = getEnv("STRMNTR_FFMPEG_PATH", ffmpegPath.string());
        userAgent = getEnv("STRMNTR_USER_AGENT", userAgent);
        verifySsl = getEnvBool("STRMNTR_VERIFY_SSL", verifySsl);
        webHost = getEnv("STRMNTR_HOST", webHost);
        webPort = getEnvInt("STRMNTR_PORT", webPort);
        webUsername = getEnv("STRMNTR_USERNAME", webUsername);
        webPassword = getEnv("STRMNTR_PASSWORD", webPassword);
        webEnabled = getEnvBool("STRMNTR_WEB_ENABLED", webEnabled);
        minFreeDiskPercent = getEnvFloat("STRMNTR_MIN_FREE_SPACE", minFreeDiskPercent);
        debug = getEnvBool("STRMNTR_DEBUG", debug);

        // Proxy from env (single proxy for backward compatibility)
        std::string envProxy = getEnv("STRMNTR_PROXY", "");
        if (!envProxy.empty())
        {
            proxyEnabled = true;
            ProxyEntry entry;
            entry.url = envProxy;
            // Auto-detect type from URL scheme
            if (envProxy.find("socks5h://") == 0)
                entry.type = ProxyType::SOCKS5H;
            else if (envProxy.find("socks5://") == 0)
                entry.type = ProxyType::SOCKS5;
            else if (envProxy.find("socks4a://") == 0)
                entry.type = ProxyType::SOCKS4A;
            else if (envProxy.find("socks4://") == 0)
                entry.type = ProxyType::SOCKS4;
            else if (envProxy.find("https://") == 0)
                entry.type = ProxyType::HTTPS;
            else
                entry.type = ProxyType::HTTP;
            proxies.clear();
            proxies.push_back(entry);
        }

        int segTime = getEnvInt("STRMNTR_SEGMENT_TIME", 0);
        segmentTimeSec = segTime > 0 ? segTime : 0;

        std::string resPref = getEnv("STRMNTR_RESOLUTION_PREF", "closest");
        if (resPref == "exact")
            resolutionPref = ResolutionPref::Exact;
        else if (resPref == "exact_or_least_higher")
            resolutionPref = ResolutionPref::ExactOrLeastHigher;
        else if (resPref == "exact_or_highest_lower")
            resolutionPref = ResolutionPref::ExactOrHighestLower;
        else
            resolutionPref = ResolutionPref::Closest;

        // Encoding config from environment
        std::string encType = getEnv("STRMNTR_ENCODER", "");
        if (!encType.empty())
            encoding.encoder = parseEncoderType(encType);
        encoding.enableCuda = getEnvBool("STRMNTR_CUDA", encoding.enableCuda);
        encoding.crf = getEnvInt("STRMNTR_CRF", encoding.crf);
        encoding.preset = getEnv("STRMNTR_PRESET", encoding.preset);
        encoding.copyAudio = getEnvBool("STRMNTR_COPY_AUDIO", encoding.copyAudio);
        encoding.audioBitrate = getEnvInt("STRMNTR_AUDIO_BITRATE", encoding.audioBitrate);
        encoding.threads = getEnvInt("STRMNTR_ENCODER_THREADS", encoding.threads);
    }

    void AppConfig::loadFromFile(const std::filesystem::path &path)
    {
        if (!std::filesystem::exists(path))
            return;
        try
        {
            std::ifstream f(path);
            auto j = nlohmann::json::parse(f);
            if (j.contains("downloads_dir"))
                downloadsDir = j["downloads_dir"].get<std::string>();
            if (j.contains("container"))
                container = parseContainerFormat(j["container"]);
            if (j.contains("resolution"))
                wantedResolution = j["resolution"];
            if (j.contains("ffmpeg_path"))
                ffmpegPath = j["ffmpeg_path"].get<std::string>();
            if (j.contains("user_agent"))
                userAgent = j["user_agent"];
            if (j.contains("verify_ssl"))
                verifySsl = j["verify_ssl"];
            if (j.contains("minimize_to_tray"))
                minimizeToTray = j["minimize_to_tray"];
            if (j.contains("auto_start_on_login"))
                autoStartOnLogin = j["auto_start_on_login"];
            if (j.contains("debug"))
                debug = j["debug"];
            if (j.contains("segment_time"))
                segmentTimeSec = j["segment_time"];
            if (j.contains("web_host"))
                webHost = j["web_host"];
            if (j.contains("web_port"))
                webPort = j["web_port"];
            if (j.contains("web_username"))
                webUsername = j["web_username"];
            if (j.contains("web_password"))
                webPassword = j["web_password"];
            if (j.contains("web_enabled"))
                webEnabled = j["web_enabled"];
            if (j.contains("web_static_dir"))
                webStaticDir = j["web_static_dir"].get<std::string>();

            // Proxy config — new array format with backward compatibility
            if (j.contains("proxy_enabled"))
                proxyEnabled = j["proxy_enabled"];
            if (j.contains("proxy_max_failures"))
                proxyMaxFailures = j["proxy_max_failures"];
            if (j.contains("proxy_disable_sec"))
                proxyDisableSec = j["proxy_disable_sec"];
            if (j.contains("proxy_auto_disable"))
                proxyAutoDisable = j["proxy_auto_disable"];

            proxies.clear();
            if (j.contains("proxies") && j["proxies"].is_array())
            {
                // New format: array of proxy objects
                for (const auto &pj : j["proxies"])
                {
                    ProxyEntry entry;
                    entry.url = pj.value("url", "");
                    entry.type = parseProxyType(pj.value("type", "http"));
                    entry.rolling = pj.value("rolling", false);
                    entry.enabled = pj.value("enabled", true);
                    entry.name = pj.value("name", "");
                    if (!entry.url.empty())
                        proxies.push_back(entry);
                }
            }
            else if (j.contains("proxy_url") && !j["proxy_url"].get<std::string>().empty())
            {
                // Legacy single proxy format — convert to array
                ProxyEntry entry;
                entry.url = j["proxy_url"].get<std::string>();
                if (j.contains("proxy_type"))
                {
                    entry.type = parseProxyType(j["proxy_type"].get<std::string>());
                }
                proxies.push_back(entry);
            }

            // Encoding config
            if (j.contains("encoding") && j["encoding"].is_object())
            {
                auto &enc = j["encoding"];
                if (enc.contains("encoder"))
                    encoding.encoder = parseEncoderType(enc["encoder"].get<std::string>());
                encoding.enableCuda = enc.value("enable_cuda", encoding.enableCuda);
                encoding.crf = enc.value("crf", encoding.crf);
                encoding.preset = enc.value("preset", encoding.preset);
                encoding.copyAudio = enc.value("copy_audio", encoding.copyAudio);
                encoding.audioBitrate = enc.value("audio_bitrate", encoding.audioBitrate);
                encoding.maxWidth = enc.value("max_width", encoding.maxWidth);
                encoding.maxHeight = enc.value("max_height", encoding.maxHeight);
                encoding.threads = enc.value("threads", encoding.threads);
            }

            // Per-site VR defaults
            if (j.contains("site_vr_defaults") && j["site_vr_defaults"].is_object())
            {
                for (auto &[slug, vrj] : j["site_vr_defaults"].items())
                {
                    VRConfig vr;
                    vr.projection = parseVRProjection(vrj.value("projection", "none"));
                    vr.stereoMode = parseVRStereoMode(vrj.value("stereoMode", "mono"));
                    vr.fov = vrj.value("fov", 180);
                    vr.embedMetadata = vrj.value("embedMetadata", true);
                    if (vrj.contains("customTags") && vrj["customTags"].is_object())
                    {
                        vr.customTags = vrj["customTags"].get<std::map<std::string, std::string>>();
                    }
                    siteVRDefaults[slug] = vr;
                }
            }
            else
            {
                // Set sensible defaults for known VR sites
                VRConfig scvrDefault;
                scvrDefault.projection = VRProjection::Fisheye180;
                scvrDefault.stereoMode = VRStereoMode::SideBySide;
                scvrDefault.fov = 180;
                siteVRDefaults["SCVR"] = scvrDefault;

                VRConfig dcvrDefault;
                dcvrDefault.projection = VRProjection::Fisheye180;
                dcvrDefault.stereoMode = VRStereoMode::SideBySide;
                dcvrDefault.fov = 180;
                siteVRDefaults["DCVR"] = dcvrDefault;
            }
        }
        catch (const std::exception &e)
        {
            spdlog::error("Failed to load app config: {}", e.what());
        }
    }

    void AppConfig::saveToFile(const std::filesystem::path &path) const
    {
        nlohmann::json j;
        j["downloads_dir"] = downloadsDir.string();
        j["container"] = (container == ContainerFormat::MP4) ? "mp4" : (container == ContainerFormat::TS) ? "ts"
                                                                                                          : "mkv";
        j["resolution"] = wantedResolution;
        j["ffmpeg_path"] = ffmpegPath.string();
        j["user_agent"] = userAgent;
        j["verify_ssl"] = verifySsl;
        j["minimize_to_tray"] = minimizeToTray;
        j["auto_start_on_login"] = autoStartOnLogin;
        j["debug"] = debug;
        j["segment_time"] = segmentTimeSec;
        j["web_host"] = webHost;
        j["web_port"] = webPort;
        j["web_username"] = webUsername;
        j["web_password"] = webPassword;
        j["web_enabled"] = webEnabled;
        j["web_static_dir"] = webStaticDir;

        // Proxy config — new array format
        j["proxy_enabled"] = proxyEnabled;
        j["proxy_max_failures"] = proxyMaxFailures;
        j["proxy_disable_sec"] = proxyDisableSec;
        j["proxy_auto_disable"] = proxyAutoDisable;

        nlohmann::json proxyArr = nlohmann::json::array();
        for (const auto &proxy : proxies)
        {
            nlohmann::json pj;
            pj["url"] = proxy.url;
            pj["type"] = proxyTypeToString(proxy.type);
            pj["rolling"] = proxy.rolling;
            pj["enabled"] = proxy.enabled;
            if (!proxy.name.empty())
                pj["name"] = proxy.name;
            proxyArr.push_back(pj);
        }
        j["proxies"] = proxyArr;

        // Per-site VR defaults
        nlohmann::json vrDefaults;
        for (const auto &[slug, vr] : siteVRDefaults)
        {
            vrDefaults[slug] = {
                {"projection", vrProjectionToString(vr.projection)},
                {"stereoMode", vrStereoModeToString(vr.stereoMode)},
                {"fov", vr.fov},
                {"embedMetadata", vr.embedMetadata}};
            if (!vr.customTags.empty())
                vrDefaults[slug]["customTags"] = vr.customTags;
        }
        j["site_vr_defaults"] = vrDefaults;

        // Encoding config
        j["encoding"] = {
            {"encoder", encoderTypeToString(encoding.encoder)},
            {"enable_cuda", encoding.enableCuda},
            {"crf", encoding.crf},
            {"preset", encoding.preset},
            {"copy_audio", encoding.copyAudio},
            {"audio_bitrate", encoding.audioBitrate},
            {"max_width", encoding.maxWidth},
            {"max_height", encoding.maxHeight},
            {"threads", encoding.threads}};

        std::ofstream f(path);
        f << j.dump(2);
    }

} // namespace sm
