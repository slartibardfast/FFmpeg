/*
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

#include <math.h>
#include <stdint.h>

#include "ffmpeg.h"

#include "libavutil/avassert.h"
#include "libavutil/avstring.h"
#include "libavutil/avutil.h"
#include "libavutil/dict.h"
#include "libavutil/display.h"
#include "libavutil/eval.h"
#include "libavutil/frame.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"
#include "libavutil/rational.h"
#include "libavutil/time.h"
#include "libavutil/timestamp.h"

#include "libavcodec/avcodec.h"

#include "libavfilter/subtitle_render.h"

#include "libavutil/quantize.h"

#include "ffmpeg_subtitle_animation.c"

typedef struct EncoderPriv {
    Encoder        e;

    void          *log_parent;
    char           log_name[32];

    // combined size of all the packets received from the encoder
    uint64_t data_size;

    // number of packets received from the encoder
    uint64_t packets_encoded;

    int opened;
    int attach_par;

    Scheduler      *sch;
    unsigned        sch_idx;

    AVSubtitleRenderContext *sub_render;

    /* Coalescing buffer for overlapping text subtitle events.
     * Multiple ASS Dialogue lines with the same PTS are buffered
     * here and rendered as a single composite before encoding. */
    struct {
        char    **texts;
        int64_t  *durations;
        int       nb;
        int       cap;
        int64_t   pts;
        uint32_t  start_display_time;
        uint32_t  end_display_time;
    } sub_coalesce;
} EncoderPriv;

static EncoderPriv *ep_from_enc(Encoder *enc)
{
    return (EncoderPriv*)enc;
}

// data that is local to the decoder thread and not visible outside of it
typedef struct EncoderThread {
    AVFrame *frame;
    AVPacket  *pkt;
} EncoderThread;

void enc_free(Encoder **penc)
{
    Encoder *enc = *penc;
    EncoderPriv *ep;

    if (!enc)
        return;

    ep = ep_from_enc(enc);
    for (int i = 0; i < ep->sub_coalesce.nb; i++)
        av_freep(&ep->sub_coalesce.texts[i]);
    av_freep(&ep->sub_coalesce.texts);
    av_freep(&ep->sub_coalesce.durations);
    avfilter_subtitle_render_freep(&ep->sub_render);

    if (enc->enc_ctx)
        av_freep(&enc->enc_ctx->stats_in);
    avcodec_free_context(&enc->enc_ctx);

    av_freep(penc);
}

static const char *enc_item_name(void *obj)
{
    const EncoderPriv *ep = obj;

    return ep->log_name;
}

static const AVClass enc_class = {
    .class_name                = "Encoder",
    .version                   = LIBAVUTIL_VERSION_INT,
    .parent_log_context_offset = offsetof(EncoderPriv, log_parent),
    .item_name                 = enc_item_name,
};

int enc_alloc(Encoder **penc, const AVCodec *codec,
              Scheduler *sch, unsigned sch_idx, void *log_parent)
{
    EncoderPriv *ep;
    int ret = 0;

    *penc = NULL;

    ep = av_mallocz(sizeof(*ep));
    if (!ep)
        return AVERROR(ENOMEM);

    ep->e.class    = &enc_class;
    ep->log_parent = log_parent;

    ep->sch     = sch;
    ep->sch_idx = sch_idx;

    snprintf(ep->log_name, sizeof(ep->log_name), "enc:%s", codec->name);

    ep->e.enc_ctx = avcodec_alloc_context3(codec);
    if (!ep->e.enc_ctx) {
        ret = AVERROR(ENOMEM);
        goto fail;
    }

    *penc = &ep->e;

    return 0;
fail:
    enc_free((Encoder**)&ep);
    return ret;
}

static int hw_device_setup_for_encode(Encoder *e, AVCodecContext *enc_ctx,
                                      AVBufferRef *frames_ref)
{
    const AVCodecHWConfig *config;
    HWDevice *dev = NULL;

    if (frames_ref &&
        ((AVHWFramesContext*)frames_ref->data)->format ==
        enc_ctx->pix_fmt) {
        // Matching format, will try to use hw_frames_ctx.
    } else {
        frames_ref = NULL;
    }

    for (int i = 0;; i++) {
        config = avcodec_get_hw_config(enc_ctx->codec, i);
        if (!config)
            break;

        if (frames_ref &&
            config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX &&
            (config->pix_fmt == AV_PIX_FMT_NONE ||
             config->pix_fmt == enc_ctx->pix_fmt)) {
            av_log(e, AV_LOG_VERBOSE, "Using input "
                   "frames context (format %s) with %s encoder.\n",
                   av_get_pix_fmt_name(enc_ctx->pix_fmt),
                   enc_ctx->codec->name);
            enc_ctx->hw_frames_ctx = av_buffer_ref(frames_ref);
            if (!enc_ctx->hw_frames_ctx)
                return AVERROR(ENOMEM);
            return 0;
        }

        if (!dev &&
            config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)
            dev = hw_device_get_by_type(config->device_type);
    }

    if (dev) {
        av_log(e, AV_LOG_VERBOSE, "Using device %s "
               "(type %s) with %s encoder.\n", dev->name,
               av_hwdevice_get_type_name(dev->type), enc_ctx->codec->name);
        enc_ctx->hw_device_ctx = av_buffer_ref(dev->device_ref);
        if (!enc_ctx->hw_device_ctx)
            return AVERROR(ENOMEM);
    } else {
        // No device required, or no device available.
    }
    return 0;
}

