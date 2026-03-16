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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    printf("OK\n");
    return 0;
}

int main(void)
{
    int ret = 0;

    ret |= test_error_handling();
    ret |= test_small_palette();
    ret |= test_basic_quantize();

    return !!ret;
}
