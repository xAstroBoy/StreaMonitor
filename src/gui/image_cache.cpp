// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Image Cache Implementation
// Background download + stb_image decode + GL texture upload
// ─────────────────────────────────────────────────────────────────

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "gui/stb_image.h"

#include "gui/image_cache.h"
#include <spdlog/spdlog.h>

// Minimal HTTP download (we use WinHTTP on Windows, curl otherwise)
#ifdef _WIN32
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <algorithm>

namespace sm
{

    ImageCache::~ImageCache()
    {
        // clear() queues textures for deletion; process them now.
        // Destructor runs on the main (GL) thread during app teardown.
        clear();
        std::lock_guard dlock(deleteMutex_);
        if (!pendingDeletes_.empty())
        {
            glDeleteTextures(static_cast<GLsizei>(pendingDeletes_.size()),
                             pendingDeletes_.data());
            pendingDeletes_.clear();
        }
    }

    GLuint ImageCache::getTexture(const std::string &url)
    {
        if (url.empty())
            return 0;

        std::lock_guard lock(mutex_);
        auto it = cache_.find(url);
        if (it != cache_.end())
        {
            if (it->second.state == State::Ready)
            {
                // Auto-refresh local file previews every 10 seconds
                // (live stream thumbnails update on disk periodically)
                bool isLocal = false;
#ifdef _WIN32
                isLocal = (url.size() > 2 && url[1] == ':');
#else
                isLocal = (!url.empty() && url[0] == '/');
#endif
                if (isLocal)
                {
                    auto age = std::chrono::steady_clock::now() - it->second.loadedAt;
                    if (std::chrono::duration_cast<std::chrono::seconds>(age).count() >= 30)
                    {
                        // Stale — delete texture and re-fetch
                        if (it->second.textureId)
                            glDeleteTextures(1, &it->second.textureId);
                        cache_.erase(it);
                        // Fall through to start a new download below
                    }
                    else
                    {
                        return it->second.textureId;
                    }
                }
                else
                {
                    return it->second.textureId;
                }
            }
            else
            {
                // Still loading or failed — for failed local files, retry after a bit
                if (it->second.state == State::Failed)
                {
                    auto age = std::chrono::steady_clock::now() - it->second.loadedAt;
                    if (std::chrono::duration_cast<std::chrono::seconds>(age).count() >= 5)
                    {
                        cache_.erase(it);
                        // Fall through to retry
                    }
                    else
                    {
                        return 0;
                    }
                }
                else
                {
                    return 0; // still loading
                }
            }
        }

        // Start async download (limit concurrent threads)
        if (activeDownloads_.load() < 4)
        {
            cache_[url] = Entry{State::Loading, 0, 0, 0, std::chrono::steady_clock::now()};
            activeDownloads_.fetch_add(1);
            std::thread([this, url]()
                        {
                            downloadAndDecode(url);
                            activeDownloads_.fetch_sub(1); })
                .detach();
        }
        else
        {
            // Too many concurrent downloads — don't cache as Loading,
            // let it retry next frame when a slot opens
        }

        return 0;
    }

    void ImageCache::uploadPending()
    {
        // Process pending texture deletions (must happen on GL thread)
        {
            std::lock_guard dlock(deleteMutex_);
            if (!pendingDeletes_.empty())
            {
                glDeleteTextures(static_cast<GLsizei>(pendingDeletes_.size()),
                                 pendingDeletes_.data());
                pendingDeletes_.clear();
            }
        }

        std::lock_guard lock(uploadMutex_);
        while (!pendingUploads_.empty())
        {
            auto &p = pendingUploads_.front();

            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, p.width, p.height, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, p.pixels.data());
            glBindTexture(GL_TEXTURE_2D, 0);

            {
                std::lock_guard lock2(mutex_);
                auto it = cache_.find(p.url);
                if (it != cache_.end())
                {
                    it->second.state = State::Ready;
                    it->second.textureId = tex;
                    it->second.width = p.width;
                    it->second.height = p.height;
                    it->second.loadedAt = std::chrono::steady_clock::now();
                }
            }

            pendingUploads_.pop_front();
        }
    }

    void ImageCache::invalidate(const std::string &url)
    {
        std::lock_guard lock(mutex_);
        auto it = cache_.find(url);
        if (it != cache_.end())
        {
            if (it->second.textureId)
            {
                // Queue for deletion on GL thread instead of deleting here
                std::lock_guard dlock(deleteMutex_);
                pendingDeletes_.push_back(it->second.textureId);
            }
            cache_.erase(it);
        }
    }

    void ImageCache::clear()
    {
        std::lock_guard lock(mutex_);
        for (auto &[url, entry] : cache_)
        {
            if (entry.textureId)
            {
                std::lock_guard dlock(deleteMutex_);
                pendingDeletes_.push_back(entry.textureId);
            }
        }
        cache_.clear();
    }

    size_t ImageCache::size() const
    {
        std::lock_guard lock(mutex_);
        return cache_.size();
    }

    // ─────────────────────────────────────────────────────────────────
    // Background: download URL → decode JPEG/PNG → queue for GL upload
    // ─────────────────────────────────────────────────────────────────
