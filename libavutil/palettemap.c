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

#include "palettemap.h"

#include "attributes.h"
#include "common.h"
#include "error.h"
#include "macros.h"
#include "mem.h"
#include "palette.h"
#include "qsort.h"

struct FFPaletteMapContext {
    struct cache_node cache[FF_PALETTE_CACHE_SIZE];
    struct color_node map[AVPALETTE_COUNT];
    uint32_t palette[AVPALETTE_COUNT];
    int transparency_index;
    int trans_thresh;
};

static av_always_inline uint32_t dither_color(uint32_t px, int er, int eg,
                                              int eb, int scale, int shift)
{
    return (px & 0xff000000)
         | av_clip_uint8((px >> 16 & 0xff) + ((er * scale) / (1<<shift))) << 16
         | av_clip_uint8((px >>  8 & 0xff) + ((eg * scale) / (1<<shift))) <<  8
         | av_clip_uint8((px       & 0xff) + ((eb * scale) / (1<<shift)));
}

static av_always_inline int diff(const struct color_info *a,
                                 const struct color_info *b,
                                 const int trans_thresh)
{
    const uint8_t alpha_a = a->srgb >> 24;
    const uint8_t alpha_b = b->srgb >> 24;

    if (alpha_a < trans_thresh && alpha_b < trans_thresh) {
        return 0;
    } else if (alpha_a >= trans_thresh && alpha_b >= trans_thresh) {
        const int64_t dL = a->lab[0] - b->lab[0];
        const int64_t da = a->lab[1] - b->lab[1];
        const int64_t db = a->lab[2] - b->lab[2];
        const int64_t ret = dL*dL + da*da + db*db;
        return FFMIN(ret, INT32_MAX - 1);
    } else {
        return INT32_MAX - 1;
    }
}

static struct color_info get_color_from_srgb(uint32_t srgb)
{
    const struct Lab lab = ff_srgb_u8_to_oklab_int(srgb);
    struct color_info ret = {.srgb=srgb, .lab={lab.L, lab.a, lab.b}};
    return ret;
}

struct nearest_color {
    int node_pos;
    int64_t dist_sqd;
};

static void colormap_nearest_node(const struct color_node *map,
                                  const int node_pos,
                                  const struct color_info *target,
                                  const int trans_thresh,
                                  struct nearest_color *nearest)
{
    const struct color_node *kd = map + node_pos;
    int nearer_kd_id, further_kd_id;
    const struct color_info *current = &kd->c;
    const int64_t current_to_target = diff(target, current, trans_thresh);

    if (current_to_target < nearest->dist_sqd) {
        nearest->node_pos = node_pos;
        nearest->dist_sqd = current_to_target;
    }

    if (kd->left_id != -1 || kd->right_id != -1) {
        const int64_t dx = target->lab[kd->split] - current->lab[kd->split];

        if (dx <= 0) nearer_kd_id = kd->left_id,  further_kd_id = kd->right_id;
        else         nearer_kd_id = kd->right_id, further_kd_id = kd->left_id;

        if (nearer_kd_id != -1)
            colormap_nearest_node(map, nearer_kd_id, target,
                                 trans_thresh, nearest);

        if (further_kd_id != -1 && dx*dx < nearest->dist_sqd)
            colormap_nearest_node(map, further_kd_id, target,
                                 trans_thresh, nearest);
    }
}

static av_always_inline uint8_t colormap_nearest(
    const struct color_node *node,
    const struct color_info *target,
                                                 const int trans_thresh)
{
    struct nearest_color res = {.dist_sqd = INT_MAX, .node_pos = -1};
    colormap_nearest_node(node, 0, target, trans_thresh, &res);
    return node[res.node_pos].palette_id;
}

/**
 * Check if the requested color is in the cache already. If not, find it in the
 * color tree and cache it.
 */
static av_always_inline int color_get(FFPaletteMapContext *ctx, uint32_t color)
{
    struct color_info clrinfo;
    const uint32_t hash = ff_lowbias32(color) & (FF_PALETTE_CACHE_SIZE - 1);
    struct cache_node *node = &ctx->cache[hash];
    struct cached_color *e;

    // first, check for transparency
    if (color>>24 < ctx->trans_thresh && ctx->transparency_index >= 0) {
        return ctx->transparency_index;
    }

    for (int i = 0; i < node->nb_entries; i++) {
        e = &node->entries[i];
        if (e->color == color)
            return e->pal_entry;
    }

    e = av_dynarray2_add((void**)&node->entries, &node->nb_entries,
                         sizeof(*node->entries), NULL);
    if (!e)
        return AVERROR(ENOMEM);
    e->color = color;
    clrinfo = get_color_from_srgb(color);
    e->pal_entry = colormap_nearest(ctx->map, &clrinfo, ctx->trans_thresh);

    return e->pal_entry;
}