int enc_open(void *opaque, const AVFrame *frame)
{
    OutputStream *ost = opaque;
    InputStream *ist = ost->ist;
    Encoder              *e = ost->enc;
    EncoderPriv         *ep = ep_from_enc(e);
    AVCodecContext *enc_ctx = e->enc_ctx;
    Decoder            *dec = NULL;
    const AVCodec      *enc = enc_ctx->codec;
    OutputFile          *of = ost->file;
    FrameData *fd;
    int frame_samples = 0;
    int ret;

    if (ep->opened)
        return 0;

    // frame is always non-NULL for audio and video
    av_assert0(frame || (enc->type != AVMEDIA_TYPE_VIDEO && enc->type != AVMEDIA_TYPE_AUDIO));

    if (frame) {
        av_assert0(frame->opaque_ref);
        fd = (FrameData*)frame->opaque_ref->data;

        ret = clone_side_data(&enc_ctx->decoded_side_data, &enc_ctx->nb_decoded_side_data,
                              fd->side_data, fd->nb_side_data, AV_FRAME_SIDE_DATA_FLAG_UNIQUE);
        if (ret < 0)
            return ret;
    }

    if (ist)
        dec = ist->decoder;

    // the timebase is chosen by filtering code
    if (ost->type == AVMEDIA_TYPE_AUDIO || ost->type == AVMEDIA_TYPE_VIDEO) {
        enc_ctx->time_base      = frame->time_base;
        enc_ctx->framerate      = fd->frame_rate_filter;
    }

    switch (enc_ctx->codec_type) {
    case AVMEDIA_TYPE_AUDIO:
        av_assert0(frame->format != AV_SAMPLE_FMT_NONE &&
                   frame->sample_rate > 0 &&
                   frame->ch_layout.nb_channels > 0);
        enc_ctx->sample_fmt     = frame->format;
        enc_ctx->sample_rate    = frame->sample_rate;
        ret = av_channel_layout_copy(&enc_ctx->ch_layout, &frame->ch_layout);
        if (ret < 0)
            return ret;

        if (ost->bits_per_raw_sample)
            enc_ctx->bits_per_raw_sample = ost->bits_per_raw_sample;
        else
            enc_ctx->bits_per_raw_sample = FFMIN(fd->bits_per_raw_sample,
                                                 av_get_bytes_per_sample(enc_ctx->sample_fmt) << 3);
        break;

    case AVMEDIA_TYPE_VIDEO: {
        av_assert0(frame->format != AV_PIX_FMT_NONE &&
                   frame->width > 0 &&
                   frame->height > 0);
        enc_ctx->width  = frame->width;
        enc_ctx->height = frame->height;
        enc_ctx->sample_aspect_ratio =
            ost->frame_aspect_ratio.num ? // overridden by the -aspect cli option
            av_mul_q(ost->frame_aspect_ratio, (AVRational){ enc_ctx->height, enc_ctx->width }) :
            frame->sample_aspect_ratio;

        enc_ctx->pix_fmt = frame->format;

        if (ost->bits_per_raw_sample)
            enc_ctx->bits_per_raw_sample = ost->bits_per_raw_sample;
        else
            enc_ctx->bits_per_raw_sample = FFMIN(fd->bits_per_raw_sample,
                                                 av_pix_fmt_desc_get(enc_ctx->pix_fmt)->comp[0].depth);

        /**
         * The video color properties should always be in sync with the user-
         * requested values, since we forward them to the filter graph.
         */
        enc_ctx->color_range            = frame->color_range;
        enc_ctx->color_primaries        = frame->color_primaries;
        enc_ctx->color_trc              = frame->color_trc;
        enc_ctx->colorspace             = frame->colorspace;
        enc_ctx->alpha_mode             = frame->alpha_mode;

        /* Video properties which are not part of filter graph negotiation */
        if (enc_ctx->chroma_sample_location == AVCHROMA_LOC_UNSPECIFIED) {
            enc_ctx->chroma_sample_location = frame->chroma_location;
        } else if (enc_ctx->chroma_sample_location != frame->chroma_location &&
                   frame->chroma_location != AVCHROMA_LOC_UNSPECIFIED) {
            av_log(e, AV_LOG_WARNING,
                   "Requested chroma sample location '%s' does not match the "
                   "frame tagged sample location '%s'; result may be incorrect.\n",
                   av_chroma_location_name(enc_ctx->chroma_sample_location),
                   av_chroma_location_name(frame->chroma_location));
        }

        if (enc_ctx->flags & (AV_CODEC_FLAG_INTERLACED_DCT | AV_CODEC_FLAG_INTERLACED_ME) ||
            (frame->flags & AV_FRAME_FLAG_INTERLACED)
#if FFMPEG_OPT_TOP
            || ost->top_field_first >= 0
#endif
            ) {
            int top_field_first =
#if FFMPEG_OPT_TOP
                ost->top_field_first >= 0 ?
                ost->top_field_first :
#endif
                !!(frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);

            if (enc->id == AV_CODEC_ID_MJPEG)
                enc_ctx->field_order = top_field_first ? AV_FIELD_TT : AV_FIELD_BB;
            else
                enc_ctx->field_order = top_field_first ? AV_FIELD_TB : AV_FIELD_BT;
        } else
            enc_ctx->field_order = AV_FIELD_PROGRESSIVE;

        break;
        }
    case AVMEDIA_TYPE_SUBTITLE:
        enc_ctx->time_base = AV_TIME_BASE_Q;

        if (!enc_ctx->width) {
            enc_ctx->width     = ost->ist->par->width;
            enc_ctx->height    = ost->ist->par->height;
        }

        av_assert0(dec);
        if (dec->subtitle_header) {
            /* ASS code assumes this buffer is null terminated so add extra byte. */
            enc_ctx->subtitle_header = av_mallocz(dec->subtitle_header_size + 1);
            if (!enc_ctx->subtitle_header)
                return AVERROR(ENOMEM);
            memcpy(enc_ctx->subtitle_header, dec->subtitle_header,
                   dec->subtitle_header_size);
            enc_ctx->subtitle_header_size = dec->subtitle_header_size;
        }

        break;
    default:
        av_assert0(0);
        break;
    }

    if (ost->bitexact)
        enc_ctx->flags |= AV_CODEC_FLAG_BITEXACT;

    if (enc->capabilities & AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE)
        enc_ctx->flags |= AV_CODEC_FLAG_COPY_OPAQUE;

    enc_ctx->flags |= AV_CODEC_FLAG_FRAME_DURATION;

    ret = hw_device_setup_for_encode(e, enc_ctx, frame ? frame->hw_frames_ctx : NULL);
    if (ret < 0) {
        av_log(e, AV_LOG_ERROR,
               "Encoding hardware device setup failed: %s\n", av_err2str(ret));
        return ret;
    }

    if ((ret = avcodec_open2(enc_ctx, enc, NULL)) < 0) {
        if (ret != AVERROR_EXPERIMENTAL)
            av_log(e, AV_LOG_ERROR, "Error while opening encoder - maybe "
                   "incorrect parameters such as bit_rate, rate, width or height.\n");
        return ret;
    }

    ep->opened = 1;

    if (enc_ctx->frame_size)
        frame_samples = enc_ctx->frame_size;

    if (enc_ctx->bit_rate && enc_ctx->bit_rate < 1000 &&
        enc_ctx->codec_id != AV_CODEC_ID_CODEC2 /* don't complain about 700 bit/s modes */)
        av_log(e, AV_LOG_WARNING, "The bitrate parameter is set too low."
                                    " It takes bits/s as argument, not kbits/s\n");

    ret = of_stream_init(of, ost, enc_ctx);
    if (ret < 0)
        return ret;

    return frame_samples;
}

static int check_recording_time(OutputStream *ost, int64_t ts, AVRational tb)
{
    OutputFile *of = ost->file;

    if (of->recording_time != INT64_MAX &&
        av_compare_ts(ts, tb, of->recording_time, AV_TIME_BASE_Q) >= 0) {
        return 0;
    }
    return 1;
}

/**
 * Fill an AVSubtitleRect from pre-quantized palette and indices.
 * The indices_region must point to rw*rh contiguous bytes.
 */
static int fill_rect_bitmap(AVSubtitleRect *rect,
                             const uint8_t *indices_region,
                             const uint32_t *palette, int nb_colors,
                             int rx, int ry, int rw, int rh)
{
    int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);

    rect->type       = SUBTITLE_BITMAP;
    rect->x          = rx;
    rect->y          = ry;
    rect->w          = rw;
    rect->h          = rh;
    rect->nb_colors  = nb_colors;

    rect->data[0] = av_memdup(indices_region, nb_pixels);
    if (!rect->data[0])
        return AVERROR(ENOMEM);
    rect->linesize[0] = rw;

    rect->data[1] = av_memdup(palette, 256 * 4);
    if (!rect->data[1]) {
        av_freep(&rect->data[0]);
        return AVERROR(ENOMEM);
    }
    rect->linesize[1] = nb_colors * 4;

    return 0;
}

/**
 * Quantize an RGBA region and fill an AVSubtitleRect as SUBTITLE_BITMAP.
 */
static int quantize_rgba_to_rect(AVSubtitleRect *rect,
                                 const uint8_t *rgba,
                                 int rx, int ry, int rw, int rh)
{
    AVQuantizeContext *qctx;
    uint32_t palette[256] = {0};
    uint8_t *indices;
    int nb_pixels = (int)FFMIN((int64_t)rw * rh, INT_MAX);
    int nb_colors, ret;

    qctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
    if (!qctx)
        return AVERROR(ENOMEM);

    nb_colors = av_quantize_generate_palette(qctx, rgba, nb_pixels,
                                             palette, 10);
    if (nb_colors < 0) {
        av_quantize_freep(&qctx);
        return nb_colors;
    }

    indices = av_malloc(nb_pixels);
    if (!indices) {
        av_quantize_freep(&qctx);
        return AVERROR(ENOMEM);
    }

    ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
    av_quantize_freep(&qctx);
    if (ret < 0) {
        av_free(indices);
        return ret;
    }

    rect->type       = SUBTITLE_BITMAP;
    rect->x          = rx;
    rect->y          = ry;
    rect->w          = rw;
    rect->h          = rh;
    rect->nb_colors  = nb_colors;
    rect->data[0]    = indices;
    rect->linesize[0] = rw;

    rect->data[1] = av_malloc(256 * 4);
    if (!rect->data[1]) {
        av_freep(&rect->data[0]);
        return AVERROR(ENOMEM);
    }
    memcpy(rect->data[1], palette, 256 * 4);
    rect->linesize[1] = nb_colors * 4;

    return 0;
}

/**
 * Lazy-init the subtitle rendering context for text-to-bitmap conversion.
 */
