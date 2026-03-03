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

#ifndef AVFILTER_SUBTITLE_RENDER_H
#define AVFILTER_SUBTITLE_RENDER_H

#include <stdint.h>

/**
 * @defgroup lavfi_subtitle_render Text subtitle rendering
 * @ingroup lavfi
 *
 * Render text (ASS/SSA) subtitle events to cropped RGBA bitmaps
 * suitable for quantization and encoding as bitmap subtitles.
 *
 * Requires FFmpeg to be configured with --enable-libass. When libass
 * is not available, avfilter_subtitle_render_alloc() returns NULL
 * and all other functions return AVERROR(ENOSYS).
 *
 * @{
 */

/**
 * Opaque subtitle rendering context.
 */
typedef struct AVSubtitleRenderContext AVSubtitleRenderContext;

/**
 * Allocate a subtitle rendering context.
 *
 * @param[in] canvas_w  output canvas width in pixels (e.g. 1920)
 * @param[in] canvas_h  output canvas height in pixels (e.g. 1080)
 * @return rendering context, or NULL on failure or if libass is
 *         not available
 */
AVSubtitleRenderContext *avfilter_subtitle_render_alloc(int canvas_w,
                                                        int canvas_h);

/**
 * Free a subtitle rendering context.
 *
 * @param[in,out] ctx pointer to context pointer; set to NULL on return
 */
void avfilter_subtitle_render_freep(AVSubtitleRenderContext **ctx);

/**
 * Set ASS script header (styles, script info).
 *
 * This should be called once after allocation with the decoder's
 * subtitle_header to configure fonts, styles, and layout.
 *
 * @param[in] ctx    rendering context
 * @param[in] header ASS header string
 *                   (e.g. from AVCodecContext.subtitle_header)
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_set_header(AVSubtitleRenderContext *ctx,
                                         const char *header);

/**
 * Add an embedded font for rendering.
 *
 * Call this for each font attachment (e.g. from MKV container) before
 * rendering. Fonts are referenced by name in ASS style definitions.
 *
 * @param[in] ctx   rendering context
 * @param[in] name  font family name (from attachment metadata)
 * @param[in] data  raw font file data (TTF/OTF)
 * @param[in] size  font data size in bytes
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_add_font(AVSubtitleRenderContext *ctx,
                                       const char *name,
                                       const uint8_t *data, int size);

/**
 * Render one subtitle event to a cropped RGBA bitmap.
 *
 * The output is an RGBA bitmap cropped to the bounding box of the
 * rendered text, with position coordinates relative to the canvas.
 * The caller must free the returned RGBA buffer with av_free().
 *
 * Convenience wrapper: calls init_event() then sample() at start_ms.
 *
 * @param[in]  ctx         rendering context
 * @param[in]  text        ASS dialogue line
 * @param[in]  start_ms    event start time in milliseconds
 * @param[in]  duration_ms event duration in milliseconds
 * @param[out] rgba        allocated RGBA buffer (caller frees with av_free())
 * @param[out] linesize    RGBA buffer row stride in bytes
 * @param[out] x           crop X position on canvas
 * @param[out] y           crop Y position on canvas
 * @param[out] w           crop width in pixels
 * @param[out] h           crop height in pixels
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_frame(AVSubtitleRenderContext *ctx,
                                    const char *text,
                                    int64_t start_ms, int64_t duration_ms,
                                    uint8_t **rgba, int *linesize,
                                    int *x, int *y, int *w, int *h);

/**
 * Set up a subtitle event for multi-timepoint rendering.
 *
 * Clears any previous events and adds the given text as a new event.
 * After calling this, use avfilter_subtitle_render_sample() to render
 * at one or more timestamps within the event's duration.
 *
 * @param[in] ctx         rendering context
 * @param[in] text        ASS dialogue line
 * @param[in] start_ms    event start time in milliseconds
 * @param[in] duration_ms event duration in milliseconds
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_init_event(AVSubtitleRenderContext *ctx,
                                         const char *text,
                                         int64_t start_ms,
                                         int64_t duration_ms);

/**
 * Add an additional subtitle event without clearing previous events.
 *
 * Unlike init_event(), this does not flush the track first.
 * Use this to load multiple overlapping events before sampling
 * so that libass composites them together.
 *
 * Must be called after avfilter_subtitle_render_init_event().
 *
 * @param[in] ctx         rendering context
 * @param[in] text        ASS dialogue line
 * @param[in] start_ms    event start time in milliseconds
 * @param[in] duration_ms event duration in milliseconds
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_add_event(AVSubtitleRenderContext *ctx,
                                        const char *text,
                                        int64_t start_ms,
                                        int64_t duration_ms);

/**
 * Render a snapshot of the current event at a specific time.
 *
 * Must be called after avfilter_subtitle_render_init_event(). Can be
 * called multiple times at different timestamps to sample animation.
 * The caller must free the returned RGBA buffer with av_free().
 *
 * @param[in]  ctx            rendering context
 * @param[in]  render_time_ms timestamp to render at (milliseconds)
 * @param[out] rgba           allocated RGBA buffer
 *                            (caller frees with av_free())
 * @param[out] linesize       RGBA buffer row stride in bytes
 * @param[out] x              crop X position on canvas
 * @param[out] y              crop Y position on canvas
 * @param[out] w              crop width in pixels
 * @param[out] h              crop height in pixels
 * @param[out] detect_change  0=identical to previous sample, 1=changed,
 *                            2=unknown (first call after init_event)
 * @return 0 on success, negative AVERROR on failure
 */
int avfilter_subtitle_render_sample(AVSubtitleRenderContext *ctx,
                                     int64_t render_time_ms,
                                     uint8_t **rgba, int *linesize,
                                     int *x, int *y, int *w, int *h,
                                     int *detect_change);

/**
 * @}
 */

#endif /* AVFILTER_SUBTITLE_RENDER_H */
