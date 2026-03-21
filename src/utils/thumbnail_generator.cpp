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
#include <libavutil/hwcontext.h>
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

        // RAII wrapper for hardware device context (CUDA/NVDEC)
        struct HwDevice
        {
            AVBufferRef *ctx = nullptr;
            ~HwDevice()
            {
                if (ctx)
                    av_buffer_unref(&ctx);
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

        // ── 3. Open decoder (CUDA/NVDEC if available, fallback CPU) ────
        const AVCodec *decoder = avcodec_find_decoder(codecpar->codec_id);
        if (!decoder)
        {
            log("thumbnail: no decoder for codec");
            return false;
        }

        HwDevice hwDev;
        bool useCuda = false;
        if (av_hwdevice_ctx_create(&hwDev.ctx, AV_HWDEVICE_TYPE_CUDA,
                                   nullptr, nullptr, 0) >= 0)
        {
            useCuda = true;
        }

        CodecCtx decCtx;
        decCtx.ctx = avcodec_alloc_context3(decoder);
        avcodec_parameters_to_context(decCtx.ctx, codecpar);

        if (useCuda)
        {
            decCtx.ctx->hw_device_ctx = av_buffer_ref(hwDev.ctx);
            decCtx.ctx->extra_hw_frames = 16; // Extra GPU frame buffers for maximum throughput
            decCtx.ctx->thread_count = 1;     // NVDEC handles parallelism in hardware
        }
        else
        {
            // Use ALL CPU cores for software decoding
            decCtx.ctx->thread_count = 0; // 0 = auto-detect optimal thread count
            decCtx.ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }

        if (avcodec_open2(decCtx.ctx, decoder, nullptr) < 0)
        {
            // CUDA decoder open failed — retry with software
            if (useCuda)
            {
                log("thumbnail: CUDA decoder open failed, falling back to CPU");
                avcodec_free_context(&decCtx.ctx);
                decCtx.ctx = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(decCtx.ctx, codecpar);
                decCtx.ctx->thread_count = 4;
                useCuda = false;
                if (avcodec_open2(decCtx.ctx, decoder, nullptr) < 0)
                {
                    log("thumbnail: failed to open decoder");
                    return false;
                }
            }
            else
            {
                log("thumbnail: failed to open decoder");
                return false;
            }
        }

        if (useCuda)
            log("thumbnail: GPU (CUDA/NVDEC) hardware decoding active");

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

        // ── 6. Scaler — created lazily after first frame decode ────────
        //  (CUDA frames need GPU→CPU transfer before we know the pixel format)
        SwsCtx thumbSws;

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
        Frame swFrame; // for GPU→CPU transfer when using CUDA
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

            // GPU → CPU transfer for CUDA-decoded frames
            AVFrame *srcFrame = decFrame.f;
            if (useCuda && srcFrame->format == AV_PIX_FMT_CUDA)
            {
                av_frame_unref(swFrame.f);
                if (av_hwframe_transfer_data(swFrame.f, srcFrame, 0) < 0)
                {
                    // CUDA transfer failed — fill with dark grey
                    int col = i % cfg.columns;
                    int row = i / cfg.columns;
                    int x = cfg.spacing + col * (thumbW + cfg.spacing);
                    int y = headerH + cfg.spacing + row * (thumbH + cfg.spacing);
                    fillRect(image.data(), stride, totalW, totalH,
                             x, y, thumbW, thumbH, 40, 40, 45);
                    av_frame_unref(decFrame.f);
                    continue;
                }
                srcFrame = swFrame.f;
            }

            // Lazy SWS init — pixel format known only after first decode
            if (!thumbSws.ctx)
            {
                thumbSws.ctx = sws_getContext(srcW, srcH, (AVPixelFormat)srcFrame->format,
                                              thumbW, thumbH, AV_PIX_FMT_RGB24,
                                              SWS_BICUBIC, nullptr, nullptr, nullptr);
                if (!thumbSws.ctx)
                {
                    log("thumbnail: failed to create scaler");
                    av_frame_unref(decFrame.f);
                    return false;
                }
            }

            // Scale decoded frame to thumbnail size (RGB24)
            uint8_t *dstData[1] = {thumbBuf.data()};
            int dstStride[1] = {thumbW * 3};
            sws_scale(thumbSws.ctx,
                      srcFrame->data, srcFrame->linesize, 0, srcFrame->height,
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
        // Check if file starts with EBML magic bytes (real Matroska container)
        // NOTE: also exposed as sm::isRealMatroska via the forwarding wrapper below
        static bool isRealMatroskaImpl(const std::string &path)
        {
            std::ifstream f(path, std::ios::binary);
            if (!f)
                return false;
            unsigned char hdr[4]{};
            f.read(reinterpret_cast<char *>(hdr), 4);
            // EBML header magic: 0x1A 0x45 0xDF 0xA3
            return hdr[0] == 0x1A && hdr[1] == 0x45 && hdr[2] == 0xDF && hdr[3] == 0xA3;
        }

        // Remux a non-Matroska .mkv (e.g. MP4-in-.mkv) to a real Matroska container
        // using FFmpeg API. Stream-copy only — zero re-encoding, no quality loss.
        // Fixes timestamp jumps / discontinuities by normalising DTS/PTS per stream.
        // On success the original file is replaced in-place.
        static bool remuxToRealMKV(const std::string &path,
                                   std::function<void(const std::string &)> log)
        {
            namespace fs = std::filesystem;

            // Temp output next to original: video~remuxed.mkv
            auto stem = fs::path(path).stem().string();
            auto dir = fs::path(path).parent_path();
            std::string tmpPath = (dir / (stem + "~remuxed.mkv")).string();

            AVFormatContext *inCtx = nullptr;
            if (avformat_open_input(&inCtx, path.c_str(), nullptr, nullptr) < 0)
            {
                if (log)
                    log("remux: failed to open input: " + path);
                return false;
            }
            if (avformat_find_stream_info(inCtx, nullptr) < 0)
            {
                avformat_close_input(&inCtx);
                if (log)
                    log("remux: failed to read stream info");
                return false;
            }

            AVFormatContext *outCtx = nullptr;
            if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", tmpPath.c_str()) < 0)
            {
                avformat_close_input(&inCtx);
                if (log)
                    log("remux: failed to create matroska output context");
                return false;
            }

            // Set matroska muxer options for proper cluster sizing
            // This prevents fragmented output with too many segments
            AVDictionary *opts = nullptr;
            av_dict_set(&opts, "cluster_size_limit", "5242880", 0); // 5MB clusters max
            av_dict_set(&opts, "cluster_time_limit", "5000000", 0); // 5 second clusters max

            // Map every stream (video, audio, subs) — stream copy
            std::vector<int> streamMap(inCtx->nb_streams, -1);
            int outIdx = 0;
            for (unsigned i = 0; i < inCtx->nb_streams; i++)
            {
                AVStream *inStream = inCtx->streams[i];
                auto ctype = inStream->codecpar->codec_type;
                if (ctype != AVMEDIA_TYPE_VIDEO &&
                    ctype != AVMEDIA_TYPE_AUDIO &&
                    ctype != AVMEDIA_TYPE_SUBTITLE)
                    continue;

                AVStream *outStream = avformat_new_stream(outCtx, nullptr);
                if (!outStream)
                {
                    if (log)
                        log("remux: failed to create output stream");
                    avformat_close_input(&inCtx);
                    avformat_free_context(outCtx);
                    return false;
                }
                avcodec_parameters_copy(outStream->codecpar, inStream->codecpar);
                outStream->codecpar->codec_tag = 0; // let muxer pick
                outStream->time_base = inStream->time_base;
                streamMap[i] = outIdx++;
            }

            // Open output file
            if (!(outCtx->oformat->flags & AVFMT_NOFILE))
            {
                if (avio_open(&outCtx->pb, tmpPath.c_str(), AVIO_FLAG_WRITE) < 0)
                {
                    if (log)
                        log("remux: failed to open output file: " + tmpPath);
                    avformat_close_input(&inCtx);
                    avformat_free_context(outCtx);
                    return false;
                }
            }

            if (avformat_write_header(outCtx, &opts) < 0)
            {
                if (log)
                    log("remux: failed to write header");
                av_dict_free(&opts);
                avformat_close_input(&inCtx);
                if (outCtx->pb)
                    avio_closep(&outCtx->pb);
                avformat_free_context(outCtx);
                std::error_code ec;
                fs::remove(tmpPath, ec);
                return false;
            }
            av_dict_free(&opts);

            // Per-stream DTS tracker to fix timestamp jumps / discontinuities
            struct StreamTS
            {
                int64_t lastDts = AV_NOPTS_VALUE;
                int64_t offset = 0; // cumulative offset to fix jumps
            };
            std::vector<StreamTS> tsTrack(inCtx->nb_streams);

            AVPacket *pkt = av_packet_alloc();
            bool ok = true;

            while (av_read_frame(inCtx, pkt) >= 0)
            {
                unsigned srcIdx = pkt->stream_index;
                if (srcIdx >= (unsigned)streamMap.size() || streamMap[srcIdx] < 0)
                {
                    av_packet_unref(pkt);
                    continue;
                }

                AVStream *inStream = inCtx->streams[srcIdx];
                AVStream *outStream = outCtx->streams[streamMap[srcIdx]];
                auto &ts = tsTrack[srcIdx];

                // ── Fix timestamp jumps ──────────────────────────
                // If DTS jumps backwards or has a large forward gap,
                // adjust offset so output timestamps stay monotonic.
                if (pkt->dts != AV_NOPTS_VALUE)
                {
                    int64_t adjDts = pkt->dts + ts.offset;

                    if (ts.lastDts != AV_NOPTS_VALUE)
                    {
                        int64_t diff = adjDts - ts.lastDts;
                        // Detect backward jump or huge forward gap (> 10 s in stream TB)
                        double diffSec = diff * av_q2d(inStream->time_base);
                        if (diff < 0 || diffSec > 10.0)
                        {
                            // Shift so this packet follows right after the last one
                            // with a 1-tick gap to keep strictly monotonic
                            ts.offset += (ts.lastDts + 1) - (pkt->dts + ts.offset);
                            adjDts = ts.lastDts + 1;
                        }
                    }
                    ts.lastDts = adjDts;
                    pkt->dts = adjDts;

                    // PTS must be >= DTS
                    if (pkt->pts != AV_NOPTS_VALUE)
                    {
                        pkt->pts += ts.offset;
                        if (pkt->pts < pkt->dts)
                            pkt->pts = pkt->dts;
                    }
                }
                else if (pkt->pts != AV_NOPTS_VALUE)
                {
                    pkt->pts += ts.offset;
                }

                // Rescale to output timebase
                pkt->stream_index = streamMap[srcIdx];
                av_packet_rescale_ts(pkt, inStream->time_base, outStream->time_base);

                if (av_interleaved_write_frame(outCtx, pkt) < 0)
                {
                    if (log)
                        log("remux: write error, aborting");
                    ok = false;
                    av_packet_unref(pkt);
                    break;
                }
                // av_interleaved_write_frame already unrefs on success
            }

            av_packet_free(&pkt);
            if (ok)
                ok = (av_write_trailer(outCtx) >= 0);

            avformat_close_input(&inCtx);
            if (outCtx->pb)
                avio_closep(&outCtx->pb);
            avformat_free_context(outCtx);

            if (!ok)
            {
                std::error_code ec;
                fs::remove(tmpPath, ec);
                if (log)
                    log("remux: failed, keeping original file");
                return false;
            }

            // Verify output is valid BEFORE touching the original
            {
                std::error_code szEc;
                auto tmpSz = fs::file_size(tmpPath, szEc);
                auto origSz = fs::file_size(path, szEc);
                if (tmpSz == 0 || (origSz > 0 && tmpSz < origSz / 4))
                {
                    std::error_code ec2;
                    fs::remove(tmpPath, ec2);
                    if (log)
                        log("remux: output corrupt/too small (" +
                            std::to_string(tmpSz) + " vs " + std::to_string(origSz) +
                            " bytes), keeping original");
                    return false;
                }
                if (!isRealMatroska(tmpPath))
                {
                    std::error_code ec2;
                    fs::remove(tmpPath, ec2);
                    if (log)
                        log("remux: output is not valid Matroska, keeping original");
                    return false;
                }
            }

            // Replace original with remuxed file
            std::error_code ec;
            fs::remove(path, ec);
            if (ec)
            {
                if (log)
                    log("remux: failed to remove original: " + ec.message());
                fs::remove(tmpPath, ec);
                return false;
            }
            fs::rename(tmpPath, path, ec);
            if (ec)
            {
                if (log)
                    log("remux: failed to rename temp file: " + ec.message());
                return false;
            }

            if (log)
                log("remux: converted to real Matroska: " + fs::path(path).filename().string());
            return true;
        }

#ifdef _WIN32
        // Run exe silently (no console window flash) on Windows.
        // Captures stderr so caller can log the actual error message.
        static int silentRunExe(const std::string &exePath, const std::string &cmdLine,
                                std::string *stderrOut = nullptr)
        {
            // Create pipe for stderr capture
            HANDLE hReadErr = nullptr, hWriteErr = nullptr;
            SECURITY_ATTRIBUTES sa{};
            sa.nLength = sizeof(sa);
            sa.bInheritHandle = TRUE;
            if (stderrOut)
                CreatePipe(&hReadErr, &hWriteErr, &sa, 0);

            STARTUPINFOA si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
            si.wShowWindow = SW_HIDE;
            si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            si.hStdError = stderrOut ? hWriteErr : GetStdHandle(STD_ERROR_HANDLE);

            PROCESS_INFORMATION pi{};
            std::string buf = cmdLine;
            BOOL ok = CreateProcessA(exePath.c_str(), buf.data(), nullptr, nullptr, TRUE,
                                     CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
            if (stderrOut && hWriteErr)
                CloseHandle(hWriteErr); // close write end in parent

            if (!ok)
            {
                if (stderrOut && hReadErr)
                    CloseHandle(hReadErr);
                return -1;
            }

            // Read stderr while process runs
            if (stderrOut && hReadErr)
            {
                char tmp[512];
                DWORD nRead;
                while (ReadFile(hReadErr, tmp, sizeof(tmp) - 1, &nRead, nullptr) && nRead > 0)
                {
                    tmp[nRead] = '\0';
                    *stderrOut += tmp;
                }
                CloseHandle(hReadErr);
            }

            WaitForSingleObject(pi.hProcess, INFINITE);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            return (int)code;
        }
#endif
    } // anonymous namespace

    bool hasCoverArt(const std::string &videoPath)
    {
        AVFormatContext *fmtCtx = nullptr;
        if (avformat_open_input(&fmtCtx, videoPath.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
        {
            avformat_close_input(&fmtCtx);
            return false;
        }
        // Check for attached picture streams (cover art)
        bool found = false;
        for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
        {
            if (fmtCtx->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC)
            {
                found = true;
                break;
            }
        }
        // Also check Matroska-style attachments via metadata
        if (!found)
        {
            for (unsigned i = 0; i < fmtCtx->nb_streams; i++)
            {
                auto *par = fmtCtx->streams[i]->codecpar;
                if (par->codec_type == AVMEDIA_TYPE_VIDEO &&
                    par->codec_id == AV_CODEC_ID_MJPEG)
                {
                    found = true;
                    break;
                }
                if (par->codec_id == AV_CODEC_ID_PNG ||
                    par->codec_id == AV_CODEC_ID_BMP)
                {
                    found = true;
                    break;
                }
            }
        }
        avformat_close_input(&fmtCtx);
        return found;
    }

    bool isRealMatroska(const std::string &path)
    {
        return isRealMatroskaImpl(path);
    }

    bool hasTimestampIssues(const std::string &videoPath,
                            std::function<void(const std::string &)> logCb)
    {
        AVFormatContext *fmtCtx = nullptr;
        if (avformat_open_input(&fmtCtx, videoPath.c_str(), nullptr, nullptr) < 0)
            return false;
        if (avformat_find_stream_info(fmtCtx, nullptr) < 0)
        {
            avformat_close_input(&fmtCtx);
            return false;
        }

        struct TS
        {
            int64_t lastDts = AV_NOPTS_VALUE;
        };
        std::vector<TS> tracks(fmtCtx->nb_streams);

        AVPacket *pkt = av_packet_alloc();
        bool issues = false;
        int count = 0;
        constexpr int kMaxPackets = 5000;

        while (av_read_frame(fmtCtx, pkt) >= 0)
        {
            unsigned si = pkt->stream_index;
            if (si < (unsigned)tracks.size() && pkt->dts != AV_NOPTS_VALUE)
            {
                auto &t = tracks[si];
                if (t.lastDts != AV_NOPTS_VALUE)
                {
                    int64_t diff = pkt->dts - t.lastDts;
                    double diffSec = diff * av_q2d(fmtCtx->streams[si]->time_base);
                    if (diff < 0 || diffSec > 10.0)
                    {
                        if (logCb)
                            logCb("timestamps: DTS jump at pkt " + std::to_string(count) +
                                  " (stream " + std::to_string(si) +
                                  ", gap " + std::to_string((int)diffSec) + "s)");
                        issues = true;
                        av_packet_unref(pkt);
                        break;
                    }
                }
                t.lastDts = pkt->dts;
            }
            av_packet_unref(pkt);
            if (++count >= kMaxPackets)
                break;
        }

        av_packet_free(&pkt);
        avformat_close_input(&fmtCtx);
        return issues;
    }

    bool fixTimestamps(const std::string &videoPath,
                       std::function<void(const std::string &)> logCb)
    {
        if (logCb)
            logCb("timestamps: remuxing to fix DTS discontinuities...");
        return remuxToRealMKV(videoPath, logCb);
    }

    bool isVRFromPath(const std::string &videoPath)
    {
        std::string lower = videoPath;
        for (auto &c : lower)
            c = (char)std::tolower((unsigned char)c);

        // Check 1: [*VR] bracket tags (e.g. [SCVR], [DCVR])
        size_t pos = 0;
        while ((pos = lower.find('[', pos)) != std::string::npos)
        {
            auto end = lower.find(']', pos);
            if (end != std::string::npos && end - pos >= 3)
            {
                if (lower.compare(end - 2, 2, "vr") == 0)
                    return true;
            }
            pos++;
        }

        // Check 2: /VR/ or \VR\ as a standalone folder segment
        //           (but NOT "NO VR" — that folder means non-VR)
        std::string norm = lower;
        for (auto &c : norm)
            if (c == '\\')
                c = '/';

        // Ensure path starts/ends with separator for uniform matching
        if (norm.front() != '/')
            norm = "/" + norm;

        auto vrPos = norm.find("/vr/");
        while (vrPos != std::string::npos)
        {
            // Reject if preceded by "no " → "/no vr/"
            if (vrPos >= 3 && norm.compare(vrPos - 3, 4, "/no ") == 0)
            {
                vrPos = norm.find("/vr/", vrPos + 4);
                continue;
            }
            return true;
        }

        return false;
    }

    // Remux any video file to a real Matroska .mkv container.
    // For .mkv files that aren't real Matroska, remuxes in-place.
    // For non-.mkv files (mp4, etc), creates a .mkv alongside and returns the new path.
    // Stream-copy only — zero re-encoding, no quality loss.
    static std::string remuxToMKVIfNeeded(
        const std::string &videoPath,
        std::function<void(const std::string &)> logCb)
    {
        namespace fs = std::filesystem;
        auto ext = fs::path(videoPath).extension().string();
        for (auto &ch : ext)
            ch = (char)std::tolower((unsigned char)ch);

        if (ext == ".mkv")
        {
            // Already .mkv — check if real Matroska
            if (isRealMatroska(videoPath))
                return videoPath; // good to go
            // Fake mkv — remux in-place
            if (logCb)
                logCb("thumbnail: not a real Matroska container, remuxing in-place...");
            if (remuxToRealMKV(videoPath, logCb))
                return videoPath;
            return ""; // failed
        }
        else
        {
            // Non-MKV (mp4, ts, etc) — remux to .mkv next to original
            auto mkvPath = fs::path(videoPath);
            mkvPath.replace_extension(".mkv");
            std::string mkvStr = mkvPath.string();

            // If .mkv already exists and is real Matroska, use it
            if (fs::exists(mkvPath) && isRealMatroska(mkvStr))
                return mkvStr;

            if (logCb)
                logCb("thumbnail: remuxing " + ext + " to MKV (stream copy)...");

            // Use temp name then rename
            auto stem = mkvPath.stem().string();
            auto dir = mkvPath.parent_path();
            std::string tmpPath = (dir / (stem + "~remuxed.mkv")).string();

            // We can't use remuxToRealMKV directly since paths differ.
            // Open input from the original non-mkv path, write to tmpPath
            AVFormatContext *inCtx = nullptr;
            if (avformat_open_input(&inCtx, videoPath.c_str(), nullptr, nullptr) < 0)
            {
                if (logCb)
                    logCb("remux: failed to open input: " + videoPath);
                return "";
            }
            if (avformat_find_stream_info(inCtx, nullptr) < 0)
            {
                avformat_close_input(&inCtx);
                if (logCb)
                    logCb("remux: failed to read stream info");
                return "";
            }

            AVFormatContext *outCtx = nullptr;
            if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", tmpPath.c_str()) < 0)
            {
                avformat_close_input(&inCtx);
                if (logCb)
                    logCb("remux: failed to create matroska output");
                return "";
            }

            std::vector<int> smap(inCtx->nb_streams, -1);
            int outIdx = 0;
            for (unsigned i = 0; i < inCtx->nb_streams; i++)
            {
                auto ctype = inCtx->streams[i]->codecpar->codec_type;
                if (ctype != AVMEDIA_TYPE_VIDEO && ctype != AVMEDIA_TYPE_AUDIO &&
                    ctype != AVMEDIA_TYPE_SUBTITLE)
                    continue;
                AVStream *os = avformat_new_stream(outCtx, nullptr);
                if (!os)
                {
                    avformat_close_input(&inCtx);
                    avformat_free_context(outCtx);
                    return "";
                }
                avcodec_parameters_copy(os->codecpar, inCtx->streams[i]->codecpar);
                os->codecpar->codec_tag = 0;
                os->time_base = inCtx->streams[i]->time_base;
                smap[i] = outIdx++;
            }

            if (!(outCtx->oformat->flags & AVFMT_NOFILE))
                if (avio_open(&outCtx->pb, tmpPath.c_str(), AVIO_FLAG_WRITE) < 0)
                {
                    avformat_close_input(&inCtx);
                    avformat_free_context(outCtx);
                    return "";
                }

            if (avformat_write_header(outCtx, nullptr) < 0)
            {
                avformat_close_input(&inCtx);
                if (outCtx->pb)
                    avio_closep(&outCtx->pb);
                avformat_free_context(outCtx);
                std::error_code ec;
                fs::remove(tmpPath, ec);
                return "";
            }

            struct STS
            {
                int64_t lastDts = AV_NOPTS_VALUE;
                int64_t offset = 0;
            };
            std::vector<STS> tst(inCtx->nb_streams);
            AVPacket *pkt = av_packet_alloc();
            bool ok = true;
            while (av_read_frame(inCtx, pkt) >= 0)
            {
                unsigned si = pkt->stream_index;
                if (si >= smap.size() || smap[si] < 0)
                {
                    av_packet_unref(pkt);
                    continue;
                }
                auto *is = inCtx->streams[si];
                auto *os = outCtx->streams[smap[si]];
                auto &t = tst[si];
                if (pkt->dts != AV_NOPTS_VALUE)
                {
                    int64_t adj = pkt->dts + t.offset;
                    if (t.lastDts != AV_NOPTS_VALUE)
                    {
                        int64_t d = adj - t.lastDts;
                        double ds = d * av_q2d(is->time_base);
                        if (d < 0 || ds > 10.0)
                        {
                            t.offset += (t.lastDts + 1) - (pkt->dts + t.offset);
                            adj = t.lastDts + 1;
                        }
                    }
                    t.lastDts = adj;
                    pkt->dts = adj;
                    if (pkt->pts != AV_NOPTS_VALUE)
                    {
                        pkt->pts += t.offset;
                        if (pkt->pts < pkt->dts)
                            pkt->pts = pkt->dts;
                    }
                }
                else if (pkt->pts != AV_NOPTS_VALUE)
                    pkt->pts += t.offset;
                pkt->stream_index = smap[si];
                av_packet_rescale_ts(pkt, is->time_base, os->time_base);
                if (av_interleaved_write_frame(outCtx, pkt) < 0)
                {
                    ok = false;
                    av_packet_unref(pkt);
                    break;
                }
            }
            av_packet_free(&pkt);
            if (ok)
                ok = (av_write_trailer(outCtx) >= 0);
            avformat_close_input(&inCtx);
            if (outCtx->pb)
                avio_closep(&outCtx->pb);
            avformat_free_context(outCtx);

            if (!ok)
            {
                std::error_code ec;
                fs::remove(tmpPath, ec);
                if (logCb)
                    logCb("remux: failed");
                return "";
            }

            // Verify output is valid BEFORE finalizing
            {
                std::error_code szEc;
                auto tmpSz = fs::file_size(tmpPath, szEc);
                auto origSz = fs::file_size(videoPath, szEc);
                if (tmpSz == 0 || (origSz > 0 && tmpSz < origSz / 4))
                {
                    std::error_code ec2;
                    fs::remove(tmpPath, ec2);
                    if (logCb)
                        logCb("remux: output corrupt/too small (" +
                              std::to_string(tmpSz) + " vs " + std::to_string(origSz) +
                              " bytes), aborting");
                    return "";
                }
                if (!isRealMatroska(tmpPath))
                {
                    std::error_code ec2;
                    fs::remove(tmpPath, ec2);
                    if (logCb)
                        logCb("remux: output is not valid Matroska, aborting");
                    return "";
                }
            }

            // Rename temp to final .mkv
            std::error_code ec;
            if (fs::exists(mkvPath))
                fs::remove(mkvPath, ec);
            fs::rename(tmpPath, mkvPath, ec);
            if (ec)
            {
                if (logCb)
                    logCb("remux: rename failed: " + ec.message());
                fs::remove(tmpPath, ec);
                return "";
            }

            if (logCb)
                logCb("remux: created " + mkvPath.filename().string() + " (stream copy)");
            return mkvStr;
        }
    }

    // ── mkvpropedit path resolution (shared by embed + VR inject) ────
    static std::string resolveMkvpropedit(
        const std::string &provided,
        std::function<void(const std::string &)> logCb)
    {
        auto log = [&](const std::string &msg)
        { if (logCb) logCb(msg); };

        std::string mkv_exe = provided;
        if (mkv_exe.empty())
            mkv_exe = "mkvpropedit";

#ifdef _WIN32
        if (mkv_exe == "mkvpropedit")
        {
            char pathBuf[MAX_PATH];
            if (SearchPathA(nullptr, "mkvpropedit.exe", nullptr, MAX_PATH, pathBuf, nullptr) > 0)
            {
                mkv_exe = pathBuf;
            }
            else
            {
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

        if (mkv_exe != "mkvpropedit" && GetFileAttributesA(mkv_exe.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            log("mkvpropedit not found at " + mkv_exe);
            return "";
        }
        if (mkv_exe == "mkvpropedit")
        {
            log("mkvpropedit not found (not in PATH or Program Files)");
            return "";
        }
#endif
        return mkv_exe;
    }

    // ── VR spatial metadata injection ────────────────────────────────
    bool injectVRSpatialMetadata(
        const std::string &mkvPath,
        std::function<void(const std::string &)> logCb,
        const std::string &mkvpropeditPath)
    {
        namespace fs = std::filesystem;
        auto log = [&](const std::string &msg)
        { if (logCb) logCb(msg); };

        if (!fs::exists(mkvPath))
        {
            log("vr: file not found: " + mkvPath);
            return false;
        }

        std::string mkv_exe = resolveMkvpropedit(mkvpropeditPath, logCb);
        if (mkv_exe.empty())
            return false;

        // Set Matroska native VR track elements via mkvpropedit:
        //   stereo-mode=1              → side-by-side (left eye first)
        //   projection-type=1          → equirectangular
        //   projection-pose-*=0        → no rotation
        //   projection-private          → protobuf EquirectProjection with FOV bounds:
        //     Field 3 (left_fov=90.0f):  tag=0x1D + float32 LE 0x0000B442
        //     Field 4 (right_fov=90.0f): tag=0x25 + float32 LE 0x0000B442
        //     → crops equirectangular to front 180° hemisphere (fisheye 180 SBS)
        std::string cmdLine = "\"" + mkv_exe + "\" \"" + mkvPath + "\""
                                                                   " --edit track:v1"
                                                                   " --set stereo-mode=1"
                                                                   " --set projection-type=1"
                                                                   " --set projection-pose-yaw=0"
                                                                   " --set projection-pose-pitch=0"
                                                                   " --set projection-pose-roll=0"
                                                                   " --set projection-private=hex:1D0000B442250000B442";

        log("vr: injecting fisheye 180\xC2\xB0 SBS spatial metadata...");

        int ret;
#ifdef _WIN32
        std::string errMsg;
        ret = silentRunExe(mkv_exe, cmdLine, &errMsg);
#else
        cmdLine += " 2>&1";
        ret = std::system(cmdLine.c_str());
        std::string errMsg;
#endif

        if (ret == 0)
        {
            log("vr: spatial metadata set (fisheye 180\xC2\xB0 SBS)");
            return true;
        }

        while (!errMsg.empty() && (errMsg.back() == '\n' || errMsg.back() == '\r' || errMsg.back() == ' '))
            errMsg.pop_back();

        std::string failMsg = "vr: mkvpropedit failed (exit " + std::to_string(ret) + ")";
        if (!errMsg.empty())
            failMsg += ": " + errMsg;
        log(failMsg);
        return false;
    }

    // ── Embed thumbnail + VR metadata ───────────────────────────────
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

        // Step 1: Ensure we have a real Matroska file (remux if needed)
        std::string mkvPath = remuxToMKVIfNeeded(videoPath, logCb);
        if (mkvPath.empty())
        {
            log("thumbnail: cannot prepare MKV for processing");
            return false;
        }

        // Step 2: Resolve mkvpropedit
        std::string mkv_exe = resolveMkvpropedit(mkvpropeditPath, logCb);
        if (mkv_exe.empty())
            return false;

        log("thumbnail: using mkvpropedit: " + mkv_exe);

        bool ok = true;

        // Step 3: Embed cover art (if jpg exists and no cover yet)
        // Use standard Matroska cover art naming for DLNA/Skybox/Plex compatibility:
        // - attachment-name: cover.jpg (required)
        // - attachment-description: cover (helps some players)
        // - mime-type: image/jpeg
        if (fs::exists(jpegPath))
        {
            if (hasCoverArt(mkvPath))
            {
                log("thumbnail: already has cover art, skipping embed");
            }
            else
            {
                // Use --attachment-description "cover" for better DLNA compatibility
                std::string cmdLine = "\"" + mkv_exe + "\" \"" + mkvPath + "\""
                                                                           " --attachment-name cover.jpg"
                                                                           " --attachment-description cover"
                                                                           " --attachment-mime-type image/jpeg"
                                                                           " --add-attachment \"" +
                                      jpegPath + "\"";

                log("thumbnail: embedding in " + fs::path(mkvPath).filename().string() + "...");

                int ret;
#ifdef _WIN32
                std::string errMsg;
                ret = silentRunExe(mkv_exe, cmdLine, &errMsg);
#else
                cmdLine += " 2>&1";
                ret = std::system(cmdLine.c_str());
                std::string errMsg;
#endif

                if (ret == 0)
                {
                    log("thumbnail: embedded cover art in " +
                        fs::path(mkvPath).filename().string());
                }
                else
                {
                    while (!errMsg.empty() && (errMsg.back() == '\n' || errMsg.back() == '\r' || errMsg.back() == ' '))
                        errMsg.pop_back();

                    std::string failMsg = "thumbnail: mkvpropedit failed (exit code " + std::to_string(ret) + ")";
                    if (!errMsg.empty())
                        failMsg += ": " + errMsg;
                    failMsg += ", keeping external .jpg";
                    log(failMsg);
                    ok = false;
                }
            }
        }

        // Step 4: Inject VR spatial metadata if this is VR content
        if (isVRFromPath(videoPath))
        {
            log("thumbnail: VR content detected, injecting spatial metadata...");
            injectVRSpatialMetadata(mkvPath, logCb, mkv_exe);
        }

        return ok;
    }

    std::string ensureRealMKV(
        const std::string &videoPath,
        std::function<void(const std::string &)> logCb)
    {
        return remuxToMKVIfNeeded(videoPath, logCb);
    }

} // namespace sm
