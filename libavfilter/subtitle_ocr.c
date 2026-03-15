/*
 * Bitmap subtitle OCR utility using Tesseract.
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
#include "subtitle_ocr.h"

#include <string.h>

#include "libavutil/avstring.h"
#include "libavutil/error.h"
#include "libavutil/mem.h"

#if CONFIG_LIBTESSERACT

#include <tesseract/capi.h>

struct FFSubOCRContext {
    TessBaseAPI *tess;
    int          initialized;
};

FFSubOCRContext *ff_sub_ocr_alloc(void)
{
    FFSubOCRContext *ctx;

    ctx = av_mallocz(sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->tess = TessBaseAPICreate();
    if (!ctx->tess) {
        av_freep(&ctx);
        return NULL;
    }

    return ctx;
}

void ff_sub_ocr_free(FFSubOCRContext **pctx)
{
    FFSubOCRContext *ctx;

    if (!pctx || !*pctx)
        return;

    ctx = *pctx;
    if (ctx->tess) {
        if (ctx->initialized)
            TessBaseAPIEnd(ctx->tess);
        TessBaseAPIDelete(ctx->tess);
    }
    av_freep(pctx);
}

int ff_sub_ocr_init(FFSubOCRContext *ctx,
                                const char *language,
                                const char *datapath)
{
    if (!ctx)
        return AVERROR(EINVAL);
    if (ctx->initialized)
        return AVERROR(EINVAL);
    if (!language)
        language = "eng";

    if (TessBaseAPIInit3(ctx->tess, datapath, language) == -1)
        return AVERROR(EINVAL);

    ctx->initialized = 1;

    /* Single uniform block mode -- subtitle bitmaps contain one or
     * two lines of pre-rendered text without page structure.  PSM 6
     * treats the image as a uniform text block, handling both single
     * and multi-line subtitles correctly. */
    TessBaseAPISetVariable(ctx->tess, "tessedit_pageseg_mode", "6");

    /* Subtitle bitmaps are typically low-resolution. Setting a low DPI
     * prevents Tesseract from over-estimating resolution and improves
     * recognition of small text. */
    TessBaseAPISetVariable(ctx->tess, "user_defined_dpi", "72");

    /* Interword space handling depends on script direction.
     *
     * For CJK and most LTR scripts, preserve_interword_spaces=1 is
     * needed to prevent Tesseract from inserting spurious spaces
     * between characters (especially CJK ideographs).
     *
     * For RTL scripts (Arabic, Hebrew, etc.), preserve_interword_spaces=1
     * causes Tesseract to merge adjacent words by dropping the spaces
     * between them.  Setting it to 0 restores correct word boundaries.
     * This is a known Tesseract behavior where the interword space
     * detection logic interacts poorly with RTL text layout. */
    if (language && (av_strstart(language, "ara", NULL) ||
                     av_strstart(language, "fas", NULL) ||
                     av_strstart(language, "heb", NULL) ||
                     av_strstart(language, "urd", NULL) ||
                     av_strstart(language, "yid", NULL) ||
                     av_strstart(language, "syr", NULL) ||
                     av_strstart(language, "pus", NULL) ||
                     av_strstart(language, "snd", NULL) ||
                     av_strstart(language, "uig", NULL)))
        TessBaseAPISetVariable(ctx->tess, "preserve_interword_spaces", "0");
    else
        TessBaseAPISetVariable(ctx->tess, "preserve_interword_spaces", "1");

    return 0;
}

int ff_sub_ocr_pageseg(FFSubOCRContext *ctx,
                                            int mode)
{
    char buf[16];

    if (!ctx || !ctx->initialized)
        return AVERROR(EINVAL);
    if (mode < 0 || mode > 13)
        return AVERROR(EINVAL);

    snprintf(buf, sizeof(buf), "%d", mode);
    TessBaseAPISetVariable(ctx->tess, "tessedit_pageseg_mode", buf);
    return 0;
}

int ff_sub_ocr_recognize(FFSubOCRContext *ctx,
                                     const uint8_t *data, int bpp,
                                     int linesize, int w, int h,
                                     char **text, int *avg_confidence)
{
    char *result;
    int *confs;
    int sum, count;

    if (!ctx || !ctx->initialized)
        return AVERROR(EINVAL);
    if (!data || bpp != 1 || w <= 0 || h <= 0 || linesize < w)
        return AVERROR(EINVAL);
    if (!text)
        return AVERROR(EINVAL);

    *text = NULL;
    if (avg_confidence)
        *avg_confidence = -1;

    result = TessBaseAPIRect(ctx->tess, data, bpp, linesize, 0, 0, w, h);
    if (!result)
        return AVERROR(ENOMEM);

    *text = av_strdup(result);
    TessDeleteText(result);
    if (!*text)
        return AVERROR(ENOMEM);

    /* Fix common OCR misreads: pipe '|' is never valid in subtitle
     * text but is frequently confused with capital 'I'.  Replace '|'
     * with 'I' when it appears at a word boundary (start of line,
     * after space, or before space/apostrophe). */
    {
        char *p = *text;
        int len = strlen(p);
        for (int i = 0; i < len; i++) {
            if (p[i] != '|')
                continue;
            int at_start = (i == 0 || p[i - 1] == ' ' || p[i - 1] == '\n'
                            || p[i - 1] == '-');
            int at_end   = (i + 1 >= len || p[i + 1] == ' '
                            || p[i + 1] == '\'' || p[i + 1] == '\n'
                            || p[i + 1] == ',' || p[i + 1] == '.');
            if (at_start || at_end)
                p[i] = 'I';
        }
    }

    if (avg_confidence) {
        confs = TessBaseAPIAllWordConfidences(ctx->tess);
        if (confs) {
            sum   = 0;
            count = 0;
            for (int i = 0; confs[i] != -1; i++) {
                sum += confs[i];
                count++;
            }
            TessDeleteIntArray(confs);
            *avg_confidence = count > 0 ? sum / count : -1;
        }
    }

    return 0;
}

#else /* !CONFIG_LIBTESSERACT */

FFSubOCRContext *ff_sub_ocr_alloc(void)
{
    return NULL;
}

void ff_sub_ocr_free(FFSubOCRContext **ctx)
{
}

int ff_sub_ocr_init(FFSubOCRContext *ctx,
                                const char *language,
                                const char *datapath)
{
    return AVERROR(ENOSYS);
}

int ff_sub_ocr_pageseg(FFSubOCRContext *ctx,
                                            int mode)
{
    return AVERROR(ENOSYS);
}

int ff_sub_ocr_recognize(FFSubOCRContext *ctx,
                                     const uint8_t *data, int bpp,
                                     int linesize, int w, int h,
                                     char **text, int *avg_confidence)
{
    return AVERROR(ENOSYS);
}

#endif /* CONFIG_LIBTESSERACT */
