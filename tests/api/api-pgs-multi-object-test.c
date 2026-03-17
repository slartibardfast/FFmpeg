/*
 * PGS encoder multi-object test.
 *
 * Verifies that the encoder correctly handles two non-overlapping
 * composition objects in a single Display Set: two ODS segments,
 * two window definitions in WDS, and two composition descriptors
 * in the PCS.
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

/* Find the Nth segment of a given type (0-indexed) */
static const uint8_t *find_nth_segment(const uint8_t *data, int size,
                                       uint8_t type, int n)
{
    const uint8_t *p = data;
    int count = 0;
    while (p < data + size - 3) {
        uint8_t seg_type = p[0];
        int seg_len = (p[1] << 8) | p[2];
        if (seg_type == type) {
            if (count == n)
                return p;
            count++;
        }
        p += 3 + seg_len;
    }
    return NULL;
}

/* Count segments of a given type */
static int count_segments(const uint8_t *data, int size, uint8_t type)
{
    const uint8_t *p = data;
    int count = 0;
    while (p < data + size - 3) {
        uint8_t seg_type = p[0];
        int seg_len = (p[1] << 8) | p[2];
        if (seg_type == type)
            count++;
        p += 3 + seg_len;
    }
    return count;
}

int main(void)
{
    const AVCodec *codec;
    AVCodecContext *ctx = NULL;
    uint8_t *buf;
    int ret = 0, size;

    /* Two 4x4 bitmaps at different positions */
    uint8_t indices_top[16], indices_bot[16];
    uint32_t palette[4] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
    };
    AVSubtitle sub;
    AVSubtitleRect rect_top, rect_bot;
    AVSubtitleRect *rects[2];

    memset(indices_top, 0, sizeof(indices_top));
    memset(indices_bot, 0, sizeof(indices_bot));
    indices_top[0] = 1;
    indices_top[5] = 2;
    indices_bot[0] = 2;
    indices_bot[5] = 3;

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
     * Construct a subtitle with 2 non-overlapping rects:
     * rect_top at y=50 (top of screen), rect_bot at y=900 (bottom)
     */
    memset(&sub, 0, sizeof(sub));
    memset(&rect_top, 0, sizeof(rect_top));
    memset(&rect_bot, 0, sizeof(rect_bot));

    rect_top.type       = SUBTITLE_BITMAP;
    rect_top.x          = 100;
    rect_top.y          = 50;
    rect_top.w          = 4;
    rect_top.h          = 4;
    rect_top.nb_colors  = 4;
    rect_top.data[0]    = indices_top;
    rect_top.linesize[0] = 4;
    rect_top.data[1]    = (uint8_t *)palette;
    rect_top.linesize[1] = 4 * 4;

    rect_bot.type       = SUBTITLE_BITMAP;
    rect_bot.x          = 100;
    rect_bot.y          = 900;
    rect_bot.w          = 4;
    rect_bot.h          = 4;
    rect_bot.nb_colors  = 4;
    rect_bot.data[0]    = indices_bot;
    rect_bot.linesize[0] = 4;
    rect_bot.data[1]    = (uint8_t *)palette;
    rect_bot.linesize[1] = 4 * 4;

    rects[0] = &rect_top;
    rects[1] = &rect_bot;

    sub.num_rects            = 2;
    sub.rects                = rects;
    sub.start_display_time   = 0;
    sub.end_display_time     = 3000;
    sub.pts                  = 0;

    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }

    /* Verify PCS has 2 composition objects */
    {
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        int num_objs;
        if (!pcs) {
            fprintf(stderr, "Missing PCS\n");
            ret = 1;
            goto end;
        }
        /* PCS layout: video_w(2) + video_h(2) + frame_rate(1) +
         * composition_number(2) + composition_state(1) +
         * palette_update_flag(1) + palette_id(1) +
         * num_composition_objects(1) = offset 13 from segment start */
        num_objs = pcs[13];
        if (num_objs != 2) {
            fprintf(stderr, "Expected 2 composition objects, got %d\n",
                    num_objs);
            ret = 1;
            goto end;
        }
        /* Verify object_ids are 0 and 1 in the composition descriptors.
         * Each descriptor: object_id(2) + window_id(1) +
         * composition_flag(1) + x(2) + y(2) = 8 bytes.
         * First descriptor starts at offset 14. */
        {
            int obj_id_0 = (pcs[14] << 8) | pcs[15];
            int obj_id_1 = (pcs[22] << 8) | pcs[23];
            if (obj_id_0 != 0 || obj_id_1 != 1) {
                fprintf(stderr, "Expected object_ids 0,1 — got %d,%d\n",
                        obj_id_0, obj_id_1);
                ret = 1;
                goto end;
            }
        }
        printf("PCS: %d composition objects, object_ids 0 and 1\n",
               num_objs);
    }

    /* Verify 2 ODS segments */
    {
        int ods_count = count_segments(buf, size, 0x15);
        if (ods_count != 2) {
            fprintf(stderr, "Expected 2 ODS segments, got %d\n",
                    ods_count);
            ret = 1;
            goto end;
        }
        /* Verify object_ids in ODS: first ODS has id=0, second id=1 */
        {
            const uint8_t *ods0 = find_nth_segment(buf, size, 0x15, 0);
            const uint8_t *ods1 = find_nth_segment(buf, size, 0x15, 1);
            int ods0_id = (ods0[3] << 8) | ods0[4];
            int ods1_id = (ods1[3] << 8) | ods1[4];
            if (ods0_id != 0 || ods1_id != 1) {
                fprintf(stderr, "ODS object_ids: expected 0,1 — "
                        "got %d,%d\n", ods0_id, ods1_id);
                ret = 1;
                goto end;
            }
        }
        printf("ODS: %d segments, object_ids 0 and 1\n", ods_count);
    }

    /* Verify WDS has 2 window definitions */
    {
        const uint8_t *wds = find_segment(buf, size, 0x17);
        int num_windows;
        if (!wds) {
            fprintf(stderr, "Missing WDS\n");
            ret = 1;
            goto end;
        }
        /* WDS layout: num_windows(1) + N * (window_id(1) + x(2) +
         * y(2) + w(2) + h(2)) */
        num_windows = wds[3];
        if (num_windows != 2) {
            fprintf(stderr, "Expected 2 windows, got %d\n", num_windows);
            ret = 1;
            goto end;
        }
        printf("WDS: %d window definitions\n", num_windows);
    }

    /* Verify single PDS (shared palette) */
    {
        int pds_count = count_segments(buf, size, 0x14);
        if (pds_count != 1) {
            fprintf(stderr, "Expected 1 PDS (shared palette), got %d\n",
                    pds_count);
            ret = 1;
            goto end;
        }
        printf("PDS: 1 segment (shared palette)\n");
    }

    printf("\nAll multi-object tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