static av_always_inline int get_dst_color_err(FFPaletteMapContext *ctx,
                                              uint32_t c, int *er,
                                              int *eg, int *eb)
{
    uint32_t dstc;
    const int dstx = color_get(ctx, c);
    if (dstx < 0)
        return dstx;
    dstc = ctx->palette[dstx];
    if (dstx == ctx->transparency_index) {
        *er = *eg = *eb = 0;
    } else {
        const uint8_t r = c >> 16 & 0xff;
        const uint8_t g = c >>  8 & 0xff;
        const uint8_t b = c       & 0xff;
        *er = (int)r - (int)(dstc >> 16 & 0xff);
        *eg = (int)g - (int)(dstc >>  8 & 0xff);
        *eb = (int)b - (int)(dstc       & 0xff);
    }
    return dstx;
}

static int dither_value(int p)
{
    const int q = p ^ (p >> 3);
    return   (p & 4) >> 2 | (q & 4) >> 1
           | (p & 2) << 1 | (q & 2) << 2
           | (p & 1) << 4 | (q & 1) << 5;
}

static int set_frame_internal(FFPaletteMapContext *ctx,
                              uint8_t *dst, int dst_linesize,
                              uint32_t *src, int src_linesize,
                              int x_start, int y_start, int w, int h,
                              enum FFDitheringMode dither,
                              const int *ordered_dither)
{
    src += (size_t)y_start * src_linesize;
    dst += (size_t)y_start * dst_linesize;

    w += x_start;
    h += y_start;

    for (int y = y_start; y < h; y++) {
        for (int x = x_start; x < w; x++) {
            int er, eg, eb;

            if (dither == FF_DITHERING_BAYER) {
                const int d = ordered_dither[(y & 7)<<3 | (x & 7)];
                const uint8_t a8 = src[x] >> 24;
                const uint8_t r8 = src[x] >> 16 & 0xff;
                const uint8_t g8 = src[x] >>  8 & 0xff;
                const uint8_t b8 = src[x]       & 0xff;
                const uint8_t r = av_clip_uint8(r8 + d);
                const uint8_t g = av_clip_uint8(g8 + d);
                const uint8_t b = av_clip_uint8(b8 + d);
                const uint32_t color_new = (unsigned)(a8) << 24 | r << 16 | g << 8 | b;
                const int color = color_get(ctx, color_new);

                if (color < 0)
                    return color;
                dst[x] = color;

            } else if (dither == FF_DITHERING_HECKBERT) {
                const int right = x < w - 1, down = y < h - 1;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)         src[               x + 1] = dither_color(src[               x + 1], er, eg, eb, 3, 3);
                if (         down) src[src_linesize + x    ] = dither_color(src[src_linesize + x    ], er, eg, eb, 3, 3);
                if (right && down) src[src_linesize + x + 1] = dither_color(src[src_linesize + x + 1], er, eg, eb, 2, 3);

            } else if (dither == FF_DITHERING_FLOYD_STEINBERG) {
                const int right = x < w - 1, down = y < h - 1, left = x > x_start;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)         src[               x + 1] = dither_color(src[               x + 1], er, eg, eb, 7, 4);
                if (left  && down) src[src_linesize + x - 1] = dither_color(src[src_linesize + x - 1], er, eg, eb, 3, 4);
                if (         down) src[src_linesize + x    ] = dither_color(src[src_linesize + x    ], er, eg, eb, 5, 4);
                if (right && down) src[src_linesize + x + 1] = dither_color(src[src_linesize + x + 1], er, eg, eb, 1, 4);

            } else if (dither == FF_DITHERING_SIERRA2) {
                const int right  = x < w - 1, down  = y < h - 1, left  = x > x_start;
                const int right2 = x < w - 2,                    left2 = x > x_start + 1;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)          src[                 x + 1] = dither_color(src[                 x + 1], er, eg, eb, 4, 4);
                if (right2)         src[                 x + 2] = dither_color(src[                 x + 2], er, eg, eb, 3, 4);

                if (down) {
                    if (left2)      src[  src_linesize + x - 2] = dither_color(src[  src_linesize + x - 2], er, eg, eb, 1, 4);
                    if (left)       src[  src_linesize + x - 1] = dither_color(src[  src_linesize + x - 1], er, eg, eb, 2, 4);
                    if (1)          src[  src_linesize + x    ] = dither_color(src[  src_linesize + x    ], er, eg, eb, 3, 4);
                    if (right)      src[  src_linesize + x + 1] = dither_color(src[  src_linesize + x + 1], er, eg, eb, 2, 4);
                    if (right2)     src[  src_linesize + x + 2] = dither_color(src[  src_linesize + x + 2], er, eg, eb, 1, 4);
                }

            } else if (dither == FF_DITHERING_SIERRA2_4A) {
                const int right = x < w - 1, down = y < h - 1, left = x > x_start;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)         src[               x + 1] = dither_color(src[               x + 1], er, eg, eb, 2, 2);
                if (left  && down) src[src_linesize + x - 1] = dither_color(src[src_linesize + x - 1], er, eg, eb, 1, 2);
                if (         down) src[src_linesize + x    ] = dither_color(src[src_linesize + x    ], er, eg, eb, 1, 2);

            } else if (dither == FF_DITHERING_SIERRA3) {
                const int right  = x < w - 1, down  = y < h - 1, left  = x > x_start;
                const int right2 = x < w - 2, down2 = y < h - 2, left2 = x > x_start + 1;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)         src[                 x + 1] = dither_color(src[                 x + 1], er, eg, eb, 5, 5);
                if (right2)        src[                 x + 2] = dither_color(src[                 x + 2], er, eg, eb, 3, 5);

                if (down) {
                    if (left2)     src[src_linesize   + x - 2] = dither_color(src[src_linesize   + x - 2], er, eg, eb, 2, 5);
                    if (left)      src[src_linesize   + x - 1] = dither_color(src[src_linesize   + x - 1], er, eg, eb, 4, 5);
                    if (1)         src[src_linesize   + x    ] = dither_color(src[src_linesize   + x    ], er, eg, eb, 5, 5);
                    if (right)     src[src_linesize   + x + 1] = dither_color(src[src_linesize   + x + 1], er, eg, eb, 4, 5);
                    if (right2)    src[src_linesize   + x + 2] = dither_color(src[src_linesize   + x + 2], er, eg, eb, 2, 5);

                    if (down2) {
                        if (left)  src[src_linesize*2 + x - 1] = dither_color(src[src_linesize*2 + x - 1], er, eg, eb, 2, 5);
                        if (1)     src[src_linesize*2 + x    ] = dither_color(src[src_linesize*2 + x    ], er, eg, eb, 3, 5);
                        if (right) src[src_linesize*2 + x + 1] = dither_color(src[src_linesize*2 + x + 1], er, eg, eb, 2, 5);
                    }
                }

            } else if (dither == FF_DITHERING_BURKES) {
                const int right  = x < w - 1, down  = y < h - 1, left  = x > x_start;
                const int right2 = x < w - 2,                    left2 = x > x_start + 1;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)      src[                 x + 1] = dither_color(src[                 x + 1], er, eg, eb, 8, 5);
                if (right2)     src[                 x + 2] = dither_color(src[                 x + 2], er, eg, eb, 4, 5);

                if (down) {
                    if (left2)  src[src_linesize   + x - 2] = dither_color(src[src_linesize   + x - 2], er, eg, eb, 2, 5);
                    if (left)   src[src_linesize   + x - 1] = dither_color(src[src_linesize   + x - 1], er, eg, eb, 4, 5);
                    if (1)      src[src_linesize   + x    ] = dither_color(src[src_linesize   + x    ], er, eg, eb, 8, 5);
                    if (right)  src[src_linesize   + x + 1] = dither_color(src[src_linesize   + x + 1], er, eg, eb, 4, 5);
                    if (right2) src[src_linesize   + x + 2] = dither_color(src[src_linesize   + x + 2], er, eg, eb, 2, 5);
                }

            } else if (dither == FF_DITHERING_ATKINSON) {
                const int right  = x < w - 1, down  = y < h - 1, left = x > x_start;
                const int right2 = x < w - 2, down2 = y < h - 2;
                const int color = get_dst_color_err(ctx, src[x], &er, &eg, &eb);

                if (color < 0)
                    return color;
                dst[x] = color;

                if (right)     src[                 x + 1] = dither_color(src[                 x + 1], er, eg, eb, 1, 3);
                if (right2)    src[                 x + 2] = dither_color(src[                 x + 2], er, eg, eb, 1, 3);

                if (down) {
                    if (left)  src[src_linesize   + x - 1] = dither_color(src[src_linesize   + x - 1], er, eg, eb, 1, 3);
                    if (1)     src[src_linesize   + x    ] = dither_color(src[src_linesize   + x    ], er, eg, eb, 1, 3);
                    if (right) src[src_linesize   + x + 1] = dither_color(src[src_linesize   + x + 1], er, eg, eb, 1, 3);
                    if (down2) src[src_linesize*2 + x    ] = dither_color(src[src_linesize*2 + x    ], er, eg, eb, 1, 3);
                }

            } else {
                const int color = color_get(ctx, src[x]);

                if (color < 0)
                    return color;
                dst[x] = color;
            }
        }
        src += src_linesize;
        dst += dst_linesize;
    }
    return 0;
}