static int ensure_render_context(EncoderPriv *ep, OutputStream *ost)
{
    AVCodecContext *enc_ctx = ep->e.enc_ctx;

    if (ep->sub_render)
        return 0;

    if (enc_ctx->width <= 0 || enc_ctx->height <= 0) {
        av_log(&ep->e, AV_LOG_ERROR,
               "Canvas size required for text to bitmap subtitle "
               "conversion (use -s WxH)\n");
        return AVERROR(EINVAL);
    }

    ep->sub_render = avfilter_subtitle_render_alloc(enc_ctx->width,
                                                    enc_ctx->height);
    if (!ep->sub_render)
        return AVERROR(ENOMEM);

    if (enc_ctx->subtitle_header)
        avfilter_subtitle_render_set_header(
            ep->sub_render,
            (const char *)enc_ctx->subtitle_header);

    /* Load fonts from input file attachments */
    if (ost->ist && ost->ist->file) {
        AVFormatContext *fmt = ost->ist->file->ctx;
        for (int j = 0; j < fmt->nb_streams; j++) {
            AVStream *st = fmt->streams[j];
            const AVDictionaryEntry *tag;

            if (st->codecpar->codec_type != AVMEDIA_TYPE_ATTACHMENT)
                continue;
            if (!st->codecpar->extradata_size)
                continue;
            tag = av_dict_get(st->metadata, "filename", NULL,
                              AV_DICT_MATCH_CASE);
            if (!tag)
                continue;
            avfilter_subtitle_render_add_font(
                ep->sub_render, tag->value,
                st->codecpar->extradata,
                st->codecpar->extradata_size);
        }
    }

    return 0;
}

