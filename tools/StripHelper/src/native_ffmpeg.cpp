// ─────────────────────────────────────────────────────────────────
// StripHelper — Native FFmpeg operations implementation
// Direct libavformat/libavcodec API for probing, remux, concat.
// ─────────────────────────────────────────────────────────────────

#include "native_ffmpeg.h"
#include <algorithm>
#include <fstream>
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

        // Copy packets
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
            // Use streamMap.size() — nb_streams could theoretically change
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
            // NOTE: Do NOT clamp DTS <= PTS or force non-negative here!
            // HEVC B-frames legitimately have DTS > PTS (negative composition
            // time offset). The muxer's avoid_negative_ts = MAKE_ZERO handles
            // any truly negative timestamps at write time. Manually forcing
            // DTS = PTS would destroy B-frame display order and corrupt seeking.
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

            bytesWritten += pkt->size;

            if (av_interleaved_write_frame(ofmt.ctx, pkt) < 0)
            {
                av_packet_unref(pkt);
                // Non-fatal: skip problematic packets
                continue;
            }

            // Progress callback
            if (progress && pkt->dts != AV_NOPTS_VALUE)
            {
                double timeSec = pkt->dts * av_q2d(outTb);
                progress(timeSec, bytesWritten, totalSize);
            }
        }

        av_packet_free(&pkt);

        if (av_write_trailer(ofmt.ctx) < 0)
            return false;

        return true;
    }

    // ── Native concat ────────────────────────────────────────────────

    bool nativeConcat(const fs::path &concatListFile, const fs::path &output,
                      const fs::path &workDir,
                      NativeProgressCb progress)
    {
        // Use FFmpeg's built-in concat demuxer
        FmtCtxIn ifmt;
        std::string listPath = concatListFile.string();

        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "safe", "0", 0);

        // The concat demuxer needs the working directory for relative paths
        const AVInputFormat *concatFmt = av_find_input_format("concat");
        if (!concatFmt)
        {
            av_dict_free(&opts);
            return false;
        }

        // Change to work directory for relative paths in concat list
        auto prevDir = fs::current_path();
        fs::current_path(workDir);

        if (avformat_open_input(&ifmt.ctx, listPath.c_str(), concatFmt, &opts) < 0)
        {
            av_dict_free(&opts);
            fs::current_path(prevDir);
            return false;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(ifmt.ctx, nullptr) < 0)
        {
            fs::current_path(prevDir);
            return false;
        }

        // Create output
        FmtCtxOut ofmt;
        std::string outPath = output.string();
        if (avformat_alloc_output_context2(&ofmt.ctx, nullptr, "matroska", outPath.c_str()) < 0)
        {
            fs::current_path(prevDir);
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
                    fs::current_path(prevDir);
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
                fs::current_path(prevDir);
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
            fs::current_path(prevDir);
            return false;
        }
        av_dict_free(&outOpts);

        // Per-stream DTS monotonicity tracking to drop out-of-order packets
        // at concat segment boundaries
        std::unordered_map<int, int64_t> lastDts;

        // Copy packets
        AVPacket *pkt = av_packet_alloc();
        int64_t bytesWritten = 0;

        while (av_read_frame(ifmt.ctx, pkt) >= 0)
        {
            int srcIdx = pkt->stream_index;
            // Use streamMap.size() for bounds — the concat demuxer can
            // dynamically add streams from later segments, growing
            // nb_streams beyond our original map.  Guard against OOB.
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

            // Enforce DTS monotonicity across concat segment boundaries.
            // The concat demuxer normally handles this, but edge cases
            // (truncated segments, timestamp wraps) can produce non-monotonic
            // DTS which corrupts the output container's index.
            if (pkt->dts != AV_NOPTS_VALUE)
            {
                auto it = lastDts.find(dstIdx);
                if (it != lastDts.end() && pkt->dts <= it->second)
                {
                    av_packet_unref(pkt);
                    continue; // Drop non-monotonic packet
                }
                lastDts[dstIdx] = pkt->dts;
            }

            bytesWritten += pkt->size;

            av_interleaved_write_frame(ofmt.ctx, pkt);
            // Don't check return — some packets may fail in concat

            if (progress && pkt->dts != AV_NOPTS_VALUE)
            {
                double timeSec = pkt->dts * av_q2d(outTb);
                progress(timeSec, bytesWritten, 0);
            }
        }

        av_packet_free(&pkt);
        av_write_trailer(ofmt.ctx);

        fs::current_path(prevDir);
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

} // namespace sh
