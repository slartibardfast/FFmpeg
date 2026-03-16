/*
 * Text-to-bitmap subtitle conversion for ffmpeg.
 *
 * Renders text (ASS/SSA) subtitle events to cropped bitmaps using
 * libass, quantizes to 256-color palettes, and encodes as bitmap
 * subtitles (PGS).  Handles animation detection, coalescing of
 * overlapping events, and transparent-gap splitting.
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
#include "ffmpeg.h"
#include "ffmpeg_enc_sub.h"

#include "libavcodec/avcodec.h"
#include "libavcodec/codec_desc.h"

#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/quantize.h"
#include "libavutil/rational.h"
#include "libavutil/timestamp.h"

#include <limits.h>
#include <string.h>

#include "libavfilter/subtitle_render.h"

/* ------------------------------------------------------------------ */
/* Animation utilities (shared with test harnesses via ffmpeg_sub_util.h) */
#include "libavutil/sub_util.h"

/* ------------------------------------------------------------------ */
/* Subtitle encoding context                                           */
/* ------------------------------------------------------------------ */

struct SubtitleEncContext {
    FFSubRenderContext *render;

    Scheduler *sch;
    unsigned   sch_idx;

    /* Coalescing buffer for overlapping text subtitle events.
     * Multiple ASS Dialogue lines with the same PTS are buffered
     * here and rendered as a single composite before encoding. */
    struct {
        char    **texts;
        int64_t  *durations;
        int       nb;
        int       cap;
        int64_t   pts;
        uint32_t  start_display_time;
        uint32_t  end_display_time;
    } coalesce;
};

SubtitleEncContext *enc_sub_alloc(Scheduler *sch, unsigned sch_idx)
{
    SubtitleEncContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->sch     = sch;
    ctx->sch_idx = sch_idx;
    return ctx;
}

void enc_sub_free(SubtitleEncContext **pctx)
{
    SubtitleEncContext *ctx;

    if (!pctx || !*pctx)
        return;

    ctx = *pctx;
    for (int i = 0; i < ctx->coalesce.nb; i++)
        av_freep(&ctx->coalesce.texts[i]);
    av_freep(&ctx->coalesce.texts);
    av_freep(&ctx->coalesce.durations);
    ff_sub_render_free(&ctx->render);
    av_freep(pctx);
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * Fill an AVSubtitleRect from pre-quantized palette and indices.
 * The indices_region must point to rw*rh contiguous bytes.
 */
static int fill_rect_bitmap(AVSubtitleRect *rect,
                            const uint8_t *indices_region,
                            const uint32_t *palette, int nb_colors,
                            int rx, int ry, int rw, int rh)
{
    int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);

    rect->type       = SUBTITLE_BITMAP;
    rect->x          = rx;
    rect->y          = ry;
    rect->w          = rw;
    rect->h          = rh;
    rect->nb_colors  = nb_colors;

    rect->data[0] = av_memdup(indices_region, nb_pixels);
    if (!rect->data[0])
        return AVERROR(ENOMEM);
    rect->linesize[0] = rw;

    rect->data[1] = av_memdup(palette, 256 * 4);
    if (!rect->data[1]) {
        av_freep(&rect->data[0]);
        return AVERROR(ENOMEM);
    }
    rect->linesize[1] = nb_colors * 4;

    return 0;
}

/**
 * Read the quantize_method option from the encoder's private data.
 * Falls back to AV_QUANTIZE_NEUQUANT if the option does not exist.
 */
static enum AVQuantizeAlgorithm get_quantize_algo(const AVCodecContext *enc_ctx)
{
    int64_t val;
    if (enc_ctx->priv_data &&
        av_opt_get_int(enc_ctx->priv_data, "quantize_method", 0, &val) >= 0)
        return val;
    return AV_QUANTIZE_NEUQUANT;
}

/**
 * Quantize an RGBA region and fill an AVSubtitleRect as SUBTITLE_BITMAP.
 */
