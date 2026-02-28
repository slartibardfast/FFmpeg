/*
 * HDMV Presentation Graphic Stream subtitle encoder
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

/**
 * @file
 * HDMV Presentation Graphic Stream subtitle encoder
 */

#include <string.h>

#include "avcodec.h"
#include "bytestream.h"
#include "codec_internal.h"
#include "libavutil/colorspace.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#define PGS_MAX_OBJECT_REFS 2

#define PGS_EPOCH_START  0x80
#define PGS_ACQUISITION  0x40
#define PGS_NORMAL       0x00

typedef struct PGSSubEncContext {
    AVClass *class;
    int composition_number;
    int frame_rate;

    /* epoch state */
    int epoch_active;
    int palette_version;

    /* cached state for composition state detection */
    int prev_num_rects;
    int prev_obj_w[PGS_MAX_OBJECT_REFS];
    int prev_obj_h[PGS_MAX_OBJECT_REFS];
    int prev_obj_x[PGS_MAX_OBJECT_REFS];
    int prev_obj_y[PGS_MAX_OBJECT_REFS];
    uint32_t prev_palette[AVPALETTE_COUNT];
    int prev_nb_colors;
} PGSSubEncContext;

/**
 * Map a frame rate rational to the HDMV frame rate code.
 * Returns 0x10 (23.976) as default if no match.
 */
static int fps_to_pgs_code(AVRational rate)
{
    if (!rate.num || !rate.den)
        return 0x10;
    /* compare in millihertz to avoid floating point */
    switch ((int)(1000LL * rate.num / rate.den)) {
    case 23976: return 0x10;
    case 24000: return 0x20;
    case 25000: return 0x30;
    case 29970: return 0x40;
    case 50000: return 0x60;
    case 59940: return 0x70;
    default:    return 0x10;
    }
}

static av_cold int pgssub_init(AVCodecContext *avctx)
{
    PGSSubEncContext *s = avctx->priv_data;

    if (!s->frame_rate)
        s->frame_rate = fps_to_pgs_code(avctx->framerate);

    return 0;
}

static int pgs_encode_rle(uint8_t **pq, int buf_size,
                          const uint8_t *bitmap, int linesize,
                          int w, int h)
{
    uint8_t *q, *line_begin;
    int x, y, len, x1, color;

    q = *pq;

    for (y = 0; y < h; y++) {
        if (buf_size < w * 4 + 2)
            return AVERROR_BUFFER_TOO_SMALL;
        line_begin = q;

        x = 0;
        while (x < w) {
            x1 = x;
            color = bitmap[x1++];
            while (x1 < w && bitmap[x1] == color)
                x1++;
            len = x1 - x;

            if (color == 0) {
                while (len > 0) {
                    int run = FFMIN(len, 16383);
                    *q++ = 0x00;
                    if (run < 64) {
                        *q++ = run & 0x3f;
                    } else {
                        *q++ = 0x40 | ((run >> 8) & 0x3f);
                        *q++ = run & 0xff;
                    }
                    len -= run;
                }
            } else {
                while (len > 0) {
                    if (len < 3) {
                        int j;
                        for (j = 0; j < len; j++)
                            *q++ = color;
                        len = 0;
                    } else {
                        int run = FFMIN(len, 16383);
                        *q++ = 0x00;
                        if (run < 64) {
                            *q++ = 0x80 | (run & 0x3f);
                        } else {
                            *q++ = 0xc0 | ((run >> 8) & 0x3f);
                            *q++ = run & 0xff;
                        }
                        *q++ = color;
                        len -= run;
                    }
                }
            }
            x = x1;
        }

        *q++ = 0x00;
        *q++ = 0x00;

        buf_size -= q - line_begin;
        bitmap += linesize;
    }

    len = q - *pq;
    *pq = q;
    return len;
}

/**
 * Determine the composition state for this Display Set by comparing
 * the current subtitle against cached state from the previous call.
 */
