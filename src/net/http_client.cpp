#include "net/http_client.h"
#include <curl/curl.h>
#include <spdlog/spdlog.h>
#include <thread>
#include <algorithm>
#include <fstream>
#include <filesystem>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // CURL write callbacks
    // ─────────────────────────────────────────────────────────────────
    static size_t writeStringCallback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        auto &str = *static_cast<std::string *>(userp);
        str.append(static_cast<char *>(contents), size * nmemb);
        return size * nmemb;
    }

    static size_t writeFileCallback(void *contents, size_t size, size_t nmemb, void *userp)
    {
        auto &f = *static_cast<std::ofstream *>(userp);
        f.write(static_cast<char *>(contents), size * nmemb);
        return size * nmemb;
    }

    static size_t headerCallback(char *buffer, size_t size, size_t nitems, void *userp)
    {
        auto &headers = *static_cast<std::map<std::string, std::string> *>(userp);
        std::string line(buffer, size * nitems);
        auto colon = line.find(':');
        if (colon != std::string::npos)
        {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim whitespace
            val.erase(0, val.find_first_not_of(" \t\r\n"));
            val.erase(val.find_last_not_of(" \t\r\n") + 1);
            // Lowercase key
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            headers[key] = val;
        }
        return size * nitems;
    }

    // ─────────────────────────────────────────────────────────────────
    // HttpClient::Impl
    // ─────────────────────────────────────────────────────────────────
    struct HttpClient::Impl
    {
        CURL *curl = nullptr;
        std::string defaultUA;
        int defaultTimeout = 30;
        bool verifySsl = true;
        std::map<std::string, std::string> defaultHeaders;
        std::string proxyUrl;
        long proxyType = 0; // CURLPROXY_HTTP
        std::mutex mutex;   // one CURL handle is not thread-safe

        Impl()
        {
            curl = curl_easy_init();
        }

        ~Impl()
        {
            if (curl)
                curl_easy_cleanup(curl);
        }

        void applyProxy(CURL *c)
        {
            if (!proxyUrl.empty())
            {
                curl_easy_setopt(c, CURLOPT_PROXY, proxyUrl.c_str());
                curl_easy_setopt(c, CURLOPT_PROXYTYPE, proxyType);
                // If proxy URL contains user:pass@, libcurl handles it automatically.
                // Tunnel HTTPS through CONNECT only for actual HTTP proxies.
                // NEVER set HTTPPROXYTUNNEL for SOCKS proxies — even if proxyType
                // was mis-detected, the URL scheme is authoritative (Issue #3).
                if ((proxyType == CURLPROXY_HTTP || proxyType == CURLPROXY_HTTPS) &&
                    proxyUrl.find("socks") == std::string::npos)
                    curl_easy_setopt(c, CURLOPT_HTTPPROXYTUNNEL, 1L);
            }
        }

        HttpResponse execute(const HttpRequest &req)
        {
            std::lock_guard lock(mutex);
            HttpResponse resp;

            if (!curl)
            {
                resp.error = "CURL not initialized";
                return resp;
            }

            curl_easy_reset(curl);

            // URL
            curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());

            // Method
            if (req.method == "POST")
            {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)req.body.size());
            }
            else if (req.method == "PUT")
            {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
            }
            else if (req.method == "DELETE")
            {
                curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            }

            // Headers
            struct curl_slist *headerList = nullptr;
            auto mergedHeaders = defaultHeaders;
            for (const auto &[k, v] : req.headers)
                mergedHeaders[k] = v;
            if (!req.contentType.empty())
                mergedHeaders["Content-Type"] = req.contentType;

            std::string ua = req.userAgent.empty() ? defaultUA : req.userAgent;
            if (!ua.empty())
                curl_easy_setopt(curl, CURLOPT_USERAGENT, ua.c_str());

            for (const auto &[k, v] : mergedHeaders)
            {
                std::string h = k + ": " + v;
                headerList = curl_slist_append(headerList, h.c_str());
            }
            if (headerList)
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

            // Cookies
            if (!req.cookieString.empty())
                curl_easy_setopt(curl, CURLOPT_COOKIE, req.cookieString.c_str());

            // SSL
            bool ssl = req.verifySsl && verifySsl;
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, ssl ? 1L : 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, ssl ? 2L : 0L);
#ifdef _WIN32
            // Use Windows native certificate store even with OpenSSL backend.
            // Without this, curl can't verify SSL certs (no CA bundle shipped).
            curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
