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

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libavutil/macros.h"
#include "libavutil/mem.h"
#include "libavutil/quantize.h"

#define WIDTH  64
#define HEIGHT 64
#define NB_PIXELS (WIDTH * HEIGHT)

/* generate a deterministic test image with color gradients */
static void generate_test_image(uint8_t *rgba)
{
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int i = (y * WIDTH + x) * 4;
            rgba[i + 0] = x * 255 / (WIDTH - 1);    /* R: horizontal */
            rgba[i + 1] = y * 255 / (HEIGHT - 1);   /* G: vertical */
            rgba[i + 2] = (x + y) * 255 / (WIDTH + HEIGHT - 2); /* B */
            rgba[i + 3] = 255;                       /* A: opaque */
        }
    }
}

static int test_basic_quantize(void)
{
    AVQuantizeContext *ctx = NULL;
    uint8_t *rgba = NULL;
    uint8_t *indices = NULL;
    uint32_t palette[256];
    int ret, nb_colors;

    printf("test basic quantize: ");

    rgba = av_malloc(NB_PIXELS * 4);
    indices = av_malloc(NB_PIXELS);
    if (!rgba || !indices) {
        ret = -1;
        goto end;
    }

    generate_test_image(rgba);

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) {
        ret = -1;
        goto end;
    }

    nb_colors = av_quantize_generate_palette(ctx, rgba, NB_PIXELS,
                                              palette, 10);
    if (nb_colors != 16) {
        fprintf(stderr, "expected 16 colors, got %d\n", nb_colors);
        ret = -1;
        goto end;
    }

    ret = av_quantize_apply(ctx, rgba, indices, NB_PIXELS);
    if (ret < 0) {
        fprintf(stderr, "av_quantize_apply failed: %d\n", ret);
        goto end;
    }

    /* verify all indices are in range */
    for (int i = 0; i < NB_PIXELS; i++) {
        if (indices[i] >= 16) {
            fprintf(stderr, "index %d out of range at pixel %d\n",
                    indices[i], i);
            ret = -1;
            goto end;
        }
    }

    /* verify palette has non-zero entries (not all black) */
    {
        int nonzero = 0;
        for (int i = 0; i < 16; i++)
            if (palette[i] & 0x00FFFFFF)
                nonzero++;
        if (nonzero < 2) {
            fprintf(stderr, "palette has too few non-black colors\n");
            ret = -1;
            goto end;
        }
    }

    ret = 0;
    printf("OK\n");

end:
    av_quantize_freep(&ctx);
    av_free(rgba);
    av_free(indices);
    return ret;
}

static int test_small_palette(void)
{
    AVQuantizeContext *ctx;
    uint8_t rgba[] = {
        255, 0, 0, 255,   /* red */
        0, 255, 0, 255,   /* green */
        0, 0, 255, 255,   /* blue */
        255, 255, 0, 255, /* yellow */
    };
    uint32_t palette[4];
    uint8_t indices[4];
    int nb_colors, ret;

    printf("test small palette: ");

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 4);
    if (!ctx)
        return -1;

    nb_colors = av_quantize_generate_palette(ctx, rgba, 4, palette, 1);
    if (nb_colors != 4) {
        fprintf(stderr, "expected 4 colors, got %d\n", nb_colors);
        av_quantize_freep(&ctx);
        return -1;
    }

    ret = av_quantize_apply(ctx, rgba, indices, 4);
    if (ret < 0) {
        fprintf(stderr, "av_quantize_apply failed: %d\n", ret);
        av_quantize_freep(&ctx);
        return ret;
    }

    /* verify indices are in range */
    for (int i = 0; i < 4; i++) {
        if (indices[i] >= 4) {
            fprintf(stderr, "index %d out of range at pixel %d\n",
                    indices[i], i);
            av_quantize_freep(&ctx);
            return -1;
        }
    }

    av_quantize_freep(&ctx);
    printf("OK\n");
    return 0;
}

