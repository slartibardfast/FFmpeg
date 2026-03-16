/*
 * Text subtitle rendering utility using libass.
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

#include "config.h"
#include "subtitle_render.h"

#include "libavutil/error.h"
#include "libavutil/mem.h"

#include <limits.h>
#include <string.h>

#if CONFIG_LIBASS

#include <ass/ass.h>

struct FFSubRenderContext {
    ASS_Library  *library;
    ASS_Renderer *renderer;
    ASS_Track    *track;
    int canvas_w, canvas_h;
};

FFSubRenderContext *ff_sub_render_alloc(int canvas_w,
                                                        int canvas_h)
{
    FFSubRenderContext *ctx;

    if (canvas_w <= 0 || canvas_h <= 0 ||
        (int64_t)canvas_w * canvas_h > INT_MAX / 4)
        return NULL;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->library = ass_library_init();
    if (!ctx->library)
        goto fail;

    ctx->renderer = ass_renderer_init(ctx->library);
    if (!ctx->renderer)
        goto fail;

    ass_set_frame_size(ctx->renderer, canvas_w, canvas_h);
    ass_set_storage_size(ctx->renderer, canvas_w, canvas_h);
    ass_set_fonts(ctx->renderer, NULL, NULL, 1, NULL, 1);

    ctx->track = ass_new_track(ctx->library);
    if (!ctx->track)
        goto fail;

    ctx->canvas_w = canvas_w;
    ctx->canvas_h = canvas_h;
    return ctx;

fail:
    ff_sub_render_free(&ctx);
    return NULL;
}

void ff_sub_render_free(FFSubRenderContext **pctx)
{
    FFSubRenderContext *ctx;

    if (!pctx || !*pctx)
        return;

    ctx = *pctx;
    if (ctx->track)
        ass_free_track(ctx->track);
    if (ctx->renderer)
        ass_renderer_done(ctx->renderer);
    if (ctx->library)
        ass_library_done(ctx->library);
    av_freep(pctx);
}

int ff_sub_render_header(FFSubRenderContext *ctx,
                                         const char *header)
{
    if (!ctx)
        return AVERROR(EINVAL);
    if (!header)
        return 0;

    /* libass API takes non-const but does not modify */
    ass_process_codec_private(ctx->track, (char *)header, strlen(header));
    return 0;
}

int ff_sub_render_font(FFSubRenderContext *ctx,
                                       const char *name,
                                       const uint8_t *data, int size)
{
    if (!ctx)
        return AVERROR(EINVAL);
    if (!data || size <= 0)
        return AVERROR(EINVAL);

    ass_add_font(ctx->library, (char *)name, (char *)data, size);
    return 0;
}

/* libass stores RGBA as 0xRRGGBBTT where TT is transparency (0=opaque) */
#define ASS_R(c) (((c) >> 24) & 0xFF)
#define ASS_G(c) (((c) >> 16) & 0xFF)
#define ASS_B(c) (((c) >>  8) & 0xFF)
#define ASS_A(c) (0xFF - ((c) & 0xFF))

int ff_sub_render_event(FFSubRenderContext *ctx,
                                         const char *text,
                                         int64_t start_ms,
                                         int64_t duration_ms)
{
    if (!ctx || !text)
        return AVERROR(EINVAL);

    ass_flush_events(ctx->track);
    ass_process_chunk(ctx->track, (char *)text, strlen(text),
                      start_ms, duration_ms);
    return 0;
}

int ff_sub_render_add(FFSubRenderContext *ctx,
                                        const char *text,
                                        int64_t start_ms,
                                        int64_t duration_ms)
{
    if (!ctx || !text)
        return AVERROR(EINVAL);

    ass_process_chunk(ctx->track, (char *)text, strlen(text),
                      start_ms, duration_ms);
    return 0;
}