#endif

            // Proxy
            applyProxy(curl);

            // Timeout
            int timeout = req.timeoutSec > 0 ? req.timeoutSec : defaultTimeout;
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)timeout);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, (long)std::min(timeout, 15));

            // Redirects
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);

            // Response callbacks
            std::string responseBody;
            std::map<std::string, std::string> responseHeaders;

            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeStringCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseBody);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCallback);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders);

            // Accept encoding (gzip)
            curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

            // Execute
            CURLcode res = curl_easy_perform(curl);

            if (res != CURLE_OK)
            {
                resp.error = curl_easy_strerror(res);
                spdlog::debug("HTTP request failed: {} - {}", req.url, resp.error);
            }
            else
            {
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp.statusCode);
                double totalTime = 0;
                curl_easy_getinfo(curl, CURLINFO_TOTAL_TIME, &totalTime);
                resp.totalTimeSec = totalTime;
            }

            resp.body = std::move(responseBody);
            resp.headers = std::move(responseHeaders);

            if (headerList)
                curl_slist_free_all(headerList);

            return resp;
        }
    };

    // ─────────────────────────────────────────────────────────────────
    // HttpClient public API
    // ─────────────────────────────────────────────────────────────────
    HttpClient::HttpClient() : impl_(std::make_unique<Impl>()) {}
    HttpClient::~HttpClient() = default;
    HttpClient::HttpClient(HttpClient &&) noexcept = default;
    HttpClient &HttpClient::operator=(HttpClient &&) noexcept = default;

    void HttpClient::setDefaultUserAgent(const std::string &ua) { impl_->defaultUA = ua; }
    void HttpClient::setDefaultTimeout(int seconds) { impl_->defaultTimeout = seconds; }
    void HttpClient::setVerifySsl(bool verify) { impl_->verifySsl = verify; }
    void HttpClient::setDefaultHeaders(const std::map<std::string, std::string> &headers)
    {
        impl_->defaultHeaders = headers;
    }

    void HttpClient::setProxy(const std::string &proxyUrl, ProxyType proxyType)
    {
        impl_->proxyUrl = proxyUrl;
        impl_->proxyType = proxyTypeToCurlType(proxyType);
    }

    void HttpClient::setProxy(const ProxyEntry &proxy)
    {
        impl_->proxyUrl = proxy.url;
        impl_->proxyType = proxyTypeToCurlType(proxy.type);
    }

    void HttpClient::clearProxy()
    {
        impl_->proxyUrl.clear();
        impl_->proxyType = 0;
    }

    std::string HttpClient::currentProxyUrl() const
    {
        return impl_->proxyUrl;
    }

    HttpResponse HttpClient::get(const std::string &url, int timeoutSec)
    {
        HttpRequest req;
        req.url = url;
        req.timeoutSec = timeoutSec;
        return execute(req);
    }

    HttpResponse HttpClient::get(const HttpRequest &req)
    {
        HttpRequest r = req;
        r.method = "GET";
        return execute(r);
    }

    HttpResponse HttpClient::post(const std::string &url, const std::string &body,
                                  const std::string &contentType, int timeoutSec)
    {
        HttpRequest req;
        req.url = url;
        req.method = "POST";
        req.body = body;
        req.contentType = contentType;
        req.timeoutSec = timeoutSec;
        return execute(req);
    }

    HttpResponse HttpClient::post(const HttpRequest &req, const std::string &body)
    {
        HttpRequest r = req;
        r.method = "POST";
        r.body = body;
        if (r.contentType.empty())
            r.contentType = "application/x-www-form-urlencoded";
        return execute(r);
    }

    HttpResponse HttpClient::postJson(const std::string &url, const std::string &jsonBody,
                                      int timeoutSec)
    {
        return post(url, jsonBody, "application/json", timeoutSec);
    }

    HttpResponse HttpClient::execute(const HttpRequest &req)
    {
        return impl_->execute(req);
    }

    bool HttpClient::downloadToFile(const std::string &url, const std::string &filePath,
                                    int timeoutSec)
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->curl)
            return false;

        curl_easy_reset(impl_->curl);
        curl_easy_setopt(impl_->curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT, (long)timeoutSec);
        curl_easy_setopt(impl_->curl, CURLOPT_FOLLOWLOCATION, 1L);

        if (!impl_->defaultUA.empty())
            curl_easy_setopt(impl_->curl, CURLOPT_USERAGENT, impl_->defaultUA.c_str());

        curl_easy_setopt(impl_->curl, CURLOPT_SSL_VERIFYPEER, impl_->verifySsl ? 1L : 0L);

        // Proxy
        impl_->applyProxy(impl_->curl);

        std::ofstream file(filePath, std::ios::binary);
        if (!file)
            return false;

        curl_easy_setopt(impl_->curl, CURLOPT_WRITEFUNCTION, writeFileCallback);
        curl_easy_setopt(impl_->curl, CURLOPT_WRITEDATA, &file);

        CURLcode res = curl_easy_perform(impl_->curl);
        file.close();

        if (res != CURLE_OK)
        {
            std::filesystem::remove(filePath);
            return false;
        }
        return true;
    }

    std::optional<std::string> HttpClient::fetchText(const std::string &url, int timeoutSec)
    {
        auto resp = get(url, timeoutSec);
        if (resp.ok())
            return resp.body;
        return std::nullopt;
    }

    std::string HttpClient::getCookiesForUrl(const std::string & /*url*/) const
    {
        // TODO: implement cookie jar
        return "";
    }

    // ─────────────────────────────────────────────────────────────────
    // RateLimiter
    // ─────────────────────────────────────────────────────────────────
    RateLimiter::RateLimiter(int maxRequests, double perSeconds,
                             double backoffMin, double backoffMax)
        : maxTokens_(maxRequests), refillRate_(maxRequests / perSeconds), tokens_(maxRequests), backoffMin_(backoffMin), backoffMax_(backoffMax), currentBackoff_(backoffMin), lastRefill_(std::chrono::steady_clock::now())
    {
    }

    void RateLimiter::acquire()
    {
        int waitMs = 0;
        {
            std::lock_guard lock(mutex_);

            auto now = std::chrono::steady_clock::now();
            double elapsed = std::chrono::duration<double>(now - lastRefill_).count();
            tokens_ = std::min(static_cast<double>(maxTokens_), tokens_ + elapsed * refillRate_);
            lastRefill_ = now;

            if (tokens_ < 1.0)
            {
                double waitSec = (1.0 - tokens_) / refillRate_;
                waitMs = static_cast<int>(waitSec * 1000);
                tokens_ = 0;
            }
            else
            {
                tokens_ -= 1.0;
            }
        }
        // Sleep OUTSIDE the lock so other threads aren't blocked
        if (waitMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    }

    void RateLimiter::reportError()
    {
        int waitMs = 0;
        {
            std::lock_guard lock(mutex_);
            currentBackoff_ = std::min(currentBackoff_ * 2.0, backoffMax_);
            waitMs = static_cast<int>(currentBackoff_ * 1000);
        }
        // Sleep OUTSIDE the lock
        if (waitMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(waitMs));
    }

    void RateLimiter::reportSuccess()
    {
        std::lock_guard lock(mutex_);
        currentBackoff_ = backoffMin_;
    }

    // ─────────────────────────────────────────────────────────────────
    // Global init / URL encode
    // ─────────────────────────────────────────────────────────────────
    void HttpClient::globalInit()
    {
        curl_global_init(CURL_GLOBAL_ALL);
    }

    void HttpClient::globalCleanup()
    {
        curl_global_cleanup();
    }

    std::string HttpClient::urlEncode(const std::string &value)
    {
        CURL *curl = curl_easy_init();
        if (!curl)
            return value;
        char *encoded = curl_easy_escape(curl, value.c_str(), (int)value.size());
        std::string result = encoded ? encoded : value;
        if (encoded)
            curl_free(encoded);
        curl_easy_cleanup(curl);
        return result;
    }

} // namespace sm
