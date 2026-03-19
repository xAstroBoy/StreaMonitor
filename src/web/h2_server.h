#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — HTTP/2 + TLS Server
// Native HTTP/2 multiplexing via nghttp2 + OpenSSL.
// No more 6-connection browser limit. No WebSocket needed.
// Falls back to HTTP/1.1 for curl / non-h2 clients.
// ─────────────────────────────────────────────────────────────────

#include <openssl/ssl.h>

// nghttp2 is a C library — needs ssize_t defined on MSVC
#ifdef _MSC_VER
#   include <BaseTsd.h>
#   ifndef ssize_t
        typedef SSIZE_T ssize_t;
#   endif
#endif

extern "C" {
#include <nghttp2/nghttp2.h>
}

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <regex>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sm_socket_t = SOCKET;
constexpr sm_socket_t SM_INVALID_SOCKET = INVALID_SOCKET;
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
using sm_socket_t = int;
constexpr sm_socket_t SM_INVALID_SOCKET = -1;
inline int closesocket(int fd) { return ::close(fd); }
#endif

namespace sm
{
    // Forward declarations
    struct H2Connection;
    struct H2Stream;

    // ── HTTP Request ──────────────────────────────────────────────
    struct H2Request
    {
        std::string method;
        std::string path;     // Path without query string
        std::string raw_path; // Original :path pseudo-header (includes query)
        std::map<std::string, std::string> headers;
        std::string body;
        std::vector<std::string> matches; // Regex captures (index 0 = full match)
        std::map<std::string, std::string> params;

        std::string get_header_value(const std::string &key) const;
        bool has_header(const std::string &key) const;
    };

    // ── Data Sink for streaming responses ─────────────────────────
    class H2DataSink
    {
    public:
        bool write(const char *data, size_t len);
        bool is_writable() const;

        // Internal — used by the I/O loop & nghttp2 data callback
        std::string buffer_;
        bool writable_ = true;

        size_t readInto(uint8_t *buf, size_t maxLen);
        bool hasData() const { return !buffer_.empty(); }
        void clear() { buffer_.clear(); }
    };

    // ── HTTP Response ─────────────────────────────────────────────
    struct H2Response
    {
        int status = 200;
        std::map<std::string, std::string> headers;
        std::string body;

        void set_content(const std::string &content, const std::string &content_type);
        void set_header(const std::string &key, const std::string &value);

        // Streaming content provider (same contract as cpp-httplib)
        // Provider MUST be non-blocking for HTTP/2. Called at ~60 fps.
        using ContentProvider = std::function<bool(size_t offset, H2DataSink &sink)>;
        using ContentProviderDone = std::function<void(bool success)>;

        void set_content_provider(
            const std::string &content_type,
            ContentProvider provider,
            ContentProviderDone done = nullptr);

        // Internal
        bool hasProvider_ = false;
        ContentProvider provider_;
        ContentProviderDone providerDone_;
        std::string contentType_;
    };

    using RouteHandler = std::function<void(const H2Request &, H2Response &)>;
    using ErrorHandler = std::function<void(const H2Request &, H2Response &)>;

    // ── Per-stream state within a connection ──────────────────────
    struct H2Stream
    {
        int32_t streamId = 0;
        H2Request request;
        H2Response response;
        bool requestComplete = false;
        bool responseSubmitted = false;

        // Regular response body offset
        size_t bodyOffset = 0;

        // Streaming response state
        H2DataSink dataSink;
        bool streamingActive = false;
        bool streamingEnded = false;
        size_t providerOffset = 0;
    };

    // ── Per-connection state ──────────────────────────────────────
    struct H2Connection
    {
        sm_socket_t fd = SM_INVALID_SOCKET;
        SSL *ssl = nullptr;
        nghttp2_session *session = nullptr;
        class H2Server *server = nullptr;
        bool isHttp2 = true;
        std::atomic<bool> alive{true};
        std::map<int32_t, std::unique_ptr<H2Stream>> streams;

