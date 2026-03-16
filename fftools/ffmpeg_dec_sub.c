/*
 * Bitmap-to-text subtitle conversion for ffmpeg.
 *
 * Converts bitmap subtitle rects (PGS, DVD/VobSub) to text (ASS)
 * using Tesseract OCR.  Deduplicates consecutive identical bitmaps
 * (common in PGS fade sequences) to avoid redundant OCR calls,
 * and assembles ASS Dialogue lines with positioning tags.
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
#include "ffmpeg_dec_sub.h"

#include "libavcodec/avcodec.h"

#include "libavutil/avstring.h"
#include "libavutil/dict.h"
#include "libavutil/error.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/rational.h"
#include "libavutil/timestamp.h"

#include <limits.h>
#include <string.h>

#include "libavfilter/subtitle_ocr.h"

/* ------------------------------------------------------------------ */
/* ISO 639 language code helpers                                       */
/* ------------------------------------------------------------------ */

/**
 * Minimal ISO 639-2/B to 639-2/T conversion for the language codes
 * where bibliographic and terminological forms differ.  Only the
 * codes relevant to subtitle OCR (common Blu-ray/DVD languages) are
 * included.  For languages not listed here, the bibliographic code
 * is identical to the terminological code.
 */
static const char *iso639_bib_to_term(const char *lang)
{
    static const struct {
        const char bib[4];
        const char term[4];
    } map[] = {
        { "alb", "sqi" },
        { "arm", "hye" },
        { "baq", "eus" },
        { "bur", "mya" },
        { "chi", "zho" },
        { "cze", "ces" },
        { "dut", "nld" },
        { "fre", "fra" },
        { "geo", "kat" },
        { "ger", "deu" },
        { "gre", "ell" },
        { "ice", "isl" },
        { "mac", "mkd" },
        { "mao", "mri" },
        { "may", "msa" },
        { "per", "fas" },
        { "rum", "ron" },
        { "slo", "slk" },
        { "tib", "bod" },
        { "wel", "cym" },
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(map); i++) {
        if (!strcmp(lang, map[i].bib))
            return map[i].term;
    }
    return NULL;
}

/**
 * Map an ISO 639 language code to a Tesseract language code.
 *
 * Tesseract uses its own codes for languages with multiple scripts
 * (e.g. chi_tra, chi_sim).  This table maps from ISO 639-2/B and
 * 639-2/T codes to Tesseract codes.  For most languages the codes
 * are identical; only exceptions are listed here.
 *
 * Returns the Tesseract code, or NULL if no special mapping exists.
 */
