// ─────────────────────────────────────────────────────────────────
// StripChatVR site plugin — VR extension of StripChat
// Port from Python: VR capability detection with flexible OR logic,
// comprehensive path searching, recursive key finding,
// VR format suffix building
// ─────────────────────────────────────────────────────────────────

#include "sites/stripchatvr.h"

namespace sm
{

    REGISTER_SITE(StripChatVR);

    const std::map<std::string, std::string> StripChatVR::kVrFrameFormatMap = {
        {"FISHEYE", "F"},
        {"PANORAMIC", "P"},
        {"CIRCULAR", "C"},
        {"EQUIRECTANGULAR", "E"}};

    StripChatVR::StripChatVR(const std::string &username)
        : StripChat(kSiteName, kSiteSlug, username)
    {
    }

    std::string StripChatVR::getWebsiteUrl() const
    {
        return "https://vr.stripchat.com/cam/" + username();
    }

    // ── JSON path helpers ───────────────────────────────────────────

    nlohmann::json StripChatVR::findInPaths(
        const nlohmann::json &root,
        const std::vector<std::vector<std::string>> &paths) const
    {
        for (const auto &path : paths)
        {
            const nlohmann::json *current = &root;
            bool found = true;
            for (const auto &key : path)
            {
                if (current->is_object() && current->contains(key))
                    current = &(*current)[key];
                else
                {
                    found = false;
                    break;
                }
            }
            if (found && !current->is_null())
                return *current;
        }
        return nullptr;
    }

    nlohmann::json StripChatVR::recursiveFind(
        const nlohmann::json &root, const std::string &key) const
    {
        if (root.is_object())
        {
            if (root.contains(key))
                return root[key];
            for (auto it = root.begin(); it != root.end(); ++it)
            {
                auto result = recursiveFind(it.value(), key);
                if (!result.is_null())
                    return result;
            }
        }
        else if (root.is_array())
        {
            for (const auto &item : root)
            {
                auto result = recursiveFind(item, key);
                if (!result.is_null())
                    return result;
            }
        }
        return nullptr;
    }

    nlohmann::json StripChatVR::findVrCamSettings() const
    {
        if (lastInfo_.is_null())
            return nullptr;

        std::vector<std::vector<std::string>> paths = {
            {"vrCameraSettings"},
            {"broadcastSettings", "vrCameraSettings"},
            {"cam", "broadcastSettings", "vrCameraSettings"},
            {"cam", "vrCameraSettings"},
            {"model", "broadcastSettings", "vrCameraSettings"},
            {"model", "vrCameraSettings"},
            {"user", "broadcastSettings", "vrCameraSettings"},
            {"user", "user", "broadcastSettings", "vrCameraSettings"},
            {"user", "user", "vrCameraSettings"},
            {"settings", "vrCameraSettings"}};

        auto val = findInPaths(lastInfo_, paths);
        if (val.is_object() && !val.empty())
            return val;

        // Recursive fallback
        auto found = recursiveFind(lastInfo_, "vrCameraSettings");
        if (found.is_object() && !found.empty())
            return found;

        return nullptr;
    }

    // ── VR capability detection ─────────────────────────────────────

    bool StripChatVR::getIsVrModel() const
    {
        if (lastInfo_.is_null())
            return false;

        std::vector<std::vector<std::string>> paths = {
            {"model", "isVr"},
            {"user", "user", "isVr"},
            {"user", "isVr"},
            {"cam", "isVr"},
            {"isVr"}};

        auto val = findInPaths(lastInfo_, paths);
        if (!val.is_null())
            return val.get<bool>();

        auto found = recursiveFind(lastInfo_, "isVr");
        return !found.is_null() && found.get<bool>();
    }

    bool StripChatVR::getHasVrSettings() const
    {
        auto vrCs = findVrCamSettings();
        if (vrCs.is_object() && !vrCs.empty())
        {
            static const std::vector<std::string> vrKeys = {
                "frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle",
                "frame_format", "stereo_packing", "horizontal_angle", "vertical_angle",
                "packing", "angle"};

            for (const auto &k : vrKeys)
            {
                if (vrCs.contains(k))
                    return true;
            }
            return !vrCs.empty();
        }

        // Looser check: look for VR keys anywhere
        for (const auto &key : {"frameFormat", "stereoPacking", "horizontalAngle"})
        {
            auto found = recursiveFind(lastInfo_, key);
            if (!found.is_null())
                return true;
        }

        return false;
    }

