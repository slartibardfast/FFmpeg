/*
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2022 Clément Bœsch <u pkh me>
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

/**
 * @file
 * Generate one palette for a whole video stream.
 */

#include "libavutil/avassert.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mediancut.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "video.h"

enum {
    STATS_MODE_ALL_FRAMES,
    STATS_MODE_DIFF_FRAMES,
    STATS_MODE_SINGLE_FRAMES,
    NB_STATS_MODE
};

typedef struct PaletteGenContext {
    const AVClass *class;

    int max_colors;
    int reserve_transparent;
    int stats_mode;

    AVFrame *prev_frame;                    // previous frame used for the diff stats_mode
    MedianCutContext *mc;
    int palette_pushed;                     // if the palette frame is pushed into the outlink or not
    uint8_t transparency_color[4];          // background color for transparency
} PaletteGenContext;

#define OFFSET(x) offsetof(PaletteGenContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption palettegen_options[] = {
    { "max_colors", "set the maximum number of colors to use in the palette", OFFSET(max_colors), AV_OPT_TYPE_INT, {.i64=256}, 2, 256, FLAGS },
    { "reserve_transparent", "reserve a palette entry for transparency", OFFSET(reserve_transparent), AV_OPT_TYPE_BOOL, {.i64=1}, 0, 1, FLAGS },
    { "transparency_color", "set a background color for transparency", OFFSET(transparency_color), AV_OPT_TYPE_COLOR, {.str="lime"}, 0, 0, FLAGS },
    { "stats_mode", "set statistics mode", OFFSET(stats_mode), AV_OPT_TYPE_INT, {.i64=STATS_MODE_ALL_FRAMES}, 0, NB_STATS_MODE-1, FLAGS, .unit = "mode" },
        { "full", "compute full frame histograms", 0, AV_OPT_TYPE_CONST, {.i64=STATS_MODE_ALL_FRAMES}, INT_MIN, INT_MAX, FLAGS, .unit = "mode" },
        { "diff", "compute histograms only for the part that differs from previous frame", 0, AV_OPT_TYPE_CONST, {.i64=STATS_MODE_DIFF_FRAMES}, INT_MIN, INT_MAX, FLAGS, .unit = "mode" },
        { "single", "compute new histogram for each frame", 0, AV_OPT_TYPE_CONST, {.i64=STATS_MODE_SINGLE_FRAMES}, INT_MIN, INT_MAX, FLAGS, .unit = "mode" },
    { NULL }
};

AVFILTER_DEFINE_CLASS(palettegen);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat in_fmts[]  = {AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE};
    static const enum AVPixelFormat out_fmts[] = {AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE};
    int ret;

    if ((ret = ff_formats_ref(ff_make_format_list(in_fmts) , &cfg_in[0]->formats)) < 0)
        return ret;
    if ((ret = ff_formats_ref(ff_make_format_list(out_fmts), &cfg_out[0]->formats)) < 0)
        return ret;
    return 0;
}

/**
 * Write the palette into the output frame.
 */
static void write_palette(AVFilterContext *ctx, AVFrame *out,
                          const uint32_t *palette, int nb_colors)
{
    const PaletteGenContext *s = ctx->priv;
    int idx = 0;
    uint32_t *pal = (uint32_t *)out->data[0];
    const int pal_linesize = out->linesize[0] >> 2;
    uint32_t last_color = 0;

    for (int y = 0; y < out->height; y++) {
        for (int x = 0; x < out->width; x++) {
            if (idx < nb_colors) {
                pal[x] = palette[idx++];
                if ((x || y) && pal[x] == last_color)
                    av_log(ctx, AV_LOG_WARNING, "Duped color: %08"PRIX32"\n", pal[x]);
                last_color = pal[x];
            } else {
                pal[x] = last_color; // pad with last color
            }
        }
        pal += pal_linesize;
    }

    if (s->reserve_transparent) {
        av_assert0(nb_colors < 256);
        pal[out->width - pal_linesize - 1] = AV_RB32(&s->transparency_color) >> 8;
    }
}

static double set_colorquant_ratio_meta(AVFrame *out, int nb_out, int nb_in)
{
    char buf[32];
    const double ratio = (double)nb_out / nb_in;
    snprintf(buf, sizeof(buf), "%f", ratio);
    av_dict_set(&out->metadata, "lavfi.color_quant_ratio", buf, 0);
    return ratio;
}

/**
 * Build the palette frame using the Median Cut algorithm on the
 * accumulated histogram.
 */
static AVFrame *get_palette_frame(AVFilterContext *ctx)
{
    AVFrame *out;
    PaletteGenContext *s = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    uint32_t palette[256];
    int nb_colors, nb_refs;
    double ratio;

    nb_colors = ff_mediancut_build_palette(s->mc);
    if (nb_colors < 0) {
        av_log(ctx, AV_LOG_ERROR, "Unable to build palette\n");
        return NULL;
    }

    /* create the palette frame */
    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out)
        return NULL;
    out->pts = 0;

    ff_mediancut_get_palette(s->mc, palette);
    nb_refs = ff_mediancut_nb_colors(s->mc);

    ratio = set_colorquant_ratio_meta(out, nb_colors, nb_refs);
    av_log(ctx, AV_LOG_INFO, "%d%s colors generated out of %d colors; ratio=%f\n",
           nb_colors, s->reserve_transparent ? "(+1)" : "", nb_refs, ratio);

    write_palette(ctx, out, palette, nb_colors);

    return out;
}

