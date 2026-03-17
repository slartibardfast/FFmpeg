/*
 * Shared test utilities for PGS encoder FATE tests.
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

#ifndef PGS_TEST_UTIL_H
#define PGS_TEST_UTIL_H

#include <string.h>
#include "libavcodec/avcodec.h"
#include "libavutil/mem.h"

/* Find the first segment of a given type in encoder output.
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

/* Set up a single-rect bitmap subtitle */
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

#endif /* PGS_TEST_UTIL_H */