static int quantize_rgba_to_rect(AVSubtitleRect *rect,
                                 const uint8_t *rgba,
                                 int rx, int ry, int rw, int rh,
                                 enum AVQuantizeAlgorithm algo)
{
    AVQuantizeContext *qctx;
    uint32_t palette[256] = {0};
    uint8_t *indices;
    int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);
    int nb_colors, ret;

    qctx = av_quantize_alloc(algo, 256);
    if (!qctx)
        return AVERROR(ENOMEM);

    nb_colors = av_quantize_generate_palette(qctx, rgba, nb_pixels,
                                             palette, 10);
    if (nb_colors < 0) {
        av_quantize_freep(&qctx);
        return nb_colors;
    }

    indices = av_malloc(nb_pixels);
    if (!indices) {
        av_quantize_freep(&qctx);
        return AVERROR(ENOMEM);
    }

    ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
    av_quantize_freep(&qctx);
    if (ret < 0) {
        av_free(indices);
        return ret;
    }

    rect->type       = SUBTITLE_BITMAP;
    rect->x          = rx;
    rect->y          = ry;
    rect->w          = rw;
    rect->h          = rh;
    rect->nb_colors  = nb_colors;
    rect->data[0]    = indices;
    rect->linesize[0] = rw;

    rect->data[1] = av_malloc(256 * 4);
    if (!rect->data[1]) {
        av_freep(&rect->data[0]);
        return AVERROR(ENOMEM);
    }
    memcpy(rect->data[1], palette, 256 * 4);
    rect->linesize[1] = nb_colors * 4;

    return 0;
}

/**
 * Lazy-init the subtitle rendering context for text-to-bitmap conversion.
 */
static int ensure_render_context(SubtitleEncContext *ctx, OutputStream *ost)
{
    AVCodecContext *enc_ctx = ost->enc->enc_ctx;

    if (ctx->render)
        return 0;

    if (enc_ctx->width <= 0 || enc_ctx->height <= 0) {
        av_log(ost->enc, AV_LOG_ERROR,
               "Canvas size required for text to bitmap subtitle "
               "conversion (use -s WxH)\n");
        return AVERROR(EINVAL);
    }

    ctx->render = ff_sub_render_alloc(enc_ctx->width, enc_ctx->height);
    if (!ctx->render)
        return AVERROR(ENOMEM);

    if (enc_ctx->subtitle_header)
        ff_sub_render_header(
            ctx->render,
            (const char *)enc_ctx->subtitle_header);

    /* Load fonts from input file attachments */
    if (ost->ist && ost->ist->file) {
        AVFormatContext *fmt = ost->ist->file->ctx;
        unsigned j;
        for (j = 0; j < fmt->nb_streams; j++) {
            AVStream *st = fmt->streams[j];
            const AVDictionaryEntry *tag;

            if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
                continue;
            if (!st->codecpar->extradata_size)
                continue;
            tag = av_dict_get(st->metadata, "filename", NULL,
                              AV_DICT_MATCH_CASE);
            if (!tag)
                continue;
            ff_sub_render_font(
                ctx->render, tag->value,
                st->codecpar->extradata,
                st->codecpar->extradata_size);
        }
    }

    return 0;
}

static int check_recording_time(OutputStream *ost, int64_t ts, AVRational tb)
{
    OutputFile *of = ost->file;

    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ts, tb, of->recording_time, AV_TIME_BASE_Q) >= 0) {
        return 0;
    }
    return 1;
}

/**
 * Encode a single subtitle frame (one avcodec_encode_subtitle call)
 * and send the resulting packet to the muxer.
 */
