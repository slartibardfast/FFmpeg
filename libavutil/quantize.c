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

#include <string.h>

#include "common.h"
#include "elbg.h"
#include "error.h"
#include "lfg.h"
#include "mediancut.h"
#include "mem.h"
#include "neuquant.h"
#include "quantize.h"

#define MAX_REGIONS 16

/* Maximum samples drawn from each region.  Keeps the interleaved
 * buffer bounded regardless of individual region size. */
#define SAMPLES_PER_REGION 8192

typedef struct QuantizeRegion {
    uint8_t *rgba;
    int nb_pixels;
} QuantizeRegion;

struct AVQuantizeContext {
    enum AVQuantizeAlgorithm algorithm;
    int max_colors;
    NeuQuantContext *nq;
    MedianCutContext *mc;
    struct ELBGContext *elbg;
    AVLFG elbg_lfg;
    int *elbg_codebook;
    int elbg_trained;
    QuantizeRegion regions[MAX_REGIONS];
    int nb_regions;
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
    case AV_QUANTIZE_MEDIAN_CUT:
        ctx->mc = ff_mediancut_alloc(max_colors);
        if (!ctx->mc) {
            av_freep(&ctx);
            return NULL;
        }
        break;
    case AV_QUANTIZE_ELBG:
        ctx->elbg_codebook = av_malloc_array(max_colors, 4 * sizeof(int));
        if (!ctx->elbg_codebook) {
            av_freep(&ctx);
            return NULL;
        }
        av_lfg_init(&ctx->elbg_lfg, 1);
        break;
    default:
        av_freep(&ctx);
        return NULL;
    }

    return ctx;
}

static void free_regions(AVQuantizeContext *ctx)
{
    for (int i = 0; i < ctx->nb_regions; i++)
        av_freep(&ctx->regions[i].rgba);
    ctx->nb_regions = 0;
}

void av_quantize_freep(AVQuantizeContext **pctx)
{
    if (pctx && *pctx) {
        AVQuantizeContext *ctx = *pctx;
        free_regions(ctx);
        ff_neuquant_free(&ctx->nq);
        ff_mediancut_free(&ctx->mc);
        avpriv_elbg_free(&ctx->elbg);
        av_freep(&ctx->elbg_codebook);
        av_freep(pctx);
    }
}

int av_quantize_add_region(AVQuantizeContext *ctx,
                            const uint8_t *rgba, int nb_pixels)
{
    uint8_t *copy;

    if (!ctx || !rgba || nb_pixels < 1)
        return AVERROR(EINVAL);
    if (ctx->nb_regions >= MAX_REGIONS)
        return AVERROR(ENOSPC);
    if (nb_pixels > INT_MAX / 4)
        return AVERROR(EINVAL);

    copy = av_memdup(rgba, (size_t)nb_pixels * 4);
    if (!copy)
        return AVERROR(ENOMEM);

    ctx->regions[ctx->nb_regions].rgba      = copy;
    ctx->regions[ctx->nb_regions].nb_pixels = nb_pixels;
    ctx->nb_regions++;
    return 0;
}

/**
 * Build an interleaved sample buffer from all regions.
 *
 * Every region contributes exactly @p per_region pixels, drawn by
 * stepping through the region with a stride that covers its full
 * extent.  Regions smaller than per_region wrap around.  The result
 * is a flat RGBA buffer suitable for ff_neuquant_learn().
 */
static uint8_t *build_region_samples(const AVQuantizeContext *ctx,
                                      int per_region, int *out_total)
{
    size_t total = (size_t)per_region * ctx->nb_regions;
    uint8_t *buf;
    int pos = 0;

    buf = av_malloc(total * 4);
    if (!buf)
        return NULL;

    for (int r = 0; r < ctx->nb_regions; r++) {
        const QuantizeRegion *reg = &ctx->regions[r];
        int step = FFMAX(reg->nb_pixels / per_region, 1);

        for (int i = 0, idx = 0; i < per_region; i++, idx += step) {
            if (idx >= reg->nb_pixels)
                idx %= reg->nb_pixels;
            memcpy(buf + (size_t)pos * 4,
                   reg->rgba + (size_t)idx * 4, 4);
            pos++;
        }
    }

    *out_total = pos;
    return buf;
}

/**
 * Convert RGBA byte buffer to int point array for ELBG (4D).
 */
static int *rgba_to_int_points(const uint8_t *rgba, int nb_pixels)
{
    int *points = av_malloc_array(nb_pixels, 4 * sizeof(int));

    if (!points)
        return NULL;

    for (int i = 0; i < nb_pixels; i++) {
        const uint8_t *p = rgba + (size_t)i * 4;
        points[i * 4    ] = p[0];
        points[i * 4 + 1] = p[1];
        points[i * 4 + 2] = p[2];
        points[i * 4 + 3] = p[3];
    }

    return points;
}

static int run_elbg(AVQuantizeContext *ctx, const uint8_t *rgba,
                     int nb_pixels, uint32_t *palette, int num_steps)
{
    int *points, *closest_cb;
    int ret;

    points = rgba_to_int_points(rgba, nb_pixels);
    if (!points)
        return AVERROR(ENOMEM);

    closest_cb = av_malloc_array(nb_pixels, sizeof(int));
    if (!closest_cb) {
        av_free(points);
        return AVERROR(ENOMEM);
    }

    ret = avpriv_elbg_do(&ctx->elbg, points, 4, nb_pixels,
                          ctx->elbg_codebook, ctx->max_colors,
                          num_steps, closest_cb, &ctx->elbg_lfg, 0);
    av_free(points);
    av_free(closest_cb);
    if (ret < 0)
        return ret;

    for (int i = 0; i < ctx->max_colors; i++) {
        const int *cb = ctx->elbg_codebook + i * 4;
        palette[i] = ((uint32_t)av_clip(cb[3], 0, 255) << 24) |
                     ((uint32_t)av_clip(cb[0], 0, 255) << 16) |
                     ((uint32_t)av_clip(cb[1], 0, 255) <<  8) |
                      (uint32_t)av_clip(cb[2], 0, 255);
    }

    ctx->elbg_trained = 1;
    return 0;
}

