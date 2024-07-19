#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Proxy pool with round-robin and health tracking
// ─────────────────────────────────────────────────────────────────

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <optional>

namespace sm
{

    // ── Proxy type enum (matches AppConfig::ProxyType) ──────────────
    enum class ProxyType
    {
        None = 0,
        HTTP,
        HTTPS,
        SOCKS4,
        SOCKS4A,
        SOCKS5,
        SOCKS5H // SOCKS5 with hostname resolution on proxy side
    };

    const char *proxyTypeToString(ProxyType type);
    ProxyType parseProxyType(const std::string &s);
    int proxyTypeToCurlType(ProxyType type);

    // ── Single proxy entry ──────────────────────────────────────────
    struct ProxyEntry
    {
        std::string url; // e.g. "http://user:pass@host:port" or "socks5://host:port"
        ProxyType type = ProxyType::None;
        bool rolling = false; // Rolling proxy (IP changes per request)
        bool enabled = true;  // Can be disabled by user or auto-disabled on failures
        std::string name;     // Optional friendly name

        // Health tracking (not persisted)
        int failureCount = 0;
        int successCount = 0;
        std::chrono::steady_clock::time_point lastUsed;
        std::chrono::steady_clock::time_point lastFailure;
        std::chrono::steady_clock::time_point disabledUntil;

        ProxyEntry() = default;
        ProxyEntry(const std::string &u, ProxyType t = ProxyType::HTTP, bool r = false)
            : url(u), type(t), rolling(r) {}

        bool isTemporarilyDisabled() const
        {
            return disabledUntil > std::chrono::steady_clock::now();
        }
    };

    // ── Proxy pool with round-robin selection and health tracking ───
    class ProxyPool
    {
    public:
        ProxyPool() = default;

        // Configuration
        void setProxies(std::vector<ProxyEntry> proxies);
        void addProxy(const ProxyEntry &proxy);
        void removeProxy(const std::string &url);
        void clear();

        // Get next available proxy (round-robin among healthy proxies)
        // Returns nullopt if no healthy proxies available
        std::optional<ProxyEntry> getNext();

        // Get a specific proxy by index
        std::optional<ProxyEntry> getByIndex(size_t index);

        // Report success/failure for health tracking
        void reportSuccess(const std::string &proxyUrl);
        void reportFailure(const std::string &proxyUrl);

        // Re-enable all temporarily disabled proxies
        void resetHealth();

        // Getters
        std::vector<ProxyEntry> getAll() const;
        size_t size() const;
        bool empty() const;
        bool hasHealthyProxy() const;

        // Configuration options
        void setMaxFailuresBeforeDisable(int n) { maxFailuresBeforeDisable_ = n; }
        void setDisableDurationSec(int sec) { disableDurationSec_ = sec; }
        void setAutoDisable(bool enable) { autoDisable_ = enable; }

    private:
        mutable std::mutex mutex_;
        std::vector<ProxyEntry> proxies_;
        size_t currentIndex_ = 0;

        // Health tracking settings
        int maxFailuresBeforeDisable_ = 5; // Failures before temp-disable
        int disableDurationSec_ = 60;      // Seconds to disable after max failures
        bool autoDisable_ = true;          // Auto-disable failing proxies
    };

} // namespace sm
