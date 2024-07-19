// ─────────────────────────────────────────────────────────────────
// ManyVids site plugin — Roompool + CloudFront cookies
// Port from Python: MVModelInfo, player-settings, base64 policy
// ─────────────────────────────────────────────────────────────────

#include "sites/manyvids.h"
#include <sstream>
#include <regex>

namespace sm
{

    REGISTER_SITE(ManyVids);

    ManyVids::ManyVids(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
        sleepOnOffline_ = 10;
        sleepOnRateLimit_ = 60;
        maxConsecutiveErrors_ = 80;
        updateSiteCookies();
    }

    std::string ManyVids::getWebsiteUrl() const
    {
        return "https://www.manyvids.com/live/" + username();
    }

    void ManyVids::updateSiteCookies()
    {
        // Fetch redirect page to get session cookies
        HttpRequest req;
        req.url = "https://www.manyvids.com/tak-live-redirect.php";
        req.timeoutSec = 30;
        req.followRedirects = false;

        auto resp = http().execute(req);
        // Cookies are automatically stored in libcurl's cookie jar
    }

    std::string ManyVids::requestStreamInfo()
    {
        if (lastInfo_.empty() ||
            !lastInfo_.contains("publicAPIURL") ||
            !lastInfo_.contains("floorId"))
            return "";

        std::string apiUrl = lastInfo_.value("publicAPIURL", "");
        std::string floorId = std::to_string(lastInfo_.value("floorId", 0));

        std::string url = apiUrl + "/" + floorId + "/player-settings/" + username();

        auto resp = http().get(url, 30);
        if (!resp.ok())
            return "";

        return resp.body;
    }

    std::string ManyVids::extractCloudFrontUrl(const std::string &policyB64) const
    {
        // The CloudFront-Policy cookie is base64 encoded with _ instead of =
        // Decode and extract the resource URL from the Statement
        try
        {
            // Replace _ back to = for proper base64
            std::string policy = policyB64;
            std::replace(policy.begin(), policy.end(), '_', '=');

            // Simple base64 decode — we'll use a manual approach
            // since we only need to find the Resource field
            static const std::string base64_chars =
                "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

            auto b64decode = [&](const std::string &in) -> std::string
            {
                std::string out;
                std::vector<int> T(256, -1);
                for (int i = 0; i < 64; i++)
                    T[base64_chars[i]] = i;

                int val = 0, valb = -8;
                for (unsigned char c : in)
                {
                    if (T[c] == -1)
                        break;
                    val = (val << 6) + T[c];
                    valb += 6;
                    if (valb >= 0)
                    {
                        out.push_back(char((val >> valb) & 0xFF));
                        valb -= 8;
                    }
                }
                return out;
            };

            std::string decoded = b64decode(policy);

            // Parse JSON to extract Resource
            auto policyJson = nlohmann::json::parse(decoded);
            if (policyJson.contains("Statement") && policyJson["Statement"].is_array() &&
                !policyJson["Statement"].empty())
            {
                std::string resource = policyJson["Statement"][0].value("Resource", "");
                if (!resource.empty())
                {
                    // Replace wildcard with username.m3u8
                    if (resource.back() == '*')
                        resource = resource.substr(0, resource.size() - 1) + username() + ".m3u8";
                    return resource;
                }
            }
        }
        catch (const std::exception &e)
        {
            logger_->error("CloudFront policy decode error: {}", e.what());
        }

        return "";
    }

    Status ManyVids::checkStatus()
    {
        std::string url = "https://roompool.live.manyvids.com/roompool/" +
                          username() + "?private=false";

        auto resp = http().get(url, 30);

        if (resp.isNotFound())
        {
            setLastError("Not found (404)", resp.statusCode);
            return Status::NotExist;
        }
        if (!resp.ok())
        {
            logger_->warn("HTTP {} for {}", resp.statusCode, username());
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Unknown;
        }

        try
        {
            lastInfo_ = nlohmann::json::parse(resp.body);
            setLastApiResponse(resp.body);

            std::string reason = lastInfo_.value("roomLocationReason", "");

            if (reason == "ROOM_VALIDATION_FAILED")
                return Status::NotExist;

            if (reason == "ROOM_OK")
            {
                // Verify stream is actually available
                std::string streamBody = requestStreamInfo();
                if (streamBody.empty())
                    return Status::Error;

                try
                {
                    auto streamJson = nlohmann::json::parse(streamBody);
                    if (!streamJson.contains("withCredentials"))
                        return Status::Offline;
                    return Status::Public;
                }
                catch (...)
                {
                    return Status::Offline;
                }
            }

            return Status::Unknown;
        }
        catch (const std::exception &e)
        {
            logger_->error("Parse error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string ManyVids::getVideoUrl()
    {
        // Python logic: Get player-settings, then decode CloudFront-Policy cookie
        // to extract the HLS URL
        std::string streamBody = requestStreamInfo();
        if (streamBody.empty())
            return "";

        try
        {
            auto streamJson = nlohmann::json::parse(streamBody);

            // Try to get CloudFront-Policy from the response cookies
            // The URL is decoded from the base64 policy cookie
            if (streamJson.contains("withCredentials") && streamJson["withCredentials"].is_boolean())
            {
                // The player-settings response should trigger CloudFront cookies
                // Get the cookies from the cookie jar for manyvids.com
                std::string cookieStr = http().getCookiesForUrl("https://www.manyvids.com/");

                std::string policyB64;
                // Cookie string is semicolon-separated: "name1=val1; name2=val2"
                auto cfPos = cookieStr.find("CloudFront-Policy=");
                if (cfPos != std::string::npos)
                {
                    auto eqPos = cfPos + std::string("CloudFront-Policy=").size();
                    auto endPos = cookieStr.find(';', eqPos);
                    policyB64 = cookieStr.substr(eqPos,
                                                 endPos == std::string::npos ? std::string::npos : endPos - eqPos);
                }

                if (!policyB64.empty())
                {
                    std::string url = extractCloudFrontUrl(policyB64);
                    if (!url.empty())
                        return selectResolution(url);
                }
            }

            // Fallback: try direct URL field
            if (streamJson.contains("url") && streamJson["url"].is_string())
            {
                std::string hlsUrl = streamJson["url"].get<std::string>();
                if (!hlsUrl.empty())
                    return selectResolution(hlsUrl);
            }
        }
        catch (const std::exception &e)
        {
            logger_->error("ManyVids getVideoUrl error: {}", e.what());
        }

        return "";
    }

} // namespace sm
