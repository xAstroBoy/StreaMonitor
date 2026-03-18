#include "sites/stripchat.h"
#include "net/mouflon.h"
#include "net/m3u8_parser.h"
#include <random>
#include <algorithm>
#include <regex>

namespace sm
{

    REGISTER_SITE(StripChat);

    // Static members for one-time mouflon init
    bool StripChat::mouflonInitialized_ = false;
    std::mutex StripChat::mouflonInitMutex_;

    // Generate 16 random alphanumeric chars (Python: uniq parameter)
    static std::string generateUniq()
    {
        static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
        // thread_local to avoid data races from multiple bot threads
        thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<> dist(0, sizeof(chars) - 2);
        std::string result;
        result.reserve(16);
        for (int i = 0; i < 16; i++)
            result += chars[dist(rng)];
        return result;
    }

    StripChat::StripChat(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnRateLimit_ = 120;
        sleepOnError_ = 5; // Fast retry — outer loop gets fresh URL from different CDN edge
        maxConsecutiveErrors_ = 200;
        // Mouflon init is lazy — done on first checkStatus/getVideoUrl call
        // so it doesn't block the UI thread at startup.
    }

    StripChat::StripChat(const std::string &siteName, const std::string &siteSlug,
                         const std::string &username)
        : SitePlugin(siteName, siteSlug, username)
    {
        sleepOnRateLimit_ = 120;
        sleepOnError_ = 5;
        maxConsecutiveErrors_ = 200;
        // Mouflon init is lazy — done on first checkStatus/getVideoUrl call
    }

    void StripChat::ensureMouflonInit()
    {
        std::lock_guard lock(mouflonInitMutex_);
        if (!mouflonInitialized_)
        {
            HttpClient tmpHttp;
            tmpHttp.setDefaultUserAgent(
                "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:135.0) Gecko/20100101 Firefox/135.0");
            MouflonKeys::instance().initialize(tmpHttp);
            mouflonInitialized_ = true;
        }
    }

    std::string StripChat::getWebsiteUrl() const
    {
        return "https://stripchat.com/" + username();
    }

    std::string StripChat::getPreviewUrl() const
    {
        // Extract from cached API response: user.user.previewUrl or snapshotUrl
        try
        {
            if (lastInfo_.is_null())
                return "";
            auto userOuter = lastInfo_.value("user", nlohmann::json::object());
            auto userInner = userOuter.value("user", nlohmann::json::object());
            std::string url = userInner.value("previewUrl", "");
            if (url.empty())
                url = userInner.value("snapshotUrl", "");
            if (url.empty())
                url = userInner.value("avatarUrl", "");
            return url;
        }
        catch (...)
        {
            return "";
        }
    }

