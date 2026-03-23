// ─────────────────────────────────────────────────────────────────
// StripHelper — Native FFmpeg operations implementation
// Direct libavformat/libavcodec API for probing, remux, concat.
// ─────────────────────────────────────────────────────────────────

#include "native_ffmpeg.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <unordered_map>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
#include <libavutil/hwcontext.h>
}

namespace sh
{
    // ── RAII helpers ─────────────────────────────────────────────────
    namespace
    {
        struct FmtCtxIn
        {
            AVFormatContext *ctx = nullptr;
            ~FmtCtxIn()
            {
                if (ctx)
                    avformat_close_input(&ctx);
            }
        };

        struct FmtCtxOut
        {
            AVFormatContext *ctx = nullptr;
            bool pbOpened = false;
            ~FmtCtxOut()
            {
                if (ctx)
                {
                    if (pbOpened && ctx->pb)
                        avio_closep(&ctx->pb);
                    avformat_free_context(ctx);
                }
            }
        };
    }

    // ── Probe ────────────────────────────────────────────────────────

    NativeProbeResult nativeProbe(const fs::path &filePath)
    {
        NativeProbeResult r;
        FmtCtxIn fmt;

        try
        {
            r.fileSize = (int64_t)fs::file_size(filePath);
        }
        catch (...)
        {
        }

        std::string path = filePath.string();
        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return r;

        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return r;

        // Duration
        if (fmt.ctx->duration > 0)
            r.duration = fmt.ctx->duration / (double)AV_TIME_BASE;

        // Scan streams
        for (unsigned i = 0; i < fmt.ctx->nb_streams; ++i)
        {
            auto *st = fmt.ctx->streams[i];
            auto *par = st->codecpar;

            if (par->codec_type == AVMEDIA_TYPE_VIDEO && !r.hasVideo)
            {
                r.hasVideo = true;
                r.width = par->width;
                r.height = par->height;
                r.videoCodec = avcodec_get_name(par->codec_id);

                // FPS
                if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
                    r.fps = av_q2d(st->avg_frame_rate);
                else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
                    r.fps = av_q2d(st->r_frame_rate);

                // Pix format
                const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)par->format);
                if (desc)
                    r.pixFmt = desc->name;

                // Try stream-level duration if container-level is missing
                if (r.duration <= 0 && st->duration > 0 && st->time_base.den > 0)
                    r.duration = st->duration * av_q2d(st->time_base);
            }
            else if (par->codec_type == AVMEDIA_TYPE_AUDIO && !r.hasAudio)
            {
                r.hasAudio = true;
                r.audioCodec = avcodec_get_name(par->codec_id);
                r.sampleRate = par->sample_rate;
                r.channels = par->ch_layout.nb_channels;
            }
        }

        return r;
    }

    double nativeProbeDuration(const fs::path &filePath)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();
        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return 0;
        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return 0;

        if (fmt.ctx->duration > 0)
            return fmt.ctx->duration / (double)AV_TIME_BASE;

        // Fallback: check individual streams
        for (unsigned i = 0; i < fmt.ctx->nb_streams; ++i)
        {
            auto *st = fmt.ctx->streams[i];
            if (st->duration > 0 && st->time_base.den > 0)
                return st->duration * av_q2d(st->time_base);
        }
        return 0;
    }

    bool nativeProbeOk(const fs::path &filePath)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();
        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return false;
        return fmt.ctx->nb_streams > 0;
    }

    // ── Native remux (stream copy) ──────────────────────────────────

    bool nativeRemux(const fs::path &input, const fs::path &output,
                     const fs::path &workDir,
                     NativeProgressCb progress)
    {
        FmtCtxIn ifmt;
        std::string inPath = input.string();

        AVDictionary *inOpts = nullptr;
        av_dict_set(&inOpts, "fflags", "+genpts+discardcorrupt", 0);
        av_dict_set(&inOpts, "err_detect", "+ignore_err", 0);

        if (avformat_open_input(&ifmt.ctx, inPath.c_str(), nullptr, &inOpts) < 0)
        {
            av_dict_free(&inOpts);
            return false;
        }
        av_dict_free(&inOpts);

        if (avformat_find_stream_info(ifmt.ctx, nullptr) < 0)
            return false;

        // Create output
        FmtCtxOut ofmt;
        std::string outPath = output.string();
        if (avformat_alloc_output_context2(&ofmt.ctx, nullptr, "matroska", outPath.c_str()) < 0)
            return false;

        // Map streams (video + audio only)
        std::vector<int> streamMap(ifmt.ctx->nb_streams, -1);
        int outIdx = 0;
        for (unsigned i = 0; i < ifmt.ctx->nb_streams; ++i)
        {
            auto type = ifmt.ctx->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            {
                AVStream *outStream = avformat_new_stream(ofmt.ctx, nullptr);
                if (!outStream)
                    return false;
                avcodec_parameters_copy(outStream->codecpar, ifmt.ctx->streams[i]->codecpar);
                outStream->codecpar->codec_tag = 0;
                streamMap[i] = outIdx++;
            }
        }

        // Open output file
        if (!(ofmt.ctx->oformat->flags & AVFMT_NOFILE))
        {
            if (avio_open(&ofmt.ctx->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
                return false;
            ofmt.pbOpened = true;
        }

        // Write header with MKV-friendly options
        // avoid_negative_ts: let the muxer handle truly negative timestamps
        // (preserves HEVC B-frame CTO instead of manual clamping)
        AVDictionary *outOpts = nullptr;
        av_dict_set(&outOpts, "max_interleave_delta", "0", 0);
        ofmt.ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
        if (avformat_write_header(ofmt.ctx, &outOpts) < 0)
        {
            av_dict_free(&outOpts);
            return false;
        }
        av_dict_free(&outOpts);

        // Per-stream DTS offsets (dynamic map — no fixed-size limit)
        std::unordered_map<int, int64_t> dtsOffset;
        std::unordered_map<int, bool> dtsOffsetSet;

        // Copy packets — use av_write_frame for direct disk writes (no RAM buffer).
        // av_write_frame does NOT take ownership of pkt, so we unref it ourselves.
        AVPacket *pkt = av_packet_alloc();
        int64_t bytesWritten = 0;
        int64_t totalSize = 0;
        try
        {
            totalSize = (int64_t)fs::file_size(input);
        }
        catch (...)
        {
        }

        while (av_read_frame(ifmt.ctx, pkt) >= 0)
        {
            int srcIdx = pkt->stream_index;
            if (srcIdx < 0 || srcIdx >= (int)streamMap.size() || streamMap[srcIdx] < 0)
            {
                av_packet_unref(pkt);
                continue;
            }

            int dstIdx = streamMap[srcIdx];
            if (dstIdx >= (int)ofmt.ctx->nb_streams)
            {
                av_packet_unref(pkt);
                continue;
            }

            // Record first DTS for offset calculation
            if (!dtsOffsetSet[srcIdx] && pkt->dts != AV_NOPTS_VALUE)
            {
                dtsOffset[srcIdx] = pkt->dts;
                dtsOffsetSet[srcIdx] = true;
            }

            // Normalize timestamps: subtract initial offset so stream starts near 0.
            if (dtsOffsetSet[srcIdx])
            {
                if (pkt->dts != AV_NOPTS_VALUE)
                    pkt->dts -= dtsOffset[srcIdx];
                if (pkt->pts != AV_NOPTS_VALUE)
                    pkt->pts -= dtsOffset[srcIdx];
            }

            // Rescale timestamps to output timebase
            AVRational inTb = ifmt.ctx->streams[srcIdx]->time_base;
            AVRational outTb = ofmt.ctx->streams[dstIdx]->time_base;
            pkt->stream_index = dstIdx;
            av_packet_rescale_ts(pkt, inTb, outTb);

            // Grab fields before write
            int64_t pktDts = pkt->dts;
            int pktSize = pkt->size;

            // Direct write — data goes to muxer immediately, no interleave buffer
            int ret = av_write_frame(ofmt.ctx, pkt);
            av_packet_unref(pkt); // av_write_frame does NOT unref
            if (ret < 0)
                continue;

            bytesWritten += pktSize;

            if (progress && pktDts != AV_NOPTS_VALUE)
            {
                double timeSec = pktDts * av_q2d(outTb);
                progress(timeSec, bytesWritten, totalSize);
            }
        }

        av_packet_free(&pkt);

        // Signal finalization stage
        if (progress)
            progress(-1.0, bytesWritten, totalSize);

        if (av_write_trailer(ofmt.ctx) < 0)
            return false;

        return true;
    }

    // ── Native concat ────────────────────────────────────────────────

    bool nativeConcat(const fs::path &concatListFile, const fs::path &output,
                      const fs::path &workDir,
                      NativeProgressCb progress,
                      NativeCancelCb cancelCb)
    {
        // The caller (writeConcat) already writes absolute paths with forward
        // slashes, so the concat demuxer never needs relative path resolution.
        // No temp file rewrite needed — fully thread-safe.

        FmtCtxIn ifmt;
        std::string listPath = concatListFile.string();

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "safe", "0", 0);

        const AVInputFormat *concatFmt = av_find_input_format("concat");
        if (!concatFmt)
        {
            av_dict_free(&opts);
            return false;
        }

        if (avformat_open_input(&ifmt.ctx, listPath.c_str(), concatFmt, &opts) < 0)
        {
            av_dict_free(&opts);
            return false;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(ifmt.ctx, nullptr) < 0)
            return false;

        // Create output
        FmtCtxOut ofmt;
        std::string outPath = output.string();
        if (avformat_alloc_output_context2(&ofmt.ctx, nullptr, "matroska", outPath.c_str()) < 0)
        {
            return false;
        }

        // Map all streams
        std::vector<int> streamMap(ifmt.ctx->nb_streams, -1);
        int outIdx = 0;
        for (unsigned i = 0; i < ifmt.ctx->nb_streams; ++i)
        {
            auto type = ifmt.ctx->streams[i]->codecpar->codec_type;
            if (type == AVMEDIA_TYPE_VIDEO || type == AVMEDIA_TYPE_AUDIO)
            {
                AVStream *outStream = avformat_new_stream(ofmt.ctx, nullptr);
                if (!outStream)
                {
                    return false;
                }
                avcodec_parameters_copy(outStream->codecpar, ifmt.ctx->streams[i]->codecpar);
                outStream->codecpar->codec_tag = 0;
                streamMap[i] = outIdx++;
            }
        }

        // Open output file
        if (!(ofmt.ctx->oformat->flags & AVFMT_NOFILE))
        {
            if (avio_open(&ofmt.ctx->pb, outPath.c_str(), AVIO_FLAG_WRITE) < 0)
            {
                return false;
            }
            ofmt.pbOpened = true;
        }

        // Write header with timestamp normalization
        AVDictionary *outOpts = nullptr;
        av_dict_set(&outOpts, "max_interleave_delta", "0", 0);
        ofmt.ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_ZERO;
        if (avformat_write_header(ofmt.ctx, &outOpts) < 0)
        {
            av_dict_free(&outOpts);
            return false;
        }
        av_dict_free(&outOpts);

        // Per-stream DTS monotonicity tracking to drop out-of-order packets
        // at concat segment boundaries
        std::unordered_map<int, int64_t> lastDts;

        // Copy packets — use av_write_frame for direct disk writes (no RAM buffer).
        AVPacket *pkt = av_packet_alloc();
        int64_t bytesWritten = 0;
        int pktCount = 0;

        while (av_read_frame(ifmt.ctx, pkt) >= 0)
        {
            // Check for cancellation every ~100 packets
            if (cancelCb && (++pktCount % 100) == 0 && cancelCb())
            {
                av_packet_free(&pkt);
                return false;
            }

            int srcIdx = pkt->stream_index;
            if (srcIdx < 0 || srcIdx >= (int)streamMap.size() || streamMap[srcIdx] < 0)
            {
                av_packet_unref(pkt);
                continue;
            }

            int dstIdx = streamMap[srcIdx];
            if (dstIdx >= (int)ofmt.ctx->nb_streams)
            {
                av_packet_unref(pkt);
                continue;
            }

            // Rescale timestamps
            AVRational inTb = ifmt.ctx->streams[srcIdx]->time_base;
            AVRational outTb = ofmt.ctx->streams[dstIdx]->time_base;
            pkt->stream_index = dstIdx;
            av_packet_rescale_ts(pkt, inTb, outTb);

            // Enforce DTS monotonicity across concat segment boundaries
            if (pkt->dts != AV_NOPTS_VALUE)
            {
                auto it = lastDts.find(dstIdx);
                if (it != lastDts.end() && pkt->dts <= it->second)
                {
                    av_packet_unref(pkt);
                    continue;
                }
                lastDts[dstIdx] = pkt->dts;
            }

            // Grab fields before write
            int64_t pktDts = pkt->dts;
            int pktSize = pkt->size;

            // Direct write — data goes to muxer immediately, no interleave buffer
            av_write_frame(ofmt.ctx, pkt);
            av_packet_unref(pkt); // av_write_frame does NOT unref

            bytesWritten += pktSize;

            if (progress && pktDts != AV_NOPTS_VALUE)
            {
                double timeSec = pktDts * av_q2d(outTb);
                progress(timeSec, bytesWritten, 0);
            }
        }

        av_packet_free(&pkt);

        // Signal finalization stage
        if (progress)
            progress(-1.0, bytesWritten, 0);

        av_write_trailer(ofmt.ctx);

        return true;
    }

    // ── PTS analysis ─────────────────────────────────────────────────

    double nativeDetectMaxPtsJump(const fs::path &filePath)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();

        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return 0;
        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return 0;

        // Find video stream
        int videoIdx = av_find_best_stream(fmt.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIdx < 0)
            return 0;

        AVRational tb = fmt.ctx->streams[videoIdx]->time_base;

        // Sample packets at intervals to detect PTS jumps
        double duration = 0;
        if (fmt.ctx->duration > 0)
            duration = fmt.ctx->duration / (double)AV_TIME_BASE;
        if (duration <= 0)
            return 0;

        // Sample at ~20 evenly spaced points through the file, 100 packets each.
        // This gives ~2000 sample points — enough to catch PTS discontinuities
        // that sparse sampling might miss in long recordings.
        int numSamples = 20;
        double interval = duration / (numSamples + 1);
        double maxJump = 0;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            double seekTime = (sample + 1) * interval;
            int64_t seekTs = (int64_t)(seekTime / av_q2d(tb));

            if (av_seek_frame(fmt.ctx, videoIdx, seekTs, AVSEEK_FLAG_BACKWARD) < 0)
                continue;

            // Read a window of packets and check for PTS jumps
            double prevPts = -1;
            int pktsRead = 0;
            AVPacket *pkt = av_packet_alloc();

            while (pktsRead < 100)
            {
                if (av_read_frame(fmt.ctx, pkt) < 0)
                    break;

                if (pkt->stream_index == videoIdx && pkt->pts != AV_NOPTS_VALUE)
                {
                    double pts = pkt->pts * av_q2d(tb);
                    if (prevPts >= 0)
                    {
                        double jump = std::abs(pts - prevPts);
                        if (jump > maxJump)
                            maxJump = jump;
                    }
                    prevPts = pts;
                    pktsRead++;
                }
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }

        return maxJump;
    }

    // ── Stream-level duration ────────────────────────────────────────
    // Replaces ffprobeStreamDuration() — returns the duration of a
    // specific stream ("v:0" for first video, "a:0" for first audio).

    double nativeStreamDuration(const fs::path &filePath, const std::string &sel)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();
        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return 0;
        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return 0;

        // Parse selector: "v:0" → video, "a:0" → audio
        AVMediaType targetType = AVMEDIA_TYPE_UNKNOWN;
        int targetIdx = 0;
        if (!sel.empty())
        {
            char typeChar = sel[0];
            if (typeChar == 'v' || typeChar == 'V')
                targetType = AVMEDIA_TYPE_VIDEO;
            else if (typeChar == 'a' || typeChar == 'A')
                targetType = AVMEDIA_TYPE_AUDIO;

            auto colonPos = sel.find(':');
            if (colonPos != std::string::npos && colonPos + 1 < sel.size())
            {
                try
                {
                    targetIdx = std::stoi(sel.substr(colonPos + 1));
                }
                catch (...)
                {
                }
            }
        }

        int matchCount = 0;
        for (unsigned i = 0; i < fmt.ctx->nb_streams; ++i)
        {
            auto *st = fmt.ctx->streams[i];
            if (st->codecpar->codec_type == targetType)
            {
                if (matchCount == targetIdx)
                {
                    if (st->duration > 0 && st->time_base.den > 0)
                        return st->duration * av_q2d(st->time_base);
                    // Fallback to container duration
                    if (fmt.ctx->duration > 0)
                        return fmt.ctx->duration / (double)AV_TIME_BASE;
                    return 0;
                }
                ++matchCount;
            }
        }
        return 0;
    }

    // ── Last packet PTS ──────────────────────────────────────────────
    // Replaces ffprobeLastPacketPts() — reads all packets to find
    // the maximum PTS across video and audio streams.

    double nativeLastPacketPts(const fs::path &filePath)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "fflags", "+discardcorrupt", 0);

        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, &opts) < 0)
        {
            av_dict_free(&opts);
            return 0;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return 0;

        // Find video and audio stream indices
        int videoIdx = av_find_best_stream(fmt.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        int audioIdx = av_find_best_stream(fmt.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

        double maxPts = 0;
        AVPacket *pkt = av_packet_alloc();

        while (av_read_frame(fmt.ctx, pkt) >= 0)
        {
            if (pkt->pts != AV_NOPTS_VALUE)
            {
                int idx = pkt->stream_index;
                if (idx == videoIdx || idx == audioIdx)
                {
                    double pts = pkt->pts * av_q2d(fmt.ctx->streams[idx]->time_base);
                    if (pts > maxPts)
                        maxPts = pts;
                }
            }
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);
        return maxPts;
    }

    // ── Frame count → duration ───────────────────────────────────────
    // Replaces ffprobeFramesDuration() — counts all video packets
    // (no decode needed, just demux) and divides by FPS.

    double nativeFrameCount(const fs::path &filePath)
    {
        FmtCtxIn fmt;
        std::string path = filePath.string();
        if (avformat_open_input(&fmt.ctx, path.c_str(), nullptr, nullptr) < 0)
            return 0;
        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
            return 0;

        int videoIdx = av_find_best_stream(fmt.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (videoIdx < 0)
            return 0;

        // Get FPS
        auto *st = fmt.ctx->streams[videoIdx];
        double fps = 0;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
            fps = av_q2d(st->avg_frame_rate);
        else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
            fps = av_q2d(st->r_frame_rate);

        if (fps <= 0)
            return 0;

        // Count video packets (frames)
        int64_t frameCount = 0;
        AVPacket *pkt = av_packet_alloc();

        while (av_read_frame(fmt.ctx, pkt) >= 0)
        {
            if (pkt->stream_index == videoIdx)
                ++frameCount;
            av_packet_unref(pkt);
        }

        av_packet_free(&pkt);

        if (frameCount <= 0)
            return 0;

        return static_cast<double>(frameCount) / fps;
    }

    // ── Encoder availability ─────────────────────────────────────────
    // Direct avcodec lookup — no subprocess needed.

    bool nativeEncoderAvailable(const std::string &name)
    {
        const AVCodec *codec = avcodec_find_encoder_by_name(name.c_str());
        return codec != nullptr;
    }

    // ── CUDA hardware acceleration ───────────────────────────────────
    // Try to create a CUDA device context. If the driver/GPU is missing
    // this returns false instantly (no subprocess, no console flash).

    bool nativeCudaAvailable()
    {
        static std::atomic<int> cached{-1};
        int val = cached.load(std::memory_order_acquire);
        if (val >= 0)
            return val == 1;

        AVBufferRef *hwCtx = nullptr;
        int ret = av_hwdevice_ctx_create(&hwCtx, AV_HWDEVICE_TYPE_CUDA,
                                         nullptr, nullptr, 0);
        bool ok = (ret >= 0);
        if (hwCtx)
            av_buffer_unref(&hwCtx);

        cached.store(ok ? 1 : 0, std::memory_order_release);
        return ok;
    }

} // namespace sh
