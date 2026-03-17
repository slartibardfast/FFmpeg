/*
 * PGS encoder forced subtitle test.
 *
 * Verifies that the forced_on_flag is correctly set in PCS composition
 * descriptors: from per-rect AV_SUBTITLE_FLAG_FORCED, and from the
 * force_all encoder option.
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
#include "pgs-test-util.h"
#include "libavutil/opt.h"

/* Get the composition_flag byte for the first object in a PCS.
 * PCS layout: type(1) + len(2) + video_w(2) + video_h(2) +
 * frame_rate(1) + comp_num(2) + comp_state(1) + palette_update(1) +
 * palette_id(1) + num_objs(1) + object_id(2) + window_id(1) +
 * composition_flag(1) = offset 17 from segment start */
static int get_pcs_forced_flag(const uint8_t *pcs)
{
    return pcs[17] & 0x40;
}

static AVCodecContext *open_encoder(int force_all)
{
    const AVCodec *codec;
    AVCodecContext *ctx;

    codec = avcodec_find_encoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    if (!codec)
        return NULL;

    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return NULL;

    ctx->width      = 1920;
    ctx->height     = 1080;
    ctx->time_base  = (AVRational){ 1, 90000 };

    if (force_all)
        av_opt_set_int(ctx->priv_data, "force_all", 1, 0);

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        avcodec_free_context(&ctx);
        return NULL;
    }
    return ctx;
}

static int encode_one(AVCodecContext *ctx, uint8_t *buf, int buf_size,
                      int forced_flag)
{
    AVSubtitle sub;
    AVSubtitleRect rect;
    uint8_t indices[16];
    uint32_t palette[4] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
    };
    int size;

    memset(&sub, 0, sizeof(sub));
    memset(&rect, 0, sizeof(rect));
    memset(indices, 0, sizeof(indices));
    indices[0] = 1;

    sub.num_rects = 1;
    sub.rects = av_malloc(sizeof(*sub.rects));
    if (!sub.rects)
        return -1;
    sub.rects[0] = &rect;
    sub.start_display_time = 0;
    sub.end_display_time   = 3000;
    sub.pts                = 0;

    rect.type       = SUBTITLE_BITMAP;
    rect.x          = 100;
    rect.y          = 800;
    rect.w          = 4;
    rect.h          = 4;
    rect.nb_colors  = 4;
    rect.data[0]    = indices;
    rect.linesize[0] = 4;
    rect.data[1]    = (uint8_t *)palette;
    rect.linesize[1] = 4 * 4;
    rect.flags      = forced_flag ? AV_SUBTITLE_FLAG_FORCED : 0;

    size = avcodec_encode_subtitle(ctx, buf, buf_size, &sub);
    av_freep(&sub.rects);
    return size;
}

int main(void)
{
    AVCodecContext *ctx;
    uint8_t *buf;
    int ret = 0, size;

    buf = av_malloc(1024 * 1024);
    if (!buf) {
        fprintf(stderr, "Failed to allocate buffer\n");
        return 1;
    }

    /*
     * Test A: rect with AV_SUBTITLE_FLAG_FORCED  -> 0x40
     */
    ctx = open_encoder(0);
    if (!ctx) {
        fprintf(stderr, "Test A: failed to open encoder\n");
        ret = 1;
        goto end;
    }
    size = encode_one(ctx, buf, 1024 * 1024, 1);
    if (size <= 0) {
        fprintf(stderr, "Test A: encode failed\n");
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pcs) {
            fprintf(stderr, "Test A: missing PCS\n");
            ret = 1;
            goto end;
        }
        if (!get_pcs_forced_flag(pcs)) {
            fprintf(stderr, "Test A: expected forced flag 0x40\n");
            ret = 1;
            goto end;
        }
        printf("Test A: rect with FORCED flag  -> PCS 0x40 OK\n");
    }
    avcodec_free_context(&ctx);

    /*
     * Test B: rect without FORCED flag  -> 0x00
     */
    ctx = open_encoder(0);
    if (!ctx) {
        fprintf(stderr, "Test B: failed to open encoder\n");
        ret = 1;
        goto end;
    }
    size = encode_one(ctx, buf, 1024 * 1024, 0);
    if (size <= 0) {
        fprintf(stderr, "Test B: encode failed\n");
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pcs) {
            fprintf(stderr, "Test B: missing PCS\n");
            ret = 1;
            goto end;
        }
        if (get_pcs_forced_flag(pcs)) {
            fprintf(stderr, "Test B: expected no forced flag\n");
            ret = 1;
            goto end;
        }
        printf("Test B: rect without FORCED flag  -> PCS 0x00 OK\n");
    }
    avcodec_free_context(&ctx);

    /*
     * Test C: force_all=1, rect without flag  -> 0x40
     */
    ctx = open_encoder(1);
    if (!ctx) {
        fprintf(stderr, "Test C: failed to open encoder\n");
        ret = 1;
        goto end;
    }
    size = encode_one(ctx, buf, 1024 * 1024, 0);
    if (size <= 0) {
        fprintf(stderr, "Test C: encode failed\n");
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pcs) {
            fprintf(stderr, "Test C: missing PCS\n");
            ret = 1;
            goto end;
        }
        if (!get_pcs_forced_flag(pcs)) {
            fprintf(stderr, "Test C: force_all=1 should set 0x40\n");
            ret = 1;
            goto end;
        }
        printf("Test C: force_all=1, no rect flag  -> PCS 0x40 OK\n");
    }
    avcodec_free_context(&ctx);
    ctx = NULL;

    printf("\nAll forced subtitle tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
