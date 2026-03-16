/*
 * Median Cut color quantization algorithm -- internal interface
 *
 * Based on the Median Cut Algorithm by Paul Heckbert (1982),
 * extracted from libavfilter/vf_palettegen.c.
 *
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2022 Clement Boesch <u pkh me>
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

#ifndef AVUTIL_MEDIANCUT_H
#define AVUTIL_MEDIANCUT_H

#include <stdint.h>

typedef struct AVPrivMedianCutContext AVPrivMedianCutContext;

AVPrivMedianCutContext *avpriv_mediancut_alloc(int max_colors);
void avpriv_mediancut_free(AVPrivMedianCutContext **ctx);

/**
 * Increment the histogram count for a single color.
 *
 * @param color 0xAARRGGBB packed color
 * @return 1 if the color is new, 0 if it already existed, negative on error
 */
int avpriv_mediancut_add_color(AVPrivMedianCutContext *ctx, uint32_t color);

/**
 * Run Median Cut on the accumulated histogram and generate the palette.
 *
 * Colors must have been added beforehand via avpriv_mediancut_add_color()
 * or avpriv_mediancut_learn().
 *
 * @return number of palette entries (>0) on success, negative on error
 */
int avpriv_mediancut_build_palette(AVPrivMedianCutContext *ctx);

/**
 * Convenience wrapper: reset state, build histogram from RGBA pixels,
 * and generate the palette in one call.
 */
int avpriv_mediancut_learn(AVPrivMedianCutContext *ctx, const uint8_t *rgba,
                       int nb_pixels);

void avpriv_mediancut_get_palette(const AVPrivMedianCutContext *ctx, uint32_t *palette);
int avpriv_mediancut_map_pixel(const AVPrivMedianCutContext *ctx,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int avpriv_mediancut_is_trained(const AVPrivMedianCutContext *ctx);

/**
 * Return the number of distinct colors in the histogram.
 */
int avpriv_mediancut_nb_colors(const AVPrivMedianCutContext *ctx);

/**
 * Reset all state (histogram, palette, trained flag) for reuse.
 */
void avpriv_mediancut_reset(AVPrivMedianCutContext *ctx);

#endif /* AVUTIL_MEDIANCUT_H */