static int pgs_determine_state(PGSSubEncContext *s, const AVSubtitle *h,
                                int *palette_update)
{
    int i;

    *palette_update = 0;

    if (!h->num_rects) {
        if (s->epoch_active)
            s->epoch_active = 0;
        return PGS_NORMAL;
    }

    if (!s->epoch_active)
        return PGS_EPOCH_START;

    /* object count or dimensions changed -> new epoch */
    if (h->num_rects != s->prev_num_rects)
        return PGS_EPOCH_START;
    for (i = 0; i < h->num_rects; i++) {
        if (h->rects[i]->w != s->prev_obj_w[i] ||
            h->rects[i]->h != s->prev_obj_h[i])
            return PGS_EPOCH_START;
    }

    /* same dimensions -- check palette */
    {
        const uint32_t *pal = (const uint32_t *)h->rects[0]->data[1];
        int nc = FFMIN(h->rects[0]->nb_colors, AVPALETTE_COUNT);
        if (nc != s->prev_nb_colors ||
            memcmp(pal, s->prev_palette, nc * sizeof(uint32_t))) {
            *palette_update = 1;
            return PGS_NORMAL;
        }
    }

    /* same palette -- check position */
    for (i = 0; i < h->num_rects; i++) {
        if (h->rects[i]->x != s->prev_obj_x[i] ||
            h->rects[i]->y != s->prev_obj_y[i])
            return PGS_NORMAL;
    }

    /* everything identical -- emit as acquisition point for refresh */
    return PGS_ACQUISITION;
}

static void pgs_update_cache(PGSSubEncContext *s, const AVSubtitle *h,
                              int state, int palette_update)
{
    int i;

    if (state == PGS_EPOCH_START)
        s->epoch_active = 1;

    s->prev_num_rects = h->num_rects;
    for (i = 0; i < h->num_rects && i < PGS_MAX_OBJECT_REFS; i++) {
        s->prev_obj_w[i] = h->rects[i]->w;
        s->prev_obj_h[i] = h->rects[i]->h;
        s->prev_obj_x[i] = h->rects[i]->x;
        s->prev_obj_y[i] = h->rects[i]->y;
    }

    if (h->num_rects) {
        const uint32_t *pal = (const uint32_t *)h->rects[0]->data[1];
        int nc = FFMIN(h->rects[0]->nb_colors, AVPALETTE_COUNT);
        memcpy(s->prev_palette, pal, nc * sizeof(uint32_t));
        s->prev_nb_colors = nc;
    }

    if (!h->num_rects)
        s->epoch_active = 0;
}

static int pgs_write_pcs(uint8_t **pq, const uint8_t *buf_end,
                          AVCodecContext *avctx, PGSSubEncContext *s,
                          const AVSubtitle *h, int state,
                          int palette_update)
{
    uint8_t *q = *pq, *pseg_len;
    int i, rects = h->num_rects;

    if (buf_end - q < 3 + 11 + rects * 8)
        return AVERROR_BUFFER_TOO_SMALL;

    *q++ = 0x16;
    pseg_len = q;
    q += 2;
    bytestream_put_be16(&q, avctx->width);
    bytestream_put_be16(&q, avctx->height);
    *q++ = s->frame_rate;
    bytestream_put_be16(&q, s->composition_number);
    *q++ = state;
    *q++ = palette_update ? 0x80 : 0x00;
    *q++ = 0x00;
    *q++ = rects;

    for (i = 0; i < rects; i++) {
        bytestream_put_be16(&q, i);
        *q++ = i;
        *q++ = (h->rects[i]->flags & AV_SUBTITLE_FLAG_FORCED)
               ? 0x40 : 0x00;
        bytestream_put_be16(&q, h->rects[i]->x);
        bytestream_put_be16(&q, h->rects[i]->y);
    }
    bytestream_put_be16(&pseg_len, q - pseg_len - 2);

    *pq = q;
    return 0;
}

static int pgs_write_wds(uint8_t **pq, const uint8_t *buf_end,
                          const AVSubtitle *h)
{
    uint8_t *q = *pq, *pseg_len;
    int i, rects = h->num_rects;

    if (buf_end - q < 3 + 1 + rects * 9)
        return AVERROR_BUFFER_TOO_SMALL;

    *q++ = 0x17;
    pseg_len = q;
    q += 2;
    *q++ = rects;
    for (i = 0; i < rects; i++) {
        *q++ = i;
        bytestream_put_be16(&q, h->rects[i]->x);
        bytestream_put_be16(&q, h->rects[i]->y);
        bytestream_put_be16(&q, h->rects[i]->w);
        bytestream_put_be16(&q, h->rects[i]->h);
    }
    bytestream_put_be16(&pseg_len, q - pseg_len - 2);

    *pq = q;
    return 0;
}

