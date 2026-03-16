/*
 * Palette mapping internals
 *
 * Copyright (c) 2015 Stupeflix
 * Copyright (c) 2022 Clément Bœsch <u pkh me>
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

#ifndef AVUTIL_PALETTEMAP_INTERNAL_H
#define AVUTIL_PALETTEMAP_INTERNAL_H

#include <stdint.h>

struct FFColorInfo {
    uint32_t srgb;
    int32_t lab[3];
};

struct FFColorNode {
    struct FFColorInfo c;
    uint8_t palette_id;
    int split;
    int left_id, right_id;
};

#define FF_PALETTE_CACHE_SIZE (1 << 15)

struct FFCachedColor {
    uint32_t color;
    uint8_t pal_entry;
};

struct FFCacheNode {
    struct FFCachedColor *entries;
    int nb_entries;
};

#endif /* AVUTIL_PALETTEMAP_INTERNAL_H */
