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

#ifndef AVFILTER_SUBTITLE_OCR_H
#define AVFILTER_SUBTITLE_OCR_H

#include <stdint.h>

/**
 * @defgroup lavfi_subtitle_ocr Bitmap subtitle OCR
 * @ingroup lavfi
 *
 * Recognize text in grayscale bitmap subtitle images using Tesseract OCR.
 *
 * Requires FFmpeg to be configured with --enable-libtesseract. When
 * Tesseract is not available, avfilter_subtitle_ocr_alloc() returns NULL
 * and all other functions return AVERROR(ENOSYS).
 *
 * @{
 */

/**
 * Opaque subtitle OCR context.
 */
typedef struct AVSubtitleOCRContext AVSubtitleOCRContext;

/**
 * Allocate a subtitle OCR context.
 *
 * @return OCR context, or NULL on failure or if libtesseract is
 *         not available
 */
AVSubtitleOCRContext *avfilter_subtitle_ocr_alloc(void);

/**
 * Free a subtitle OCR context.
 *
 * @param[in,out] ctx pointer to context pointer; set to NULL on return
 */
void avfilter_subtitle_ocr_freep(AVSubtitleOCRContext **ctx);

/**
 * Initialize the OCR engine with a language and optional data path.
 *
 * @param[in] ctx      OCR context
 * @param[in] language Tesseract language code (e.g. "eng", "fra",
 *                     "eng+fra" for multi-language)
 * @param[in] datapath path to Tesseract training data directory,
 *                     or NULL to use the default
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_ocr_init(AVSubtitleOCRContext *ctx,
                                const char *language,
                                const char *datapath);

/**
 * Set the Tesseract page segmentation mode.
 *
 * Must be called after avfilter_subtitle_ocr_init().  The default is 6
 * (PSM_SINGLE_BLOCK), which treats the image as a single block of text
 * without page structure -- the right choice for most subtitle bitmaps.
 *
 * Other potentially useful modes for subtitles:
 *   7  Single text line
 *   8  Single word
 *  13  Raw line (treat image as a single text line, no OSD or hacks)
 *
 * @param[in] ctx  OCR context (must be initialized)
 * @param[in] mode page segmentation mode (0-13, see Tesseract docs)
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_ocr_set_pageseg_mode(AVSubtitleOCRContext *ctx,
                                            int mode);

/**
 * Recognize text in a grayscale bitmap.
 *
 * The input must be an 8-bit grayscale image (1 byte per pixel).
 * Dark text on a light background produces the best results.
 *
 * The returned text string is allocated with av_malloc() and must
 * be freed by the caller with av_free().
 *
 * @param[in]  ctx            OCR context (must be initialized)
 * @param[in]  data           grayscale pixel data
 * @param[in]  bpp            bytes per pixel (must be 1)
 * @param[in]  linesize       row stride in bytes
 * @param[in]  w              image width in pixels
 * @param[in]  h              image height in pixels
 * @param[out] text           recognized text (caller frees with av_free())
 * @param[out] avg_confidence average word confidence (0-100), or -1
 *                            if unavailable; may be NULL
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_ocr_recognize(AVSubtitleOCRContext *ctx,
                                     const uint8_t *data, int bpp,
                                     int linesize, int w, int h,
                                     char **text, int *avg_confidence);

/**
 * @}
 */

#endif /* AVFILTER_SUBTITLE_OCR_H */
