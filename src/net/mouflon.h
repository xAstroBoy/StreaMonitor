#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — StripChat Mouflon key extraction & playlist
//                     decryption system
//
// Port of Python stripchat.py mouflon system:
//   - Fetch Doppio JS from MMP CDN
//   - Extract pkey/pdkey pairs (v2.1.3, v2.1.1, legacy)
//   - Decrypt mouflon-encrypted HLS playlists (SHA256-XOR)
//   - Rewrite media-hls URLs to b-hls direct CDN with auth params
// ─────────────────────────────────────────────────────────────────

#include "net/http_client.h"
#include <string>
#include <map>
#include <mutex>
#include <optional>
#include <functional>
#include <tuple>

namespace sm
{

    class MouflonKeys
    {
    public:
        // Get singleton instance
        static MouflonKeys &instance();

        // Initialize: fetch static config → main.js → Doppio JS → extract keys
        // Call once at startup (thread-safe, will skip if already initialized)
        void initialize(HttpClient &http);

        // Re-initialize: force re-fetch keys even if already initialized
        void reinitialize(HttpClient &http);

        // Extract psch, pkey, pdkey from a playlist's #EXT-X-MOUFLON lines
        struct MouflonInfo
        {
            std::string psch;  // version: "v1" or "v2"
            std::string pkey;  // public key
            std::string pdkey; // decryption key
        };

        // Decode a mouflon-encrypted playlist (equivalent to Python m3u_decoder)
        // Returns decoded playlist content, or original if no mouflon encryption
        std::string decodePlaylists(const std::string &content) const;

        // Overload with fallback keys — used when media playlist may lack
        // #EXT-X-MOUFLON: lines (keys only in master playlist).
        // Tries extracting from content first; if that fails, uses fallbackInfo.
        std::string decodePlaylists(const std::string &content,
                                    const MouflonInfo &fallbackInfo) const;

        // Look up pdkey for a given pkey
        std::optional<std::string> getDecKey(const std::string &pkey) const;

        // Rewrite a variant URL to use direct CDN with auth keys
        // From: https://media-hls.doppiocdn.com/b-hls-25/189420462/189420462.m3u8
        // To:   https://b-hls-25.doppiocdn.live/hls/189420462/189420462.m3u8?psch=v2&pkey=X&pdkey=Y
        std::string rewriteVariantUrl(const std::string &url,
                                      const std::string &psch,
                                      const std::string &pkey,
                                      const std::string &pdkey) const;

        MouflonInfo extractFromPlaylist(const std::string &m3u8Content) const;

        // Get all known keys (for debug logging)
        std::map<std::string, std::string> getKeys() const;

        // Add a new pkey→pdkey pair and persist to cache
        void addKey(const std::string &pkey, const std::string &pdkey);

        // Remove a key by pkey and persist to cache
        void removeKey(const std::string &pkey);

        // Check if keys are available
        bool hasKeys() const;

    private:
        MouflonKeys();
        ~MouflonKeys() = default;

        // Non-copyable
        MouflonKeys(const MouflonKeys &) = delete;
        MouflonKeys &operator=(const MouflonKeys &) = delete;

        // Key storage (pkey → pdkey)
        mutable std::mutex mutex_;
        std::map<std::string, std::string> keys_;
        bool initialized_ = false;

        // Doppio JS content (kept for dynamic key lookups)
        std::string doppioJsData_;

        // ── Internal methods ────────────────────────────────────────

        // Load/save key cache from JSON file
        void loadFromCache();
        void saveToCache() const;
        std::string getCachePath() const;

        // Fetch initial data: static config → MMP version → main.js → Doppio JS
        // Retries up to 3 times on failure
        bool fetchDoppioJs(HttpClient &http);
        bool fetchDoppioJsOnce(HttpClient &http);

        // Extract keys from Doppio JS
        void parseKeys();
        std::optional<std::pair<std::string, std::string>> extractKeysV213(const std::string &js) const;
        std::optional<std::pair<std::string, std::string>> extractKeysV211(const std::string &js) const;
        std::optional<std::pair<std::string, std::string>> extractKeysLegacy(const std::string &js) const;
        void parseLegacyKeyPairs(const std::string &js);

        // Crypto helpers
        static std::string sha256(const std::string &input);
        static std::string base64Decode(const std::string &input);
        static std::string xorDecrypt(const std::string &data, const std::string &hashBytes);

        // Base36 conversion
        static std::string toBase36(int64_t n);

        // Character shift
        static std::string shiftChars(const std::string &s, int offset);

        // IIFE decoders
        static std::string decodeIifeV213(const std::vector<int> &args, int offset = 38);
        static std::string decodeIifeV213Pdkey(const std::vector<int> &args, int offset = 39);
        static std::string decodeIife(const std::vector<int> &args, int offset);
    };

} // namespace sm
