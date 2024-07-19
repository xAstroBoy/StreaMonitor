#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — M3U8 HLS playlist parser
// ─────────────────────────────────────────────────────────────────

#include "core/types.h"
#include <string>
#include <vector>
#include <optional>

namespace sm
{

    // ── Segment in a media playlist ─────────────────────────────────
    struct HLSSegment
    {
        std::string uri;
        double duration = 0;
        int64_t sequenceNumber = 0;
        bool isMap = false;    // #EXT-X-MAP init segment
        std::string keyUri;    // #EXT-X-KEY URI
        std::string keyMethod; // AES-128, NONE, etc.
        std::string keyIV;     // initialization vector
    };

    // ── Media playlist ──────────────────────────────────────────────
    struct HLSMediaPlaylist
    {
        std::vector<HLSSegment> segments;
        double targetDuration = 0;
        int64_t mediaSequence = 0;
        bool isEndList = false; // VOD or ended stream
        bool isLive = true;
        int version = 0;
        std::string mapUri; // #EXT-X-MAP URI
    };

    // ── Master playlist variant ─────────────────────────────────────
    // (HLSVariant already in types.h, re-used here)

    struct HLSMasterPlaylist
    {
        std::vector<HLSVariant> variants;
        bool isValid() const { return !variants.empty(); }
    };

    // ── Parser ──────────────────────────────────────────────────────
    class M3U8Parser
    {
    public:
        // Parse a master playlist from text
        static HLSMasterPlaylist parseMaster(const std::string &content,
                                             const std::string &baseUrl);

        // Parse a media playlist from text
        static HLSMediaPlaylist parseMedia(const std::string &content,
                                           const std::string &baseUrl);

        // Detect if content is master or media playlist
        static bool isMasterPlaylist(const std::string &content);

        // Select best variant for desired resolution
        static std::optional<HLSVariant> selectVariant(
            const HLSMasterPlaylist &master,
            int wantedHeight,
            ResolutionPref pref);

        // Resolve a potentially relative URL against a base
        static std::string resolveUrl(const std::string &base, const std::string &relative);

        // Extract query parameters from URL and apply to another
        static std::string inheritQueryParams(const std::string &sourceUrl,
                                              const std::string &targetUrl);
    };

} // namespace sm
