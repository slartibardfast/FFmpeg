/*
 * PGS encoder overlap composition state test.
 *
 * Encodes a sequence of overlapping subtitles with different
 * bitmap dimensions followed by clear events, and verifies the
 * composition state (Epoch Start / Normal) and composition object
 * count in each Display Set's PCS segment.
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

#define PGS_EPOCH_START  0x80
#define PGS_NORMAL       0x00

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
                          int x, int y, int w, int h,
                          unsigned start_ms, unsigned end_ms)
{
    memset(sub, 0, sizeof(*sub));
    memset(rect, 0, sizeof(*rect));

    sub->num_rects = 1;
    sub->rects = av_malloc(sizeof(*sub->rects));
    if (!sub->rects)
        return -1;
    sub->rects[0] = rect;
    sub->start_display_time = start_ms;
    sub->end_display_time   = end_ms;

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

static int setup_clear(AVSubtitle *sub)
{
    memset(sub, 0, sizeof(*sub));
    sub->num_rects = 0;
    sub->rects     = NULL;
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

    /* Bitmap A: 32x16 */
    uint8_t indices_a[32 * 16];
    uint32_t palette_a[4] = {
        0xFF000000, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF
    };

    /* Bitmap B: 48x16 (different width triggers new epoch) */
    uint8_t indices_b[48 * 16];
    uint32_t palette_b[4] = {
        0xFF000000, 0xFF00FFFF, 0xFFFF00FF, 0xFFFFFF00
    };

    AVSubtitle sub;
    AVSubtitleRect rect;

    struct {
        const char *label;
        int expected_state;
        int expected_objects;
    } checks[4] = {
        { "DS1 (sub A)",  PGS_EPOCH_START, 1 },
        { "DS2 (sub B)",  PGS_EPOCH_START, 1 },
        { "DS3 (clear)",  PGS_NORMAL,      0 },
        { "DS4 (clear)",  PGS_NORMAL,      0 },
    };

    memset(indices_a, 1, sizeof(indices_a));
    memset(indices_b, 2, sizeof(indices_b));

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
    if (!ctx) {
        av_free(buf);
        return 1;
    }

    ctx->width     = 640;
    ctx->height    = 360;
    ctx->time_base = (AVRational){ 1, 90000 };

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        avcodec_free_context(&ctx);
        av_free(buf);
        return 1;
    }

    /*
     * DS 1: Subtitle A at PTS=90000 (1s), 4s duration, 32x16 bitmap.
     * First encode -> Epoch Start, 1 object.
     */
    setup_subtitle(&sub, &rect, indices_a, palette_a,
                   10, 300, 32, 16, 0, 4000);
    sub.pts = 90000;
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "DS1: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        int state, num_obj;
        if (!pcs) {
            fprintf(stderr, "DS1: missing PCS\n");
            ret = 1;
            goto end;
        }
        state   = pcs[10];
        num_obj = pcs[13];
        printf("DS1: state=0x%02x objects=%d\n", state, num_obj);
        if (state != checks[0].expected_state) {
            fprintf(stderr, "DS1: expected state 0x%02x, got 0x%02x\n",
                    checks[0].expected_state, state);
            ret = 1;
            goto end;
        }
        if (num_obj != checks[0].expected_objects) {
            fprintf(stderr, "DS1: expected %d objects, got %d\n",
                    checks[0].expected_objects, num_obj);
            ret = 1;
            goto end;
        }
    }
    cleanup_subtitle(&sub);

    /*
     * DS 2: Subtitle B at PTS=270000 (3s), 4s duration, 48x16 bitmap.
     * Different dimensions from A -> Epoch Start, 1 object.
     */
    setup_subtitle(&sub, &rect, indices_b, palette_b,
                   10, 200, 48, 16, 0, 4000);
    sub.pts = 270000;
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "DS2: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        int state, num_obj;
        if (!pcs) {
            fprintf(stderr, "DS2: missing PCS\n");
            ret = 1;
            goto end;
        }
        state   = pcs[10];
        num_obj = pcs[13];
        printf("DS2: state=0x%02x objects=%d\n", state, num_obj);
        if (state != checks[1].expected_state) {
            fprintf(stderr, "DS2: expected state 0x%02x, got 0x%02x\n",
                    checks[1].expected_state, state);
            ret = 1;
            goto end;
        }
        if (num_obj != checks[1].expected_objects) {
            fprintf(stderr, "DS2: expected %d objects, got %d\n",
                    checks[1].expected_objects, num_obj);
            ret = 1;
            goto end;
        }
    }
    cleanup_subtitle(&sub);

    /*
     * DS 3: Clear at PTS=450000 (5s).
     * Epoch active -> Normal, 0 objects.
     */
    setup_clear(&sub);
    sub.pts = 450000;
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "DS3: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        int state, num_obj;
        if (!pcs) {
            fprintf(stderr, "DS3: missing PCS\n");
            ret = 1;
            goto end;
        }
        state   = pcs[10];
        num_obj = pcs[13];
        printf("DS3: state=0x%02x objects=%d\n", state, num_obj);
        if (state != checks[2].expected_state) {
            fprintf(stderr, "DS3: expected state 0x%02x, got 0x%02x\n",
                    checks[2].expected_state, state);
            ret = 1;
            goto end;
        }
        if (num_obj != checks[2].expected_objects) {
            fprintf(stderr, "DS3: expected %d objects, got %d\n",
                    checks[2].expected_objects, num_obj);
            ret = 1;
            goto end;
        }
    }

    /*
     * DS 4: Clear at PTS=630000 (7s).
     * Epoch already inactive -> Normal, 0 objects.
     */
    setup_clear(&sub);
    sub.pts = 630000;
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "DS4: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        int state, num_obj;
        if (!pcs) {
            fprintf(stderr, "DS4: missing PCS\n");
            ret = 1;
            goto end;
        }
        state   = pcs[10];
        num_obj = pcs[13];
        printf("DS4: state=0x%02x objects=%d\n", state, num_obj);
        if (state != checks[3].expected_state) {
            fprintf(stderr, "DS4: expected state 0x%02x, got 0x%02x\n",
                    checks[3].expected_state, state);
            ret = 1;
            goto end;
        }
        if (num_obj != checks[3].expected_objects) {
            fprintf(stderr, "DS4: expected %d objects, got %d\n",
                    checks[3].expected_objects, num_obj);
            ret = 1;
            goto end;
        }
    }

    printf("\nAll overlap composition state tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
