/*
 * Stub for bitmap-to-text subtitle conversion (Series E).
 * Provides no-op implementations so Series D compiles without OCR.
 */

#ifndef FFMPEG_DEC_SUB_H
#define FFMPEG_DEC_SUB_H

#include "ffmpeg.h"

typedef struct SubtitleDecContext SubtitleDecContext;

static inline SubtitleDecContext *dec_sub_alloc(void *sch, unsigned idx)
{
    (void)sch; (void)idx;
    return NULL;
}

static inline void dec_sub_free(SubtitleDecContext **ctx)
{
    (void)ctx;
}

static inline void dec_sub_set_options(SubtitleDecContext *ctx,
                                       const char *lang,
                                       const char *datapath,
                                       int pageseg_mode,
                                       int min_duration)
{
    (void)ctx; (void)lang; (void)datapath;
    (void)pageseg_mode; (void)min_duration;
}

static inline int dec_sub_process(SubtitleDecContext *ctx,
                                  OutputFile *of, OutputStream *ost,
                                  AVSubtitle *sub, AVPacket *pkt)
{
    (void)ctx; (void)of; (void)ost; (void)sub; (void)pkt;
    return 0;
}

static inline int dec_sub_flush(SubtitleDecContext *ctx,
                                OutputFile *of, OutputStream *ost,
                                AVPacket *pkt, int64_t pts)
{
    (void)ctx; (void)of; (void)ost; (void)pkt; (void)pts;
    return 0;
}

#endif /* FFMPEG_DEC_SUB_H */