static int convert_text_to_bitmap(EncoderPriv *ep, OutputStream *ost,
                                  AVSubtitle *sub)
{
    AVCodecContext *enc_ctx = ep->e.enc_ctx;
    const AVCodecDescriptor *enc_desc;
    int need_convert = 0;
    unsigned i;
    int ret;

    enc_desc = avcodec_descriptor_get(enc_ctx->codec_id);
    if (!enc_desc || !(enc_desc->props & AV_CODEC_PROP_BITMAP_SUB))
        return 0;

    for (i = 0; i < sub->num_rects; i++) {
        if (sub->rects[i]->type == SUBTITLE_ASS ||
            sub->rects[i]->type == SUBTITLE_TEXT) {
            need_convert = 1;
            break;
        }
    }
    if (!need_convert)
        return 0;

    ret = ensure_render_context(ep, ost);
    if (ret < 0)
        return ret;

    for (i = 0; i < sub->num_rects; i++) {
        AVSubtitleRect *rect = sub->rects[i];
        const char *text;
        uint8_t *rgba = NULL;
        int linesize, rx, ry, rw, rh;
        int64_t start_ms, duration_ms;
        int gap_start, gap_end;

        if (rect->type != SUBTITLE_ASS && rect->type != SUBTITLE_TEXT)
            continue;

        text = rect->ass ? rect->ass : rect->text;
        if (!text || !text[0])
            continue;

        start_ms    = sub->start_display_time;
        duration_ms = sub->end_display_time - sub->start_display_time;

        ret = avfilter_subtitle_render_frame(ep->sub_render, text,
                                             start_ms, duration_ms,
                                             &rgba, &linesize,
                                             &rx, &ry, &rw, &rh);
        if (ret < 0)
            return ret;
        if (!rgba)
            continue; /* empty render */

        av_freep(&rect->ass);
        av_freep(&rect->text);

        /* Quantize the full RGBA first, then optionally split.  Both
         * halves must share one palette because PGS uses a single
         * Palette Definition Segment per Display Set. */
        {
            AVQuantizeContext *qctx;
            uint32_t pal[256] = {0};
            uint8_t *indices;
            int nb_pixels = rw * rh;
            int nc;

            qctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
            if (!qctx) {
                av_free(rgba);
                return AVERROR(ENOMEM);
            }

            nc = av_quantize_generate_palette(qctx, rgba, nb_pixels, pal, 10);
            if (nc < 0) {
                av_quantize_freep(&qctx);
                av_free(rgba);
                return nc;
            }

            indices = av_malloc(nb_pixels);
            if (!indices) {
                av_quantize_freep(&qctx);
                av_free(rgba);
                return AVERROR(ENOMEM);
            }

            ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
            av_quantize_freep(&qctx);
            if (ret < 0) {
                av_free(indices);
                av_free(rgba);
                return ret;
            }

            if (sub->num_rects < 2 &&
                find_transparent_gap(rgba, linesize, rw, rh,
                                     &gap_start, &gap_end)) {
                AVSubtitleRect *bot_rect;
                AVSubtitleRect **new_rects;
                int top_h = gap_start;
                int bot_h = rh - gap_end;

                new_rects = av_realloc_array(sub->rects, sub->num_rects + 1,
                                             sizeof(*sub->rects));
                if (!new_rects) {
                    av_free(indices);
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                sub->rects = new_rects;

                bot_rect = av_mallocz(sizeof(*bot_rect));
                if (!bot_rect) {
                    av_free(indices);
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                sub->rects[sub->num_rects] = bot_rect;
                sub->num_rects++;

                ret = fill_rect_bitmap(rect, indices, pal, nc,
                                       rx, ry, rw, top_h);
                if (ret < 0) {
                    av_free(indices);
                    av_free(rgba);
                    return ret;
                }

                ret = fill_rect_bitmap(bot_rect, indices + gap_end * rw,
                                       pal, nc, rx, ry + gap_end, rw, bot_h);
                av_free(indices);
                av_free(rgba);
                if (ret < 0)
                    return ret;
            } else {
                /* No split -- single rect, transfer indices ownership */
                rect->type       = SUBTITLE_BITMAP;
                rect->x          = rx;
                rect->y          = ry;
                rect->w          = rw;
                rect->h          = rh;
                rect->nb_colors  = nc;
                rect->data[0]    = indices;
                rect->linesize[0] = rw;
                rect->data[1] = av_malloc(256 * 4);
                if (!rect->data[1]) {
                    av_free(rgba);
                    return AVERROR(ENOMEM);
                }
                memcpy(rect->data[1], pal, 256 * 4);
                rect->linesize[1] = nc * 4;
                av_free(rgba);
            }
        }
    }

    return 0;
}

/**
 * Encode a single subtitle frame (one avcodec_encode_subtitle call)
 * and send the resulting packet to the muxer.
 */
static int encode_subtitle_packet(EncoderPriv *ep, AVCodecContext *enc,
                                  AVSubtitle *sub, int64_t pts,
                                  AVPacket *pkt)
{
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, ret;

    ret = av_new_packet(pkt, subtitle_out_max_size);
    if (ret < 0)
        return AVERROR(ENOMEM);

    ep->e.frames_encoded++;

    subtitle_out_size = avcodec_encode_subtitle(enc, pkt->data, pkt->size, sub);
    if (subtitle_out_size < 0) {
        av_log(&ep->e, AV_LOG_FATAL, "Subtitle encoding failed\n");
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

    ret = sch_enc_send(ep->sch, ep->sch_idx, pkt);
    if (ret < 0) {
        av_packet_unref(pkt);
        return ret;
    }
    return 0;
}

/**
 * Animated subtitle encoding for PGS.
 *
 * Renders the event at every frame interval, classifies changes
 * (alpha-only, position-only, or content), and encodes using the
 * optimal PGS Display Set type for each frame. Format-agnostic:
 * works with any text subtitle format by observing renderer output.
 */
static int do_subtitle_out_animated(OutputFile *of, OutputStream *ost,
                                    AVSubtitle *sub, AVPacket *pkt,
                                    const char *text, int events_loaded)
{
    Encoder *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    AVCodecContext *enc = e->enc_ctx;
    int64_t start_ms, duration_ms, base_pts, pts;
    int frame_ms, ret;
    int64_t t;

    /* Pass 1 scan state */
    uint8_t *rgba0 = NULL;
    int ls0, x0, y0, w0, h0;
    int64_t peak_alpha, first_alpha;
    int64_t peak_time;

    /* Recorded animated frame timestamps */
    int64_t *anim_times = NULL;
    int n_anim = 0, anim_cap = 0;

    enum SubtitleChangeType worst_change = SUB_CHANGE_NONE;

    /* Encoding state */
    AVSubtitle local_sub;
    AVSubtitleRect local_rect;
    AVSubtitleRect *local_rects[1];
    uint32_t ref_palette[256];
    uint32_t scaled_pal[256];
    int nb_colors;

    start_ms    = sub->start_display_time;
    duration_ms = sub->end_display_time - sub->start_display_time;

    if (duration_ms <= 0 || sub->pts == AV_NOPTS_VALUE)
        return 0;

    /* Frame interval from encoder framerate, fallback to ~24fps */
    if (enc->framerate.num > 0 && enc->framerate.den > 0)
        frame_ms = 1000 * enc->framerate.den / enc->framerate.num;
    else
        frame_ms = 42;
    if (frame_ms < 1)
        frame_ms = 1;

    base_pts = sub->pts;
    if (of->start_time != AV_NOPTS_VALUE)
        base_pts -= of->start_time;

    /* --- Pass 1: Scan all frames, classify, find peak alpha --- */

    if (!events_loaded) {
        ret = avfilter_subtitle_render_init_event(ep->sub_render, text,
                                                  start_ms, duration_ms);
        if (ret < 0)
            return ret;
    }

    /* Render first frame */
    ret = avfilter_subtitle_render_sample(ep->sub_render, start_ms,
                                          &rgba0, &ls0,
                                          &x0, &y0, &w0, &h0, NULL);
    if (ret < 0)
        return ret;
    if (!rgba0)
        return 0; /* empty render -- nothing to encode */

    first_alpha = rgba_alpha_sum(rgba0, w0, h0, ls0);
    peak_alpha  = first_alpha;
    peak_time   = start_ms;

    for (t = start_ms + frame_ms; t <= start_ms + duration_ms; t += frame_ms) {
        uint8_t *rgba = NULL;
        int ls, sx, sy, sw, sh, dc;
        enum SubtitleChangeType change;

        ret = avfilter_subtitle_render_sample(ep->sub_render, t,
                                              &rgba, &ls,
                                              &sx, &sy, &sw, &sh, &dc);
        if (ret < 0)
            goto fail;

        if (dc == 0 && rgba) {
            av_free(rgba);
            continue;
        }

        if (!rgba) {
            /* Became fully transparent -- treat as content change */
            change = SUB_CHANGE_CONTENT;
        } else {
            int64_t cur_alpha;
            change = classify_subtitle_change(rgba0, x0, y0, w0, h0, ls0,
                                              rgba, sx, sy, sw, sh, ls);
            cur_alpha = rgba_alpha_sum(rgba, sw, sh, ls);
            if (cur_alpha > peak_alpha) {
                peak_alpha = cur_alpha;
                peak_time  = t;
            }
            av_free(rgba);
        }

        if (change > worst_change)
            worst_change = change;

        /* Record this timestamp */
        if (n_anim >= anim_cap) {
            int64_t *tmp;
            anim_cap = anim_cap ? anim_cap * 2 : 64;
            tmp = av_realloc_array(anim_times, anim_cap, sizeof(*anim_times));
            if (!tmp) {
                ret = AVERROR(ENOMEM);
                goto fail;
            }
            anim_times = tmp;
        }
        anim_times[n_anim++] = t;
    }

    if (worst_change == SUB_CHANGE_NONE) {
        /* Static subtitle -- quantize first frame, single encode */
        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, rgba0, x0, y0, w0, h0);
        av_freep(&rgba0);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
        goto done;
    }

    av_freep(&rgba0);

    /* --- Pass 2: Re-render and encode based on classification --- */

    ret = avfilter_subtitle_render_init_event(ep->sub_render, text,
                                              start_ms, duration_ms);
    if (ret < 0)
        goto fail;

    if (worst_change == SUB_CHANGE_ALPHA) {
        uint8_t *peak_rgba = NULL;
        int peak_ls, px, py, pw, ph;
        int alpha_pct, i;
        int palette_updates = 0;

        /* Render and quantize the peak frame as reference */
        ret = avfilter_subtitle_render_sample(ep->sub_render, peak_time,
                                              &peak_rgba, &peak_ls,
                                              &px, &py, &pw, &ph, NULL);
        if (ret < 0)
            goto fail;
        if (!peak_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, peak_rgba, px, py, pw, ph);
        av_free(peak_rgba);
        if (ret < 0)
            goto fail;

        nb_colors = FFMIN(local_rect.nb_colors, 256);
        memcpy(ref_palette, local_rect.data[1], nb_colors * 4);

        /* Epoch Start with first frame's alpha (may be 0% for fade-in) */
        {
            uint8_t *first_rgba = NULL;
            int fls, fx, fy, fw, fh;
            int64_t fa;

            ret = avfilter_subtitle_render_init_event(ep->sub_render, text,
                                                      start_ms, duration_ms);
            if (ret < 0)
                goto fail_rect;
            ret = avfilter_subtitle_render_sample(ep->sub_render, start_ms,
                                                  &first_rgba, &fls,
                                                  &fx, &fy, &fw, &fh, NULL);
            if (ret < 0)
                goto fail_rect;

            if (first_rgba && peak_alpha > 0) {
                fa = rgba_alpha_sum(first_rgba, fw, fh, fls);
                alpha_pct = (int)(fa * 100 / peak_alpha);
            } else {
                alpha_pct = 0;
            }
            av_free(first_rgba);

            scale_palette_alpha(ref_palette, scaled_pal, nb_colors, alpha_pct);
            memcpy(local_rect.data[1], scaled_pal, nb_colors * 4);
        }

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_rect;

        ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_rect;

        /* Encode each animated frame with scaled palette */
        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;
            int64_t cur_alpha;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            /* Palette version safety: reset epoch before wrapping */
            if (palette_updates >= 254) {
                memcpy(local_rect.data[1], ref_palette, nb_colors * 4);
                pts = base_pts + av_rescale_q(anim_times[i],
                                              (AVRational){ 1, 1000 },
                                              AV_TIME_BASE_Q);
                local_sub.pts = pts;
                ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
                if (ret < 0)
                    goto fail_rect;
                palette_updates = 0;
            }

            ret = avfilter_subtitle_render_sample(ep->sub_render,
                                                  anim_times[i],
                                                  &rgba, &ls,
                                                  &sx, &sy, &sw, &sh,
                                                  NULL);
            if (ret < 0)
                goto fail_rect;
            if (!rgba)
                continue;

            cur_alpha = rgba_alpha_sum(rgba, sw, sh, ls);
            av_free(rgba);

            alpha_pct = peak_alpha > 0
                        ? (int)(cur_alpha * 100 / peak_alpha) : 0;
            scale_palette_alpha(ref_palette, scaled_pal, nb_colors, alpha_pct);
            memcpy(local_rect.data[1], scaled_pal, nb_colors * 4);

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_rect;
            palette_updates++;
        }

done_rect:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    } else if (worst_change == SUB_CHANGE_POSITION) {
        uint8_t *first_rgba = NULL;
        int fls, fx, fy, fw, fh, i;

        /* Quantize first frame (all frames have same bitmap) */
        ret = avfilter_subtitle_render_sample(ep->sub_render, start_ms,
                                              &first_rgba, &fls,
                                              &fx, &fy, &fw, &fh, NULL);
        if (ret < 0)
            goto fail;
        if (!first_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, first_rgba, fx, fy, fw, fh);
        av_free(first_rgba);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_pos;

        ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_pos;

        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            ret = avfilter_subtitle_render_sample(ep->sub_render,
                                                  anim_times[i],
                                                  &rgba, &ls,
                                                  &sx, &sy, &sw, &sh,
                                                  NULL);
            if (ret < 0)
                goto fail_pos;
            av_free(rgba);

            local_rect.x = sx;
            local_rect.y = sy;

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_pos;
        }

done_pos:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    } else {
        /* CONTENT: each frame gets independent quantization */
        uint8_t *first_rgba = NULL;
        int fls, fx, fy, fw, fh, i;

        ret = avfilter_subtitle_render_sample(ep->sub_render, start_ms,
                                              &first_rgba, &fls,
                                              &fx, &fy, &fw, &fh, NULL);
        if (ret < 0)
            goto fail;
        if (!first_rgba) {
            ret = 0;
            goto done;
        }

        memset(&local_rect, 0, sizeof(local_rect));
        ret = quantize_rgba_to_rect(&local_rect, first_rgba, fx, fy, fw, fh);
        av_free(first_rgba);
        if (ret < 0)
            goto fail;

        local_rects[0] = &local_rect;
        local_sub = *sub;
        local_sub.num_rects = 1;
        local_sub.rects = local_rects;
        pts = base_pts + av_rescale_q(start_ms,
                                      (AVRational){ 1, 1000 },
                                      AV_TIME_BASE_Q);
        local_sub.pts                = pts;
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
            goto done_content;

        ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
        if (ret < 0)
            goto fail_content;

        for (i = 0; i < n_anim; i++) {
            uint8_t *rgba = NULL;
            int ls, sx, sy, sw, sh;

            if (!check_recording_time(ost, base_pts, AV_TIME_BASE_Q))
                break;

            ret = avfilter_subtitle_render_sample(ep->sub_render,
                                                  anim_times[i],
                                                  &rgba, &ls,
                                                  &sx, &sy, &sw, &sh,
                                                  NULL);
            if (ret < 0)
                goto fail_content;
            if (!rgba)
                continue;

            av_freep(&local_rect.data[0]);
            av_freep(&local_rect.data[1]);
            memset(&local_rect, 0, sizeof(local_rect));
            ret = quantize_rgba_to_rect(&local_rect, rgba, sx, sy, sw, sh);
            av_free(rgba);
            if (ret < 0)
                goto fail;

            pts = base_pts + av_rescale_q(anim_times[i],
                                          (AVRational){ 1, 1000 },
                                          AV_TIME_BASE_Q);
            local_sub.pts = pts;
            ret = encode_subtitle_packet(ep, enc, &local_sub, pts, pkt);
            if (ret < 0)
                goto fail_content;
        }

done_content:
        av_freep(&local_rect.data[0]);
        av_freep(&local_rect.data[1]);
    }

done:
    av_freep(&anim_times);
    av_freep(&rgba0);
    return 0;

fail_rect:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
    goto fail;
fail_pos:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
    goto fail;
fail_content:
    av_freep(&local_rect.data[0]);
    av_freep(&local_rect.data[1]);
fail:
    av_freep(&anim_times);
    av_freep(&rgba0);
    return ret;
}