static const char *iso639_to_tesseract(const char *lang)
{
    static const struct {
        const char *iso;
        const char *tess;
    } map[] = {
        /* Chinese: ISO 639 has one code, Tesseract splits by script.
         * Default to Traditional (common for Blu-ray/DVD subtitles). */
        { "chi",     "chi_tra" },   /* 639-2/B */
        { "zho",     "chi_tra" },   /* 639-2/T */
        /* Serbian: Tesseract has separate Cyrillic (srp) and Latin */
        { "srp",     "srp" },       /* identity -- Cyrillic default */
        /* Uzbek: Tesseract has separate Cyrillic variant */
        { "uzb",     "uzb" },       /* identity -- Latin default */
    };

    for (int i = 0; i < FF_ARRAY_ELEMS(map); i++) {
        if (!strcmp(lang, map[i].iso))
            return map[i].tess;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Grayscale conversion                                                */
/* ------------------------------------------------------------------ */

#define GRAYSCALE_PAD 16

/**
 * Convert a paletted bitmap subtitle rect to an 8-bit grayscale image
 * suitable for OCR.  Returns an av_malloc'd buffer with white padding;
 * caller frees with av_free().
 *
 * For blocky sources (DVD, <= 8 colors): binary text-body extraction.
 * For anti-aliased sources (PGS, 256-color): luminance-alpha mapping.
 */
static uint8_t *bitmap_to_grayscale(const AVSubtitleRect *rect,
                                     int *out_w, int *out_h)
{
    const uint8_t *pixels  = rect->data[0];
    const uint32_t *palette = (const uint32_t *)rect->data[1];
    int w = rect->w, h = rect->h;
    int nb_colors = FFMIN(rect->nb_colors, 256);
    int ow = w + 2 * GRAYSCALE_PAD;
    int oh = h + 2 * GRAYSCALE_PAD;
    uint8_t *gray;
    int counts[256] = {0};
    int text_idx = -1, text_count = 0;

    if (w <= 0 || h <= 0)
        return NULL;
    if ((int64_t)ow * oh > INT_MAX)
        return NULL;

    /* Count pixels per palette entry to find the dominant opaque color. */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            counts[pixels[y * rect->linesize[0] + x]]++;

    for (int i = 0; i < nb_colors; i++) {
        uint8_t a = (palette[i] >> 24) & 0xFF;
        if (a > 128 && counts[i] > text_count) {
            text_count = counts[i];
            text_idx = i;
        }
    }

    /* Allocate with white padding around the bitmap.  Subtitle rects
     * are tightly cropped; Tesseract needs margin for reliable text
     * block detection and line segmentation. */
    gray = av_malloc(ow * oh);
    if (!gray)
        return NULL;
    memset(gray, 255, ow * oh);

    if (nb_colors <= 8) {
        /* Blocky sources (DVD 4-color): text-body extraction. */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                gray[(y + GRAYSCALE_PAD) * ow + x + GRAYSCALE_PAD] =
                    (pixels[y * rect->linesize[0] + x] == text_idx)
                        ? 0 : 255;
    } else {
        /* Anti-aliased sources (PGS 256-color): luminance-alpha mapping. */
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int idx = pixels[y * rect->linesize[0] + x];
                uint32_t c = palette[idx];
                int r  = (c >> 16) & 0xFF;
                int g  = (c >> 8)  & 0xFF;
                int b  =  c        & 0xFF;
                int a  = (c >> 24) & 0xFF;
                int lum = (r * 77 + g * 150 + b * 29) >> 8;
                gray[(y + GRAYSCALE_PAD) * ow + x + GRAYSCALE_PAD] =
                    255 - (a * lum / 255);
            }
    }

    *out_w = ow;
    *out_h = oh;
    return gray;
}

/* ------------------------------------------------------------------ */
/* Alignment and ASS line construction                                 */
/* ------------------------------------------------------------------ */

/**
 * Compute ASS alignment tag (\anN) from bitmap position on canvas.
 *
 * Divides the canvas into a 3x3 grid (numpad layout):
 *   7 8 9  (top)
 *   4 5 6  (middle)
 *   1 2 3  (bottom)
 */
static int compute_alignment(int rx, int ry, int rw, int rh,
                              int canvas_w, int canvas_h)
{
    int cx = rx + rw / 2;
    int cy = ry + rh / 2;
    int col, row;

    if (cx < canvas_w / 3)
        col = 0; /* left */
    else if (cx < canvas_w * 2 / 3)
        col = 1; /* center */
    else
        col = 2; /* right */

    if (cy < canvas_h / 3)
        row = 2; /* top */
    else if (cy < canvas_h * 2 / 3)
        row = 1; /* middle */
    else
        row = 0; /* bottom */

    /* \an numpad: bottom-left=1, bottom-center=2, bottom-right=3, etc. */
    return row * 3 + col + 1;
}

/* ------------------------------------------------------------------ */
/* OCR post-processing: pipe-to-I correction                           */
/* ------------------------------------------------------------------ */

/**
 * Fix common OCR misreads: pipe '|' confused with capital 'I'.
 * Replace '|' with 'I' when it appears at a word boundary.
 */
static void ocr_fix_pipe_to_I(char *text)
{
    int len = strlen(text);
    for (int i = 0; i < len; i++) {
        if (text[i] != '|')
            continue;
        int at_start = (i == 0 || text[i - 1] == ' ' ||
                        text[i - 1] == '\n' || text[i - 1] == '-');
        int at_end   = (i + 1 >= len || text[i + 1] == ' ' ||
                        text[i + 1] == '\'' || text[i + 1] == '\n' ||
                        text[i + 1] == ',' || text[i + 1] == '.');
        if (at_start || at_end)
            text[i] = 'I';
    }
}

/* ------------------------------------------------------------------ */
/* Context and public interface                                        */
/* ------------------------------------------------------------------ */

#define SUB_OCR_MIN_DURATION_MS 200

struct SubtitleDecContext {
    FFSubOCRContext *ocr;

    Scheduler *sch;
    unsigned   sch_idx;

    /* OCR options (set via dec_sub_set_options) */
    char     *lang;
    char     *datapath;
    int       pageseg_mode;   /* -1 = default with PSM 6/7 fallback */
    int       min_duration;   /* ms, default 200 */

    /* Deduplication state for bitmap-to-text OCR conversion.
     * Consecutive bitmap events with identical pixel data are merged
     * into a single text event, avoiding redundant OCR calls for
     * palette-only changes (e.g. PGS fade sequences). */
    struct {
        uint8_t  *prev_bitmap;
        int       prev_bitmap_size;
        int       prev_w, prev_h;
        int       prev_x, prev_y;
        char     *prev_text;
        int64_t   run_pts;
        uint32_t  run_start;
        uint32_t  run_end;
        int       first_x, first_y;
        int       last_x, last_y;
        int       position_changed;
    } dedup;
};

SubtitleDecContext *dec_sub_alloc(Scheduler *sch, unsigned sch_idx)
{
    SubtitleDecContext *ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;
    ctx->sch     = sch;
    ctx->sch_idx = sch_idx;
    ctx->pageseg_mode = -1;
    ctx->min_duration = 200;
    return ctx;
}

void dec_sub_set_options(SubtitleDecContext *ctx,
                         const char *lang, const char *datapath,
                         int pageseg_mode, int min_duration)
{
    if (!ctx)
        return;
    av_freep(&ctx->lang);
    av_freep(&ctx->datapath);
    if (lang)
        ctx->lang = av_strdup(lang);
    if (datapath)
        ctx->datapath = av_strdup(datapath);
    if (pageseg_mode >= 0)
        ctx->pageseg_mode = pageseg_mode;
    if (min_duration > 0)
        ctx->min_duration = min_duration;
}

void dec_sub_free(SubtitleDecContext **pctx)
{
    SubtitleDecContext *ctx;

    if (!pctx || !*pctx)
        return;

    ctx = *pctx;
    av_freep(&ctx->dedup.prev_bitmap);
    av_freep(&ctx->dedup.prev_text);
    av_freep(&ctx->lang);
    av_freep(&ctx->datapath);
    ff_sub_ocr_free(&ctx->ocr);
    av_freep(pctx);
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/**
 * Lazy-init the OCR context for bitmap-to-text conversion.
 * Language is read from the input stream metadata, defaulting to "eng".
 */
static int ensure_ocr_context(SubtitleDecContext *ctx, OutputStream *ost)
{
    const char *lang = "eng";
    const char *datapath = NULL;
    const char *converted, *tess_lang;
    AVDictionaryEntry *tag;
    int ret;

    if (ctx->ocr)
        return 0;

    ctx->ocr = ff_sub_ocr_alloc();
    if (!ctx->ocr)
        return AVERROR(ENOSYS);

    /* CLI override takes priority over stream metadata */
    if (ctx->lang) {
        lang = ctx->lang;
    } else if (ost->ist && ost->ist->st) {
        tag = av_dict_get(ost->ist->st->metadata, "language", NULL, 0);
        if (tag && tag->value && tag->value[0]) {
            /* First check direct Tesseract mapping (handles chi->chi_tra etc.) */
            tess_lang = iso639_to_tesseract(tag->value);
            if (tess_lang) {
                lang = tess_lang;
            } else {
                /* Convert ISO 639-2/B to 639-2/T, then check again */
                converted = iso639_bib_to_term(tag->value);
                if (converted) {
                    tess_lang = iso639_to_tesseract(converted);
                    lang = tess_lang ? tess_lang : converted;
                } else {
                    lang = tag->value;
                }
            }
        }
    }

    if (ctx->datapath)
        datapath = ctx->datapath;

    av_log(NULL, AV_LOG_INFO,
           "Initializing OCR with language '%s'\n", lang);

    ret = ff_sub_ocr_init(ctx->ocr, lang, datapath);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR,
               "Failed to initialize Tesseract OCR with language '%s'. "
               "Is the training data installed?\n", lang);
        ff_sub_ocr_free(&ctx->ocr);
        return ret;
    }

    /* Apply non-default page segmentation mode if requested */
    if (ctx->pageseg_mode > 0) {
        ret = ff_sub_ocr_pageseg(ctx->ocr,
                                             ctx->pageseg_mode);
        if (ret < 0) {
            av_log(NULL, AV_LOG_ERROR,
                   "Invalid OCR page segmentation mode %d\n",
                   ctx->pageseg_mode);
            ff_sub_ocr_free(&ctx->ocr);
            return ret;
        }
    }

    return 0;
}

