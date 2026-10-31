// avtest.c - sanity/performance check of the libavcodec decode core on XAVC clips.
//
//   avtest <file.mp4> [max_frames] [first_frame.bmp]
//
// Decodes the first video stream with frame+slice threading, reports the
// stream's profile and pixel format, the decode speed, and optionally writes
// the first decoded frame as a 24-bit BMP (converted with libswscale).
//
// Build: build\build-win.ps1 (needs -FFmpeg <dir>)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

static double now_s(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static int save_bmp(const char* path, const AVFrame* fr) {
    int w = fr->width, h = fr->height;
    struct SwsContext* sws = sws_getContext(w, h, fr->format, w, h, AV_PIX_FMT_BGR24,
                                            SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) return -1;
    int stride = (w * 3 + 3) & ~3;
    uint8_t* buf = malloc((size_t)stride * h);
    uint8_t* dst[4] = { buf + (size_t)stride * (h - 1), NULL, NULL, NULL };   // bottom-up BMP
    int dstride[4] = { -stride, 0, 0, 0 };
    sws_scale(sws, (const uint8_t* const*)fr->data, fr->linesize, 0, h, dst, dstride);
    sws_freeContext(sws);

    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + (DWORD)stride * h;
    FILE* f = fopen(path, "wb");
    if (!f) { free(buf); return -1; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(buf, 1, (size_t)stride * h, f);
    fclose(f);

    // center pixel, as a quick "is this a real picture" signal
    const uint8_t* c = buf + (size_t)stride * (h / 2) + (w / 2) * 3;
    printf("first frame -> %s (center B=%u G=%u R=%u)\n", path, c[0], c[1], c[2]);
    free(buf);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: avtest <file> [max_frames] [first.bmp]\n"); return 64; }
    int max_frames = argc >= 3 ? atoi(argv[2]) : 100000;
    const char* bmp = argc >= 4 ? argv[3] : NULL;

    AVFormatContext* fmt = NULL;
    if (avformat_open_input(&fmt, argv[1], NULL, NULL) < 0) { printf("cannot open input\n"); return 1; }
    avformat_find_stream_info(fmt, NULL);
    int vi = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    if (vi < 0) { printf("no video stream\n"); return 2; }
    AVStream* st = fmt->streams[vi];
    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, st->codecpar);
    ctx->thread_count = 0;                                   // auto = all logical CPUs
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (avcodec_open2(ctx, dec, NULL) < 0) { printf("cannot open decoder\n"); return 3; }

    printf("libavcodec %s, decoder '%s', threads=%d\n", av_version_info(), dec->name, ctx->thread_count);
    printf("stream: %dx%d profile=%d (%s) level=%d pix_fmt=%s\n", ctx->width, ctx->height, ctx->profile,
           avcodec_profile_name(ctx->codec_id, ctx->profile), ctx->level,
           av_get_pix_fmt_name(ctx->pix_fmt));

    AVPacket* pkt = av_packet_alloc();
    AVFrame* fr = av_frame_alloc();
    int frames = 0, saved = 0;
    double t0 = now_s(), t_first = 0;
    int eof = 0;
    while (frames < max_frames) {
        if (!eof) {
            int r = av_read_frame(fmt, pkt);
            if (r < 0) { eof = 1; avcodec_send_packet(ctx, NULL); }
            else if (pkt->stream_index != vi) { av_packet_unref(pkt); continue; }
            else { avcodec_send_packet(ctx, pkt); av_packet_unref(pkt); }
        }
        int got = 0;
        while (avcodec_receive_frame(ctx, fr) == 0) {
            got = 1;
            if (!frames) {
                t_first = now_s() - t0;
                printf("decoded frame: %dx%d %s\n", fr->width, fr->height, av_get_pix_fmt_name(fr->format));
            }
            if (bmp && !saved) { save_bmp(bmp, fr); saved = 1; }
            frames++;
            av_frame_unref(fr);
            if (frames >= max_frames) break;
        }
        if (eof && !got) break;
    }
    double dt = now_s() - t0;
    printf("decoded %d frames in %.2f s -> %.1f fps (first frame after %.2f s)\n",
           frames, dt, dt > 0 ? frames / dt : 0.0, t_first);

    av_frame_free(&fr);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return frames > 0 ? 0 : 4;
}