static int pgs_write_pds(uint8_t **pq, const uint8_t *buf_end,
                          const AVSubtitle *h, PGSSubEncContext *s,
                          int bt709)
{
    uint8_t *q = *pq, *pseg_len;
    const uint32_t *pal;
    int i, nc;

    pal = (const uint32_t *)h->rects[0]->data[1];
    nc = FFMIN(h->rects[0]->nb_colors, AVPALETTE_COUNT);

    if (buf_end - q < 3 + 2 + nc * 5)
        return AVERROR_BUFFER_TOO_SMALL;

    *q++ = 0x14;
    pseg_len = q;
    q += 2;
    *q++ = 0x00;
    *q++ = s->palette_version & 0xff;

    for (i = 0; i < nc; i++) {
        uint32_t c = pal[i];
        int r = (c >> 16) & 0xff;
        int g = (c >>  8) & 0xff;
        int b =  c        & 0xff;

        *q++ = i;
        if (bt709) {
            *q++ = RGB_TO_Y_BT709(r, g, b);
            *q++ = RGB_TO_V_BT709(r, g, b, 0);
            *q++ = RGB_TO_U_BT709(r, g, b, 0);
        } else {
            *q++ = RGB_TO_Y_CCIR(r, g, b);
            *q++ = RGB_TO_V_CCIR(r, g, b, 0);
            *q++ = RGB_TO_U_CCIR(r, g, b, 0);
        }
        *q++ = (c >> 24) & 0xff;
    }
    bytestream_put_be16(&pseg_len, q - pseg_len - 2);

    *pq = q;
    return 0;
}

static int pgs_write_ods(uint8_t **pq, const uint8_t *buf_end,
                          AVSubtitleRect *rect, int object_id)
{
    uint8_t *q = *pq;
    uint8_t *rle, *rp;
    int rsz, ret, odl;
    int64_t alloc64 = (int64_t)rect->w * rect->h * 4
                    + rect->h * 2;
    int alloc;

    if (alloc64 > INT_MAX)
        return AVERROR(EINVAL);
    alloc = alloc64;

    rle = av_malloc(alloc);
    if (!rle)
        return AVERROR(ENOMEM);

    rp = rle;
    ret = pgs_encode_rle(&rp, alloc, rect->data[0],
                         rect->linesize[0],
                         rect->w, rect->h);
    if (ret < 0) {
        av_free(rle);
        return ret;
    }
    rsz = ret;
    odl = 4 + rsz;

    if (rsz <= 0xffff - 11) {
        int ss = 11 + rsz;
        if (buf_end - q < 3 + ss) {
            av_free(rle);
            return AVERROR_BUFFER_TOO_SMALL;
        }
        *q++ = 0x15;
        bytestream_put_be16(&q, ss);
        bytestream_put_be16(&q, object_id);
        *q++ = 0x00;
        *q++ = 0xc0;
        bytestream_put_be24(&q, odl);
        bytestream_put_be16(&q, rect->w);
        bytestream_put_be16(&q, rect->h);
        memcpy(q, rle, rsz);
        q += rsz;
    } else {
        int off = 0, fs;
        int fmax = 0xffff - 11;
        int cmax = 0xffff - 4;

        fs = fmax;
        if (buf_end - q < 3 + 11 + fs) {
            av_free(rle);
            return AVERROR_BUFFER_TOO_SMALL;
        }
        *q++ = 0x15;
        bytestream_put_be16(&q, 11 + fs);
        bytestream_put_be16(&q, object_id);
        *q++ = 0x00;
        *q++ = 0x80;
        bytestream_put_be24(&q, odl);
        bytestream_put_be16(&q, rect->w);
        bytestream_put_be16(&q, rect->h);
        memcpy(q, rle, fs);
        q += fs;
        off += fs;

        while (off < rsz) {
            int rem = rsz - off;
            int last = rem <= cmax;
            fs = last ? rem : cmax;

            if (buf_end - q < 3 + 4 + fs) {
                av_free(rle);
                return AVERROR_BUFFER_TOO_SMALL;
            }
            *q++ = 0x15;
            bytestream_put_be16(&q, 4 + fs);
            bytestream_put_be16(&q, object_id);
            *q++ = 0x00;
            *q++ = last ? 0x40 : 0x00;
            memcpy(q, rle + off, fs);
            q += fs;
            off += fs;
        }
    }
    av_free(rle);

    *pq = q;
    return 0;
}

static int pgs_write_end(uint8_t **pq, const uint8_t *buf_end)
{
    uint8_t *q = *pq;

    if (buf_end - q < 3)
        return AVERROR_BUFFER_TOO_SMALL;
    *q++ = 0x80;
    bytestream_put_be16(&q, 0);

    *pq = q;
    return 0;
}