/* KD-tree construction helpers */

struct color {
    struct Lab value;
    uint8_t pal_id;
};

struct color_rect {
    int32_t min[3];
    int32_t max[3];
};

typedef int (*cmp_func)(const void *, const void *);

#define DECLARE_CMP_FUNC(name)                          \
static int cmp_##name(const void *pa, const void *pb)   \
{                                                       \
    const struct color *a = pa;                         \
    const struct color *b = pb;                         \
    return FFDIFFSIGN(a->value.name, b->value.name);    \
}

DECLARE_CMP_FUNC(L)
DECLARE_CMP_FUNC(a)
DECLARE_CMP_FUNC(b)

static const cmp_func cmp_funcs[] = {cmp_L, cmp_a, cmp_b};

static int get_next_color(const uint8_t *color_used, const uint32_t *palette,
                          int *component, const struct color_rect *box)
{
    int wL, wa, wb;
    int longest = 0;
    unsigned nb_color = 0;
    struct color_rect ranges;
    struct color tmp_pal[256];
    cmp_func cmpf;

    ranges.min[0] = ranges.min[1] = ranges.min[2] = 0xffff;
    ranges.max[0] = ranges.max[1] = ranges.max[2] = -0xffff;

    for (int i = 0; i < AVPALETTE_COUNT; i++) {
        const uint32_t c = palette[i];
        const uint8_t a = c >> 24;
        const struct Lab lab = ff_srgb_u8_to_oklab_int(c);

        if (color_used[i] || (a != 0xff) ||
            lab.L < box->min[0] || lab.a < box->min[1] || lab.b < box->min[2] ||
            lab.L > box->max[0] || lab.a > box->max[1] || lab.b > box->max[2])
            continue;

        if (lab.L < ranges.min[0]) ranges.min[0] = lab.L;
        if (lab.a < ranges.min[1]) ranges.min[1] = lab.a;
        if (lab.b < ranges.min[2]) ranges.min[2] = lab.b;

        if (lab.L > ranges.max[0]) ranges.max[0] = lab.L;
        if (lab.a > ranges.max[1]) ranges.max[1] = lab.a;
        if (lab.b > ranges.max[2]) ranges.max[2] = lab.b;

        tmp_pal[nb_color].value  = lab;
        tmp_pal[nb_color].pal_id = i;

        nb_color++;
    }

    if (!nb_color)
        return -1;

    /* define longest axis that will be the split component */
    wL = ranges.max[0] - ranges.min[0];
    wa = ranges.max[1] - ranges.min[1];
    wb = ranges.max[2] - ranges.min[2];
    if (wb >= wL && wb >= wa) longest = 2;
    if (wa >= wL && wa >= wb) longest = 1;
    if (wL >= wa && wL >= wb) longest = 0;
    cmpf = cmp_funcs[longest];
    *component = longest;

    /* sort along this axis to get median */
    AV_QSORT(tmp_pal, nb_color, struct color, cmpf);

    return tmp_pal[nb_color >> 1].pal_id;
}

