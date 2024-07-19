#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — HTTP client (libcurl wrapper)
// ─────────────────────────────────────────────────────────────────

#include "net/proxy_pool.h"
#include <string>
#include <map>
#include <vector>
#include <optional>
#include <mutex>
#include <memory>
#include <functional>
#include <chrono>

namespace sm
{

    struct HttpResponse
    {
        long statusCode = 0;
        std::string body;
        std::map<std::string, std::string> headers;
        std::string error;
        double totalTimeSec = 0;

        bool ok() const { return statusCode >= 200 && statusCode < 300; }
        bool isRateLimit() const { return statusCode == 429; }
        bool isNotFound() const { return statusCode == 404; }
        bool isServerError() const { return statusCode >= 500; }
        // Connection error: curl failed before getting any HTTP response
        // This is NOT a rate limit - it's a network/DNS/timeout failure
        bool isConnectionError() const { return statusCode == 0 && !error.empty(); }
    };

    struct HttpRequest
    {
        std::string url;
        std::string method = "GET";
        std::map<std::string, std::string> headers;
        std::string body;        // POST body
        std::string contentType; // for POST
        int timeoutSec = 30;
        bool verifySsl = true;
        bool followRedirects = true;
        std::string userAgent;
        std::string cookieString; // manual cookie header
    };

    class HttpClient
    {
    public:
        HttpClient();
        ~HttpClient();

        // Non-copyable, movable
        HttpClient(const HttpClient &) = delete;
        HttpClient &operator=(const HttpClient &) = delete;
        HttpClient(HttpClient &&) noexcept;
        HttpClient &operator=(HttpClient &&) noexcept;

        // Configuration
        void setDefaultUserAgent(const std::string &ua);
        void setDefaultTimeout(int seconds);
        void setVerifySsl(bool verify);
        void setDefaultHeaders(const std::map<std::string, std::string> &headers);

        // Proxy configuration — use ProxyType enum
        void setProxy(const std::string &proxyUrl, ProxyType proxyType = ProxyType::HTTP);
        void setProxy(const ProxyEntry &proxy);
        void clearProxy();

        // Simple methods
        HttpResponse get(const std::string &url, int timeoutSec = 0);
        HttpResponse get(const HttpRequest &req); // convenience: sets method=GET and executes
        HttpResponse post(const std::string &url, const std::string &body,
                          const std::string &contentType = "application/x-www-form-urlencoded",
                          int timeoutSec = 0);
        HttpResponse post(const HttpRequest &req, const std::string &body); // convenience: sets method=POST
        HttpResponse postJson(const std::string &url, const std::string &jsonBody,
                              int timeoutSec = 0);

        // Full control
        HttpResponse execute(const HttpRequest &req);

        // Download to file
        bool downloadToFile(const std::string &url, const std::string &filePath,
                            int timeoutSec = 300);

        // Fetch text (convenience)
        std::optional<std::string> fetchText(const std::string &url, int timeoutSec = 0);

        // Get cookie string for a domain
        std::string getCookiesForUrl(const std::string &url) const;

        // URL encode
        static std::string urlEncode(const std::string &value);

        // Global init/cleanup (call once at program start/end)
        static void globalInit();
        static void globalCleanup();

        // Get current proxy URL (for health reporting)
        std::string currentProxyUrl() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    // ── Rate limiter ────────────────────────────────────────────────
    class RateLimiter
    {
    public:
        RateLimiter(int maxRequests, double perSeconds, double backoffMin = 2.0,
                    double backoffMax = 30.0);

        void acquire(); // blocks until allowed
        void reportError();
        void reportSuccess();

    private:
        std::mutex mutex_;
        int maxTokens_;
        double refillRate_;
        double tokens_;
        double backoffMin_, backoffMax_, currentBackoff_;
        std::chrono::steady_clock::time_point lastRefill_;
    };

} // namespace sm