static int test_error_handling(void)
{
    AVQuantizeContext *ctx;

    printf("test error handling: ");

    /* invalid max_colors */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 0);
    if (ctx) {
        fprintf(stderr, "should reject max_colors=0\n");
        av_quantize_freep(&ctx);
        return -1;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 257);
    if (ctx) {
        fprintf(stderr, "should reject max_colors=257\n");
        av_quantize_freep(&ctx);
        return -1;
    }

    /* freep with NULL should be safe */
    av_quantize_freep(NULL);

    ctx = NULL;
    av_quantize_freep(&ctx);

    /* add_region with nb_pixels that would overflow size_t*4 */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx)
        return -1;
    {
        uint8_t dummy[4] = {0};
        int ret = av_quantize_add_region(ctx, dummy, INT_MAX);
        if (ret >= 0) {
            fprintf(stderr, "should reject nb_pixels=INT_MAX\n");
            av_quantize_freep(&ctx);
            return -1;
        }
    }
    av_quantize_freep(&ctx);

    printf("OK\n");
    return 0;
}

/* Test region-weighted quantization.
 *
 * Region A: 4000 white pixels (dominant by count)
 * Region B: 100 red pixels (small but colorful)
 *
 * Without regions, NeuQuant allocates most palette entries to white shades.
 * With regions, red gets equal representation.  Verify by checking that
 * the palette contains a clearly red entry (R > 200, G < 80, B < 80).
 */
static int test_region_weighted(void)
{
    AVQuantizeContext *ctx;
    uint8_t *big, *small_buf;
    uint32_t palette[16];
    int ret, nb_colors, has_red;

    printf("test region weighted: ");

    big = av_malloc(4000 * 4);
    small_buf = av_malloc(100 * 4);
    if (!big || !small_buf) {
        av_free(big);
        av_free(small_buf);
        return -1;
    }

    /* Region A: white pixels */
    for (int i = 0; i < 4000; i++) {
        big[i * 4 + 0] = 255;
        big[i * 4 + 1] = 255;
        big[i * 4 + 2] = 255;
        big[i * 4 + 3] = 255;
    }

    /* Region B: red pixels */
    for (int i = 0; i < 100; i++) {
        small_buf[i * 4 + 0] = 255;
        small_buf[i * 4 + 1] = 0;
        small_buf[i * 4 + 2] = 0;
        small_buf[i * 4 + 3] = 255;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) {
        av_free(big);
        av_free(small_buf);
        return -1;
    }

    ret = av_quantize_add_region(ctx, big, 4000);
    if (ret < 0) goto fail;
    ret = av_quantize_add_region(ctx, small_buf, 100);
    if (ret < 0) goto fail;

    nb_colors = av_quantize_generate_palette(ctx, NULL, 0, palette, 10);
    if (nb_colors != 16) {
        fprintf(stderr, "expected 16 colors, got %d\n", nb_colors);
        goto fail;
    }

    /* Check palette contains a red entry */
    has_red = 0;
    for (int i = 0; i < 16; i++) {
        uint8_t r = (palette[i] >> 16) & 0xFF;
        uint8_t g = (palette[i] >>  8) & 0xFF;
        uint8_t b =  palette[i]        & 0xFF;
        if (r > 200 && g < 80 && b < 80) {
            has_red = 1;
            break;
        }
    }

    if (!has_red) {
        fprintf(stderr, "palette missing red entry with region weighting\n");
        goto fail;
    }

    av_quantize_freep(&ctx);
    av_free(big);
    av_free(small_buf);
    printf("OK\n");
    return 0;

fail:
    av_quantize_freep(&ctx);
    av_free(big);
    av_free(small_buf);
    return -1;
}

/* Verify backward compatibility: generate_palette still works
 * without regions */
static int test_region_backward_compat(void)
{
    AVQuantizeContext *ctx;
    uint8_t rgba[] = {
        255, 0, 0, 255,
        0, 255, 0, 255,
        0, 0, 255, 255,
    };
    uint32_t palette[4];
    int nb_colors;

    printf("test region backward compat: ");

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 4);
    if (!ctx)
        return -1;

    /* No add_region calls -- should work as before */
    nb_colors = av_quantize_generate_palette(ctx, rgba, 3, palette, 1);
    if (nb_colors != 4) {
        fprintf(stderr, "expected 4 colors, got %d\n", nb_colors);
        av_quantize_freep(&ctx);
        return -1;
    }

    av_quantize_freep(&ctx);
    printf("OK\n");
    return 0;
}