#ifdef _WIN32
    static bool httpDownload(const std::string &url, std::vector<unsigned char> &out)
    {
        // Parse URL
        std::wstring wurl(url.begin(), url.end());

        URL_COMPONENTS urlComp = {};
        urlComp.dwStructSize = sizeof(urlComp);
        wchar_t hostBuf[256] = {};
        wchar_t pathBuf[2048] = {};
        urlComp.lpszHostName = hostBuf;
        urlComp.dwHostNameLength = 256;
        urlComp.lpszUrlPath = pathBuf;
        urlComp.dwUrlPathLength = 2048;

        if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &urlComp))
            return false;

        bool isHttps = (urlComp.nScheme == INTERNET_SCHEME_HTTPS);

        HINTERNET session = WinHttpOpen(L"StreaMonitor/2.0",
                                        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        if (!session)
            return false;

        HINTERNET connect = WinHttpConnect(session, hostBuf,
                                           urlComp.nPort, 0);
        if (!connect)
        {
            WinHttpCloseHandle(session);
            return false;
        }

        DWORD flags = isHttps ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET request = WinHttpOpenRequest(connect, L"GET", pathBuf,
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!request)
        {
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        // Set timeouts (8s — prevent hangs on slow/dead CDN URLs)
        DWORD timeout = 8000;
        WinHttpSetOption(request, WINHTTP_OPTION_RESOLVE_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(request, WINHTTP_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(request, WINHTTP_OPTION_SEND_TIMEOUT, &timeout, sizeof(timeout));
        WinHttpSetOption(request, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

        if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(request, nullptr))
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        // Check status code
        DWORD statusCode = 0;
        DWORD size = sizeof(statusCode);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (statusCode != 200)
        {
            WinHttpCloseHandle(request);
            WinHttpCloseHandle(connect);
            WinHttpCloseHandle(session);
            return false;
        }

        // Read response
        out.clear();
        DWORD bytesAvailable = 0;
        while (WinHttpQueryDataAvailable(request, &bytesAvailable) && bytesAvailable > 0)
        {
            std::vector<unsigned char> buf(bytesAvailable);
            DWORD bytesRead = 0;
            WinHttpReadData(request, buf.data(), bytesAvailable, &bytesRead);
            out.insert(out.end(), buf.begin(), buf.begin() + bytesRead);

            if (out.size() > 5 * 1024 * 1024) // 5MB limit
                break;
        }

        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return !out.empty();
    }
#else
    // Non-Windows: use curl
    static size_t curlWriteCb(void *data, size_t sz, size_t nmemb, void *userp)
    {
        auto *buf = static_cast<std::vector<unsigned char> *>(userp);
        auto totalSize = sz * nmemb;
        buf->insert(buf->end(), (unsigned char *)data, (unsigned char *)data + totalSize);
        return totalSize;
    }

    static bool httpDownload(const std::string &url, std::vector<unsigned char> &out)
    {
        // Fallback stub — on non-Windows we'd use libcurl here
        // For now return false
        return false;
    }
#endif

    void ImageCache::downloadAndDecode(const std::string &url)
    {
        std::vector<unsigned char> data;

        // Check if this is a local file path (e.g. preview captured from stream)
        bool isLocalFile = false;
#ifdef _WIN32
        isLocalFile = (url.size() > 2 && url[1] == ':' && (url[2] == '\\' || url[2] == '/'));
#else
        isLocalFile = (!url.empty() && url[0] == '/');
#endif

        if (isLocalFile)
        {
            // Read directly from local file
            FILE *f = fopen(url.c_str(), "rb");
            if (f)
            {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                if (sz > 0 && sz < 10 * 1024 * 1024)
                {
                    data.resize(static_cast<size_t>(sz));
                    fread(data.data(), 1, data.size(), f);
                }
                fclose(f);
            }
        }
        else
        {
            httpDownload(url, data);
        }

        if (data.empty())
        {
            std::lock_guard lock(mutex_);
            auto it = cache_.find(url);
            if (it != cache_.end())
            {
                it->second.state = State::Failed;
                it->second.loadedAt = std::chrono::steady_clock::now();
            }
            return;
        }

        // Decode with stb_image
        int w = 0, h = 0, channels = 0;
        unsigned char *pixels = stbi_load_from_memory(
            data.data(), (int)data.size(), &w, &h, &channels, 4 /*RGBA*/);

        if (!pixels || w <= 0 || h <= 0)
        {
            if (pixels)
                stbi_image_free(pixels);
            std::lock_guard lock(mutex_);
            auto it = cache_.find(url);
            if (it != cache_.end())
            {
                it->second.state = State::Failed;
                it->second.loadedAt = std::chrono::steady_clock::now();
            }
            return;
        }

        // Queue for GL upload on the main thread
        PendingUpload upload;
        upload.url = url;
        upload.width = w;
        upload.height = h;
        upload.pixels.assign(pixels, pixels + (w * h * 4));
        stbi_image_free(pixels);

        {
            std::lock_guard lock(uploadMutex_);
            pendingUploads_.push_back(std::move(upload));
        }
    }

} // namespace sm