/* -- Subtitle event coalescing ------------------------------------------- */

static void sub_coalesce_reset(EncoderPriv *ep)
{
    for (int i = 0; i < ep->sub_coalesce.nb; i++)
        av_freep(&ep->sub_coalesce.texts[i]);
    ep->sub_coalesce.nb = 0;
}

static int sub_coalesce_append(EncoderPriv *ep, const char *text,
                                int64_t duration, int64_t pts,
                                uint32_t start_display_time,
                                uint32_t end_display_time)
{
    if (ep->sub_coalesce.nb >= ep->sub_coalesce.cap) {
        int new_cap = ep->sub_coalesce.cap ? ep->sub_coalesce.cap * 2 : 4;
        void *tmp;

        /* Grow both arrays. If the second realloc fails, cap is
         * not updated so the next call retries both arrays. */
        tmp = av_realloc_array(ep->sub_coalesce.texts,
                               new_cap, sizeof(*ep->sub_coalesce.texts));
        if (!tmp)
            return AVERROR(ENOMEM);
        ep->sub_coalesce.texts = tmp;

        tmp = av_realloc_array(ep->sub_coalesce.durations,
                               new_cap, sizeof(*ep->sub_coalesce.durations));
        if (!tmp)
            return AVERROR(ENOMEM);
        ep->sub_coalesce.durations = tmp;

        ep->sub_coalesce.cap = new_cap;
    }

    ep->sub_coalesce.texts[ep->sub_coalesce.nb] = av_strdup(text);
    if (!ep->sub_coalesce.texts[ep->sub_coalesce.nb])
        return AVERROR(ENOMEM);
    ep->sub_coalesce.durations[ep->sub_coalesce.nb] = duration;

    if (ep->sub_coalesce.nb == 0) {
        ep->sub_coalesce.pts                = pts;
        ep->sub_coalesce.start_display_time = start_display_time;
        ep->sub_coalesce.end_display_time   = end_display_time;
    } else {
        if (end_display_time > ep->sub_coalesce.end_display_time)
            ep->sub_coalesce.end_display_time = end_display_time;
    }

    ep->sub_coalesce.nb++;
    return 0;
}

/**
 * Flush buffered coalesced subtitle events.
 *
 * Renders all buffered events as a composite, quantizes, and encodes.
 * For animated events, delegates to the multi-timepoint renderer.
 */