        // nghttp2 data provider callback
        static ssize_t onDataSourceRead(
            nghttp2_session *session, int32_t stream_id,
            uint8_t *buf, size_t length, uint32_t *data_flags,
            nghttp2_data_source *source, void *user_data);
    };

    // ── HTTP/2 + TLS Server ───────────────────────────────────────
    class H2Server
    {
    public:
        explicit H2Server(const std::string &certPath, const std::string &keyPath);
        ~H2Server();

        H2Server(const H2Server &) = delete;
        H2Server &operator=(const H2Server &) = delete;

        // Route registration (cpp-httplib–compatible API)
        void Get(const std::string &pattern, RouteHandler handler);
        void Post(const std::string &pattern, RouteHandler handler);
        void Put(const std::string &pattern, RouteHandler handler);
        void Delete(const std::string &pattern, RouteHandler handler);
        void Options(const std::string &pattern, RouteHandler handler);

        // Default headers applied to every response (CORS, etc.)
        void set_default_headers(std::map<std::string, std::string> headers);

        // Static file serving (Next.js export)
        bool set_mount_point(const std::string &prefix, const std::string &dir);

        // Error handler (SPA 404 fallback)
        void set_error_handler(ErrorHandler handler);

        // Lifecycle
        bool listen(const std::string &host, int port);
        void stop();
        bool is_running() const { return running_.load(); }

        friend struct H2Connection;

    private:
        struct Route
        {
            std::string method;
            std::regex pattern;
            std::string rawPattern;
            RouteHandler handler;
        };

        // TLS
        SSL_CTX *sslCtx_ = nullptr;
        std::string certPath_, keyPath_;
        bool initSSL();
        static int alpnSelectCb(SSL *ssl, const unsigned char **out,
                                unsigned char *outlen, const unsigned char *in,
                                unsigned int inlen, void *arg);

        // Networking
        sm_socket_t listenFd_ = SM_INVALID_SOCKET;
        std::atomic<bool> running_{false};
        std::unique_ptr<std::thread> acceptThread_;
        std::mutex connMutex_;
        std::vector<std::unique_ptr<std::thread>> connThreads_;

        void acceptLoop();
        void handleConnection(sm_socket_t clientFd);
        void runHttp2(H2Connection &conn);
        void runHttp11(H2Connection &conn);

        // Routes
        std::vector<Route> routes_;
        std::map<std::string, std::string> defaultHeaders_;
        std::vector<std::pair<std::string, std::string>> mountPoints_;
        ErrorHandler errorHandler_;

        void addRoute(const std::string &method, const std::string &pattern, RouteHandler handler);
        RouteHandler matchRoute(const std::string &method, const std::string &path,
                                std::vector<std::string> &captures) const;
        bool tryServeStatic(const std::string &path, H2Response &res) const;
        static std::string guessMimeType(const std::string &path);

        // nghttp2 callbacks
        static int onBeginHeaders(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
        static int onHeader(nghttp2_session *session, const nghttp2_frame *frame,
                            const uint8_t *name, size_t namelen,
                            const uint8_t *value, size_t valuelen, uint8_t flags, void *user_data);
        static int onFrameRecv(nghttp2_session *session, const nghttp2_frame *frame, void *user_data);
        static int onDataChunkRecv(nghttp2_session *session, uint8_t flags, int32_t stream_id,
                                   const uint8_t *data, size_t len, void *user_data);
        static int onStreamClose(nghttp2_session *session, int32_t stream_id,
                                 uint32_t error_code, void *user_data);

        // Request processing
        void processRequest(H2Connection &conn, int32_t streamId);
        void submitH2Response(H2Connection &conn, int32_t streamId);

        // SSL I/O helpers
        static bool sslWriteAll(SSL *ssl, const uint8_t *data, size_t len);
        static bool sslWriteAll(SSL *ssl, const std::string &data);
        static std::string sslReadLine(SSL *ssl);
        static bool sslReadN(SSL *ssl, std::string &out, size_t n);
    };

} // namespace sm
