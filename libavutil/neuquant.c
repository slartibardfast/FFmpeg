/*
 * NeuQuant Neural-Net Quantization Algorithm
 *
 * Copyright (c) 1994 Anthony Dekker
 * Copyright (c) 2004-2006 Stuart Coyle (RGBA modification)
 * Copyright (c) 2009 Kornel Lesiński (pngnq rewrite)
 * Copyright (c) 2026 David Connolly (FFmpeg/OkLab adaptation)
 *
 * Based on the NeuQuant algorithm by Anthony Dekker, as modified in
 * pngnq and neuquant32.  Original algorithm described in:
 *   Dekker, A.H., "Kohonen neural networks for optimal colour
 *   quantization", Network: Computation in Neural Systems 5 (1994) 351-367.
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
#include "error.h"
#include "mem.h"
#include "neuquant.h"
#include "palette.h"

/* maximum network size */
#define MAXNETSIZE 256

/* number of learning cycles */
#define NCYCLES 100

/* extra precision during learning */
#define NETBIASSHIFT 4

/* bias for freq/bias adjustment */
#define INTBIASSHIFT 16
#define INTBIAS      (1 << INTBIASSHIFT)
#define BETASHIFT    10
#define BETA         (INTBIAS >> BETASHIFT)
#define GAMMASHIFT   10
#define BETAGAMMA    (INTBIAS << (GAMMASHIFT - BETASHIFT))

/* radius decay */
#define RADIUSBIASSHIFT 6
#define RADIUSBIAS      (1 << RADIUSBIASSHIFT)
#define RADIUSDEC       30

/* learning rate */
#define ALPHABIASSHIFT 10
#define INITALPHA      (1 << ALPHABIASSHIFT)

/* radpower precomputation scale */
#define RADBIASSHIFT 8
#define RADBIAS      (1 << RADBIASSHIFT)

/* primes for pixel stepping (avoids resonance) */
#define PRIME1 499
#define PRIME2 491
#define PRIME3 487
#define PRIME4 503

/* OkLab scale factor: K = (1 << 16) - 1 */
#define K 65535

typedef struct NeuQuantContext {
    int netsize;

    /* neuron weights: L, a, b, alpha in OkLab integer scale << NETBIASSHIFT */
    int32_t network[MAXNETSIZE][4];

    /* frequency and bias for neuron selection */
    int32_t bias[MAXNETSIZE];
    int32_t freq[MAXNETSIZE];

    /* precomputed radius falloff (needs index 1..initrad) */
    int32_t radpower[(MAXNETSIZE >> 3) + 1];

    /* lookup index keyed on L >> 8 (after unbiasing) */
    int netindex[256];

    int trained;
} NeuQuantContext;

/* convert RGBA pixel to internal representation (OkLab + alpha, shifted) */
static void rgba_to_internal(const uint8_t *rgba, int32_t out[4])
{
    uint32_t srgb = (uint32_t)rgba[0] << 16 | (uint32_t)rgba[1] << 8 | rgba[2];
    struct Lab c = ff_srgb_u8_to_oklab_int(srgb);
    out[0] = c.L << NETBIASSHIFT;
    out[1] = c.a << NETBIASSHIFT;
    out[2] = c.b << NETBIASSHIFT;
    out[3] = ((int32_t)rgba[3] * 257) << NETBIASSHIFT;
}

/* alpha-aware importance: transparent pixels have less color influence */
static int colorimportance(int32_t alpha_internal)
{
    /* alpha_internal in [0, K << NETBIASSHIFT], rescale to [0, 255] */
    int a = alpha_internal >> (NETBIASSHIFT + 8);
    return 1 + av_clip(a, 0, 255);
}

static void init_network(NeuQuantContext *ctx)
{
    int initfreq = INTBIAS / ctx->netsize;

    for (int i = 0; i < ctx->netsize; i++) {
        /* spread L evenly along grayscale, a = b = 0, alpha = opaque */
        int32_t L = (int32_t)((int64_t)i * K / (ctx->netsize - 1));
        ctx->network[i][0] = L << NETBIASSHIFT;
        ctx->network[i][1] = 0;
        ctx->network[i][2] = 0;
        ctx->network[i][3] = K << NETBIASSHIFT;
        ctx->freq[i] = initfreq;
        ctx->bias[i] = 0;
    }
}

