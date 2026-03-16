/*
 * Subtitle bitmap utility functions.
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

#include "sub_util.h"

int64_t ff_sub_alpha_sum(const uint8_t *rgba, int w, int h,
                         int linesize)
{
    int64_t sum = 0;
    int x, y;

    for (y = 0; y < h; y++) {
        const uint8_t *row = rgba + y * linesize;
        for (x = 0; x < w; x++)
            sum += row[x * 4 + 3];
    }
    return sum;
}

enum FFSubChangeType ff_sub_classify_change(
    const uint8_t *rgba0, int x0, int y0, int w0, int h0, int ls0,
    const uint8_t *rgba1, int x1, int y1, int w1, int h1, int ls1)
{
    int x, y;
    int position_differs, has_alpha_diff, has_rgb_diff;

    if (!rgba0 && !rgba1)
        return FF_SUB_CHANGE_NONE;
    if (!rgba0 || !rgba1)
        return FF_SUB_CHANGE_CONTENT;

    if (w0 != w1 || h0 != h1)
        return FF_SUB_CHANGE_CONTENT;

    position_differs = (x0 != x1 || y0 != y1);

    has_alpha_diff = 0;
    has_rgb_diff   = 0;
    for (y = 0; y < h0; y++) {
        const uint8_t *p0 = rgba0 + y * ls0;
        const uint8_t *p1 = rgba1 + y * ls1;
        for (x = 0; x < w0; x++) {
            uint8_t r0 = p0[x * 4],     r1 = p1[x * 4];
            uint8_t g0 = p0[x * 4 + 1], g1 = p1[x * 4 + 1];
            uint8_t b0 = p0[x * 4 + 2], b1 = p1[x * 4 + 2];
            uint8_t a0 = p0[x * 4 + 3], a1 = p1[x * 4 + 3];

            if (a0 == 0 && a1 == 0)
                continue;

            if (a0 == 0 || a1 == 0) {
                has_alpha_diff = 1;
                continue;
            }

            {
                int dr = r0 > r1 ? r0 - r1 : r1 - r0;
                int dg = g0 > g1 ? g0 - g1 : g1 - g0;
                int db = b0 > b1 ? b0 - b1 : b1 - b0;
                if (dr > 20 || dg > 20 || db > 20) {
                    has_rgb_diff = 1;
                    break;
                }
            }
            if (a0 != a1)
                has_alpha_diff = 1;
        }
        if (has_rgb_diff)
            break;
    }

    if (has_rgb_diff)
        return FF_SUB_CHANGE_CONTENT;

    if (!has_alpha_diff)
        return position_differs ? FF_SUB_CHANGE_POSITION
                                : FF_SUB_CHANGE_NONE;

    if (position_differs)
        return FF_SUB_CHANGE_CONTENT;

    return FF_SUB_CHANGE_ALPHA;
}

int ff_sub_find_gap(const uint8_t *rgba, int stride,
                    int w, int h,
                    int *gap_start, int *gap_end)
{
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    int y, x;

    for (y = 0; y < h; y++) {
        const uint8_t *row = rgba + y * stride;
        int transparent = 1;

        for (x = 0; x < w; x++) {
            if (row[x * 4 + 3] != 0) {
                transparent = 0;
                break;
            }
        }

        if (transparent) {
            if (cur_start < 0)
                cur_start = y;
            cur_len++;
        } else {
            if (cur_len > best_len) {
                best_start = cur_start;
                best_len   = cur_len;
            }
            cur_start = -1;
            cur_len   = 0;
        }
    }
    if (cur_len > best_len) {
        best_start = cur_start;
        best_len   = cur_len;
    }

    if (best_len >= 32 && best_start > 0 && best_start + best_len < h) {
        *gap_start = best_start;
        *gap_end   = best_start + best_len;
        return 1;
    }
    return 0;
}

void ff_sub_scale_alpha(const uint32_t *src, uint32_t *dst,
                        int nb_colors, int alpha_pct)
{
    int i;

    for (i = 0; i < nb_colors; i++) {
        uint32_t a = (src[i] >> 24) & 0xff;
        a = a * alpha_pct / 100;
        dst[i] = (src[i] & 0x00ffffff) | (a << 24);
    }
}