/**
 * Build an ASS dialogue line from OCR'd text with positioning tags.
 * Returns an av_malloc'd string; caller frees with av_free().
 */
static char *build_ocr_ass_line(const SubtitleDecContext *ctx,
                                 int is_ass_encoder,
                                 int canvas_w, int canvas_h)
{
    int an = compute_alignment(ctx->dedup.first_x,
                                ctx->dedup.first_y,
                                ctx->dedup.prev_w,
                                ctx->dedup.prev_h,
                                canvas_w, canvas_h);

    if (is_ass_encoder && ctx->dedup.position_changed) {
        return av_asprintf(
            "0,0,Default,,0,0,0,,{\\an7\\move(%d,%d,%d,%d)}%s",
            ctx->dedup.first_x, ctx->dedup.first_y,
            ctx->dedup.last_x, ctx->dedup.last_y,
            ctx->dedup.prev_text);
    } else if (is_ass_encoder) {
        return av_asprintf(
            "0,0,Default,,0,0,0,,{\\an7\\pos(%d,%d)}%s",
            ctx->dedup.first_x, ctx->dedup.first_y,
            ctx->dedup.prev_text);
    }
    return av_asprintf(
        "0,0,Default,,0,0,0,,{\\an%d}%s",
        an, ctx->dedup.prev_text);
}

