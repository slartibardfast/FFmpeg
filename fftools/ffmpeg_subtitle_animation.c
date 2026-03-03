/*
 * Subtitle animation utilities for PGS encoding.
 *
 * This file is included directly by ffmpeg_enc.c and test files
 * (standard FFmpeg .c inclusion pattern for testability).
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

enum SubtitleChangeType {
    SUB_CHANGE_NONE     = 0,
    SUB_CHANGE_POSITION = 1,
    SUB_CHANGE_ALPHA    = 2,
    SUB_CHANGE_CONTENT  = 3,
};

/**
 * Compute the sum of alpha values across an RGBA buffer.
 */
static int64_t rgba_alpha_sum(const uint8_t *rgba, int w, int h,
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

/**
 * Classify the change between two rendered RGBA frames.
 *
 * Compares bounding box position, dimensions, and pixel content to
 * determine the type of change. This is format-agnostic -- it observes
 * rendered output without parsing any subtitle tags.
 *
 * @return SubtitleChangeType
 */
static enum SubtitleChangeType classify_subtitle_change(
    const uint8_t *rgba0, int x0, int y0, int w0, int h0, int ls0,
    const uint8_t *rgba1, int x1, int y1, int w1, int h1, int ls1)
{
    int x, y;
    int position_differs, has_alpha_diff, has_rgb_diff;

    /* NULL or empty renders */
    if (!rgba0 && !rgba1)
        return SUB_CHANGE_NONE;
    if (!rgba0 || !rgba1)
        return SUB_CHANGE_CONTENT;

    /* Dimension change -> content change */
    if (w0 != w1 || h0 != h1)
        return SUB_CHANGE_CONTENT;

    position_differs = (x0 != x1 || y0 != y1);

    /* Compare pixel data */
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

            /* When either pixel is fully transparent, its RGB is
             * undefined -- only compare alpha, skip RGB. */
            if (a0 == 0 || a1 == 0) {
                has_alpha_diff = 1;
                continue;
            }

            /* Tolerate small RGB differences caused by integer rounding
             * in alpha compositing. When outline or shadow spans
             * overlap text at varying fade levels, the compositing
             * formula (division by out_a) shifts RGB values by up to
             * ~16 per channel for anti-aliased edge pixels. A
             * threshold of 20 covers these artifacts while still
             * detecting real content changes (50+ per channel). */
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
        return SUB_CHANGE_CONTENT;

    if (!has_alpha_diff) {
        /* Pixels identical */
        return position_differs ? SUB_CHANGE_POSITION : SUB_CHANGE_NONE;
    }

    /* Alpha differs but RGB matches -- this is a fade, but only if
     * position is unchanged (PGS can't combine palette_update and
     * position change in one Normal DS). */
    if (position_differs)
        return SUB_CHANGE_CONTENT;

    return SUB_CHANGE_ALPHA;
}

/**
 * Find the largest fully-transparent horizontal gap in an RGBA buffer.
 *
 * Scans rows top-to-bottom. A "gap" is a contiguous run of rows where
 * every pixel has alpha == 0. Returns the largest such gap, but only
 * if it is >= 32 rows and has opaque content on both sides.
 *
 * @param gap_start set to first transparent row on success
 * @param gap_end   set to first non-transparent row after gap on success
 * @return 1 if a splittable gap was found, 0 otherwise
 */
static int find_transparent_gap(const uint8_t *rgba, int stride,
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

    /* Only split if gap is significant and has content on both sides */
    if (best_len >= 32 && best_start > 0 && best_start + best_len < h) {
        *gap_start = best_start;
        *gap_end   = best_start + best_len;
        return 1;
    }
    return 0;
}

/**
 * Scale palette alpha values by a percentage.
 *
 * For each entry, the alpha channel (bits 24-31 in ARGB uint32) is
 * scaled by alpha_pct/100. Other channels are preserved.
 */
static void scale_palette_alpha(const uint32_t *src, uint32_t *dst,
                                int nb_colors, int alpha_pct)
{
    int i;

    for (i = 0; i < nb_colors; i++) {
        uint32_t a = (src[i] >> 24) & 0xff;
        a = a * alpha_pct / 100;
        dst[i] = (src[i] & 0x00ffffff) | (a << 24);
    }
}
