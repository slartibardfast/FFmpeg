/*
 * PGS encoder acquisition point interval test.
 *
 * Verifies that the ap_interval option promotes Normal Display Sets
 * to Acquisition Points when the interval has elapsed, and that AP
 * Display Sets contain full PDS + ODS + WDS (not delta).
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

/* Find a segment by type in encoder output */
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
                          int x, int y, int nb_colors, int64_t pts)
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
    sub->pts                = pts;

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
    uint32_t palette[4] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
    };
    AVSubtitle sub;
    AVSubtitleRect rect;

    memset(indices, 0, sizeof(indices));
    indices[0] = 1;
    indices[5] = 2;

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

    /* Set ap_interval to 1000ms (1 second) */
    av_opt_set_int(ctx->priv_data, "ap_interval", 1000, 0);

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        avcodec_free_context(&ctx);
        return 1;
    }

    /*
     * Test 1: PTS=0 — Epoch Start (first event)
     */
    setup_subtitle(&sub, &rect, indices, palette, 100, 800, 4, 0);
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 1: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pcs || pcs[10] != 0x80) {
            fprintf(stderr, "Test 1: expected Epoch Start (0x80)\n");
            ret = 1;
            goto end;
        }
        printf("Test 1: PTS=0 — Epoch Start (0x80) ✓\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 2: PTS=45000 (0.5s) — Normal (interval not elapsed)
     * Same palette, different position → position-only change
     */
    setup_subtitle(&sub, &rect, indices, palette, 200, 800, 4, 45000);
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
        if (!pcs || pcs[10] != 0x00) {
            fprintf(stderr, "Test 2: expected Normal (0x00), got 0x%02x\n",
                    pcs ? pcs[10] : -1);
            ret = 1;
            goto end;
        }
        /* Normal position-only: no PDS, no ODS */
        if (pds || ods) {
            fprintf(stderr, "Test 2: Normal should not have PDS/ODS\n");
            ret = 1;
            goto end;
        }
        printf("Test 2: PTS=45000 (0.5s) — Normal (0x00), "
               "no PDS/ODS ✓\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 3: PTS=135000 (1.5s) — Acquisition Point (interval elapsed)
     * Same palette, different position again. ap_interval=1000ms has
     * elapsed since the last AP (Epoch Start at PTS=0).
     */
    setup_subtitle(&sub, &rect, indices, palette, 300, 800, 4, 135000);
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 3: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        const uint8_t *wds = find_segment(buf, size, 0x17);
        if (!pcs || pcs[10] != 0x40) {
            fprintf(stderr, "Test 3: expected Acquisition Point (0x40), "
                    "got 0x%02x\n", pcs ? pcs[10] : -1);
            ret = 1;
            goto end;
        }
        /* AP must have full PDS + ODS + WDS */
        if (!pds) {
            fprintf(stderr, "Test 3: AP must have PDS\n");
            ret = 1;
            goto end;
        }
        if (!ods) {
            fprintf(stderr, "Test 3: AP must have ODS\n");
            ret = 1;
            goto end;
        }
        if (!wds) {
            fprintf(stderr, "Test 3: AP must have WDS\n");
            ret = 1;
            goto end;
        }
        printf("Test 3: PTS=135000 (1.5s) — Acquisition Point (0x40), "
               "full PDS+ODS+WDS ✓\n");
    }
    cleanup_subtitle(&sub);

    printf("\nAll AP interval tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
