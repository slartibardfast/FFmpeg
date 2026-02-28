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

#include "error.h"
#include "mem.h"
#include "neuquant.h"
#include "quantize.h"

struct AVQuantizeContext {
    enum AVQuantizeAlgorithm algorithm;
    int max_colors;
    NeuQuantContext *nq;
};

AVQuantizeContext *av_quantize_alloc(enum AVQuantizeAlgorithm algorithm,
                                     int max_colors)
{
    AVQuantizeContext *ctx;

    if (max_colors < 2 || max_colors > 256)
        return NULL;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->algorithm  = algorithm;
    ctx->max_colors = max_colors;

    switch (algorithm) {
    case AV_QUANTIZE_NEUQUANT:
        ctx->nq = ff_neuquant_alloc(max_colors);
        if (!ctx->nq) {
            av_freep(&ctx);
            return NULL;
        }
        break;
    default:
        av_freep(&ctx);
        return NULL;
    }

    return ctx;
}

void av_quantize_freep(AVQuantizeContext **pctx)
{
    if (pctx && *pctx) {
        AVQuantizeContext *ctx = *pctx;
        ff_neuquant_free(&ctx->nq);
        av_freep(pctx);
    }
}

int av_quantize_generate_palette(AVQuantizeContext *ctx,
                                  const uint8_t *rgba, int nb_pixels,
                                  uint32_t *palette, int quality)
{
    int ret, samplefac;

    if (!ctx || !rgba || !palette || nb_pixels < 1)
        return AVERROR(EINVAL);
    if (quality < 1 || quality > 30)
        return AVERROR(EINVAL);

    /* quality 1 (fast) -> samplefac 30, quality 30 (best) -> samplefac 1 */
    samplefac = 31 - quality;

    switch (ctx->algorithm) {
    case AV_QUANTIZE_NEUQUANT:
        ret = ff_neuquant_learn(ctx->nq, rgba, nb_pixels, samplefac);
        if (ret < 0)
            return ret;
        ff_neuquant_get_palette(ctx->nq, palette);
        return ctx->max_colors;
    }

    return AVERROR(EINVAL);
}

int av_quantize_apply(AVQuantizeContext *ctx,
                       const uint8_t *rgba, uint8_t *indices,
                       int nb_pixels)
{
    if (!ctx || !rgba || !indices || nb_pixels < 1)
        return AVERROR(EINVAL);

    switch (ctx->algorithm) {
    case AV_QUANTIZE_NEUQUANT:
        if (!ff_neuquant_is_trained(ctx->nq))
            return AVERROR(EINVAL);
        for (int i = 0; i < nb_pixels; i++) {
            const uint8_t *p = rgba + (size_t)i * 4;
            indices[i] = ff_neuquant_map_pixel(ctx->nq, p[0], p[1], p[2], p[3]);
        }
        return 0;
    }

    return AVERROR(EINVAL);
}