/* Compute squared sRGB distance between a pixel and a palette entry */
static int color_dist_sq(const uint8_t *pixel, uint32_t pal_entry)
{
    int dr = pixel[0] - (int)((pal_entry >> 16) & 0xFF);
    int dg = pixel[1] - (int)((pal_entry >>  8) & 0xFF);
    int db = pixel[2] - (int)( pal_entry        & 0xFF);
    return dr * dr + dg * dg + db * db;
}

/* Find the minimum distance from a pixel to any palette entry */
static int min_palette_dist_sq(const uint8_t *pixel, const uint32_t *pal,
                                int nb_colors)
{
    int best = INT32_MAX;
    for (int i = 0; i < nb_colors; i++) {
        int d = color_dist_sq(pixel, pal[i]);
        if (d < best)
            best = d;
    }
    return best;
}

/* Starvation comparison: prove region weighting reduces mapping error.
 *
 * Scenario: 5000 white pixels + 200 vivid pixels (red, green, blue, yellow).
 * Compare max palette error for the vivid pixels with and without regions.
 *
 * Without regions, NeuQuant sees the vivid pixels as <4% of input -- most
 * palette entries go to white shades and the vivid colors map poorly.
 * With regions, vivid colors get 50% of training time and map well.
 */
static int test_region_starvation_comparison(void)
{
    AVQuantizeContext *ctx;
    uint8_t *dominant, *minority;
    uint32_t pal_flat[16], pal_region[16];
    uint8_t *combined;
    int nb_dom = 5000, nb_min = 200;
    int ret, max_err_flat, max_err_region;

    printf("test region starvation comparison: ");

    dominant = av_malloc((size_t)nb_dom * 4);
    minority = av_malloc((size_t)nb_min * 4);
    combined = av_malloc((size_t)(nb_dom + nb_min) * 4);
    if (!dominant || !minority || !combined) {
        av_free(dominant);
        av_free(minority);
        av_free(combined);
        return -1;
    }

    /* Dominant region: white */
    for (int i = 0; i < nb_dom; i++) {
        dominant[i * 4 + 0] = 255;
        dominant[i * 4 + 1] = 255;
        dominant[i * 4 + 2] = 255;
        dominant[i * 4 + 3] = 255;
    }

    /* Minority region: 4 vivid colors in equal proportion */
    for (int i = 0; i < nb_min; i++) {
        int c = i % 4;
        minority[i * 4 + 0] = (c == 0 || c == 3) ? 255 : 0;  /* R */
        minority[i * 4 + 1] = (c == 1 || c == 3) ? 255 : 0;  /* G */
        minority[i * 4 + 2] = (c == 2)            ? 255 : 0;  /* B */
        minority[i * 4 + 3] = 255;
    }

    /* Flat buffer: dominant then minority concatenated */
    memcpy(combined, dominant, (size_t)nb_dom * 4);
    memcpy(combined + (size_t)nb_dom * 4, minority, (size_t)nb_min * 4);

    /* Path 1: flat (no regions) */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) goto fail;
    ret = av_quantize_generate_palette(ctx, combined, nb_dom + nb_min,
                                        pal_flat, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) goto fail;

    /* Path 2: region-weighted */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) goto fail;
    ret = av_quantize_add_region(ctx, dominant, nb_dom);
    if (ret < 0) { av_quantize_freep(&ctx); goto fail; }
    ret = av_quantize_add_region(ctx, minority, nb_min);
    if (ret < 0) { av_quantize_freep(&ctx); goto fail; }
    ret = av_quantize_generate_palette(ctx, NULL, 0, pal_region, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) goto fail;

    /* Measure max error for minority pixels against each palette */
    max_err_flat = 0;
    max_err_region = 0;
    for (int i = 0; i < nb_min; i++) {
        const uint8_t *p = minority + i * 4;
        int d_flat   = min_palette_dist_sq(p, pal_flat, 16);
        int d_region = min_palette_dist_sq(p, pal_region, 16);
        if (d_flat > max_err_flat)
            max_err_flat = d_flat;
        if (d_region > max_err_region)
            max_err_region = d_region;
    }

    printf("flat_max_err=%d region_max_err=%d ", max_err_flat, max_err_region);

    if (max_err_region > max_err_flat) {
        fprintf(stderr, "\nregion weighting WORSE than flat "
                "(region=%d > flat=%d)\n", max_err_region, max_err_flat);
        goto fail;
    }

    av_free(dominant);
    av_free(minority);
    av_free(combined);
    printf("OK\n");
    return 0;

