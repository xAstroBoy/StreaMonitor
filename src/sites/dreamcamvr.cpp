// ─────────────────────────────────────────────────────────────────
// DreamCamVR site plugin — VR extension of DreamCam
// Port from Python: uses "video3D" streamType instead of "video2D"
// VR format suffix from stream URL query parameters
// ─────────────────────────────────────────────────────────────────

#include "sites/dreamcamvr.h"

namespace sm
{

    REGISTER_SITE(DreamCamVR);

    const std::map<std::string, std::string> DreamCamVR::kVrFrameFormatMap = {
        {"FISHEYE", "F"},
        {"PANORAMIC", "P"},
        {"CIRCULAR", "C"}};

    DreamCamVR::DreamCamVR(const std::string &username)
        : DreamCam(kSiteName, kSiteSlug, username)
    {
        streamType_ = "video3D"; // Override to look for 3D streams
    }

} // namespace sm
