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
 * Use a palette to downsample an input video stream.
 */

#include "libavutil/bprint.h"
#include "libavutil/file_open.h"
#include "libavutil/internal.h"
#include "libavutil/opt.h"
#include "libavutil/palettemap.h"
#include "avfilter.h"
#include "filters.h"
#include "formats.h"
#include "framesync.h"
#include "video.h"

enum dithering_mode {
    DITHERING_NONE,
    DITHERING_BAYER,
    DITHERING_HECKBERT,
    DITHERING_FLOYD_STEINBERG,
    DITHERING_SIERRA2,
    DITHERING_SIERRA2_4A,
    DITHERING_SIERRA3,
    DITHERING_BURKES,
    DITHERING_ATKINSON,
    NB_DITHERING
};

enum diff_mode {
    DIFF_MODE_NONE,
    DIFF_MODE_RECTANGLE,
    NB_DIFF_MODE
};

typedef struct PaletteUseContext {
    const AVClass *class;
    FFFrameSync fs;
    FFPaletteMapContext *pm;
    int trans_thresh;
    int palette_loaded;
    int dither;
    int new;
    int bayer_scale;
    int diff_mode;
    AVFrame *last_in;
    AVFrame *last_out;

    /* debug options */
    char *dot_filename;
    int calc_mean_err;
    uint64_t total_mean_err;
} PaletteUseContext;

