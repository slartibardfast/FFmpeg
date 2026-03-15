/*
 * PGS animation timing and rendering pipeline test.
 *
 * End-to-end test: render ASS event with \fad() at multiple timepoints,
 * classify changes, encode with palette animation, and verify Display
 * Set types and palette_version progression.
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

/* Include animation utilities for classify/scale functions */
#include "fftools/ffmpeg_sub_util.h"

#define PGS_PCS 0x16
#define PGS_PDS 0x14
#define PGS_ODS 0x15

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
    "&H00000000,&H80000000,0,0,0,0,100,100,0,0,1,3,1,2,10,10,40,1\n";

/* ASS dialogue with 500ms fade-in, 500ms fade-out, 2s total.
 * Format: ReadOrder, Layer, Style, Name, MarginL,
 *         MarginR, MarginV, Effect, Text
 * (ass_process_chunk format -- no Start/End,
 *  those come from init_event params) */
static const char *fade_text =
    "0,0,Default,,0,0,0,,{\\fad(500,500)}Fade Test";

int main(void)
{
    FFSubRenderContext *render = NULL;
    const AVCodec *codec;
    AVCodecContext *enc = NULL;
    uint8_t *rgba0 = NULL, *peak_rgba = NULL;
    uint8_t *ref_indices = NULL;
    int ret = 1;

    const int64_t start_ms = 0, dur_ms = 2000;
    const int frame_ms = 42; /* ~24fps */

    /* Pass 1 state */
    int ls0, x0, y0, w0, h0;
    int64_t peak_alpha, alpha_t0, alpha_mid, alpha_end;
    int64_t peak_time = start_ms;
    enum SubtitleChangeType worst = SUB_CHANGE_NONE;
    int n_changes = 0;

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

    /* --- Pass 1: Scan animation, classify, find peak --- */

    ff_sub_render_event(render, fade_text,
                                         start_ms, dur_ms);

    ff_sub_render_sample(render, start_ms,
                                     &rgba0, &ls0,
                                     &x0, &y0, &w0, &h0, NULL);
    if (!rgba0) {
        fprintf(stderr, "FAIL: First frame empty\n");
        goto end;
    }

    alpha_t0   = rgba_alpha_sum(rgba0, w0, h0, ls0);
    peak_alpha = alpha_t0;

    for (int64_t t = start_ms + frame_ms; t <= start_ms + dur_ms;
         t += frame_ms) {
        uint8_t *rgba = NULL;
        int ls, sx, sy, sw, sh, dc;
        enum SubtitleChangeType change;
        int64_t alpha;

        ff_sub_render_sample(render, t, &rgba, &ls,
                                         &sx, &sy, &sw, &sh, &dc);

        change = classify_subtitle_change(
            rgba0, x0, y0, w0, h0, ls0,
            rgba,  sx, sy, sw, sh, ls);

        if (change > worst)
            worst = change;
        if (change != SUB_CHANGE_NONE)
            n_changes++;

        if (rgba) {
            alpha = rgba_alpha_sum(rgba, sw, sh, ls);
            if (alpha > peak_alpha) {
                peak_alpha = alpha;
                peak_time  = t;
            }
        }

        av_free(rgba0);
        rgba0 = rgba;
        ls0 = ls; x0 = sx; y0 = sy; w0 = sw; h0 = sh;
    }
    av_freep(&rgba0);

    /* Sample at midpoint and end for alpha pattern verification */
    ff_sub_render_event(render, fade_text,
                                         start_ms, dur_ms);
    {
        uint8_t *rgba = NULL;
        int ls, sx, sy, sw, sh;

        ff_sub_render_sample(render, start_ms,
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh, NULL);
        alpha_t0 = rgba ? rgba_alpha_sum(rgba, sw, sh, ls) : 0;
        av_free(rgba);

        ff_sub_render_sample(render, 1000,
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh, NULL);
        alpha_mid = rgba ? rgba_alpha_sum(rgba, sw, sh, ls) : 0;
        av_free(rgba);

        ff_sub_render_sample(render, dur_ms,
                                         &rgba, &ls,
                                         &sx, &sy, &sw, &sh, NULL);
        alpha_end = rgba ? rgba_alpha_sum(rgba, sw, sh, ls) : 0;
        av_free(rgba);
    }

    printf("Pass 1 results:\n");
    printf("  worst_change = %d (ALPHA=2)\n", worst);
    printf("  n_changes    = %d\n", n_changes);
    printf("  peak_time    = %ld ms\n", (long)peak_time);
    printf("  alpha_t0     = %ld\n", (long)alpha_t0);
    printf("  alpha_mid    = %ld\n", (long)alpha_mid);
    printf("  alpha_end    = %ld\n", (long)alpha_end);

    /* Verify: classification should be ALPHA (fade only) */
    if (worst != SUB_CHANGE_ALPHA) {
        fprintf(stderr, "FAIL: Expected ALPHA(2), got %d\n", worst);
        goto end;
    }
    printf("PASS: Fade classified as SUB_CHANGE_ALPHA\n");

    /* Verify: multiple animated frames detected */
    if (n_changes < 5) {
        fprintf(stderr, "FAIL: Expected >=5 change frames, got %d\n",
                n_changes);
        goto end;
    }
    printf("PASS: %d animated frames\n", n_changes);

    /* Verify: peak alpha in steady-state region */
    if (peak_time < 400 || peak_time > 1600) {
        fprintf(stderr, "FAIL: Peak at %ld ms (expected 400-1600)\n",
                (long)peak_time);
        goto end;
    }
    printf("PASS: Peak alpha at %ld ms\n", (long)peak_time);

    /* Verify: alpha follows fade pattern (low -> high -> low) */
    if (alpha_mid <= alpha_t0 || alpha_mid <= alpha_end) {
        fprintf(stderr, "FAIL: Alpha pattern incorrect "
                "(t0=%ld mid=%ld end=%ld)\n",
                (long)alpha_t0, (long)alpha_mid, (long)alpha_end);
        goto end;
    }
    printf("PASS: Alpha pattern correct (low -> high -> low)\n");

    /* --- Pass 2: Encode animation sequence ------------ */

    ff_sub_render_event(render, fade_text,
                                         start_ms, dur_ms);
    ff_sub_render_sample(render, peak_time,
                                     &peak_rgba, &ls0,
                                     &x0, &y0, &w0, &h0, NULL);
    if (!peak_rgba) {
        fprintf(stderr, "FAIL: Peak render empty\n");
        goto end;
    }

    {
        AVQuantizeContext *qctx;
        uint32_t ref_pal[256] = {0};
        int nb_colors, nb_px = w0 * h0;
        uint8_t out[1024 * 1024];
        int ds_count = 0, pal_updates = 0;
        int last_pal_ver = -1;
        int pal_ver_ok = 1;

        qctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
        if (!qctx) goto end;

        nb_colors = av_quantize_generate_palette(qctx, peak_rgba, nb_px,
                                                  ref_pal, 10);
        ref_indices = av_malloc(nb_px);
        if (!ref_indices) {
            av_quantize_freep(&qctx);
            goto end;
        }
        av_quantize_apply(qctx, peak_rgba, ref_indices, nb_px);
        av_quantize_freep(&qctx);
        av_freep(&peak_rgba);

        /* Re-init for encoding pass */
        ff_sub_render_event(render, fade_text,
                                             start_ms, dur_ms);

        int prev_pct = -1;
        for (int64_t t = start_ms; t <= start_ms + dur_ms;
             t += frame_ms) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh, dc;
            int64_t alpha;
            int pct, size;
            uint32_t scaled[256];
            AVSubtitle sub = {0};
            AVSubtitleRect rect = {0};
            AVSubtitleRect *rects[1] = { &rect };

            ff_sub_render_sample(render, t, &rgba, &ls,
                                             &sx, &sy, &sw, &sh, &dc);
            if (!rgba)
                continue;

            alpha = rgba_alpha_sum(rgba, sw, sh, ls);
            av_free(rgba);

            pct = peak_alpha ? (int)(100 * alpha / peak_alpha) : 0;
            if (pct == prev_pct)
                continue;
            prev_pct = pct;

            scale_palette_alpha(ref_pal, scaled, nb_colors, pct);

            sub.num_rects           = 1;
            sub.rects               = rects;
            sub.start_display_time  = 0;
            sub.end_display_time    = dur_ms;
            rect.type               = SUBTITLE_BITMAP;
            rect.x = x0; rect.y = y0;
            rect.w = w0; rect.h = h0;
            rect.nb_colors          = nb_colors;
            rect.data[0]            = ref_indices;
            rect.linesize[0]        = w0;
            rect.data[1]            = (uint8_t *)scaled;
            rect.linesize[1]        = nb_colors * 4;

            size = avcodec_encode_subtitle(enc, out, sizeof(out), &sub);
            if (size <= 0)
                continue;

            {
                const uint8_t *pcs = find_segment(out, size, PGS_PCS);
                const uint8_t *pds = find_segment(out, size, PGS_PDS);

                if (!pcs)
                    continue;

                ds_count++;

                if (pcs[10] == 0x80) {
                    /* Epoch Start */
                } else if (pcs[11] == 0x80) {
                    pal_updates++;
                }

                /* Verify palette_version increments */
                if (pds) {
                    int ver = pds[4];
                    if (last_pal_ver >= 0 && ver != last_pal_ver + 1)
                        pal_ver_ok = 0;
                    last_pal_ver = ver;
                }
            }
        }

        printf("\nPass 2 results:\n");
        printf("  Display Sets:     %d\n", ds_count);
        printf("  Palette updates:  %d\n", pal_updates);
        printf("  palette_version:  %s\n",
               pal_ver_ok ? "monotonic" : "NOT monotonic");

        /* Verify: first DS is Epoch Start, rest are palette updates */
        if (ds_count < 4) {
            fprintf(stderr, "FAIL: Expected >=4 Display Sets, got %d\n",
                    ds_count);
            goto end;
        }
        printf("PASS: %d Display Sets encoded\n", ds_count);

        if (pal_updates < 3) {
            fprintf(stderr, "FAIL: Expected >=3 palette updates, got %d\n",
                    pal_updates);
            goto end;
        }
        printf("PASS: %d palette update Display Sets\n", pal_updates);

        if (!pal_ver_ok) {
            fprintf(stderr, "FAIL: palette_version not monotonic\n");
            goto end;
        }
        printf("PASS: palette_version increments correctly\n");
    }

    printf("\nAll animation timing tests passed.\n");
    ret = 0;

end:
    av_free(rgba0);
    av_free(peak_rgba);
    av_free(ref_indices);
    ff_sub_render_free(&render);
    avcodec_free_context(&enc);
    return ret;
}
