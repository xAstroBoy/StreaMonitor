#include "net/proxy_pool.h"
#include <curl/curl.h>
#include <algorithm>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // Proxy type utilities
    // ─────────────────────────────────────────────────────────────────
    const char *proxyTypeToString(ProxyType type)
    {
        switch (type)
        {
        case ProxyType::HTTP:
            return "http";
        case ProxyType::HTTPS:
            return "https";
        case ProxyType::SOCKS4:
            return "socks4";
        case ProxyType::SOCKS4A:
            return "socks4a";
        case ProxyType::SOCKS5:
            return "socks5";
        case ProxyType::SOCKS5H:
            return "socks5h";
        default:
            return "none";
        }
    }

    ProxyType parseProxyType(const std::string &s)
    {
        if (s == "http")
            return ProxyType::HTTP;
        if (s == "https")
            return ProxyType::HTTPS;
        if (s == "socks4")
            return ProxyType::SOCKS4;
        if (s == "socks4a")
            return ProxyType::SOCKS4A;
        if (s == "socks5")
            return ProxyType::SOCKS5;
        if (s == "socks5h")
            return ProxyType::SOCKS5H;
        return ProxyType::None;
    }

    int proxyTypeToCurlType(ProxyType type)
    {
        switch (type)
        {
        case ProxyType::HTTP:
            return CURLPROXY_HTTP;
        case ProxyType::HTTPS:
            return CURLPROXY_HTTPS;
        case ProxyType::SOCKS4:
            return CURLPROXY_SOCKS4;
        case ProxyType::SOCKS4A:
            return CURLPROXY_SOCKS4A;
        case ProxyType::SOCKS5:
            return CURLPROXY_SOCKS5;
        case ProxyType::SOCKS5H:
            return CURLPROXY_SOCKS5_HOSTNAME;
        default:
            return CURLPROXY_HTTP;
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // ProxyPool implementation
    // ─────────────────────────────────────────────────────────────────
    void ProxyPool::setProxies(std::vector<ProxyEntry> proxies)
    {
        std::lock_guard lock(mutex_);
        proxies_ = std::move(proxies);
        currentIndex_ = 0;
    }

    void ProxyPool::addProxy(const ProxyEntry &proxy)
    {
        std::lock_guard lock(mutex_);
        // Check for duplicate
        for (const auto &p : proxies_)
        {
            if (p.url == proxy.url)
                return;
        }
        proxies_.push_back(proxy);
    }

    void ProxyPool::removeProxy(const std::string &url)
    {
        std::lock_guard lock(mutex_);
        proxies_.erase(
            std::remove_if(proxies_.begin(), proxies_.end(),
                           [&](const ProxyEntry &p)
                           { return p.url == url; }),
            proxies_.end());
        if (currentIndex_ >= proxies_.size() && !proxies_.empty())
            currentIndex_ = 0;
    }

    void ProxyPool::clear()
    {
        std::lock_guard lock(mutex_);
        proxies_.clear();
        currentIndex_ = 0;
    }

    std::optional<ProxyEntry> ProxyPool::getNext()
    {
        std::lock_guard lock(mutex_);

        if (proxies_.empty())
            return std::nullopt;

        // Find next enabled, non-disabled proxy (round-robin)
        auto now = std::chrono::steady_clock::now();
        size_t startIdx = currentIndex_;
        size_t attempts = 0;

        while (attempts < proxies_.size())
        {
            auto &proxy = proxies_[currentIndex_];
            currentIndex_ = (currentIndex_ + 1) % proxies_.size();
            attempts++;

            if (proxy.enabled && proxy.disabledUntil <= now)
            {
                proxy.lastUsed = now;
                return proxy;
            }
        }

        // All proxies disabled — check if any can be re-enabled
        for (auto &proxy : proxies_)
        {
            if (proxy.enabled && proxy.disabledUntil <= now)
            {
                proxy.lastUsed = now;
                return proxy;
            }
        }

        return std::nullopt;
    }

    std::optional<ProxyEntry> ProxyPool::getByIndex(size_t index)
    {
        std::lock_guard lock(mutex_);
        if (index >= proxies_.size())
            return std::nullopt;
        return proxies_[index];
    }

    void ProxyPool::reportSuccess(const std::string &proxyUrl)
    {
        std::lock_guard lock(mutex_);
        for (auto &proxy : proxies_)
        {
            if (proxy.url == proxyUrl)
            {
                proxy.successCount++;
                proxy.failureCount = 0;   // Reset failure count on success
                proxy.disabledUntil = {}; // Re-enable if was temporarily disabled
                return;
            }
        }
    }

    void ProxyPool::reportFailure(const std::string &proxyUrl)
    {
        std::lock_guard lock(mutex_);
        for (auto &proxy : proxies_)
        {
            if (proxy.url == proxyUrl)
            {
                proxy.failureCount++;
                proxy.lastFailure = std::chrono::steady_clock::now();

                // Auto-disable if too many failures
                if (autoDisable_ && proxy.failureCount >= maxFailuresBeforeDisable_)
                {
                    proxy.disabledUntil = std::chrono::steady_clock::now() +
                                          std::chrono::seconds(disableDurationSec_);
                }
                return;
            }
        }
    }

    void ProxyPool::resetHealth()
    {
        std::lock_guard lock(mutex_);
        for (auto &proxy : proxies_)
        {
            proxy.failureCount = 0;
            proxy.disabledUntil = {};
        }
    }

    std::vector<ProxyEntry> ProxyPool::getAll() const
    {
        std::lock_guard lock(mutex_);
        return proxies_;
    }

    size_t ProxyPool::size() const
    {
        std::lock_guard lock(mutex_);
        return proxies_.size();
    }

    bool ProxyPool::empty() const
    {
        std::lock_guard lock(mutex_);
        return proxies_.empty();
    }

    bool ProxyPool::hasHealthyProxy() const
    {
        std::lock_guard lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (const auto &proxy : proxies_)
        {
            if (proxy.enabled && proxy.disabledUntil <= now)
                return true;
        }
        return false;
    }

} // namespace sm