#define OFFSET(x) offsetof(PaletteUseContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption paletteuse_options[] = {
    { "dither", "select dithering mode", OFFSET(dither), AV_OPT_TYPE_INT, {.i64=DITHERING_SIERRA2_4A}, 0, NB_DITHERING-1, FLAGS, .unit = "dithering_mode" },
        { "bayer",           "ordered 8x8 bayer dithering (deterministic)",                            0, AV_OPT_TYPE_CONST, {.i64=DITHERING_BAYER},           INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "heckbert",        "dithering as defined by Paul Heckbert in 1982 (simple error diffusion)", 0, AV_OPT_TYPE_CONST, {.i64=DITHERING_HECKBERT},        INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "floyd_steinberg", "Floyd and Steingberg dithering (error diffusion)",                       0, AV_OPT_TYPE_CONST, {.i64=DITHERING_FLOYD_STEINBERG}, INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "sierra2",         "Frankie Sierra dithering v2 (error diffusion)",                          0, AV_OPT_TYPE_CONST, {.i64=DITHERING_SIERRA2},         INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "sierra2_4a",      "Frankie Sierra dithering v2 \"Lite\" (error diffusion)",                 0, AV_OPT_TYPE_CONST, {.i64=DITHERING_SIERRA2_4A},      INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "sierra3",         "Frankie Sierra dithering v3 (error diffusion)",                          0, AV_OPT_TYPE_CONST, {.i64=DITHERING_SIERRA3},         INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "burkes",          "Burkes dithering (error diffusion)",                                     0, AV_OPT_TYPE_CONST, {.i64=DITHERING_BURKES},          INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
        { "atkinson",        "Atkinson dithering by Bill Atkinson at Apple Computer (error diffusion)",0, AV_OPT_TYPE_CONST, {.i64=DITHERING_ATKINSON},        INT_MIN, INT_MAX, FLAGS, .unit = "dithering_mode" },
    { "bayer_scale", "set scale for bayer dithering", OFFSET(bayer_scale), AV_OPT_TYPE_INT, {.i64=2}, 0, 5, FLAGS },
    { "diff_mode",   "set frame difference mode",     OFFSET(diff_mode),   AV_OPT_TYPE_INT, {.i64=DIFF_MODE_NONE}, 0, NB_DIFF_MODE-1, FLAGS, .unit = "diff_mode" },
        { "rectangle", "process smallest different rectangle", 0, AV_OPT_TYPE_CONST, {.i64=DIFF_MODE_RECTANGLE}, INT_MIN, INT_MAX, FLAGS, .unit = "diff_mode" },
    { "new", "take new palette for each output frame", OFFSET(new), AV_OPT_TYPE_BOOL, {.i64=0}, 0, 1, FLAGS },
    { "alpha_threshold", "set the alpha threshold for transparency", OFFSET(trans_thresh), AV_OPT_TYPE_INT, {.i64=128}, 0, 255, FLAGS },

    /* following are the debug options, not part of the official API */
    { "debug_kdtree", "save Graphviz graph of the kdtree in specified file", OFFSET(dot_filename), AV_OPT_TYPE_STRING, {.str=NULL}, 0, 0, FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(paletteuse);

static int load_apply_palette(FFFrameSync *fs);

static int query_formats(const AVFilterContext *ctx,
                         AVFilterFormatsConfig **cfg_in,
                         AVFilterFormatsConfig **cfg_out)
{
    static const enum AVPixelFormat in_fmts[]    = {AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE};
    static const enum AVPixelFormat inpal_fmts[] = {AV_PIX_FMT_RGB32, AV_PIX_FMT_NONE};
    static const enum AVPixelFormat out_fmts[]   = {AV_PIX_FMT_PAL8,  AV_PIX_FMT_NONE};
    int ret;
    if ((ret = ff_formats_ref(ff_make_format_list(in_fmts),
                              &cfg_in[0]->formats)) < 0 ||
        (ret = ff_formats_ref(ff_make_format_list(inpal_fmts),
                              &cfg_in[1]->formats)) < 0 ||
        (ret = ff_formats_ref(ff_make_format_list(out_fmts),
                              &cfg_out[0]->formats)) < 0)
        return ret;
    return 0;
}

struct stack_node {
    int color_id;
    int dx2;
};


#define INDENT 4
static void disp_node(AVBPrint *buf,
                      const struct color_node *map,
                      int parent_id, int node_id,
                      int depth)
{
    const struct color_node *node = &map[node_id];
    const uint32_t fontcolor = node->c.lab[0] > 0x7fff ? 0 : 0xffffff;
    const int lab_comp = node->split;
    av_bprintf(buf, "%*cnode%d ["
               "label=\"%c%d%c%d%c%d%c\" "
               "fillcolor=\"#%06"PRIX32"\" "
               "fontcolor=\"#%06"PRIX32"\"]\n",
               depth*INDENT, ' ', node->palette_id,
               "[  "[lab_comp], node->c.lab[0],
               "][ "[lab_comp], node->c.lab[1],
               " ]["[lab_comp], node->c.lab[2],
               "  ]"[lab_comp],
               node->c.srgb & 0xffffff,
               fontcolor);
    if (parent_id != -1)
        av_bprintf(buf, "%*cnode%d -> node%d\n", depth*INDENT, ' ',
                   map[parent_id].palette_id, node->palette_id);
    if (node->left_id  != -1) disp_node(buf, map, node_id, node->left_id,  depth + 1);
    if (node->right_id != -1) disp_node(buf, map, node_id, node->right_id, depth + 1);
}

// debug_kdtree=kdtree.dot -> dot -Tpng kdtree.dot > kdtree.png
static int disp_tree(const struct color_node *node, const char *fname)
{
    AVBPrint buf;
    FILE *f = avpriv_fopen_utf8(fname, "w");

    if (!f) {
        int ret = AVERROR(errno);
        av_log(NULL, AV_LOG_ERROR, "Cannot open file '%s' for writing: %s\n",
               fname, av_err2str(ret));
        return ret;
    }

    av_bprint_init(&buf, 0, AV_BPRINT_SIZE_UNLIMITED);

    av_bprintf(&buf, "digraph {\n");
    av_bprintf(&buf, "    node [style=filled fontsize=10 shape=box]\n");
    disp_node(&buf, node, -1, 0, 0);
    av_bprintf(&buf, "}\n");

    fwrite(buf.str, 1, buf.len, f);
    fclose(f);
    av_bprint_finalize(&buf, NULL);
    return 0;
}

static void set_processing_window(enum diff_mode diff_mode,
                                  const AVFrame *prv_src, const AVFrame *cur_src,
                                  const AVFrame *prv_dst,       AVFrame *cur_dst,
                                  int *xp, int *yp, int *wp, int *hp)
{
    int x_start = 0, y_start = 0;
    int width  = cur_src->width;
    int height = cur_src->height;

    if (prv_src->data[0] && diff_mode == DIFF_MODE_RECTANGLE) {
        int y;
        int x_end = cur_src->width  - 1,
            y_end = cur_src->height - 1;
        const uint32_t *prv_srcp = (const uint32_t *)prv_src->data[0];
        const uint32_t *cur_srcp = (const uint32_t *)cur_src->data[0];
        const uint8_t  *prv_dstp = prv_dst->data[0];
        uint8_t        *cur_dstp = cur_dst->data[0];

        const int prv_src_linesize = prv_src->linesize[0] >> 2;
        const int cur_src_linesize = cur_src->linesize[0] >> 2;
        const int prv_dst_linesize = prv_dst->linesize[0];
        const int cur_dst_linesize = cur_dst->linesize[0];

        /* skip common lines */
        while (y_start < y_end && !memcmp(prv_srcp + y_start*prv_src_linesize,
                                          cur_srcp + y_start*cur_src_linesize,
                                          cur_src->width * 4)) {
            memcpy(cur_dstp + y_start*cur_dst_linesize,
                   prv_dstp + y_start*prv_dst_linesize,
                   cur_dst->width);
            y_start++;
        }
        while (y_end > y_start && !memcmp(prv_srcp + y_end*prv_src_linesize,
                                          cur_srcp + y_end*cur_src_linesize,
                                          cur_src->width * 4)) {
            memcpy(cur_dstp + y_end*cur_dst_linesize,
                   prv_dstp + y_end*prv_dst_linesize,
                   cur_dst->width);
            y_end--;
        }

        height = y_end + 1 - y_start;

        /* skip common columns */
        while (x_start < x_end) {
            int same_column = 1;
            for (y = y_start; y <= y_end; y++) {
                if (prv_srcp[y*prv_src_linesize + x_start] != cur_srcp[y*cur_src_linesize + x_start]) {
                    same_column = 0;
                    break;
                }
            }
            if (!same_column)
                break;
            x_start++;
        }
        while (x_end > x_start) {
            int same_column = 1;
            for (y = y_start; y <= y_end; y++) {
                if (prv_srcp[y*prv_src_linesize + x_end] != cur_srcp[y*cur_src_linesize + x_end]) {
                    same_column = 0;
                    break;
                }
            }
            if (!same_column)
                break;
            x_end--;
        }
        width = x_end + 1 - x_start;

        if (x_start) {
            for (y = y_start; y <= y_end; y++)
                memcpy(cur_dstp + y*cur_dst_linesize,
                       prv_dstp + y*prv_dst_linesize, x_start);
        }
        if (x_end != cur_src->width - 1) {
            const int copy_len = cur_src->width - 1 - x_end;
            for (y = y_start; y <= y_end; y++)
                memcpy(cur_dstp + y*cur_dst_linesize + x_end + 1,
                       prv_dstp + y*prv_dst_linesize + x_end + 1,
                       copy_len);
        }
    }
    *xp = x_start;
    *yp = y_start;
    *wp = width;
    *hp = height;
}

static int apply_palette(AVFilterLink *inlink, AVFrame *in, AVFrame **outf)
{
    int x, y, w, h, ret;
    AVFilterContext *ctx = inlink->dst;
    PaletteUseContext *s = ctx->priv;
    AVFilterLink *outlink = inlink->dst->outputs[0];

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        *outf = NULL;
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    set_processing_window(s->diff_mode, s->last_in, in,
                          s->last_out, out, &x, &y, &w, &h);
    av_frame_unref(s->last_out);
    if ((ret = av_frame_replace(s->last_in, in))   < 0 ||
        (ret = av_frame_ref(s->last_out, out))     < 0 ||
        (ret = ff_inlink_make_frame_writable(inlink, &s->last_in)) < 0) {
        av_frame_free(&out);
        *outf = NULL;
        return ret;
    }

    ff_dlog(ctx, "%dx%d rect: (%d;%d) -> (%d,%d) [area:%dx%d]\n",
            w, h, x, y, x+w, y+h, in->width, in->height);

    ret = ff_palette_map_apply(s->pm, out->data[0], out->linesize[0],
                               (uint32_t *)in->data[0], in->linesize[0],
                               x, y, w, h,
                               (enum FFDitheringMode)s->dither,
                               s->bayer_scale);
    if (ret < 0) {
        av_frame_free(&out);
        *outf = NULL;
        return ret;
    }
    memcpy(out->data[1], ff_palette_map_get_palette(s->pm), AVPALETTE_SIZE);
    *outf = out;
    return 0;
}

static int config_output(AVFilterLink *outlink)
{
    int ret;
    AVFilterContext *ctx = outlink->src;
    PaletteUseContext *s = ctx->priv;

    ret = ff_framesync_init_dualinput(&s->fs, ctx);
    if (ret < 0)
        return ret;
    s->fs.opt_repeatlast = 1; // only 1 frame in the palette
    s->fs.in[1].before = s->fs.in[1].after = EXT_INFINITY;
    s->fs.on_event = load_apply_palette;

    outlink->w = ctx->inputs[0]->w;
    outlink->h = ctx->inputs[0]->h;

    outlink->time_base = ctx->inputs[0]->time_base;
    if ((ret = ff_framesync_configure(&s->fs)) < 0)
        return ret;
    return 0;
}

static int config_input_palette(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;

    if (inlink->w * inlink->h != AVPALETTE_COUNT) {
        av_log(ctx, AV_LOG_ERROR,
               "Palette input must contain exactly %d pixels. "
               "Specified input has %dx%d=%d pixels\n",
               AVPALETTE_COUNT, inlink->w, inlink->h,
               inlink->w * inlink->h);
        return AVERROR(EINVAL);
    }
    return 0;
}

static int load_palette(PaletteUseContext *s, const AVFrame *palette_frame)
{
    int i, x, y;
    uint32_t palette[AVPALETTE_COUNT];
    const uint32_t *p = (const uint32_t *)palette_frame->data[0];
    const ptrdiff_t p_linesize = palette_frame->linesize[0] >> 2;

    i = 0;
    for (y = 0; y < palette_frame->height; y++) {
        for (x = 0; x < palette_frame->width; x++)
            palette[i++] = p[x];
        p += p_linesize;
    }

    ff_palette_map_uninit(&s->pm);
    s->pm = ff_palette_map_init(palette, s->trans_thresh);
    if (!s->pm)
        return AVERROR(ENOMEM);

    if (s->dot_filename)
        disp_tree(ff_palette_map_get_nodes(s->pm), s->dot_filename);

    if (!s->new)
        s->palette_loaded = 1;
    return 0;
}

static int load_apply_palette(FFFrameSync *fs)
{
    AVFilterContext *ctx = fs->parent;
    AVFilterLink *inlink = ctx->inputs[0];
    PaletteUseContext *s = ctx->priv;
    AVFrame *master, *second, *out = NULL;
    int ret;

    // writable for error diffusal dithering
    ret = ff_framesync_dualinput_get_writable(fs, &master, &second);
    if (ret < 0)
        return ret;
    if (!master || !second) {
        av_frame_free(&master);
        return AVERROR_BUG;
    }
    if (!s->palette_loaded) {
        ret = load_palette(s, second);
        if (ret < 0) {
            av_frame_free(&master);
            return ret;
        }
    }
    ret = apply_palette(inlink, master, &out);
    av_frame_free(&master);
    if (ret < 0)
        return ret;
    return ff_filter_frame(ctx->outputs[0], out);
}

static av_cold int init(AVFilterContext *ctx)
{
    PaletteUseContext *s = ctx->priv;

    s->last_in  = av_frame_alloc();
    s->last_out = av_frame_alloc();
    if (!s->last_in || !s->last_out)
        return AVERROR(ENOMEM);

    return 0;
}

static int activate(AVFilterContext *ctx)
{
    PaletteUseContext *s = ctx->priv;
    return ff_framesync_activate(&s->fs);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    PaletteUseContext *s = ctx->priv;

    ff_framesync_uninit(&s->fs);
    ff_palette_map_uninit(&s->pm);
    av_frame_free(&s->last_in);
    av_frame_free(&s->last_out);
}

static const AVFilterPad paletteuse_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
    },{
        .name           = "palette",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_input_palette,
    },
};

static const AVFilterPad paletteuse_outputs[] = {
    {
        .name          = "default",
        .type          = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
};

const FFFilter ff_vf_paletteuse = {
    .p.name        = "paletteuse",
    .p.description = NULL_IF_CONFIG_SMALL("Use a palette to downsample an input video stream."),
    .p.priv_class  = &paletteuse_class,
    .priv_size     = sizeof(PaletteUseContext),
    .init          = init,
    .uninit        = uninit,
    .activate      = activate,
    FILTER_INPUTS(paletteuse_inputs),
    FILTER_OUTPUTS(paletteuse_outputs),
    FILTER_QUERY_FUNC2(query_formats),
};