static int flush_coalesced_subtitles(OutputFile *of, OutputStream *ost,
                                      AVPacket *pkt)
{
    Encoder *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    AVCodecContext *enc = e->enc_ctx;
    int64_t start_ms, duration_ms, pts;
    int ret, i, has_animation = 0;
    uint8_t *rgba = NULL;
    int linesize, rx, ry, rw, rh;
    int gap_start, gap_end;

    AVSubtitle comp_sub;
    AVSubtitleRect comp_rect  = {0};
    AVSubtitleRect comp_rect2 = {0};
    AVSubtitleRect *comp_rects[2] = { &comp_rect, &comp_rect2 };

    if (ep->sub_coalesce.nb == 0)
        return 0;

    ret = ensure_render_context(ep, ost);
    if (ret < 0)
        goto cleanup;

    start_ms    = ep->sub_coalesce.start_display_time;
    duration_ms = ep->sub_coalesce.end_display_time -
                  ep->sub_coalesce.start_display_time;

    /* Load all events into render context */
    ret = avfilter_subtitle_render_init_event(ep->sub_render,
              ep->sub_coalesce.texts[0], start_ms,
              ep->sub_coalesce.durations[0]);
    if (ret < 0)
        goto cleanup;

    for (i = 1; i < ep->sub_coalesce.nb; i++) {
        ret = avfilter_subtitle_render_add_event(ep->sub_render,
                  ep->sub_coalesce.texts[i], start_ms,
                  ep->sub_coalesce.durations[i]);
        if (ret < 0)
            goto cleanup;
    }

    /* Check if any text has animation override tags */
    for (i = 0; i < ep->sub_coalesce.nb; i++) {
        if (strchr(ep->sub_coalesce.texts[i], '{')) {
            has_animation = 1;
            break;
        }
    }

    if (has_animation) {
        AVSubtitle anim_sub = {0};
        anim_sub.pts                = ep->sub_coalesce.pts;
        anim_sub.start_display_time = ep->sub_coalesce.start_display_time;
        anim_sub.end_display_time   = ep->sub_coalesce.end_display_time;
        ret = do_subtitle_out_animated(of, ost, &anim_sub, pkt, NULL, 1);
        goto cleanup;
    }

    /* Static path: render composite, quantize, encode */
    ret = avfilter_subtitle_render_sample(ep->sub_render, start_ms,
              &rgba, &linesize, &rx, &ry, &rw, &rh, NULL);
    if (ret < 0)
        goto cleanup;
    if (!rgba) {
        ret = 0;
        goto cleanup;
    }

    pts = ep->sub_coalesce.pts;
    if (of->start_time != AV_NOPTS_VALUE)
        pts -= of->start_time;

    if (!check_recording_time(ost, pts, AV_TIME_BASE_Q)) {
        av_free(rgba);
        ret = AVERROR_EOF;
        goto cleanup;
    }

    /* Build composite subtitle for encoding */
    memset(&comp_sub, 0, sizeof(comp_sub));
    comp_sub.start_display_time = 0;
    comp_sub.end_display_time   = duration_ms;
    comp_sub.rects     = comp_rects;
    comp_sub.num_rects = 1;

    /* Quantize full RGBA first, then optionally split.  Both halves
     * must share one palette (PGS: one PDS per Display Set). */
    {
        AVQuantizeContext *qctx;
        uint32_t pal[256] = {0};
        uint8_t *indices;
        int nb_pixels = rw * rh;
        int nc;

        qctx = av_quantize_alloc(AV_QUANTIZE_NEUQUANT, 256);
        if (!qctx) {
            av_free(rgba);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        nc = av_quantize_generate_palette(qctx, rgba, nb_pixels, pal, 10);
        if (nc < 0) {
            av_quantize_freep(&qctx);
            av_free(rgba);
            ret = nc;
            goto cleanup;
        }

        indices = av_malloc(nb_pixels);
        if (!indices) {
            av_quantize_freep(&qctx);
            av_free(rgba);
            ret = AVERROR(ENOMEM);
            goto cleanup;
        }

        ret = av_quantize_apply(qctx, rgba, indices, nb_pixels);
        av_quantize_freep(&qctx);
        if (ret < 0) {
            av_free(indices);
            av_free(rgba);
            goto cleanup;
        }

        if (find_transparent_gap(rgba, linesize, rw, rh,
                                 &gap_start, &gap_end)) {
            int top_h = gap_start;
            int bot_h = rh - gap_end;

            ret = fill_rect_bitmap(&comp_rect, indices, pal, nc,
                                   rx, ry, rw, top_h);
            if (ret < 0) {
                av_free(indices);
                av_free(rgba);
                goto cleanup;
            }

            ret = fill_rect_bitmap(&comp_rect2, indices + gap_end * rw,
                                   pal, nc, rx, ry + gap_end, rw, bot_h);
            av_free(indices);
            av_free(rgba);
            if (ret < 0)
                goto cleanup;

            comp_sub.num_rects = 2;
        } else {
            comp_rect.type       = SUBTITLE_BITMAP;
            comp_rect.x          = rx;
            comp_rect.y          = ry;
            comp_rect.w          = rw;
            comp_rect.h          = rh;
            comp_rect.nb_colors  = nc;
            comp_rect.data[0]    = indices;
            comp_rect.linesize[0] = rw;
            comp_rect.data[1] = av_malloc(256 * 4);
            if (!comp_rect.data[1]) {
                av_free(rgba);
                ret = AVERROR(ENOMEM);
                goto cleanup;
            }
            memcpy(comp_rect.data[1], pal, 256 * 4);
            comp_rect.linesize[1] = nc * 4;
            av_free(rgba);
        }
    }

    ret = encode_subtitle_packet(ep, enc, &comp_sub, pts, pkt);

cleanup:
    av_freep(&comp_rect.data[0]);
    av_freep(&comp_rect.data[1]);
    av_freep(&comp_rect2.data[0]);
    av_freep(&comp_rect2.data[1]);
    sub_coalesce_reset(ep);
    return ret;
}

static int do_subtitle_out(OutputFile *of, OutputStream *ost, AVSubtitle *sub,
                           AVPacket *pkt)
{
    Encoder *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    int subtitle_out_max_size = 1024 * 1024;
    int subtitle_out_size, nb, i, ret;
    AVCodecContext *enc;
    int64_t pts;

    if (sub->pts == AV_NOPTS_VALUE) {
        av_log(e, AV_LOG_ERROR, "Subtitle packets must have a pts\n");
        return exit_on_error ? AVERROR(EINVAL) : 0;
    }
    if ((of->start_time != AV_NOPTS_VALUE && sub->pts < of->start_time))
        return 0;

    enc = e->enc_ctx;

    /* Coalesce overlapping text subtitle events for PGS bitmap encoding.
     * Multiple ASS Dialogue lines with the same PTS are buffered and
     * rendered as a single composite before quantization and encoding. */
    if (enc->codec_id == AV_CODEC_ID_HDMV_PGS_SUBTITLE &&
        sub->num_rects > 0 &&
        (sub->rects[0]->type == SUBTITLE_ASS ||
         sub->rects[0]->type == SUBTITLE_TEXT)) {
        const char *text = sub->rects[0]->ass ? sub->rects[0]->ass
                                               : sub->rects[0]->text;
        if (!text || !text[0])
            return 0;

        /* Flush pending events if PTS changed */
        if (ep->sub_coalesce.nb > 0 && sub->pts != ep->sub_coalesce.pts) {
            ret = flush_coalesced_subtitles(of, ost, pkt);
            if (ret < 0)
                return ret;
        }

        return sub_coalesce_append(ep, text,
                    sub->end_display_time - sub->start_display_time,
                    sub->pts,
                    sub->start_display_time,
                    sub->end_display_time);
    }

    /* Flush any pending coalesced events before non-coalesced processing */
    ret = flush_coalesced_subtitles(of, ost, pkt);
    if (ret < 0)
        return ret;

    /* Convert text subtitles to bitmap if encoder requires it */
    ret = convert_text_to_bitmap(ep, ost, sub);
    if (ret < 0)
        return ret;

    /* Note: DVB subtitle need one packet to draw them and one other
       packet to clear them */
    /* XXX: signal it in the codec context ? */
    if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE)
        nb = 2;
    else if (enc->codec_id == AV_CODEC_ID_ASS)
        nb = FFMAX(sub->num_rects, 1);
    else
        nb = 1;

    /* shift timestamp to honor -ss and make check_recording_time() work with -t */
    pts = sub->pts;
    if (of->start_time != AV_NOPTS_VALUE)
        pts -= of->start_time;
    for (i = 0; i < nb; i++) {
        AVSubtitle local_sub = *sub;

        if (!check_recording_time(ost, pts, AV_TIME_BASE_Q))
            return AVERROR_EOF;

        ret = av_new_packet(pkt, subtitle_out_max_size);
        if (ret < 0)
            return AVERROR(ENOMEM);

        local_sub.pts = pts;
        // start_display_time is required to be 0
        local_sub.pts               += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, AV_TIME_BASE_Q);
        local_sub.end_display_time  -= sub->start_display_time;
        local_sub.start_display_time = 0;

        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE && i == 1)
            local_sub.num_rects = 0;
        else if (enc->codec_id == AV_CODEC_ID_ASS && sub->num_rects > 0) {
            local_sub.num_rects = 1;
            local_sub.rects += i;
        }

        e->frames_encoded++;

        subtitle_out_size = avcodec_encode_subtitle(enc, pkt->data, pkt->size, &local_sub);
        if (subtitle_out_size < 0) {
            av_log(e, AV_LOG_FATAL, "Subtitle encoding failed\n");
            return subtitle_out_size;
        }

        av_shrink_packet(pkt, subtitle_out_size);
        pkt->time_base = AV_TIME_BASE_Q;
        pkt->pts       = sub->pts;
        pkt->duration = av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
        if (enc->codec_id == AV_CODEC_ID_DVB_SUBTITLE) {
            /* XXX: the pts correction is handled here. Maybe handling
               it in the codec would be better */
            if (i == 0)
                pkt->pts += av_rescale_q(sub->start_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
            else
                pkt->pts += av_rescale_q(sub->end_display_time, (AVRational){ 1, 1000 }, pkt->time_base);
        }
        pkt->dts = pkt->pts;

        ret = sch_enc_send(ep->sch, ep->sch_idx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

    return 0;
}

void enc_stats_write(OutputStream *ost, EncStats *es,
                     const AVFrame *frame, const AVPacket *pkt,
                     uint64_t frame_num)
{
    Encoder      *e = ost->enc;
    EncoderPriv *ep = ep_from_enc(e);
    AVIOContext *io = es->io;
    AVRational   tb = frame ? frame->time_base : pkt->time_base;
    int64_t     pts = frame ? frame->pts : pkt->pts;

    AVRational  tbi = (AVRational){ 0, 1};
    int64_t    ptsi = INT64_MAX;

    const FrameData *fd = NULL;

    if (frame ? frame->opaque_ref : pkt->opaque_ref) {
        fd   = (const FrameData*)(frame ? frame->opaque_ref->data : pkt->opaque_ref->data);
        tbi  = fd->dec.tb;
        ptsi = fd->dec.pts;
    }

    pthread_mutex_lock(&es->lock);

    for (size_t i = 0; i < es->nb_components; i++) {
        const EncStatsComponent *c = &es->components[i];

        switch (c->type) {
        case ENC_STATS_LITERAL:         avio_write (io, c->str,     c->str_len);                    continue;
        case ENC_STATS_FILE_IDX:        avio_printf(io, "%d",       ost->file->index);              continue;
        case ENC_STATS_STREAM_IDX:      avio_printf(io, "%d",       ost->index);                    continue;
        case ENC_STATS_TIMEBASE:        avio_printf(io, "%d/%d",    tb.num, tb.den);                continue;
        case ENC_STATS_TIMEBASE_IN:     avio_printf(io, "%d/%d",    tbi.num, tbi.den);              continue;
        case ENC_STATS_PTS:             avio_printf(io, "%"PRId64,  pts);                           continue;
        case ENC_STATS_PTS_IN:          avio_printf(io, "%"PRId64,  ptsi);                          continue;
        case ENC_STATS_PTS_TIME:        avio_printf(io, "%g",       pts * av_q2d(tb));              continue;
        case ENC_STATS_PTS_TIME_IN:     avio_printf(io, "%g",       ptsi == INT64_MAX ?
                                                                    INFINITY : ptsi * av_q2d(tbi)); continue;
        case ENC_STATS_FRAME_NUM:       avio_printf(io, "%"PRIu64,  frame_num);                     continue;
        case ENC_STATS_FRAME_NUM_IN:    avio_printf(io, "%"PRIu64,  fd ? fd->dec.frame_num : -1);   continue;
        }

        if (frame) {
            switch (c->type) {
            case ENC_STATS_SAMPLE_NUM:  avio_printf(io, "%"PRIu64,  e->samples_encoded);            continue;
            case ENC_STATS_NB_SAMPLES:  avio_printf(io, "%d",       frame->nb_samples);             continue;
            default: av_assert0(0);
            }
        } else {
            switch (c->type) {
            case ENC_STATS_DTS:         avio_printf(io, "%"PRId64,  pkt->dts);                      continue;
            case ENC_STATS_DTS_TIME:    avio_printf(io, "%g",       pkt->dts * av_q2d(tb));         continue;
            case ENC_STATS_PKT_SIZE:    avio_printf(io, "%d",       pkt->size);                     continue;
            case ENC_STATS_KEYFRAME:    avio_write(io, (pkt->flags & AV_PKT_FLAG_KEY) ?
                                                       "K" : "N", 1);                               continue;
            case ENC_STATS_BITRATE: {
                double duration = FFMAX(pkt->duration, 1) * av_q2d(tb);
                avio_printf(io, "%g",  8.0 * pkt->size / duration);
                continue;
            }
            case ENC_STATS_AVG_BITRATE: {
                double duration = pkt->dts * av_q2d(tb);
                avio_printf(io, "%g",  duration > 0 ? 8.0 * ep->data_size / duration : -1.);
                continue;
            }
            default: av_assert0(0);
            }
        }
    }
    avio_w8(io, '\n');
    avio_flush(io);

    pthread_mutex_unlock(&es->lock);
}

static inline double psnr(double d)
{
    return -10.0 * log10(d);
}

static int update_video_stats(OutputStream *ost, const AVPacket *pkt, int write_vstats)
{
    Encoder        *e = ost->enc;
    EncoderPriv   *ep = ep_from_enc(e);
    const uint8_t *sd = av_packet_get_side_data(pkt, AV_PKT_DATA_QUALITY_STATS,
                                                NULL);
    AVCodecContext *enc = e->enc_ctx;
    enum AVPictureType pict_type;
    int64_t frame_number;
    double ti1, bitrate, avg_bitrate;
    double psnr_val = -1;
    int quality;

    quality        = sd ? AV_RL32(sd) : -1;
    pict_type      = sd ? sd[4] : AV_PICTURE_TYPE_NONE;

    atomic_store(&ost->quality, quality);

    if ((enc->flags & AV_CODEC_FLAG_PSNR) && sd && sd[5]) {
        // FIXME the scaling assumes 8bit
        double error = AV_RL64(sd + 8) / (enc->width * enc->height * 255.0 * 255.0);
        if (error >= 0 && error <= 1)
            psnr_val = psnr(error);
    }

    if (!write_vstats)
        return 0;

    /* this is executed just the first time update_video_stats is called */
    if (!vstats_file) {
        vstats_file = fopen(vstats_filename, "w");
        if (!vstats_file) {
            perror("fopen");
            return AVERROR(errno);
        }
    }

    frame_number = ep->packets_encoded;
    if (vstats_version <= 1) {
        fprintf(vstats_file, "frame= %5"PRId64" q= %2.1f ", frame_number,
                quality / (float)FF_QP2LAMBDA);
    } else  {
        fprintf(vstats_file, "out= %2d st= %2d frame= %5"PRId64" q= %2.1f ",
                ost->file->index, ost->index, frame_number,
                quality / (float)FF_QP2LAMBDA);
    }

    if (psnr_val >= 0)
        fprintf(vstats_file, "PSNR= %6.2f ", psnr_val);

    fprintf(vstats_file,"f_size= %6d ", pkt->size);
    /* compute pts value */
    ti1 = pkt->dts * av_q2d(pkt->time_base);
    if (ti1 < 0.01)
        ti1 = 0.01;

    bitrate     = (pkt->size * 8) / av_q2d(enc->time_base) / 1000.0;
    avg_bitrate = (double)(ep->data_size * 8) / ti1 / 1000.0;
    fprintf(vstats_file, "s_size= %8.0fKiB time= %0.3f br= %7.1fkbits/s avg_br= %7.1fkbits/s ",
           (double)ep->data_size / 1024, ti1, bitrate, avg_bitrate);
    fprintf(vstats_file, "type= %c\n", av_get_picture_type_char(pict_type));

    return 0;
}

static int encode_frame(OutputFile *of, OutputStream *ost, AVFrame *frame,
                        AVPacket *pkt)
{
    Encoder            *e = ost->enc;
    EncoderPriv       *ep = ep_from_enc(e);
    AVCodecContext   *enc = e->enc_ctx;
    const char *type_desc = av_get_media_type_string(enc->codec_type);
    const char    *action = frame ? "encode" : "flush";
    int ret;

    if (frame) {
        FrameData *fd = frame_data(frame);

        if (!fd)
            return AVERROR(ENOMEM);

        fd->wallclock[LATENCY_PROBE_ENC_PRE] = av_gettime_relative();

        if (ost->enc_stats_pre.io)
            enc_stats_write(ost, &ost->enc_stats_pre, frame, NULL,
                            e->frames_encoded);

        e->frames_encoded++;
        e->samples_encoded += frame->nb_samples;

        if (debug_ts) {
            av_log(e, AV_LOG_INFO, "encoder <- type:%s "
                   "frame_pts:%s frame_pts_time:%s time_base:%d/%d\n",
                   type_desc,
                   av_ts2str(frame->pts), av_ts2timestr(frame->pts, &enc->time_base),
                   enc->time_base.num, enc->time_base.den);
        }

        if (frame->sample_aspect_ratio.num && !ost->frame_aspect_ratio.num)
            enc->sample_aspect_ratio = frame->sample_aspect_ratio;
    }

    update_benchmark(NULL);

    ret = avcodec_send_frame(enc, frame);
    if (ret < 0 && !(ret == AVERROR_EOF && !frame)) {
        av_log(e, AV_LOG_ERROR, "Error submitting %s frame to the encoder\n",
               type_desc);
        return ret;
    }

    while (1) {
        FrameData *fd;

        av_packet_unref(pkt);

        ret = avcodec_receive_packet(enc, pkt);
        update_benchmark("%s_%s %d.%d", action, type_desc,
                         of->index, ost->index);

        pkt->time_base = enc->time_base;

        /* if two pass, output log on success and EOF */
        if ((ret >= 0 || ret == AVERROR_EOF) && ost->logfile && enc->stats_out)
            fprintf(ost->logfile, "%s", enc->stats_out);

        if (ret == AVERROR(EAGAIN)) {
            av_assert0(frame); // should never happen during flushing
            return 0;
        } else if (ret < 0) {
            if (ret != AVERROR_EOF)
                av_log(e, AV_LOG_ERROR, "%s encoding failed\n", type_desc);
            return ret;
        }

        fd = packet_data(pkt);
        if (!fd)
            return AVERROR(ENOMEM);
        fd->wallclock[LATENCY_PROBE_ENC_POST] = av_gettime_relative();

        // attach stream parameters to first packet if requested
        avcodec_parameters_free(&fd->par_enc);
        if (ep->attach_par && !ep->packets_encoded) {
            fd->par_enc = avcodec_parameters_alloc();
            if (!fd->par_enc)
                return AVERROR(ENOMEM);

            ret = avcodec_parameters_from_context(fd->par_enc, enc);
            if (ret < 0)
                return ret;
        }

        pkt->flags |= AV_PKT_FLAG_TRUSTED;

        if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
            ret = update_video_stats(ost, pkt, !!vstats_filename);
            if (ret < 0)
                return ret;
        }

        if (ost->enc_stats_post.io)
            enc_stats_write(ost, &ost->enc_stats_post, NULL, pkt,
                            ep->packets_encoded);

        if (debug_ts) {
            av_log(e, AV_LOG_INFO, "encoder -> type:%s "
                   "pkt_pts:%s pkt_pts_time:%s pkt_dts:%s pkt_dts_time:%s "
                   "duration:%s duration_time:%s\n",
                   type_desc,
                   av_ts2str(pkt->pts), av_ts2timestr(pkt->pts, &enc->time_base),
                   av_ts2str(pkt->dts), av_ts2timestr(pkt->dts, &enc->time_base),
                   av_ts2str(pkt->duration), av_ts2timestr(pkt->duration, &enc->time_base));
        }

        ep->data_size += pkt->size;

        ep->packets_encoded++;

        ret = sch_enc_send(ep->sch, ep->sch_idx, pkt);
        if (ret < 0) {
            av_packet_unref(pkt);
            return ret;
        }
    }

    av_unreachable("encode_frame() loop should return");
}