fail:
    av_free(dominant);
    av_free(minority);
    av_free(combined);
    return -1;
}

/* Multi-region diversity: 3 regions with distinct color profiles.
 *
 * Region A: 2000 cyan pixels
 * Region B: 2000 magenta pixels
 * Region C: 200 yellow pixels (minority)
 *
 * Verify palette contains entries close to all three primaries.
 */
static int test_region_multi_diversity(void)
{
    AVQuantizeContext *ctx;
    uint8_t *cyan, *magenta, *yellow;
    uint32_t palette[16];
    int ret, has_cyan = 0, has_magenta = 0, has_yellow = 0;

    printf("test region multi diversity: ");

    cyan    = av_malloc(2000 * 4);
    magenta = av_malloc(2000 * 4);
    yellow  = av_malloc(200 * 4);
    if (!cyan || !magenta || !yellow) {
        av_free(cyan);
        av_free(magenta);
        av_free(yellow);
        return -1;
    }

    for (int i = 0; i < 2000; i++) {
        cyan[i * 4 + 0] = 0;   cyan[i * 4 + 1] = 255;
        cyan[i * 4 + 2] = 255; cyan[i * 4 + 3] = 255;
    }
    for (int i = 0; i < 2000; i++) {
        magenta[i * 4 + 0] = 255; magenta[i * 4 + 1] = 0;
        magenta[i * 4 + 2] = 255; magenta[i * 4 + 3] = 255;
    }
    for (int i = 0; i < 200; i++) {
        yellow[i * 4 + 0] = 255; yellow[i * 4 + 1] = 255;
        yellow[i * 4 + 2] = 0;   yellow[i * 4 + 3] = 255;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) goto fail;

    ret = av_quantize_add_region(ctx, cyan, 2000);
    if (ret < 0) { av_quantize_freep(&ctx); goto fail; }
    ret = av_quantize_add_region(ctx, magenta, 2000);
    if (ret < 0) { av_quantize_freep(&ctx); goto fail; }
    ret = av_quantize_add_region(ctx, yellow, 200);
    if (ret < 0) { av_quantize_freep(&ctx); goto fail; }

    ret = av_quantize_generate_palette(ctx, NULL, 0, palette, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) goto fail;

    for (int i = 0; i < 16; i++) {
        uint8_t r = (palette[i] >> 16) & 0xFF;
        uint8_t g = (palette[i] >>  8) & 0xFF;
        uint8_t b =  palette[i]        & 0xFF;
        /* cyan: low R, high G, high B */
        if (r < 80 && g > 180 && b > 180) has_cyan = 1;
        /* magenta: high R, low G, high B */
        if (r > 180 && g < 80 && b > 180) has_magenta = 1;
        /* yellow: high R, high G, low B */
        if (r > 180 && g > 180 && b < 80) has_yellow = 1;
    }

    if (!has_cyan || !has_magenta || !has_yellow) {
        fprintf(stderr, "missing colors: cyan=%d magenta=%d yellow=%d\n",
                has_cyan, has_magenta, has_yellow);
        goto fail;
    }

    av_free(cyan);
    av_free(magenta);
    av_free(yellow);
    printf("OK\n");
    return 0;

fail:
    av_free(cyan);
    av_free(magenta);
    av_free(yellow);
    return -1;
}

/* Single region quality equivalence: one add_region call should produce
 * a palette of comparable quality to the flat-buffer path.
 *
 * We compare sum-of-squared-errors for all pixels against each palette.
 * The region path may differ slightly due to the SAMPLES_PER_REGION cap,
 * but should not be materially worse.
 */