/**
 * Encode and send a synthetic ASS subtitle event constructed from
 * the buffered OCR text.
 */
static int encode_subtitle_packet(SubtitleDecContext *ctx,
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
    return 0;
}

/**
 * Flush the buffered OCR dedup run as an encoded subtitle packet.
 *
 * Called when a new (different) bitmap arrives, or at end of stream.
 * Constructs a synthetic AVSubtitle with a single SUBTITLE_ASS rect
 * from the buffered OCR text and encodes it via the normal path.
 *
 * @param next_pts  PTS of the next subtitle event (AV_TIME_BASE units),
 *                  or AV_NOPTS_VALUE at end of stream.
 */
static int flush_ocr_dedup(SubtitleDecContext *ctx,
                            OutputFile *of, OutputStream *ost,
                            AVPacket *pkt, int64_t next_pts)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc = e->enc_ctx;
    AVSubtitle flush_sub = {0};
    AVSubtitleRect flush_rect = {0};
    AVSubtitleRect *flush_rects[1] = { &flush_rect };
    uint32_t duration;
    int64_t pts;
    int canvas_w, canvas_h, is_ass_encoder;
    int ret;

    if (!ctx->dedup.prev_text)
        return 0;

    duration = ctx->dedup.run_end - ctx->dedup.run_start;

    /* PGS decoder reports UINT32_MAX for end_display_time.  Compute
     * actual duration from the next event's PTS when available. */
    if (ctx->dedup.run_end == UINT32_MAX) {
        if (next_pts != AV_NOPTS_VALUE &&
            next_pts > ctx->dedup.run_pts) {
            int64_t gap_us = next_pts - ctx->dedup.run_pts;
            duration = (uint32_t)FFMIN(gap_us / 1000, UINT32_MAX - 1);
        } else {
            duration = 5000; /* fallback: 5 seconds */
        }
    }

    /* Discard events shorter than minimum duration */
    {
        int min_dur = ctx->min_duration > 0
                        ? ctx->min_duration
                        : SUB_OCR_MIN_DURATION_MS;
        if ((int)duration < min_dur) {
            av_log(NULL, AV_LOG_DEBUG,
                   "OCR: discarding short event (%ums < %dms)\n",
                   duration, min_dur);
            av_freep(&ctx->dedup.prev_text);
            av_freep(&ctx->dedup.prev_bitmap);
            ctx->dedup.prev_bitmap_size = 0;
            return 0;
        }
    }

    /* Resolve canvas dimensions */
    canvas_w = enc->width;
    canvas_h = enc->height;
    if (canvas_w <= 0 || canvas_h <= 0) {
        if (ost->ist && ost->ist->par) {
            canvas_w = ost->ist->par->width;
            canvas_h = ost->ist->par->height;
        }
    }
    if (canvas_w <= 0) canvas_w = 1920;
    if (canvas_h <= 0) canvas_h = 1080;

    is_ass_encoder = (enc->codec_id == AV_CODEC_ID_ASS);

    flush_rect.type = SUBTITLE_ASS;
    flush_rect.ass  = build_ocr_ass_line(ctx, is_ass_encoder,
                                          canvas_w, canvas_h);
    if (!flush_rect.ass) {
        av_freep(&ctx->dedup.prev_text);
        av_freep(&ctx->dedup.prev_bitmap);
        ctx->dedup.prev_bitmap_size = 0;
        return AVERROR(ENOMEM);
    }

    pts = ctx->dedup.run_pts;
    if (of->start_time != AV_NOPTS_VALUE)
        pts -= of->start_time;

    flush_sub.pts                = pts;
    flush_sub.start_display_time = 0;
    flush_sub.end_display_time   = duration;
    flush_sub.rects              = flush_rects;
    flush_sub.num_rects          = 1;

    ret = encode_subtitle_packet(ctx, ost, &flush_sub, pts, pkt);

    av_freep(&flush_rect.ass);
    av_freep(&ctx->dedup.prev_text);
    av_freep(&ctx->dedup.prev_bitmap);
    ctx->dedup.prev_bitmap_size = 0;
    ctx->dedup.position_changed = 0;

    return ret;
}

