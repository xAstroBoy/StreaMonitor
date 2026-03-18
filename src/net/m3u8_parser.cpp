#include "net/m3u8_parser.h"
#include <sstream>
#include <algorithm>
#include <regex>
#include <cmath>
#include <iomanip>
#include <spdlog/spdlog.h>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // URL utilities
    // ─────────────────────────────────────────────────────────────────
    std::string M3U8Parser::resolveUrl(const std::string &base, const std::string &relative)
    {
        if (relative.empty())
            return base;

        // Already absolute
        if (relative.find("http://") == 0 || relative.find("https://") == 0)
            return relative;

        // Protocol-relative
        if (relative.find("//") == 0)
        {
            auto scheme = base.substr(0, base.find("//"));
            return scheme + relative;
        }

        // Find base directory
        std::string baseDir;
        auto queryPos = base.find('?');
        std::string basePath = (queryPos != std::string::npos) ? base.substr(0, queryPos) : base;
        auto lastSlash = basePath.rfind('/');
        if (lastSlash != std::string::npos)
            baseDir = basePath.substr(0, lastSlash + 1);
        else
            baseDir = basePath + "/";

        // Absolute path on same host
        if (relative[0] == '/')
        {
            // Extract scheme + host
            auto schemeEnd = base.find("://");
            if (schemeEnd != std::string::npos)
            {
                auto hostEnd = base.find('/', schemeEnd + 3);
                if (hostEnd != std::string::npos)
                    return base.substr(0, hostEnd) + relative;
            }
            return relative;
        }

        return baseDir + relative;
    }

    std::string M3U8Parser::inheritQueryParams(const std::string &sourceUrl,
                                               const std::string &targetUrl)
    {
        auto qpos = sourceUrl.find('?');
        if (qpos == std::string::npos)
            return targetUrl;

        std::string query = sourceUrl.substr(qpos);
        auto tqpos = targetUrl.find('?');
        if (tqpos != std::string::npos)
        {
            // Merge: append with &
            return targetUrl + "&" + query.substr(1);
        }
        return targetUrl + query;
    }

    // ─────────────────────────────────────────────────────────────────
    // Attribute parsing helpers
    // ─────────────────────────────────────────────────────────────────
    static std::string extractAttribute(const std::string &line, const std::string &attr)
    {
        std::string search = attr + "=";
        auto pos = line.find(search);
        if (pos == std::string::npos)
            return "";

        pos += search.size();
        if (pos >= line.size())
            return "";

        // Quoted value
        if (line[pos] == '"')
        {
            auto end = line.find('"', pos + 1);
            if (end == std::string::npos)
                return line.substr(pos + 1);
            return line.substr(pos + 1, end - pos - 1);
        }

        // Unquoted value
        auto end = line.find(',', pos);
        if (end == std::string::npos)
            end = line.size();
        return line.substr(pos, end - pos);
    }

    static int extractIntAttribute(const std::string &line, const std::string &attr)
    {
        std::string val = extractAttribute(line, attr);
        if (val.empty())
            return 0;
        try
        {
            return std::stoi(val);
        }
        catch (...)
        {
            return 0;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Detection
    // ─────────────────────────────────────────────────────────────────
    bool M3U8Parser::isMasterPlaylist(const std::string &content)
    {
        return content.find("#EXT-X-STREAM-INF") != std::string::npos;
    }

    // ─────────────────────────────────────────────────────────────────
    // Master playlist parser
    // ─────────────────────────────────────────────────────────────────
    HLSMasterPlaylist M3U8Parser::parseMaster(const std::string &content,
                                              const std::string &baseUrl)
    {
        HLSMasterPlaylist master;
        std::istringstream stream(content);
        std::string line;

        while (std::getline(stream, line))
        {
            // Trim
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Parse audio renditions: #EXT-X-MEDIA:TYPE=AUDIO,...
            if (line.find("#EXT-X-MEDIA:") == 0 || line.find("#EXT-X-MEDIA :") == 0)
            {
                std::string typeVal = extractAttribute(line, "TYPE");
                if (typeVal == "AUDIO")
                {
                    HLSAudioRendition audio;
                    audio.groupId = extractAttribute(line, "GROUP-ID");
                    audio.name = extractAttribute(line, "NAME");
                    std::string uri = extractAttribute(line, "URI");
                    if (!uri.empty())
                        audio.uri = resolveUrl(baseUrl, uri);
                    audio.rawLine = line;
                    master.audioRenditions.push_back(std::move(audio));
                }
            }

            if (line.find("#EXT-X-STREAM-INF") == 0)
            {
                HLSVariant variant;
                variant.bandwidth = extractIntAttribute(line, "BANDWIDTH");
                variant.codecs = extractAttribute(line, "CODECS");
                variant.audioGroupId = extractAttribute(line, "AUDIO");

                // FRAME-RATE=
                std::string fr = extractAttribute(line, "FRAME-RATE");
                if (!fr.empty())
                {
                    try
                    {
                        variant.frameRate = std::stof(fr);
                    }
                    catch (...)
                    {
                    }
                }

                // RESOLUTION=WxH
                std::string res = extractAttribute(line, "RESOLUTION");
                if (!res.empty())
                {
                    auto xpos = res.find('x');
                    if (xpos != std::string::npos)
                    {
                        try
                        {
                            variant.width = std::stoi(res.substr(0, xpos));
                            variant.height = std::stoi(res.substr(xpos + 1));
                        }
                        catch (...)
                        {
                        }
                    }
                }

                // Next line is the URL
                if (std::getline(stream, line))
                {
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    variant.url = resolveUrl(baseUrl, line);
                }

                if (!variant.url.empty())
                    master.variants.push_back(std::move(variant));
            }
        }

        // Sort by resolution (height descending)
        std::sort(master.variants.begin(), master.variants.end(),
                  [](const HLSVariant &a, const HLSVariant &b)
                  {
                      return a.height > b.height;
                  });

        return master;
    }

    // ─────────────────────────────────────────────────────────────────
    // Media playlist parser
    // ─────────────────────────────────────────────────────────────────
    HLSMediaPlaylist M3U8Parser::parseMedia(const std::string &content,
                                            const std::string &baseUrl)
    {
        HLSMediaPlaylist playlist;
        std::istringstream stream(content);
        std::string line;
        double currentDuration = 0;
        std::string currentKeyUri;
        std::string currentKeyMethod = "NONE";
        std::string currentKeyIV;
        int64_t seqCounter = 0;

        while (std::getline(stream, line))
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            if (line.empty() || line[0] == '#')
            {
                if (line.find("#EXT-X-VERSION") == 0)
                {
                    try
                    {
                        playlist.version = std::stoi(line.substr(15));
                    }
                    catch (...)
                    {
                    }
                }
                else if (line.find("#EXT-X-TARGETDURATION") == 0)
                {
                    try
                    {
                        playlist.targetDuration = std::stod(line.substr(22));
                    }
                    catch (...)
                    {
                    }
                }
                else if (line.find("#EXT-X-MEDIA-SEQUENCE") == 0)
                {
                    try
                    {
                        playlist.mediaSequence = std::stoll(line.substr(22));
                        seqCounter = playlist.mediaSequence;
                    }
                    catch (...)
                    {
                    }
                }
                else if (line.find("#EXTINF:") == 0)
                {
                    auto commaPos = line.find(',', 8);
                    std::string durStr = (commaPos != std::string::npos)
                                             ? line.substr(8, commaPos - 8)
                                             : line.substr(8);
                    try
                    {
                        currentDuration = std::stod(durStr);
                    }
                    catch (...)
                    {
                        currentDuration = 0;
                    }
                }
                else if (line.find("#EXT-X-ENDLIST") == 0)
                {
                    playlist.isEndList = true;
                    playlist.isLive = false;
                }
                else if (line.find("#EXT-X-KEY") == 0)
                {
                    currentKeyMethod = extractAttribute(line, "METHOD");
                    currentKeyUri = extractAttribute(line, "URI");
                    currentKeyIV = extractAttribute(line, "IV");
                    if (!currentKeyUri.empty())
                        currentKeyUri = resolveUrl(baseUrl, currentKeyUri);
                }
                else if (line.find("#EXT-X-MAP") == 0)
                {
                    std::string mapUri = extractAttribute(line, "URI");
                    if (!mapUri.empty())
                    {
                        mapUri = resolveUrl(baseUrl, mapUri);
                        playlist.mapUri = mapUri;

                        HLSSegment seg;
                        seg.uri = mapUri;
                        seg.isMap = true;
                        seg.duration = 0;
                        seg.keyMethod = currentKeyMethod;
                        seg.keyUri = currentKeyUri;
                        seg.keyIV = currentKeyIV;
                        playlist.segments.push_back(std::move(seg));
                    }
                }
                continue;
            }

            // Regular segment line
            HLSSegment seg;
            seg.uri = resolveUrl(baseUrl, line);
            seg.duration = currentDuration;
            seg.sequenceNumber = seqCounter++;
            seg.keyMethod = currentKeyMethod;
            seg.keyUri = currentKeyUri;
            seg.keyIV = currentKeyIV;
            playlist.segments.push_back(std::move(seg));

            currentDuration = 0;
        }

        return playlist;
    }

    // ─────────────────────────────────────────────────────────────────
    // Variant selection
    // ─────────────────────────────────────────────────────────────────
    std::optional<HLSVariant> M3U8Parser::selectVariant(
        const HLSMasterPlaylist &master,
        int wantedHeight,
        ResolutionPref pref)
    {
        if (master.variants.empty())
            return std::nullopt;

        // If wantedHeight is very high (99999), just pick highest
        if (wantedHeight >= 99999)
            return master.variants.front(); // already sorted desc

        switch (pref)
        {
        case ResolutionPref::Exact:
            for (const auto &v : master.variants)
            {
                if (v.height == wantedHeight)
                    return v;
            }
            return std::nullopt;

        case ResolutionPref::Closest:
        {
            const HLSVariant *best = nullptr;
            int bestDiff = INT_MAX;
            for (const auto &v : master.variants)
            {
                int diff = std::abs(v.height - wantedHeight);
                if (diff < bestDiff)
                {
                    bestDiff = diff;
                    best = &v;
                }
            }
            return best ? std::optional(*best) : std::nullopt;
        }

        case ResolutionPref::ExactOrLeastHigher:
            for (auto it = master.variants.rbegin(); it != master.variants.rend(); ++it)
            {
                if (it->height >= wantedHeight)
                    return *it;
            }
            return master.variants.front();

        case ResolutionPref::ExactOrHighestLower:
            for (const auto &v : master.variants)
            {
                if (v.height <= wantedHeight)
                    return v;
            }
            return master.variants.back();
        }

        return master.variants.front();
    }

    // ─────────────────────────────────────────────────────────────────
    // Build filtered master playlist for split audio/video streams
    // Creates a proper master playlist with only the selected variant
    // and its matching audio group, using absolute URLs throughout.
    // FFmpeg will read both audio and video chunklists and sync them
    // via EXT-X-PROGRAM-DATE-TIME tags.
    // ─────────────────────────────────────────────────────────────────
    std::string M3U8Parser::buildFilteredMaster(const HLSMasterPlaylist &master,
                                                const HLSVariant &selectedVariant,
                                                const std::string &baseUrl)
    {
        std::ostringstream out;
        out << "#EXTM3U\n";
        out << "#EXT-X-VERSION:6\n";
        out << "#EXT-X-INDEPENDENT-SEGMENTS\n";

        // Emit only the audio renditions that match the selected variant's audio group
        if (!selectedVariant.audioGroupId.empty())
        {
            for (const auto &audio : master.audioRenditions)
            {
                if (audio.groupId == selectedVariant.audioGroupId && !audio.uri.empty())
                {
                    // Reconstruct the EXT-X-MEDIA line with absolute URI
                    out << "#EXT-X-MEDIA:TYPE=AUDIO"
                        << ",GROUP-ID=\"" << audio.groupId << "\""
                        << ",NAME=\"" << audio.name << "\""
                        << ",DEFAULT=YES,AUTOSELECT=YES,FORCED=NO,CHANNELS=\"2\""
                        << ",URI=\"" << audio.uri << "\"\n";
                }
            }
        }

        // Emit the selected variant
        out << "#EXT-X-STREAM-INF:BANDWIDTH=" << selectedVariant.bandwidth
            << ",RESOLUTION=" << selectedVariant.width << "x" << selectedVariant.height;
        if (selectedVariant.frameRate > 0)
            out << ",FRAME-RATE=" << std::fixed << std::setprecision(3) << selectedVariant.frameRate;
        if (!selectedVariant.codecs.empty())
            out << ",CODECS=\"" << selectedVariant.codecs << "\"";
        if (!selectedVariant.audioGroupId.empty())
            out << ",AUDIO=\"" << selectedVariant.audioGroupId << "\"";
        out << "\n";
        out << selectedVariant.url << "\n";

        return out.str();
    }

} // namespace sm