    Status StripChat::checkStatus()
    {
        // Lazy mouflon init — runs once, on first status check (not in constructor)
        ensureMouflonInit();

        // Python: /api/front/v2/models/username/{username}/cam?uniq={16random}
        std::string url = "https://stripchat.com/api/front/v2/models/username/" +
                          username() + "/cam?uniq=" + generateUniq();

        auto resp = http().get(url, 30);

        // Connection error (DNS, timeout, etc.) — NOT a rate limit!
        if (resp.isConnectionError())
        {
            setLastError("Connection error: " + resp.error, 0);
            logger_->warn("Connection error: {}", resp.error);
            return Status::ConnectionError;
        }

        if (resp.isNotFound())
        {
            setLastError("User not found (404)", resp.statusCode);
            logger_->info("User not found");
            return Status::NotExist;
        }
        if (resp.statusCode == 403)
        {
            // Python: check for Cloudflare
            if (resp.body.find("cloudflare") != std::string::npos ||
                resp.body.find("Cloudflare") != std::string::npos)
            {
                setLastError("Cloudflare challenge detected", resp.statusCode);
                return Status::Cloudflare;
            }
            setLastError("Forbidden (403)", resp.statusCode);
            return Status::Restricted;
        }
        if (resp.isRateLimit())
        {
            setLastError("Rate limited (429)", resp.statusCode);
            return Status::RateLimit;
        }
        if (resp.statusCode >= 500)
        {
            if (resp.body.find("cloudflare") != std::string::npos ||
                resp.body.find("Cloudflare") != std::string::npos)
            {
                setLastError("Cloudflare 5xx error", resp.statusCode);
                return Status::Cloudflare;
            }
            setLastError("Server error: " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::RateLimit;
        }
        if (!resp.ok())
        {
            setLastError("HTTP error: " + std::to_string(resp.statusCode), resp.statusCode);
            logger_->warn("HTTP {}", resp.statusCode);
            return Status::Error; // Unknown HTTP error, not necessarily rate limit
        }

        try
        {
            auto json = nlohmann::json::parse(resp.body);
            lastInfo_ = json;
            setLastApiResponse(resp.body); // Store for inspection

            // Python JSON structure: json["user"]["user"] for user data
            auto userOuter = json.value("user", nlohmann::json::object());
            auto userInner = userOuter.value("user", nlohmann::json::object());

            // Check isDeleted at user.user level
            bool isDeleted = userInner.value("isDeleted", false);
            if (isDeleted)
                return Status::Deleted;

            // Check isGeoBanned at user level
            bool isGeoBanned = userOuter.value("isGeoBanned", false);
            if (isGeoBanned)
                return Status::Restricted;

            // Status from user.user.status (the authoritative status field)
            std::string status = userInner.value("status", "");
            std::string statusLower = status;
            std::transform(statusLower.begin(), statusLower.end(), statusLower.begin(), ::tolower);

            // Model ID from user.user.id
            modelId_ = userInner.value("id", (int64_t)0);

            // Gender detection - priority: broadcastGender > genderCategory > user
            std::string genderStr;
            if (userInner.contains("broadcastGender") && userInner["broadcastGender"].is_string())
                genderStr = userInner["broadcastGender"].get<std::string>();
            else if (userInner.contains("genderCategory") && userInner["genderCategory"].is_string())
                genderStr = userInner["genderCategory"].get<std::string>();
            else
                genderStr = userInner.value("gender", "");

            std::transform(genderStr.begin(), genderStr.end(), genderStr.begin(), ::tolower);
            if (genderStr == "female")
                setGender(Gender::Female);
            else if (genderStr == "male")
                setGender(Gender::Male);
            else if (genderStr == "couple")
                setGender(Gender::Couple);
            else if (genderStr == "trans")
                setGender(Gender::Trans);

            // Country
            std::string country = userInner.value("country", "");
            if (!country.empty())
                setCountry(country);

            // Mobile detection from cam-level data (json["cam"])
            auto cam = json.value("cam", nlohmann::json::object());
            auto broadcastSettings = cam.value("broadcastSettings", nlohmann::json::object());

            isMobile_ = broadcastSettings.value("isMobile", false);
            if (!isMobile_)
                isMobile_ = userInner.value("isMobile", false);
            setMobile(isMobile_);

            // NOTE: isVr_ is NOT set here. The base StripChat class always
            // records the non-VR stream. Only StripChatVR sets isVr_ = true
            // in its own checkStatus() override.

            // Stream name from cam level (json["cam"]["streamName"])
            hlsStreamName_ = cam.value("streamName", "");

            // isCamAvailable (only true during actual public streaming)
            bool isCamAvailable = cam.value("isCamAvailable", false);
            // isLive from user.user
            bool isLive = userInner.value("isLive", false);

            // Python status mapping
            if (statusLower == "public")
            {
                if (isCamAvailable || isLive)
                    return Status::Public;
                return Status::Online; // public but not streaming yet
            }

            if (statusLower == "private" || statusLower == "groupshow" ||
                statusLower == "p2p" || statusLower == "virtualprivate" ||
                statusLower == "p2pvoice" || statusLower == "p2pvideo" ||
                statusLower == "recordingprivate")
                return Status::Private;

            if (statusLower == "off" || statusLower == "idle")
                return Status::Offline;

            if (statusLower == "connected")
                return Status::Online;

            logger_->debug("Unknown StripChat status: {}", status);
            return Status::Offline;
        }
        catch (const std::exception &e)
        {
            setLastError(std::string("JSON parse error: ") + e.what(), 0);
            logger_->error("Parse error: {}", e.what());
            return Status::RateLimit;
        }
    }

    std::string StripChat::getVideoUrl()
    {
        ensureMouflonInit();

        if (hlsStreamName_.empty())
        {
            logger_->warn("Missing stream name");
            return "";
        }

        // Use the mouflon key system to get authenticated playlist URLs
        auto url = getPlaylistWithKeys();
        if (!url.empty())
            return url;

        // Fallback: try without mouflon (will likely get ads)
        logger_->warn("CDN playlist unavailable with mouflon keys, trying unauthenticated fallback");

        std::string vr = isVr_ ? "_vr" : "";
        std::string autoSuffix = isVr_ ? "" : "_auto";

        static const std::vector<std::string> tlds = {"org", "com", "net", "live"};
        for (const auto &tld : tlds)
        {
            std::string masterUrl = "https://edge-hls.doppiocdn." + tld +
                                    "/hls/" + hlsStreamName_ + vr +
                                    "/master/" + hlsStreamName_ + vr + autoSuffix + ".m3u8";
            auto testResp = http().get(masterUrl, 10);
            if (testResp.ok())
                return selectResolution(masterUrl);
        }

        return "";
    }

    std::string StripChat::getPlaylistWithKeys()
    {
        auto &mouflon = MouflonKeys::instance();

        std::string vr = isVr_ ? "_vr" : "";
        std::string autoSuffix = isVr_ ? "" : "_auto";

        // CDN hosts to try (Python shuffles these)
        std::vector<std::string> cdnHosts = {"doppiocdn.org", "doppiocdn.com", "doppiocdn.net", "doppiocdn.live"};
        {
            static std::mt19937 rng(std::random_device{}());
            std::shuffle(cdnHosts.begin(), cdnHosts.end(), rng);
        }

        // Retry loop: model might have just gone live, CDN propagation lags
        constexpr int maxAttempts = 5;
        constexpr int retryDelaySec = 3;

        for (int attempt = 0; attempt < maxAttempts; attempt++)
        {
            HttpResponse result;
            std::string playlistUrl;

            for (const auto &host : cdnHosts)
            {
                playlistUrl = "https://edge-hls." + host +
                              "/hls/" + hlsStreamName_ + vr +
                              "/master/" + hlsStreamName_ + vr + autoSuffix + ".m3u8";

                logger_->debug("Fetching playlist from: {}", playlistUrl);
                result = http().get(playlistUrl, 10);

                if (result.ok())
                {
                    logger_->debug("Playlist OK from {}", host);
                    break;
                }
                else
                {
                    logger_->debug("Playlist {} from {}", result.statusCode, host);
                    result = HttpResponse{}; // reset
                }
            }

            if (!result.ok())
            {
                if (attempt < maxAttempts - 1)
                {
                    logger_->info("Playlist not on any CDN yet, waiting {}s... (attempt {}/{})",
                                  retryDelaySec, attempt + 1, maxAttempts);
                    std::this_thread::sleep_for(std::chrono::seconds(retryDelaySec));
                    std::shuffle(cdnHosts.begin(), cdnHosts.end(), std::mt19937(std::random_device{}()));
                    continue;
                }
                logger_->error("Failed to fetch playlist from any CDN host");
                return "";
            }

            auto m3u8Doc = result.body;
            logger_->debug("M3U8 content (first 200): {}", m3u8Doc.substr(0, 200));

            // Extract mouflon keys from the master playlist
            auto mouflonInfo = mouflon.extractFromPlaylist(m3u8Doc);
            logger_->debug("Mouflon: psch={}, pkey={}, pdkey={}",
                           mouflonInfo.psch,
                           mouflonInfo.pkey,
                           mouflonInfo.pdkey.empty() ? "(none)" : "***");

            if (mouflonInfo.pkey.empty())
            {
                logger_->warn("No mouflon pkey found in playlist — keys may not be extracted");
                auto keys = mouflon.getKeys();
                logger_->debug("Known keys: {}", keys.size());
            }

            // Parse master playlist to get variants
            if (!M3U8Parser::isMasterPlaylist(m3u8Doc))
            {
                // It's already a media playlist — add keys if we have them
                if (!mouflonInfo.pkey.empty())
                    return mouflon.rewriteVariantUrl(playlistUrl, mouflonInfo.psch,
                                                     mouflonInfo.pkey, mouflonInfo.pdkey);
                return playlistUrl;
            }

            auto master = M3U8Parser::parseMaster(m3u8Doc, playlistUrl);
            if (master.variants.empty())
            {
                logger_->warn("No variants in master playlist");
                return "";
            }

            // Select best resolution
            int wantedRes = config_ ? config_->wantedResolution : 99999;
            ResolutionPref pref = config_ ? config_->resolutionPref : ResolutionPref::Closest;
            auto selected = M3U8Parser::selectVariant(master, wantedRes, pref);

            if (!selected)
            {
                logger_->error("Could not select a resolution variant");
                return "";
            }

            logger_->info("Selected quality: {}x{}", selected->width, selected->height);

            // Store selected resolution for stats reporting
            setRecordingResolution(selected->width, selected->height);

            // Resolve the variant URL
            auto variantUrl = M3U8Parser::resolveUrl(playlistUrl, selected->url);

            // Rewrite with mouflon keys for authentication
            if (!mouflonInfo.pkey.empty())
            {
                variantUrl = mouflon.rewriteVariantUrl(variantUrl, mouflonInfo.psch,
                                                       mouflonInfo.pkey, mouflonInfo.pdkey);
                logger_->info("Rewritten URL with mouflon keys: {}...",
                              variantUrl.substr(0, std::min<size_t>(80, variantUrl.size())));
            }

            return variantUrl;
        }

        return "";
    }

} // namespace sm
