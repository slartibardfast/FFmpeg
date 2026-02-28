/*
 * PGS encoder composition state machine test.
 *
 * Constructs AVSubtitle structs and calls avcodec_encode_subtitle()
 * multiple times to exercise every state transition in pgs_determine_state().
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

/* Helper to find a segment by type in encoder output */
static const uint8_t *find_segment(const uint8_t *data, int size, uint8_t type)
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

/* Create a small 4x4 bitmap with a given palette */
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
    uint8_t buf[1024 * 1024];
    int ret, size;

    /* Small 4x4 bitmap data */
    uint8_t indices[16];
    uint32_t palette_a[4] = { 0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
    uint32_t palette_b[4] = { 0x80000000, 0x80FF0000, 0x8000FF00, 0x800000FF };
    AVSubtitle sub;
    AVSubtitleRect rect;

    memset(indices, 0, sizeof(indices));
    indices[0] = 1;
    indices[5] = 2;
    indices[10] = 3;

    codec = avcodec_find_encoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    if (!codec) {
        fprintf(stderr, "PGS encoder not found\n");
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
     * Test 1: First subtitle -> Epoch Start (0x80)
     */
    setup_subtitle(&sub, &rect, indices, palette_a, 100, 800, 4);
    size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 1: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        if (!pcs || !pds || !ods) {
            fprintf(stderr, "Test 1: missing PCS/PDS/ODS segments\n");
            ret = 1;
            goto end;
        }
        /* PCS byte 10 = composition_state */
        if (pcs[10] != 0x80) {
            fprintf(stderr, "Test 1: expected "
                    "composition_state=0x80, got 0x%02x\n",
                    pcs[10]);
            ret = 1;
            goto end;
        }
        /* palette_update_flag should be 0 for Epoch Start */
        if (pcs[11] != 0x00) {
            fprintf(stderr, "Test 1: expected "
                    "palette_update_flag=0x00, got 0x%02x\n",
                    pcs[11]);
            ret = 1;
            goto end;
        }
        /* PDS palette_version = 0 */
        if (pds[4] != 0x00) {
            fprintf(stderr, "Test 1: expected "
                    "palette_version=0, got %d\n", pds[4]);
            ret = 1;
            goto end;
        }
        printf("Test 1 PASS: Epoch Start, "
               "composition_state=0x80, palette_version=0\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 2: Same position, different palette -> Normal + palette_update
     */
    setup_subtitle(&sub, &rect, indices, palette_b, 100, 800, 4);
    size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 2: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        if (!pcs || !pds) {
            fprintf(stderr, "Test 2: missing PCS/PDS segments\n");
            ret = 1;
            goto end;
        }
        /* Normal composition */
        if (pcs[10] != 0x00) {
            fprintf(stderr, "Test 2: expected "
                    "composition_state=0x00, got 0x%02x\n",
                    pcs[10]);
            ret = 1;
            goto end;
        }
        /* palette_update_flag set */
        if (pcs[11] != 0x80) {
            fprintf(stderr, "Test 2: expected "
                    "palette_update_flag=0x80, got 0x%02x\n",
                    pcs[11]);
            ret = 1;
            goto end;
        }
        /* No ODS for palette-only update */
        if (ods) {
            fprintf(stderr, "Test 2: unexpected ODS in palette-only update\n");
            ret = 1;
            goto end;
        }
        /* PDS palette_version should be 1 */
        if (pds[4] != 0x01) {
            fprintf(stderr, "Test 2: expected "
                    "palette_version=1, got %d\n", pds[4]);
            ret = 1;
            goto end;
        }
        printf("Test 2 PASS: Normal palette_update, composition_state=0x00, "
               "palette_update_flag=0x80, palette_version=1\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 3: Same palette, different position -> Normal (no palette_update)
     */
    setup_subtitle(&sub, &rect, indices, palette_b, 200, 700, 4);
    size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 3: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        if (!pcs) {
            fprintf(stderr, "Test 3: missing PCS segment\n");
            ret = 1;
            goto end;
        }
        /* Normal, no palette update */
        if (pcs[10] != 0x00) {
            fprintf(stderr, "Test 3: expected "
                    "composition_state=0x00, got 0x%02x\n",
                    pcs[10]);
            ret = 1;
            goto end;
        }
        if (pcs[11] != 0x00) {
            fprintf(stderr, "Test 3: expected "
                    "palette_update_flag=0x00, got 0x%02x\n",
                    pcs[11]);
            ret = 1;
            goto end;
        }
        /* No PDS when palette unchanged */
        if (pds) {
            fprintf(stderr, "Test 3: unexpected PDS when palette unchanged\n");
            ret = 1;
            goto end;
        }
        printf("Test 3 PASS: Normal position update, "
               "no palette_update, no PDS\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 4: Clear (num_rects=0) -> display removal
     */
    memset(&sub, 0, sizeof(sub));
    sub.num_rects = 0;
    sub.rects = NULL;
    sub.start_display_time = 0;
    sub.end_display_time   = 0;
    size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 4: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        if (!pcs) {
            fprintf(stderr, "Test 4: missing PCS segment\n");
            ret = 1;
            goto end;
        }
        /* num_composition_objects = 0 */
        if (pcs[13] != 0x00) {
            fprintf(stderr, "Test 4: expected 0 composition objects, got %d\n",
                    pcs[13]);
            ret = 1;
            goto end;
        }
        if (ods) {
            fprintf(stderr, "Test 4: unexpected ODS in clear Display Set\n");
            ret = 1;
            goto end;
        }
        printf("Test 4 PASS: Clear Display Set, 0 composition objects\n");
    }

    /*
     * Test 5: New subtitle after clear -> new Epoch Start (0x80)
     */
    setup_subtitle(&sub, &rect, indices, palette_a, 300, 900, 4);
    size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 5: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *ods = find_segment(buf, size, 0x15);
        if (!pcs || !pds || !ods) {
            fprintf(stderr, "Test 5: missing PCS/PDS/ODS segments\n");
            ret = 1;
            goto end;
        }
        if (pcs[10] != 0x80) {
            fprintf(stderr, "Test 5: expected "
                    "composition_state=0x80, got 0x%02x\n",
                    pcs[10]);
            ret = 1;
            goto end;
        }
        /* palette_version reset to 0 for new epoch */
        if (pds[4] != 0x00) {
            fprintf(stderr, "Test 5: expected "
                    "palette_version=0, got %d\n", pds[4]);
            ret = 1;
            goto end;
        }
        printf("Test 5 PASS: New Epoch Start after clear, "
               "palette_version reset to 0\n");
    }
    cleanup_subtitle(&sub);

    /*
     * Test 6: Multiple palette updates -> palette_version increments
     *
     * Clear first to reset epoch, then Epoch Start + 4 palette updates.
     * palette_version: 0, 1, 2, 3, 4
     */
    {
        int step;
        uint32_t pal[4];

        /* Clear to reset epoch state */
        memset(&sub, 0, sizeof(sub));
        sub.num_rects = 0;
        sub.rects = NULL;
        avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);

        for (step = 0; step < 5; step++) {
            /* Each step uses different alpha to force palette change */
            pal[0] = ((step * 30 + 20) << 24) | 0x000000;
            pal[1] = ((step * 30 + 20) << 24) | 0xFF0000;
            pal[2] = ((step * 30 + 20) << 24) | 0x00FF00;
            pal[3] = ((step * 30 + 20) << 24) | 0x0000FF;

            setup_subtitle(&sub, &rect, indices, pal, 400, 500, 4);
            size = avcodec_encode_subtitle(ctx, buf, sizeof(buf), &sub);
            if (size <= 0) {
                fprintf(stderr, "Test 6 step %d: "
                        "encode failed (%d)\n", step, size);
                ret = 1;
                goto end;
            }

            {
                const uint8_t *pds = find_segment(buf, size, 0x14);
                if (!pds) {
                    fprintf(stderr, "Test 6 step %d: missing PDS\n", step);
                    ret = 1;
                    goto end;
                }
                /* Step 0: Epoch Start with palette_version=0
                 * Step N: palette_update with palette_version=N */
                if (pds[4] != step) {
                    fprintf(stderr, "Test 6 step %d: "
                            "expected palette_version=%d, "
                            "got %d\n", step, step, pds[4]);
                    ret = 1;
                    goto end;
                }
            }
            cleanup_subtitle(&sub);
        }
        printf("Test 6 PASS: palette_version increments "
               "correctly over %d steps\n",
               5);
    }

    printf("\nAll tests passed.\n");
    ret = 0;

end:
    avcodec_free_context(&ctx);
    return ret;
}
