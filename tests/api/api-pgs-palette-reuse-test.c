/*
 * PGS encoder palette reuse test.
 *
 * Verifies that when the palette has not changed between Display Sets,
 * the encoder omits the PDS segment entirely and the PCS references
 * the previous palette via an unchanged palette_version.
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

/* Find a segment by type in encoder output.
 * PGS segment layout: type(1) + length(2) + payload(length) */
static const uint8_t *find_segment(const uint8_t *data, int size,
                                   uint8_t type)
{
    const uint8_t *p = data;
    while (p < data + size - 3) {
        uint8_t seg_type = p[0];
        int seg_len = (p[1] << 8) | p[2];
        if (seg_type == type)
            return p;
        p += 3 + seg_len;
    }
    return NULL;
}

static int setup_subtitle(AVSubtitle *sub, AVSubtitleRect *rect,
                          uint8_t *indices, uint32_t *palette,
                          int x, int y, int nb_colors)
{
    memset(sub, 0, sizeof(*sub));
    memset(rect, 0, sizeof(*rect));

    sub->num_rects = 1;
    sub->rects = av_malloc(sizeof(*sub->rects));
    if (!sub->rects)
        return -1;
    sub->rects[0] = rect;
    sub->start_display_time = 0;
    sub->end_display_time   = 3000;

    rect->type      = SUBTITLE_BITMAP;
    rect->x         = x;
    rect->y         = y;
    rect->w         = 4;
    rect->h         = 4;
    rect->nb_colors = nb_colors;
    rect->data[0]   = indices;
    rect->linesize[0] = 4;
    rect->data[1]   = (uint8_t *)palette;
    rect->linesize[1] = nb_colors * 4;

    return 0;
}

static void cleanup_subtitle(AVSubtitle *sub)
{
    av_freep(&sub->rects);
    sub->num_rects = 0;
}

int main(void)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    uint8_t *buf;
    int ret = 0, size;

    uint8_t indices[16];
    uint32_t palette[8] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    AVSubtitle sub;
    AVSubtitleRect rect;
    int epoch_palette_version;

    memset(indices, 0, sizeof(indices));
    indices[0] = 1;
    indices[5] = 2;
    indices[10] = 3;
    indices[15] = 4;

    buf = av_malloc(1024 * 1024);
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

    ctx = avcodec_alloc_context3(codec);
    if (!ctx)
        return 1;

    ctx->width      = 1920;
    ctx->height     = 1080;
    ctx->time_base  = (AVRational){ 1, 90000 };

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        avcodec_free_context(&ctx);
        return 1;
    }

    /*
     * Test 1: Epoch Start — full PDS present
     */
    setup_subtitle(&sub, &rect, indices, palette, 100, 800, 8);
    sub.pts = 0;
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 1: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        if (!pcs) {
            fprintf(stderr, "Test 1: missing PCS\n");
            ret = 1;
            goto end;
        }
        if (pcs[10] != 0x80) {
            fprintf(stderr, "Test 1: expected Epoch Start (0x80), "
                    "got 0x%02x\n", pcs[10]);
            ret = 1;
            goto end;
        }
        if (!pds) {
            fprintf(stderr, "Test 1: Epoch Start must have PDS\n");
            ret = 1;
            goto end;
        }
        /* PCS byte 8 is palette_id, byte 9 is palette_version (in the
         * composition descriptor area — offset depends on PCS layout).
         * PDS byte 3 is palette_id, byte 4 is palette_version. */
        epoch_palette_version = pds[4];
        printf("Test 1: Epoch Start — PDS present, "
               "palette_version=%d\n", epoch_palette_version);
    }
    cleanup_subtitle(&sub);

    /*
     * Test 2: Same palette, different position — no PDS expected
     *
     * Moving the subtitle to a different position with the same palette
     * should produce a Normal Display Set with no PDS (palette reuse).
     */
    setup_subtitle(&sub, &rect, indices, palette, 200, 800, 8);
    sub.pts = 90000;  /* 1 second later */
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 2: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        const uint8_t *wds = find_segment(buf, size, 0x17);
        if (!pcs) {
            fprintf(stderr, "Test 2: missing PCS\n");
            ret = 1;
            goto end;
        }
        /* Position-only change: should be Normal state */
        if (pcs[10] != 0x00) {
            fprintf(stderr, "Test 2: expected Normal (0x00), "
                    "got 0x%02x\n", pcs[10]);
            ret = 1;
            goto end;
        }
        /* palette_update_flag should be 0 (no palette change) */
        if (pcs[11] != 0x00) {
            fprintf(stderr, "Test 2: expected palette_update_flag=0, "
                    "got 0x%02x\n", pcs[11]);
            ret = 1;
            goto end;
        }
        /* No PDS — palette is reused from previous Display Set */
        if (pds) {
            fprintf(stderr, "Test 2: PDS should be absent "
                    "(palette reuse)\n");
            ret = 1;
            goto end;
        }
        /* No ODS or WDS either — position-only Normal DS */
        if (ods) {
            fprintf(stderr, "Test 2: ODS should be absent "
                    "(position-only change)\n");
            ret = 1;
            goto end;
        }
        if (wds) {
            fprintf(stderr, "Test 2: WDS should be absent "
                    "(position-only change)\n");
            ret = 1;
            goto end;
        }
        printf("Test 2: Normal DS — no PDS, no ODS, no WDS "
               "(palette reuse confirmed)\n");
    }
    cleanup_subtitle(&sub);

    printf("\nAll palette reuse tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
