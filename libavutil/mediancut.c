/*
 * Median Cut color quantization algorithm
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

#include <stdlib.h>
#include <string.h>

#include "avassert.h"
#include "common.h"
#include "error.h"
#include "mem.h"
#include "palette.h"
#include "mediancut.h"

#define HIST_SIZE (1 << 15)

struct color_ref {
    uint32_t color;
    struct Lab lab;
    int64_t count;
};

struct range_box {
    uint32_t color;
    struct Lab avg;
    int major_axis;
    int64_t weight;
    int64_t cut_score;
    int start;
    int len;
    int sorted_by;
};

struct hist_node {
    struct color_ref *entries;
    int nb_entries;
};

struct MedianCutContext {
    int max_colors;
    int trained;
    struct hist_node histogram[HIST_SIZE];
    struct color_ref **refs;
    int nb_refs;
    struct range_box boxes[256];
    int nb_boxes;
    uint32_t palette[256];
};

typedef int (*cmp_func)(const void *, const void *);

#define DECLARE_CMP_FUNC(k0, k1, k2)                        \
static int cmp_##k0##k1##k2(const void *pa, const void *pb) \
{                                                            \
    const struct color_ref * const *a = pa;                  \
    const struct color_ref * const *b = pb;                  \
    const int c0 = FFDIFFSIGN((*a)->lab.k0, (*b)->lab.k0);   \
    const int c1 = FFDIFFSIGN((*a)->lab.k1, (*b)->lab.k1);   \
    const int c2 = FFDIFFSIGN((*a)->lab.k2, (*b)->lab.k2);   \
    return c0 ? c0 : c1 ? c1 : c2;                           \
}

DECLARE_CMP_FUNC(L, a, b)
DECLARE_CMP_FUNC(L, b, a)
DECLARE_CMP_FUNC(a, L, b)
DECLARE_CMP_FUNC(a, b, L)
DECLARE_CMP_FUNC(b, L, a)
DECLARE_CMP_FUNC(b, a, L)

enum { ID_XYZ, ID_XZY, ID_ZXY, ID_YXZ, ID_ZYX, ID_YZX };

static const cmp_func cmp_funcs[] = {
    [ID_XYZ] = cmp_Lab,
    [ID_XZY] = cmp_Lba,
    [ID_ZXY] = cmp_bLa,
    [ID_YXZ] = cmp_aLb,
    [ID_ZYX] = cmp_baL,
    [ID_YZX] = cmp_abL,
};

static int sort3id(int64_t x, int64_t y, int64_t z)
{
    if (x >= y) {
        if (y >= z) return ID_XYZ;
        if (x >= z) return ID_XZY;
        return ID_ZXY;
    }
    if (x >= z) return ID_YXZ;
    if (y >= z) return ID_YZX;
    return ID_ZYX;
}

static int cmp_color(const void *a, const void *b)
{
    const struct range_box *box1 = a;
    const struct range_box *box2 = b;
    return FFDIFFSIGN(box1->color, box2->color);
}

static void compute_box_stats(const struct color_ref **refs,
                               struct range_box *box)
{
    int64_t er2[3] = {0};
    int64_t sL = 0, sa = 0, sb = 0;

    box->weight = 0;
    for (int i = box->start; i < box->start + box->len; i++) {
        const struct color_ref *ref = refs[i];
        sL += ref->lab.L * ref->count;
        sa += ref->lab.a * ref->count;
        sb += ref->lab.b * ref->count;
        box->weight += ref->count;
    }
    box->avg.L = sL / box->weight;
    box->avg.a = sa / box->weight;
    box->avg.b = sb / box->weight;

    for (int i = box->start; i < box->start + box->len; i++) {
        const struct color_ref *ref = refs[i];
        const int64_t dL = ref->lab.L - box->avg.L;
        const int64_t da = ref->lab.a - box->avg.a;
        const int64_t db = ref->lab.b - box->avg.b;
        er2[0] += dL * dL * ref->count;
        er2[1] += da * da * ref->count;
        er2[2] += db * db * ref->count;
    }

    box->major_axis = sort3id(er2[0], er2[1], er2[2]);
    box->cut_score = FFMAX3(er2[0], er2[1], er2[2]);
}

static int get_next_box_id_to_split(const struct range_box *boxes,
                                     int nb_boxes, int max_colors)
{
    int best_box_id = -1;
    int64_t max_score = -1;

    if (nb_boxes >= max_colors)
        return -1;

    for (int box_id = 0; box_id < nb_boxes; box_id++) {
        const struct range_box *box = &boxes[box_id];
        if (box->len >= 2 && box->cut_score > max_score) {
            best_box_id = box_id;
            max_score = box->cut_score;
        }
    }
    return best_box_id;
}

static void split_box(struct color_ref **refs,
                       struct range_box *boxes, int *nb_boxes,
                       struct range_box *box, int n)
{
    struct range_box *new_box = &boxes[(*nb_boxes)++];
    new_box->start     = n + 1;
    new_box->len       = box->start + box->len - new_box->start;
    new_box->sorted_by = box->sorted_by;
    box->len -= new_box->len;

    av_assert0(box->len     >= 1);
    av_assert0(new_box->len >= 1);

    compute_box_stats((const struct color_ref **)refs, box);
    compute_box_stats((const struct color_ref **)refs, new_box);
}

static int color_inc(struct hist_node *hist, uint32_t color)
{
    const uint32_t hash = ff_lowbias32(color) & (HIST_SIZE - 1);
    struct hist_node *node = &hist[hash];
    struct color_ref *e;

    for (int i = 0; i < node->nb_entries; i++) {
        e = &node->entries[i];
        if (e->color == color) {
            e->count++;
            return 0;
        }
    }

    e = av_dynarray2_add((void **)&node->entries, &node->nb_entries,
                         sizeof(*node->entries), NULL);
    if (!e)
        return AVERROR(ENOMEM);
    e->color = color;
    e->lab = ff_srgb_u8_to_oklab_int(color);
    e->count = 1;
    return 1;
}

static struct color_ref **load_color_refs(const struct hist_node *hist,
                                          int nb_refs)
{
    int k = 0;
    struct color_ref **refs = av_malloc_array(nb_refs, sizeof(*refs));

    if (!refs)
        return NULL;

    for (int j = 0; j < HIST_SIZE; j++) {
        const struct hist_node *node = &hist[j];
        for (int i = 0; i < node->nb_entries; i++)
            refs[k++] = &node->entries[i];
    }

    return refs;
}

static void reset_state(MedianCutContext *ctx)
{
    for (int i = 0; i < HIST_SIZE; i++)
        av_freep(&ctx->histogram[i].entries);
    av_freep(&ctx->refs);
    ctx->nb_refs = 0;
    ctx->nb_boxes = 0;
    memset(ctx->histogram, 0, sizeof(ctx->histogram));
    memset(ctx->boxes, 0, sizeof(ctx->boxes));
}

MedianCutContext *ff_mediancut_alloc(int max_colors)
{
    MedianCutContext *ctx;

    if (max_colors < 2 || max_colors > 256)
        return NULL;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->max_colors = max_colors;
    return ctx;
}

void ff_mediancut_free(MedianCutContext **pctx)
{
    if (pctx && *pctx) {
        MedianCutContext *ctx = *pctx;
        reset_state(ctx);
        av_freep(pctx);
    }
}

int ff_mediancut_learn(MedianCutContext *ctx, const uint8_t *rgba,
                       int nb_pixels)
{
    int box_id;
    struct range_box *box;

    if (!ctx || !rgba || nb_pixels < 1)
        return AVERROR(EINVAL);

    reset_state(ctx);
    ctx->trained = 0;

    /* Build color histogram from RGBA input */
    for (int i = 0; i < nb_pixels; i++) {
        const uint8_t *p = rgba + (size_t)i * 4;
        uint32_t color = ((uint32_t)p[3] << 24) |
                         ((uint32_t)p[0] << 16) |
                         ((uint32_t)p[1] <<  8) |
                          (uint32_t)p[2];
        int ret = color_inc(ctx->histogram, color);
        if (ret < 0) {
            reset_state(ctx);
            return ret;
        }
        if (ret > 0)
            ctx->nb_refs++;
    }

    if (ctx->nb_refs < 1) {
        reset_state(ctx);
        return AVERROR(EINVAL);
    }

    /* Linearize histogram into sorted color reference array */
    ctx->refs = load_color_refs(ctx->histogram, ctx->nb_refs);
    if (!ctx->refs) {
        reset_state(ctx);
        return AVERROR(ENOMEM);
    }

    /* Initialize first box spanning all colors */
    box = &ctx->boxes[0];
    box->len = ctx->nb_refs;
    box->sorted_by = -1;
    compute_box_stats((const struct color_ref **)ctx->refs, box);
    ctx->nb_boxes = 1;

    /* Iteratively split boxes along major variance axis */
    box_id = 0;
    while (box && box->len > 1) {
        int64_t median, weight;
        int i;

        if (box->sorted_by != box->major_axis) {
            cmp_func cmpf = cmp_funcs[box->major_axis];
            qsort(&ctx->refs[box->start], box->len,
                  sizeof(struct color_ref *), cmpf);
            box->sorted_by = box->major_axis;
        }

        median = (box->weight + 1) >> 1;
        weight = 0;
        for (i = box->start; i < box->start + box->len - 2; i++) {
            weight += ctx->refs[i]->count;
            if (weight > median)
                break;
        }
        split_box(ctx->refs, ctx->boxes, &ctx->nb_boxes, box, i);

        box_id = get_next_box_id_to_split(ctx->boxes, ctx->nb_boxes,
                                           ctx->max_colors);
        box = box_id >= 0 ? &ctx->boxes[box_id] : NULL;
    }

    /* Convert box averages to sRGB palette */
    for (int i = 0; i < ctx->nb_boxes; i++)
        ctx->boxes[i].color = 0xffU << 24 |
                              ff_oklab_int_to_srgb_u8(ctx->boxes[i].avg);

    qsort(ctx->boxes, ctx->nb_boxes, sizeof(*ctx->boxes), cmp_color);

    /* Store palette in 0xAARRGGBB format */
    memset(ctx->palette, 0, sizeof(ctx->palette));
    for (int i = 0; i < ctx->nb_boxes; i++)
        ctx->palette[i] = ctx->boxes[i].color;

    ctx->trained = 1;
    return 0;
}

void ff_mediancut_get_palette(const MedianCutContext *ctx, uint32_t *palette)
{
    memcpy(palette, ctx->palette, ctx->max_colors * sizeof(uint32_t));
}

int ff_mediancut_map_pixel(const MedianCutContext *ctx,
                           uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    uint32_t color = ((uint32_t)a << 24) | ((uint32_t)r << 16) |
                     ((uint32_t)g << 8) | b;
    struct Lab target = ff_srgb_u8_to_oklab_int(color);
    int best = 0;
    int64_t best_dist = INT64_MAX;

    for (int i = 0; i < ctx->nb_boxes; i++) {
        const int64_t dL = target.L - ctx->boxes[i].avg.L;
        const int64_t da = target.a - ctx->boxes[i].avg.a;
        const int64_t db = target.b - ctx->boxes[i].avg.b;
        int64_t dist = dL * dL + da * da + db * db;
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

int ff_mediancut_is_trained(const MedianCutContext *ctx)
{
    return ctx && ctx->trained;
}
