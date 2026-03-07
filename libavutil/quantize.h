/*
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

/**
 * @file
 * @ingroup lavu_quantize
 * Color quantization API.
 */

#ifndef AVUTIL_QUANTIZE_H
#define AVUTIL_QUANTIZE_H

#include <stdint.h>

/**
 * @defgroup lavu_quantize Color Quantization
 * @ingroup lavu_data
 *
 * Reduce RGBA images to a palette of up to 256 colors.
 *
 * @code
 * AVQuantizeContext *ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
 * uint32_t palette[256];
 * int nb_colors = av_quantize_generate_palette(ctx, rgba, nb_pixels,
 *                                              palette, 10);
 * uint8_t *indices = av_malloc(nb_pixels);
 * av_quantize_apply(ctx, rgba, indices, nb_pixels);
 * av_quantize_freep(&ctx);
 * @endcode
 *
 * @{
 */

enum AVQuantizeAlgorithm {
    AV_QUANTIZE_NEUQUANT,    /**< NeuQuant neural-net quantizer */
    AV_QUANTIZE_MEDIAN_CUT,  /**< Median Cut (Heckbert 1982) */
};

typedef struct AVQuantizeContext AVQuantizeContext;

/**
 * Allocate a color quantization context.
 *
 * @param[in] algorithm  quantization algorithm to use
 * @param[in] max_colors maximum palette size (2-256)
 * @return newly allocated context, or NULL on failure
 */
AVQuantizeContext *av_quantize_alloc(enum AVQuantizeAlgorithm algorithm,
                                     int max_colors);

/**
 * Free a quantization context and set the pointer to NULL.
 *
 * @param[in,out] pctx pointer to context to free
 */
void av_quantize_freep(AVQuantizeContext **pctx);

/**
 * Add a region of pixels to the quantization input.
 *
 * When regions are added, av_quantize_generate_palette() samples from
 * all regions in equal proportion, ensuring palette representation
 * regardless of pixel count.  This prevents large regions from starving
 * small regions of palette entries.
 *
 * If no regions are added, generate_palette() operates on the rgba
 * buffer passed directly to it (backward compatible).
 *
 * The pixel data is copied internally; the caller may free @p rgba
 * immediately after this call.  Regions are consumed (freed) by the
 * next call to av_quantize_generate_palette().  Up to 16 regions
 * may be added per palette generation.
 *
 * @param[in] ctx       quantization context
 * @param[in] rgba      RGBA pixel data for this region (4 bytes per pixel)
 * @param[in] nb_pixels number of pixels in this region
 * @return 0 on success, negative AVERROR on failure
 */
int av_quantize_add_region(AVQuantizeContext *ctx,
                            const uint8_t *rgba, int nb_pixels);

/**
 * Analyze pixels and generate an optimal palette.
 *
 * Must be called before av_quantize_apply(). The context retains
 * the generated palette for subsequent mapping calls.
 *
 * If regions have been added via av_quantize_add_region(), the palette
 * is generated from equal-weight sampling across all regions; @p rgba
 * and @p nb_pixels are ignored and may be NULL/0.  All regions are
 * consumed (freed) by this call.
 *
 * If no regions have been added, @p rgba and @p nb_pixels are required.
 *
 * @param[in]  ctx       quantization context
 * @param[in]  rgba      input pixels in RGBA byte order (4 bytes per pixel),
 *                        or NULL when regions are used
 * @param[in]  nb_pixels number of pixels in the input, or 0 when regions
 *                        are used
 * @param[out] palette   output palette in 0xAARRGGBB format
 * @param[in]  quality   learning quality 1 (fast) to 30 (best), 10 typical
 * @return number of palette entries on success, negative AVERROR on failure
 */
int av_quantize_generate_palette(AVQuantizeContext *ctx,
                                  const uint8_t *rgba, int nb_pixels,
                                  uint32_t *palette, int quality);

/**
 * Map each pixel to its nearest palette entry.
 *
 * av_quantize_generate_palette() must have been called first.
 *
 * @param[in]  ctx       quantization context
 * @param[in]  rgba      input pixels in RGBA byte order (4 bytes per pixel)
 * @param[out] indices   output palette indices, one byte per pixel
 * @param[in]  nb_pixels number of pixels
 * @return 0 on success, negative AVERROR on failure
 */
int av_quantize_apply(AVQuantizeContext *ctx,
                       const uint8_t *rgba, uint8_t *indices,
                       int nb_pixels);

/**
 * @}
 */

#endif /* AVUTIL_QUANTIZE_H */