int av_quantize_generate_palette(AVQuantizeContext *ctx,
                                  const uint8_t *rgba, int nb_pixels,
                                  uint32_t *palette, int quality)
{
    int ret, samplefac;

    if (!ctx || !palette)
        return AVERROR(EINVAL);
    if (ctx->nb_regions == 0 && (!rgba || nb_pixels < 1))
        return AVERROR(EINVAL);
    if (nb_pixels > INT_MAX / 4)
        return AVERROR(EINVAL);
    if (quality < 1 || quality > 30)
        return AVERROR(EINVAL);

    /* quality 1 (fast) -> samplefac 30, quality 30 (best) -> samplefac 1 */
    samplefac = 31 - quality;

    switch (ctx->algorithm) {
    case AV_QUANTIZE_NEUQUANT:
        if (ctx->nb_regions > 0) {
            int max_px = 0, per_region, total;
            uint8_t *samples;

            for (int r = 0; r < ctx->nb_regions; r++)
                max_px = FFMAX(max_px, ctx->regions[r].nb_pixels);
            per_region = FFMIN(max_px, SAMPLES_PER_REGION);

            samples = build_region_samples(ctx, per_region, &total);
            if (!samples) {
                free_regions(ctx);
                return AVERROR(ENOMEM);
            }

            ret = ff_neuquant_learn(ctx->nq, samples, total, samplefac);
            av_free(samples);
            if (ret < 0) {
                free_regions(ctx);
                return ret;
            }
        } else {
            ret = ff_neuquant_learn(ctx->nq, rgba, nb_pixels, samplefac);
            if (ret < 0)
                return ret;
        }
        ff_neuquant_get_palette(ctx->nq, palette);
        free_regions(ctx);
        return ctx->max_colors;

    case AV_QUANTIZE_MEDIAN_CUT:
        if (ctx->nb_regions > 0) {
            int max_px = 0, per_region, total;
            uint8_t *samples;

            for (int r = 0; r < ctx->nb_regions; r++)
                max_px = FFMAX(max_px, ctx->regions[r].nb_pixels);
            per_region = FFMIN(max_px, SAMPLES_PER_REGION);

            samples = build_region_samples(ctx, per_region, &total);
            if (!samples) {
                free_regions(ctx);
                return AVERROR(ENOMEM);
            }

            ret = ff_mediancut_learn(ctx->mc, samples, total);
            av_free(samples);
            if (ret < 0) {
                free_regions(ctx);
                return ret;
            }
        } else {
            ret = ff_mediancut_learn(ctx->mc, rgba, nb_pixels);
            if (ret < 0)
                return ret;
        }
        ff_mediancut_get_palette(ctx->mc, palette);
        free_regions(ctx);
        return ret;

    case AV_QUANTIZE_ELBG:
        /* quality 1-10 -> 1 step, 11-20 -> 2 steps, 21-30 -> 3 steps */
        {
            int num_steps = 1 + (quality - 1) / 10;
            if (ctx->nb_regions > 0) {
                int max_px = 0, per_region, total;
                uint8_t *samples;

                for (int r = 0; r < ctx->nb_regions; r++)
                    max_px = FFMAX(max_px, ctx->regions[r].nb_pixels);
                per_region = FFMIN(max_px, SAMPLES_PER_REGION);

                samples = build_region_samples(ctx, per_region, &total);
                if (!samples) {
                    free_regions(ctx);
                    return AVERROR(ENOMEM);
                }

                ret = run_elbg(ctx, samples, total, palette, num_steps);
                av_free(samples);
                if (ret < 0) {
                    free_regions(ctx);
                    return ret;
                }
            } else {
                ret = run_elbg(ctx, rgba, nb_pixels, palette, num_steps);
                if (ret < 0)
                    return ret;
            }
            free_regions(ctx);
            return ctx->max_colors;
        }
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

    case AV_QUANTIZE_MEDIAN_CUT:
        if (!ff_mediancut_is_trained(ctx->mc))
            return AVERROR(EINVAL);
        for (int i = 0; i < nb_pixels; i++) {
            const uint8_t *p = rgba + (size_t)i * 4;
            indices[i] = ff_mediancut_map_pixel(ctx->mc,
                                                p[0], p[1], p[2], p[3]);
        }
        return 0;

    case AV_QUANTIZE_ELBG:
        if (!ctx->elbg_trained)
            return AVERROR(EINVAL);
        for (int i = 0; i < nb_pixels; i++) {
            const uint8_t *p = rgba + (size_t)i * 4;
            int best = 0, best_dist = INT_MAX;
            for (int j = 0; j < ctx->max_colors; j++) {
                const int *cb = ctx->elbg_codebook + j * 4;
                int dr = p[0] - cb[0];
                int dg = p[1] - cb[1];
                int db = p[2] - cb[2];
                int da = p[3] - cb[3];
                int dist = dr * dr + dg * dg + db * db + da * da;
                if (dist < best_dist) {
                    best_dist = dist;
                    best = j;
                }
            }
            indices[i] = best;
        }
        return 0;
    }

    return AVERROR(EINVAL);
}
