// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — HTTP/2 + TLS Server Implementation
// nghttp2 for HTTP/2 framing, OpenSSL for TLS, HTTP/1.1 fallback
// ─────────────────────────────────────────────────────────────────

#include "web/h2_server.h"
#include <spdlog/spdlog.h>
#include <openssl/err.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cstring>
#include <cerrno>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#endif

namespace sm
{

    // ─── Helpers ──────────────────────────────────────────────────
    namespace
    {
        nghttp2_nv makeNV(const std::string &name, const std::string &value,
                          uint8_t flags = NGHTTP2_NV_FLAG_NONE)
        {
            nghttp2_nv nv{};
            nv.name = (uint8_t *)name.data();
            nv.namelen = name.size();
            nv.value = (uint8_t *)value.data();
            nv.valuelen = value.size();
            nv.flags = flags;
            return nv;
        }

        std::string urlDecode(const std::string &s)
        {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size())
                {
                    int hi = 0, lo = 0;
                    if (std::sscanf(s.c_str() + i + 1, "%1x%1x", &hi, &lo) == 2)
                    {
                        out += static_cast<char>((hi << 4) | lo);
                        i += 2;
                        continue;
                    }
                }
                else if (s[i] == '+')
                {
                    out += ' ';
                    continue;
                }
                out += s[i];
            }
            return out;
        }

        void parseQueryString(const std::string &qs, std::map<std::string, std::string> &params)
        {
            std::istringstream ss(qs);
            std::string pair;
            while (std::getline(ss, pair, '&'))
            {
                auto eq = pair.find('=');
                if (eq != std::string::npos)
                    params[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
                else
                    params[urlDecode(pair)] = "";
            }
        }

        void splitPath(const std::string &raw, std::string &path, std::string &query)
        {
            auto q = raw.find('?');
            if (q != std::string::npos)
            {
                path = raw.substr(0, q);
                query = raw.substr(q + 1);
            }
            else
            {
                path = raw;
                query.clear();
            }
        }
    } // namespace

    // ─── H2Request ────────────────────────────────────────────────
    std::string H2Request::get_header_value(const std::string &key) const
    {
        auto it = headers.find(key);
        return it != headers.end() ? it->second : "";
    }

    bool H2Request::has_header(const std::string &key) const
    {
        return headers.count(key) > 0;
    }

    // ─── H2DataSink ──────────────────────────────────────────────
    bool H2DataSink::write(const char *data, size_t len)
    {
        if (!writable_)
            return false;
        buffer_.append(data, len);
        return true;
    }

    bool H2DataSink::is_writable() const { return writable_; }

    size_t H2DataSink::readInto(uint8_t *buf, size_t maxLen)
    {
        size_t n = std::min(maxLen, buffer_.size());
        if (n > 0)
        {
            std::memcpy(buf, buffer_.data(), n);
            buffer_.erase(0, n);
        }
        return n;
    }

    // ─── H2Response ──────────────────────────────────────────────
    void H2Response::set_content(const std::string &content, const std::string &content_type)
    {
        body = content;
        contentType_ = content_type;
    }

    void H2Response::set_header(const std::string &key, const std::string &value)
    {
        headers[key] = value;
    }

    void H2Response::set_content_provider(
        const std::string &content_type,
        ContentProvider provider,
        ContentProviderDone done)
    {
        hasProvider_ = true;
        contentType_ = content_type;
        provider_ = std::move(provider);
        providerDone_ = std::move(done);
    }

    // ─── MIME type guesser ────────────────────────────────────────
    std::string H2Server::guessMimeType(const std::string &path)
    {
        auto ext = std::filesystem::path(path).extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".html" || ext == ".htm")
            return "text/html; charset=utf-8";
        if (ext == ".css")
            return "text/css; charset=utf-8";
        if (ext == ".js" || ext == ".mjs")
            return "application/javascript; charset=utf-8";
        if (ext == ".json")
            return "application/json";
        if (ext == ".png")
            return "image/png";
        if (ext == ".jpg" || ext == ".jpeg")
            return "image/jpeg";
        if (ext == ".gif")
            return "image/gif";
        if (ext == ".svg")
            return "image/svg+xml";
        if (ext == ".ico")
            return "image/x-icon";
        if (ext == ".woff")
            return "font/woff";
        if (ext == ".woff2")
            return "font/woff2";
        if (ext == ".ttf")
            return "font/ttf";
        if (ext == ".txt")
            return "text/plain; charset=utf-8";
        if (ext == ".xml")
            return "application/xml";
        if (ext == ".webp")
            return "image/webp";
        return "application/octet-stream";
    }

    // ─── SSL setup ────────────────────────────────────────────────
    bool H2Server::initSSL()
    {
        sslCtx_ = SSL_CTX_new(TLS_server_method());
        if (!sslCtx_)
        {
            spdlog::error("SSL_CTX_new failed");
            return false;
        }

        if (SSL_CTX_use_certificate_file(sslCtx_, certPath_.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            spdlog::error("Failed to load SSL certificate: {}", certPath_);
            return false;
        }
        if (SSL_CTX_use_PrivateKey_file(sslCtx_, keyPath_.c_str(), SSL_FILETYPE_PEM) != 1)
        {
            spdlog::error("Failed to load SSL private key: {}", keyPath_);
            return false;
        }

        // ALPN callback — negotiate h2 or http/1.1
        SSL_CTX_set_alpn_select_cb(sslCtx_, alpnSelectCb, nullptr);

        // Enable HTTP/2 in OpenSSL
        SSL_CTX_set_options(sslCtx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

        spdlog::info("SSL context initialised (cert={})", certPath_);
        return true;
    }

    int H2Server::alpnSelectCb(SSL *, const unsigned char **out,
                               unsigned char *outlen, const unsigned char *in,
                               unsigned int inlen, void *)
    {
        // Prefer h2, fall back to http/1.1
        if (nghttp2_select_next_protocol((unsigned char **)out, outlen, in, inlen) == 1)
            return SSL_TLSEXT_ERR_OK; // h2 selected

        // Manual scan for http/1.1
        for (unsigned int i = 0; i < inlen;)
        {
            unsigned int plen = in[i];
            if (plen + 1 + i > inlen)
                break;
            if (plen == 8 && std::memcmp(&in[i + 1], "http/1.1", 8) == 0)
            {
                *out = &in[i + 1];
                *outlen = 8;
                return SSL_TLSEXT_ERR_OK;
            }
            i += 1 + plen;
        }
        return SSL_TLSEXT_ERR_NOACK;
    }

    // ─── SSL I/O helpers ──────────────────────────────────────────
    bool H2Server::sslWriteAll(SSL *ssl, const uint8_t *data, size_t len)
    {
        size_t written = 0;
        while (written < len)
        {
            int n = SSL_write(ssl, data + written, static_cast<int>(len - written));
            if (n <= 0)
            {
                int err = SSL_get_error(ssl, n);
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
                    continue;
                return false;
            }
            written += static_cast<size_t>(n);
        }
        return true;
    }

    bool H2Server::sslWriteAll(SSL *ssl, const std::string &data)
    {
        return sslWriteAll(ssl, reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }

    std::string H2Server::sslReadLine(SSL *ssl)
    {
        std::string line;
        char ch;
        while (true)
        {
            int n = SSL_read(ssl, &ch, 1);
            if (n <= 0)
                break;
            if (ch == '\n')
                break;
            if (ch != '\r')
                line += ch;
        }
        return line;
    }

    bool H2Server::sslReadN(SSL *ssl, std::string &out, size_t n)
    {
        out.resize(n);
        size_t total = 0;
        while (total < n)
        {
            int r = SSL_read(ssl, &out[total], static_cast<int>(n - total));
            if (r <= 0)
            {
                int err = SSL_get_error(ssl, r);
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
                    continue;
                return false;
            }
            total += static_cast<size_t>(r);
        }
        return true;
    }

    // ─── Constructor / Destructor ─────────────────────────────────
    H2Server::H2Server(const std::string &certPath, const std::string &keyPath)
        : certPath_(certPath), keyPath_(keyPath) {}

    H2Server::~H2Server() { stop(); }

    // ─── Route registration ───────────────────────────────────────
    void H2Server::addRoute(const std::string &method, const std::string &pattern, RouteHandler handler)
    {
        Route r;
        r.method = method;
        r.rawPattern = pattern;
        // If pattern looks like a regex or contains capture groups, use it as-is
        // Otherwise wrap with ^ $ for exact match
        std::string regexStr = pattern;
        if (regexStr.find('(') == std::string::npos &&
            regexStr.find('[') == std::string::npos &&
            regexStr.find('.') == std::string::npos &&
            regexStr.find('*') == std::string::npos)
        {
            regexStr = "^" + regexStr + "$";
        }
        r.pattern = std::regex(regexStr);
        r.handler = std::move(handler);
        routes_.push_back(std::move(r));
    }

    void H2Server::Get(const std::string &p, RouteHandler h) { addRoute("GET", p, std::move(h)); }
    void H2Server::Post(const std::string &p, RouteHandler h) { addRoute("POST", p, std::move(h)); }
    void H2Server::Put(const std::string &p, RouteHandler h) { addRoute("PUT", p, std::move(h)); }
    void H2Server::Delete(const std::string &p, RouteHandler h) { addRoute("DELETE", p, std::move(h)); }
    void H2Server::Options(const std::string &p, RouteHandler h) { addRoute("OPTIONS", p, std::move(h)); }

    void H2Server::set_default_headers(std::map<std::string, std::string> headers)
    {
        defaultHeaders_ = std::move(headers);
    }

    bool H2Server::set_mount_point(const std::string &prefix, const std::string &dir)
    {
        if (!std::filesystem::exists(dir))
            return false;
        mountPoints_.emplace_back(prefix, dir);
        return true;
    }

    void H2Server::set_error_handler(ErrorHandler handler)
    {
        errorHandler_ = std::move(handler);
    }

    // ─── Route matching ───────────────────────────────────────────
    RouteHandler H2Server::matchRoute(const std::string &method, const std::string &path,
                                      std::vector<std::string> &captures) const
    {
        for (const auto &route : routes_)
        {
            if (route.method != method)
                continue;
            std::smatch m;
            if (std::regex_match(path, m, route.pattern))
            {
                captures.clear();
                for (size_t i = 0; i < m.size(); ++i)
                    captures.push_back(m[i].str());
                return route.handler;
            }
        }
        return nullptr;
    }

    // ─── Static file serving ──────────────────────────────────────
    bool H2Server::tryServeStatic(const std::string &path, H2Response &res) const
    {
        for (const auto &[prefix, dir] : mountPoints_)
        {
            if (path.find(prefix) != 0)
                continue;
            std::string relPath = path.substr(prefix.size());
            if (relPath.empty() || relPath == "/")
                relPath = "/index.html";
            // Prevent path traversal
            if (relPath.find("..") != std::string::npos)
                continue;

            auto filePath = std::filesystem::path(dir) / relPath.substr(1);
            if (std::filesystem::exists(filePath) && std::filesystem::is_regular_file(filePath))
            {
                std::ifstream ifs(filePath, std::ios::binary);
                if (!ifs)
                    continue;
                std::string content((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
                res.set_content(content, guessMimeType(filePath.string()));
                return true;
            }
        }
        return false;
    }

    // ─── listen / stop ────────────────────────────────────────────
    bool H2Server::listen(const std::string &host, int port)
    {
        if (running_.load())
            return true;

#ifdef _WIN32
        WSADATA wd;
        WSAStartup(MAKEWORD(2, 2), &wd);
#endif

        if (!initSSL())
            return false;

        // Create listening socket
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ == SM_INVALID_SOCKET)
        {
            spdlog::error("socket() failed: {}", errno);
            return false;
        }

        // Allow port reuse
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&one), sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (host == "0.0.0.0" || host.empty())
            addr.sin_addr.s_addr = INADDR_ANY;
        else
            inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

        if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0)
        {
            spdlog::error("bind({}:{}) failed: {}", host, port, errno);
            closesocket(listenFd_);
            listenFd_ = SM_INVALID_SOCKET;
            return false;
        }

        if (::listen(listenFd_, SOMAXCONN) != 0)
        {
            spdlog::error("listen() failed: {}", errno);
            closesocket(listenFd_);
            listenFd_ = SM_INVALID_SOCKET;
            return false;
        }

        running_.store(true);
        acceptThread_ = std::make_unique<std::thread>([this]()
                                                      { acceptLoop(); });
        return true;
    }

    void H2Server::stop()
    {
        if (!running_.load())
            return;
        running_.store(false);

        // Close listen socket to unblock accept()
        if (listenFd_ != SM_INVALID_SOCKET)
        {
            closesocket(listenFd_);
            listenFd_ = SM_INVALID_SOCKET;
        }

        if (acceptThread_ && acceptThread_->joinable())
            acceptThread_->join();

        // Wait for connection threads
        {
            std::lock_guard<std::mutex> lk(connMutex_);
            for (auto &t : connThreads_)
            {
                if (t && t->joinable())
                    t->join();
            }
            connThreads_.clear();
        }

        if (sslCtx_)
        {
            SSL_CTX_free(sslCtx_);
            sslCtx_ = nullptr;
        }

        spdlog::info("H2Server stopped");
    }

    // ─── Accept loop ──────────────────────────────────────────────
    void H2Server::acceptLoop()
    {
        spdlog::info("H2Server accept loop started");
        while (running_.load())
        {
            sockaddr_in clientAddr{};
            socklen_t len = sizeof(clientAddr);
            sm_socket_t clientFd = ::accept(listenFd_,
                                            reinterpret_cast<sockaddr *>(&clientAddr), &len);
            if (clientFd == SM_INVALID_SOCKET)
            {
                if (running_.load())
                    spdlog::debug("accept() returned error (probably shutting down)");
                break;
            }

            char ipBuf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
            spdlog::debug("New connection from {}", ipBuf);

            // Spawn connection thread
            std::lock_guard<std::mutex> lk(connMutex_);
            connThreads_.push_back(std::make_unique<std::thread>(
                [this, clientFd]()
                { handleConnection(clientFd); }));
        }
        spdlog::info("H2Server accept loop exiting");
    }

    // ─── Connection handler (TLS handshake → dispatch h2/h1.1) ───
    void H2Server::handleConnection(sm_socket_t clientFd)
    {
        H2Connection conn;
        conn.fd = clientFd;
        conn.server = this;

        // Create SSL handle
        conn.ssl = SSL_new(sslCtx_);
        if (!conn.ssl)
        {
            closesocket(clientFd);
            return;
        }
        SSL_set_fd(conn.ssl, static_cast<int>(clientFd));

        // TLS handshake
        if (SSL_accept(conn.ssl) <= 0)
        {
            spdlog::debug("TLS handshake failed");
            SSL_free(conn.ssl);
            closesocket(clientFd);
            return;
        }

        // Check ALPN result
        const unsigned char *alpn = nullptr;
        unsigned int alpnLen = 0;
        SSL_get0_alpn_selected(conn.ssl, &alpn, &alpnLen);
        conn.isHttp2 = (alpnLen == 2 && std::memcmp(alpn, "h2", 2) == 0);

        if (conn.isHttp2)
        {
            spdlog::debug("HTTP/2 connection established");
            runHttp2(conn);
        }
        else
        {
            spdlog::debug("HTTP/1.1 connection established");
            runHttp11(conn);
        }

        // Cleanup
        if (conn.session)
            nghttp2_session_del(conn.session);
        SSL_shutdown(conn.ssl);
        SSL_free(conn.ssl);
        closesocket(clientFd);
    }

    // ═══════════════════════════════════════════════════════════════
    //                       nghttp2 CALLBACKS
    // ═══════════════════════════════════════════════════════════════

    int H2Server::onBeginHeaders(nghttp2_session *, const nghttp2_frame *frame, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
            return 0;

        auto stream = std::make_unique<H2Stream>();
        stream->streamId = frame->hd.stream_id;
        conn->streams[frame->hd.stream_id] = std::move(stream);
        return 0;
    }

    int H2Server::onHeader(nghttp2_session *, const nghttp2_frame *frame,
                           const uint8_t *name, size_t namelen,
                           const uint8_t *value, size_t valuelen,
                           uint8_t, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);
        if (frame->hd.type != NGHTTP2_HEADERS)
            return 0;

        auto it = conn->streams.find(frame->hd.stream_id);
        if (it == conn->streams.end())
            return 0;

        std::string n(reinterpret_cast<const char *>(name), namelen);
        std::string v(reinterpret_cast<const char *>(value), valuelen);

        auto &req = it->second->request;
        if (n == ":method")
            req.method = v;
        else if (n == ":path")
        {
            req.raw_path = v;
            std::string query;
            splitPath(v, req.path, query);
            if (!query.empty())
                parseQueryString(query, req.params);
        }
        else if (n[0] != ':')
            req.headers[n] = v;

        return 0;
    }

    int H2Server::onFrameRecv(nghttp2_session *, const nghttp2_frame *frame, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);

        // Request complete when we see END_STREAM
        if (frame->hd.type == NGHTTP2_HEADERS &&
            frame->headers.cat == NGHTTP2_HCAT_REQUEST &&
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        {
            auto it = conn->streams.find(frame->hd.stream_id);
            if (it != conn->streams.end() && !it->second->requestComplete)
            {
                it->second->requestComplete = true;
                conn->server->processRequest(*conn, frame->hd.stream_id);
            }
        }
        else if (frame->hd.type == NGHTTP2_DATA &&
                 (frame->hd.flags & NGHTTP2_FLAG_END_STREAM))
        {
            auto it = conn->streams.find(frame->hd.stream_id);
            if (it != conn->streams.end() && !it->second->requestComplete)
            {
                it->second->requestComplete = true;
                conn->server->processRequest(*conn, frame->hd.stream_id);
            }
        }
        return 0;
    }

    int H2Server::onDataChunkRecv(nghttp2_session *, uint8_t, int32_t stream_id,
                                  const uint8_t *data, size_t len, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);
        auto it = conn->streams.find(stream_id);
        if (it != conn->streams.end())
            it->second->request.body.append(reinterpret_cast<const char *>(data), len);
        return 0;
    }

    int H2Server::onStreamClose(nghttp2_session *, int32_t stream_id,
                                uint32_t, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);
        auto it = conn->streams.find(stream_id);
        if (it != conn->streams.end())
        {
            // Fire done callback for streaming responses
            auto &stream = it->second;
            if (stream->streamingActive && stream->response.providerDone_)
                stream->response.providerDone_(true);
            stream->dataSink.writable_ = false;
            conn->streams.erase(it);
        }
        return 0;
    }

    // ─── Data source read (nghttp2 pulls response body) ──────────
    ssize_t H2Connection::onDataSourceRead(
        nghttp2_session *, int32_t stream_id,
        uint8_t *buf, size_t length, uint32_t *data_flags,
        nghttp2_data_source *source, void *user_data)
    {
        auto *conn = static_cast<H2Connection *>(user_data);
        auto it = conn->streams.find(stream_id);
        if (it == conn->streams.end())
        {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }

        auto &stream = *it->second;

        // ── Regular (non-streaming) response ──
        if (!stream.response.hasProvider_)
        {
            size_t remaining = stream.response.body.size() - stream.bodyOffset;
            size_t n = std::min(length, remaining);
            if (n > 0)
            {
                std::memcpy(buf, stream.response.body.data() + stream.bodyOffset, n);
                stream.bodyOffset += n;
            }
            if (stream.bodyOffset >= stream.response.body.size())
                *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return static_cast<ssize_t>(n);
        }

        // ── Streaming response — read from dataSink buffer ──
        size_t n = stream.dataSink.readInto(buf, length);
        if (n > 0)
            return static_cast<ssize_t>(n);

        if (stream.streamingEnded)
        {
            *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return 0;
        }

        // No data yet — defer and retry when provider yields data
        return NGHTTP2_ERR_DEFERRED;
    }

    // ─── Process a complete request → route → submit response ────
    void H2Server::processRequest(H2Connection &conn, int32_t streamId)
    {
        auto it = conn.streams.find(streamId);
        if (it == conn.streams.end())
            return;
        auto &stream = *it->second;

        // Try route match
        auto handler = matchRoute(stream.request.method, stream.request.path,
                                  stream.request.matches);

        if (handler)
        {
            handler(stream.request, stream.response);
        }
        else if (tryServeStatic(stream.request.path, stream.response))
        {
            // Served from static files
        }
        else
        {
            // 404
            stream.response.status = 404;
            stream.response.set_content("{\"error\":\"Not Found\"}", "application/json");

            // SPA fallback
            if (errorHandler_)
                errorHandler_(stream.request, stream.response);
        }

        submitH2Response(conn, streamId);
    }

    // ─── Submit response via nghttp2 ─────────────────────────────
    void H2Server::submitH2Response(H2Connection &conn, int32_t streamId)
    {
        auto it = conn.streams.find(streamId);
        if (it == conn.streams.end())
            return;
        auto &stream = *it->second;
        if (stream.responseSubmitted)
            return;
        stream.responseSubmitted = true;

        std::string statusStr = std::to_string(stream.response.status);

        // Build header list
        std::vector<nghttp2_nv> nva;
        nva.push_back(makeNV(":status", statusStr));

        // Content-Type
        if (!stream.response.contentType_.empty())
            nva.push_back(makeNV("content-type", stream.response.contentType_));

        // Content-Length for regular responses
        std::string clStr;
        if (!stream.response.hasProvider_)
        {
            clStr = std::to_string(stream.response.body.size());
            nva.push_back(makeNV("content-length", clStr));
        }

        // Default headers (CORS etc.)
        for (const auto &[k, v] : defaultHeaders_)
            nva.push_back(makeNV(k, v));

        // Response-specific headers
        for (const auto &[k, v] : stream.response.headers)
            nva.push_back(makeNV(k, v));

        // Set up data provider
        nghttp2_data_provider dataPrd{};
        dataPrd.source.ptr = nullptr;
        dataPrd.read_callback = H2Connection::onDataSourceRead;

        if (stream.response.hasProvider_)
            stream.streamingActive = true;

        nghttp2_submit_response(conn.session, streamId, nva.data(), nva.size(), &dataPrd);
    }

    // ═══════════════════════════════════════════════════════════════
    //                   HTTP/2 I/O LOOP
    // ═══════════════════════════════════════════════════════════════

    void H2Server::runHttp2(H2Connection &conn)
    {
        // Create nghttp2 server session
        nghttp2_session_callbacks *cbs = nullptr;
        nghttp2_session_callbacks_new(&cbs);
        nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, onBeginHeaders);
        nghttp2_session_callbacks_set_on_header_callback(cbs, onHeader);
        nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, onFrameRecv);
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, onDataChunkRecv);
        nghttp2_session_callbacks_set_on_stream_close_callback(cbs, onStreamClose);
        nghttp2_session_server_new(&conn.session, cbs, &conn);
        nghttp2_session_callbacks_del(cbs);

        // Send server connection preface (SETTINGS)
        nghttp2_settings_entry iv[] = {
            {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 128},
            {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, 1 << 20}, // 1 MB
        };
        nghttp2_submit_settings(conn.session, NGHTTP2_FLAG_NONE, iv, 2);

        // Also bump connection-level window to allow more concurrent data
        nghttp2_submit_window_update(conn.session, NGHTTP2_FLAG_NONE, 0, (1 << 24) - 65535);

        // Flush initial SETTINGS
        {
            const uint8_t *data;
            for (;;)
            {
                ssize_t len = nghttp2_session_mem_send(conn.session, &data);
                if (len <= 0)
                    break;
                if (!sslWriteAll(conn.ssl, data, static_cast<size_t>(len)))
                {
                    conn.alive.store(false);
                    return;
                }
            }
        }

        // Main I/O loop
        uint8_t readBuf[16384];

        while (conn.alive.load() && running_.load())
        {
            bool hasSSLBuffered = SSL_pending(conn.ssl) > 0;
            bool socketReady = false;

            if (!hasSSLBuffered)
            {
                fd_set rfds;
                FD_ZERO(&rfds);
                FD_SET(conn.fd, &rfds);
                timeval tv{0, 16000}; // 16 ms
                int sel = ::select(static_cast<int>(conn.fd) + 1, &rfds, nullptr, nullptr, &tv);
                if (sel < 0)
                    break;
                socketReady = (sel > 0);
            }

            // ── Read incoming data ──
            if (hasSSLBuffered || socketReady)
            {
                int nread = SSL_read(conn.ssl, readBuf, sizeof(readBuf));
                if (nread > 0)
                {
                    ssize_t consumed = nghttp2_session_mem_recv(conn.session, readBuf, static_cast<size_t>(nread));
                    if (consumed < 0)
                    {
                        spdlog::debug("nghttp2_session_mem_recv error: {}", nghttp2_strerror(static_cast<int>(consumed)));
                        break;
                    }
                }
                else
                {
                    int err = SSL_get_error(conn.ssl, nread);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
                        break; // Connection closed or fatal error
                }
            }

            // ── Drive streaming content providers ──
            for (auto &[sid, streamPtr] : conn.streams)
            {
                if (!streamPtr->streamingActive || streamPtr->streamingEnded)
                    continue;

                bool ok = streamPtr->response.provider_(
                    streamPtr->providerOffset, streamPtr->dataSink);

                if (!ok)
                    streamPtr->streamingEnded = true;

                if (streamPtr->dataSink.hasData())
                {
                    streamPtr->providerOffset += streamPtr->dataSink.buffer_.size();
                    nghttp2_session_resume_data(conn.session, sid);
                }
                else if (streamPtr->streamingEnded)
                {
                    nghttp2_session_resume_data(conn.session, sid);
                }
            }

            // ── Flush outgoing data ──
            for (;;)
            {
                const uint8_t *data;
                ssize_t len = nghttp2_session_mem_send(conn.session, &data);
                if (len < 0)
                {
                    conn.alive.store(false);
                    break;
                }
                if (len == 0)
                    break;
                if (!sslWriteAll(conn.ssl, data, static_cast<size_t>(len)))
                {
                    conn.alive.store(false);
                    break;
                }
            }

            // If nghttp2 has no more work, check if we're truly done
            if (!nghttp2_session_want_read(conn.session) &&
                !nghttp2_session_want_write(conn.session))
                break;
        }
    }

    // ═══════════════════════════════════════════════════════════════
    //                   HTTP/1.1 FALLBACK
    // ═══════════════════════════════════════════════════════════════

    void H2Server::runHttp11(H2Connection &conn)
    {
        while (conn.alive.load() && running_.load())
        {
            // Read request line
            std::string requestLine = sslReadLine(conn.ssl);
            if (requestLine.empty())
                break;

            std::istringstream rl(requestLine);
            std::string method, rawPath, version;
            rl >> method >> rawPath >> version;

            // Read headers
            std::map<std::string, std::string> hdrs;
            while (true)
            {
                std::string line = sslReadLine(conn.ssl);
                if (line.empty())
                    break;
                auto colon = line.find(':');
                if (colon != std::string::npos)
                {
                    std::string key = line.substr(0, colon);
                    std::string val = line.substr(colon + 1);
                    // Trim leading space
                    while (!val.empty() && val[0] == ' ')
                        val.erase(val.begin());
                    // Lowercase key for lookup
                    std::string keyLower = key;
                    std::transform(keyLower.begin(), keyLower.end(), keyLower.begin(), ::tolower);
                    hdrs[keyLower] = val;
                    hdrs[key] = val; // Keep original case too
                }
            }

            // Read body
            std::string body;
            auto clIt = hdrs.find("content-length");
            if (clIt != hdrs.end())
            {
                size_t cl = std::stoull(clIt->second);
                if (!sslReadN(conn.ssl, body, cl))
                    break;
            }

            // Build H2Request
            H2Request req;
            req.method = method;
            req.raw_path = rawPath;
            req.headers = hdrs;
            req.body = body;
            {
                std::string qstr;
                splitPath(rawPath, req.path, qstr);
                if (!qstr.empty())
                    parseQueryString(qstr, req.params);
            }

            // Route
            H2Response res;
            auto handler = matchRoute(method, req.path, req.matches);
            if (handler)
                handler(req, res);
            else if (tryServeStatic(req.path, res))
            { /* ok */
            }
            else
            {
                res.status = 404;
                res.set_content("{\"error\":\"Not Found\"}", "application/json");
                if (errorHandler_)
                    errorHandler_(req, res);
            }

            // Apply default headers
            for (const auto &[k, v] : defaultHeaders_)
            {
                if (res.headers.find(k) == res.headers.end())
                    res.headers[k] = v;
            }

            // ── Send response ──
            if (!res.hasProvider_)
            {
                // Regular response
                std::ostringstream oss;
                oss << "HTTP/1.1 " << res.status << " OK\r\n";
                if (!res.contentType_.empty())
                    oss << "Content-Type: " << res.contentType_ << "\r\n";
                oss << "Content-Length: " << res.body.size() << "\r\n";
                for (const auto &[k, v] : res.headers)
                    oss << k << ": " << v << "\r\n";
                oss << "\r\n";
                oss << res.body;
                if (!sslWriteAll(conn.ssl, oss.str()))
                    break;
            }
            else
            {
                // Streaming response (MJPEG etc.)
                std::ostringstream oss;
                oss << "HTTP/1.1 " << res.status << " OK\r\n";
                if (!res.contentType_.empty())
                    oss << "Content-Type: " << res.contentType_ << "\r\n";
                oss << "Connection: keep-alive\r\n";
                for (const auto &[k, v] : res.headers)
                    oss << k << ": " << v << "\r\n";
                oss << "\r\n";
                if (!sslWriteAll(conn.ssl, oss.str()))
                    break;

                // Run content provider in a blocking loop (fine for HTTP/1.1)
                H2DataSink sink;
                size_t offset = 0;
                while (conn.alive.load() && running_.load())
                {
                    bool ok = res.provider_(offset, sink);
                    if (sink.hasData())
                    {
                        if (!sslWriteAll(conn.ssl, sink.buffer_))
                        {
                            conn.alive.store(false);
                            break;
                        }
                        offset += sink.buffer_.size();
                        sink.clear();
                    }
                    if (!ok)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                if (res.providerDone_)
                    res.providerDone_(conn.alive.load());
                break; // End connection after streaming
            }

            // Check Connection header
            auto connHdr = hdrs.find("connection");
            if (connHdr != hdrs.end())
            {
                std::string val = connHdr->second;
                std::transform(val.begin(), val.end(), val.begin(), ::tolower);
                if (val == "close")
                    break;
            }
            // HTTP/1.0 default close, HTTP/1.1 default keep-alive
            if (version == "HTTP/1.0")
                break;
        }
    }

} // namespace sm
