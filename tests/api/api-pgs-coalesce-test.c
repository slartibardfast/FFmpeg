/*
 * PGS subtitle event coalescing test.
 *
 * Validates that multiple overlapping ASS events render as a single
 * composite when loaded via init_event() + add_event().
 *
 * Requires libass (for subtitle_render API).
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

#include <stdio.h>
#include <string.h>

#include "libavcodec/avcodec.h"
#include "libavfilter/subtitle_render.h"
#include "libavutil/quantize.h"
#include "libavutil/mem.h"

#define PGS_PCS 0x16

static const uint8_t *find_segment(const uint8_t *data, int size,
                                    uint8_t type)
{
    const uint8_t *p = data;
    while (p < data + size - 3) {
        uint8_t seg_type = p[0];
        int seg_len = (p[1] << 8) | p[2];
        if (seg_type == type)
            return p;
        p += 3 + seg_len;
    }
    return NULL;
}

static const char *ass_header =
    "[Script Info]\n"
    "ScriptType: v4.00+\n"
    "PlayResX: 1920\n"
    "PlayResY: 1080\n"
    "\n"
    "[V4+ Styles]\n"
    "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
    "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, "
    "ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, "
    "Alignment, MarginL, MarginR, MarginV, Encoding\n"
    "Style: Default,sans-serif,72,&H00FFFFFF,&H000000FF,"
    "&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,3,1,2,10,10,40,1\n"
    "Style: Top,sans-serif,72,&H0000FFFF,&H000000FF,"
    "&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,3,1,8,10,10,40,1\n";

/* Two events: Default at bottom (alignment 2), Top at top (alignment 8).
 * Format: ReadOrder, Layer, Style, Name, MarginL,
 *         MarginR, MarginV, Effect, Text
 * (ass_process_chunk format -- no Start/End,
 *  those come from init_event params).
 * ReadOrder must differ per event -- same ReadOrder causes overwrite. */
static const char *text_bottom =
    "0,0,Default,,0,0,0,,Bottom Line";
static const char *text_top =
    "1,0,Top,,0,0,0,,Top Line";