static int test_region_single_equivalence(void)
{
    AVQuantizeContext *ctx;
    uint8_t *rgba;
    uint32_t pal_flat[16], pal_region[16];
    int nb_px = 1024;
    int ret;
    int64_t sse_flat = 0, sse_region = 0;

    printf("test region single equivalence: ");

    rgba = av_malloc((size_t)nb_px * 4);
    if (!rgba) return -1;

    /* Deterministic gradient: enough pixels for NeuQuant to learn well */
    for (int i = 0; i < nb_px; i++) {
        rgba[i * 4 + 0] = (i * 4) & 0xFF;
        rgba[i * 4 + 1] = (i * 7) & 0xFF;
        rgba[i * 4 + 2] = (i * 11) & 0xFF;
        rgba[i * 4 + 3] = 255;
    }

    /* Flat path */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) { av_free(rgba); return -1; }
    ret = av_quantize_generate_palette(ctx, rgba, nb_px, pal_flat, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) { av_free(rgba); return -1; }

    /* Region path: one region with the same pixels */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) { av_free(rgba); return -1; }
    ret = av_quantize_add_region(ctx, rgba, nb_px);
    if (ret < 0) { av_quantize_freep(&ctx); av_free(rgba); return -1; }
    ret = av_quantize_generate_palette(ctx, NULL, 0, pal_region, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) { av_free(rgba); return -1; }

    /* Compare SSE for all pixels against each palette */
    for (int i = 0; i < nb_px; i++) {
        const uint8_t *p = rgba + i * 4;
        sse_flat   += min_palette_dist_sq(p, pal_flat, 16);
        sse_region += min_palette_dist_sq(p, pal_region, 16);
    }

    printf("flat_sse=%" PRId64 " region_sse=%" PRId64 " ",
           sse_flat, sse_region);

    /* Region path should be within 20% of flat path quality.
     * They process the same data, so large divergence indicates a bug. */
    if (sse_region > sse_flat * 6 / 5) {
        fprintf(stderr, "\nsingle-region quality much worse than flat "
                "(region=%" PRId64 " > 120%% of flat=%" PRId64 ")\n",
                sse_region, sse_flat);
        av_free(rgba);
        return -1;
    }

    av_free(rgba);
    printf("OK\n");
    return 0;
}

/* Karaoke scenario: simulate the real-world use case.
 *
 * Region A: 3000 pixels of white dialogue text (R=G=B=255)
 * Region B: 300 pixels of karaoke highlight (bright red, green, blue mix)
 *
 * Without regions, the karaoke colors map to white or near-white shades.
 * With regions, the palette spans both white and karaoke colors.
 *
 * Verify: the sum of squared errors for minority pixels is strictly
 * lower with region weighting.
 */