static int encode_subtitle_packet(SubtitleEncContext *ctx,
                                  OutputStream *ost,
                                  AVSubtitle *sub, int64_t pts,
                                  AVPacket *pkt)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc = e->enc_ctx;
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, ret;

    ret = av_new_packet(pkt, subtitle_out_max_size);
    if (ret < 0)
        return AVERROR(ENOMEM);

    e->frames_encoded++;

    subtitle_out_size = avcodec_encode_subtitle(enc, pkt->data,
                                                pkt->size, sub);
    if (subtitle_out_size < 0) {
        av_log(e, AV_LOG_FATAL, "Subtitle encoding failed\n");
        av_packet_unref(pkt);
        return subtitle_out_size;
    }

    av_shrink_packet(pkt, subtitle_out_size);
    pkt->time_base = AV_TIME_BASE_Q;
    pkt->pts       = pts;
    pkt->dts       = pts;
    pkt->duration  = av_rescale_q(sub->end_display_time,
                                  (AVRational){ 1, 1000 },
                                  pkt->time_base);

    ret = sch_enc_send(ctx->sch, ctx->sch_idx, pkt);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }

    /* Emit clear event for codecs that need explicit end-of-display */
    if (av_subtitle_needs_clear(enc->codec_id) &&
        sub->num_rects > 0 && sub->end_display_time) {
        AVSubtitle clear_sub = *sub;
        int64_t clear_pts = pts + av_rescale_q(sub->end_display_time,
                                               (AVRational){ 1, 1000 },
                                               AV_TIME_BASE_Q);
        clear_sub.num_rects = 0;

        ret = av_new_packet(pkt, subtitle_out_max_size);
        if (ret < 0)
            return AVERROR(ENOMEM);

        e->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, pkt->data,
                                                    pkt->size, &clear_sub);
        if (subtitle_out_size < 0) {
            av_packet_unref(pkt);
            return subtitle_out_size;
        }

        av_shrink_packet(pkt, subtitle_out_size);
        pkt->time_base = AV_TIME_BASE_Q;
        pkt->pts       = clear_pts;
        pkt->dts       = clear_pts;
        pkt->duration  = 0;

        ret = sch_enc_send(ctx->sch, ctx->sch_idx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Text-to-bitmap conversion (single event, non-animated)              */
/* ------------------------------------------------------------------ */

static int convert_text_to_bitmap(SubtitleEncContext *ctx,
                                  OutputStream *ost,
                                  AVSubtitle *sub)
{
    AVCodecContext *enc_ctx = ost->enc->enc_ctx;
    const AVCodecDescriptor *enc_desc;
    enum AVQuantizeAlgorithm algo = get_quantize_algo(enc_ctx);
    int need_convert = 0;
    unsigned i;
    int ret;

    enc_desc = avcodec_descriptor_get(enc_ctx->codec_id);
    if (!enc_desc || !(enc_desc->props & AV_CODEC_PROP_BITMAP_SUB))
        return 0;

    for (i = 0; i < sub->num_rects; i++) {
        if (sub->rects[i]->type == SUBTITLE_ASS ||
            sub->rects[i]->type == SUBTITLE_TEXT) {
            need_convert = 1;
            break;
        }
    }
    if (!need_convert)
        return 0;

    ret = ensure_render_context(ctx, ost);
    if (ret < 0)
        return ret;

    for (i = 0; i < sub->num_rects; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        const char *text;
        uint8_t *rgba = NULL;
        int linesize, rx, ry, rw, rh;
        int64_t start_ms, duration_ms;
        int gap_start, gap_end;

        if (rect->type != SUBTITLE_ASS && rect->type != SUBTITLE_TEXT)
            continue;

        text = rect->ass ? rect->ass : rect->text;
        if (!text || !text[0])
            continue;

        start_ms    = sub->start_display_time;
        duration_ms = sub->end_display_time - sub->start_display_time;

        ret = ff_sub_render_frame(ctx->render, text,
                                    start_ms, duration_ms,
                                    &rgba, &linesize,
                                    &rx, &ry, &rw, &rh);
        if (ret < 0)
            return ret;
        if (!rgba)
            continue; /* empty render */

        av_freep(&rect->ass);
        av_freep(&rect->text);

        /* Quantize the full RGBA first, then optionally split.  Both
         * halves must share one palette because PGS uses a single
         * Palette Definition Segment per Display Set. */
        {
            AVQuantizeContext *qctx;
            uint32_t pal[256] = {0};
            uint8_t *indices;
            int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);
            int nc;

            qctx = av_quantize_alloc(algo, 256);
            if (!qctx) {
                av_free(rgba);
                return AVERROR(ENOMEM);
            }

            nc = av_quantize_generate_palette(qctx, rgba, nb_pixels,
                                              pal, 10);
            if (nc < 0) {
                av_quantize_freep(&qctx);
                av_free(rgba);
                return nc;
            }

            indices = av_malloc(nb_pixels);
            if (!indices) {
                av_quantize_freep(&qctx);
                av_free(rgba);
                return AVERROR(ENOMEM);
            }

            ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
            av_quantize_freep(&qctx);
            if (ret < 0) {
                av_free(indices);
                av_free(rgba);
                return ret;
            }

            if (sub->num_rects < 2 &&
                ff_sub_find_gap(rgba, linesize, rw, rh,
                                     &gap_start, &gap_end)) {
                AVSubtitleRect *bot_rect;
                AVSubtitleRect **new_rects;
                int top_h = gap_start;
                int bot_h = rh - gap_end;

                new_rects = av_realloc_array(sub->rects,
                                             sub->num_rects + 1,
                                             sizeof(*sub->rects));
                if (!new_rects) {
                    av_free(indices);
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                sub->rects = new_rects;

                bot_rect = av_mallocz(sizeof(*bot_rect));
                if (!bot_rect) {
                    av_free(indices);
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                sub->rects[sub->num_rects] = bot_rect;
                sub->num_rects++;

                ret = fill_rect_bitmap(rect, indices, pal, nc,
                                       rx, ry, rw, top_h);
                if (ret < 0) {
                    av_free(indices);
                    av_free(rgba);
                    return ret;
                }

                ret = fill_rect_bitmap(bot_rect,
                                       indices + gap_end * rw,
                                       pal, nc,
                                       rx, ry + gap_end, rw, bot_h);
                av_free(indices);
                av_free(rgba);
                if (ret < 0) {
                    av_freep(&rect->data[0]);
                    av_freep(&rect->data[1]);
                    return ret;
                }
            } else {
                /* No split -- single rect, transfer indices ownership */
                rect->type       = SUBTITLE_BITMAP;
                rect->x          = rx;
                rect->y          = ry;
                rect->w          = rw;
                rect->h          = rh;
                rect->nb_colors  = nc;
                rect->data[0]    = indices;
                rect->linesize[0] = rw;
                rect->data[1] = av_malloc(256 * 4);
                if (!rect->data[1]) {
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                memcpy(rect->data[1], pal, 256 * 4);
                rect->linesize[1] = nc * 4;
                av_free(rgba);
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Animated subtitle encoding                                          */
/* ------------------------------------------------------------------ */

/**
 * Animated subtitle encoding for PGS.
 *
 * Renders the event at every frame interval, classifies changes
 * (alpha-only, position-only, or content), and encodes using the
 * optimal PGS Display Set type for each frame. Format-agnostic:
 * works with any text subtitle format by observing renderer output.
 */
static int do_subtitle_out_animated(SubtitleEncContext *ctx,
                                    OutputFile *of, OutputStream *ost,
                                    AVSubtitle *sub, AVPacket *pkt,
                                    const char *text, int events_loaded)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc = e->enc_ctx;
    int64_t start_ms, duration_ms, base_pts, pts;
    int frame_ms, ret;
    int64_t t;

    /* Pass 1 scan state */
    uint8_t *rgba0 = NULL;
    int ls0, x0, y0, w0, h0;
    int64_t peak_alpha, first_alpha;
    int64_t peak_time;

    /* Recorded animated frame timestamps */
    int64_t *anim_times = NULL;
    int n_anim = 0, anim_cap = 0;

    enum FFSubChangeType worst_change = FF_SUB_CHANGE_NONE;

    /* Encoding state */
    AVSubtitle local_sub;
    AVSubtitleRect local_rect;
    AVSubtitleRect *local_rects[1];
    uint32_t ref_palette[256];
    uint32_t scaled_pal[256];
    int nb_colors;
    enum AVQuantizeAlgorithm algo = get_quantize_algo(enc);

    start_ms    = sub->start_display_time;
    duration_ms = sub->end_display_time - sub->start_display_time;

    if (duration_ms <= 0 || sub->pts == AV_NOPTS_VALUE)
        return 0;

    /* Frame interval from encoder framerate, fallback to ~24fps */
    if (enc->framerate.num > 0 && enc->framerate.den > 0)
        frame_ms = 1000 * enc->framerate.den / enc->framerate.num;
    else
        frame_ms = 42;
    if (frame_ms < 1)
        frame_ms = 1;

    base_pts = sub->pts;
    if (of->start_time != AV_NOPTS_VALUE)
        base_pts -= of->start_time;

    /* --- Pass 1: Scan all frames, classify, find peak alpha --- */

    if (!events_loaded) {
        ret = ff_sub_render_event(ctx->render, text,
                                         start_ms, duration_ms);
        if (ret < 0)
            return ret;
    }

    /* Render first frame */
    ret = ff_sub_render_sample(ctx->render, start_ms,
                                 &rgba0, &ls0,
                                 &x0, &y0, &w0, &h0, NULL);
    if (ret < 0)
        return ret;
    if (!rgba0)
        return 0; /* empty render -- nothing to encode */

    first_alpha = ff_sub_alpha_sum(rgba0, w0, h0, ls0);
    peak_alpha  = first_alpha;
    peak_time   = start_ms;

    for (t = start_ms + frame_ms; t <= start_ms + duration_ms;
         t += frame_ms) {
        uint8_t *rgba = NULL;
        int ls, sx, sy, sw, sh, dc;
        enum FFSubChangeType change;

        ret = ff_sub_render_sample(ctx->render, t,
                                     &rgba, &ls,
                                     &sx, &sy, &sw, &sh, &dc);
        if (ret < 0)
            goto fail;

        if (dc == 0 && rgba) {
            av_free(rgba);
            continue;
        }

        if (!rgba) {
            /* Became fully transparent -- treat as content change */
            change = FF_SUB_CHANGE_CONTENT;
        } else {
            int64_t cur_alpha;
            change = ff_sub_classify_change(rgba0, x0, y0, w0, h0, ls0,
                                              rgba, sx, sy, sw, sh, ls);
            cur_alpha = ff_sub_alpha_sum(rgba, sw, sh, ls);
            if (cur_alpha > peak_alpha) {
                peak_alpha = cur_alpha;
                peak_time  = t;
            }
            av_free(rgba);
        }

        if (change > worst_change)
            worst_change = change;

        /* Record this timestamp */
        if (n_anim >= anim_cap) {
            int64_t *tmp;
            anim_cap = anim_cap ? anim_cap * 2 : 64;
            tmp = av_realloc_array(anim_times, anim_cap,
                                   sizeof(*anim_times));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            anim_times = tmp;
        }
        anim_times[n_anim++] = t;
    }

    if (worst_change == FF_SUB_CHANGE_NONE) {
        /* Static subtitle -- quantize first frame, single encode */
        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, rgba0,
                                    x0, y0, w0, h0, algo);
        av_freep(&rgba0);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
        goto done;
    }

    av_freep(&rgba0);

    /* --- Pass 2: Re-render and encode based on classification --- */

    /* Re-init events for single-event path. For coalesced events
     * (events_loaded=1), the track still has all events from Pass 1
     * -- ass_render_frame is read-only on the track state. */
    if (!events_loaded) {
        ret = ff_sub_render_event(ctx->render, text,
                                         start_ms, duration_ms);
        if (ret < 0)
            goto fail;
    }

    if (worst_change == FF_SUB_CHANGE_ALPHA) {
        uint8_t *peak_rgba = NULL;
        int peak_ls, px, py, pw, ph;
        int alpha_pct, i;
        int palette_updates = 0;

        /* Render and quantize the peak frame as reference */
        ret = ff_sub_render_sample(ctx->render, peak_time,
                                     &peak_rgba, &peak_ls,
                                     &px, &py, &pw, &ph, NULL);
        if (ret < 0)
            goto fail;
        if (!peak_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, peak_rgba,
                                    px, py, pw, ph, algo);
        av_free(peak_rgba);
        if (ret < 0)
            goto fail;

        nb_colors = FFMIN(local_rect.nb_colors, 256);
        memcpy(ref_palette, local_rect.data[1], nb_colors * 4);

        /* Epoch Start with first frame's alpha (may be 0% for fade-in) */
        {
            uint8_t *first_rgba = NULL;
            int fls, fx, fy, fw, fh;
            int64_t fa;

            ret = ff_sub_render_event(ctx->render, text,
                                             start_ms, duration_ms);
            if (ret < 0)
                goto fail_rect;
            ret = ff_sub_render_sample(ctx->render, start_ms,
                                         &first_rgba, &fls,
                                         &fx, &fy, &fw, &fh, NULL);
            if (ret < 0)
                goto fail_rect;

            if (first_rgba && peak_alpha > 0) {
                fa = ff_sub_alpha_sum(first_rgba, fw, fh, fls);
                alpha_pct = (int)(fa * 100 / peak_alpha);
            } else {
                alpha_pct = 0;
            }
            av_free(first_rgba);

            ff_sub_scale_alpha(ref_palette, scaled_pal,
                                nb_colors, alpha_pct);
            memcpy(local_rect.data[1], scaled_pal, nb_colors * 4);
        }

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_rect;

        ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_rect;

        /* Encode each animated frame with scaled palette */
        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;
            int64_t cur_alpha;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            /* Palette version safety: reset epoch before wrapping */
            if (palette_updates >= 254) {
                memcpy(local_rect.data[1], ref_palette,
                       nb_colors * 4);
                pts = base_pts + av_rescale_q(anim_times[i],
                                              (AVRational){ 1, 1000 },
                                              AV_TIME_BASE_Q);
                local_sub.pts = pts;
                ret = encode_subtitle_packet(ctx, ost, &local_sub,
                                             pts, pkt);
                if (ret < 0)
                    goto fail_rect;
                palette_updates = 0;
            }

            ret = ff_sub_render_sample(ctx->render,
                                         anim_times[i],
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh,
                                         NULL);
            if (ret < 0)
                goto fail_rect;
            if (!rgba)
                continue;

            cur_alpha = ff_sub_alpha_sum(rgba, sw, sh, ls);
            av_free(rgba);

            alpha_pct = peak_alpha > 0
                        ? (int)(cur_alpha * 100 / peak_alpha) : 0;
            ff_sub_scale_alpha(ref_palette, scaled_pal,
                                nb_colors, alpha_pct);
            memcpy(local_rect.data[1], scaled_pal, nb_colors * 4);

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_rect;
            palette_updates++;
        }

done_rect:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    } else if (worst_change == FF_SUB_CHANGE_POSITION) {
        uint8_t *first_rgba = NULL;
        int fls, fx, fy, fw, fh, i;

        /* Quantize first frame (all frames have same bitmap) */
        ret = ff_sub_render_sample(ctx->render, start_ms,
                                     &first_rgba, &fls,
                                     &fx, &fy, &fw, &fh, NULL);
        if (ret < 0)
            goto fail;
        if (!first_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, first_rgba,
                                    fx, fy, fw, fh, algo);
        av_free(first_rgba);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_pos;

        ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_pos;

        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            ret = ff_sub_render_sample(ctx->render,
                                         anim_times[i],
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh,
                                         NULL);
            if (ret < 0)
                goto fail_pos;
            av_free(rgba);

            local_rect.x = sx;
            local_rect.y = sy;

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_pos;
        }

done_pos:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    } else {
        /* CONTENT: each frame gets independent quantization */
        uint8_t *first_rgba = NULL;
        int fls, fx, fy, fw, fh, i;

        ret = ff_sub_render_sample(ctx->render, start_ms,
                                     &first_rgba, &fls,
                                     &fx, &fy, &fw, &fh, NULL);
        if (ret < 0)
            goto fail;
        if (!first_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, first_rgba,
                                    fx, fy, fw, fh, algo);
        av_free(first_rgba);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_content;

        ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_content;

        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            ret = ff_sub_render_sample(ctx->render,
                                         anim_times[i],
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh,
                                         NULL);
            if (ret < 0)
                goto fail_content;
            if (!rgba)
                continue;

            av_freep(&local_rect.data[0]);
            av_freep(&local_rect.data[1]);
            memset(&local_rect, 0, sizeof(local_rect));
            ret = quantize_rgba_to_rect(&local_rect, rgba,
                                        sx, sy, sw, sh, algo);
            av_free(rgba);
            if (ret < 0)
                goto fail;

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ctx, ost, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_content;
        }

done_content:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    }

done:
    av_freep(&anim_times);
    av_freep(&rgba0);
    return 0;

fail_rect:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
    goto fail;
fail_pos:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
    goto fail;
fail_content:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
fail:
    av_freep(&anim_times);
    av_freep(&rgba0);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Event coalescing                                                    */
/* ------------------------------------------------------------------ */

static void sub_coalesce_reset(SubtitleEncContext *ctx)
{
    int i;
    for (i = 0; i < ctx->coalesce.nb; i++)
        av_freep(&ctx->coalesce.texts[i]);
    ctx->coalesce.nb = 0;
}

static int sub_coalesce_append(SubtitleEncContext *ctx, const char *text,
                               int64_t duration, int64_t pts,
                               uint32_t start_display_time,
                               uint32_t end_display_time)
{
    if (ctx->coalesce.nb >= ctx->coalesce.cap) {
        int new_cap = ctx->coalesce.cap ? ctx->coalesce.cap * 2 : 4;
        void *tmp;

        /* Grow both arrays. If the second realloc fails, cap is
         * not updated so the next call retries both arrays. */
        tmp = av_realloc_array(ctx->coalesce.texts,
                               new_cap,
                               sizeof(*ctx->coalesce.texts));
        if (!tmp)
            return AVERROR(ENOMEM);
        ctx->coalesce.texts = tmp;

        tmp = av_realloc_array(ctx->coalesce.durations,
                               new_cap,
                               sizeof(*ctx->coalesce.durations));
        if (!tmp)
            return AVERROR(ENOMEM);
        ctx->coalesce.durations = tmp;

        ctx->coalesce.cap = new_cap;
    }

    ctx->coalesce.texts[ctx->coalesce.nb] = av_strdup(text);
    if (!ctx->coalesce.texts[ctx->coalesce.nb])
        return AVERROR(ENOMEM);
    ctx->coalesce.durations[ctx->coalesce.nb] = duration;

    if (ctx->coalesce.nb == 0) {
        ctx->coalesce.pts                = pts;
        ctx->coalesce.start_display_time = start_display_time;
        ctx->coalesce.end_display_time   = end_display_time;
    } else {
        if (end_display_time > ctx->coalesce.end_display_time)
            ctx->coalesce.end_display_time = end_display_time;
    }

    ctx->coalesce.nb++;
    return 0;
}

/**
 * Flush buffered coalesced subtitle events.
 *
 * Renders all buffered events as a composite, quantizes, and encodes.
 * For animated events, delegates to the multi-timepoint renderer.
 */
static int flush_coalesced_subtitles(SubtitleEncContext *ctx,
                                     OutputFile *of, OutputStream *ost,
                                     AVPacket *pkt)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc = e->enc_ctx;
    int64_t start_ms, duration_ms, pts;
    int ret, i, has_animation = 0;
    uint8_t *rgba = NULL;
    int linesize, rx, ry, rw, rh;
    int gap_start, gap_end;

    AVSubtitle comp_sub;
    AVSubtitleRect comp_rect  = {0};
    AVSubtitleRect comp_rect2 = {0};
    AVSubtitleRect *comp_rects[2] = { &comp_rect, &comp_rect2 };
    enum AVQuantizeAlgorithm algo = get_quantize_algo(enc);

    if (ctx->coalesce.nb == 0)
        return 0;

    ret = ensure_render_context(ctx, ost);
    if (ret < 0)
        goto cleanup;

    start_ms    = ctx->coalesce.start_display_time;
    duration_ms = ctx->coalesce.end_display_time -
                  ctx->coalesce.start_display_time;

    /* Load all events into render context */
    ret = ff_sub_render_event(ctx->render,
              ctx->coalesce.texts[0], start_ms,
              ctx->coalesce.durations[0]);
    if (ret < 0)
        goto cleanup;

    for (i = 1; i < ctx->coalesce.nb; i++) {
        ret = ff_sub_render_add(ctx->render,
                  ctx->coalesce.texts[i], start_ms,
                  ctx->coalesce.durations[i]);
        if (ret < 0)
            goto cleanup;
    }

    /* Check if any text has animation override tags */
    for (i = 0; i < ctx->coalesce.nb; i++) {
        if (strchr(ctx->coalesce.texts[i], '{')) {
            has_animation = 1;
            break;
        }
    }

    if (has_animation) {
        AVSubtitle anim_sub = {0};
        anim_sub.pts                = ctx->coalesce.pts;
        anim_sub.start_display_time = ctx->coalesce.start_display_time;
        anim_sub.end_display_time   = ctx->coalesce.end_display_time;
        ret = do_subtitle_out_animated(ctx, of, ost, &anim_sub,
                                       pkt, NULL, 1);
        goto cleanup;
    }

    /* Static path: render composite, quantize, encode */
    ret = ff_sub_render_sample(ctx->render, start_ms,
              &rgba, &linesize, &rx, &ry, &rw, &rh, NULL);
    if (ret < 0)
        goto cleanup;
    if (!rgba) {
        ret = 0;
        goto cleanup;
    }

    pts = ctx->coalesce.pts;
    if (of->start_time != AV_NOPTS_VALUE)
        pts -= of->start_time;

    if (!check_recording_time(ost, pts, AV_TIME_BASE_Q)) {
        av_free(rgba);
        ret = AVERROR_EOF;
        goto cleanup;
    }

    /* Build composite subtitle for encoding */
    memset(&comp_sub, 0, sizeof(comp_sub));
    comp_sub.start_display_time = 0;
    comp_sub.end_display_time   = duration_ms;
    comp_sub.rects     = comp_rects;
    comp_sub.num_rects = 1;

    /* Quantize full RGBA first, then optionally split.  Both halves
     * must share one palette (PGS: one PDS per Display Set). */
    {
        AVQuantizeContext *qctx;
        uint32_t pal[256] = {0};
        uint8_t *indices;
        int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);
        int nc;

        qctx = av_quantize_alloc(algo, 256);
        if (!qctx) {
            av_free(rgba);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        /* Region-weighted quantization: render each event separately
         * so NeuQuant samples equally from each, preventing large
         * events from starving small events of palette entries.
         * The composite RGBA (rendered above) is used for apply(). */
        if (ctx->coalesce.nb > 1) {
            for (i = 0; i < ctx->coalesce.nb; i++) {
                uint8_t *ev_rgba = NULL;
                int ev_ls, ev_x, ev_y, ev_w, ev_h, nb_ev_pixels;

                ret = ff_sub_render_frame(ctx->render,
                          ctx->coalesce.texts[i], start_ms,
                          ctx->coalesce.durations[i],
                          &ev_rgba, &ev_ls,
                          &ev_x, &ev_y, &ev_w, &ev_h);
                if (ret < 0 || !ev_rgba) {
                    av_free(ev_rgba);
                    continue;
                }

                nb_ev_pixels = (int)FFMIN((int64_t)ev_w * ev_h,
                                          INT_MAX);
                ret = av_quantize_add_region(qctx, ev_rgba,
                                             nb_ev_pixels);
                av_free(ev_rgba);
                if (ret < 0) {
                    av_quantize_freep(&qctx);
                    av_free(rgba);
                    goto cleanup;
                }
            }
        }

        nc = av_quantize_generate_palette(qctx, rgba, nb_pixels,
                                          pal, 10);
        if (nc < 0) {
            av_quantize_freep(&qctx);
            av_free(rgba);
            ret = nc;
            goto cleanup;
        }

        indices = av_malloc(nb_pixels);
        if (!indices) {
            av_quantize_freep(&qctx);
            av_free(rgba);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
        av_quantize_freep(&qctx);
        if (ret < 0) {
            av_free(indices);
            av_free(rgba);
            goto cleanup;
        }

        if (ff_sub_find_gap(rgba, linesize, rw, rh,
                                 &gap_start, &gap_end)) {
            int top_h = gap_start;
            int bot_h = rh - gap_end;

            ret = fill_rect_bitmap(&comp_rect, indices, pal, nc,
                                   rx, ry, rw, top_h);
            if (ret < 0) {
                av_free(indices);
                av_free(rgba);
                goto cleanup;
            }

            ret = fill_rect_bitmap(&comp_rect2,
                                   indices + gap_end * rw,
                                   pal, nc,
                                   rx, ry + gap_end, rw, bot_h);
            av_free(indices);
            av_free(rgba);
            if (ret < 0)
                goto cleanup;

            comp_sub.num_rects = 2;
        } else {
            comp_rect.type       = SUBTITLE_BITMAP;
            comp_rect.x          = rx;
            comp_rect.y          = ry;
            comp_rect.w          = rw;
            comp_rect.h          = rh;
            comp_rect.nb_colors  = nc;
            comp_rect.data[0]    = indices;
            comp_rect.linesize[0] = rw;
            comp_rect.data[1] = av_malloc(256 * 4);
            if (!comp_rect.data[1]) {
                av_free(rgba);
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            memcpy(comp_rect.data[1], pal, 256 * 4);
            comp_rect.linesize[1] = nc * 4;
            av_free(rgba);
        }
    }

    ret = encode_subtitle_packet(ctx, ost, &comp_sub, pts, pkt);

cleanup:
    av_freep(&comp_rect.data[0]);
    av_freep(&comp_rect.data[1]);
    av_freep(&comp_rect2.data[0]);
    av_freep(&comp_rect2.data[1]);
    sub_coalesce_reset(ctx);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                     */
/* ------------------------------------------------------------------ */

int enc_sub_process(SubtitleEncContext *ctx,
                    OutputFile *of, OutputStream *ost,
                    AVSubtitle *sub, AVPacket *pkt)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc = e->enc_ctx;
    int ret;

    /* Coalesce overlapping text subtitle events for PGS bitmap encoding.
     * Multiple ASS Dialogue lines with the same PTS are buffered and
     * rendered as a single composite before quantization and encoding. */
    if (enc->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE &&
        sub->num_rects > 0 &&
        (sub->rects[0]->type == SUBTITLE_ASS ||
         sub->rects[0]->type == SUBTITLE_TEXT)) {
        const char *text = sub->rects[0]->ass ? sub->rects[0]->ass
                                               : sub->rects[0]->text;
        if (!text || !text[0])
            return 0;

        /* Flush pending events if PTS changed */
        if (ctx->coalesce.nb > 0 && sub->pts != ctx->coalesce.pts) {
            ret = flush_coalesced_subtitles(ctx, of, ost, pkt);
            if (ret < 0)
                return ret;
        }

        ret = sub_coalesce_append(ctx, text,
                    sub->end_display_time - sub->start_display_time,
                    sub->pts,
                    sub->start_display_time,
                    sub->end_display_time);
        return ret < 0 ? ret : 1; /* 1 = event consumed */
    }

    /* Flush any pending coalesced events before non-coalesced processing */
    ret = flush_coalesced_subtitles(ctx, of, ost, pkt);
    if (ret < 0)
        return ret;

    /* Convert text subtitles to bitmap if encoder requires it */
    ret = convert_text_to_bitmap(ctx, ost, sub);
    if (ret < 0)
        return ret;

    return 0; /* 0 = caller should encode normally */
}

int enc_sub_flush(SubtitleEncContext *ctx,
                  OutputFile *of, OutputStream *ost,
                  AVPacket *pkt)
{
    return flush_coalesced_subtitles(ctx, of, ost, pkt);
}
