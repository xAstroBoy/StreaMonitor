#include "sites/camsoda.h"

namespace sm
{

    REGISTER_SITE(CamSoda);

    CamSoda::CamSoda(const std::string &username)
        : SitePlugin(kSiteName, kSiteSlug, username)
    {
    }

    std::string CamSoda::getWebsiteUrl() const
    {
        return "https://www.camsoda.com/" + username();
    }

    Status CamSoda::checkStatus()
    {
        std::string url = std::string(kApiBase) + "/" + username();

        auto resp = http().get(url, 30);

        // Connection error check first — NOT a rate limit!
        if (resp.isConnectionError())
        {
            logger_->warn("Connection error: {}", resp.error);
            setLastError("Connection error: " + resp.error, 0);
            return Status::ConnectionError;
        }
        if (resp.isRateLimit())
        {
            logger_->warn("Rate limited (429)");
            setLastError("Rate limited (429)", resp.statusCode);
            return Status::RateLimit;
        }
        if (resp.statusCode == 403)
        {
            logger_->warn("Forbidden (403)");
            setLastError("Forbidden (403)", resp.statusCode);
            return Status::RateLimit;
        }
        if (!resp.ok())
        {
            logger_->warn("HTTP {}", resp.statusCode);
            setLastError("HTTP " + std::to_string(resp.statusCode), resp.statusCode);
            return Status::Error;
        }

        try
        {
            auto json = nlohmann::json::parse(resp.body);
            lastInfo_ = json;
            setLastApiResponse(resp.body);

            // Check for non-existent user
            std::string error = json.value("error", "");
            if (error == "No username found.")
                return Status::NotExist;

            // Parse chat status and mode
            auto chat = json.value("chat", nlohmann::json::object());
            auto stream = json.value("stream", nlohmann::json::object());
            std::string chatStatus = chat.value("status", "");
            std::string mode = json.value("mode", "");

            // stream.status can be number or string depending on API response
            int streamStatus = -1;
            if (stream.contains("status"))
            {
                auto &sv = stream["status"];
                if (sv.is_number())
                    streamStatus = sv.get<int>();
                else if (sv.is_string())
                {
                    try
                    {
                        streamStatus = std::stoi(sv.get<std::string>());
                    }
                    catch (...)
                    {
                        streamStatus = -1;
                    }
                }
            }

            if (chatStatus == "online" && mode == "public")
                return Status::Public;
            if (chatStatus == "online" && mode == "private")
                return Status::Private;
            if (chatStatus == "offline")
                return Status::Offline;
            if (streamStatus == 1)
                return Status::Public;

            return Status::Unknown;
        }
        catch (const std::exception &e)
        {
            logger_->error("Parse error: {}", e.what());
            setLastError(std::string("JSON parse error: ") + e.what(), resp.statusCode);
            return Status::Error;
        }
    }

    std::string CamSoda::getVideoUrl()
    {
        if (lastInfo_.empty())
            return "";

        auto stream = lastInfo_.value("stream", nlohmann::json::object());
        auto servers = stream.value("edge_servers", std::vector<std::string>{});
        std::string streamName = stream.value("stream_name", "");

        if (servers.empty() || streamName.empty())
            return "";

        std::string base = servers[0];
        if (base.find("http") != 0)
            base = "https://" + base;

        std::string masterUrl = base + "/" + streamName + "_v1/index.ll.m3u8";
        return selectResolution(masterUrl);
    }

} // namespace sm