int ff_sub_render_sample(FFSubRenderContext *ctx,
                                     int64_t render_time_ms,
                                     uint8_t **rgba, int *linesize,
                                     int *x, int *y, int *w, int *h,
                                     int *detect_change)
{
    ASS_Image *images, *img;
    int dc;
    int x_min, y_min, x_max, y_max;
    int bw, bh, stride;
    uint8_t *buf;

    if (!ctx || !rgba || !linesize || !x || !y || !w || !h)
        return AVERROR(EINVAL);

    *rgba = NULL;
    *linesize = 0;
    *x = *y = *w = *h = 0;
    if (detect_change)
        *detect_change = 0;

    images = ass_render_frame(ctx->renderer, ctx->track,
                              render_time_ms, &dc);
    if (detect_change)
        *detect_change = dc;

    if (!images)
        return 0; /* empty render is not an error */

    /* Compute bounding box over all image spans */
    x_min = ctx->canvas_w;
    y_min = ctx->canvas_h;
    x_max = 0;
    y_max = 0;
    for (img = images; img; img = img->next) {
        if (img->w == 0 || img->h == 0)
            continue;
        if (img->dst_x < x_min)
            x_min = img->dst_x;
        if (img->dst_y < y_min)
            y_min = img->dst_y;
        if (img->dst_x + img->w > x_max)
            x_max = img->dst_x + img->w;
        if (img->dst_y + img->h > y_max)
            y_max = img->dst_y + img->h;
    }

    if (x_min >= x_max || y_min >= y_max)
        return 0; /* no visible content */

    bw = x_max - x_min;
    bh = y_max - y_min;
    stride = bw * 4;

    buf = av_mallocz((size_t)stride * bh);
    if (!buf)
        return AVERROR(ENOMEM);

    /* Composite each ASS_Image span onto the RGBA canvas */
    for (img = images; img; img = img->next) {
        uint8_t r = ASS_R(img->color);
        uint8_t g = ASS_G(img->color);
        uint8_t b = ASS_B(img->color);
        uint8_t a = ASS_A(img->color);
        int ix, iy;

        if (img->w == 0 || img->h == 0)
            continue;

        for (iy = 0; iy < img->h; iy++) {
            uint8_t *dst = buf + (img->dst_y - y_min + iy) * stride +
                           (img->dst_x - x_min) * 4;
            const uint8_t *src = img->bitmap + iy * img->stride;

            for (ix = 0; ix < img->w; ix++) {
                unsigned mask = src[ix];
                unsigned sa = (mask * a + 127) / 255;
                unsigned da, dr, dg, db;

                if (sa == 0) {
                    dst += 4;
                    continue;
                }

                /* Alpha compositing: src over dst */
                da = dst[3];
                if (da == 0) {
                    dst[0] = r;
                    dst[1] = g;
                    dst[2] = b;
                    dst[3] = sa;
                } else {
                    unsigned out_a = sa + da - (sa * da + 127) / 255;
                    if (out_a == 0) {
                        dst += 4;
                        continue;
                    }
                    dr = dst[0];
                    dg = dst[1];
                    db = dst[2];
                    dst[0] = (r * sa + dr * da - dr * da * sa / 255
                              + out_a / 2) / out_a;
                    dst[1] = (g * sa + dg * da - dg * da * sa / 255
                              + out_a / 2) / out_a;
                    dst[2] = (b * sa + db * da - db * da * sa / 255
                              + out_a / 2) / out_a;
                    dst[3] = out_a;
                }
                dst += 4;
            }
        }
    }

    *rgba = buf;
    *linesize = stride;
    *x = x_min;
    *y = y_min;
    *w = bw;
    *h = bh;
    return 0;
}

int ff_sub_render_frame(FFSubRenderContext *ctx,
                                    const char *text,
                                    int64_t start_ms, int64_t duration_ms,
                                    uint8_t **rgba, int *linesize,
                                    int *x, int *y, int *w, int *h)
{
    int ret;

    ret = ff_sub_render_event(ctx, text,
                                              start_ms, duration_ms);
    if (ret < 0)
        return ret;

    return ff_sub_render_sample(ctx, start_ms,
                                           rgba, linesize,
                                           x, y, w, h, NULL);
}

#else /* !CONFIG_LIBASS */

FFSubRenderContext *ff_sub_render_alloc(int canvas_w,
                                                        int canvas_h)
{
    return NULL;
}

void ff_sub_render_free(FFSubRenderContext **ctx)
{
}

int ff_sub_render_header(FFSubRenderContext *ctx,
                                         const char *header)
{
    return AVERROR(ENOSYS);
}

int ff_sub_render_font(FFSubRenderContext *ctx,
                                       const char *name,
                                       const uint8_t *data, int size)
{
    return AVERROR(ENOSYS);
}

int ff_sub_render_frame(FFSubRenderContext *ctx,
                                    const char *text,
                                    int64_t start_ms, int64_t duration_ms,
                                    uint8_t **rgba, int *linesize,
                                    int *x, int *y, int *w, int *h)
{
    return AVERROR(ENOSYS);
}

int ff_sub_render_event(FFSubRenderContext *ctx,
                                         const char *text,
                                         int64_t start_ms,
                                         int64_t duration_ms)
{
    return AVERROR(ENOSYS);
}

int ff_sub_render_add(FFSubRenderContext *ctx,
                                        const char *text,
                                        int64_t start_ms,
                                        int64_t duration_ms)
{
    return AVERROR(ENOSYS);
}

int ff_sub_render_sample(FFSubRenderContext *ctx,
                                     int64_t render_time_ms,
                                     uint8_t **rgba, int *linesize,
                                     int *x, int *y, int *w, int *h,
                                     int *detect_change)
{
    return AVERROR(ENOSYS);
}

#endif /* CONFIG_LIBASS */
