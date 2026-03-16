/*
 * SUP muxer
 * Copyright (c) 2014 Petri Hintukainen <phintuka@users.sourceforge.net>
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

#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "libavutil/intreadwrite.h"

#define SUP_PGS_MAGIC 0x5047 /* "PG", big endian */

/* HDMV decoder model rates (US7620297B2) */
#define PGS_FREQ 90000
#define PGS_RD   16000000   /* Decoded Object Buffer fill rate */
#define PGS_RC   32000000   /* Graphics Plane write rate */

/**
 * Compute per-segment PTS/DTS for a PGS Display Set.
 *
 * Parse PCS and ODS segments to determine composition state and
 * object dimensions, then compute decode duration per the HDMV
 * timing model.  Each segment gets its own PTS/DTS in the SUP
 * header based on its role in the decode pipeline.
 */
static int sup_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    const uint8_t *data = pkt->data;
    size_t size = pkt->size;
    uint32_t base_pts = 0;
    int64_t decode_dur = 0;
    uint32_t seg_pts, seg_dts;
    uint32_t last_ods_pts = 0;
    int have_ods = 0;

    /* Pass 1: scan segments for timing parameters */
    {
        const uint8_t *p = data;
        size_t rem = size;
        int comp_state = -1;
        int video_w = 0, video_h = 0;
        int64_t obj_decode = 0, win_write = 0;

        while (rem > 2) {
            int type = p[0];
            size_t seg_size = AV_RB16(p + 1);
            size_t seg_len = seg_size + 3;
            const uint8_t *payload = p + 3;

            if (seg_len > rem)
                break;

            switch (type) {
            case 0x16: /* PCS */
                if (seg_size >= 11) {
                    video_w    = AV_RB16(payload);
                    video_h    = AV_RB16(payload + 2);
                    comp_state = payload[7] >> 6;
                }
                break;
            case 0x15: /* ODS */
                if (seg_size >= 11) {
                    int seq_flag = payload[3];
                    if (seq_flag & 0x80) { /* first in sequence */
                        int ow = AV_RB16(payload + 7);
                        int oh = AV_RB16(payload + 9);
                        int64_t px = (int64_t)ow * oh;
                        int64_t od = (px * PGS_FREQ + PGS_RD - 1) / PGS_RD;
                        int64_t ww = (px * PGS_FREQ + PGS_RC - 1) / PGS_RC;
                        obj_decode = FFMAX(obj_decode, od);
                        win_write += ww;
                    }
                }
                break;
            }

            p   += seg_len;
            rem -= seg_len;
        }

        if (comp_state == 2) { /* Epoch Start */
            decode_dur = ((int64_t)PGS_FREQ * video_w * video_h +
                          PGS_RC - 1) / PGS_RC;
        } else if (obj_decode > 0) {
            decode_dur = obj_decode + win_write;
        }
    }

    if (pkt->pts != AV_NOPTS_VALUE)
        base_pts = pkt->pts;

    /* Pass 2: write segments with per-segment timing */
    while (size > 2) {
        int type = data[0];
        size_t seg_size = AV_RB16(data + 1);
        size_t len = seg_size + 3;

        if (len > size) {
            av_log(s, AV_LOG_ERROR,
                   "Not enough data, skipping %zu bytes\n", size);
            return AVERROR_INVALIDDATA;
        }

        switch (type) {
        case 0x16: /* PCS */
        case 0x17: /* WDS */
        case 0x14: /* PDS */
            seg_pts = base_pts;
            seg_dts = base_pts - (uint32_t)FFMIN(decode_dur, base_pts);
            break;
        case 0x15: /* ODS */
            if (!have_ods) {
                /* First ODS */
                seg_dts = base_pts - (uint32_t)FFMIN(decode_dur, base_pts);
                if (seg_size >= 11) {
                    int seq_flag = data[3 + 3];
                    if (seq_flag & 0x80) {
                        int ow = AV_RB16(data + 3 + 7);
                        int oh = AV_RB16(data + 3 + 9);
                        int64_t od = ((int64_t)ow * oh * PGS_FREQ +
                                      PGS_RD - 1) / PGS_RD;
                        seg_pts = seg_dts + (uint32_t)od;
                    } else {
                        seg_pts = seg_dts;
                    }
                } else {
                    seg_pts = seg_dts;
                }
                last_ods_pts = seg_pts;
                have_ods = 1;
            } else {
                /* Continuation ODS */
                seg_dts = last_ods_pts;
                seg_pts = last_ods_pts;
            }
            break;
        case 0x80: /* END */
            if (have_ods) {
                seg_pts = last_ods_pts;
                seg_dts = last_ods_pts;
            } else {
                seg_pts = base_pts - (uint32_t)FFMIN(decode_dur, base_pts);
                seg_dts = seg_pts;
            }
            break;
        default:
            seg_pts = base_pts;
            seg_dts = base_pts;
            break;
        }

        avio_wb16(s->pb, SUP_PGS_MAGIC);
        avio_wb32(s->pb, seg_pts);
        avio_wb32(s->pb, seg_dts);
        avio_write(s->pb, data, len);

        data += len;
        size -= len;
    }

    if (size > 0) {
        av_log(s, AV_LOG_ERROR,
               "Skipping %zu bytes after last segment in frame\n", size);
        return AVERROR_INVALIDDATA;
    }

    return 0;
}

static av_cold int sup_init(AVFormatContext *s)
{
    avpriv_set_pts_info(s->streams[0], 32, 1, 90000);

    return 0;
}

const FFOutputFormat ff_sup_muxer = {
    .p.name           = "sup",
    .p.long_name      = NULL_IF_CONFIG_SMALL("raw HDMV Presentation Graphic Stream subtitles"),
    .p.extensions     = "sup",
    .p.mime_type      = "application/x-pgs",
    .p.video_codec    = AV_CODEC_ID_NONE,
    .p.audio_codec    = AV_CODEC_ID_NONE,
    .p.subtitle_codec = AV_CODEC_ID_HDMV_PGS_SUBTITLE,
    .p.flags          = AVFMT_VARIABLE_FPS | AVFMT_TS_NONSTRICT,
    .flags_internal   = FF_OFMT_FLAG_MAX_ONE_OF_EACH |
                        FF_OFMT_FLAG_ONLY_DEFAULT_CODECS,
    .init             = sup_init,
    .write_packet   = sup_write_packet,
};
