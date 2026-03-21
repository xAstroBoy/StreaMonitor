// ─────────────────────────────────────────────────────────────────
// Video Contact Sheet (Thumbnail) Generator — Implementation
// Uses native FFmpeg API: libavformat, libavcodec, libswscale
// ─────────────────────────────────────────────────────────────────

#include "utils/thumbnail_generator.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

extern "C"
{
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace sm
{
    namespace
    {
        // ── Embedded 5×8 bitmap font ────────────────────────────────
        // Covers ASCII 32–127. Each glyph is 8 rows × 5 columns.
        // Stored as 8 bytes per glyph; bits 7–3 hold the 5 columns.
        // clang-format off
        static constexpr uint8_t g_font[96][8] = {
            // 32 ' '
            {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
            // 33 '!'
            {0x20,0x20,0x20,0x20,0x20,0x00,0x20,0x00},
            // 34 '"'
            {0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x00},
            // 35 '#'
            {0x50,0xF8,0x50,0x50,0xF8,0x50,0x00,0x00},
            // 36 '$'
            {0x20,0x78,0xA0,0x70,0x28,0xF0,0x20,0x00},
            // 37 '%'
            {0xC8,0xD0,0x10,0x20,0x40,0x58,0x98,0x00},
            // 38 '&'
            {0x40,0xA0,0xA0,0x40,0xA8,0x90,0x68,0x00},
            // 39 '\''
            {0x20,0x20,0x00,0x00,0x00,0x00,0x00,0x00},
            // 40 '('
            {0x10,0x20,0x40,0x40,0x40,0x20,0x10,0x00},
            // 41 ')'
            {0x40,0x20,0x10,0x10,0x10,0x20,0x40,0x00},
            // 42 '*'
            {0x00,0xA8,0x70,0xF8,0x70,0xA8,0x00,0x00},
            // 43 '+'
            {0x00,0x20,0x20,0xF8,0x20,0x20,0x00,0x00},
            // 44 ','
            {0x00,0x00,0x00,0x00,0x00,0x20,0x20,0x40},
            // 45 '-'
            {0x00,0x00,0x00,0xF8,0x00,0x00,0x00,0x00},
            // 46 '.'
            {0x00,0x00,0x00,0x00,0x00,0x00,0x20,0x00},
            // 47 '/'
            {0x08,0x08,0x10,0x20,0x40,0x80,0x80,0x00},
            // 48 '0'
            {0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0x00},
            // 49 '1'
            {0x20,0x60,0x20,0x20,0x20,0x20,0x70,0x00},
            // 50 '2'
            {0x70,0x88,0x08,0x30,0x40,0x80,0xF8,0x00},
            // 51 '3'
            {0x70,0x88,0x08,0x30,0x08,0x88,0x70,0x00},
            // 52 '4'
            {0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0x00},
            // 53 '5'
            {0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0x00},
            // 54 '6'
            {0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0x00},
            // 55 '7'
            {0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0x00},
            // 56 '8'
            {0x70,0x88,0x88,0x70,0x88,0x88,0x70,0x00},
            // 57 '9'
            {0x70,0x88,0x88,0x78,0x08,0x10,0x60,0x00},
            // 58 ':'
            {0x00,0x00,0x20,0x00,0x00,0x20,0x00,0x00},
            // 59 ';'
            {0x00,0x00,0x20,0x00,0x00,0x20,0x20,0x40},
            // 60 '<'
            {0x08,0x10,0x20,0x40,0x20,0x10,0x08,0x00},
            // 61 '='
            {0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0x00},
            // 62 '>'
            {0x80,0x40,0x20,0x10,0x20,0x40,0x80,0x00},
            // 63 '?'
            {0x70,0x88,0x08,0x10,0x20,0x00,0x20,0x00},
            // 64 '@'
            {0x70,0x88,0xB8,0xA8,0xB8,0x80,0x70,0x00},
            // 65 'A'
            {0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},
            // 66 'B'
            {0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0x00},
            // 67 'C'
            {0x70,0x88,0x80,0x80,0x80,0x88,0x70,0x00},
            // 68 'D'
            {0xF0,0x88,0x88,0x88,0x88,0x88,0xF0,0x00},
            // 69 'E'
            {0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0x00},
            // 70 'F'
            {0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0x00},
            // 71 'G'
            {0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0x00},
            // 72 'H'
            {0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0x00},
            // 73 'I'
            {0x70,0x20,0x20,0x20,0x20,0x20,0x70,0x00},
            // 74 'J'
            {0x38,0x10,0x10,0x10,0x10,0x90,0x60,0x00},
            // 75 'K'
            {0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0x00},
            // 76 'L'
            {0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0x00},
            // 77 'M'
            {0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0x00},
            // 78 'N'
            {0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0x00},
            // 79 'O'
            {0x70,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
            // 80 'P'
            {0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0x00},
            // 81 'Q'
            {0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0x00},
            // 82 'R'
            {0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0x00},
            // 83 'S'
            {0x70,0x88,0x80,0x70,0x08,0x88,0x70,0x00},
            // 84 'T'
            {0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0x00},
            // 85 'U'
            {0x88,0x88,0x88,0x88,0x88,0x88,0x70,0x00},
            // 86 'V'
            {0x88,0x88,0x88,0x50,0x50,0x20,0x20,0x00},
            // 87 'W'
            {0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88,0x00},
            // 88 'X'
            {0x88,0x88,0x50,0x20,0x50,0x88,0x88,0x00},
            // 89 'Y'
            {0x88,0x88,0x50,0x20,0x20,0x20,0x20,0x00},
            // 90 'Z'
            {0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0x00},
            // 91 '['
            {0x70,0x40,0x40,0x40,0x40,0x40,0x70,0x00},
            // 92 '\\'
            {0x80,0x80,0x40,0x20,0x10,0x08,0x08,0x00},
            // 93 ']'
            {0x70,0x10,0x10,0x10,0x10,0x10,0x70,0x00},
            // 94 '^'
            {0x20,0x50,0x88,0x00,0x00,0x00,0x00,0x00},
            // 95 '_'
            {0x00,0x00,0x00,0x00,0x00,0x00,0xF8,0x00},
            // 96 '`'
            {0x40,0x20,0x00,0x00,0x00,0x00,0x00,0x00},
            // 97 'a'
            {0x00,0x00,0x70,0x08,0x78,0x88,0x78,0x00},
            // 98 'b'
            {0x80,0x80,0xF0,0x88,0x88,0x88,0xF0,0x00},
            // 99 'c'
            {0x00,0x00,0x70,0x80,0x80,0x80,0x70,0x00},
            // 100 'd'
            {0x08,0x08,0x78,0x88,0x88,0x88,0x78,0x00},
            // 101 'e'
            {0x00,0x00,0x70,0x88,0xF8,0x80,0x70,0x00},
            // 102 'f'
            {0x30,0x48,0x40,0xF0,0x40,0x40,0x40,0x00},
            // 103 'g'
            {0x00,0x00,0x78,0x88,0x78,0x08,0x70,0x00},
            // 104 'h'
            {0x80,0x80,0xF0,0x88,0x88,0x88,0x88,0x00},
            // 105 'i'
            {0x20,0x00,0x60,0x20,0x20,0x20,0x70,0x00},
            // 106 'j'
            {0x10,0x00,0x30,0x10,0x10,0x90,0x60,0x00},
            // 107 'k'
            {0x80,0x80,0x90,0xA0,0xC0,0xA0,0x90,0x00},
            // 108 'l'
            {0x60,0x20,0x20,0x20,0x20,0x20,0x70,0x00},
            // 109 'm'
            {0x00,0x00,0xD0,0xA8,0xA8,0xA8,0xA8,0x00},
            // 110 'n'
            {0x00,0x00,0xF0,0x88,0x88,0x88,0x88,0x00},
            // 111 'o'
            {0x00,0x00,0x70,0x88,0x88,0x88,0x70,0x00},
            // 112 'p'
            {0x00,0x00,0xF0,0x88,0xF0,0x80,0x80,0x00},
            // 113 'q'
            {0x00,0x00,0x78,0x88,0x78,0x08,0x08,0x00},
            // 114 'r'
            {0x00,0x00,0xB0,0xC8,0x80,0x80,0x80,0x00},
            // 115 's'
            {0x00,0x00,0x78,0x80,0x70,0x08,0xF0,0x00},
            // 116 't'
            {0x40,0x40,0xF0,0x40,0x40,0x48,0x30,0x00},
            // 117 'u'
            {0x00,0x00,0x88,0x88,0x88,0x88,0x78,0x00},
            // 118 'v'
            {0x00,0x00,0x88,0x88,0x50,0x50,0x20,0x00},
            // 119 'w'
            {0x00,0x00,0x88,0xA8,0xA8,0xA8,0x50,0x00},
            // 120 'x'
            {0x00,0x00,0x88,0x50,0x20,0x50,0x88,0x00},
            // 121 'y'
            {0x00,0x00,0x88,0x88,0x78,0x08,0x70,0x00},
            // 122 'z'
            {0x00,0x00,0xF8,0x10,0x20,0x40,0xF8,0x00},
            // 123 '{'
            {0x18,0x20,0x20,0xC0,0x20,0x20,0x18,0x00},
            // 124 '|'
            {0x20,0x20,0x20,0x20,0x20,0x20,0x20,0x00},
            // 125 '}'
            {0xC0,0x20,0x20,0x18,0x20,0x20,0xC0,0x00},
            // 126 '~'
            {0x48,0xA8,0x90,0x00,0x00,0x00,0x00,0x00},
            // 127 DEL (blank)
            {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
        };
        // clang-format on

        // Draw one character on an RGB24 buffer with optional scale and shadow
        static void drawChar(uint8_t *buf, int stride, int imgW, int imgH,
                             int x, int y, char c,
                             uint8_t r, uint8_t g, uint8_t b, int scale = 2)
        {
            int idx = static_cast<unsigned char>(c) - 32;
            if (idx < 0 || idx >= 96)
                return;
            const auto &glyph = g_font[idx];
            for (int row = 0; row < 8; ++row)
            {
                uint8_t bits = glyph[row];
                for (int col = 0; col < 5; ++col)
                {
                    if (bits & (0x80 >> col))
                    {
                        for (int sy = 0; sy < scale; ++sy)
                        {
                            for (int sx = 0; sx < scale; ++sx)
                            {
                                int px = x + col * scale + sx;
                                int py = y + row * scale + sy;
                                if (px >= 0 && px < imgW && py >= 0 && py < imgH)
                                {
                                    uint8_t *p = buf + py * stride + px * 3;
                                    p[0] = r;
                                    p[1] = g;
                                    p[2] = b;
                                }
                            }
                        }
                    }
                }
            }
        }

        // Draw a string with shadow
        static void drawText(uint8_t *buf, int stride, int imgW, int imgH,
                             int x, int y, const std::string &text,
                             uint8_t r, uint8_t g, uint8_t b, int scale = 2)
        {
            const int charW = 5 * scale + scale; // 5 cols + 1 gap
            // Shadow pass
            for (size_t i = 0; i < text.size(); ++i)
                drawChar(buf, stride, imgW, imgH,
                         x + (int)i * charW + 1, y + 1, text[i], 0, 0, 0, scale);
            // Foreground pass
            for (size_t i = 0; i < text.size(); ++i)
                drawChar(buf, stride, imgW, imgH,
                         x + (int)i * charW, y, text[i], r, g, b, scale);
        }

        // Fill a rectangle in the RGB24 buffer
        static void fillRect(uint8_t *buf, int stride, int imgW, int imgH,
                             int x0, int y0, int w, int h,
                             uint8_t r, uint8_t g, uint8_t b)
        {
            for (int y = y0; y < y0 + h && y < imgH; ++y)
            {
                if (y < 0)
                    continue;
                for (int x = x0; x < x0 + w && x < imgW; ++x)
                {
                    if (x < 0)
                        continue;
                    uint8_t *p = buf + y * stride + x * 3;
                    p[0] = r;
                    p[1] = g;
                    p[2] = b;
                }
            }
        }

        // Alpha-blend a semi-transparent rectangle (darken)
        static void darkenRect(uint8_t *buf, int stride, int imgW, int imgH,
                               int x0, int y0, int w, int h, float alpha = 0.55f)
        {
            float inv = 1.0f - alpha;
            for (int y = y0; y < y0 + h && y < imgH; ++y)
            {
                if (y < 0)
                    continue;
                for (int x = x0; x < x0 + w && x < imgW; ++x)
                {
                    if (x < 0)
                        continue;
                    uint8_t *p = buf + y * stride + x * 3;
                    p[0] = (uint8_t)(p[0] * inv);
                    p[1] = (uint8_t)(p[1] * inv);
                    p[2] = (uint8_t)(p[2] * inv);
                }
            }
        }

        // Format seconds to HH:MM:SS
        static std::string formatTime(double seconds)
        {
            int total = (int)std::round(seconds);
            if (total < 0)
                total = 0;
            int h = total / 3600;
            int m = (total % 3600) / 60;
            int s = total % 60;
            std::ostringstream oss;
            if (h > 0)
                oss << h << ":" << std::setfill('0') << std::setw(2) << m << ":" << std::setw(2) << s;
            else
                oss << m << ":" << std::setfill('0') << std::setw(2) << s;
            return oss.str();
        }

        // RAII wrapper for AVFormatContext (input)
        struct FormatCtx
        {
            AVFormatContext *ctx = nullptr;
            ~FormatCtx()
            {
                if (ctx)
                    avformat_close_input(&ctx);
            }
        };

        // RAII wrapper for AVCodecContext
        struct CodecCtx
        {
            AVCodecContext *ctx = nullptr;
            ~CodecCtx()
            {
                if (ctx)
                    avcodec_free_context(&ctx);
            }
        };

        // RAII wrapper for SwsContext
        struct SwsCtx
        {
            SwsContext *ctx = nullptr;
            ~SwsCtx()
            {
                if (ctx)
                    sws_freeContext(ctx);
            }
        };

        // RAII wrapper for AVFrame
        struct Frame
        {
            AVFrame *f = nullptr;
            Frame() : f(av_frame_alloc()) {}
            ~Frame()
            {
                if (f)
                    av_frame_free(&f);
            }
        };

        // RAII wrapper for AVPacket
        struct Packet
        {
            AVPacket *p = nullptr;
            Packet() : p(av_packet_alloc()) {}
            ~Packet()
            {
                if (p)
                    av_packet_free(&p);
            }
        };

        // Decode one frame near the target timestamp
        static bool seekAndDecode(AVFormatContext *fmtCtx, AVCodecContext *decCtx,
                                  int streamIdx, int64_t targetTs,
                                  AVFrame *outFrame)
        {
            // Seek to just before the target
            int flags = AVSEEK_FLAG_BACKWARD;
            if (av_seek_frame(fmtCtx, streamIdx, targetTs, flags) < 0)
            {
                // Try seeking from start if backward fails
                av_seek_frame(fmtCtx, -1, 0, AVSEEK_FLAG_BYTE);
            }
            avcodec_flush_buffers(decCtx);

            Packet pkt;
            int maxAttempts = 500; // Safety limit
            while (maxAttempts-- > 0)
            {
                int ret = av_read_frame(fmtCtx, pkt.p);
                if (ret < 0)
                    break;

                if (pkt.p->stream_index != streamIdx)
                {
                    av_packet_unref(pkt.p);
                    continue;
                }

                ret = avcodec_send_packet(decCtx, pkt.p);
                av_packet_unref(pkt.p);
                if (ret < 0)
                    continue;

                ret = avcodec_receive_frame(decCtx, outFrame);
                if (ret == 0)
                    return true;
            }

            // Flush decoder
            avcodec_send_packet(decCtx, nullptr);
            if (avcodec_receive_frame(decCtx, outFrame) == 0)
                return true;

            return false;
        }

        // Encode the composite RGB frame as JPEG
        static bool encodeJpeg(const uint8_t *rgbData, int width, int height, int stride,
                               const std::string &outputPath, int quality)
        {
            // Find MJPEG encoder
            const AVCodec *mjpegCodec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
            if (!mjpegCodec)
                return false;

            CodecCtx encCtx;
            encCtx.ctx = avcodec_alloc_context3(mjpegCodec);
            if (!encCtx.ctx)
                return false;

            encCtx.ctx->width = width;
            encCtx.ctx->height = height;
            encCtx.ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
            encCtx.ctx->time_base = {1, 25};
            encCtx.ctx->flags |= AV_CODEC_FLAG_QSCALE;
            encCtx.ctx->global_quality = quality * FF_QP2LAMBDA;

            if (avcodec_open2(encCtx.ctx, mjpegCodec, nullptr) < 0)
                return false;

            // Convert RGB24 → YUVJ420P
            SwsCtx sws;
            sws.ctx = sws_getContext(width, height, AV_PIX_FMT_RGB24,
                                     width, height, AV_PIX_FMT_YUVJ420P,
                                     SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!sws.ctx)
                return false;

            Frame yuvFrame;
            yuvFrame.f->format = AV_PIX_FMT_YUVJ420P;
            yuvFrame.f->width = width;
            yuvFrame.f->height = height;
            if (av_frame_get_buffer(yuvFrame.f, 32) < 0)
                return false;

            const uint8_t *srcSlice[1] = {rgbData};
            int srcStride[1] = {stride};
            sws_scale(sws.ctx, srcSlice, srcStride, 0, height,
                      yuvFrame.f->data, yuvFrame.f->linesize);

            yuvFrame.f->pts = 0;

            // Encode
            if (avcodec_send_frame(encCtx.ctx, yuvFrame.f) < 0)
                return false;

            Packet pkt;
            if (avcodec_receive_packet(encCtx.ctx, pkt.p) < 0)
                return false;

            // Write raw JPEG data to file
            std::ofstream out(outputPath, std::ios::binary);
            if (!out)
                return false;
            out.write(reinterpret_cast<const char *>(pkt.p->data), pkt.p->size);
            return out.good();
        }

    } // anonymous namespace

    bool generateContactSheet(
        const std::string &videoPath,
        const std::string &outputPath,
        const ThumbnailConfig &cfg,
        std::function<void(const std::string &)> logCb)
    {
        auto log = [&](const std::string &msg)
        {
            if (logCb)
                logCb(msg);
        };

        // ── 1. Open input video ──────────────────────────────────────────
        FormatCtx fmt;
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "analyzeduration", "5000000", 0);
        av_dict_set(&opts, "probesize", "2000000", 0);

        if (avformat_open_input(&fmt.ctx, videoPath.c_str(), nullptr, &opts) < 0)
        {
            log("thumbnail: failed to open " + videoPath);
            av_dict_free(&opts);
            return false;
        }
        av_dict_free(&opts);

        if (avformat_find_stream_info(fmt.ctx, nullptr) < 0)
        {
            log("thumbnail: failed to find stream info");
            return false;
        }

        // ── 2. Find video stream ─────────────────────────────────────────
        int streamIdx = av_find_best_stream(fmt.ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIdx < 0)
        {
            log("thumbnail: no video stream found");
            return false;
        }

        AVStream *vStream = fmt.ctx->streams[streamIdx];
        AVCodecParameters *codecpar = vStream->codecpar;

        // ── 3. Open decoder ──────────────────────────────────────────────
        const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder)
        {
            log("thumbnail: no decoder for codec");
            return false;
        }

        CodecCtx decCtx;
        decCtx.ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(decCtx.ctx, codecpar);
        decCtx.ctx->thread_count = 4;

        if (avcodec_open2(decCtx.ctx, decoder, nullptr) < 0)
        {
            log("thumbnail: failed to open decoder");
            return false;
        }

        int srcW = decCtx.ctx->width;
        int srcH = decCtx.ctx->height;
        if (srcW <= 0 || srcH <= 0)
        {
            log("thumbnail: invalid video dimensions");
            return false;
        }

        // ── 4. Calculate layout (adaptive high-res) ─────────────────────
        int effectiveWidth = cfg.width;
        if (cfg.adaptiveWidth)
            effectiveWidth = std::max(cfg.width, srcW); // never downscale below source

        int totalFrames = cfg.totalFrames();
        int thumbW = (effectiveWidth - (cfg.columns + 1) * cfg.spacing) / cfg.columns;
        double aspectRatio = (double)srcW / (double)srcH;
        int thumbH = (int)(thumbW / aspectRatio);
        if (thumbH <= 0)
            thumbH = 1;

        // Scale font + header proportionally to image width
        int fontScale = std::clamp(effectiveWidth / 1280, 2, 5);
        int headerH = std::max(cfg.headerHeight, fontScale * 24);

        int gridH = cfg.rows * thumbH + (cfg.rows + 1) * cfg.spacing;
        int totalH = headerH + gridH;
        int totalW = effectiveWidth;

        // ── 5. Allocate composite image (RGB24) ─────────────────────────
        int stride = totalW * 3;
        std::vector<uint8_t> image(stride * totalH, 0); // black background

        // Dark-grey header background
        fillRect(image.data(), stride, totalW, totalH,
                 0, 0, totalW, headerH, 30, 30, 35);

        // ── 6. Set up scaler (bicubic for best quality) ─────────────────
        SwsCtx thumbSws;
        thumbSws.ctx = sws_getContext(srcW, srcH, decCtx.ctx->pix_fmt,
                                      thumbW, thumbH, AV_PIX_FMT_RGB24,
                                      SWS_BICUBIC, nullptr, nullptr, nullptr);
        if (!thumbSws.ctx)
        {
            log("thumbnail: failed to create scaler");
            return false;
        }

        // ── 7. Calculate seek positions ──────────────────────────────────
        double duration = 0;
        if (fmt.ctx->duration > 0)
            duration = fmt.ctx->duration / (double)AV_TIME_BASE;
        else if (vStream->duration > 0 && vStream->time_base.den > 0)
            duration = vStream->duration * av_q2d(vStream->time_base);
        if (duration <= 0)
        {
            log("thumbnail: cannot determine video duration");
            return false;
        }

        double skipStart = std::min((double)cfg.skipStartSec, duration * 0.05);
        double usable = duration - skipStart;
        if (usable < 1.0)
            usable = duration; // very short video — use full duration
        double interval = usable / (totalFrames + 1);

        // ── 8. Extract frames and compose grid ──────────────────────────
        Frame decFrame;
        int thumbBufSize = thumbW * thumbH * 3;
        std::vector<uint8_t> thumbBuf(thumbBufSize);

        int framesExtracted = 0;
        for (int i = 0; i < totalFrames; ++i)
        {
            double seekTime = skipStart + (i + 1) * interval;
            if (seekTime >= duration)
                seekTime = duration - 0.5;

            // Convert to stream time base
            int64_t ts = (int64_t)(seekTime / av_q2d(vStream->time_base));

            if (!seekAndDecode(fmt.ctx, decCtx.ctx, streamIdx, ts, decFrame.f))
            {
                // Failed to decode — fill with dark grey
                int col = i % cfg.columns;
                int row = i / cfg.columns;
                int x = cfg.spacing + col * (thumbW + cfg.spacing);
                int y = headerH + cfg.spacing + row * (thumbH + cfg.spacing);
                fillRect(image.data(), stride, totalW, totalH,
                         x, y, thumbW, thumbH, 40, 40, 45);
                continue;
            }

            // Scale decoded frame to thumbnail size (RGB24)
            uint8_t *dstData[1] = {thumbBuf.data()};
            int dstStride[1] = {thumbW * 3};
            sws_scale(thumbSws.ctx,
                      decFrame.f->data, decFrame.f->linesize, 0, decFrame.f->height,
                      dstData, dstStride);

            // Copy thumbnail into composite image
            int col = i % cfg.columns;
            int row = i / cfg.columns;
            int dstX = cfg.spacing + col * (thumbW + cfg.spacing);
            int dstY = headerH + cfg.spacing + row * (thumbH + cfg.spacing);

            for (int line = 0; line < thumbH; ++line)
            {
                int imgY = dstY + line;
                if (imgY >= totalH)
                    break;
                uint8_t *imgRow = image.data() + imgY * stride + dstX * 3;
                uint8_t *tbRow = thumbBuf.data() + line * thumbW * 3;
                int copyW = std::min(thumbW, totalW - dstX);
                if (copyW > 0)
                    std::memcpy(imgRow, tbRow, copyW * 3);
            }

            // Draw timestamp overlay at bottom of thumbnail
            std::string ts_str = formatTime(seekTime);
            int tsScale = std::max(1, fontScale / 2);
            int overlayH = tsScale * 8 * 2 + tsScale * 4;
            darkenRect(image.data(), stride, totalW, totalH,
                       dstX, dstY + thumbH - overlayH, thumbW, overlayH, 0.55f);
            drawText(image.data(), stride, totalW, totalH,
                     dstX + tsScale * 2, dstY + thumbH - overlayH + tsScale * 2, ts_str,
                     255, 255, 255, tsScale);

            av_frame_unref(decFrame.f);
            framesExtracted++;
        }

        if (framesExtracted == 0)
        {
            log("thumbnail: no frames extracted");
            return false;
        }

        // ── 9. Draw header ───────────────────────────────────────────────
        std::string filename = std::filesystem::path(videoPath).filename().string();
        std::string durStr = formatTime(duration);
        std::string resStr = std::to_string(srcW) + "x" + std::to_string(srcH);

        // File size
        std::string sizeStr;
        try
        {
            auto fileSize = std::filesystem::file_size(videoPath);
            if (fileSize > 1024 * 1024 * 1024)
                sizeStr = std::to_string(fileSize / (1024 * 1024 * 1024)) + " GB";
            else if (fileSize > 1024 * 1024)
                sizeStr = std::to_string(fileSize / (1024 * 1024)) + " MB";
            else
                sizeStr = std::to_string(fileSize / 1024) + " KB";
        }
        catch (...)
        {
        }

        // Compose header: "filename.mkv | 1:23:45 | 1920x1080 | 512 MB"
        std::string header = filename;
        if (!durStr.empty())
            header += "  |  " + durStr;
        if (!resStr.empty())
            header += "  |  " + resStr;
        if (!sizeStr.empty())
            header += "  |  " + sizeStr;

        // Truncate header if too long for the image
        int charW = 5 * fontScale + fontScale; // glyph width * scale + gap
        int maxChars = (totalW - 16) / charW;
        if ((int)header.size() > maxChars && maxChars > 5)
            header = header.substr(0, maxChars - 3) + "...";

        drawText(image.data(), stride, totalW, totalH,
                 fontScale * 4, (headerH - fontScale * 8) / 2, header,
                 220, 220, 220, fontScale);

        // ── 10. Encode as JPEG ───────────────────────────────────────────
        if (!encodeJpeg(image.data(), totalW, totalH, stride, outputPath, cfg.quality))
        {
            log("thumbnail: JPEG encoding failed");
            return false;
        }

        log("thumbnail: generated " + outputPath +
            " (" + std::to_string(framesExtracted) + "/" + std::to_string(totalFrames) +
            " frames, " + std::to_string(totalW) + "x" + std::to_string(totalH) + ")");
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    // Embed thumbnail as cover art attachment inside MKV (in-place)
    // Uses mkvpropedit from MKVToolNix — zero video copying.
    // ─────────────────────────────────────────────────────────────────

    namespace
    {
#ifdef _WIN32
        // Run exe silently (no console window flash) on Windows.
        // exePath: full path to the executable (used as lpApplicationName)
        // cmdLine: full command line including exe + args (used as lpCommandLine)
        static int silentRunExe(const std::string &exePath, const std::string &cmdLine)
        {
            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            // CreateProcessA needs mutable buffer for lpCommandLine
            std::string buf = cmdLine;
            // Pass exePath as lpApplicationName so Windows doesn't need to parse
            // quoted paths with spaces from the command line
            BOOL ok = CreateProcessA(exePath.c_str(), buf.data(), nullptr, nullptr, FALSE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (!ok)
                return -1;
            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return (int)code;
        }
#endif
    } // anonymous namespace

    bool embedThumbnailInMKV(
        const std::string &videoPath,
        const std::string &jpegPath,
        std::function<void(const std::string &)> logCb,
        const std::string &mkvpropeditPath)
    {
        namespace fs = std::filesystem;
        auto log = [&](const std::string &msg)
        {
            if (logCb)
                logCb(msg);
        };

        // Only works for MKV containers
        auto ext = fs::path(videoPath).extension().string();
        for (auto &ch : ext)
            ch = (char)std::tolower((unsigned char)ch);
        if (ext != ".mkv")
        {
            log("thumbnail: embed skipped (not MKV: " + ext + ")");
            return false;
        }

        if (!fs::exists(jpegPath))
        {
            log("thumbnail: embed skipped (jpg not found)");
            return false;
        }

        // Resolve mkvpropedit path: use provided, or auto-detect
        std::string mkv_exe = mkvpropeditPath;
        if (mkv_exe.empty())
            mkv_exe = "mkvpropedit"; // bare command, rely on PATH

#ifdef _WIN32
        // Auto-detect: if bare command name, resolve full path
        if (mkv_exe == "mkvpropedit")
        {
            // First check if it's in PATH
            char pathBuf[MAX_PATH];
            if (SearchPathA(nullptr, "mkvpropedit.exe", nullptr, MAX_PATH, pathBuf, nullptr) > 0)
            {
                mkv_exe = pathBuf;
            }
            else
            {
                // Check common install locations
                const char *candidates[] = {
                    R"(C:\Program Files\MKVToolNix\mkvpropedit.exe)",
                    R"(C:\Program Files (x86)\MKVToolNix\mkvpropedit.exe)",
                };
                for (auto *c : candidates)
                {
                    if (GetFileAttributesA(c) != INVALID_FILE_ATTRIBUTES)
                    {
                        mkv_exe = c;
                        break;
                    }
                }
            }
        }

        // Final check: does the resolved exe exist?
        if (mkv_exe != "mkvpropedit" && GetFileAttributesA(mkv_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            log("thumbnail: mkvpropedit not found at " + mkv_exe);
            return false;
        }
        if (mkv_exe == "mkvpropedit")
        {
            log("thumbnail: mkvpropedit not found (not in PATH or Program Files)");
            return false;
        }

        log("thumbnail: using mkvpropedit: " + mkv_exe);
#endif

        // Build command line for mkvpropedit
        std::string cmdLine = "\"" + mkv_exe + "\""
                                               " \"" +
                              videoPath + "\""
                                          " --attachment-name cover.jpg"
                                          " --attachment-mime-type image/jpeg"
                                          " --add-attachment \"" +
                              jpegPath + "\"";

        int ret;
#ifdef _WIN32
        // Pass exe path explicitly as lpApplicationName for correct resolution
        ret = silentRunExe(mkv_exe, cmdLine);
#else
        cmdLine += " >/dev/null 2>&1";
        ret = std::system(cmdLine.c_str());
#endif

        if (ret == 0)
        {
            // Embedded successfully — remove external .jpg
            std::error_code ec;
            fs::remove(jpegPath, ec);
            log("thumbnail: embedded cover art in " +
                fs::path(videoPath).filename().string());
            return true;
        }

        log("thumbnail: mkvpropedit failed (exit code " +
            std::to_string(ret) + "), keeping external .jpg");
        return false;
    }

} // namespace sm