/* ------------------------------------------------------------------ */
/* Public interface                                                     */
/* ------------------------------------------------------------------ */

int dec_sub_process(SubtitleDecContext *ctx,
                    OutputFile *of, OutputStream *ost,
                    AVSubtitle *sub, AVPacket *pkt)
{
    Encoder *e = ost->enc;
    AVCodecContext *enc_ctx = e->enc_ctx;
    const AVCodecDescriptor *enc_desc;
    int need_convert = 0;
    unsigned i;
    int ret;

    enc_desc = avcodec_descriptor_get(enc_ctx->codec_id);
    if (!enc_desc || !(enc_desc->props & AV_CODEC_PROP_TEXT_SUB))
        return 0;

    for (i = 0; i < sub->num_rects; i++) {
        if (sub->rects[i]->type == SUBTITLE_BITMAP) {
            need_convert = 1;
            break;
        }
    }
    if (!need_convert)
        return 0;

    ret = ensure_ocr_context(ctx, ost);
    if (ret < 0)
        return ret;

    for (i = 0; i < sub->num_rects; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        uint8_t *gray = NULL;
        char *text = NULL;
        int gw, gh, confidence;
        int bitmap_size;

        if (rect->type != SUBTITLE_BITMAP)
            continue;

        if (!rect->data[0] || !rect->data[1] ||
            rect->w <= 0 || rect->h <= 0)
            continue;

        if ((int64_t)rect->h * rect->linesize[0] > INT_MAX)
            continue;
        bitmap_size = rect->h * rect->linesize[0];
        if (bitmap_size <= 0)
            continue;

        /* Bitmap deduplication: compare indexed pixels with previous */
        if (ctx->dedup.prev_bitmap &&
            ctx->dedup.prev_bitmap_size == bitmap_size &&
            ctx->dedup.prev_w == rect->w &&
            ctx->dedup.prev_h == rect->h &&
            memcmp(ctx->dedup.prev_bitmap,
                   rect->data[0], bitmap_size) == 0) {
            /* Same bitmap -- extend the run, skip OCR */
            ctx->dedup.run_end = sub->end_display_time;

            /* Track position changes for movement detection */
            if (rect->x != ctx->dedup.last_x ||
                rect->y != ctx->dedup.last_y) {
                ctx->dedup.position_changed = 1;
                ctx->dedup.last_x = rect->x;
                ctx->dedup.last_y = rect->y;
            }

            av_log(NULL, AV_LOG_DEBUG,
                   "OCR: bitmap dedup match, extending to %u ms\n",
                   ctx->dedup.run_end);
            return 1; /* consumed */
        }

        /* Bitmap changed -- flush previous buffered run */
        ret = flush_ocr_dedup(ctx, of, ost, pkt, sub->pts);
        if (ret < 0)
            return ret;

        /* Convert to grayscale */
        gray = bitmap_to_grayscale(rect, &gw, &gh);
        if (!gray)
            return AVERROR(ENOMEM);

        /* Run OCR */
        ret = ff_sub_ocr_recognize(ctx->ocr,
                                      gray, 1, gw, gw, gh,
                                      &text, &confidence);
        if (ret < 0) {
            av_free(gray);
            return ret;
        }

        /* OCR post-processing: pipe-to-I correction.
         * Moved from the library recognize function to the caller
         * so the library returns raw OCR output. */
        if (text)
            ocr_fix_pipe_to_I(text);

        /* PSM fallback: PSM 6 (uniform block) fails for some RTL and
         * complex scripts.  If the result is empty or very short for
         * a bitmap wide enough to contain text, retry with PSM 7
         * (single text line) which skips layout analysis. */
        if ((!text || strlen(text) < 3) && rect->w > 100) {
            av_free(text);
            text = NULL;
            ff_sub_ocr_pageseg(ctx->ocr, 7);
            ret = ff_sub_ocr_recognize(ctx->ocr,
                                          gray, 1, gw, gw, gh,
                                          &text, &confidence);
            ff_sub_ocr_pageseg(ctx->ocr, 6);
            if (ret < 0) {
                av_free(gray);
                return ret;
            }
            if (text) {
                ocr_fix_pipe_to_I(text);
                if (strlen(text) > 0)
                    av_log(NULL, AV_LOG_DEBUG,
                           "OCR: PSM 7 fallback succeeded for rect %u\n",
                           i);
            }
        }
        av_free(gray);

        if (!text || strlen(text) == 0) {
            av_log(NULL, AV_LOG_DEBUG,
                   "OCR: empty result for rect %u\n", i);
            av_free(text);
            return 1; /* consumed (nothing to emit) */
        }

        /* Strip leading and trailing whitespace.
         * RTL scripts often produce leading spaces in Tesseract output. */
        {
            int len = strlen(text);
            int start = 0;
            while (len > 0 && (text[len - 1] == '\n' ||
                               text[len - 1] == '\r' ||
                               text[len - 1] == ' '))
                text[--len] = '\0';
            while (text[start] == ' ' || text[start] == '\n' ||
                   text[start] == '\r')
                start++;
            if (start > 0)
                memmove(text, text + start, len - start + 1);
        }

        /* RTL period fixup: move leading '.' to end of its line. */
        for (char *p = text; *p; ) {
            char *line = p;
            char *eol  = strchr(p, '\n');
            int llen   = eol ? (int)(eol - line) : (int)strlen(line);
            if (*line == '.' && llen > 1 && line[1] != '.') {
                memmove(line, line + 1, llen - 1);
                line[llen - 1] = '.';
            }
            p = eol ? eol + 1 : line + llen;
        }

        if (confidence >= 0 && confidence < 50) {
            av_log(NULL, AV_LOG_WARNING,
                   "OCR: low confidence %d%% for '%s'\n",
                   confidence, text);
        }

        /* Cache bitmap for deduplication */
        av_freep(&ctx->dedup.prev_bitmap);
        ctx->dedup.prev_bitmap = av_memdup(rect->data[0],
                                            bitmap_size);
        ctx->dedup.prev_bitmap_size = bitmap_size;
        ctx->dedup.prev_w = rect->w;
        ctx->dedup.prev_h = rect->h;
        ctx->dedup.prev_x = rect->x;
        ctx->dedup.prev_y = rect->y;

        /* Buffer the new run */
        av_freep(&ctx->dedup.prev_text);
        ctx->dedup.prev_text = text;
        ctx->dedup.run_pts   = sub->pts;
        ctx->dedup.run_start = sub->start_display_time;
        ctx->dedup.run_end   = sub->end_display_time;
        ctx->dedup.first_x   = rect->x;
        ctx->dedup.first_y   = rect->y;
        ctx->dedup.last_x    = rect->x;
        ctx->dedup.last_y    = rect->y;
        ctx->dedup.position_changed = 0;

        return 1; /* consumed (buffered for later flush) */
    }

    return 0;
}

int dec_sub_flush(SubtitleDecContext *ctx,
                  OutputFile *of, OutputStream *ost,
                  AVPacket *pkt, int64_t next_pts)
{
    return flush_ocr_dedup(ctx, of, ost, pkt, next_pts);
}
