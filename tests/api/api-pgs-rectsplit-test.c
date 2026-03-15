/*
 * PGS rect splitting (find_transparent_gap) test.
 *
 * Tests the gap-detection function that decides whether to split a
 * rendered subtitle bitmap into two PGS objects.
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

#include "libavutil/mem.h"

/* Include for find_transparent_gap */
#include "fftools/ffmpeg_sub_util.h"

#define W 16

/* Fill row y with a single opaque pixel at x=0 */
static void set_row_opaque(uint8_t *buf, int stride, int y)
{
    buf[y * stride + 3] = 255;
}

/* Allocate a fully-transparent WxH RGBA buffer */
static uint8_t *alloc_buf(int h)
{
    return av_mallocz(W * 4 * h);
}

static int test_no_gap(void)
{
    /* 64 rows, all opaque -> no gap */
    uint8_t *buf = alloc_buf(64);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 64; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, 64, &gs, &ge) != 0) {
        fprintf(stderr, "  no_gap: expected 0\n");
        errors++;
    }
    av_free(buf);
    return errors;
}

static int test_all_transparent(void)
{
    /* 64 rows, all transparent -> gap at edge, no split */
    uint8_t *buf = alloc_buf(64);
    int gs, ge, errors = 0;

    if (!buf) return 1;

    if (find_transparent_gap(buf, W * 4, W, 64, &gs, &ge) != 0) {
        fprintf(stderr, "  all_transparent: expected 0\n");
        errors++;
    }
    av_free(buf);
    return errors;
}

static int test_gap_too_small(void)
{
    /* Content at rows 0-9, 31-row gap (rows 10-40), content at 41-49.
     * Gap is 31 rows -- below the 32-row threshold. */
    int h = 50;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 10; y++)
        set_row_opaque(buf, W * 4, y);
    for (y = 41; y < h; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 0) {
        fprintf(stderr, "  gap_too_small: expected 0\n");
        errors++;
    }
    av_free(buf);
    return errors;
}

static int test_gap_at_top(void)
{
    /* 40 transparent rows at top, then content -- no content above gap */
    int h = 50;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 40; y < h; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 0) {
        fprintf(stderr, "  gap_at_top: expected 0\n");
        errors++;
    }
    av_free(buf);
    return errors;
}

static int test_gap_at_bottom(void)
{
    /* Content at top, then 40 transparent rows at bottom -- no content below */
    int h = 50;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 10; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 0) {
        fprintf(stderr, "  gap_at_bottom: expected 0\n");
        errors++;
    }
    av_free(buf);
    return errors;
}

static int test_exact_threshold(void)
{
    /* Content at rows 0-4, exactly 32-row gap (5-36), content at 37-41.
     * Gap is exactly 32 -- should split. */
    int h = 42;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 5; y++)
        set_row_opaque(buf, W * 4, y);
    for (y = 37; y < h; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 1) {
        fprintf(stderr, "  exact_threshold: expected 1\n");
        errors++;
    } else {
        if (gs != 5 || ge != 37) {
            fprintf(stderr, "  exact_threshold: expected gap 5-37, "
                    "got %d-%d\n", gs, ge);
            errors++;
        }
    }
    av_free(buf);
    return errors;
}

static int test_large_gap(void)
{
    /* Simulates top+bottom text with 900-row gap in 1080p subtitle */
    int h = 1000;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 50; y++)
        set_row_opaque(buf, W * 4, y);
    for (y = 950; y < h; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 1) {
        fprintf(stderr, "  large_gap: expected 1\n");
        errors++;
    } else {
        if (gs != 50 || ge != 950) {
            fprintf(stderr, "  large_gap: expected gap 50-950, "
                    "got %d-%d\n", gs, ge);
            errors++;
        }
    }
    av_free(buf);
    return errors;
}

static int test_multiple_gaps(void)
{
    /* Three content regions with two gaps: 40-row gap and 100-row gap.
     * Should pick the 100-row gap. */
    int h = 200;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    /* Content: rows 0-9 */
    for (y = 0; y < 10; y++)
        set_row_opaque(buf, W * 4, y);
    /* 40-row gap: rows 10-49 */
    /* Content: rows 50-59 */
    for (y = 50; y < 60; y++)
        set_row_opaque(buf, W * 4, y);
    /* 100-row gap: rows 60-159 */
    /* Content: rows 160-199 */
    for (y = 160; y < 200; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 1) {
        fprintf(stderr, "  multiple_gaps: expected 1\n");
        errors++;
    } else {
        if (gs != 60 || ge != 160) {
            fprintf(stderr, "  multiple_gaps: expected gap 60-160, "
                    "got %d-%d\n", gs, ge);
            errors++;
        }
    }
    av_free(buf);
    return errors;
}

static int test_single_pixel_breaks_row(void)
{
    /* 64-row gap but one pixel at row 30 is opaque -- splits into
     * two sub-32 gaps, neither qualifies. */
    int h = 80;
    uint8_t *buf = alloc_buf(h);
    int gs, ge, y, errors = 0;

    if (!buf) return 1;
    for (y = 0; y < 8; y++)
        set_row_opaque(buf, W * 4, y);
    /* Row 30: single opaque pixel breaks the gap */
    set_row_opaque(buf, W * 4, 38);
    for (y = 72; y < h; y++)
        set_row_opaque(buf, W * 4, y);

    if (find_transparent_gap(buf, W * 4, W, h, &gs, &ge) != 1) {
        fprintf(stderr, "  single_pixel_breaks: expected 1 "
                "(one half should still be >= 32)\n");
        errors++;
    } else {
        /* The gap 39-71 is 33 rows -- should be the best */
        if (gs != 39 || ge != 72) {
            fprintf(stderr, "  single_pixel_breaks: expected gap 39-72, "
                    "got %d-%d\n", gs, ge);
            errors++;
        }
    }
    av_free(buf);
    return errors;
}

int main(void)
{
    int errors = 0;

    printf("Testing find_transparent_gap...\n");
    errors += test_no_gap();
    errors += test_all_transparent();
    errors += test_gap_too_small();
    errors += test_gap_at_top();
    errors += test_gap_at_bottom();
    errors += test_exact_threshold();
    errors += test_large_gap();
    errors += test_multiple_gaps();
    errors += test_single_pixel_breaks_row();

    if (errors) {
        fprintf(stderr, "\n%d test(s) FAILED\n", errors);
        return 1;
    }

    printf("All rect splitting tests passed.\n");
    return 0;
}