    bool StripChatVR::isVrCapable() const
    {
        if (lastInfo_.is_null())
            return false;

        // Check 1: Explicit isVr flags
        std::vector<std::vector<std::string>> isVrPaths = {
            {"isVr"},
            {"model", "isVr"},
            {"user", "user", "isVr"},
            {"user", "isVr"},
            {"cam", "isVr"}};

        auto val = findInPaths(lastInfo_, isVrPaths);
        if (!val.is_null())
        {
            if (val.is_boolean())
            {
                if (!val.get<bool>())
                    return false;
                return true;
            }
        }

        // Check 2: VR camera settings exist and have VR data
        auto vrCamSettings = findVrCamSettings();
        if (vrCamSettings.is_object() && !vrCamSettings.empty())
        {
            if (vrCamSettings.is_array() && vrCamSettings.empty())
                return false;

            static const std::vector<std::string> vrKeys = {
                "frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle",
                "frame_format", "stereo_packing", "horizontal_angle", "vertical_angle",
                "packing", "angle"};

            for (const auto &k : vrKeys)
            {
                if (vrCamSettings.contains(k))
                    return true;
            }
        }

        // Check 3: Broadcast settings indicate VR
        std::vector<std::vector<std::string>> bsPaths = {
            {"broadcastSettings"},
            {"cam", "broadcastSettings"},
            {"model", "broadcastSettings"},
            {"user", "broadcastSettings"},
            {"user", "user", "broadcastSettings"}};

        auto bs = findInPaths(lastInfo_, bsPaths);
        if (bs.is_object() && !bs.empty())
        {
            if (bs.contains("vrCameraSettings"))
            {
                auto vrSettings = bs["vrCameraSettings"];
                if (vrSettings.is_object() && !vrSettings.empty())
                {
                    for (const auto &k : {"frameFormat", "stereoPacking", "horizontalAngle", "verticalAngle"})
                    {
                        if (vrSettings.contains(k))
                            return true;
                    }
                }
            }
            if (bs.contains("isVr") && bs["isVr"].get<bool>())
                return true;
        }

        return false;
    }

    // ── VR format suffix ────────────────────────────────────────────

    std::string StripChatVR::getVrSuffix() const
    {
        auto vrCamSettings = findVrCamSettings();
        if (!vrCamSettings.is_object() || vrCamSettings.empty())
            return "";

        // Get packing
        std::string packing = "M";
        for (const auto &k : {"stereoPacking", "stereo_packing", "packing"})
        {
            if (vrCamSettings.contains(k) && vrCamSettings[k].is_string())
            {
                packing = vrCamSettings[k].get<std::string>();
                break;
            }
        }

        // Get frame format
        std::string frameFormatRaw;
        for (const auto &k : {"frameFormat", "frame_format"})
        {
            if (vrCamSettings.contains(k) && vrCamSettings[k].is_string())
            {
                frameFormatRaw = vrCamSettings[k].get<std::string>();
                break;
            }
        }

        std::string frameFormat = "X";
        auto it = kVrFrameFormatMap.find(frameFormatRaw);
        if (it != kVrFrameFormatMap.end())
            frameFormat = it->second;
        else if (!frameFormatRaw.empty())
            frameFormat = std::string(1, std::toupper(frameFormatRaw[0]));

        // Get angle
        std::string angle = "0";
        for (const auto &k : {"horizontalAngle", "horizontal_angle", "angle"})
        {
            if (vrCamSettings.contains(k))
            {
                if (vrCamSettings[k].is_number())
                    angle = std::to_string(vrCamSettings[k].get<int>());
                else if (vrCamSettings[k].is_string())
                    angle = vrCamSettings[k].get<std::string>();
                break;
            }
        }

        return "_" + packing + "_" + frameFormat + angle;
    }

    // ── Status override (require VR) ────────────────────────────────

    Status StripChatVR::checkStatus()
    {
        // Get base StripChat status
        Status status = StripChat::checkStatus();

        if (status == Status::Public)
        {
            bool vrCapable = isVrCapable();

            if (vrCapable)
            {
                isVr_ = true; // Only SCVR sets this — base SC always stays false
                if (!lastVrCapableState_)
                    logger_->info("{} VR capability restored", username());
                lastVrCapableState_ = true;
                return Status::Public;
            }

            // Not VR-capable
            if (lastVrCapableState_)
                logger_->info("{} is public but not VR-capable — going offline", username());
            lastVrCapableState_ = false;
            return Status::Offline;
        }

        return status;
    }

} // namespace sm