static int test_region_karaoke(void)
{
    AVQuantizeContext *ctx;
    uint8_t *dialogue, *karaoke;
    uint8_t *combined;
    uint32_t pal_flat[16], pal_region[16];
    int nb_dial = 3000, nb_kara = 300;
    int ret;
    int64_t sum_flat = 0, sum_region = 0;

    printf("test region karaoke: ");

    dialogue = av_malloc((size_t)nb_dial * 4);
    karaoke  = av_malloc((size_t)nb_kara * 4);
    combined = av_malloc((size_t)(nb_dial + nb_kara) * 4);
    if (!dialogue || !karaoke || !combined) {
        av_free(dialogue);
        av_free(karaoke);
        av_free(combined);
        return -1;
    }

    /* Dialogue: white with slight gray border variation */
    for (int i = 0; i < nb_dial; i++) {
        uint8_t v = 230 + (i % 26);  /* 230-255 */
        dialogue[i * 4 + 0] = v;
        dialogue[i * 4 + 1] = v;
        dialogue[i * 4 + 2] = v;
        dialogue[i * 4 + 3] = 255;
    }

    /* Karaoke: vivid cycling colors (red, green, blue, orange, purple) */
    for (int i = 0; i < nb_kara; i++) {
        int c = i % 5;
        switch (c) {
        case 0:
            karaoke[i * 4]     = 255;
            karaoke[i * 4 + 1] = 50;
            karaoke[i * 4 + 2] = 50;
            break;
        case 1:
            karaoke[i * 4]     = 50;
            karaoke[i * 4 + 1] = 255;
            karaoke[i * 4 + 2] = 50;
            break;
        case 2:
            karaoke[i * 4]     = 50;
            karaoke[i * 4 + 1] = 50;
            karaoke[i * 4 + 2] = 255;
            break;
        case 3:
            karaoke[i * 4]     = 255;
            karaoke[i * 4 + 1] = 165;
            karaoke[i * 4 + 2] = 0;
            break;
        case 4:
            karaoke[i * 4]     = 200;
            karaoke[i * 4 + 1] = 50;
            karaoke[i * 4 + 2] = 255;
            break;
        }
        karaoke[i * 4 + 3] = 255;
    }

    memcpy(combined, dialogue, (size_t)nb_dial * 4);
    memcpy(combined + (size_t)nb_dial * 4, karaoke, (size_t)nb_kara * 4);

    /* Path 1: flat */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) goto fail;
    ret = av_quantize_generate_palette(ctx, combined, nb_dial + nb_kara,
                                        pal_flat, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) goto fail;

    /* Path 2: region-weighted */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) goto fail;
    ret = av_quantize_add_region(ctx, dialogue, nb_dial);
    if (ret < 0) goto fail;
    ret = av_quantize_add_region(ctx, karaoke, nb_kara);
    if (ret < 0) goto fail;
    ret = av_quantize_generate_palette(ctx, NULL, 0, pal_region, 10);
    av_quantize_freep(&ctx);
    if (ret < 0) goto fail;

    /* Sum of squared errors for karaoke pixels only */
    for (int i = 0; i < nb_kara; i++) {
        const uint8_t *p = karaoke + i * 4;
        sum_flat   += min_palette_dist_sq(p, pal_flat, 16);
        sum_region += min_palette_dist_sq(p, pal_region, 16);
    }

    printf("karaoke_sse: flat=%" PRId64 " region=%" PRId64 " ",
           sum_flat, sum_region);

    if (sum_region >= sum_flat) {
        fprintf(stderr, "\nregion weighting did not improve karaoke quality "
                "(region=%" PRId64 " >= flat=%" PRId64 ")\n",
                sum_region, sum_flat);
        goto fail;
    }

    av_free(dialogue);
    av_free(karaoke);
    av_free(combined);
    printf("OK\n");
    return 0;

fail:
    av_free(dialogue);
    av_free(karaoke);
    av_free(combined);
    return -1;
}

/* Test Median Cut basic quantization.
 *
 * Generate a simple image with 4 distinct color clusters and verify
 * the palette contains entries close to all of them. */
static int test_mediancut_basic(void)
{
    AVQuantizeContext *ctx;
    uint32_t palette[16];
    uint8_t indices[256];
    uint8_t rgba[256 * 4];
    int nc;

    printf("test_mediancut_basic: ");

    /* 4 clusters: red, green, blue, white (64 pixels each) */
    for (int i = 0; i < 64; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
    }
    for (int i = 64; i < 128; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 255;
    }
    for (int i = 128; i < 192; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255;
    }
    for (int i = 192; i < 256; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_MEDIAN_CUT, 16);
    if (!ctx) return -1;

    nc = av_quantize_generate_palette(ctx, rgba, 256, palette, 10);
    if (nc < 0) {
        fprintf(stderr, "generate_palette failed: %d\n", nc);
        av_quantize_freep(&ctx);
        return -1;
    }

    /* Verify we can map pixels */
    if (av_quantize_apply(ctx, rgba, indices, 256) < 0) {
        fprintf(stderr, "apply failed\n");
        av_quantize_freep(&ctx);
        return -1;
    }

    /* Check that palette has red, green, blue, white entries */
    {
        int found_r = 0, found_g = 0, found_b = 0, found_w = 0;
        for (int i = 0; i < nc; i++) {
            uint8_t r = (palette[i] >> 16) & 0xff;
            uint8_t g = (palette[i] >>  8) & 0xff;
            uint8_t b =  palette[i]        & 0xff;
            if (r > 200 && g < 50 && b < 50) found_r = 1;
            if (r < 50 && g > 200 && b < 50) found_g = 1;
            if (r < 50 && g < 50 && b > 200) found_b = 1;
            if (r > 200 && g > 200 && b > 200) found_w = 1;
        }
        if (!found_r || !found_g || !found_b || !found_w) {
            fprintf(stderr, "missing cluster: R=%d G=%d B=%d W=%d\n",
                    found_r, found_g, found_b, found_w);
            av_quantize_freep(&ctx);
            return -1;
        }
    }

    av_quantize_freep(&ctx);
    printf("OK\n");
    return 0;
}

