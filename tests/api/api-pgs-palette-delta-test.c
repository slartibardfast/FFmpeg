/*
 * PGS encoder palette delta encoding test.
 *
 * Encodes a sequence of subtitles and verifies that the PDS segment
 * uses delta encoding for Normal Display Sets: only changed palette
 * entries are written, reducing PDS size compared to the initial
 * Epoch Start which contains a full palette.
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

/* Return the total PDS segment size (3-byte header + payload) */
static int pds_total_size(const uint8_t *pds)
{
    if (!pds)
        return 0;
    return 3 + ((pds[1] << 8) | pds[2]);
}

/* Return the number of palette entries in a PDS segment.
 * PDS payload: palette_id(1) + version(1) + N * 5-byte entries */
static int pds_entry_count(const uint8_t *pds)
{
    int payload;
    if (!pds)
        return 0;
    payload = (pds[1] << 8) | pds[2];
    return (payload - 2) / 5;
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
    /* 8 distinct non-transparent colors */
    uint32_t palette_full[8] = {
        0xFF101010, 0xFFFF0000, 0xFF00FF00, 0xFF0000FF,
        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    /* Same palette but with one entry changed (index 1) */
    uint32_t palette_one_changed[8] = {
        0xFF101010, 0xFF800000, 0xFF00FF00, 0xFF0000FF,
        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    /* Two entries changed (indices 1 and 3) */
    uint32_t palette_two_changed[8] = {
        0xFF101010, 0xFF400000, 0xFF00FF00, 0xFF000080,
        0xFFFFFF00, 0xFFFF00FF, 0xFF00FFFF, 0xFFFFFFFF,
    };
    AVSubtitle sub;
    AVSubtitleRect rect;
    int epoch_entries, delta1_entries, delta2_entries;
    int epoch_size, delta1_size, delta2_size;

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
     * Test 1: Epoch Start — full palette (all 8 non-transparent entries)
     */
    setup_subtitle(&sub, &rect, indices, palette_full, 100, 800, 8);
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 1: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pds || !pcs) {
            fprintf(stderr, "Test 1: missing PDS or PCS\n");
            ret = 1;
            goto end;
        }
        if (pcs[10] != 0x80) {
            fprintf(stderr, "Test 1: expected Epoch Start, got 0x%02x\n",
                    pcs[10]);
            ret = 1;
            goto end;
        }
        epoch_entries = pds_entry_count(pds);
        epoch_size    = pds_total_size(pds);
        printf("Test 1: Epoch Start PDS: %d entries, %d bytes total\n",
               epoch_entries, epoch_size);
        /* All 8 non-transparent entries should be present */
        if (epoch_entries < 7) {
            fprintf(stderr, "Test 1: expected >= 7 PDS entries, got %d\n",
                    epoch_entries);
            ret = 1;
            goto end;
        }
    }
    cleanup_subtitle(&sub);

    /*
     * Test 2: Palette update with 1 entry changed — delta PDS
     */
    setup_subtitle(&sub, &rect, indices, palette_one_changed, 100, 800, 8);
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 2: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pds || !pcs) {
            fprintf(stderr, "Test 2: missing PDS or PCS\n");
            ret = 1;
            goto end;
        }
        /* Should be palette_update */
        if (pcs[11] != 0x80) {
            fprintf(stderr, "Test 2: expected palette_update_flag, "
                    "got 0x%02x\n", pcs[11]);
            ret = 1;
            goto end;
        }
        delta1_entries = pds_entry_count(pds);
        delta1_size    = pds_total_size(pds);
        printf("Test 2: Delta PDS (1 changed): %d entries, %d bytes\n",
               delta1_entries, delta1_size);
        /* Only the changed entry should be written */
        if (delta1_entries != 1) {
            fprintf(stderr, "Test 2: expected 1 delta entry, got %d\n",
                    delta1_entries);
            ret = 1;
            goto end;
        }
        if (delta1_size >= epoch_size) {
            fprintf(stderr, "Test 2: delta PDS (%d) should be smaller "
                    "than epoch PDS (%d)\n", delta1_size, epoch_size);
            ret = 1;
            goto end;
        }
    }
    cleanup_subtitle(&sub);

    /*
     * Test 3: Palette update with 2 entries changed
     */
    setup_subtitle(&sub, &rect, indices, palette_two_changed, 100, 800, 8);
    size = avcodec_encode_subtitle(ctx, buf, 1024 * 1024, &sub);
    if (size <= 0) {
        fprintf(stderr, "Test 3: encode failed (%d)\n", size);
        ret = 1;
        goto end;
    }
    {
        const uint8_t *pds = find_segment(buf, size, 0x14);
        const uint8_t *pcs = find_segment(buf, size, 0x16);
        if (!pds || !pcs) {
            fprintf(stderr, "Test 3: missing PDS or PCS\n");
            ret = 1;
            goto end;
        }
        if (pcs[11] != 0x80) {
            fprintf(stderr, "Test 3: expected palette_update_flag, "
                    "got 0x%02x\n", pcs[11]);
            ret = 1;
            goto end;
        }
        delta2_entries = pds_entry_count(pds);
        delta2_size    = pds_total_size(pds);
        printf("Test 3: Delta PDS (2 changed): %d entries, %d bytes\n",
               delta2_entries, delta2_size);
        if (delta2_entries != 2) {
            fprintf(stderr, "Test 3: expected 2 delta entries, got %d\n",
                    delta2_entries);
            ret = 1;
            goto end;
        }
        if (delta2_size >= epoch_size) {
            fprintf(stderr, "Test 3: delta PDS (%d) should be smaller "
                    "than epoch PDS (%d)\n", delta2_size, epoch_size);
            ret = 1;
            goto end;
        }
        /* 2-entry delta should be larger than 1-entry delta */
        if (delta2_size <= delta1_size) {
            fprintf(stderr, "Test 3: 2-entry delta (%d) should be "
                    "larger than 1-entry delta (%d)\n",
                    delta2_size, delta1_size);
            ret = 1;
            goto end;
        }
    }
    cleanup_subtitle(&sub);

    printf("\nAll palette delta tests passed.\n");

end:
    avcodec_free_context(&ctx);
    av_free(buf);
    return ret;
}