/* find best matching unit (biased by usage frequency) */
static int contest(NeuQuantContext *ctx, const int32_t pixel[4])
{
    int bestpos = 0, bestbiaspos = 0;
    int32_t bestd = INT32_MAX, bestbiasd = INT32_MAX;
    int imp = colorimportance(pixel[3]);

    for (int i = 0; i < ctx->netsize; i++) {
        const int32_t *n = ctx->network[i];
        int32_t color_dist, alpha_dist, dist, biasdist, betafreq;

        color_dist = FFABS(n[0] - pixel[0]) +
                     FFABS(n[1] - pixel[1]) +
                     FFABS(n[2] - pixel[2]);
        alpha_dist = FFABS(n[3] - pixel[3]);
        dist = color_dist * imp / 256 + alpha_dist;

        if (dist < bestd) {
            bestd   = dist;
            bestpos = i;
        }

        biasdist = dist -
            (ctx->bias[i] >> (INTBIASSHIFT - NETBIASSHIFT));
        if (biasdist < bestbiasd) {
            bestbiasd   = biasdist;
            bestbiaspos = i;
        }

        betafreq = ctx->freq[i] >> BETASHIFT;
        ctx->freq[i] -= betafreq;
        ctx->bias[i] += betafreq << GAMMASHIFT;
    }

    ctx->freq[bestpos] += BETA;
    ctx->bias[bestpos] -= BETAGAMMA;
    return bestbiaspos;
}

/* move neuron i towards the pixel */
static void altersingle(NeuQuantContext *ctx, int alpha, int i,
                         const int32_t pixel[4])
{
    int32_t *n = ctx->network[i];
    for (int k = 0; k < 4; k++)
        n[k] -= (int64_t)alpha * (n[k] - pixel[k]) / INITALPHA;
}

/* move neighbors of neuron i towards the pixel */
static void alterneigh(NeuQuantContext *ctx, int radius, int i,
                        const int32_t pixel[4])
{
    int lo = FFMAX(i - radius, 0);
    int hi = FFMIN(i + radius, ctx->netsize - 1);

    int j = i + 1;
    int k = i - 1;
    int q = 0;

    while (j <= hi || k >= lo) {
        int32_t a = ctx->radpower[++q];
        if (j <= hi) {
            int32_t *n = ctx->network[j++];
            for (int c = 0; c < 4; c++)
                n[c] -= (int64_t)a * (n[c] - pixel[c]) / (INITALPHA * RADBIAS);
        }
        if (k >= lo) {
            int32_t *n = ctx->network[k--];
            for (int c = 0; c < 4; c++)
                n[c] -= (int64_t)a * (n[c] - pixel[c]) / (INITALPHA * RADBIAS);
        }
    }
}

/* remove training bias and clamp values */
static void unbias_network(NeuQuantContext *ctx)
{
    for (int i = 0; i < ctx->netsize; i++) {
        for (int j = 0; j < 4; j++) {
            int32_t v = (ctx->network[i][j] +
                         (1 << (NETBIASSHIFT - 1))) >> NETBIASSHIFT;
            /* clamp L and alpha to [0, K]; a and b can be any value
             * (ff_oklab_int_to_srgb_u8 handles out-of-range) */
            if (j == 0 || j == 3)
                v = av_clip(v, 0, K);
            ctx->network[i][j] = v;
        }
    }
}

/* sort network by L and build lookup index */
static void build_index(NeuQuantContext *ctx)
{
    int previouscol = 0, startpos = 0;

    /* selection sort by discretized L value (L >> 8 -> [0,255]) */
    for (int i = 0; i < ctx->netsize; i++) {
        int smallpos = i;
        int smallval = av_clip(ctx->network[i][0] >> 8, 0, 255);

        for (int j = i + 1; j < ctx->netsize; j++) {
            int val = av_clip(ctx->network[j][0] >> 8, 0, 255);
            if (val < smallval) {
                smallpos = j;
                smallval = val;
            }
        }

        /* swap network[i] and network[smallpos] */
        if (i != smallpos) {
            int32_t temp[4];
            memcpy(temp, ctx->network[i], sizeof(temp));
            memcpy(ctx->network[i], ctx->network[smallpos], sizeof(temp));
            memcpy(ctx->network[smallpos], temp, sizeof(temp));
        }

        /* build index */
        if (smallval != previouscol) {
            ctx->netindex[previouscol] = (startpos + i) >> 1;
            for (int j = previouscol + 1; j < smallval; j++)
                ctx->netindex[j] = i;
            previouscol = smallval;
            startpos    = i;
        }
    }
    ctx->netindex[previouscol] = (startpos + ctx->netsize - 1) >> 1;
    for (int j = previouscol + 1; j < 256; j++)
        ctx->netindex[j] = ctx->netsize - 1;
}

NeuQuantContext *ff_neuquant_alloc(int netsize)
{
    NeuQuantContext *ctx;

    if (netsize < 2 || netsize > MAXNETSIZE)
        return NULL;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->netsize = netsize;
    return ctx;
}

void ff_neuquant_free(NeuQuantContext **pctx)
{
    if (pctx) {
        av_freep(pctx);
    }
}