static int pgssub_encode(AVCodecContext *avctx, uint8_t *outbuf,
                         int buf_size, const AVSubtitle *h)
{
    PGSSubEncContext *s = avctx->priv_data;
    const uint8_t *const buf_end = outbuf + buf_size;
    uint8_t *q;
    int i, rects, bt709, palette_update, state, ret;

    q = outbuf;
    rects = h->num_rects;

    if (rects > PGS_MAX_OBJECT_REFS) {
        av_log(avctx, AV_LOG_ERROR,
               "Too many subtitle objects (%d, max %d)\n",
               rects, PGS_MAX_OBJECT_REFS);
        return AVERROR(EINVAL);
    }

    if (rects && !h->rects)
        return AVERROR(EINVAL);

    for (i = 0; i < rects; i++) {
        if (h->rects[i]->type != SUBTITLE_BITMAP) {
            av_log(avctx, AV_LOG_ERROR,
                   "Bitmap subtitle required\n");
            return AVERROR(EINVAL);
        }
        if (!h->rects[i]->w || !h->rects[i]->h) {
            av_log(avctx, AV_LOG_ERROR,
                   "Empty subtitle rectangle\n");
            return AVERROR(EINVAL);
        }
    }

    /* PGS windows must not overlap */
    if (rects == 2) {
        AVSubtitleRect *a = h->rects[0], *b = h->rects[1];
        if (a->x < b->x + b->w && a->x + a->w > b->x &&
            a->y < b->y + b->h && a->y + a->h > b->y) {
            av_log(avctx, AV_LOG_ERROR,
                   "Overlapping subtitle rectangles\n");
            return AVERROR(EINVAL);
        }
    }

    /* BT.709 for HD, BT.601 for SD per HDMV spec */
    if (avctx->colorspace == AVCOL_SPC_BT470BG ||
        avctx->colorspace == AVCOL_SPC_SMPTE170M)
        bt709 = 0;
    else if (avctx->colorspace == AVCOL_SPC_BT709)
        bt709 = 1;
    else
        bt709 = avctx->height <= 0 || avctx->height > 576;

    state = pgs_determine_state(s, h, &palette_update);

    /* Update palette_version before writing segments so the PDS
     * reflects the new version number. Cache is updated after writing. */
    if (state == PGS_EPOCH_START)
        s->palette_version = 0;
    else if (palette_update)
        s->palette_version = (s->palette_version + 1) & 0xff;

    /* PCS -- always emitted */
    ret = pgs_write_pcs(&q, buf_end, avctx, s, h, state, palette_update);
    if (ret < 0)
        return ret;

    if (rects && (state == PGS_EPOCH_START || state == PGS_ACQUISITION)) {
        /* full display set: WDS + PDS + ODS */
        ret = pgs_write_wds(&q, buf_end, h);
        if (ret < 0)
            return ret;
        ret = pgs_write_pds(&q, buf_end, h, s, bt709);
        if (ret < 0)
            return ret;
        for (i = 0; i < rects; i++) {
            ret = pgs_write_ods(&q, buf_end, h->rects[i], i);
            if (ret < 0)
                return ret;
        }
    } else if (rects && palette_update) {
        /* palette-only update: PDS only (no WDS, no ODS) */
        ret = pgs_write_pds(&q, buf_end, h, s, bt709);
        if (ret < 0)
            return ret;
    }
    /* position-only or clear: PCS + END only */

    ret = pgs_write_end(&q, buf_end);
    if (ret < 0)
        return ret;

    pgs_update_cache(s, h, state, palette_update);

    s->composition_number = (s->composition_number + 1) & 0xffff;
    return q - outbuf;
}

#define OFFSET(x) offsetof(PGSSubEncContext, x)
#define SE AV_OPT_FLAG_SUBTITLE_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "frame_rate", "PGS frame rate code (0x10=23.976, "
      "0x20=24, 0x30=25, 0x40=29.97, 0x60=50, 0x70=59.94)",
      OFFSET(frame_rate), AV_OPT_TYPE_INT,
      {.i64 = 0}, 0, 0x70, SE },
    { NULL },
};

static const AVClass pgssub_class = {
    .class_name = "PGS subtitle encoder",
    .item_name  = av_default_item_name,
    .option     = options,
    .version    = LIBAVUTIL_VERSION_INT,
};

const FFCodec ff_pgssub_encoder = {
    .p.name         = "pgssub",
    CODEC_LONG_NAME("HDMV Presentation Graphic Stream subtitles"),
    .p.type         = AVMEDIA_TYPE_SUBTITLE,
    .p.id           = AV_CODEC_ID_HDMV_PGS_SUBTITLE,
    .priv_data_size = sizeof(PGSSubEncContext),
    .init           = pgssub_init,
    FF_CODEC_ENCODE_SUB_CB(pgssub_encode),
    .p.priv_class   = &pgssub_class,
};