/* Test Median Cut with region-weighted quantization. */
static int test_mediancut_regions(void)
{
    AVQuantizeContext *ctx;
    uint32_t palette[16];
    uint8_t large[4000 * 4], small_buf[100 * 4];
    int nc, ret;

    printf("test_mediancut_regions: ");

    /* Large region: all white */
    for (int i = 0; i < 4000; i++) {
        large[i * 4 + 0] = 255;
        large[i * 4 + 1] = 255;
        large[i * 4 + 2] = 255;
        large[i * 4 + 3] = 255;
    }
    /* Small region: vivid red */
    for (int i = 0; i < 100; i++) {
        small_buf[i * 4 + 0] = 255;
        small_buf[i * 4 + 1] = 0;
        small_buf[i * 4 + 2] = 0;
        small_buf[i * 4 + 3] = 255;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_MEDIAN_CUT, 16);
    if (!ctx) return -1;

    ret = av_quantize_add_region(ctx, large, 4000);
    if (ret < 0) goto fail;
    ret = av_quantize_add_region(ctx, small_buf, 100);
    if (ret < 0) goto fail;

    nc = av_quantize_generate_palette(ctx, NULL, 0, palette, 10);
    if (nc < 0) goto fail;

    /* Verify red is in the palette */
    {
        int found = 0;
        for (int i = 0; i < nc; i++) {
            uint8_t r = (palette[i] >> 16) & 0xff;
            uint8_t g = (palette[i] >>  8) & 0xff;
            uint8_t b =  palette[i]        & 0xff;
            if (r > 200 && g < 80 && b < 80) found = 1;
        }
        if (!found) {
            fprintf(stderr, "red not found in region-weighted palette\n");
            goto fail;
        }
    }

    av_quantize_freep(&ctx);
    printf("OK\n");
    return 0;

fail:
    av_quantize_freep(&ctx);
    return -1;
}

/* Compare Median Cut vs NeuQuant on same input to verify both produce
 * reasonable results.  We don't require identical output, just that
 * both palettes cover the input color space adequately. */
static int test_mediancut_vs_neuquant(void)
{
    AVQuantizeContext *ctx;
    uint32_t pal_mc[16], pal_nq[16];
    uint8_t rgba[1024 * 4];
    int nc;

    printf("test_mediancut_vs_neuquant: ");

    /* Generate gradient: R varies, G/B fixed */
    for (int i = 0; i < 1024; i++) {
        rgba[i * 4 + 0] = (i * 255) / 1023;
        rgba[i * 4 + 1] = 100;
        rgba[i * 4 + 2] = 50;
        rgba[i * 4 + 3] = 255;
    }

    /* Median Cut */
    ctx = av_quantize_alloc(AV_QUANTIZE_MEDIAN_CUT, 16);
    if (!ctx) return -1;
    nc = av_quantize_generate_palette(ctx, rgba, 1024, pal_mc, 10);
    av_quantize_freep(&ctx);
    if (nc < 0) return -1;

    /* NeuQuant */
    ctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 16);
    if (!ctx) return -1;
    nc = av_quantize_generate_palette(ctx, rgba, 1024, pal_nq, 10);
    av_quantize_freep(&ctx);
    if (nc < 0) return -1;

    /* Both should have entries spanning the red range (0..255) */
    {
        int mc_min = 255, mc_max = 0, nq_min = 255, nq_max = 0;
        for (int i = 0; i < 16; i++) {
            uint8_t r;
            r = (pal_mc[i] >> 16) & 0xff;
            mc_min = FFMIN(mc_min, r);
            mc_max = FFMAX(mc_max, r);
            r = (pal_nq[i] >> 16) & 0xff;
            nq_min = FFMIN(nq_min, r);
            nq_max = FFMAX(nq_max, r);
        }
        printf("mc_range=%d..%d nq_range=%d..%d ",
               mc_min, mc_max, nq_min, nq_max);
        if (mc_max - mc_min < 150) {
            fprintf(stderr, "\nMedian Cut range too narrow: %d\n",
                    mc_max - mc_min);
            return -1;
        }
        if (nq_max - nq_min < 150) {
            fprintf(stderr, "\nNeuQuant range too narrow: %d\n",
                    nq_max - nq_min);
            return -1;
        }
    }

    printf("OK\n");
    return 0;
}