static enum AVPictureType forced_kf_apply(void *logctx, KeyframeForceCtx *kf,
                                          const AVFrame *frame)
{
    double pts_time;

    if (kf->ref_pts == AV_NOPTS_VALUE)
        kf->ref_pts = frame->pts;

    pts_time = (frame->pts - kf->ref_pts) * av_q2d(frame->time_base);
    if (kf->index < kf->nb_pts &&
        av_compare_ts(frame->pts, frame->time_base, kf->pts[kf->index], AV_TIME_BASE_Q) >= 0) {
        kf->index++;
        goto force_keyframe;
    } else if (kf->pexpr) {
        double res;
        kf->expr_const_values[FKF_T] = pts_time;
        res = av_expr_eval(kf->pexpr,
                           kf->expr_const_values, NULL);
        av_log(logctx, AV_LOG_TRACE,
               "force_key_frame: n:%f n_forced:%f prev_forced_n:%f t:%f prev_forced_t:%f -> res:%f\n",
               kf->expr_const_values[FKF_N],
               kf->expr_const_values[FKF_N_FORCED],
               kf->expr_const_values[FKF_PREV_FORCED_N],
               kf->expr_const_values[FKF_T],
               kf->expr_const_values[FKF_PREV_FORCED_T],
               res);

        kf->expr_const_values[FKF_N] += 1;

        if (res) {
            kf->expr_const_values[FKF_PREV_FORCED_N] = kf->expr_const_values[FKF_N] - 1;
            kf->expr_const_values[FKF_PREV_FORCED_T] = kf->expr_const_values[FKF_T];
            kf->expr_const_values[FKF_N_FORCED]     += 1;
            goto force_keyframe;
        }
    } else if (kf->type == KF_FORCE_SOURCE && (frame->flags & AV_FRAME_FLAG_KEY)) {
        goto force_keyframe;
    } else if (kf->type == KF_FORCE_SCD_METADATA &&
               av_dict_get(frame->metadata, "lavfi.scd.time", NULL, 0)) {
        goto force_keyframe;
    }

    return AV_PICTURE_TYPE_NONE;

force_keyframe:
    av_log(logctx, AV_LOG_DEBUG, "Forced keyframe at time %f\n", pts_time);
    return AV_PICTURE_TYPE_I;
}