int main(void)
{
    FFSubRenderContext *render = NULL;
    const AVCodec *codec;
    AVCodecContext *enc = NULL;
    int ret = 1;

    /* --- Setup ----------------------------------------- */

    render = ff_sub_render_alloc(1920, 1080);
    if (!render) {
        fprintf(stderr, "Render alloc failed (libass not available?)\n");
        return 77; /* FATE skip */
    }
    ff_sub_render_header(render, ass_header);

    codec = avcodec_find_encoder(AV_CODEC_ID_HDMV_PGS_SUBTITLE);
    if (!codec) {
        fprintf(stderr, "PGS encoder not found\n");
        goto end;
    }
    enc = avcodec_alloc_context3(codec);
    if (!enc)
        goto end;
    enc->width     = 1920;
    enc->height    = 1080;
    enc->time_base = (AVRational){ 1, 90000 };
    if (avcodec_open2(enc, codec, NULL) < 0) {
        fprintf(stderr, "Failed to open encoder\n");
        goto end;
    }

    /* --- Test 1: Single event bounding box ------------ */
    {
        uint8_t *rgba = NULL;
        int ls, x, y, w, h;

        ff_sub_render_event(render, text_bottom,
                                             0, 3000);
        ff_sub_render_sample(render, 0, &rgba, &ls,
                                         &x, &y, &w, &h, NULL);
        if (!rgba) {
            fprintf(stderr, "FAIL: Single event render empty\n");
            goto end;
        }

        printf("Single event (bottom): y=%d h=%d (y+h=%d)\n",
               y, h, y + h);

        /* Bottom text should be in lower portion of canvas */
        if (y < 500) {
            fprintf(stderr, "FAIL: Bottom text at y=%d (expected >500)\n", y);
            av_free(rgba);
            goto end;
        }
        printf("PASS: Bottom text positioned correctly\n");
        av_free(rgba);
    }

    /* --- Test 2: Composite bounding box (two events) -- */
    {
        uint8_t *rgba = NULL;
        int ls, x, y, w, h;
        int single_y, single_h;

        /* First, get single-event dimensions for comparison */
        ff_sub_render_event(render, text_bottom,
                                             0, 3000);
        {
            uint8_t *rgba1 = NULL;
            int ls1, x1, y1, w1, h1;
            ff_sub_render_sample(render, 0, &rgba1, &ls1,
                                             &x1, &y1, &w1, &h1, NULL);
            single_y = y1;
            single_h = h1;
            av_free(rgba1);
        }

        /* Now load BOTH events */
        ff_sub_render_event(render, text_bottom,
                                             0, 3000);
        ff_sub_render_add(render, text_top,
                                            0, 3000);

        ff_sub_render_sample(render, 0, &rgba, &ls,
                                         &x, &y, &w, &h, NULL);
        if (!rgba) {
            fprintf(stderr, "FAIL: Composite render empty\n");
            goto end;
        }

        printf("Composite (both):     y=%d h=%d (y+h=%d)\n",
               y, h, y + h);

        /* Composite should span much more vertical space than single */
        if (h <= single_h * 2) {
            fprintf(stderr, "FAIL: Composite height %d not significantly "
                    "larger than single %d\n", h, single_h);
            av_free(rgba);
            goto end;
        }
        printf("PASS: Composite bounding box (%d px) spans both events "
               "(single was %d px)\n", h, single_h);

        /* Composite y should be higher (smaller) than bottom-only */
        if (y >= single_y) {
            fprintf(stderr, "FAIL: Composite y=%d should be above "
                    "single y=%d\n", y, single_y);
            av_free(rgba);
            goto end;
        }
        printf("PASS: Composite starts higher (y=%d vs single y=%d)\n",
               y, single_y);

        /* --- Test 3: Encode composite as single DS ---- */
        {
            AVQuantizeContext *qctx;
            uint32_t palette[256] = {0};
            uint8_t *indices;
            int nb_colors, nb_px = w * h;
            int size;
            uint8_t out[1024 * 1024];
            AVSubtitle sub = {0};
            AVSubtitleRect rect = {0};
            AVSubtitleRect *rects[1] = { &rect };

            qctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
            if (!qctx) {
                av_free(rgba);
                goto end;
            }

            nb_colors = av_quantize_generate_palette(qctx, rgba, nb_px,
                                                      palette, 10);
            indices = av_malloc(nb_px);
            if (!indices) {
                av_quantize_freep(&qctx);
                av_free(rgba);
                goto end;
            }
            av_quantize_apply(qctx, rgba, indices, nb_px);
            av_quantize_freep(&qctx);
            av_free(rgba);

            sub.num_rects           = 1;
            sub.rects               = rects;
            sub.start_display_time  = 0;
            sub.end_display_time    = 3000;
            rect.type               = SUBTITLE_BITMAP;
            rect.x = x; rect.y = y;
            rect.w = w; rect.h = h;
            rect.nb_colors          = nb_colors;
            rect.data[0]            = indices;
            rect.linesize[0]        = w;
            rect.data[1]            = (uint8_t *)palette;
            rect.linesize[1]        = nb_colors * 4;

            size = avcodec_encode_subtitle(enc, out, sizeof(out), &sub);
            av_free(indices);

            if (size <= 0) {
                fprintf(stderr, "FAIL: Composite encode failed (%d)\n",
                        size);
                goto end;
            }

            {
                const uint8_t *pcs = find_segment(out, size, PGS_PCS);
                if (!pcs) {
                    fprintf(stderr, "FAIL: No PCS in encoded output\n");
                    goto end;
                }
                if (pcs[10] != 0x80) {
                    fprintf(stderr, "FAIL: Expected Epoch Start (0x80), "
                            "got 0x%02x\n", pcs[10]);
                    goto end;
                }
                printf("PASS: Composite encodes as single Epoch Start DS "
                       "(%d bytes)\n", size);
            }
        }
    }

    printf("\nAll coalescing tests passed.\n");
    ret = 0;

end:
    ff_sub_render_free(&render);
    avcodec_free_context(&enc);
    return ret;
}
