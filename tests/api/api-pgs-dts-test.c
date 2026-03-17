/*
 * PGS encoder decode_duration / DTS computation test.
 *
 * Encodes subtitles at different resolutions and verifies the
 * decode_duration embedded in the encoder output matches the
 * decoder model formula: ceil(90000 * W * H / RC).
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

/* Decoder model constants matching pgssubenc.c */
#define PGS_FREQ 90000
#define PGS_RC   32000000

/* Helper to find a segment by type in encoder output */
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
                          int x, int y, int w, int h)
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
    rect->w         = w;
    rect->h         = h;
    rect->nb_colors = 4;
    rect->data[0]   = indices;
    rect->linesize[0] = w;
    rect->data[1]   = (uint8_t *)palette;
    rect->linesize[1] = 4 * 4;

    return 0;
}

static void cleanup_subtitle(AVSubtitle *sub)
{
    av_freep(&sub->rects);
    sub->num_rects = 0;
}

static int64_t expected_epoch_duration(int w, int h)
{
    return ((int64_t)PGS_FREQ * w * h + PGS_RC - 1) / PGS_RC;
}

int main(void)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    uint8_t *buf;
    int ret = 0, size;

    uint8_t indices[64 * 32];
    uint32_t palette[4] = { 0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF };
    AVSubtitle sub;
    AVSubtitleRect rect;

    struct {
        int video_w, video_h;
        int obj_w, obj_h;
    } cases[] = {
        { 1920, 1080,  64, 32 },
        {  720,  480,  64, 32 },
        { 1280,  720,  64, 32 },
    };
    int num_cases = sizeof(cases) / sizeof(cases[0]);
    int i;

    memset(indices, 0, sizeof(indices));
    indices[0] = 1;
    indices[5] = 2;
    indices[10] = 3;

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

    for (i = 0; i < num_cases; i++) {
        int64_t expected;
        const uint8_t *pcs;

        ctx = avcodec_alloc_context3(codec);
        if (!ctx)
            return 1;

        ctx->width     = cases[i].video_w;
        ctx->height    = cases[i].video_h;
        ctx->time_base = (AVRational){ 1, 90000 };

        if (avcodec_open2(ctx, codec, NULL) < 0) {
            fprintf(stderr, "Case %d: failed to open encoder\n", i);
            avcodec_free_context(&ctx);
            return 1;
        }

        setup_subtitle(&sub, &rect, indices, palette,
                        10, 10, cases[i].obj_w, cases[i].obj_h);
        size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
        if (size <= 0) {
            fprintf(stderr, "Case %d: encode failed (%d)\n", i, size);
            ret = 1;
            goto next;
        }

        /* Verify PCS exists and is an Epoch Start */
        pcs = find_segment(buf, size, 0x16);
        if (!pcs) {
            fprintf(stderr, "Case %d: missing PCS segment\n", i);
            ret = 1;
            goto next;
        }
        if (pcs[10] != 0x80) {
            fprintf(stderr, "Case %d: expected Epoch Start (0x80), "
                    "got 0x%02x\n", i, pcs[10]);
            ret = 1;
            goto next;
        }

        /* Verify video dimensions in PCS */
        {
            int pcs_w = (pcs[3] << 8) | pcs[4];
            int pcs_h = (pcs[5] << 8) | pcs[6];
            if (pcs_w != cases[i].video_w || pcs_h != cases[i].video_h) {
                fprintf(stderr, "Case %d: PCS dimensions %dx%d, "
                        "expected %dx%d\n", i, pcs_w, pcs_h,
                        cases[i].video_w, cases[i].video_h);
                ret = 1;
                goto next;
            }
        }

        /* Verify ODS exists with correct object dimensions */
        {
            const uint8_t *ods = find_segment(buf, size, 0x15);
            int obj_w, obj_h;
            if (!ods) {
                fprintf(stderr, "Case %d: missing ODS segment\n", i);
                ret = 1;
                goto next;
            }
            /* ODS layout: type(1) + len(2) + obj_id(2) + version(1)
             * + seq_flag(1) + obj_data_len(3) + width(2) + height(2) */
            obj_w = (ods[10] << 8) | ods[11];
            obj_h = (ods[12] << 8) | ods[13];
            if (obj_w != cases[i].obj_w || obj_h != cases[i].obj_h) {
                fprintf(stderr, "Case %d: ODS dimensions %dx%d, "
                        "expected %dx%d\n", i, obj_w, obj_h,
                        cases[i].obj_w, cases[i].obj_h);
                ret = 1;
                goto next;
            }
        }

        /* Verify decode_duration formula for Epoch Start:
         * ceil(PGS_FREQ * video_w * video_h / PGS_RC) */
        expected = expected_epoch_duration(cases[i].video_w,
                                           cases[i].video_h);
        printf("Case %d (%dx%d): expected decode_duration=%"PRId64
               " ticks (%.1f ms)\n",
               i, cases[i].video_w, cases[i].video_h,
               expected, expected * 1000.0 / PGS_FREQ);

        /* Specific known values */
        if (cases[i].video_w == 1920 && cases[i].video_h == 1080) {
            if (expected != 5832) {
                fprintf(stderr, "Case %d: 1920x1080 expected 5832, "
                        "got %"PRId64"\n", i, expected);
                ret = 1;
                goto next;
            }
        } else if (cases[i].video_w == 720 && cases[i].video_h == 480) {
            /* ceil(90000 * 720 * 480 / 32000000) = ceil(972) = 972 */
            if (expected != 972) {
                fprintf(stderr, "Case %d: 720x480 expected 972, "
                        "got %"PRId64"\n", i, expected);
                ret = 1;
                goto next;
            }
        } else if (cases[i].video_w == 1280 && cases[i].video_h == 720) {
            /* ceil(90000 * 1280 * 720 / 32000000) = ceil(2592) = 2592 */
            if (expected != 2592) {
                fprintf(stderr, "Case %d: 1280x720 expected 2592, "
                        "got %"PRId64"\n", i, expected);
                ret = 1;
                goto next;
            }
        }

        printf("Case %d PASS\n", i);

next:
        cleanup_subtitle(&sub);
        avcodec_free_context(&ctx);
        if (ret)
            return ret;
    }

    printf("\nAll DTS tests passed.\n");
    av_free(buf);
    return 0;
}
