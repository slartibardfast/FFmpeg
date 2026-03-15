/*
 * Text-to-bitmap subtitle conversion for ffmpeg.
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

#ifndef FFTOOLS_FFMPEG_ENC_SUB_H
#define FFTOOLS_FFMPEG_ENC_SUB_H

struct OutputStream;
struct OutputFile;
struct Scheduler;
struct AVPacket;
struct AVSubtitle;

/**
 * Opaque context for text-to-bitmap subtitle conversion.
 */
typedef struct SubtitleEncContext SubtitleEncContext;

/**
 * Allocate a subtitle encoding context.
 *
 * @param sch      scheduler instance (for sending encoded packets)
 * @param sch_idx  encoder index within the scheduler
 * @return context, or NULL on allocation failure
 */
SubtitleEncContext *enc_sub_alloc(struct Scheduler *sch, unsigned sch_idx);

/**
 * Free a subtitle encoding context.
 *
 * @param[in,out] pctx pointer to context pointer; set to NULL on return
 */
void enc_sub_free(SubtitleEncContext **pctx);

/**
 * Process a subtitle event for text-to-bitmap conversion.
 *
 * Handles rendering, quantization, animation detection, and
 * coalescing of overlapping text events for PGS encoding.
 * If the encoder does not require bitmap subtitles, the event
 * is left unmodified and the caller encodes it normally.
 *
 * @param ctx  subtitle encoding context
 * @param of   output file
 * @param ost  output stream
 * @param sub  subtitle event (may be modified in place)
 * @param pkt  packet for encoded output
 * @return 0 on success, negative AVERROR on failure,
 *         1 if the event was fully handled (caller should not encode it)
 */
int enc_sub_process(SubtitleEncContext *ctx,
                    struct OutputFile *of, struct OutputStream *ost,
                    struct AVSubtitle *sub, struct AVPacket *pkt);

/**
 * Flush any pending coalesced subtitle events.
 *
 * Must be called at end of stream to emit buffered events.
 *
 * @param ctx  subtitle encoding context
 * @param of   output file
 * @param ost  output stream
 * @param pkt  packet for encoded output
 * @return 0 on success, negative AVERROR on failure
 */
int enc_sub_flush(SubtitleEncContext *ctx,
                  struct OutputFile *of, struct OutputStream *ost,
                  struct AVPacket *pkt);

#endif /* FFTOOLS_FFMPEG_ENC_SUB_H */
