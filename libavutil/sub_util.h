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

#ifndef AVUTIL_SUB_UTIL_H
#define AVUTIL_SUB_UTIL_H

#include <stdint.h>

enum FFSubChangeType {
    FF_SUB_CHANGE_NONE     = 0,
    FF_SUB_CHANGE_POSITION = 1,
    FF_SUB_CHANGE_ALPHA    = 2,
    FF_SUB_CHANGE_CONTENT  = 3,
};

/**
 * Compute the sum of alpha values across an RGBA buffer.
 */
int64_t ff_sub_alpha_sum(const uint8_t *rgba, int w, int h,
                         int linesize);

/**
 * Classify the change between two rendered RGBA frames.
 *
 * Compares bounding box position, dimensions, and pixel content to
 * determine the type of change. Format-agnostic -- observes rendered
 * output without parsing subtitle tags.
 */
enum FFSubChangeType ff_sub_classify_change(
    const uint8_t *rgba0, int x0, int y0, int w0, int h0, int ls0,
    const uint8_t *rgba1, int x1, int y1, int w1, int h1, int ls1);

/**
 * Find the largest fully-transparent horizontal gap in an RGBA buffer.
 *
 * Scans rows for contiguous runs where every pixel has alpha == 0.
 * Returns the largest gap, but only if it is >= 32 rows and has
 * opaque content on both sides.
 *
 * @param gap_start set to first transparent row on success
 * @param gap_end   set to first non-transparent row after gap
 * @return 1 if a splittable gap was found, 0 otherwise
 */
int ff_sub_find_gap(const uint8_t *rgba, int stride,
                    int w, int h,
                    int *gap_start, int *gap_end);

/**
 * Scale palette alpha values by a percentage.
 *
 * For each entry, the alpha channel (bits 24-31 in ARGB uint32) is
 * scaled by alpha_pct/100. Other channels are preserved.
 */
void ff_sub_scale_alpha(const uint32_t *src, uint32_t *dst,
                        int nb_colors, int alpha_pct);

#endif /* AVUTIL_SUB_UTIL_H */