static int colormap_insert(struct color_node *map,
                           uint8_t *color_used,
                           int *nb_used,
                           const uint32_t *palette,
                           const int trans_thresh,
                           const struct color_rect *box)
{
    int component, cur_id;
    int comp_value;
    int node_left_id = -1, node_right_id = -1;
    struct color_node *node;
    struct color_rect box1, box2;
    const int pal_id = get_next_color(color_used, palette, &component, box);

    if (pal_id < 0)
        return -1;

    /* create new node with that color */
    cur_id = (*nb_used)++;
    node = &map[cur_id];
    node->split = component;
    node->palette_id = pal_id;
    node->c = get_color_from_srgb(palette[pal_id]);

    color_used[pal_id] = 1;

    /* get the two boxes this node creates */
    box1 = box2 = *box;
    comp_value = node->c.lab[component];
    box1.max[component] = comp_value;
    box2.min[component] = FFMIN(comp_value + 1, 0xffff);

    node_left_id = colormap_insert(map, color_used, nb_used, palette, trans_thresh, &box1);

    if (box2.min[component] <= box2.max[component])
        node_right_id = colormap_insert(map, color_used, nb_used, palette, trans_thresh, &box2);

    node->left_id  = node_left_id;
    node->right_id = node_right_id;

    return cur_id;
}

