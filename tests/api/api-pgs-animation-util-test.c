/*
 * PGS animation utility function tests.
 *
 * Tests classify_subtitle_change(), compute_alpha_ratio(), and
 * scale_palette_alpha() via direct .c inclusion.
 *
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
#include <string.h>

#include "libavutil/macros.h"
#include "libavutil/mem.h"

/* Include the implementation directly for access to static functions */
#include "fftools/ffmpeg_subtitle_animation.c"

/**
 * Compute the alpha ratio between two RGBA frames as a percentage.
 * Used only in tests -- the main binary computes ratios via raw sums.
 */
static int compute_alpha_ratio(const uint8_t *ref_rgba, int ref_ls,
                               const uint8_t *cur_rgba, int cur_ls,
                               int w, int h)
{
    int64_t ref_sum, cur_sum;

    ref_sum = rgba_alpha_sum(ref_rgba, w, h, ref_ls);
    cur_sum = rgba_alpha_sum(cur_rgba, w, h, cur_ls);

    if (ref_sum == 0)
        return cur_sum == 0 ? 100 : -1;

    return (int)(cur_sum * 100 / ref_sum);
}

/* Small 4x4 RGBA test buffer (linesize = 16) */
#define TEST_W  4
#define TEST_H  4
#define TEST_LS (TEST_W * 4)

static void fill_rgba(uint8_t *buf, uint8_t r, uint8_t g, uint8_t b,
                      uint8_t a)
{
    int i;

    for (i = 0; i < TEST_W * TEST_H; i++) {
        buf[i * 4]     = r;
        buf[i * 4 + 1] = g;
        buf[i * 4 + 2] = b;
        buf[i * 4 + 3] = a;
    }
}

