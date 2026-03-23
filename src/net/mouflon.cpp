// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — StripChat Mouflon key extraction & playlist
//                     decryption system
// ─────────────────────────────────────────────────────────────────

#include "net/mouflon.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <regex>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <thread>

namespace fs = std::filesystem;

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // Constants
    // ─────────────────────────────────────────────────────────────────

    static constexpr const char *MOUFLON_NEEDLE = "#EXT-X-MOUFLON:";
    static constexpr const char *MOUFLON_FILE_ATTR = "#EXT-X-MOUFLON:FILE:";
    static constexpr const char *MOUFLON_URI_ATTR = "#EXT-X-MOUFLON:URI:";
    static constexpr const char *MOUFLON_FILENAME = "media.mp4";

    // ─────────────────────────────────────────────────────────────────
    // Singleton
    // ─────────────────────────────────────────────────────────────────

    MouflonKeys &MouflonKeys::instance()
    {
        static MouflonKeys inst;
        return inst;
    }

    MouflonKeys::MouflonKeys()
    {
        // Default fallback keys (same as Python)
        keys_["Zeechoej4aleeshi"] = "ubahjae7goPoodi6";
        keys_["Zokee2OhPh9kugh4"] = "Quean4cai9boJa5a";
        keys_["Ook7quaiNgiyuhai"] = "EQueeGh2kaewa3ch";

        // Load cached keys
        loadFromCache();
    }

    // ─────────────────────────────────────────────────────────────────
    // Public API
    // ─────────────────────────────────────────────────────────────────

    void MouflonKeys::initialize(HttpClient &http)
    {
        std::lock_guard lock(mutex_);
        if (initialized_)
            return;

        spdlog::info("[Mouflon] Initializing key extraction...");

        if (fetchDoppioJs(http))
        {
            parseKeys();
            saveToCache();
            spdlog::info("[Mouflon] Keys extracted successfully ({} keys)", keys_.size());
        }
        else
        {
            spdlog::warn("[Mouflon] Failed to fetch Doppio JS — using cached/default keys ({} keys)", keys_.size());
        }

        initialized_ = true;
    }

    void MouflonKeys::reinitialize(HttpClient &http)
    {
        std::lock_guard lock(mutex_);
        initialized_ = false;

        spdlog::info("[Mouflon] Re-initializing key extraction...");

        if (fetchDoppioJs(http))
        {
            parseKeys();
            saveToCache();
            spdlog::info("[Mouflon] Keys re-extracted successfully ({} keys)", keys_.size());
        }
        else
        {
            spdlog::warn("[Mouflon] Re-fetch failed — keeping existing keys ({} keys)", keys_.size());
        }

        initialized_ = true;
    }

    bool MouflonKeys::hasKeys() const
    {
        std::lock_guard lock(mutex_);
        return !keys_.empty();
    }

    std::map<std::string, std::string> MouflonKeys::getKeys() const
    {
        std::lock_guard lock(mutex_);
        return keys_;
    }

    void MouflonKeys::addKey(const std::string &pkey, const std::string &pdkey)
    {
        std::lock_guard lock(mutex_);
        keys_[pkey] = pdkey;
        saveToCache();
        spdlog::info("[Mouflon] Added key: {} → {} ({} keys total)", pkey, pdkey, keys_.size());
    }

    void MouflonKeys::removeKey(const std::string &pkey)
    {
        std::lock_guard lock(mutex_);
        auto it = keys_.find(pkey);
        if (it != keys_.end())
        {
            keys_.erase(it);
            saveToCache();
            spdlog::info("[Mouflon] Removed key: {} ({} keys remaining)", pkey, keys_.size());
        }
    }

    std::optional<std::string> MouflonKeys::getDecKey(const std::string &pkey) const
    {
        std::lock_guard lock(mutex_);

        // Direct lookup
        auto it = keys_.find(pkey);
        if (it != keys_.end())
            return it->second;

        // Try searching in Doppio JS data for "pkey:pdkey" pattern
        if (!doppioJsData_.empty())
        {
            std::string pattern = "\"" + pkey + ":";
            auto idx = doppioJsData_.find(pattern);
            if (idx != std::string::npos)
            {
                auto start = idx + pattern.size();
                auto end = doppioJsData_.find('"', start);
                if (end != std::string::npos)
                {
                    auto key = doppioJsData_.substr(start, end - start);
                    // Cache it (const_cast is safe here since we hold the lock)
                    const_cast<MouflonKeys *>(this)->keys_[pkey] = key;
                    return key;
                }
            }
        }

        return std::nullopt;
    }

    MouflonKeys::MouflonInfo MouflonKeys::extractFromPlaylist(const std::string &m3u8Content) const
    {
        MouflonInfo info;
        size_t idx = 0;
        const size_t needleLen = strlen(MOUFLON_NEEDLE);

        while ((idx = m3u8Content.find(MOUFLON_NEEDLE, idx)) != std::string::npos)
        {
            auto lineEnd = m3u8Content.find('\n', idx);
            if (lineEnd == std::string::npos)
                lineEnd = m3u8Content.size();

            auto line = m3u8Content.substr(idx, lineEnd - idx);
            // Trim \r
            if (!line.empty() && line.back() == '\r')
                line.pop_back();

            // Format: #EXT-X-MOUFLON:psch:pkey:extra...
            // Split by ':' — first is "#EXT-X-MOUFLON", then psch, pkey, etc.
            // But the needle already includes "#EXT-X-MOUFLON:", so after that:
            // v2:Zeechoej4aleeshi  => parts: ["#EXT-X-MOUFLON", "v2", "Zeechoej4aleeshi"]
            std::vector<std::string> parts;
            std::istringstream ss(line);
            std::string part;
            while (std::getline(ss, part, ':'))
                parts.push_back(part);

            // We expect: parts[0]="#EXT-X-MOUFLON", parts[1]=scheme tag,
            // parts[2]=psch, parts[3]=pkey
            if (parts.size() >= 4)
            {
                auto psch = parts[2];
                auto pkey = parts[3];
                auto pdkey = getDecKey(pkey);

                if (pdkey)
                {
                    info.psch = psch;
                    info.pkey = pkey;
                    info.pdkey = *pdkey;
                    return info;
                }
            }

            idx += needleLen;
        }

        return info;
    }

    std::string MouflonKeys::decodePlaylists(const std::string &content) const
    {
        return decodePlaylists(content, MouflonInfo{});
    }

    std::string MouflonKeys::decodePlaylists(const std::string &content,
                                             const MouflonInfo &fallbackInfo) const
    {
        auto info = extractFromPlaylist(content);

        // If no mouflon info in the content itself, use fallback keys
        // (media playlists may lack #EXT-X-MOUFLON: lines — only in master)
        if (info.pdkey.empty() && !fallbackInfo.pdkey.empty())
        {
            spdlog::debug("[Mouflon] No keys in content, using fallback (psch={}, pkey={})",
                          fallbackInfo.psch, fallbackInfo.pkey);
            info = fallbackInfo;
        }

        if (info.pdkey.empty())
            return content; // No mouflon encryption

        auto hashBytes = sha256(info.pdkey);

        std::vector<std::string> lines;
        {
            std::istringstream iss(content);
            std::string line;
            while (std::getline(iss, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                lines.push_back(line);
            }
        }

        std::vector<std::string> decoded;
        decoded.reserve(lines.size());

        // v1 decoding (FILE attribute)
        if (info.psch == "v1")
        {
            std::string lastDecoded;
            for (const auto &line : lines)
            {
                if (line.find(MOUFLON_FILE_ATTR) == 0)
                {
                    auto encrypted = line.substr(strlen(MOUFLON_FILE_ATTR));
                    auto data = base64Decode(encrypted + "==");
                    lastDecoded = xorDecrypt(data, hashBytes);
                }
                else if (!lastDecoded.empty() && line.find(MOUFLON_FILENAME) != std::string::npos)
                {
                    // Replace media.mp4 with decoded filename
                    auto pos = line.find(MOUFLON_FILENAME);
                    if (pos != std::string::npos)
                    {
                        std::string newLine = line.substr(0, pos) + lastDecoded +
                                              line.substr(pos + strlen(MOUFLON_FILENAME));
                        decoded.push_back(newLine);
                    }
                    else
                    {
                        decoded.push_back(line);
                    }
                    lastDecoded.clear();
                }
                else
                {
                    decoded.push_back(line);
                }
            }
        }
        // v2 decoding (URI attribute)
        else if (info.psch == "v2")
        {
            size_t i = 0;
            while (i < lines.size())
            {
                const auto &line = lines[i];

                // Skip standalone "media.mp4" lines (mouflon placeholder that
                // was NOT consumed by a preceding #EXT-X-MOUFLON:URI: handler).
                if (line == MOUFLON_FILENAME || line == "media.mp4")
                {
                    i++;
                    continue;
                }

                // Handle #EXT-X-MAP:URI= lines (init segment)
                // In v2, the init segment URI already contains the correct CDN filename
                // (the "encrypted" part is a CDN token, NOT meant to be XOR-decrypted).
                // Python's m3u_decoder tries to decrypt, gets UnicodeDecodeError, and
                // falls back to keeping the original URI — which works (CDN returns 200).
                // So we pass it through — UNLESS it's the mouflon placeholder "media.mp4".
                if (line.find("#EXT-X-MAP:URI") != std::string::npos)
                {
                    // Skip if URI is the mouflon placeholder "media.mp4" — it's not a
                    // real init segment; the real one has a CDN filename like
                    // "147917182_vr_init_xxx.mp4"
                    if (line.find("\"media.mp4\"") == std::string::npos &&
                        line.find("\"" + std::string(MOUFLON_FILENAME) + "\"") == std::string::npos)
                    {
                        decoded.push_back(line);
                    }
                    else
                    {
                        spdlog::debug("[Mouflon] v2 skipping EXT-X-MAP with placeholder URI: {}", line);
                    }
                    i++;
                    continue;
                }

                // Handle #EXT-X-MOUFLON:URI: segment lines
                if (line.find(MOUFLON_URI_ATTR) == 0)
                {
                    auto uriValue = line.substr(strlen(MOUFLON_URI_ATTR));
                    bool decodeOk = false;

                    if (uriValue.find(".mp4") != std::string::npos)
                    {
                        auto lastUs = uriValue.rfind('_');
                        if (lastUs != std::string::npos && lastUs > 0)
                        {
                            auto urlWithoutTimestamp = uriValue.substr(0, lastUs);
                            auto timestampPart = uriValue.substr(lastUs + 1);

                            auto secondLastUs = urlWithoutTimestamp.rfind('_');
                            if (secondLastUs != std::string::npos && secondLastUs > 0)
                            {
                                auto urlBeforeEnc = urlWithoutTimestamp.substr(0, secondLastUs);
                                auto encrypted = urlWithoutTimestamp.substr(secondLastUs + 1);

                                // Reverse, base64-decode, XOR-decrypt
                                std::string reversedEnc(encrypted.rbegin(), encrypted.rend());
                                auto data = base64Decode(reversedEnc + "==");
                                auto decryptedSeg = xorDecrypt(data, hashBytes);

                                // Validate: decrypted part must be printable ASCII
                                bool valid = !decryptedSeg.empty();
                                for (unsigned char c : decryptedSeg)
                                {
                                    if (c < 0x20 || c > 0x7E)
                                    {
                                        valid = false;
                                        break;
                                    }
                                }

                                if (valid)
                                {
                                    auto decodedUri = urlBeforeEnc + "_" + decryptedSeg + "_" + timestampPart;
                                    decoded.push_back(decodedUri);
                                    decodeOk = true;
                                }
                                else
                                {
                                    spdlog::debug("[Mouflon] v2 decrypt produced non-ASCII for segment, using original URI");
                                    decoded.push_back(uriValue);
                                    decodeOk = true;
                                }
                            }
                        }
                    }

                    // Always consume the following "media.mp4" line
                    i++;
                    if (i < lines.size() && lines[i].find("media.mp4") != std::string::npos)
                        i++;

                    if (!decodeOk)
                    {
                        // Failed to parse URI structure — use the raw URI value
                        spdlog::debug("[Mouflon] v2 could not parse URI: {}", uriValue);
                        decoded.push_back(uriValue);
                    }
                    continue;
                }

                decoded.push_back(line);
                i++;
            }
        }
        else
        {
            return content; // Unknown scheme
        }

        // Rejoin
        std::string result;
        for (size_t j = 0; j < decoded.size(); j++)
        {
            result += decoded[j];
            if (j + 1 < decoded.size())
                result += '\n';
        }
        return result;
    }

    std::string MouflonKeys::rewriteVariantUrl(const std::string &url,
                                               const std::string &psch,
                                               const std::string &pkey,
                                               const std::string &pdkey) const
    {
        // Rewrite media-hls URLs to direct b-hls CDN
        // From: https://media-hls.doppiocdn.com/b-hls-25/189420462/189420462.m3u8
        //   or: https://media-hls.doppiocdn.com/b-hls-32/147917182_vr/147917182_vr.m3u8
        // To:   https://b-hls-25.doppiocdn.live/hls/189420462/189420462.m3u8?psch=v2&pkey=X&pdkey=Y
        static const std::regex mediaHlsRe(
            R"(https://media-hls\.doppiocdn\.\w+/(b-hls-\d+)/([^/]+)/(.+))");

        std::smatch match;
        if (std::regex_match(url, match, mediaHlsRe))
        {
            auto bHlsServer = match[1].str();
            auto streamId = match[2].str();
            auto filename = match[3].str();

            // Strip existing query params
            auto qpos = filename.find('?');
            if (qpos != std::string::npos)
                filename = filename.substr(0, qpos);

            return "https://" + bHlsServer + ".doppiocdn.live/hls/" +
                   streamId + "/" + filename +
                   "?psch=" + psch + "&pkey=" + pkey + "&pdkey=" + pdkey;
        }

        // URL doesn't match expected pattern — just append keys
        if (url.find("pkey=") == std::string::npos || url.find("pdkey=") == std::string::npos)
        {
            char sep = (url.find('?') != std::string::npos) ? '&' : '?';
            return url + sep + "psch=" + psch + "&pkey=" + pkey + "&pdkey=" + pdkey;
        }

        return url;
    }

    // ─────────────────────────────────────────────────────────────────
    // Key cache
    // ─────────────────────────────────────────────────────────────────

    std::string MouflonKeys::getCachePath() const
    {
        // Look for cache file relative to executable
        auto exePath = fs::current_path();
        return (exePath / "stripchat_mouflon_keys.json").string();
    }

    void MouflonKeys::loadFromCache()
    {
        try
        {
            auto path = getCachePath();
            if (!fs::exists(path))
                return;

            std::ifstream f(path);
            if (!f.is_open())
                return;

            auto json = nlohmann::json::parse(f);
            if (json.is_object())
            {
                for (auto &[k, v] : json.items())
                {
                    if (v.is_string())
                        keys_[k] = v.get<std::string>();
                }
                spdlog::debug("[Mouflon] Loaded {} keys from cache", json.size());
            }
        }
        catch (const std::exception &e)
        {
            spdlog::debug("[Mouflon] Cache load failed: {}", e.what());
        }
    }

    void MouflonKeys::saveToCache() const
    {
        try
        {
            auto path = getCachePath();
            nlohmann::json j = keys_;
            std::ofstream f(path);
            if (f.is_open())
            {
                f << j.dump(2);
                spdlog::debug("[Mouflon] Saved {} keys to cache", keys_.size());
            }
        }
        catch (const std::exception &e)
        {
            spdlog::debug("[Mouflon] Cache save failed: {}", e.what());
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Fetch Doppio JS from MMP CDN
    // ─────────────────────────────────────────────────────────────────

    bool MouflonKeys::fetchDoppioJs(HttpClient &http)
    {
        // Retry up to 3 times (matches Python's Retry(total=2) + initial attempt)
        constexpr int maxRetries = 3;
        for (int attempt = 1; attempt <= maxRetries; attempt++)
        {
            if (attempt > 1)
            {
                int delaySec = attempt; // 2s, 3s backoff
                spdlog::info("[Mouflon] Retrying in {}s (attempt {}/{})...", delaySec, attempt, maxRetries);
                std::this_thread::sleep_for(std::chrono::seconds(delaySec));
            }

            bool ok = fetchDoppioJsOnce(http);
            if (ok)
                return true;
        }

        spdlog::warn("[Mouflon] All {} fetch attempts failed", maxRetries);
        return false;
    }

    bool MouflonKeys::fetchDoppioJsOnce(HttpClient &http)
    {
        try
        {
            // Step 1: Fetch static config
            auto resp = http.get("https://hu.stripchat.com/api/front/v3/config/static", 15);
            if (!resp.ok())
            {
                spdlog::warn("[Mouflon] Static config fetch failed: HTTP {} ({})",
                             resp.statusCode, resp.error.empty() ? "no details" : resp.error);
                return false;
            }

            auto json = nlohmann::json::parse(resp.body);

            // Handle different response structures
            nlohmann::json staticData;
            if (json.contains("static"))
                staticData = json["static"];
            else
                staticData = json;

            if (!staticData.contains("featuresV2"))
            {
                spdlog::warn("[Mouflon] No featuresV2 in static config");
                return false;
            }

            // Get MMP version
            std::string mmpVersion;
            try
            {
                mmpVersion = staticData["featuresV2"]["playerModuleExternalLoading"]["mmpVersion"].get<std::string>();
            }
            catch (...)
            {
                spdlog::warn("[Mouflon] Could not extract mmpVersion");
                return false;
            }

            std::string mmpBase = "https://mmp.doppiocdn.com/player/mmp/" + mmpVersion;
            spdlog::debug("[Mouflon] MMP base: {}", mmpBase);

            // Step 2: Fetch main.js
            spdlog::debug("[Mouflon] Fetching main.js from: {}/main.js", mmpBase);
            resp = http.get(mmpBase + "/main.js", 15);
            if (!resp.ok())
            {
                spdlog::warn("[Mouflon] main.js fetch failed: HTTP {} ({})",
                             resp.statusCode,
                             resp.error.empty() ? "no details" : resp.error);
                return false;
            }

            auto mainJsData = resp.body;

            // Step 3: Find Doppio JS filename
            std::string doppioJsName;

            // Try webpack chunk pattern: n.e(184)...DoppioWrapper
            static const std::regex doppioChunkPat(
                R"(n\.e\((\d+)\)\]\)\.then\(n\.bind\(n,\d+\)\)\)\.DoppioWrapper)");
            static const std::regex chunkHashPat(
                R"(n\.u=e=>"chunk-"\+\{([^}]+)\}\[e\]\+"\.js")");
            // Legacy require pattern
            static const std::regex doppioRequirePat(
                R"(require\(["']\./(Doppio[^"']+\.js)["']\))");
            // Index pattern
            static const std::regex doppioIndexPat(
                R"(([0-9]+):"Doppio")");

            std::smatch match;

            if (std::regex_search(mainJsData, match, doppioRequirePat))
            {
                doppioJsName = match[1].str();
            }
            else if (std::regex_search(mainJsData, match, doppioChunkPat))
            {
                auto chunkId = match[1].str();
                std::smatch hashMatch;
                if (std::regex_search(mainJsData, hashMatch, chunkHashPat))
                {
                    auto chunkMapping = hashMatch[1].str();
                    // Parse: 149:"hash1",184:"hash2",...
                    std::istringstream mappingStream(chunkMapping);
                    std::string entry;
                    while (std::getline(mappingStream, entry, ','))
                    {
                        auto colonPos = entry.find(':');
                        if (colonPos != std::string::npos)
                        {
                            auto cid = entry.substr(0, colonPos);
                            // Trim whitespace
                            cid.erase(0, cid.find_first_not_of(" \t"));
                            cid.erase(cid.find_last_not_of(" \t") + 1);

                            if (cid == chunkId)
                            {
                                auto chash = entry.substr(colonPos + 1);
                                // Remove quotes
                                chash.erase(std::remove(chash.begin(), chash.end(), '"'), chash.end());
                                chash.erase(std::remove(chash.begin(), chash.end(), '\''), chash.end());
                                chash.erase(0, chash.find_first_not_of(" \t"));
                                chash.erase(chash.find_last_not_of(" \t") + 1);
                                doppioJsName = "chunk-" + chash + ".js";
                                break;
                            }
                        }
                    }
                }
            }
            else if (std::regex_search(mainJsData, match, doppioIndexPat))
            {
                auto idx = match[1].str();
                // Look for hash in various formats
                std::vector<std::string> hashPatterns = {
                    idx + R"RE(:\\"([a-zA-Z0-9]{20})\\")RE",
                    idx + R"RE(:"([a-zA-Z0-9]{20})")RE",
                    "\"" + idx + R"RE(":"([a-zA-Z0-9]{20})")RE",
                };
                for (const auto &pat : hashPatterns)
                {
                    std::regex hashRe(pat);
                    std::smatch hm;
                    if (std::regex_search(mainJsData, hm, hashRe))
                    {
                        doppioJsName = "chunk-Doppio-" + hm[1].str() + ".js";
                        break;
                    }
                }
            }

            if (doppioJsName.empty())
            {
                spdlog::warn("[Mouflon] Could not find Doppio JS file in main.js");
                return false;
            }

            spdlog::debug("[Mouflon] Doppio JS: {}", doppioJsName);

            // Step 4: Fetch Doppio JS
            spdlog::debug("[Mouflon] Fetching Doppio JS: {}/{}", mmpBase, doppioJsName);
            resp = http.get(mmpBase + "/" + doppioJsName, 15);
            if (!resp.ok())
            {
                spdlog::warn("[Mouflon] Doppio JS fetch failed: HTTP {} ({})",
                             resp.statusCode,
                             resp.error.empty() ? "no details" : resp.error);
                return false;
            }

            doppioJsData_ = resp.body;
            return true;
        }
        catch (const std::exception &e)
        {
            spdlog::warn("[Mouflon] fetchDoppioJs failed: {}", e.what());
            return false;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Key parsing
    // ─────────────────────────────────────────────────────────────────

    void MouflonKeys::parseKeys()
    {
        if (doppioJsData_.empty())
            return;

        // Try v2.1.3 first
        auto keys = extractKeysV213(doppioJsData_);
        if (keys)
        {
            keys_[keys->first] = keys->second;
            spdlog::info("[Mouflon] Extracted v2.1.3 keys: pkey={}, pdkey={}", keys->first, keys->second);
            return;
        }

        // Try v2.1.1
        keys = extractKeysV211(doppioJsData_);
        if (keys)
        {
            keys_[keys->first] = keys->second;
            spdlog::info("[Mouflon] Extracted v2.1.1 keys: pkey={}, pdkey={}", keys->first, keys->second);
            return;
        }

        // Try legacy
        keys = extractKeysLegacy(doppioJsData_);
        if (keys)
        {
            keys_[keys->first] = keys->second;
            spdlog::info("[Mouflon] Extracted legacy keys: pkey={}, pdkey={}", keys->first, keys->second);
            return;
        }

        // Fallback: parse "pkey:pdkey" patterns
        parseLegacyKeyPairs(doppioJsData_);
    }

    std::optional<std::pair<std::string, std::string>>
    MouflonKeys::extractKeysV213(const std::string &js) const
    {
        if (js.find("const Jn=") == std::string::npos)
            return std::nullopt;

        try
        {
            auto start = js.find("const Jn=");
            if (start == std::string::npos)
                return std::nullopt;

            auto chunk = js.substr(start, std::min<size_t>(3000, js.size() - start));

            // Find all IIFEs: }(num,num,num,...)
            static const std::regex iifePat(R"(\}\((\d+(?:,\d+)+)\))");
            std::vector<std::vector<int>> iifes;
            for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), iifePat);
                 it != std::sregex_iterator(); ++it)
            {
                auto argsStr = (*it)[1].str();
                std::vector<int> args;
                std::istringstream argStream(argsStr);
                std::string arg;
                while (std::getline(argStream, arg, ','))
                    args.push_back(std::stoi(arg));
                iifes.push_back(args);
            }

            // First IIFE (10-16 args, first arg 40-50) → pkey part1
            std::string pkeyPart1;
            for (const auto &args : iifes)
            {
                if (args.size() >= 10 && args.size() <= 16 && args[0] >= 40 && args[0] <= 50)
                {
                    pkeyPart1 = decodeIifeV213(args, 38);
                    break;
                }
            }

            // 36918.toString(36) → "shi"
            std::string pkeyPart2;
            if (chunk.find("36918") != std::string::npos)
                pkeyPart2 = toBase36(36918);

            // Hex number for pdkey part1
            std::string pdkeyPart1;
            static const std::regex hexPat(R"(0x([0-9a-fA-F]+))");
            std::smatch hexMatch;
            if (std::regex_search(chunk, hexMatch, hexPat))
            {
                auto hexVal = std::stoll(hexMatch[1].str(), nullptr, 16);
                pdkeyPart1 = toBase36(hexVal);
            }
            else
            {
                // Try large decimal numbers
                static const std::regex largePat(R"(\b(\d{12,16})\b)");
                for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), largePat);
                     it != std::sregex_iterator(); ++it)
                {
                    auto n = std::stoll((*it)[1].str());
                    if (n > 100000000000LL)
                    {
                        pdkeyPart1 = toBase36(n);
                        break;
                    }
                }
            }

            // 32 shifted by -39 → 'P'
            std::string pdkeyPart2;
            if (chunk.find("32") != std::string::npos)
                pdkeyPart2 = shiftChars(toBase36(32), -39);

            // 24.toString(36) → 'o'
            std::string pdkeyPart3;
            if (chunk.find("24") != std::string::npos)
                pdkeyPart3 = toBase36(24);

            // Second IIFE (18-22 args, first arg 40-45) → pdkey "odi6" part
            std::string pdkeyPart4;
            for (const auto &args : iifes)
            {
                if (args.size() >= 18 && args.size() <= 22 && args[0] >= 40 && args[0] <= 45)
                {
                    auto decoded = decodeIifeV213Pdkey(args, 39);
                    if (decoded.size() >= 4)
                        pdkeyPart4 = decoded.substr(0, 4);
                    break;
                }
            }

            auto pkey = pkeyPart1 + pkeyPart2;
            auto pdkey = pdkeyPart1 + pdkeyPart2 + pdkeyPart3 + pdkeyPart4;

            if (pkey.size() >= 12 && pdkey.size() >= 12)
                return std::make_pair(pkey, pdkey);
        }
        catch (const std::exception &e)
        {
            spdlog::debug("[Mouflon] v2.1.3 extraction failed: {}", e.what());
        }

        return std::nullopt;
    }

    std::optional<std::pair<std::string, std::string>>
    MouflonKeys::extractKeysV211(const std::string &js) const
    {
        if (js.find("const ss=") == std::string::npos)
            return std::nullopt;

        try
        {
            auto start = js.find("const ss=(");
            if (start == std::string::npos)
                start = js.find("const ss=");
            if (start == std::string::npos)
                return std::nullopt;

            auto chunk = js.substr(start, std::min<size_t>(10000, js.size() - start));

            // Extract numbers in toString(36) calls
            std::map<int64_t, std::string> numbers;
            {
                static const std::regex pat1(R"((\d+)\.\.toString\(36\))");
                for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), pat1);
                     it != std::sregex_iterator(); ++it)
                {
                    auto n = std::stoll((*it)[1].str());
                    numbers[n] = toBase36(n);
                }
                static const std::regex pat2(R"((\d+)\[[A-Za-z]+\([^)]+\)\]\(36\))");
                for (auto it = std::sregex_iterator(chunk.begin(), chunk.end(), pat2);
                     it != std::sregex_iterator(); ++it)
                {
                    auto n = std::stoll((*it)[1].str());
                    numbers[n] = toBase36(n);
                }
            }

            // Find IIFEs
            std::vector<std::vector<int>> iifes;
            {
                static const std::regex iifePat(R"(\}\((\d+(?:,\d+)+)\))");
                auto chunkPart = chunk.substr(0, std::min<size_t>(5000, chunk.size()));
                for (auto it = std::sregex_iterator(chunkPart.begin(), chunkPart.end(), iifePat);
                     it != std::sregex_iterator(); ++it)
                {
                    auto argsStr = (*it)[1].str();
                    std::vector<int> args;
                    std::istringstream argStream(argsStr);
                    std::string arg;
                    while (std::getline(argStream, arg, ','))
                        args.push_back(std::stoi(arg));
                    if (args.size() >= 2 && args.size() <= 15)
                        iifes.push_back(args);
                }
            }

            // Build pkey
            std::string p1 = (numbers.count(16)) ? shiftChars(toBase36(16), -13) : "";

            std::string p2;
            for (const auto &[n, s] : numbers)
            {
                if (n > 1000000000000LL)
                {
                    p2 = s;
                    break;
                }
            }

            std::string p3;
            for (const auto &args : iifes)
            {
                if (args.size() == 4 && args[0] >= 30 && args[0] <= 40)
                {
                    auto decoded = decodeIife(args, 11);
                    bool allLower = true;
                    for (char c : decoded)
                        if (!std::isalpha(c) || !std::islower(c))
                            allLower = false;
                    if (allLower && !decoded.empty())
                    {
                        p3 = decoded;
                        break;
                    }
                }
            }

            auto p4it = numbers.find(690102);
            std::string p4 = (p4it != numbers.end()) ? p4it->second : "";

            // Build pdkey
            auto p5it = numbers.find(39286);
            std::string p5 = (p5it != numbers.end()) ? p5it->second : "";

            std::string p6;
            for (const auto &args : iifes)
            {
                if (args.size() == 5 && args[0] >= 60 && args[0] <= 65)
                {
                    auto decoded = decodeIife(args, 10);
                    bool allLower = true;
                    for (char c : decoded)
                        if (!std::isalpha(c) || !std::islower(c))
                            allLower = false;
                    if (allLower && !decoded.empty())
                    {
                        p6 = decoded;
                        break;
                    }
                }
            }

            auto p7it = numbers.find(9672);
            std::string p7 = (p7it != numbers.end()) ? p7it->second : "";

            std::string p8 = (numbers.count(32)) ? shiftChars(toBase36(32), -39) : "";

            auto p9it = numbers.find(888);
            std::string p9 = (p9it != numbers.end()) ? p9it->second : "";

            std::string p10;
            for (const auto &args : iifes)
            {
                if (args.size() == 3 && args[0] >= 40 && args[0] <= 50)
                {
                    auto decoded = decodeIife(args, 39);
                    bool allLower = true;
                    for (char c : decoded)
                        if (!std::isalpha(c) || !std::islower(c))
                            allLower = false;
                    if (allLower && !decoded.empty())
                    {
                        p10 = decoded;
                        break;
                    }
                }
            }

            auto p11it = numbers.find(6);
            std::string p11 = (p11it != numbers.end()) ? p11it->second : "";

            auto pkey = p1 + p2 + p3 + p4;
            auto pdkey = p5 + p6 + p7 + p8 + p9 + p10 + p11;

            if (pkey.size() >= 12 && pdkey.size() >= 12)
                return std::make_pair(pkey, pdkey);
        }
        catch (const std::exception &e)
        {
            spdlog::debug("[Mouflon] v2.1.1 extraction failed: {}", e.what());
        }

        return std::nullopt;
    }

    std::optional<std::pair<std::string, std::string>>
    MouflonKeys::extractKeysLegacy(const std::string &js) const
    {
        if (js.find("const ns=") == std::string::npos)
            return std::nullopt;

        try
        {
            // Find the two IIFEs
            static const std::regex iife1Pat(R"(\}\((\d+(?:,\d+){8,12})\))");
            static const std::regex iife2Pat(R"(\}\((\d{2},\d{3},\d{3})\))");

            std::smatch m1, m2;
            if (!std::regex_search(js, m1, iife1Pat) || !std::regex_search(js, m2, iife2Pat))
                return std::nullopt;

            auto parseArgs = [](const std::string &s) -> std::vector<int>
            {
                std::vector<int> args;
                std::istringstream ss(s);
                std::string tok;
                while (std::getline(ss, tok, ','))
                    args.push_back(std::stoi(tok));
                return args;
            };

            auto args1 = parseArgs(m1[1].str());
            auto args2 = parseArgs(m2[1].str());

            // Decode IIFE1
            int n1 = args1[0];
            std::vector<int> rem1(args1.begin() + 1, args1.end());
            std::reverse(rem1.begin(), rem1.end());
            std::string p4;
            for (size_t i = 0; i < rem1.size(); i++)
                p4 += static_cast<char>((rem1[i] - n1 - 26) - static_cast<int>(i));

            // Decode IIFE2
            int o2 = args2[0];
            std::vector<int> rem2(args2.begin() + 1, args2.end());
            std::reverse(rem2.begin(), rem2.end());
            std::string p8;
            for (size_t i = 0; i < rem2.size(); i++)
                p8 += static_cast<char>(((rem2[i] - o2) - 56) - static_cast<int>(i));

            auto p1 = shiftChars(toBase36(16), -13);
            auto p2 = toBase36(0x531f77594da7dLL);
            auto p3 = toBase36(18676);
            auto p5 = toBase36(662856);
            auto p6 = shiftChars(toBase36(32), -39);
            auto p7 = toBase36(31981);

            auto keyString = p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8;

            auto colonPos = keyString.find(':');
            if (colonPos != std::string::npos)
            {
                auto pkey = keyString.substr(0, colonPos);
                auto pdkey = keyString.substr(colonPos + 1);
                if (pkey.size() >= 8 && pdkey.size() >= 8)
                    return std::make_pair(pkey, pdkey);
            }
        }
        catch (const std::exception &e)
        {
            spdlog::debug("[Mouflon] Legacy extraction failed: {}", e.what());
        }

        return std::nullopt;
    }

    void MouflonKeys::parseLegacyKeyPairs(const std::string &js)
    {
        // Look for "pkey:pdkey" patterns in JS
        static const std::regex pat(R"RE("(\w{8,24}):(\w{8,24})")RE");
        for (auto it = std::sregex_iterator(js.begin(), js.end(), pat);
             it != std::sregex_iterator(); ++it)
        {
            keys_[(*it)[1].str()] = (*it)[2].str();
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Crypto helpers
    // ─────────────────────────────────────────────────────────────────

    std::string MouflonKeys::sha256(const std::string &input)
    {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char *>(input.data()),
               input.size(), hash);
        return std::string(reinterpret_cast<char *>(hash), SHA256_DIGEST_LENGTH);
    }

    std::string MouflonKeys::base64Decode(const std::string &input)
    {
        // Use OpenSSL EVP for base64 decoding
        // Strip existing padding first — callers add "==" blindly (like Python),
        // which can create excess padding (e.g. 16-char input + "==" = 18 → broken).
        std::string padded = input;
        while (!padded.empty() && padded.back() == '=')
            padded.pop_back();

        // Add correct padding to make length a multiple of 4
        while (padded.size() % 4 != 0)
            padded += '=';

        // Replace URL-safe chars
        for (auto &c : padded)
        {
            if (c == '-')
                c = '+';
            else if (c == '_')
                c = '/';
        }

        // Decode using EVP
        auto *ctx = EVP_ENCODE_CTX_new();
        if (!ctx)
            return "";

        std::vector<unsigned char> out(padded.size()); // output always smaller
        int outLen = 0;
        int tmpLen = 0;

        EVP_DecodeInit(ctx);
        if (EVP_DecodeUpdate(ctx, out.data(), &outLen,
                             reinterpret_cast<const unsigned char *>(padded.data()),
                             static_cast<int>(padded.size())) < 0)
        {
            EVP_ENCODE_CTX_free(ctx);
            return "";
        }

        if (EVP_DecodeFinal(ctx, out.data() + outLen, &tmpLen) < 0)
        {
            EVP_ENCODE_CTX_free(ctx);
            // Return what we have so far — some inputs may have trailing issues
            return std::string(reinterpret_cast<char *>(out.data()), outLen);
        }

        outLen += tmpLen;
        EVP_ENCODE_CTX_free(ctx);

        return std::string(reinterpret_cast<char *>(out.data()), outLen);
    }

    std::string MouflonKeys::xorDecrypt(const std::string &data, const std::string &hashBytes)
    {
        if (hashBytes.empty())
            return data; // nothing to decrypt with — return as-is
        std::string result;
        result.reserve(data.size());
        for (size_t i = 0; i < data.size(); i++)
        {
            result += static_cast<char>(
                static_cast<unsigned char>(data[i]) ^
                static_cast<unsigned char>(hashBytes[i % hashBytes.size()]));
        }
        return result;
    }

    // ─────────────────────────────────────────────────────────────────
    // Base36 / char shift / IIFE decoders
    // ─────────────────────────────────────────────────────────────────

    std::string MouflonKeys::toBase36(int64_t n)
    {
        static const char chars[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        if (n == 0)
            return "0";

        std::string result;
        bool negative = (n < 0);
        if (negative)
            n = -n;

        while (n > 0)
        {
            result = chars[n % 36] + result;
            n /= 36;
        }

        return negative ? "-" + result : result;
    }

    std::string MouflonKeys::shiftChars(const std::string &s, int offset)
    {
        std::string result;
        result.reserve(s.size());
        for (char c : s)
            result += static_cast<char>(static_cast<int>(c) + offset);
        return result;
    }

    std::string MouflonKeys::decodeIifeV213(const std::vector<int> &args, int offset)
    {
        if (args.size() < 2)
            return "";

        int first = args[0];
        std::vector<int> remaining(args.begin() + 1, args.end());
        std::reverse(remaining.begin(), remaining.end());

        std::string result;
        for (size_t i = 0; i < remaining.size(); i++)
            result += static_cast<char>((remaining[i] - first - offset) - static_cast<int>(i));

        return result;
    }

    std::string MouflonKeys::decodeIifeV213Pdkey(const std::vector<int> &args, int offset)
    {
        return decodeIifeV213(args, offset);
    }

    std::string MouflonKeys::decodeIife(const std::vector<int> &args, int offset)
    {
        if (args.size() < 2)
            return "";

        int first = args[0];
        std::vector<int> remaining(args.begin() + 1, args.end());
        std::reverse(remaining.begin(), remaining.end());

        std::string result;
        for (size_t i = 0; i < remaining.size(); i++)
            result += static_cast<char>((remaining[i] - first - offset) - static_cast<int>(i));

        return result;
    }

} // namespace sm
