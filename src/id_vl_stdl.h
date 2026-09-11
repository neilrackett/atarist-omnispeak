/*
Omnispeak: A Commander Keen Reimplementation
Atari STE video backend on STDL
Copyright (C) 2026 Neil Rackett
SPDX-License-Identifier: GPL-2.0-or-later

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#ifndef ID_VL_STDL_H
#define ID_VL_STDL_H

#include <stdint.h>

// The STDL backend keeps 16x16 tiles, bitmaps, masked bitmaps and sprites
// in ST interleaved planar format, converted once when the cache manager
// expands the chunk (see CAL_ExpandGrChunk). The conversions below are the
// contract between id_ca.c and id_vl_stdl.c:
//
//  - Unmasked: rows of ((bw + 1) / 2) groups, each group four plane words
//    [p0 p1 p2 p3]; the low byte of a padding group (odd bw) is zero.
//  - Masked: rows of ((bw + 1) / 2) groups of five words [mask p0 p1 p2 p3],
//    mask bit set = destination preserved; padding is masked off (0xFF).
//
// 8x8 tiles, fonts and the intro's 1bpp pictures stay in EGA format.
// bw is the width in EGA bytes (8 pixels per byte), as in the headers.

void VL_STDL_ConvertUnmasked(const uint8_t *src, uint8_t *dst, int bw, int h);
void VL_STDL_ConvertMasked(const uint8_t *src, uint8_t *dst, int bw, int h);

// Bytes needed by the converted forms.
#define VL_STDL_UNMASKED_SIZE(bw, h) ((((bw) + 1) >> 1) * 8 * (h))
#define VL_STDL_MASKED_SIZE(bw, h) ((((bw) + 1) >> 1) * 10 * (h))

// Shift a converted masked sprite of bw bytes right by px (2, 4 or 6)
// pixels into a converted masked sprite of bw + 1 bytes: the ST twin of
// CAL_ShiftSprite.
void VL_STDL_ShiftSprite(const uint8_t *src, uint8_t *dst, int bw, int h, int px);

// Unpack one plane of a converted unmasked bitmap into 1bpp rows of bw
// bytes, for code which reads EGA planes as monochrome pictures.
void VL_STDL_ExtractPlane(const uint8_t *src, uint8_t *dst, int bw, int h, int plane);

// Write one EGA-format 8x8 tile (32 bytes, plane-major) into a converted
// masked sprite at byte column col, row row, as opaque pixels.
void VL_STDL_PokeTile8(uint8_t *sprite, int bw, int col, int row, const uint8_t *tile);

#endif
