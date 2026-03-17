// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Native FFmpeg HLS recorder implementation
//
// This is THE core improvement over the Python version:
// - Direct libavformat API — no subprocess, no orphan processes
// - Per-packet control — detect corruption, fix timestamps
// - Proper cancellation — instant response, no zombie FFmpeg
// - Stall detection at packet level, not stderr parsing
// - Clean resource management with RAII
// - Full transcoding support: x265, x264, NVENC (CUDA)
// ─────────────────────────────────────────────────────────────────

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/mathematics.h>
#include <libavutil/timestamp.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#include "downloaders/hls_recorder.h"
#include "net/m3u8_parser.h"
#include "net/mouflon.h"
#include <spdlog/spdlog.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <regex>

namespace sm
{

    // ─────────────────────────────────────────────────────────────────
    // FFmpeg error to string
    // ─────────────────────────────────────────────────────────────────
    static std::string ffError(int errnum)
    {
        char buf[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(errnum, buf, sizeof(buf));
        return std::string(buf);
    }

    // Configure proxy on an HttpClient from AppConfig (uses first proxy if pool)
    static void applyProxyConfig(HttpClient &http, const AppConfig &config)
    {
        if (config.proxyEnabled && !config.proxies.empty())
        {
            // Use first available proxy
            const auto &proxy = config.proxies[0];
            http.setProxy(proxy);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Segment feeder — download HLS segments directly into memory
    //
    // Instead of the old approach (write playlist to temp file, let
    // FFmpeg's HLS demuxer re-read it), we:
    //   1. Poll the remote m3u8 periodically
    //   2. Parse it with M3U8Parser to extract segment URLs
    //   3. Download each NEW segment via HttpClient
    //   4. Push raw bytes into a ring buffer
    //   5. FFmpeg reads from a custom AVIOContext backed by the buffer
    //
    // FFmpeg sees a continuous fMP4 or MPEG-TS byte stream.
    // No HLS demuxer, no temp files, no file-locking races.
    // ─────────────────────────────────────────────────────────────────

    // Detect CPA/mouflon advert playlist.  When mouflon keys expire the
    // CDN falls back to a static ad playlist.
    static bool isCpaAdvert(const std::string &body)
    {
        if (body.find("#EXT-X-MOUFLON-ADVERT") != std::string::npos)
            return true;
        if (body.find("/cpa/v2/") != std::string::npos ||
            body.find("/cpa/v1/") != std::string::npos)
            return true;
        return false;
    }

    // Rewrite media-hls gateway URL → direct b-hls edge
    // media-hls.doppiocdn.XXX/b-hls-NN/path → b-hls-NN.doppiocdn.live/hls/path
    static std::string rewriteMediaHlsUrl(const std::string &url)
    {
        static const std::regex re(
            R"(https://media-hls\.doppiocdn\.\w+/(b-hls-\d+)/([^?]+))");
        std::smatch m;
        if (std::regex_search(url, m, re))
        {
            auto qpos = url.find('?');
            std::string qs = (qpos != std::string::npos) ? url.substr(qpos) : "";
            return "https://" + m[1].str() + ".doppiocdn.live/hls/" + m[2].str() + qs;
        }
        return url;
    }

    // Resolve + fix a segment URL from the parsed playlist
    static std::string fixSegmentUrl(const std::string &baseUrl,
                                     const std::string &rawUri)
    {
        auto url = M3U8Parser::resolveUrl(baseUrl, rawUri);
        url = M3U8Parser::inheritQueryParams(baseUrl, url);
        url = rewriteMediaHlsUrl(url);
        return url;
    }

    // ── SegmentFeeder: push bytes into ring buffer ──────────────────
    void HLSRecorder::SegmentFeeder::feedBytes(const uint8_t *data, size_t len)
    {
        if (len == 0)
            return;
        {
            std::lock_guard lock(bufMutex);
            ringBuf.insert(ringBuf.end(), data, data + len);
        }
        bufCv.notify_one();
        lastFeedEpochNs.store(
            std::chrono::steady_clock::now().time_since_epoch().count(),
            std::memory_order_relaxed);
    }

    void HLSRecorder::SegmentFeeder::feedBytes(const std::string &data)
    {
        feedBytes(reinterpret_cast<const uint8_t *>(data.data()), data.size());
    }

    // ── SegmentFeeder: AVIO read callback ───────────────────────────
    // Called by FFmpeg when it wants to read bytes.  Blocks until data
    // is available, the feeder finishes, or recording is cancelled.
    int HLSRecorder::SegmentFeeder::readCallback(void *opaque, uint8_t *buf,
                                                 int bufSize)
    {
        auto *self = static_cast<SegmentFeeder *>(opaque);
        std::unique_lock lock(self->bufMutex);

        // Wait for data (with 100ms polling for cancel check)
        while (self->ringBuf.empty() && !self->finished &&
               !self->hasError &&
               !(self->cancel && self->cancel->isCancelled()))
        {
            self->bufCv.wait_for(lock, std::chrono::milliseconds(100));
        }

        // Check terminal conditions
        if (self->cancel && self->cancel->isCancelled())
            return AVERROR_EXIT;
        if (self->hasError)
            return AVERROR(EIO);
        if (self->ringBuf.empty() && self->finished)
            return AVERROR_EOF;

        // Copy available bytes
        int n = static_cast<int>(
            std::min(static_cast<size_t>(bufSize), self->ringBuf.size()));
        std::copy(self->ringBuf.begin(), self->ringBuf.begin() + n, buf);
        self->ringBuf.erase(self->ringBuf.begin(), self->ringBuf.begin() + n);
        return n;
    }

    // ── SegmentFeeder: create AVIOContext ────────────────────────────
    AVIOContext *HLSRecorder::SegmentFeeder::createAVIO()
    {
        avioBuf = static_cast<uint8_t *>(av_malloc(AVIO_BUF_SIZE));
        if (!avioBuf)
            return nullptr;

        avioCtx = avio_alloc_context(
            avioBuf,
            AVIO_BUF_SIZE,
            0,            // write_flag = 0 (read-only)
            this,         // opaque
            readCallback, // read_packet
            nullptr,      // write_packet
            nullptr);     // seek

        if (!avioCtx)
        {
            av_free(avioBuf);
            avioBuf = nullptr;
            return nullptr;
        }

        // Not seekable — continuous live stream
        avioCtx->seekable = 0;
        return avioCtx;
    }

    // ── SegmentFeeder: start ────────────────────────────────────────
    bool HLSRecorder::SegmentFeeder::start(const std::string &playlistUrl,
                                           const std::string &userAgent,
                                           CancellationToken &cancelToken,
                                           PlaylistDecoder decoder)
    {
        cancel = &cancelToken;

        // ── Initial fetch + validation ──────────────────────────────
        HttpClient http;
        http.setDefaultUserAgent(userAgent);
        if (config)
            applyProxyConfig(http, *config);

        HttpResponse resp;
        for (int attempt = 1; attempt <= 3; attempt++)
        {
            resp = http.get(playlistUrl, 15);
            if (resp.ok())
                break;
            log->warn("SegmentFeeder: initial fetch {}/3 failed (HTTP {})",
                      attempt, resp.statusCode);
            if (attempt < 3)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!resp.ok())
        {
            log->error("SegmentFeeder: all fetch attempts failed");
            return false;
        }

        // Decode (mouflon) if needed
        auto content = resp.body;
        if (decoder)
            content = decoder(content);

        // Reject CPA advert
        if (isCpaAdvert(resp.body) || isCpaAdvert(content))
        {
            log->warn("SegmentFeeder: CPA advert detected — keys expired?");
            return false;
        }

        // Parse the media playlist
        auto playlist = M3U8Parser::parseMedia(content, playlistUrl);
        if (playlist.segments.empty())
        {
            log->error("SegmentFeeder: no segments in playlist");
            return false;
        }

        // Download init segment (EXT-X-MAP) first — required for fMP4
        std::string initUrl;
        for (const auto &seg : playlist.segments)
        {
            if (seg.isMap)
            {
                initUrl = fixSegmentUrl(playlistUrl, seg.uri);
                break;
            }
        }
        if (!initUrl.empty())
        {
            auto initResp = http.get(initUrl, 15);
            if (!initResp.ok() || initResp.body.empty())
            {
                log->error("SegmentFeeder: failed to download init segment (HTTP {})",
                           initResp.statusCode);
                return false;
            }
            feedBytes(initResp.body);
            log->debug("SegmentFeeder: init segment downloaded ({} bytes)",
                       initResp.body.size());
        }

        // Download the last few media segments to seed FFmpeg's probing
        int64_t lastSeq = -1;
        {
            // Only download the last 2-3 segments (not all of them)
            std::vector<const HLSSegment *> mediaSegs;
            for (const auto &seg : playlist.segments)
                if (!seg.isMap)
                    mediaSegs.push_back(&seg);

            int startIdx = std::max(0, static_cast<int>(mediaSegs.size()) - 3);
            for (int i = startIdx; i < static_cast<int>(mediaSegs.size()); i++)
            {
                auto url = fixSegmentUrl(playlistUrl, mediaSegs[i]->uri);
                auto segResp = http.get(url, 15);
                if (segResp.ok() && !segResp.body.empty())
                {
                    feedBytes(segResp.body);
                    lastSeq = mediaSegs[i]->sequenceNumber;
                    log->debug("SegmentFeeder: seed segment seq={} ({} bytes)",
                               lastSeq, segResp.body.size());
                }
            }
        }

        if (lastSeq < 0)
        {
            log->error("SegmentFeeder: no segments could be downloaded");
            return false;
        }

        running = true;
        log->info("SegmentFeeder: started (lastSeq={})", lastSeq);

        // ── Background thread: poll → parse → download → feed ───────
        thread = std::thread([this, playlistUrl, userAgent, decoder,
                              lastSeqInit = lastSeq]()
                             {
            HttpClient threadHttp;
            threadHttp.setDefaultUserAgent(userAgent);
            if (config)
                applyProxyConfig(threadHttp, *config);

            int64_t lastSeq = lastSeqInit;
            std::string lastInitUrl;
            int consecutiveErrors = 0;
            int cpaCount = 0;

            while (running.load() && !cancel->isCancelled())
            {
                // Poll interval: ~1 second (10 × 100ms with cancel checks)
                for (int i = 0; i < 10 && running.load() &&
                                !cancel->isCancelled(); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));

                if (!running.load() || cancel->isCancelled())
                    break;

                try
                {
                    auto resp = threadHttp.get(playlistUrl, 10);
                    if (!resp.ok())
                    {
                        consecutiveErrors++;
                        if (resp.isNotFound())
                        {
                            // Stream went offline
                            log->info("SegmentFeeder: stream offline (404)");
                            break;
                        }
                        if (consecutiveErrors >= 30)
                        {
                            log->error("SegmentFeeder: {} consecutive errors, giving up",
                                       consecutiveErrors);
                            break;
                        }
                        continue;
                    }
                    consecutiveErrors = 0;

                    // CPA advert detection
                    if (isCpaAdvert(resp.body))
                    {
                        cpaCount++;
                        if (cpaCount == 1 || cpaCount % 20 == 0)
                            log->warn("SegmentFeeder: CPA advert (keys expired?), "
                                      "skipping (count={})", cpaCount);
                        continue;
                    }
                    cpaCount = 0;

                    // Decode
                    auto content = resp.body;
                    if (decoder)
                        content = decoder(content);
                    if (isCpaAdvert(content))
                        continue;

                    // Parse
                    auto playlist = M3U8Parser::parseMedia(content, playlistUrl);

                    // Check for new init segment (rare — format change)
                    for (const auto &seg : playlist.segments)
                    {
                        if (seg.isMap)
                        {
                            auto url = fixSegmentUrl(playlistUrl, seg.uri);
                            if (url != lastInitUrl)
                            {
                                auto initResp = threadHttp.get(url, 10);
                                if (initResp.ok() && !initResp.body.empty())
                                {
                                    feedBytes(initResp.body);
                                    lastInitUrl = url;
                                    log->debug("SegmentFeeder: new init segment "
                                               "({} bytes)", initResp.body.size());
                                }
                            }
                            break;
                        }
                    }

                    // Download NEW media segments (sequence > lastSeq)
                    for (const auto &seg : playlist.segments)
                    {
                        if (seg.isMap)
                            continue;
                        if (seg.sequenceNumber <= lastSeq)
                            continue;
                        if (cancel->isCancelled() || !running.load())
                            break;

                        // Skip CPA segment URLs
                        if (seg.uri.find("/cpa/") != std::string::npos)
                            continue;
                        // Skip mouflon placeholder
                        if (seg.uri == "media.mp4")
                            continue;

                        auto url = fixSegmentUrl(playlistUrl, seg.uri);
                        auto segResp = threadHttp.get(url, 15);
                        if (segResp.ok() && !segResp.body.empty())
                        {
                            feedBytes(segResp.body);
                            lastSeq = seg.sequenceNumber;
                        }
                        else
                        {
                            log->debug("SegmentFeeder: segment seq={} download "
                                       "failed (HTTP {}), skipping",
                                       seg.sequenceNumber, segResp.statusCode);
                            // Skip this segment but update lastSeq to avoid
                            // re-trying it on the next poll
                            lastSeq = seg.sequenceNumber;
                        }
                    }
                }
                catch (const std::exception &ex)
                {
                    log->error("SegmentFeeder exception: {}", ex.what());
                }
                catch (...)
                {
                    log->error("SegmentFeeder unknown exception");
                }
            }

            // Signal completion to the AVIO reader
            {
                std::lock_guard lock(bufMutex);
                finished = true;
            }
            bufCv.notify_one();
            log->debug("SegmentFeeder: thread exiting (lastSeq={})", lastSeq); });

        return true;
    }

    // ── SegmentFeeder: stop ─────────────────────────────────────────
    void HLSRecorder::SegmentFeeder::stop()
    {
        running = false;
        {
            std::lock_guard lock(bufMutex);
            finished = true;
        }
        bufCv.notify_one();

        if (thread.joinable())
            thread.join();

        // Clean up AVIO
        if (avioCtx)
        {
            // NOTE: Do NOT av_free(avioBuf) — avio_context_free handles it
            avio_context_free(&avioCtx);
            avioCtx = nullptr;
            avioBuf = nullptr;
        }

        log->debug("SegmentFeeder: stopped");
    }

    // ─────────────────────────────────────────────────────────────────
    // Interrupt callback — allows cancellation from CancellationToken
    // ─────────────────────────────────────────────────────────────────
    struct InterruptData
    {
        CancellationToken *cancel = nullptr;
        TimePoint deadline;
        bool hasDeadline = false;
    };

    int HLSRecorder::interruptCallback(void *opaque)
    {
        auto *data = static_cast<InterruptData *>(opaque);
        if (!data)
            return 0;

        // Check cancellation
        if (data->cancel && data->cancel->isCancelled())
            return 1;

        // Check timeout deadline
        if (data->hasDeadline && Clock::now() > data->deadline)
            return 1;

        return 0;
    }

    // ─────────────────────────────────────────────────────────────────
    // StallDetector
    // ─────────────────────────────────────────────────────────────────
    void HLSRecorder::StallDetector::reset()
    {
        lastPacketTime = Clock::now();
        lastPtsAdvance = Clock::now();
        lastPts = 0;
        consecutiveStalls = 0;
        avgSpeed = 1.0;
        isStalled = false;
    }

    void HLSRecorder::StallDetector::onPacket(int64_t pts, int64_t /*fileSize*/)
    {
        auto now = Clock::now();
        lastPacketTime = now;

        if (pts > lastPts)
        {
            lastPtsAdvance = now;
            lastPts = pts;
            isStalled = false;
            consecutiveStalls = 0;
        }
    }

    bool HLSRecorder::StallDetector::checkStall(const AppConfig::FFmpegTuning &tuning) const
    {
        auto now = Clock::now();

        // No packets received at all
        auto sinceLast = std::chrono::duration_cast<std::chrono::seconds>(
                             now - lastPacketTime)
                             .count();
        if (sinceLast > tuning.fallbackNoOutputSec)
            return true;

        // PTS not advancing (video frozen)
        auto sincePtsAdvance = std::chrono::duration_cast<std::chrono::seconds>(
                                   now - lastPtsAdvance)
                                   .count();
        if (sincePtsAdvance > tuning.stallSameTimeSec)
            return true;

        return false;
    }

    // ─────────────────────────────────────────────────────────────────
    // Constructor / Destructor
    // ─────────────────────────────────────────────────────────────────
    HLSRecorder::HLSRecorder(const AppConfig &config)
        : config_(config)
    {
    }

    HLSRecorder::~HLSRecorder() = default;

    void HLSRecorder::setProgressCallback(ProgressCallback cb)
    {
        progressCb_ = std::move(cb);
    }

    RecordingStats HLSRecorder::getStats() const
    {
        std::lock_guard lock(statsMutex_);
        return stats_;
    }

    // ─────────────────────────────────────────────────────────────────
    // Open input (HLS stream)
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::openInput(FFmpegState &state, const std::string &url,
                                const std::string &userAgent, const std::string &cookies,
                                const std::map<std::string, std::string> &headers,
                                AVIOContext *customAVIO)
    {
        state.inputCtx = avformat_alloc_context();
        if (!state.inputCtx)
        {
            log_->error("Failed to allocate input context");
            return false;
        }

        // Set up interrupt callback for cancellation
        auto *interruptData = new InterruptData();
        state.inputCtx->interrupt_callback.callback = interruptCallback;
        state.inputCtx->interrupt_callback.opaque = interruptData;

        // Build options dict
        AVDictionary *opts = nullptr;
        int ret;

        if (customAVIO)
        {
            // ── AVIO mode: SegmentFeeder provides raw bytes ─────────
            // The feeder downloads segments and pushes them into a ring
            // buffer.  FFmpeg reads from the custom AVIOContext.
            // No URL, no HLS demuxer — just a continuous byte stream.
            state.inputCtx->pb = customAVIO;

            // Probing
            av_dict_set(&opts, "probesize", config_.ffmpeg.probeSize.c_str(), 0);
            av_dict_set(&opts, "analyzeduration", config_.ffmpeg.analyzeDuration.c_str(), 0);

            // Flags — igndts+genpts for clean timestamps
            av_dict_set(&opts, "fflags", "igndts+genpts+discardcorrupt", 0);

            log_->debug("openInput: AVIO mode (SegmentFeeder)");
            ret = avformat_open_input(&state.inputCtx, nullptr, nullptr, &opts);

            // Log unconsumed options
            {
                AVDictionaryEntry *t = nullptr;
                while ((t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)))
                    log_->debug("Unconsumed FFmpeg option: {}={}", t->key, t->value);
            }
            av_dict_free(&opts);
        }
        else
        {

            // Detect if input is a local file (rolling playlist temp file)
            bool isLocalFile = !url.empty() && (url[0] == '/' || url[0] == '\\' ||
                                                (url.size() > 2 && url[1] == ':'));

            // User agent (needed for segment fetches even with local playlist)
            std::string ua = userAgent.empty() ? config_.userAgent : userAgent;
            av_dict_set(&opts, "user_agent", ua.c_str(), 0);

            // Cookies
            if (!cookies.empty())
                av_dict_set(&opts, "cookies", cookies.c_str(), 0);

            // Custom headers
            if (!headers.empty())
            {
                std::string headerStr;
                for (const auto &[k, v] : headers)
                    headerStr += k + ": " + v + "\r\n";
                av_dict_set(&opts, "headers", headerStr.c_str(), 0);
            }

            if (isLocalFile)
            {
                // ── Local playlist (rolling refresh) ────────────────────
                // When reading from a local .m3u8 that references remote segments,
                // we need protocol_whitelist to allow both file and HTTP access.
                av_dict_set(&opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto", 0);

                // NOTE: Do NOT pass HTTP protocol options (-reconnect*, -timeout,
                // -rw_timeout) here.  When the primary input is a local file, these
                // are matched against the 'file' protocol which ignores them.
                // Segment fetches use URL query-param auth (pkey/pdkey), so HTTP
                // headers aren't needed.  Python had the same approach.

                // HLS demuxer options — virtually infinite retry values so FFmpeg
                // almost never reports EOF to our code.  The refresher writes new
                // playlists every 1s; with 99999 hold counters × ~2s target duration,
                // FFmpeg retries internally for ~55 hours before giving up.
                // Real stream endings are detected by the refresher (HTTP 404/gone)
                // which stops writing → our stale check detects it → clean exit.
                // NOTE: seg_max_retry is moderate — too low and a transient
                // segment 404 (normal CDN rotation) triggers EOF immediately;
                // too high and FFmpeg wastes time retrying a truly dead segment.
                std::string liveStart = std::to_string(-config_.ffmpeg.liveLastSegments);
                av_dict_set(&opts, "live_start_index", liveStart.c_str(), 0);
                av_dict_set(&opts, "max_reload", "99999", 0);
                av_dict_set(&opts, "seg_max_retry", "50", 0);
                av_dict_set(&opts, "m3u8_hold_counters", "99999", 0);
                av_dict_set(&opts, "allow_reload", "1", 0);

                // Probing
                av_dict_set(&opts, "probesize", config_.ffmpeg.probeSize.c_str(), 0);
                av_dict_set(&opts, "analyzeduration", config_.ffmpeg.analyzeDuration.c_str(), 0);

                // Flags — nobuffer+igndts+genpts+discardcorrupt (matches Python)
                // nobuffer: read fast, don't wait.  Safe because max_reload=99999
                //   gives FFmpeg virtually infinite retries before EOF.
                // igndts:   ignore broken DTS from long-running live streams
                // genpts:   regenerate PTS for clean output
                av_dict_set(&opts, "fflags", "nobuffer+igndts+genpts+discardcorrupt", 0);
            }
            else
            {
                // ── Remote URL (direct HLS) ─────────────────────────────
                // Timeouts (in microseconds)
                int64_t timeoutUs = config_.ffmpeg.rwTimeoutSec * 1000000LL;
                std::string timeoutStr = std::to_string(timeoutUs);
                av_dict_set(&opts, "timeout", timeoutStr.c_str(), 0);
                av_dict_set(&opts, "rw_timeout", timeoutStr.c_str(), 0);

                // HLS-specific options — generous retry values
                std::string liveStart = std::to_string(-config_.ffmpeg.liveLastSegments);
                av_dict_set(&opts, "live_start_index", liveStart.c_str(), 0);
                av_dict_set(&opts, "max_reload", "99999", 0);
                av_dict_set(&opts, "seg_max_retry", "180", 0);
                av_dict_set(&opts, "m3u8_hold_counters", "99999", 0);
                av_dict_set(&opts, "allow_reload", "1", 0);

                // HTTP reconnection (valid for remote HTTP protocol)
                av_dict_set(&opts, "reconnect", "1", 0);
                av_dict_set(&opts, "reconnect_streamed", "1", 0);
                av_dict_set(&opts, "reconnect_on_network_error", "1", 0);
                av_dict_set(&opts, "reconnect_on_http_error", "4xx,5xx", 0);
                av_dict_set(&opts, "reconnect_at_eof", "1", 0);
                av_dict_set(&opts, "reconnect_delay_max",
                            std::to_string(config_.ffmpeg.reconnectDelayMax).c_str(), 0);

                // Probing
                av_dict_set(&opts, "probesize", config_.ffmpeg.probeSize.c_str(), 0);
                av_dict_set(&opts, "analyzeduration", config_.ffmpeg.analyzeDuration.c_str(), 0);

                // Flags — match Python: nobuffer+igndts+genpts+discardcorrupt
                av_dict_set(&opts, "fflags", "nobuffer+igndts+genpts+discardcorrupt", 0);
            }

            // Open input (force HLS format for .m3u8 inputs)
            const AVInputFormat *hlsFmt = nullptr;
            if (url.find(".m3u8") != std::string::npos || url.find(".m3u") != std::string::npos)
                hlsFmt = av_find_input_format("hls");

            ret = avformat_open_input(&state.inputCtx, url.c_str(), hlsFmt, &opts);

            // Log any unconsumed options (helps debug option routing issues)
            {
                AVDictionaryEntry *t = nullptr;
                while ((t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)))
                    log_->debug("Unconsumed FFmpeg option: {}={}", t->key, t->value);
            }
            av_dict_free(&opts);

        } // end of else (URL/file mode)

        if (ret < 0)
        {
            log_->error("Failed to open input: {} - {}",
                        customAVIO ? "(AVIO)" : url, ffError(ret));
            // inputCtx is freed by avformat_open_input on failure
            state.inputCtx = nullptr;
            delete interruptData;
            return false;
        }

        // Find stream info
        ret = avformat_find_stream_info(state.inputCtx, nullptr);
        if (ret < 0)
        {
            log_->warn("Failed to find stream info: {}", ffError(ret));
            // Continue anyway — HLS might work without full probe
        }

        // Find best video and audio streams
        state.videoIdx = av_find_best_stream(state.inputCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        state.audioIdx = av_find_best_stream(state.inputCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        if (state.videoIdx < 0 && state.audioIdx < 0)
        {
            log_->error("No video or audio streams found in {}",
                        customAVIO ? "(AVIO)" : url);
            // Clean up — caller won't call closeInput() on failure
            if (state.inputCtx->interrupt_callback.opaque)
            {
                delete static_cast<InterruptData *>(state.inputCtx->interrupt_callback.opaque);
                state.inputCtx->interrupt_callback.opaque = nullptr;
            }
            avformat_close_input(&state.inputCtx);
            return false;
        }

        log_->info("Opened input: {} video={}, audio={}, streams={}",
                   customAVIO ? "AVIO" : "HLS",
                   state.videoIdx, state.audioIdx, state.inputCtx->nb_streams);

        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // CUDA / NVENC availability detection
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::isNvencAvailable()
    {
        // Check if the hevc_nvenc encoder exists in this FFmpeg build
        const AVCodec *nvenc = avcodec_find_encoder_by_name("hevc_nvenc");
        if (!nvenc)
        {
            spdlog::debug("NVENC: hevc_nvenc encoder not found in FFmpeg build");
            return false;
        }

        // Try to create a CUDA hardware device to verify GPU is accessible
        AVBufferRef *hwCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret < 0)
        {
            spdlog::debug("NVENC: CUDA device not available: {}", ffError(ret));
            return false;
        }
        av_buffer_unref(&hwCtx);
        spdlog::info("NVENC: CUDA GPU detected and available");
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Initialize CUDA hardware device context
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::initCudaDevice(FFmpegState &state)
    {
        int ret = av_hwdevice_ctx_create(&state.hwDeviceCtx, AV_HWDEVICE_TYPE_CUDA,
                                         nullptr, nullptr, 0);
        if (ret < 0)
        {
            log_->error("Failed to create CUDA device: {}", ffError(ret));
            return false;
        }
        log_->info("CUDA hardware device initialized");
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Setup video decoder for transcoding
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::setupVideoDecoder(FFmpegState &state)
    {
        if (state.videoIdx < 0)
            return false;

        AVStream *inStream = state.inputCtx->streams[state.videoIdx];
        const AVCodec *decoder = avcodec_find_decoder(inStream->codecpar->codec_id);
        if (!decoder)
        {
            log_->error("Decoder not found for codec ID {}", (int)inStream->codecpar->codec_id);
            return false;
        }

        state.videoDecCtx = avcodec_alloc_context3(decoder);
        if (!state.videoDecCtx)
        {
            log_->error("Failed to allocate decoder context");
            return false;
        }

        int ret = avcodec_parameters_to_context(state.videoDecCtx, inStream->codecpar);
        if (ret < 0)
        {
            log_->error("Failed to copy decoder params: {}", ffError(ret));
            return false;
        }

        state.videoDecCtx->time_base = inStream->time_base;
        state.videoDecCtx->framerate = av_guess_frame_rate(state.inputCtx, inStream, nullptr);

        // Use multiple threads for decoding
        state.videoDecCtx->thread_count = 0; // auto
        state.videoDecCtx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

        ret = avcodec_open2(state.videoDecCtx, decoder, nullptr);
        if (ret < 0)
        {
            log_->error("Failed to open video decoder: {}", ffError(ret));
            return false;
        }

        // Allocate reusable decode frame
        state.decFrame = av_frame_alloc();
        if (!state.decFrame)
        {
            log_->error("Failed to allocate decode frame");
            return false;
        }

        log_->info("Video decoder ready: {} ({}x{})",
                   decoder->name, state.videoDecCtx->width, state.videoDecCtx->height);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Setup video encoder for transcoding
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::setupVideoEncoder(FFmpegState &state, const EncodingConfig &encCfg)
    {
        const char *encoderName = encCfg.ffmpegEncoderName();
        if (!encoderName)
            return false; // stream copy mode

        // For NVENC, verify CUDA is available and fall back if not
        if (encCfg.usesHardware())
        {
            if (!encCfg.enableCuda)
            {
                log_->info("CUDA encoding disabled in config, falling back to software");
                return false; // caller will retry with software encoder
            }
            if (!isNvencAvailable())
            {
                log_->warn("NVENC requested but not available, falling back to software");
                return false;
            }
            if (!initCudaDevice(state))
            {
                log_->warn("Failed to init CUDA device, falling back to software");
                return false;
            }
        }

        const AVCodec *encoder = avcodec_find_encoder_by_name(encoderName);
        if (!encoder)
        {
            log_->error("Encoder '{}' not found in FFmpeg build", encoderName);
            return false;
        }

        state.videoEncCtx = avcodec_alloc_context3(encoder);
        if (!state.videoEncCtx)
        {
            log_->error("Failed to allocate encoder context");
            return false;
        }

        // Copy dimensions from decoder (or apply scaling)
        int srcWidth = state.videoDecCtx->width;
        int srcHeight = state.videoDecCtx->height;
        int dstWidth = srcWidth;
        int dstHeight = srcHeight;

        if (encCfg.maxWidth > 0 && dstWidth > encCfg.maxWidth)
        {
            double ratio = static_cast<double>(encCfg.maxWidth) / dstWidth;
            dstWidth = encCfg.maxWidth;
            dstHeight = static_cast<int>(dstHeight * ratio) & ~1; // ensure even
        }
        if (encCfg.maxHeight > 0 && dstHeight > encCfg.maxHeight)
        {
            double ratio = static_cast<double>(encCfg.maxHeight) / dstHeight;
            dstHeight = encCfg.maxHeight;
            dstWidth = static_cast<int>(dstWidth * ratio) & ~1;
        }

        state.videoEncCtx->width = dstWidth;
        state.videoEncCtx->height = dstHeight;
        state.videoEncCtx->sample_aspect_ratio = state.videoDecCtx->sample_aspect_ratio;

        // Pixel format
        if (encCfg.usesHardware())
        {
            // NVENC prefers NV12
            state.videoEncCtx->pix_fmt = AV_PIX_FMT_NV12;
        }
        else
        {
            // Software x265/x264 prefer YUV420P
            state.videoEncCtx->pix_fmt = AV_PIX_FMT_YUV420P;
        }

        // Time base from input stream
        AVStream *inStream = state.inputCtx->streams[state.videoIdx];
        state.videoEncCtx->time_base = inStream->time_base;
        state.videoEncCtx->framerate = state.videoDecCtx->framerate;

        // Rate control
        if (encCfg.usesHardware())
        {
            // NVENC: use CQ (constant quality) mode
            av_opt_set(state.videoEncCtx->priv_data, "rc", "constqp", 0);
            av_opt_set_int(state.videoEncCtx->priv_data, "qp", encCfg.crf, 0);
            av_opt_set(state.videoEncCtx->priv_data, "preset", "p4", 0); // balanced
            av_opt_set(state.videoEncCtx->priv_data, "tune", "hq", 0);

            // Attach hardware device
            if (state.hwDeviceCtx)
                state.videoEncCtx->hw_device_ctx = av_buffer_ref(state.hwDeviceCtx);
        }
        else
        {
            // Software: CRF mode
            av_opt_set_int(state.videoEncCtx->priv_data, "crf", encCfg.crf, 0);
            av_opt_set(state.videoEncCtx->priv_data, "preset", encCfg.preset.c_str(), 0);

            // x265-specific options
            if (encCfg.encoder == EncoderType::X265)
            {
                // Optimize for live-ish encoding (low latency)
                av_opt_set(state.videoEncCtx->priv_data, "x265-params",
                           "log-level=warning:no-info=1", 0);
            }
        }

        // Threading
        state.videoEncCtx->thread_count = encCfg.threads > 0 ? encCfg.threads : 0;

        // GOP / keyframe interval (every 2 seconds)
        if (state.videoEncCtx->framerate.num > 0)
        {
            state.videoEncCtx->gop_size = state.videoEncCtx->framerate.num * 2 /
                                          std::max(1, state.videoEncCtx->framerate.den);
        }
        else
        {
            state.videoEncCtx->gop_size = 60; // fallback
        }
        state.videoEncCtx->max_b_frames = 2;

        // Flags for certain containers
        if (state.outputCtx && state.outputCtx->oformat &&
            (state.outputCtx->oformat->flags & AVFMT_GLOBALHEADER))
        {
            state.videoEncCtx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }

        int ret = avcodec_open2(state.videoEncCtx, encoder, nullptr);
        if (ret < 0)
        {
            log_->error("Failed to open encoder '{}': {}", encoderName, ffError(ret));
            avcodec_free_context(&state.videoEncCtx);
            return false;
        }

        // Set up pixel format converter if needed
        AVPixelFormat srcFmt = state.videoDecCtx->pix_fmt;
        AVPixelFormat dstFmt = state.videoEncCtx->pix_fmt;
        if (srcFmt != dstFmt || srcWidth != dstWidth || srcHeight != dstHeight)
        {
            state.swsCtx = sws_getContext(
                srcWidth, srcHeight, srcFmt,
                dstWidth, dstHeight, dstFmt,
                SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!state.swsCtx)
            {
                log_->error("Failed to create pixel format converter");
                return false;
            }

            // Allocate encoder input frame
            state.encFrame = av_frame_alloc();
            state.encFrame->format = dstFmt;
            state.encFrame->width = dstWidth;
            state.encFrame->height = dstHeight;
            ret = av_frame_get_buffer(state.encFrame, 0);
            if (ret < 0)
            {
                log_->error("Failed to allocate encoder frame buffer: {}", ffError(ret));
                return false;
            }
        }

        log_->info("Video encoder ready: {} ({}x{}, crf={}, preset={})",
                   encoderName, dstWidth, dstHeight, encCfg.crf, encCfg.preset);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Open output (container file) — with transcoding support
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::openOutput(FFmpegState &state, const std::string &outputPath,
                                 const VRConfig &vrConfig)
    {
        const auto &fmt = getFormatInfo(config_.container);
        const auto &encCfg = config_.encoding;

        // Create output context
        int ret = avformat_alloc_output_context2(&state.outputCtx, nullptr,
                                                 fmt.ffFormat, outputPath.c_str());
        if (ret < 0 || !state.outputCtx)
        {
            log_->error("Failed to create output context: {}", ffError(ret));
            return false;
        }

        // ── Determine encoding mode ─────────────────────────────────
        // Try transcoding if configured, fall back to stream copy on failure
        state.transcoding = false;
        bool encoderReady = false;

        if (encCfg.needsTranscoding() && state.videoIdx >= 0)
        {
            log_->info("Setting up transcoding: encoder={}, cuda={}",
                       encoderTypeToString(encCfg.encoder), encCfg.enableCuda);

            // Step 1: Set up video decoder
            if (setupVideoDecoder(state))
            {
                // Step 2: Try configured encoder
                EncodingConfig activeCfg = encCfg;

                // If CUDA requested, try NVENC first
                if (activeCfg.enableCuda && !activeCfg.usesHardware())
                {
                    // User wants CUDA but picked a software encoder — override to NVENC
                    EncodingConfig nvencCfg = activeCfg;
                    nvencCfg.encoder = (activeCfg.encoder == EncoderType::X264)
                                           ? EncoderType::NVENC_H264
                                           : EncoderType::NVENC_HEVC;
                    if (setupVideoEncoder(state, nvencCfg))
                    {
                        encoderReady = true;
                        log_->info("Using NVENC hardware encoder (CUDA)");
                    }
                }

                // If NVENC not used/failed, try the configured software encoder
                if (!encoderReady)
                {
                    // Make sure we use software encoder variant
                    if (activeCfg.usesHardware() && !activeCfg.enableCuda)
                    {
                        // Hardware requested but CUDA disabled — switch to software
                        activeCfg.encoder = (activeCfg.encoder == EncoderType::NVENC_H264)
                                                ? EncoderType::X264
                                                : EncoderType::X265;
                    }
                    if (setupVideoEncoder(state, activeCfg))
                    {
                        encoderReady = true;
                        log_->info("Using software encoder: {}", activeCfg.ffmpegEncoderName());
                    }
                }

                // Last resort: try the other software encoder
                if (!encoderReady)
                {
                    EncoderType fallback = (activeCfg.encoder == EncoderType::X264 ||
                                            activeCfg.encoder == EncoderType::NVENC_H264)
                                               ? EncoderType::X265
                                               : EncoderType::X264;
                    EncodingConfig fallbackCfg = activeCfg;
                    fallbackCfg.encoder = fallback;
                    if (setupVideoEncoder(state, fallbackCfg))
                    {
                        encoderReady = true;
                        log_->warn("Fell back to {} encoder", encoderTypeToString(fallback));
                    }
                }

                if (encoderReady)
                {
                    state.transcoding = true;
                }
                else
                {
                    log_->warn("All encoders failed — falling back to stream copy");
                    // Clean up partial decoder state
                    if (state.videoDecCtx)
                    {
                        avcodec_free_context(&state.videoDecCtx);
                    }
                    if (state.decFrame)
                    {
                        av_frame_free(&state.decFrame);
                    }
                }
            }
            else
            {
                log_->warn("Video decoder setup failed — falling back to stream copy");
            }
        }

        // ── Create output streams ───────────────────────────────────
        if (state.videoIdx >= 0)
        {
            AVStream *inStream = state.inputCtx->streams[state.videoIdx];
            AVStream *outStream = avformat_new_stream(state.outputCtx, nullptr);
            if (!outStream)
                return false;

            if (state.transcoding && state.videoEncCtx)
            {
                // Transcoding: copy encoder params to output stream
                ret = avcodec_parameters_from_context(outStream->codecpar, state.videoEncCtx);
                if (ret < 0)
                {
                    log_->error("Failed to copy encoder params to stream: {}", ffError(ret));
                    return false;
                }
                outStream->time_base = state.videoEncCtx->time_base;
            }
            else
            {
                // Stream copy: copy codec params directly
                ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
                if (ret < 0)
                {
                    log_->error("Failed to copy video codec params: {}", ffError(ret));
                    return false;
                }
                outStream->time_base = inStream->time_base;
            }
            outStream->codecpar->codec_tag = 0;
            state.outVideoIdx = outStream->index;
        }

        if (state.audioIdx >= 0)
        {
            AVStream *inStream = state.inputCtx->streams[state.audioIdx];
            AVStream *outStream = avformat_new_stream(state.outputCtx, nullptr);
            if (!outStream)
                return false;

            // Audio: always stream copy (unless explicitly configured otherwise)
            ret = avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
            if (ret < 0)
            {
                log_->error("Failed to copy audio codec params: {}", ffError(ret));
                return false;
            }
            outStream->codecpar->codec_tag = 0;
            outStream->time_base = inStream->time_base;
            state.outAudioIdx = outStream->index;
        }

        // MP4 movflags (if configured)
        if (config_.container == ContainerFormat::MP4 && fmt.movFlags)
        {
            av_opt_set(state.outputCtx->priv_data, "movflags", fmt.movFlags, 0);
        }

        // Open output file
        if (!(state.outputCtx->oformat->flags & AVFMT_NOFILE))
        {
            ret = avio_open(&state.outputCtx->pb, outputPath.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0)
            {
                log_->error("Failed to open output file: {} - {}", outputPath, ffError(ret));
                return false;
            }
        }

        // Apply VR metadata if configured
        if (vrConfig.embedMetadata && vrConfig.projection != VRProjection::None)
        {
            applyVRMetadata(state, vrConfig);
        }

        // Muxer options
        AVDictionary *muxOpts = nullptr;
        av_dict_set(&muxOpts, "avoid_negative_ts", "make_zero", 0);

        // For Matroska: reserve space for the Cues (seek index) at the
        // beginning of the file. This way the trailer can write Cues and
        // Duration into the reserved space via a single seek, making the
        // file properly seekable even if the write is interrupted.
        const char *containerFmtName = state.outputCtx->oformat ? state.outputCtx->oformat->name : "";
        if (std::string_view(containerFmtName).find("matroska") != std::string_view::npos ||
            std::string_view(containerFmtName).find("webm") != std::string_view::npos)
        {
            av_dict_set(&muxOpts, "reserve_index_space", "262144", 0); // 256KB for Cues
        }

        ret = avformat_write_header(state.outputCtx, &muxOpts);
        av_dict_free(&muxOpts);

        if (ret < 0)
        {
            log_->error("Failed to write output header: {}", ffError(ret));
            return false;
        }

        state.headerWritten = true;

        if (state.transcoding)
        {
            log_->info("Output opened (transcoding): {} → {}",
                       state.videoEncCtx ? state.videoEncCtx->codec->name : "?", outputPath);
        }
        else
        {
            log_->info("Output opened (stream copy): {}", outputPath);
        }
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Apply VR / 360° metadata to output streams
    // ─────────────────────────────────────────────────────────────────
    // Sets container-level and stream-level metadata for VR players.
    // MP4: Spherical Video V2 tags (Google/YouTube spec via side_data)
    // MKV: Matroska StereoMode + ProjectionType tags
    // Both: Common metadata keys recognised by VR players
    void HLSRecorder::applyVRMetadata(FFmpegState &state, const VRConfig &vrConfig)
    {
        if (!state.outputCtx || vrConfig.projection == VRProjection::None)
            return;

        log_->info("Applying VR metadata: projection={}, stereo={}, fov={}",
                   vrProjectionToString(vrConfig.projection),
                   vrStereoModeToString(vrConfig.stereoMode),
                   vrConfig.fov);

        // ── Container-level metadata (works for MP4/MKV/TS) ─────────
        AVDictionary *&meta = state.outputCtx->metadata;

        // Google / YouTube Spherical Video V1 metadata (widely supported)
        av_dict_set(&meta, "spherical", "true", 0);

        // Projection type string
        switch (vrConfig.projection)
        {
        case VRProjection::Equirect360:
            av_dict_set(&meta, "spherical_mapping_type", "equirectangular", 0);
            av_dict_set(&meta, "projection", "equirectangular", 0);
            break;
        case VRProjection::Equirect180:
            av_dict_set(&meta, "spherical_mapping_type", "equirectangular_180", 0);
            av_dict_set(&meta, "projection", "equirectangular_180", 0);
            break;
        case VRProjection::Fisheye180:
            av_dict_set(&meta, "spherical_mapping_type", "fisheye", 0);
            av_dict_set(&meta, "projection", "fisheye", 0);
            break;
        case VRProjection::Cubemap:
            av_dict_set(&meta, "spherical_mapping_type", "cubemap", 0);
            av_dict_set(&meta, "projection", "cubemap", 0);
            break;
        case VRProjection::EAC:
            av_dict_set(&meta, "spherical_mapping_type", "equi-angular-cubemap", 0);
            av_dict_set(&meta, "projection", "equi_angular_cubemap", 0);
            break;
        default:
            break;
        }

        // Stereo mode metadata
        switch (vrConfig.stereoMode)
        {
        case VRStereoMode::SideBySide:
            av_dict_set(&meta, "stereo_mode", "left_right", 0);
            av_dict_set(&meta, "stereo3d", "side_by_side", 0);
            break;
        case VRStereoMode::TopBottom:
            av_dict_set(&meta, "stereo_mode", "top_bottom", 0);
            av_dict_set(&meta, "stereo3d", "top_bottom", 0);
            break;
        case VRStereoMode::Mono:
            av_dict_set(&meta, "stereo_mode", "mono", 0);
            break;
        default:
            break;
        }

        // FOV if non-standard
        if (vrConfig.fov > 0 && vrConfig.fov != 360)
        {
            av_dict_set(&meta, "field_of_view", std::to_string(vrConfig.fov).c_str(), 0);
        }

        // Custom tags from config
        for (const auto &[key, val] : vrConfig.customTags)
        {
            av_dict_set(&meta, key.c_str(), val.c_str(), 0);
        }

        // ── Stream-level metadata (video stream) ────────────────────
        if (state.outVideoIdx >= 0 && state.outputCtx->nb_streams > static_cast<unsigned>(state.outVideoIdx))
        {
            AVStream *vStream = state.outputCtx->streams[state.outVideoIdx];
            AVDictionary *&smeta = vStream->metadata;

            // Matroska StereoMode values (for MKV container)
            // 0 = mono, 1 = side by side (left first), 3 = top-bottom (left first)
            const char *containerFmt = state.outputCtx->oformat ? state.outputCtx->oformat->name : "";
            bool isMKV = (std::string_view(containerFmt).find("matroska") != std::string_view::npos ||
                          std::string_view(containerFmt).find("webm") != std::string_view::npos);

            if (isMKV)
            {
                switch (vrConfig.stereoMode)
                {
                case VRStereoMode::Mono:
                    av_dict_set(&smeta, "stereo_mode", "0", 0);
                    break;
                case VRStereoMode::SideBySide:
                    av_dict_set(&smeta, "stereo_mode", "1", 0);
                    break;
                case VRStereoMode::TopBottom:
                    av_dict_set(&smeta, "stereo_mode", "3", 0);
                    break;
                default:
                    break;
                }
            }

            // Copy spherical metadata to video stream too (some players check stream level)
            av_dict_set(&smeta, "spherical", "true", 0);
            av_dict_set(&smeta, "projection", av_dict_get(meta, "projection", nullptr, 0) ? av_dict_get(meta, "projection", nullptr, 0)->value : "equirectangular", 0);
        }

        log_->debug("VR metadata applied to output container");
    }

    // ─────────────────────────────────────────────────────────────────
    // Close ONLY the input context (keep output/encoder/decoder alive)
    // Used during restart: we re-open the input while continuing to
    // write to the same output file for one continuous recording.
    // ─────────────────────────────────────────────────────────────────
    void HLSRecorder::closeInput(FFmpegState &state)
    {
        if (state.inputCtx)
        {
            if (state.inputCtx->interrupt_callback.opaque)
            {
                delete static_cast<InterruptData *>(state.inputCtx->interrupt_callback.opaque);
                state.inputCtx->interrupt_callback.opaque = nullptr;
            }
            avformat_close_input(&state.inputCtx);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    // Clean up all FFmpeg resources
    // ─────────────────────────────────────────────────────────────────
    void HLSRecorder::closeAll(FFmpegState &state)
    {
        // NOTE: flushEncoder() should be called BEFORE closeAll() with real counters
        // so that flushed bytes are properly counted. If somehow we get here with
        // an un-flushed encoder, flush it now as a safety net.
        if (state.transcoding && state.videoEncCtx && state.outputCtx && state.headerWritten)
        {
            uint64_t safetyBytes = 0;
            uint32_t safetyPkts = 0;
            flushEncoder(state, safetyBytes, safetyPkts);
            if (safetyBytes > 0)
                log_->warn("closeAll safety flush wrote {} bytes — caller should flush first!", safetyBytes);
        }

        if (state.outputCtx)
        {
            if (state.headerWritten)
            {
                int ret = av_write_trailer(state.outputCtx);
                if (ret < 0)
                    log_->warn("Error writing trailer: {}", ffError(ret));
            }

            if (!(state.outputCtx->oformat->flags & AVFMT_NOFILE) && state.outputCtx->pb)
                avio_closep(&state.outputCtx->pb);

            avformat_free_context(state.outputCtx);
            state.outputCtx = nullptr;
        }

        // Clean up transcoding resources
        if (state.videoEncCtx)
            avcodec_free_context(&state.videoEncCtx);
        if (state.videoDecCtx)
            avcodec_free_context(&state.videoDecCtx);
        if (state.audioEncCtx)
            avcodec_free_context(&state.audioEncCtx);
        if (state.audioDecCtx)
            avcodec_free_context(&state.audioDecCtx);
        if (state.swsCtx)
        {
            sws_freeContext(state.swsCtx);
            state.swsCtx = nullptr;
        }
        if (state.decFrame)
            av_frame_free(&state.decFrame);
        if (state.encFrame)
            av_frame_free(&state.encFrame);
        if (state.hwDeviceCtx)
            av_buffer_unref(&state.hwDeviceCtx);

        if (state.inputCtx)
        {
            // Free interrupt data
            if (state.inputCtx->interrupt_callback.opaque)
            {
                delete static_cast<InterruptData *>(state.inputCtx->interrupt_callback.opaque);
                state.inputCtx->interrupt_callback.opaque = nullptr;
            }
            avformat_close_input(&state.inputCtx);
        }

        state.headerWritten = false;
        state.transcoding = false;
    }

    // ─────────────────────────────────────────────────────────────────
    // Process a single packet (timestamp fixup + write)
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::processPacket(FFmpegState &state, AVPacket *pkt,
                                    uint64_t &bytesWritten, uint32_t &packetsWritten,
                                    uint32_t &packetsDropped)
    {
        // Determine if this packet belongs to a stream we're copying
        int outStreamIdx = -1;
        AVStream *inStream = nullptr;
        AVStream *outStream = nullptr;

        if (pkt->stream_index == state.videoIdx && state.outVideoIdx >= 0)
        {
            outStreamIdx = state.outVideoIdx;
            inStream = state.inputCtx->streams[state.videoIdx];
            outStream = state.outputCtx->streams[state.outVideoIdx];
        }
        else if (pkt->stream_index == state.audioIdx && state.outAudioIdx >= 0)
        {
            outStreamIdx = state.outAudioIdx;
            inStream = state.inputCtx->streams[state.audioIdx];
            outStream = state.outputCtx->streams[state.outAudioIdx];
        }
        else
        {
            // Stream we don't care about (subtitles, data, etc.)
            return true;
        }

        // ── Skip until first video keyframe ─────────────────────────
        // For stream-copy mode: drop all packets (video AND audio)
        // until the first video keyframe arrives. This prevents the muxer
        // from writing corrupt non-decodable frames at the start.
        if (!state.gotKeyframe)
        {
            if (pkt->stream_index == state.videoIdx && (pkt->flags & AV_PKT_FLAG_KEY))
            {
                state.gotKeyframe = true;
                log_->info("First video keyframe received — starting mux");
            }
            else
            {
                packetsDropped++;
                return true; // skip everything until keyframe
            }
        }

        // ── Timestamp fixup ─────────────────────────────────────────
        // LIVE STREAM: Rebuild timestamps from 0!
        // Don't use original stream timestamps — they can be huge values
        // since the stream may have been running for hours/days.

        // Record first PTS/DTS for offset calculation
        if (pkt->stream_index == state.videoIdx)
        {
            if (state.firstVideoPts == INT64_MIN && pkt->pts != AV_NOPTS_VALUE)
                state.firstVideoPts = pkt->pts;
            if (state.videoStartDts == 0 && pkt->dts != AV_NOPTS_VALUE)
                state.videoStartDts = pkt->dts;
        }
        if (pkt->stream_index == state.audioIdx)
        {
            if (state.firstAudioPts == INT64_MIN && pkt->pts != AV_NOPTS_VALUE)
                state.firstAudioPts = pkt->pts;
            if (state.audioStartDts == 0 && pkt->dts != AV_NOPTS_VALUE)
                state.audioStartDts = pkt->dts;
        }

        // Fix invalid timestamps
        if (pkt->dts == AV_NOPTS_VALUE && pkt->pts != AV_NOPTS_VALUE)
            pkt->dts = pkt->pts;
        if (pkt->pts == AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE)
            pkt->pts = pkt->dts;

        // Ensure PTS >= DTS
        if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
        {
            pkt->pts = pkt->dts;
        }

        // Drop packets with completely invalid timestamps
        if (pkt->dts == AV_NOPTS_VALUE && pkt->pts == AV_NOPTS_VALUE)
        {
            packetsDropped++;
            return true; // not fatal, just skip
        }

        // ── Rebuild timestamps from 0 (subtract first PTS) ──────────
        int64_t ptsOffset = 0;
        int64_t dtsOffset = 0;
        if (pkt->stream_index == state.videoIdx)
        {
            ptsOffset = (state.firstVideoPts != INT64_MIN) ? state.firstVideoPts : state.videoStartDts;
            dtsOffset = state.videoStartDts;
        }
        else if (pkt->stream_index == state.audioIdx)
        {
            ptsOffset = (state.firstAudioPts != INT64_MIN) ? state.firstAudioPts : state.audioStartDts;
            dtsOffset = state.audioStartDts;
        }

        // Subtract offset to start from 0
        if (pkt->pts != AV_NOPTS_VALUE && ptsOffset > 0)
            pkt->pts -= ptsOffset;
        if (pkt->dts != AV_NOPTS_VALUE && dtsOffset > 0)
            pkt->dts -= dtsOffset;

        // Rescale timestamps to output stream timebase
        pkt->stream_index = outStreamIdx;
        av_packet_rescale_ts(pkt, inStream->time_base, outStream->time_base);

        // ── Restart continuity offset (stream-copy mode) ────────────
        // After a restart, the new input's PTS starts from 0 again.
        // Add the accumulated offset so timestamps continue from where
        // the previous iteration left off → one continuous output file.
        if (outStreamIdx == state.outVideoIdx && state.videoRestartOffset > 0)
        {
            pkt->pts += state.videoRestartOffset;
            pkt->dts += state.videoRestartOffset;
        }
        else if (outStreamIdx == state.outAudioIdx && state.audioRestartOffset > 0)
        {
            pkt->pts += state.audioRestartOffset;
            pkt->dts += state.audioRestartOffset;
        }

        // Make timestamps non-negative (safety clamp)
        if (pkt->dts < 0)
            pkt->dts = 0;
        if (pkt->pts < 0)
            pkt->pts = 0;
        if (pkt->pts < pkt->dts)
            pkt->pts = pkt->dts;

        // Set packet duration (helps muxer calculate total stream duration)
        if (outStreamIdx == state.outVideoIdx && state.lastVideoOutPts > 0 && pkt->pts > state.lastVideoOutPts)
            pkt->duration = pkt->pts - state.lastVideoOutPts;
        else if (outStreamIdx == state.outAudioIdx && state.lastAudioOutPts > 0 && pkt->pts > state.lastAudioOutPts)
            pkt->duration = pkt->pts - state.lastAudioOutPts;

        // Track last written PTS (output timebase) for restart continuity
        if (outStreamIdx == state.outVideoIdx)
            state.lastVideoOutPts = std::max(state.lastVideoOutPts, pkt->pts);
        else if (outStreamIdx == state.outAudioIdx)
            state.lastAudioOutPts = std::max(state.lastAudioOutPts, pkt->pts);

        // ── Write packet ────────────────────────────────────────────
        // IMPORTANT: Save size BEFORE write — av_interleaved_write_frame
        // takes ownership and unrefs the packet, zeroing pkt->size!
        int pktSize = pkt->size;
        int ret = av_interleaved_write_frame(state.outputCtx, pkt);
        if (ret < 0)
        {
            // Don't fail on individual packet write errors (corruption resilience)
            packetsDropped++;
            log_->debug("Dropped packet: {}", ffError(ret));
            return true;
        }

        bytesWritten += pktSize;
        packetsWritten++;
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Transcode a single packet (decode → convert → encode → write)
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::transcodePacket(FFmpegState &state, AVPacket *pkt,
                                      uint64_t &bytesWritten, uint32_t &packetsWritten,
                                      uint32_t &packetsDropped)
    {
        // Audio packets: always stream copy (pass through processPacket)
        // But skip audio until we have the first video keyframe — otherwise
        // the muxer gets audio way ahead of video.
        if (pkt->stream_index == state.audioIdx)
        {
            if (!state.gotKeyframe)
            {
                packetsDropped++;
                return true;
            }
            return processPacket(state, pkt, bytesWritten, packetsWritten, packetsDropped);
        }

        // Only transcode video packets
        if (pkt->stream_index != state.videoIdx || !state.videoDecCtx || !state.videoEncCtx)
        {
            return true; // ignore non-video/audio streams
        }

        // ── Skip until first keyframe ───────────────────────────────
        // Live HLS streams may start mid-GOP — the HEVC decoder cannot
        // decode P/B frames without their reference (IDR) frame, causing
        // "Could not find ref with POC" errors. Drop everything until
        // the first keyframe (I-frame / IDR) arrives.
        if (!state.gotKeyframe)
        {
            if (pkt->flags & AV_PKT_FLAG_KEY)
            {
                state.gotKeyframe = true;
                log_->info("First video keyframe received — starting decode");
            }
            else
            {
                packetsDropped++;
                return true; // silently skip
            }
        }

        // ── Send packet to decoder ──────────────────────────────────
        int ret = avcodec_send_packet(state.videoDecCtx, pkt);
        if (ret < 0)
        {
            if (ret == AVERROR(EAGAIN))
            {
                // Decoder buffer full — need to receive frames first
                // Fall through to receive loop
            }
            else if (ret == AVERROR_INVALIDDATA)
            {
                packetsDropped++;
                return true; // corrupted packet, skip
            }
            else
            {
                log_->debug("Decoder send error: {}", ffError(ret));
                packetsDropped++;
                return true;
            }
        }

        // ── Receive decoded frames and encode ───────────────────────
        while (true)
        {
            ret = avcodec_receive_frame(state.videoDecCtx, state.decFrame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
            {
                log_->debug("Decoder receive error: {}", ffError(ret));
                break;
            }

            // Determine which frame to send to encoder
            AVFrame *frameToEncode = state.decFrame;

            // Pixel format / resolution conversion if needed
            if (state.swsCtx && state.encFrame)
            {
                ret = av_frame_make_writable(state.encFrame);
                if (ret < 0)
                {
                    av_frame_unref(state.decFrame);
                    continue;
                }

                sws_scale(state.swsCtx,
                          state.decFrame->data, state.decFrame->linesize,
                          0, state.videoDecCtx->height,
                          state.encFrame->data, state.encFrame->linesize);

                frameToEncode = state.encFrame;
            }

            // ── Rebuild timestamps from 0 (live stream) ─────────────
            // Don't use original stream timestamps — generate clean PTS from frame count
            // This ensures the output file starts at 0 and has monotonic timestamps
            AVRational encTimeBase = state.videoEncCtx->time_base;
            AVRational frameRate = state.videoEncCtx->framerate;

            // Calculate frame duration in encoder timebase
            int64_t frameDuration = 1;
            if (frameRate.num > 0 && frameRate.den > 0)
            {
                // duration = timebase / framerate = (timebase.num/timebase.den) / (fps.num/fps.den)
                // = timebase.num * fps.den / (timebase.den * fps.num)
                frameDuration = av_rescale_q(1, av_inv_q(frameRate), encTimeBase);
                if (frameDuration <= 0)
                    frameDuration = 1;
            }

            frameToEncode->pts = state.videoFrameCount * frameDuration;
            frameToEncode->pkt_dts = frameToEncode->pts;
            frameToEncode->duration = frameDuration;
            state.videoFrameCount++;

            // ── Send frame to encoder ───────────────────────────────
            ret = avcodec_send_frame(state.videoEncCtx, frameToEncode);
            av_frame_unref(state.decFrame);

            if (ret < 0)
            {
                if (ret != AVERROR(EAGAIN))
                    log_->debug("Encoder send error: {}", ffError(ret));
                continue;
            }

            // ── Receive encoded packets ─────────────────────────────
            AVPacket *encPkt = av_packet_alloc();
            while (true)
            {
                ret = avcodec_receive_packet(state.videoEncCtx, encPkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                    break;
                if (ret < 0)
                {
                    log_->debug("Encoder receive error: {}", ffError(ret));
                    break;
                }

                // Set output stream index and rescale timestamps
                encPkt->stream_index = state.outVideoIdx;
                AVStream *outStream = state.outputCtx->streams[state.outVideoIdx];
                av_packet_rescale_ts(encPkt, state.videoEncCtx->time_base, outStream->time_base);

                // Ensure non-negative timestamps
                if (encPkt->dts < 0)
                    encPkt->dts = 0;
                if (encPkt->pts < 0)
                    encPkt->pts = 0;
                if (encPkt->pts < encPkt->dts)
                    encPkt->pts = encPkt->dts;

                // Save size BEFORE write — av_interleaved_write_frame unrefs the packet!
                int encSize = encPkt->size;
                ret = av_interleaved_write_frame(state.outputCtx, encPkt);
                if (ret < 0)
                {
                    packetsDropped++;
                    log_->debug("Dropped encoded packet: {}", ffError(ret));
                }
                else
                {
                    bytesWritten += encSize;
                    packetsWritten++;
                }
            }
            av_packet_free(&encPkt);
        }

        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Flush encoder (drain remaining buffered frames)
    // ─────────────────────────────────────────────────────────────────
    void HLSRecorder::flushEncoder(FFmpegState &state, uint64_t &bytesWritten,
                                   uint32_t &packetsWritten)
    {
        if (!state.videoEncCtx || !state.outputCtx)
            return;

        log_->debug("Flushing video encoder...");

        // Signal end of stream to encoder
        int ret = avcodec_send_frame(state.videoEncCtx, nullptr);
        if (ret < 0 && ret != AVERROR_EOF)
        {
            log_->debug("Encoder flush send error: {}", ffError(ret));
            return;
        }

        AVPacket *encPkt = av_packet_alloc();
        while (true)
        {
            ret = avcodec_receive_packet(state.videoEncCtx, encPkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                break;
            if (ret < 0)
            {
                log_->debug("Encoder flush receive error: {}", ffError(ret));
                break;
            }

            encPkt->stream_index = state.outVideoIdx;
            AVStream *outStream = state.outputCtx->streams[state.outVideoIdx];
            av_packet_rescale_ts(encPkt, state.videoEncCtx->time_base, outStream->time_base);

            if (encPkt->dts < 0)
                encPkt->dts = 0;
            if (encPkt->pts < 0)
                encPkt->pts = 0;

            // Save size BEFORE write — av_interleaved_write_frame unrefs the packet!
            int flushSize = encPkt->size;
            ret = av_interleaved_write_frame(state.outputCtx, encPkt);
            if (ret >= 0)
            {
                bytesWritten += flushSize;
                packetsWritten++;
            }
        }
        av_packet_free(&encPkt);

        log_->debug("Encoder flushed");
    }

    // ─────────────────────────────────────────────────────────────────
    // Preview frame capture — decode one frame → scale → encode JPEG
    // ─────────────────────────────────────────────────────────────────
    bool HLSRecorder::savePreviewJpeg(AVFrame *frame, const std::string &path)
    {
        if (!frame || path.empty())
            return false;

        const AVCodec *mjpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!mjpegCodec)
        {
            log_->warn("MJPEG encoder not available for preview");
            return false;
        }

        AVCodecContext *jpegCtx = avcodec_alloc_context3(mjpegCodec);
        if (!jpegCtx)
            return false;

        // Scale down for preview (max 640px wide)
        int w = frame->width;
        int h = frame->height;
        if (w > 640)
        {
            h = h * 640 / w;
            w = 640;
        }
        // Ensure even dimensions
        w = (w / 2) * 2;
        h = (h / 2) * 2;
        if (w <= 0 || h <= 0)
        {
            avcodec_free_context(&jpegCtx);
            return false;
        }

        jpegCtx->pix_fmt = AV_PIX_FMT_YUVJ420P;
        jpegCtx->width = w;
        jpegCtx->height = h;
        jpegCtx->time_base = {1, 25};
        jpegCtx->flags |= AV_CODEC_FLAG_QSCALE;
        jpegCtx->global_quality = 5 * FF_QP2LAMBDA;

        if (avcodec_open2(jpegCtx, mjpegCodec, nullptr) < 0)
        {
            avcodec_free_context(&jpegCtx);
            return false;
        }

        SwsContext *sws = sws_getContext(
            frame->width, frame->height, (AVPixelFormat)frame->format,
            w, h, AV_PIX_FMT_YUVJ420P,
            SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws)
        {
            avcodec_free_context(&jpegCtx);
            return false;
        }

        AVFrame *scaledFrame = av_frame_alloc();
        scaledFrame->format = AV_PIX_FMT_YUVJ420P;
        scaledFrame->width = w;
        scaledFrame->height = h;
        if (av_frame_get_buffer(scaledFrame, 0) < 0)
        {
            av_frame_free(&scaledFrame);
            sws_freeContext(sws);
            avcodec_free_context(&jpegCtx);
            return false;
        }

        sws_scale(sws, frame->data, frame->linesize, 0, frame->height,
                  scaledFrame->data, scaledFrame->linesize);

        scaledFrame->pts = 0;
        avcodec_send_frame(jpegCtx, scaledFrame);

        AVPacket *jpegPkt = av_packet_alloc();
        int ret = avcodec_receive_packet(jpegCtx, jpegPkt);

        bool ok = false;
        if (ret == 0)
        {
            // Ensure parent directory exists
            auto parentDir = std::filesystem::path(path).parent_path();
            if (!parentDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parentDir, ec);
            }

            FILE *f = fopen(path.c_str(), "wb");
            if (f)
            {
                fwrite(jpegPkt->data, 1, jpegPkt->size, f);
                fclose(f);
                ok = true;
                log_->info("Preview frame captured: {} ({}x{} from {}x{})",
                           path, w, h, frame->width, frame->height);
            }
        }

        av_packet_free(&jpegPkt);
        av_frame_free(&scaledFrame);
        sws_freeContext(sws);
        avcodec_free_context(&jpegCtx);

        return ok;
    }

    // ─────────────────────────────────────────────────────────────────
    // Main recording loop
    // ─────────────────────────────────────────────────────────────────
    RecordingResult HLSRecorder::record(const std::string &hlsUrl,
                                        const std::string &outputPath,
                                        CancellationToken &cancel,
                                        const std::string &userAgent,
                                        const std::string &cookies,
                                        const std::map<std::string, std::string> &headers,
                                        const VRConfig &vrConfig)
    {
        RecordingResult result;

        // Ensure output directory exists
        auto outputDir = std::filesystem::path(outputPath).parent_path();
        if (!outputDir.empty())
            std::filesystem::create_directories(outputDir);

        log_->info("Starting HLS recording: {} → {}", hlsUrl, outputPath);

        // ── Start segment feeder ────────────────────────────────────
        // Instead of writing a playlist to a temp file (which races with
        // FFmpeg on Windows), we parse the m3u8 ourselves, download each
        // segment, and feed raw bytes to FFmpeg via a custom AVIOContext.
        // FFmpeg sees a continuous fMP4/MPEG-TS byte stream — no HLS
        // demuxer, no temp files, no file-locking races.
        SegmentFeeder feeder;
        feeder.log = log_;
        feeder.config = &config_;
        PlaylistDecoder feederDecoder = nullptr;
        std::string feederUa;
        std::string inputUrl = hlsUrl; // default: use remote URL directly
        bool useFeeder = false;

        if (hlsUrl.find(".m3u8") != std::string::npos ||
            hlsUrl.find(".m3u") != std::string::npos)
        {
            feederUa = userAgent.empty() ? config_.userAgent : userAgent;

            // If this is a StripChat/doppiocdn URL, attach mouflon decoder
            if (hlsUrl.find("doppiocdn") != std::string::npos)
            {
                auto parseQParam = [](const std::string &url, const std::string &param) -> std::string
                {
                    auto key = param + "=";
                    auto pos = url.find(key);
                    if (pos == std::string::npos)
                        return "";
                    auto start = pos + key.size();
                    auto end = url.find('&', start);
                    return (end == std::string::npos) ? url.substr(start) : url.substr(start, end - start);
                };

                MouflonKeys::MouflonInfo fallback;
                fallback.psch = parseQParam(hlsUrl, "psch");
                fallback.pkey = parseQParam(hlsUrl, "pkey");
                fallback.pdkey = parseQParam(hlsUrl, "pdkey");

                log_->debug("Mouflon fallback from URL: psch={}, pkey={}, pdkey={}",
                            fallback.psch, fallback.pkey,
                            fallback.pdkey.empty() ? "(none)" : "***");

                feederDecoder = [fallback](const std::string &content) -> std::string
                {
                    return MouflonKeys::instance().decodePlaylists(content, fallback);
                };
            }

            if (feeder.start(hlsUrl, feederUa, cancel, feederDecoder))
            {
                useFeeder = true;
                log_->info("Using SegmentFeeder (in-memory segment download)");
            }
            else if (feederDecoder)
            {
                // NEVER fall back to raw remote URL for mouflon-encrypted streams.
                log_->error("SegmentFeeder failed for encrypted stream — cannot use raw URL");
                result.error = "Failed to start segment feeder for encrypted stream";
                return result;
            }
            else
            {
                log_->warn("SegmentFeeder failed, using direct HLS URL");
            }
        }

        int restartCount = 0;
        const int maxRestarts = config_.ffmpeg.maxRestarts;

        // ── Single FFmpegState for the entire recording session ─────
        // Output is opened ONCE and kept alive across restarts.
        // Only the input is re-opened on restart → one continuous file.
        FFmpegState state;
        bool outputOpened = false;

        // Outer do-while: handles pause/resume across ALL recording exits.
        // The inner while handles restarts for stalls/errors within one
        // recording session. When the inner while exits for ANY reason
        // (EOF, stall max-out, probe failure, feeder error), the pause
        // handler below it gets a chance to keep the file open.
        bool sessionContinue = false;
        do
        {
            sessionContinue = false;

            while (!cancel.isCancelled() && restartCount <= maxRestarts)
            {
                StallDetector stall;
                stall.reset();

                // ── Open input ──────────────────────────────────────────
                AVIOContext *currentAVIO = nullptr;
                if (useFeeder)
                {
                    // Create AVIO from feeder's ring buffer
                    currentAVIO = feeder.createAVIO();
                    if (!currentAVIO)
                    {
                        result.error = "Failed to create AVIO context";
                        restartCount++;
                        log_->error("{}", result.error);
                        break;
                    }
                }

                if (!openInput(state, useFeeder ? "" : inputUrl,
                               userAgent, cookies, headers, currentAVIO))
                {
                    result.error = "Failed to open HLS stream";
                    restartCount++;
                    if (restartCount <= maxRestarts)
                    {
                        // Probe the original remote URL — if stream is dead, stop immediately
                        // (Python logic: bot layer checks getStatus() and won't re-enter download
                        //  if the model isn't PUBLIC. We mirror that here for the inner loop.)
                        if (!hlsUrl.empty())
                        {
                            HttpClient probe;
                            std::string ua = userAgent.empty() ? config_.userAgent : userAgent;
                            probe.setDefaultUserAgent(ua);
                            applyProxyConfig(probe, config_);
                            auto probeResp = probe.get(hlsUrl, 8);
                            if (!probeResp.ok() || probeResp.body.find("#EXTM3U") == std::string::npos)
                            {
                                log_->info("Stream no longer available (HTTP {}), stopping",
                                           probeResp.statusCode);
                                break;
                            }
                        }

                        // Progressive backoff: 3s, 5s, 8s, 10s, 10s, ...
                        int backoff = std::min(3 + restartCount * 2, 10);
                        log_->warn("Restart {}/{}: {} (retry in {}s)",
                                   restartCount, maxRestarts, result.error, backoff);
                        std::this_thread::sleep_for(std::chrono::seconds(backoff));
                        continue;
                    }
                    break;
                }

                // Set cancellation on interrupt callback
                auto *interruptData = static_cast<InterruptData *>(
                    state.inputCtx->interrupt_callback.opaque);
                if (interruptData)
                    interruptData->cancel = &cancel;

                // ── Open output ONCE (on first successful input) ────────
                if (!outputOpened)
                {
                    if (!openOutput(state, outputPath, vrConfig))
                    {
                        result.error = "Failed to open output file";
                        closeAll(state);
                        break; // Output failures are fatal
                    }
                    outputOpened = true;
                }

                // ── Packet reading loop ─────────────────────────────────
                AVPacket *pkt = av_packet_alloc();
                if (!pkt)
                {
                    result.error = "Failed to allocate packet";
                    break;
                }

                uint64_t bytesWritten = 0;
                uint32_t packetsWritten = 0;
                uint32_t packetsDropped = 0;
                auto recordStart = Clock::now();
                bool needRestart = false;

                {
                    std::lock_guard lock(statsMutex_);
                    stats_.recordingStarted = recordStart;
                    stats_.currentFile = outputPath;
                }

                while (!cancel.isCancelled())
                {
                    // Set read timeout via interrupt callback
                    if (interruptData)
                    {
                        interruptData->deadline = Clock::now() +
                                                  std::chrono::seconds(config_.ffmpeg.rwTimeoutSec * 2);
                        interruptData->hasDeadline = true;
                    }

                    int ret = av_read_frame(state.inputCtx, pkt);

                    if (ret < 0)
                    {
                        if (ret == AVERROR_EOF)
                        {
                            // With SegmentFeeder: EOF means the feeder's ring buffer
                            // is empty and `finished` is true — stream really ended
                            // (went offline / 404 / too many errors).
                            // With direct HLS URL: normal HLS EOF behavior.
                            if (useFeeder)
                            {
                                // SegmentFeeder sets finished=true only when the
                                // stream is truly gone. Trust it.
                                if (feeder.hasError)
                                    log_->warn("SegmentFeeder reported error — stream ended");
                                else
                                    log_->info("SegmentFeeder finished — stream ended");
                            }
                            else
                            {
                                // Direct HLS URL: check if stream is still alive
                                if (!cancel.isCancelled())
                                {
                                    HttpClient probe;
                                    std::string ua = userAgent.empty()
                                                         ? config_.userAgent
                                                         : userAgent;
                                    probe.setDefaultUserAgent(ua);
                                    applyProxyConfig(probe, config_);
                                    auto probeResp = probe.get(hlsUrl, 8);
                                    if (probeResp.ok() && !probeResp.body.empty() &&
                                        probeResp.body.find("#EXTM3U") != std::string::npos)
                                    {
                                        needRestart = true;
                                        break;
                                    }
                                }
                                log_->info("Stream appears to have ended");
                            }
                            break;
                        }
                        if (ret == AVERROR_EXIT)
                        {
                            log_->info("Recording cancelled");
                            break;
                        }
                        if (cancel.isCancelled())
                            break;

                        // Read error — for SegmentFeeder this shouldn't happen
                        // (AVIO returns clean data). For direct URL, probe stream.
                        log_->warn("Read error: {} (restart {}/{})",
                                   ffError(ret), restartCount, maxRestarts);
                        if (!useFeeder)
                        {
                            HttpClient probe;
                            std::string ua = userAgent.empty() ? config_.userAgent : userAgent;
                            probe.setDefaultUserAgent(ua);
                            applyProxyConfig(probe, config_);
                            auto probeResp = probe.get(hlsUrl, 8);
                            if (probeResp.ok() && !probeResp.body.empty() &&
                                probeResp.body.find("#EXTM3U") != std::string::npos)
                            {
                                needRestart = true;
                                break;
                            }
                        }
                        log_->info("Stream appears to have ended");
                        break;
                    }

                    // ── Process packet (transcoding or stream copy) ─────
                    bool packetOk = false;
                    if (state.transcoding)
                    {
                        packetOk = transcodePacket(state, pkt, bytesWritten, packetsWritten, packetsDropped);
                    }
                    else
                    {
                        packetOk = processPacket(state, pkt, bytesWritten, packetsWritten, packetsDropped);
                    }

                    if (!packetOk)
                    {
                        log_->error("Fatal packet processing error");
                        break;
                    }

                    // ── Stall detection ─────────────────────────────────
                    stall.onPacket(pkt->pts, bytesWritten);

                    // Only check stalls after startup grace period
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                       Clock::now() - recordStart)
                                       .count();
                    if (elapsed > config_.ffmpeg.startupGraceSec)
                    {
                        if (stall.checkStall(config_.ffmpeg))
                        {
                            log_->warn("Stall detected! No progress for {}s",
                                       config_.ffmpeg.stallSameTimeSec);
                            result.stallsDetected++;
                            needRestart = true;
                            break;
                        }
                    }

                    // ── Progress reporting ──────────────────────────────
                    if (progressCb_ && packetsWritten % 100 == 0)
                    {
                        double dur = static_cast<double>(elapsed);
                        RecordingProgress prog;
                        prog.bytesWritten = bytesWritten;
                        prog.packetsWritten = packetsWritten;
                        prog.durationSec = dur;
                        prog.speed = dur > 0 ? (bytesWritten / 1024.0 / dur) : 0;
                        prog.isStalled = stall.isStalled;
                        progressCb_(prog);
                    }

                    // Update stats
                    {
                        std::lock_guard lock(statsMutex_);
                        stats_.bytesWritten = result.bytesWritten + bytesWritten;
                        stats_.segmentsRecorded = packetsWritten;
                    }

                    av_packet_unref(pkt);
                }

                av_packet_free(&pkt);

                // Accumulate results
                result.bytesWritten += bytesWritten;
                result.packetsWritten += packetsWritten;
                result.packetsDropped += packetsDropped;

                auto dur = std::chrono::duration_cast<std::chrono::seconds>(
                               Clock::now() - recordStart)
                               .count();
                result.durationSec += static_cast<double>(dur);

                // ── Handle restart ──────────────────────────────────────
                if (needRestart && !cancel.isCancelled())
                {
                    restartCount++;
                    result.restartsPerformed++;

                    if (restartCount <= maxRestarts)
                    {
                        // Flush decoder to clear stale reference frames
                        if (state.transcoding && state.videoDecCtx)
                            avcodec_flush_buffers(state.videoDecCtx);

                        // For stream-copy: save restart offsets
                        if (!state.transcoding)
                        {
                            state.videoRestartOffset = state.lastVideoOutPts + 1;
                            state.audioRestartOffset = state.lastAudioOutPts + 1;
                        }

                        // Reset per-input-session state
                        state.firstVideoPts = INT64_MIN;
                        state.firstAudioPts = INT64_MIN;
                        state.videoStartDts = 0;
                        state.audioStartDts = 0;
                        state.gotKeyframe = false;

                        // Close ONLY the input (keep output + encoder alive)
                        closeInput(state);

                        // For SegmentFeeder: must stop old feeder and start fresh
                        // (new init segment + fresh segments for FFmpeg to probe)
                        if (useFeeder)
                        {
                            feeder.stop();

                            // Brief wait before re-creating feeder
                            for (int i = 0; i < 20 && !cancel.isCancelled(); i++)
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                            if (!feeder.start(hlsUrl, feederUa, cancel, feederDecoder))
                            {
                                log_->error("SegmentFeeder restart failed");
                                break;
                            }
                            log_->debug("SegmentFeeder restarted for attempt {}/{}",
                                        restartCount, maxRestarts);
                        }

                        // Delay before restart (stall cooldown)
                        if (restartCount >= config_.ffmpeg.cooldownAfterStalls)
                        {
                            log_->info("Cooldown for {}s after {} stalls",
                                       config_.ffmpeg.cooldownSleepSec, restartCount);
                            for (int i = 0; i < config_.ffmpeg.cooldownSleepSec && !cancel.isCancelled(); i++)
                                std::this_thread::sleep_for(1s);
                        }
                        else
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(2));
                        }
                        continue;
                    }
                    log_->error("Max restarts ({}/{}) exceeded",
                                restartCount, maxRestarts);
                }

                break;
            } // end recording while loop

            // ── Pause/resume handler (catches ALL recording exits) ──────
            // When model goes private/offline, keep the output file open and
            // poll for the model to return. This runs for ANY recording exit
            // (natural EOF, stall max-out, probe failure, feeder error).
            // Only fatal errors (output open failure, alloc failure) or
            // explicit cancellation skip this block.
            if (!cancel.isCancelled() && pauseResumeCb_ && outputOpened)
            {
                log_->info("Stream ended — pausing (output file stays open)");

                // Save restart offsets for PTS continuity when resumed
                if (!state.transcoding)
                {
                    state.videoRestartOffset = state.lastVideoOutPts + 1;
                    state.audioRestartOffset = state.lastAudioOutPts + 1;
                }
                if (state.transcoding && state.videoDecCtx)
                    avcodec_flush_buffers(state.videoDecCtx);

                state.firstVideoPts = INT64_MIN;
                state.firstAudioPts = INT64_MIN;
                state.videoStartDts = 0;
                state.audioStartDts = 0;
                state.gotKeyframe = false;

                // Close input + feeder but keep output open
                closeInput(state);
                if (useFeeder && feeder.running.load())
                    feeder.stop();

                // Poll callback every ~5 seconds until resume or stop
                while (!cancel.isCancelled())
                {
                    for (int i = 0; i < 5 && !cancel.isCancelled(); i++)
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    if (cancel.isCancelled())
                        break;

                    auto pr = pauseResumeCb_();
                    if (pr.action == PauseAction::Resume && !pr.newUrl.empty())
                    {
                        inputUrl = pr.newUrl;

                        // Rebuild mouflon decoder for new URL if needed
                        if (inputUrl.find("doppiocdn") != std::string::npos)
                        {
                            auto parseQ = [](const std::string &u, const std::string &p) -> std::string
                            {
                                auto k = p + "=";
                                auto pos = u.find(k);
                                if (pos == std::string::npos)
                                    return "";
                                auto s = pos + k.size();
                                auto e = u.find('&', s);
                                return (e == std::string::npos) ? u.substr(s) : u.substr(s, e - s);
                            };
                            MouflonKeys::MouflonInfo fb;
                            fb.psch = parseQ(inputUrl, "psch");
                            fb.pkey = parseQ(inputUrl, "pkey");
                            fb.pdkey = parseQ(inputUrl, "pdkey");
                            feederDecoder = [fb](const std::string &content) -> std::string
                            {
                                return MouflonKeys::instance().decodePlaylists(content, fb);
                            };
                        }

                        // Restart feeder with new URL
                        if (useFeeder)
                        {
                            if (feeder.start(inputUrl, feederUa, cancel, feederDecoder))
                            {
                                log_->info("Recording resumed with new stream");
                                restartCount = 0;
                                sessionContinue = true;
                                break;
                            }
                            log_->warn("Feeder restart failed, will retry...");
                            continue;
                        }
                        log_->info("Recording resumed (direct URL)");
                        restartCount = 0;
                        sessionContinue = true;
                        break;
                    }
                    else if (pr.action == PauseAction::Stop)
                    {
                        log_->info("Stopping paused recording");
                        break;
                    }
                    // PauseAction::Wait → keep polling
                }
            }

        } while (sessionContinue);

        // ── Final cleanup: flush encoder + close everything ─────────
        if (state.transcoding && state.videoEncCtx && state.outputCtx && state.headerWritten)
        {
            uint64_t flushBytes = 0;
            uint32_t flushPkts = 0;
            flushEncoder(state, flushBytes, flushPkts);
            result.bytesWritten += flushBytes;
            result.packetsWritten += flushPkts;
        }
        closeAll(state);

        // ── Stop segment feeder ─────────────────────────────────────
        if (feeder.running.load())
            feeder.stop();

        // ── Clean up zero-byte output files ─────────────────────────
        if (std::filesystem::exists(outputPath) &&
            std::filesystem::file_size(outputPath) == 0)
        {
            std::filesystem::remove(outputPath);
        }

        result.success = result.bytesWritten > 0;

        log_->info("Recording finished: {} bytes, {} packets, {:.1f}s, {} restarts",
                   result.bytesWritten, result.packetsWritten,
                   result.durationSec, result.restartsPerformed);

        return result;
    }

    // ─────────────────────────────────────────────────────────────────
    // Playlist probe
    // ─────────────────────────────────────────────────────────────────
    bool probeHLSPlaylist(const std::string &url, const std::string &userAgent,
                          int timeoutSec)
    {
        AVFormatContext *ctx = avformat_alloc_context();
        if (!ctx)
            return false;

        AVDictionary *opts = nullptr;
        if (!userAgent.empty())
            av_dict_set(&opts, "user_agent", userAgent.c_str(), 0);

        std::string timeout = std::to_string(timeoutSec * 1000000LL);
        av_dict_set(&opts, "timeout", timeout.c_str(), 0);

        int ret = avformat_open_input(&ctx, url.c_str(), nullptr, &opts);
        av_dict_free(&opts);

        if (ret < 0)
        {
            // ctx is already freed on failure
            return false;
        }

        avformat_close_input(&ctx);
        return true;
    }

} // namespace sm