static int frame_encode(OutputStream *ost, AVFrame *frame, AVPacket *pkt)
{
    Encoder *e = ost->enc;
    OutputFile *of = ost->file;
    enum AVMediaType type = ost->type;

    if (type == AVMEDIA_TYPE_SUBTITLE) {
        AVSubtitle *subtitle = frame && frame->buf[0] ?
                               (AVSubtitle*)frame->buf[0]->data : NULL;

        if (subtitle && subtitle->num_rects)
            return do_subtitle_out(of, ost, subtitle, pkt);

        /* End of stream: flush any pending coalesced events */
        return flush_coalesced_subtitles(of, ost, pkt);
    }

    if (frame) {
        if (!check_recording_time(ost, frame->pts, frame->time_base))
            return AVERROR_EOF;

        if (type == AVMEDIA_TYPE_VIDEO) {
            frame->quality   = e->enc_ctx->global_quality;
            frame->pict_type = forced_kf_apply(e, &ost->kf, frame);

#if FFMPEG_OPT_TOP
            if (ost->top_field_first >= 0) {
                frame->flags &= ~AV_FRAME_FLAG_TOP_FIELD_FIRST;
                frame->flags |= AV_FRAME_FLAG_TOP_FIELD_FIRST * (!!ost->top_field_first);
            }
#endif
        } else {
            if (!(e->enc_ctx->codec->capabilities & AV_CODEC_CAP_PARAM_CHANGE) &&
                e->enc_ctx->ch_layout.nb_channels != frame->ch_layout.nb_channels) {
                av_log(e, AV_LOG_ERROR,
                       "Audio channel count changed and encoder does not support parameter changes\n");
                return 0;
            }
        }
    }

    return encode_frame(of, ost, frame, pkt);
}

static void enc_thread_set_name(const OutputStream *ost)
{
    char name[16];
    snprintf(name, sizeof(name), "enc%d:%d:%s", ost->file->index, ost->index,
             ost->enc->enc_ctx->codec->name);
    ff_thread_setname(name);
}

static void enc_thread_uninit(EncoderThread *et)
{
    av_packet_free(&et->pkt);
    av_frame_free(&et->frame);

    memset(et, 0, sizeof(*et));
}

static int enc_thread_init(EncoderThread *et)
{
    memset(et, 0, sizeof(*et));

    et->frame = av_frame_alloc();
    if (!et->frame)
        goto fail;

    et->pkt = av_packet_alloc();
    if (!et->pkt)
        goto fail;

    return 0;

fail:
    enc_thread_uninit(et);
    return AVERROR(ENOMEM);
}

int encoder_thread(void *arg)
{
    OutputStream *ost = arg;
    Encoder        *e = ost->enc;
    EncoderPriv   *ep = ep_from_enc(e);
    EncoderThread et;
    int ret = 0, input_status = 0;
    int name_set = 0;

    ret = enc_thread_init(&et);
    if (ret < 0)
        goto finish;

    /* Open the subtitle encoders immediately. AVFrame-based encoders
     * are opened through a callback from the scheduler once they get
     * their first frame
     *
     * N.B.: because the callback is called from a different thread,
     * enc_ctx MUST NOT be accessed before sch_enc_receive() returns
     * for the first time for audio/video. */
    if (ost->type != AVMEDIA_TYPE_VIDEO && ost->type != AVMEDIA_TYPE_AUDIO) {
        ret = enc_open(ost, NULL);
        if (ret < 0)
            goto finish;
    }

    while (!input_status) {
        input_status = sch_enc_receive(ep->sch, ep->sch_idx, et.frame);
        if (input_status < 0) {
            if (input_status == AVERROR_EOF) {
                av_log(e, AV_LOG_VERBOSE, "Encoder thread received EOF\n");
                if (ep->opened)
                    break;

                av_log(e, AV_LOG_ERROR, "Could not open encoder before EOF\n");
                ret = AVERROR(EINVAL);
            } else {
                av_log(e, AV_LOG_ERROR, "Error receiving a frame for encoding: %s\n",
                       av_err2str(ret));
                ret = input_status;
            }
            goto finish;
        }

        if (!name_set) {
            enc_thread_set_name(ost);
            name_set = 1;
        }

        ret = frame_encode(ost, et.frame, et.pkt);

        av_packet_unref(et.pkt);
        av_frame_unref(et.frame);

        if (ret < 0) {
            if (ret == AVERROR_EOF)
                av_log(e, AV_LOG_VERBOSE, "Encoder returned EOF, finishing\n");
            else
                av_log(e, AV_LOG_ERROR, "Error encoding a frame: %s\n",
                       av_err2str(ret));
            break;
        }
    }

    // flush the encoder
    if (ret == 0 || ret == AVERROR_EOF) {
        ret = frame_encode(ost, NULL, et.pkt);
        if (ret < 0 && ret != AVERROR_EOF)
            av_log(e, AV_LOG_ERROR, "Error flushing encoder: %s\n",
                   av_err2str(ret));
    }

    // EOF is normal thread termination
    if (ret == AVERROR_EOF)
        ret = 0;

finish:
    enc_thread_uninit(&et);

    return ret;
}

int enc_loopback(Encoder *enc)
{
    EncoderPriv *ep = ep_from_enc(enc);
    ep->attach_par = 1;
    return ep->sch_idx;
}
