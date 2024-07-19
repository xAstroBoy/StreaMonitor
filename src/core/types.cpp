#include "core/types.h"
#include <algorithm>
#include <cctype>

namespace sm
{

    const char *statusToString(Status s)
    {
        switch (s)
        {
        case Status::Unknown:
            return "Unknown";
        case Status::NotRunning:
            return "Not Running";
        case Status::Error:
            return "Error";
        case Status::ConnectionError:
            return "Connection Error";
        case Status::Restricted:
            return "Restricted";
        case Status::Online:
            return "Online";
        case Status::Public:
            return "Public";
        case Status::NotExist:
            return "Not Found";
        case Status::Private:
            return "Private";
        case Status::Offline:
            return "Offline";
        case Status::LongOffline:
            return "Long Offline";
        case Status::Deleted:
            return "Deleted";
        case Status::RateLimit:
            return "Rate Limited";
        case Status::Cloudflare:
            return "Cloudflare";
        }
        return "Unknown";
    }

    bool statusIsRecordable(Status s)
    {
        return s == Status::Public;
    }

    bool statusIsTemporaryError(Status s)
    {
        return s == Status::RateLimit || s == Status::Cloudflare ||
               s == Status::ConnectionError || s == Status::Unknown;
    }

    const char *genderToString(Gender g)
    {
        switch (g)
        {
        case Gender::Unknown:
            return "Unknown";
        case Gender::Female:
            return "Female";
        case Gender::Male:
            return "Male";
        case Gender::Couple:
            return "Couple";
        case Gender::TransWoman:
            return "Trans Woman";
        case Gender::TransMan:
            return "Trans Man";
        case Gender::Trans:
            return "Trans";
        case Gender::FemalCouple:
            return "Female Couple";
        case Gender::MaleCouple:
            return "Male Couple";
        }
        return "Unknown";
    }

    static const FormatInfo FORMAT_MKV = {".mkv", "matroska", "matroska", nullptr};
    static const FormatInfo FORMAT_MP4 = {".mp4", "mp4", "mp4", nullptr};
    static const FormatInfo FORMAT_TS = {".ts", "mpegts", "mpegts", nullptr};

    const FormatInfo &getFormatInfo(ContainerFormat fmt)
    {
        switch (fmt)
        {
        case ContainerFormat::MP4:
            return FORMAT_MP4;
        case ContainerFormat::TS:
            return FORMAT_TS;
        default:
            return FORMAT_MKV;
        }
    }

    ContainerFormat parseContainerFormat(const std::string &s)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        if (lower == "mp4")
            return ContainerFormat::MP4;
        if (lower == "ts")
            return ContainerFormat::TS;
        return ContainerFormat::MKV;
    }

    // ── VR projection/stereo helpers ────────────────────────────────

    const char *vrProjectionToString(VRProjection p)
    {
        switch (p)
        {
        case VRProjection::None:
            return "none";
        case VRProjection::Fisheye180:
            return "fisheye180";
        case VRProjection::Equirect360:
            return "equirect360";
        case VRProjection::Equirect180:
            return "equirect180";
        case VRProjection::Cubemap:
            return "cubemap";
        case VRProjection::EAC:
            return "eac";
        }
        return "none";
    }

    const char *vrStereoModeToString(VRStereoMode m)
    {
        switch (m)
        {
        case VRStereoMode::Mono:
            return "mono";
        case VRStereoMode::SideBySide:
            return "side_by_side";
        case VRStereoMode::TopBottom:
            return "top_bottom";
        case VRStereoMode::Custom:
            return "custom";
        }
        return "mono";
    }

    VRProjection parseVRProjection(const std::string &s)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        if (lower == "fisheye180" || lower == "fisheye" || lower == "fish_eye_180")
            return VRProjection::Fisheye180;
        if (lower == "equirect360" || lower == "equirectangular" || lower == "equirectangular360")
            return VRProjection::Equirect360;
        if (lower == "equirect180" || lower == "equirectangular180")
            return VRProjection::Equirect180;
        if (lower == "cubemap" || lower == "cube")
            return VRProjection::Cubemap;
        if (lower == "eac")
            return VRProjection::EAC;
        return VRProjection::None;
    }

    VRStereoMode parseVRStereoMode(const std::string &s)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        if (lower == "sbs" || lower == "side_by_side" || lower == "sidebyside" || lower == "lr")
            return VRStereoMode::SideBySide;
        if (lower == "tb" || lower == "top_bottom" || lower == "topbottom" || lower == "ou")
            return VRStereoMode::TopBottom;
        if (lower == "mono" || lower == "2d")
            return VRStereoMode::Mono;
        if (lower == "custom")
            return VRStereoMode::Custom;
        return VRStereoMode::Mono;
    }

    // ── Encoder type helpers ────────────────────────────────────────

    const char *encoderTypeToString(EncoderType e)
    {
        switch (e)
        {
        case EncoderType::Copy:
            return "copy";
        case EncoderType::X265:
            return "x265";
        case EncoderType::X264:
            return "x264";
        case EncoderType::NVENC_HEVC:
            return "nvenc_hevc";
        case EncoderType::NVENC_H264:
            return "nvenc_h264";
        }
        return "x265";
    }

    EncoderType parseEncoderType(const std::string &s)
    {
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c)
                       { return std::tolower(c); });
        if (lower == "copy" || lower == "none" || lower == "stream_copy")
            return EncoderType::Copy;
        if (lower == "x265" || lower == "libx265" || lower == "hevc")
            return EncoderType::X265;
        if (lower == "x264" || lower == "libx264" || lower == "h264" || lower == "avc")
            return EncoderType::X264;
        if (lower == "nvenc_hevc" || lower == "hevc_nvenc" || lower == "cuda_hevc")
            return EncoderType::NVENC_HEVC;
        if (lower == "nvenc_h264" || lower == "h264_nvenc" || lower == "cuda_h264" || lower == "nvenc")
            return EncoderType::NVENC_H264;
        return EncoderType::X265; // default
    }

} // namespace sm