/**
 * Update histogram when pixels differ from previous frame.
 */
static int update_histogram_diff(MedianCutContext *mc,
                                 const AVFrame *f1, const AVFrame *f2)
{
    int x, y, ret, nb_diff_colors = 0;

    for (y = 0; y < f1->height; y++) {
        const uint32_t *p = (const uint32_t *)(f1->data[0] + y*f1->linesize[0]);
        const uint32_t *q = (const uint32_t *)(f2->data[0] + y*f2->linesize[0]);

        for (x = 0; x < f1->width; x++) {
            if (p[x] == q[x])
                continue;
            ret = ff_mediancut_add_color(mc, p[x]);
            if (ret < 0)
                return ret;
            nb_diff_colors += ret;
        }
    }
    return nb_diff_colors;
}

/**
 * Simple histogram of the frame.
 */
static int update_histogram_frame(MedianCutContext *mc, const AVFrame *f)
{
    int x, y, ret, nb_diff_colors = 0;

    for (y = 0; y < f->height; y++) {
        const uint32_t *p = (const uint32_t *)(f->data[0] + y*f->linesize[0]);

        for (x = 0; x < f->width; x++) {
            ret = ff_mediancut_add_color(mc, p[x]);
            if (ret < 0)
                return ret;
            nb_diff_colors += ret;
        }
    }
    return nb_diff_colors;
}

/**
 * Update the histogram for each passing frame. No frame will be pushed here.
 */
static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    PaletteGenContext *s = ctx->priv;
    int ret;

    if (in->color_trc != AVCOL_TRC_UNSPECIFIED && in->color_trc != AVCOL_TRC_IEC61966_2_1)
        av_log(ctx, AV_LOG_WARNING, "The input frame is not in sRGB, colors may be off\n");

    ret = s->prev_frame ? update_histogram_diff(s->mc, s->prev_frame, in)
                        : update_histogram_frame(s->mc, in);
    if (ret < 0)
        return ret;

    if (s->stats_mode == STATS_MODE_DIFF_FRAMES) {
        av_frame_free(&s->prev_frame);
        s->prev_frame = in;
    } else if (s->stats_mode == STATS_MODE_SINGLE_FRAMES && ff_mediancut_nb_colors(s->mc) > 0) {
        AVFrame *out;

        out = get_palette_frame(ctx);
        out->pts = in->pts;
        av_frame_free(&in);
        ret = ff_filter_frame(ctx->outputs[0], out);
        ff_mediancut_reset(s->mc);
    } else {
        av_frame_free(&in);
    }

    return ret;
}

/**
 * Returns only one frame at the end containing the full palette.
 */
static int request_frame(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    AVFilterLink *inlink = ctx->inputs[0];
    PaletteGenContext *s = ctx->priv;
    int r;

    r = ff_request_frame(inlink);
    if (r == AVERROR_EOF && !s->palette_pushed && ff_mediancut_nb_colors(s->mc) > 0
            && s->stats_mode != STATS_MODE_SINGLE_FRAMES) {
        r = ff_filter_frame(outlink, get_palette_frame(ctx));
        s->palette_pushed = 1;
        return r;
    }
    return r;
}

/**
 * The output is one simple 16x16 squared-pixels palette.
 */
static int config_output(AVFilterLink *outlink)
{
    outlink->w = outlink->h = 16;
    outlink->sample_aspect_ratio = av_make_q(1, 1);
    return 0;
}

static int init(AVFilterContext *ctx)
{
    PaletteGenContext* s = ctx->priv;

    if (s->max_colors - s->reserve_transparent < 2) {
        av_log(ctx, AV_LOG_ERROR, "max_colors=2 is only allowed without reserving a transparent color slot\n");
        return AVERROR(EINVAL);
    }

    s->mc = ff_mediancut_alloc(s->max_colors - s->reserve_transparent);
    if (!s->mc)
        return AVERROR(ENOMEM);

    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PaletteGenContext *s = ctx->priv;

    ff_mediancut_free(&s->mc);
    av_frame_free(&s->prev_frame);
}

static const AVFilterPad palettegen_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
};

static const AVFilterPad palettegen_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
        .request_frame = request_frame,
    },
};

const FFFilter ff_vf_palettegen = {
    .p.name        = "palettegen",
    .p.description = NULL_IF_CONFIG_SMALL("Find the optimal palette for a given stream."),
    .p.priv_class  = &palettegen_class,
    .priv_size     = sizeof(PaletteGenContext),
    .init          = init,
    .uninit        = uninit,
    FILTER_INPUTS(palettegen_inputs),
    FILTER_OUTPUTS(palettegen_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
