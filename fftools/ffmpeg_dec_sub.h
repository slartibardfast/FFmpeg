/*
 * Bitmap-to-text subtitle conversion for ffmpeg.
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

#ifndef FFTOOLS_FFMPEG_DEC_SUB_H
#define FFTOOLS_FFMPEG_DEC_SUB_H

struct OutputStream;
struct OutputFile;
struct Scheduler;
struct AVPacket;
struct AVSubtitle;

/**
 * Opaque context for bitmap-to-text subtitle conversion.
 */
typedef struct SubtitleDecContext SubtitleDecContext;

/**
 * Allocate a subtitle decoding/OCR context.
 *
 * @param sch      scheduler instance (for sending encoded packets)
 * @param sch_idx  encoder index within the scheduler
 * @return context, or NULL on allocation failure
 */
SubtitleDecContext *dec_sub_alloc(struct Scheduler *sch, unsigned sch_idx);

/**
 * Configure OCR options. Call before first dec_sub_process().
 *
 * @param lang          Tesseract language code (NULL = auto/eng)
 * @param datapath      tessdata directory path (NULL = default)
 * @param pageseg_mode  page segmentation mode (-1 = default with fallback)
 * @param min_duration  minimum subtitle duration in ms (0 = default 200)
 */
void dec_sub_set_options(SubtitleDecContext *ctx,
                         const char *lang, const char *datapath,
                         int pageseg_mode, int min_duration);

/**
 * Free a subtitle decoding/OCR context.
 *
 * @param[in,out] pctx pointer to context pointer; set to NULL on return
 */
void dec_sub_free(SubtitleDecContext **pctx);

/**
 * Process a subtitle event for bitmap-to-text conversion.
 *
 * Handles OCR of bitmap subtitle rects via Tesseract, with
 * deduplication of consecutive identical bitmaps (e.g. PGS fade
 * sequences) to avoid redundant OCR calls.
 *
 * @param ctx  subtitle decoding context
 * @param of   output file
 * @param ost  output stream
 * @param sub  subtitle event (may be modified in place)
 * @param pkt  packet for encoded output
 * @return 0 on success, negative AVERROR on failure,
 *         1 if the event was fully handled (caller should not encode it)
 */
int dec_sub_process(SubtitleDecContext *ctx,
                    struct OutputFile *of, struct OutputStream *ost,
                    struct AVSubtitle *sub, struct AVPacket *pkt);

/**
 * Flush any pending deduplicated OCR subtitle events.
 *
 * Must be called at end of stream to emit buffered events.
 *
 * @param ctx      subtitle decoding context
 * @param of       output file
 * @param ost      output stream
 * @param pkt      packet for encoded output
 * @param next_pts PTS of the next event (AV_TIME_BASE units),
 *                 or AV_NOPTS_VALUE at end of stream
 * @return 0 on success, negative AVERROR on failure
 */
int dec_sub_flush(SubtitleDecContext *ctx,
                  struct OutputFile *of, struct OutputStream *ost,
                  struct AVPacket *pkt, int64_t next_pts);

#endif /* FFTOOLS_FFMPEG_DEC_SUB_H */