static int test_classify_identical(void)
{
    uint8_t buf[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    fill_rgba(buf, 255, 0, 0, 200);
    ct = classify_subtitle_change(buf, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf, 10, 20, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_NONE) {
        fprintf(stderr, "  identical: expected NONE, got %d\n", ct);
        errors++;
    }
    return errors;
}

static int test_classify_position(void)
{
    uint8_t buf[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    fill_rgba(buf, 255, 0, 0, 200);
    ct = classify_subtitle_change(buf, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf, 30, 40, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_POSITION) {
        fprintf(stderr, "  position: expected POSITION, got %d\n", ct);
        errors++;
    }
    return errors;
}

static int test_classify_alpha(void)
{
    uint8_t buf0[TEST_W * TEST_H * 4];
    uint8_t buf1[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    /* Same RGB, different alpha, same position -> ALPHA */
    fill_rgba(buf0, 255, 128, 64, 200);
    fill_rgba(buf1, 255, 128, 64, 100);
    ct = classify_subtitle_change(buf0, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf1, 10, 20, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_ALPHA) {
        fprintf(stderr, "  alpha: expected ALPHA, got %d\n", ct);
        errors++;
    }

    /* Same RGB, different alpha, different position -> CONTENT */
    ct = classify_subtitle_change(buf0, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf1, 30, 40, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_CONTENT) {
        fprintf(stderr, "  alpha+pos: expected CONTENT, got %d\n", ct);
        errors++;
    }

    return errors;
}

static int test_classify_content(void)
{
    uint8_t buf0[TEST_W * TEST_H * 4];
    uint8_t buf1[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    /* Different RGB -> CONTENT */
    fill_rgba(buf0, 255, 0, 0, 200);
    fill_rgba(buf1, 0, 255, 0, 200);
    ct = classify_subtitle_change(buf0, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf1, 10, 20, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_CONTENT) {
        fprintf(stderr, "  content rgb: expected CONTENT, got %d\n", ct);
        errors++;
    }

    /* Different dimensions -> CONTENT */
    ct = classify_subtitle_change(buf0, 10, 20, 4, 4, TEST_LS,
                                  buf1, 10, 20, 8, 4, 32);
    if (ct != SUB_CHANGE_CONTENT) {
        fprintf(stderr, "  content dim: expected CONTENT, got %d\n", ct);
        errors++;
    }

    return errors;
}

static int test_classify_null(void)
{
    uint8_t buf[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    fill_rgba(buf, 255, 0, 0, 200);

    /* Both NULL -> NONE */
    ct = classify_subtitle_change(NULL, 0, 0, 0, 0, 0,
                                  NULL, 0, 0, 0, 0, 0);
    if (ct != SUB_CHANGE_NONE) {
        fprintf(stderr, "  null both: expected NONE, got %d\n", ct);
        errors++;
    }

    /* One NULL -> CONTENT */
    ct = classify_subtitle_change(buf, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  NULL, 0, 0, 0, 0, 0);
    if (ct != SUB_CHANGE_CONTENT) {
        fprintf(stderr, "  null one: expected CONTENT, got %d\n", ct);
        errors++;
    }

    return errors;
}

static int test_classify_transparent_pixels(void)
{
    uint8_t buf0[TEST_W * TEST_H * 4];
    uint8_t buf1[TEST_W * TEST_H * 4];
    enum SubtitleChangeType ct;
    int errors = 0;

    /* Both fully transparent -- RGB differs but should be ignored */
    fill_rgba(buf0, 255, 0, 0, 0);
    fill_rgba(buf1, 0, 255, 0, 0);
    ct = classify_subtitle_change(buf0, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf1, 10, 20, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_NONE) {
        fprintf(stderr, "  transparent: expected NONE, got %d\n", ct);
        errors++;
    }

    /* One transparent, one opaque -- fade-in/out pattern -> ALPHA */
    fill_rgba(buf0, 255, 0, 0, 0);
    fill_rgba(buf1, 255, 0, 0, 200);
    ct = classify_subtitle_change(buf0, 10, 20, TEST_W, TEST_H, TEST_LS,
                                  buf1, 10, 20, TEST_W, TEST_H, TEST_LS);
    if (ct != SUB_CHANGE_ALPHA) {
        fprintf(stderr, "  fade-in: expected ALPHA, got %d\n", ct);
        errors++;
    }

    return errors;
}

static int test_compute_alpha_ratio(void)
{
    uint8_t ref[TEST_W * TEST_H * 4];
    uint8_t cur[TEST_W * TEST_H * 4];
    int ratio;
    int errors = 0;

    /* 50% alpha ratio */
    fill_rgba(ref, 255, 0, 0, 200);
    fill_rgba(cur, 255, 0, 0, 100);
    ratio = compute_alpha_ratio(ref, TEST_LS, cur, TEST_LS, TEST_W, TEST_H);
    if (ratio != 50) {
        fprintf(stderr, "  ratio 50%%: expected 50, got %d\n", ratio);
        errors++;
    }

    /* 100% (identical) */
    ratio = compute_alpha_ratio(ref, TEST_LS, ref, TEST_LS, TEST_W, TEST_H);
    if (ratio != 100) {
        fprintf(stderr, "  ratio 100%%: expected 100, got %d\n", ratio);
        errors++;
    }

    /* 0% */
    fill_rgba(cur, 255, 0, 0, 0);
    ratio = compute_alpha_ratio(ref, TEST_LS, cur, TEST_LS, TEST_W, TEST_H);
    if (ratio != 0) {
        fprintf(stderr, "  ratio 0%%: expected 0, got %d\n", ratio);
        errors++;
    }

    /* Both zero -> 100% (no change) */
    fill_rgba(ref, 255, 0, 0, 0);
    fill_rgba(cur, 255, 0, 0, 0);
    ratio = compute_alpha_ratio(ref, TEST_LS, cur, TEST_LS, TEST_W, TEST_H);
    if (ratio != 100) {
        fprintf(stderr, "  ratio 0/0: expected 100, got %d\n", ratio);
        errors++;
    }

    return errors;
}

static int test_scale_palette_alpha(void)
{
    uint32_t src[4] = { 0xFF000000, 0x80FF0000, 0x4000FF00, 0x000000FF };
    uint32_t dst[4];
    int errors = 0;

    /* 50% alpha scaling */
    scale_palette_alpha(src, dst, 4, 50);

    if (((dst[0] >> 24) & 0xff) != 127) {
        fprintf(stderr, "  alpha scale [0]: expected 127, got %d\n",
                (dst[0] >> 24) & 0xff);
        errors++;
    }
    if (((dst[1] >> 24) & 0xff) != 64) {
        fprintf(stderr, "  alpha scale [1]: expected 64, got %d\n",
                (dst[1] >> 24) & 0xff);
        errors++;
    }
    /* Color channels preserved */
    if ((dst[1] & 0x00ffffff) != 0x00FF0000) {
        fprintf(stderr, "  alpha scale [1]: color channels modified\n");
        errors++;
    }

    /* 0% alpha */
    scale_palette_alpha(src, dst, 4, 0);
    if (((dst[0] >> 24) & 0xff) != 0) {
        fprintf(stderr, "  alpha scale 0%%: expected 0, got %d\n",
                (dst[0] >> 24) & 0xff);
        errors++;
    }

    /* 100% alpha */
    scale_palette_alpha(src, dst, 4, 100);
    if (((dst[0] >> 24) & 0xff) != 255) {
        fprintf(stderr, "  alpha scale 100%%: expected 255, got %d\n",
                (dst[0] >> 24) & 0xff);
        errors++;
    }

    return errors;
}

int main(void)
{
    int errors = 0;

    printf("Testing classify_subtitle_change...\n");
    errors += test_classify_identical();
    errors += test_classify_position();
    errors += test_classify_alpha();
    errors += test_classify_content();
    errors += test_classify_null();
    errors += test_classify_transparent_pixels();

    printf("Testing compute_alpha_ratio...\n");
    errors += test_compute_alpha_ratio();

    printf("Testing scale_palette_alpha...\n");
    errors += test_scale_palette_alpha();

    if (errors) {
        fprintf(stderr, "\n%d test(s) FAILED\n", errors);
        return 1;
    }

    printf("\nAll tests passed.\n");
    return 0;
}