/* Test ELBG basic quantization with 4 color clusters. */
static int test_elbg_basic(void)
{
    AVQuantizeContext *ctx;
    uint32_t palette[16];
    uint8_t indices[256];
    uint8_t rgba[256 * 4];
    int nc;

    printf("test_elbg_basic: ");

    /* 4 clusters: red, green, blue, white (64 pixels each) */
    for (int i = 0; i < 64; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255;
    }
    for (int i = 64; i < 128; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 255;
    }
    for (int i = 128; i < 192; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255;
    }
    for (int i = 192; i < 256; i++) {
        uint8_t *p = rgba + i * 4;
        p[0] = 255; p[1] = 255; p[2] = 255; p[3] = 255;
    }

    ctx = av_quantize_alloc(AV_QUANTIZE_ELBG, 16);
    if (!ctx) return -1;

    nc = av_quantize_generate_palette(ctx, rgba, 256, palette, 10);
    if (nc < 0) {
        fprintf(stderr, "generate_palette failed: %d\n", nc);
        av_quantize_freep(&ctx);
        return -1;
    }

    if (av_quantize_apply(ctx, rgba, indices, 256) < 0) {
        fprintf(stderr, "apply failed\n");
        av_quantize_freep(&ctx);
        return -1;
    }

    /* Verify all indices are in range */
    for (int i = 0; i < 256; i++) {
        if (indices[i] >= 16) {
            fprintf(stderr, "index %d out of range at pixel %d\n",
                    indices[i], i);
            av_quantize_freep(&ctx);
            return -1;
        }
    }

    /* Check palette has entries near the 4 clusters */
    {
        int found_r = 0, found_g = 0, found_b = 0, found_w = 0;
        for (int i = 0; i < nc; i++) {
            uint8_t r = (palette[i] >> 16) & 0xff;
            uint8_t g = (palette[i] >>  8) & 0xff;
            uint8_t b =  palette[i]        & 0xff;
            if (r > 200 && g < 50 && b < 50) found_r = 1;
            if (r < 50 && g > 200 && b < 50) found_g = 1;
            if (r < 50 && g < 50 && b > 200) found_b = 1;
            if (r > 200 && g > 200 && b > 200) found_w = 1;
        }
        if (!found_r || !found_g || !found_b || !found_w) {
            fprintf(stderr, "missing cluster: R=%d G=%d B=%d W=%d\n",
                    found_r, found_g, found_b, found_w);
            av_quantize_freep(&ctx);
            return -1;
        }
    }

    av_quantize_freep(&ctx);
    printf("OK\n");
    return 0;
}

int main(void)
{
    int ret = 0;

    ret |= test_error_handling();
    ret |= test_small_palette();
    ret |= test_basic_quantize();
    ret |= test_region_weighted();
    ret |= test_region_backward_compat();
    ret |= test_region_starvation_comparison();
    ret |= test_region_multi_diversity();
    ret |= test_region_single_equivalence();
    ret |= test_region_karaoke();
    ret |= test_mediancut_basic();
    ret |= test_mediancut_regions();
    ret |= test_mediancut_vs_neuquant();
    ret |= test_elbg_basic();

    return !!ret;
}
