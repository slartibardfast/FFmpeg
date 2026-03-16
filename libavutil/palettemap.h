/*
 * Palette mapping and dithering
 *
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2022 Clément Bœsch <u pkh me>
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

#ifndef AVUTIL_PALETTEMAP_H
#define AVUTIL_PALETTEMAP_H

#include <stdint.h>

#include "pixfmt.h"

enum FFDitheringMode {
    FF_DITHERING_NONE,
    FF_DITHERING_BAYER,
    FF_DITHERING_HECKBERT,
    FF_DITHERING_FLOYD_STEINBERG,
    FF_DITHERING_SIERRA2,
    FF_DITHERING_SIERRA2_4A,
    FF_DITHERING_SIERRA3,
    FF_DITHERING_BURKES,
    FF_DITHERING_ATKINSON,
    FF_NB_DITHERING
};

struct FFColorNode;

typedef struct AVPrivPaletteMapContext AVPrivPaletteMapContext;

/**
 * Allocate and initialize a palette mapping context.
 *
 * Builds a KD-tree for efficient nearest-color lookup using
 * OkLab perceptual distance. The palette is copied internally.
 *
 * @param palette      256-entry palette in 0xAARRGGBB format
 * @param trans_thresh alpha threshold below which colors are
 *                     considered transparent (typically 128)
 * @return allocated context, or NULL on failure
 */
AVPrivPaletteMapContext *avpriv_palette_map_init(const uint32_t *palette,
                                         int trans_thresh);

/**
 * Free a palette mapping context and set the pointer to NULL.
 *
 * @param pctx pointer to context pointer (may be NULL or point to NULL)
 */
void avpriv_palette_map_uninit(AVPrivPaletteMapContext **pctx);

/**
 * Map an RGBA pixel region to palette indices with optional dithering.
 *
 * @param ctx          palette mapping context
 * @param dst          output palette indices (1 byte per pixel)
 * @param dst_linesize output row stride in bytes
 * @param src          input RGBA pixels (4 bytes per pixel, stored as
 *                     native-endian uint32_t). May be modified in-place
 *                     by error diffusion dithering.
 * @param src_linesize input row stride in bytes
 * @param x_start      left edge of region to process
 * @param y_start      top edge of region to process
 * @param w            width of region (pixels)
 * @param h            height of region (pixels)
 * @param dither       dithering algorithm to use
 * @param bayer_scale  bayer scale parameter (0-5), only used when
 *                     dither is FF_DITHERING_BAYER
 * @return 0 on success, negative AVERROR on failure
 */
int avpriv_palette_map_apply(AVPrivPaletteMapContext *ctx,
                          uint8_t *dst, int dst_linesize,
                          uint32_t *src, int src_linesize,
                          int x_start, int y_start, int w, int h,
                          enum FFDitheringMode dither, int bayer_scale);

/**
 * Look up the nearest palette entry for a single color.
 *
 * Uses a hash cache for repeated lookups.
 *
 * @param ctx   palette mapping context
 * @param color input color in 0xAARRGGBB format
 * @return palette index (0-255), or negative AVERROR on failure
 */
int avpriv_palette_map_color(AVPrivPaletteMapContext *ctx, uint32_t color);

/**
 * Get the palette stored in the context.
 *
 * @param ctx palette mapping context
 * @return pointer to 256-entry palette in 0xAARRGGBB format
 */
const uint32_t *avpriv_palette_map_get_palette(const AVPrivPaletteMapContext *ctx);

/**
 * Get the KD-tree color nodes for debug visualization.
 *
 * @param ctx palette mapping context
 * @return pointer to AVPALETTE_COUNT color_node entries
 */
const struct FFColorNode *avpriv_palette_map_get_nodes(
    const AVPrivPaletteMapContext *ctx);

#endif /* AVUTIL_PALETTEMAP_H */
