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

typedef struct MedianCutContext MedianCutContext;

MedianCutContext *ff_mediancut_alloc(int max_colors);
void ff_mediancut_free(MedianCutContext **ctx);
int ff_mediancut_learn(MedianCutContext *ctx, const uint8_t *rgba,
                       int nb_pixels);
void ff_mediancut_get_palette(const MedianCutContext *ctx, uint32_t *palette);
int ff_mediancut_map_pixel(const MedianCutContext *ctx,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a);
int ff_mediancut_is_trained(const MedianCutContext *ctx);

#endif /* AVUTIL_MEDIANCUT_H */