static int cmp_pal_entry(const void *a, const void *b)
{
    const int c1 = *(const uint32_t *)a & 0xffffff;
    const int c2 = *(const uint32_t *)b & 0xffffff;
    return c1 - c2;
}

static void load_colormap(FFPaletteMapContext *ctx)
{
    int nb_used = 0;
    uint8_t color_used[AVPALETTE_COUNT] = {0};
    uint32_t last_color = 0;
    struct color_rect box;

    if (ctx->transparency_index >= 0) {
        FFSWAP(uint32_t, ctx->palette[ctx->transparency_index],
               ctx->palette[255]);
    }

    /* disable transparent colors and dups */
    qsort(ctx->palette,
          AVPALETTE_COUNT - (ctx->transparency_index >= 0),
          sizeof(*ctx->palette), cmp_pal_entry);

    for (int i = 0; i < AVPALETTE_COUNT; i++) {
        const uint32_t c = ctx->palette[i];
        if (i != 0 && c == last_color) {
            color_used[i] = 1;
            continue;
        }
        last_color = c;
        if (c >> 24 < ctx->trans_thresh) {
            color_used[i] = 1; // ignore transparent color(s)
            continue;
        }
    }

    box.min[0] = box.min[1] = box.min[2] = -0xffff;
    box.max[0] = box.max[1] = box.max[2] = 0xffff;

    colormap_insert(ctx->map, color_used, &nb_used, ctx->palette,
                    ctx->trans_thresh, &box);
}

FFPaletteMapContext *ff_palette_map_init(const uint32_t *palette,
                                         int trans_thresh)
{
    FFPaletteMapContext *ctx;

    if (!palette)
        return NULL;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->trans_thresh = trans_thresh;
    ctx->transparency_index = -1;

    for (int i = 0; i < AVPALETTE_COUNT; i++) {
        ctx->palette[i] = palette[i];
        if (palette[i] >> 24 < trans_thresh)
            ctx->transparency_index = i;
    }

    load_colormap(ctx);
    return ctx;
}

void ff_palette_map_uninit(FFPaletteMapContext **pctx)
{
    if (pctx && *pctx) {
        FFPaletteMapContext *ctx = *pctx;
        for (int i = 0; i < FF_PALETTE_CACHE_SIZE; i++)
            av_freep(&ctx->cache[i].entries);
        av_freep(pctx);
    }
}

int ff_palette_map_apply(FFPaletteMapContext *ctx,
                          uint8_t *dst, int dst_linesize,
                          uint32_t *src, int src_linesize,
                          int x_start, int y_start, int w, int h,
                          enum FFDitheringMode dither, int bayer_scale)
{
    int ordered_dither[8 * 8] = {0};
    int src_linesize_px = src_linesize >> 2;

    if (!ctx)
        return AVERROR(EINVAL);
    if (dither >= FF_NB_DITHERING)
        return AVERROR(EINVAL);
    if (dither == FF_DITHERING_BAYER &&
        (bayer_scale < 0 || bayer_scale > 5))
        return AVERROR(EINVAL);
    if (w < 0 || h < 0 || x_start < 0 || y_start < 0)
        return AVERROR(EINVAL);

    if (dither == FF_DITHERING_BAYER) {
        const int delta = 1 << (5 - bayer_scale);
        for (int i = 0; i < 64; i++)
            ordered_dither[i] = (dither_value(i) >> bayer_scale) - delta;
    }

    return set_frame_internal(ctx, dst, dst_linesize, src, src_linesize_px,
                              x_start, y_start, w, h, dither,
                              ordered_dither);
}

int ff_palette_map_color(FFPaletteMapContext *ctx, uint32_t color)
{
    if (!ctx)
        return AVERROR(EINVAL);
    return color_get(ctx, color);
}

const uint32_t *ff_palette_map_get_palette(const FFPaletteMapContext *ctx)
{
    if (!ctx)
        return NULL;
    return ctx->palette;
}

const struct color_node *ff_palette_map_get_nodes(const FFPaletteMapContext *ctx)
{
    if (!ctx)
        return NULL;
    return ctx->map;
}
