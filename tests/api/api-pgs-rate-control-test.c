/*
 * PGS encoder rate control test.
 *
 * Verifies that the max_cdb_usage option drops Display Sets that
 * would overflow the HDMV coded data buffer model, and that events
 * within budget are encoded normally.
 *
 * Copyright (c) 2026 David Connolly
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

static int encode_large_subtitle(AVCodecContext *ctx, uint8_t *buf,
                                 int buf_size, int x, int y,
                                 int w, int h, int64_t pts)
{
    AVSubtitle sub;
    AVSubtitleRect rect;
    uint8_t *indices;
    uint32_t palette[4] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
    };
    int size;

    indices = av_mallocz(w * h);
    if (!indices)
        return AVERROR(ENOMEM);
    /* Fill with alternating indices to defeat RLE compression.
     * PGS RLE encodes runs of identical pixels; alternating values
     * produce worst-case output close to 1 byte per pixel. */
    for (int k = 0; k < w * h; k++)
        indices[k] = k % 4;

    memset(&sub, 0, sizeof(sub));
    memset(&rect, 0, sizeof(rect));

    sub.num_rects = 1;
    sub.rects = av_malloc(sizeof(*sub.rects));
    if (!sub.rects) {
        av_free(indices);
        return AVERROR(ENOMEM);
    }
    sub.rects[0] = &rect;
    sub.start_display_time = 0;
    sub.end_display_time   = 3000;
    sub.pts                = pts;

    rect.type       = SUBTITLE_BITMAP;
    rect.x          = x;
    rect.y          = y;
    rect.w          = w;
    rect.h          = h;
    rect.nb_colors  = 4;
    rect.data[0]    = indices;
    rect.linesize[0] = w;
    rect.data[1]    = (uint8_t *)palette;
    rect.linesize[1] = 4 * 4;

    size = avcodec_encode_subtitle(ctx, buf, buf_size, &sub);

    av_freep(&sub.rects);
    av_free(indices);
    return size;
}

int main(void)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    uint8_t *buf;
    int ret = 0, size;
    int buf_size = 2 * 1024 * 1024;

    buf = av_malloc(buf_size);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return 1;
    }

    codec = avcodec_find_encoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    if (!codec) {
        fprintf(stderr, "PGS encoder not found\n");
        av_free(buf);
        return 1;
    }

    /*
     * Test 1: Without rate control, large subtitles at tight intervals
     * should all encode successfully (even if CDB would overflow).
     */
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) { ret = 1; goto end; }
    ctx->width      = 1920;
    ctx->height     = 1080;
    ctx->time_base  = (AVRational){ 1, 90000 };
    /* max_cdb_usage=0 (disabled, default) */
    if (avcodec_open2(ctx, codec, NULL) < 0) { ret = 1; goto end; }

    /* Encode a 1920x200 subtitle — roughly 384KB uncompressed.
     * RLE with uniform fill compresses well but still large. */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 0,
                                 1920, 200, 0);
    if (size <= 0) {
        fprintf(stderr, "Test 1a: first encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    printf("Test 1a: 1920x200 at PTS=0 — encoded %d bytes ✓\n", size);

    /* Second encode 100ms later — different height forces Epoch Start.
     * CDB may be tight but no rate control enabled. */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 200,
                                 1920, 250, 9000);
    if (size <= 0) {
        fprintf(stderr, "Test 1b: second encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    printf("Test 1b: 1920x250 at PTS=9000 (100ms) — "
           "encoded %d bytes ✓\n", size);
    avcodec_free_context(&ctx);

    /*
     * Test 2: With rate control (max_cdb_usage=0.5), the second large
     * subtitle at a tight interval should be dropped (returns 0).
     */
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) { ret = 1; goto end; }
    ctx->width      = 1920;
    ctx->height     = 1080;
    ctx->time_base  = (AVRational){ 1, 90000 };
    av_opt_set_double(ctx->priv_data, "max_cdb_usage", 0.5, 0);
    if (avcodec_open2(ctx, codec, NULL) < 0) { ret = 1; goto end; }

    /* First encode: CDB starts full, should succeed */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 0,
                                 1920, 200, 0);
    if (size <= 0) {
        fprintf(stderr, "Test 2a: first encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    printf("Test 2a: 1920x200 at PTS=0, max_cdb=0.5 — "
           "encoded %d bytes ✓\n", size);

    /* Second encode 100ms later: different height forces Epoch Start.
     * CDB refilled ~200KB but estimated DS is ~480KB uncompressed.
     * With 50% threshold, should be dropped. */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 200,
                                 1920, 250, 9000);
    if (size != 0) {
        fprintf(stderr, "Test 2b: expected drop (size=0), got %d\n",
                size);
        ret = 1;
        goto end;
    }
    printf("Test 2b: 1920x250 at PTS=9000, max_cdb=0.5 — "
           "dropped (CDB pressure) ✓\n");
    avcodec_free_context(&ctx);

    /*
     * Test 3: With rate control but sufficient time gap, should encode.
     */
    ctx = avcodec_alloc_context3(codec);
    if (!ctx) { ret = 1; goto end; }
    ctx->width      = 1920;
    ctx->height     = 1080;
    ctx->time_base  = (AVRational){ 1, 90000 };
    av_opt_set_double(ctx->priv_data, "max_cdb_usage", 0.9, 0);
    if (avcodec_open2(ctx, codec, NULL) < 0) { ret = 1; goto end; }

    /* First encode */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 0,
                                 1920, 200, 0);
    if (size <= 0) {
        fprintf(stderr, "Test 3a: first encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    printf("Test 3a: 1920x200 at PTS=0, max_cdb=0.9 — "
           "encoded %d bytes ✓\n", size);

    /* Second encode 5 seconds later: CDB fully refilled.
     * Different height forces Epoch Start. */
    size = encode_large_subtitle(ctx, buf, buf_size, 0, 200,
                                 1920, 250, 450000);
    if (size <= 0) {
        fprintf(stderr, "Test 3b: expected success, got %d\n", size);
        ret = 1;
        goto end;
    }
    printf("Test 3b: 1920x250 at PTS=450000 (5s), max_cdb=0.9 — "
           "encoded %d bytes ✓\n", size);
    avcodec_free_context(&ctx);
    ctx = NULL;

    printf("\nAll rate control tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