int ff_neuquant_learn(NeuQuantContext *ctx, const uint8_t *rgba,
                       int nb_pixels, int samplefac)
{
    int samplepixels, delta, step;
    int alpha, radius, rad;
    int initrad;
    int pos;

    if (!ctx || !rgba || nb_pixels < 1 || samplefac < 1 || samplefac > 30)
        return AVERROR(EINVAL);

    init_network(ctx);

    samplepixels = nb_pixels / samplefac;
    if (samplepixels < 1)
        samplepixels = 1;
    delta = FFMAX(samplepixels / NCYCLES, 1);

    alpha  = INITALPHA;
    initrad = FFMAX(ctx->netsize >> 3, 1);
    radius = initrad << RADIUSBIASSHIFT;

    /* choose step size coprime to nb_pixels */
    if (nb_pixels % PRIME1)
        step = PRIME1;
    else if (nb_pixels % PRIME2)
        step = PRIME2;
    else if (nb_pixels % PRIME3)
        step = PRIME3;
    else
        step = PRIME4;

    /* initialize radpower before first iteration */
    rad = radius >> RADIUSBIASSHIFT;
    for (int j = 0; j <= rad; j++)
        ctx->radpower[j] = alpha *
            (((rad * rad - j * j) * RADBIAS) / (rad * rad));

    pos = 0;
    for (int i = 0; i < samplepixels; i++) {
        int32_t pixel[4];
        int best;

        rgba_to_internal(rgba + (size_t)pos * 4, pixel);

        best = contest(ctx, pixel);
        altersingle(ctx, alpha, best, pixel);

        rad = radius >> RADIUSBIASSHIFT;
        if (rad > 1)
            alterneigh(ctx, rad, best, pixel);

        pos += step;
        if (pos >= nb_pixels)
            pos -= nb_pixels;

        /* decay alpha and radius */
        if ((i + 1) % delta == 0) {
            alpha -= alpha / 30;
            if (alpha < 1)
                alpha = 1;
            radius -= radius / RADIUSDEC;
            if (radius < 1)
                radius = 1;
            rad = radius >> RADIUSBIASSHIFT;
            if (rad > 0) {
                for (int j = 0; j < rad; j++)
                    ctx->radpower[j] = alpha *
                        (((rad * rad - j * j) * RADBIAS) / (rad * rad));
            }
        }
    }

    unbias_network(ctx);
    build_index(ctx);
    ctx->trained = 1;
    return 0;
}

void ff_neuquant_get_palette(const NeuQuantContext *ctx, uint32_t *palette)
{
    for (int i = 0; i < ctx->netsize; i++) {
        struct Lab c = {
            .L = ctx->network[i][0],
            .a = ctx->network[i][1],
            .b = ctx->network[i][2],
        };
        uint32_t srgb = ff_oklab_int_to_srgb_u8(c);
        int alpha = av_clip((ctx->network[i][3] + 128) / 257, 0, 255);
        palette[i] = (uint32_t)alpha << 24 | srgb;
    }
}

int ff_neuquant_is_trained(const NeuQuantContext *ctx)
{
    return ctx && ctx->trained;
}

int ff_neuquant_map_pixel(const NeuQuantContext *ctx,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint32_t srgb = (uint32_t)r << 16 | (uint32_t)g << 8 | b;
    struct Lab c = ff_srgb_u8_to_oklab_int(srgb);
    int32_t L     = c.L;
    int32_t ca    = c.a;
    int32_t cb    = c.b;
    int32_t alpha = (int32_t)a * 257;

    int bestd = INT32_MAX;
    int best  = 0;
    int bin   = av_clip(L >> 8, 0, 255);
    int i     = ctx->netindex[bin];
    int j     = i - 1;

    while (i < ctx->netsize || j >= 0) {
        if (i < ctx->netsize) {
            const int32_t *n = ctx->network[i];
            int32_t dist = FFABS(n[0] - L);
            if (dist >= bestd) {
                i = ctx->netsize;
            } else {
                dist += FFABS(n[1] - ca) + FFABS(n[2] - cb) +
                        FFABS(n[3] - alpha);
                if (dist < bestd) {
                    bestd = dist;
                    best  = i;
                }
                i++;
            }
        }
        if (j >= 0) {
            const int32_t *n = ctx->network[j];
            int32_t dist = FFABS(n[0] - L);
            if (dist >= bestd) {
                j = -1;
            } else {
                dist += FFABS(n[1] - ca) + FFABS(n[2] - cb) +
                        FFABS(n[3] - alpha);
                if (dist < bestd) {
                    bestd = dist;
                    best  = j;
                }
                j--;
            }
        }
    }
    return best;
}
