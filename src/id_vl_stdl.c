/*
Omnispeak: A Commander Keen Reimplementation
Atari STE video backend on STDL
Copyright (C) 2026 Neil Rackett

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

SPDX-License-Identifier: GPL-2.0-or-later
 */

// The Atari STE video backend. It follows id_vl_dos.c: the screen is a
// 336x224 pair of pages that the game scrolls by moving their base
// pointers, and the hardware shows a 320x200 window of the active one
// through the STE's video base, LINEWIDTH and HSCROLL registers (STDL's
// hardware-scroll module). Surfaces are ST interleaved planar - four
// consecutive plane words per 16 pixels - so 16x16 tiles, bitmaps and
// sprites are converted once at cache time (id_vl_stdl.h) and blitted
// with word operations; 8x8 tiles, fonts and the intro's monochrome
// pictures stay in EGA format and go through slower generic paths.

#include "id_us.h"
#include "id_vl.h"
#include "id_vl_private.h"
#include "id_vl_stdl.h"
#include "id_sd.h"
#include "ck_cross.h"

#include <stdlib.h>
#include <string.h>

#include <stdl/stdl.h>

// mintlib: stack reserved by crt0 before the heap takes the rest of the
// TPA. The default is too small for the level loader's temporaries.
long _stksize = 64 * 1024;

// Each front-buffer page gets this much slack either side of its pixels,
// so that scrolling can move the page's base pointer instead of the
// page. When the pointer runs off one end the page is copied back to the
// other (once per 32K of travel, about twelve tile rows).
#define VL_STDL_SLACK 16384

typedef struct VL_STDL_Surface
{
	VL_SurfaceUsage use;
	int w, h;
	int groups, stride;
	// page[1] and the blocks exist for the front buffer only. A page's
	// pixel pointer drifts inside its block between lo and hi.
	STDL_Surface *page[2];
	uint8_t *block[2];
	uint8_t *lo[2];
	// Set for a surface big enough to be worth scrolling by moving its
	// origin rather than its contents (the tile buffer). page[0]'s
	// pixels drift inside block[0] between lo[0] and lo[0] + 2 * SLACK.
	bool canDrift;
	// (the top of a page's travel is always lo + 2 * VL_STDL_SLACK)
	int activePage;
} VL_STDL_Surface;

// STDL's own screen surface: the palette registers are programmed
// through it, nothing is ever drawn on it.
static STDL_Surface *vl_stdl_screen;

// Set by present when a page flip has been requested: the page we are
// about to draw into may still be on screen until STDL has programmed
// the new window, so the first write to the front buffer waits for it.
static bool vl_stdl_flipPending;

#ifdef CK_STDL_PROFILE
#define VL_STDL_COUNTBLIT() (vl_stdl_blits++)
#else
#define VL_STDL_COUNTBLIT() ((void)0)
#endif

#ifdef CK_STDL_PROFILE
// Frame-rate and blit-count trace.
//
// STDL_GetTicks is milliseconds; it and STDL_GetHz200 are the same
// counter (GetTicks is (hz200 - base) * 5), and neither is affected by
// an emulator's fast-forward. Milliseconds are used here only because
// the arithmetic is harder to get wrong: a 200Hz tick is 5ms, so a rate
// computed from a tick delta as though it were seconds reads five times
// high. That error is believed to be why this port was thought to run
// at 28fps when it ran at 6.
//
// Two guards against believing a wrong number. The game clock is
// printed beside the frame rate: it is 70Hz by design, so any error in
// the time scale shows up there immediately. And the blit count is a
// plain integer that no clock can distort - a frame claiming 200ms
// while issuing four blits says the cost is not where you think.
uint32_t vl_stdl_blits;
#endif



static void VL_STDL_WaitFlip(void)
{
	while (STDL_ScrollWindowPending())
	{
		STDL_WaitVBL();
	}
	vl_stdl_flipPending = false;
}

// Called before writing to a surface.
static inline void VL_STDL_Writable(VL_STDL_Surface *surf)
{
	if (surf->use == VL_SurfaceUsage_FrontBuffer && vl_stdl_flipPending)
		VL_STDL_WaitFlip();
}

// The page drawing goes to (the DOS backend's activePage ? data2 : data).
static inline STDL_Surface *VL_STDL_Target(VL_STDL_Surface *surf)
{
	return surf->page[surf->use == VL_SurfaceUsage_FrontBuffer ? surf->activePage : 0];
}

// The page on screen.
static inline int VL_STDL_ShownPage(VL_STDL_Surface *surf)
{
	return surf->activePage ^ 1;
}

// ===================================================================
// Format conversion (contract in id_vl_stdl.h)
// ===================================================================

void VL_STDL_ConvertUnmasked(const uint8_t *src, uint8_t *dst, int bw, int h)
{
	int groups = (bw + 1) >> 1;
	int planeSize = bw * h;
	uint16_t *d = (uint16_t *)dst;
	for (int row = 0; row < h; ++row)
	{
		const uint8_t *s = src + row * bw;
		for (int g = 0; g < groups; ++g)
		{
			int b = g * 2;
			bool full = (b + 1 < bw);
			for (int p = 0; p < 4; ++p)
			{
				const uint8_t *sp = s + p * planeSize + b;
				*d++ = (uint16_t)((sp[0] << 8) | (full ? sp[1] : 0));
			}
		}
	}
}

void VL_STDL_ConvertMasked(const uint8_t *src, uint8_t *dst, int bw, int h)
{
	int groups = (bw + 1) >> 1;
	int planeSize = bw * h;
	uint16_t *d = (uint16_t *)dst;
	for (int row = 0; row < h; ++row)
	{
		const uint8_t *s = src + row * bw;
		for (int g = 0; g < groups; ++g)
		{
			int b = g * 2;
			bool full = (b + 1 < bw);
			// The mask plane comes first in EGA masked data too.
			*d++ = (uint16_t)((s[b] << 8) | (full ? s[b + 1] : 0xFF));
			for (int p = 1; p < 5; ++p)
			{
				const uint8_t *sp = s + p * planeSize + b;
				*d++ = (uint16_t)((sp[0] << 8) | (full ? sp[1] : 0));
			}
		}
	}
}

void VL_STDL_ShiftSprite(const uint8_t *src, uint8_t *dst, int bw, int h, int px)
{
	int sgroups = (bw + 1) >> 1;
	int dgroups = (bw + 2) >> 1;
	const uint16_t *s = (const uint16_t *)src;
	uint16_t *d = (uint16_t *)dst;

	// Each destination word is a 32-bit window over two source words
	// shifted right by px; beyond the source, masks read as all-preserve
	// and planes as zero. The row is walked word by word with a carried
	// previous word, one 32-bit shift per word, no per-word tests.
	for (int row = 0; row < h; ++row)
	{
		const uint16_t *sp = s;
		uint16_t m = 0xFFFF, p0 = 0, p1 = 0, p2 = 0, p3 = 0;
		for (int g = 0; g < dgroups; ++g)
		{
			uint16_t cm = 0xFFFF, c0 = 0, c1 = 0, c2 = 0, c3 = 0;
			if (g < sgroups)
			{
				cm = sp[0];
				c0 = sp[1];
				c1 = sp[2];
				c2 = sp[3];
				c3 = sp[4];
				sp += 5;
			}
			d[0] = (uint16_t)((((uint32_t)m << 16) | cm) >> px);
			d[1] = (uint16_t)((((uint32_t)p0 << 16) | c0) >> px);
			d[2] = (uint16_t)((((uint32_t)p1 << 16) | c1) >> px);
			d[3] = (uint16_t)((((uint32_t)p2 << 16) | c2) >> px);
			d[4] = (uint16_t)((((uint32_t)p3 << 16) | c3) >> px);
			d += 5;
			m = cm;
			p0 = c0;
			p1 = c1;
			p2 = c2;
			p3 = c3;
		}
		s += sgroups * 5;
	}
}

void VL_STDL_ExtractPlane(const uint8_t *src, uint8_t *dst, int bw, int h, int plane)
{
	int groups = (bw + 1) >> 1;
	const uint16_t *s = (const uint16_t *)src;
	for (int row = 0; row < h; ++row)
	{
		for (int b = 0; b < bw; ++b)
		{
			uint16_t word = s[(b >> 1) * 4 + plane];
			*dst++ = (b & 1) ? (uint8_t)word : (uint8_t)(word >> 8);
		}
		s += groups * 4;
	}
}

void VL_STDL_PokeTile8(uint8_t *sprite, int bw, int col, int row, const uint8_t *tile)
{
	int groups = (bw + 1) >> 1;
	int half = col & 1;
	for (int r = 0; r < 8; ++r)
	{
		uint8_t *g = sprite + ((row + r) * groups + (col >> 1)) * 10 + half;
		g[0] = 0; // opaque
		g[2] = tile[r];
		g[4] = tile[8 + r];
		g[6] = tile[16 + r];
		g[8] = tile[24 + r];
	}
}

// ===================================================================
// Backend: mode and surfaces
// ===================================================================

static void VL_STDL_SetVideoMode(int mode)
{
	if (mode == 0xD)
	{
		if (STDL_Init(STDL_INIT_VIDEO | STDL_INIT_JOYSTICK) < 0)
			QuitF("STDL_Init failed: %s", STDL_GetError());
		vl_stdl_screen = STDL_SetVideoMode(320, 200, 4, 0);
		if (!vl_stdl_screen)
			QuitF("STDL_SetVideoMode failed: %s", STDL_GetError());
		if (!STDL_HasHwScroll())
			Quit("Omnispeak needs an Atari STE or Mega STE: the plain ST has no hardware scrolling.");
	}
	else
	{
		STDL_ResetScrollWindow();
		STDL_Quit();
		vl_stdl_screen = 0;
	}
}

static void *VL_STDL_CreateSurface(int w, int h, VL_SurfaceUsage usage)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)calloc(1, sizeof(VL_STDL_Surface));
	if (!surf)
		Quit("VL_STDL_CreateSurface: out of memory");
	surf->use = usage;
	surf->w = w;
	surf->h = h;
	surf->groups = (w + 15) >> 4;
	surf->stride = surf->groups * 8;

	if (usage == VL_SurfaceUsage_FrontBuffer)
	{
		size_t pageBytes = (size_t)surf->stride * h;
		for (int p = 0; p < 2; ++p)
		{
			// The block is a whole page plus slack either side, aligned
			// to a group so drifted pointers stay group aligned.
			size_t blockBytes = pageBytes + 2 * VL_STDL_SLACK + 16;
			surf->block[p] = (uint8_t *)malloc(blockBytes);
			if (!surf->block[p])
				Quit("VL_STDL_CreateSurface: out of memory for a screen page");
			memset(surf->block[p], 0, blockBytes);
			surf->lo[p] = (uint8_t *)(((uintptr_t)surf->block[p] + 7) & ~(uintptr_t)7);
			surf->page[p] = STDL_CreateSurfaceFrom(surf->lo[p] + VL_STDL_SLACK, w, h, surf->stride, NULL, 0);
			if (!surf->page[p])
				QuitF("VL_STDL_CreateSurface: %s", STDL_GetError());
		}
	}
	else if (usage != VL_SurfaceUsage_Sprite && w >= 320 && h >= 200)
	{
		// A full-screen-sized offscreen surface is the refresh manager's
		// tile buffer, which the engine scrolls a tile at a time by
		// asking us to copy it onto itself. That is 37KB of memory
		// traffic per tile of scroll and it is bandwidth-bound, so it
		// gets the same slack the screen pages have and the copy becomes
		// a move of the origin. Anything smaller is a sprite or a
		// bitmap, never scrolled, and is not worth the slack.
		size_t pageBytes = (size_t)surf->stride * h;
		size_t blockBytes = pageBytes + 2 * VL_STDL_SLACK + 16;
		surf->block[0] = (uint8_t *)malloc(blockBytes);
		if (!surf->block[0])
			Quit("VL_STDL_CreateSurface: out of memory for a scroll buffer");
		memset(surf->block[0], 0, blockBytes);
		surf->lo[0] = (uint8_t *)(((uintptr_t)surf->block[0] + 7) & ~(uintptr_t)7);
		surf->page[0] = STDL_CreateSurfaceFrom(surf->lo[0] + VL_STDL_SLACK, w, h, surf->stride, NULL, 0);
		if (!surf->page[0])
			QuitF("VL_STDL_CreateSurface: %s", STDL_GetError());
		surf->canDrift = true;
	}
	else
	{
		surf->page[0] = STDL_CreateSurface(w, h);
		if (!surf->page[0])
			QuitF("VL_STDL_CreateSurface: %s", STDL_GetError());
	}
	return surf;
}

static void VL_STDL_DestroySurface(void *surface)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	if (surf->use == VL_SurfaceUsage_FrontBuffer)
	{
		// The display may still point into these pages.
		STDL_ResetScrollWindow();
		vl_stdl_flipPending = false;
	}
	for (int p = 0; p < 2; ++p)
	{
		if (surf->page[p])
			STDL_FreeSurface(surf->page[p]);
		free(surf->block[p]);
	}
	free(surf);
}

static long VL_STDL_GetSurfaceMemUse(void *surface)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	long page = (long)surf->stride * surf->h;
	return (surf->use == VL_SurfaceUsage_FrontBuffer) ? 2 * (page + 2 * VL_STDL_SLACK) : page;
}

static void VL_STDL_GetSurfaceDimensions(void *surface, int *w, int *h)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	if (w)
		*w = surf->w;
	if (h)
		*h = surf->h;
}

static void VL_STDL_RefreshPaletteAndBorderColor(void *screen)
{
	STDL_Colour cols[16];
	(void)screen;
	if (!vl_stdl_screen)
		return;
	for (int i = 0; i < 16; ++i)
	{
		uint8_t ega = vl_emuegavgaadapter.palette[i] & 15;
		cols[i].r = VL_EGARGBColorTable[ega][0];
		cols[i].g = VL_EGARGBColorTable[ega][1];
		cols[i].b = VL_EGARGBColorTable[ega][2];
		cols[i].unused = 0;
	}
	// The ST's border is always colour 0; Keen's border colour is cosmetic.
	STDL_SetColours(vl_stdl_screen, cols, 0, 16);
}

static int VL_STDL_SurfacePGet(void *surface, int x, int y)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	if (x < 0 || y < 0 || x >= surf->w || y >= surf->h)
		return 0;
	const uint16_t *g = (const uint16_t *)(s->pixels + y * surf->stride + (x >> 4) * 8);
	int bit = 15 - (x & 15);
	return ((g[0] >> bit) & 1) | (((g[1] >> bit) & 1) << 1) | (((g[2] >> bit) & 1) << 2) | (((g[3] >> bit) & 1) << 3);
}

// ===================================================================
// Rectangles
// ===================================================================

static void VL_STDL_SurfaceRect(void *dst_surface, int x, int y, int w, int h, int colour)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	STDL_Rect r;
	VL_STDL_Writable(surf);

	// The fizzle fade and the Star Wars scroller plot single pixels.
	if (w == 1 && h == 1)
	{
		if (x < 0 || y < 0 || x >= surf->w || y >= surf->h)
			return;
		uint16_t *g = (uint16_t *)(s->pixels + y * surf->stride + (x >> 4) * 8);
		uint16_t bit = (uint16_t)(0x8000 >> (x & 15));
		for (int p = 0; p < 4; ++p)
		{
			if (colour & (1 << p))
				g[p] |= bit;
			else
				g[p] &= (uint16_t)~bit;
		}
		return;
	}
	r.x = (int16_t)x;
	r.y = (int16_t)y;
	r.w = (uint16_t)w;
	r.h = (uint16_t)h;
	STDL_FillRect(s, &r, (uint8_t)(colour & 15));
}

static void VL_STDL_SurfaceRect_PM(void *dst_surface, int x, int y, int w, int h, int colour, int mapmask)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);

	mapmask &= 0xF;
	colour &= mapmask;

	// Clip.
	if (x < 0)
	{
		w += x;
		x = 0;
	}
	if (y < 0)
	{
		h += y;
		y = 0;
	}
	if (x + w > surf->w)
		w = surf->w - x;
	if (y + h > surf->h)
		h = surf->h - y;
	if (w <= 0 || h <= 0)
		return;

	int g0 = x >> 4, g1 = (x + w - 1) >> 4;
	uint16_t lm = (uint16_t)(0xFFFF >> (x & 15));
	uint16_t rm = (uint16_t)(0xFFFF << (15 - ((x + w - 1) & 15)));
	if (g0 == g1)
		lm &= rm;

	uint8_t *row = s->pixels + y * surf->stride + g0 * 8;
	for (int _y = 0; _y < h; ++_y, row += surf->stride)
	{
		uint16_t *g = (uint16_t *)row;
		for (int gi = g0; gi <= g1; ++gi, g += 4)
		{
			uint16_t cover = (gi == g0) ? lm : (gi == g1) ? rm : 0xFFFF;
			for (int p = 0; p < 4; ++p)
			{
				if (!(mapmask & (1 << p)))
					continue;
				if (colour & (1 << p))
					g[p] |= cover;
				else
					g[p] &= (uint16_t)~cover;
			}
		}
	}
}

// ===================================================================
// Surface copies
// ===================================================================

static void VL_STDL_SurfaceToSurface(void *src_surface, void *dst_surface, int x, int y, int sx, int sy, int sw, int sh)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *src = (VL_STDL_Surface *)src_surface;
	VL_STDL_Surface *dst = (VL_STDL_Surface *)dst_surface;
	STDL_Rect srect, drect;
	VL_STDL_Writable(dst);

	srect.x = (int16_t)sx;
	srect.y = (int16_t)sy;
	srect.w = (uint16_t)sw;
	srect.h = (uint16_t)sh;
	drect.x = (int16_t)x;
	drect.y = (int16_t)y;
	drect.w = 0;
	drect.h = 0;
	// The dirty-tile refresh copies 16x16 blocks between two of our own
	// group-aligned, maskless surfaces, dozens of times a frame. That
	// case is a straight run of long moves, but going through
	// STDL_BlitSurface costs a fixed ~0.23ms of clipping and dispatch on
	// a Mega STE plus a memcpy call for each 8-byte row, which together
	// are five times the cost of the copy itself. Measured: 0.86ms per
	// 16x16 tile through the library, and the rows are only 8 bytes, so
	// the per-row call overhead dominates. Do it directly.
	STDL_Surface *ss = VL_STDL_Target(src);
	STDL_Surface *ds = VL_STDL_Target(dst);
	if (((sx | x) & 15) == 0 && (sw & 15) == 0
		&& ss->mask == NULL && ds->mask == NULL
		&& sx >= 0 && sy >= 0 && x >= 0 && y >= 0
		&& sx + sw <= ss->w && x + sw <= ds->w
		&& sy + sh <= ss->h && y + sh <= ds->h)
	{
		int rowBytes = (sw >> 4) * 8;
		const uint8_t *sp = ss->pixels + sy * ss->stride + (sx >> 4) * 8;
		uint8_t *dp = ds->pixels + y * ds->stride + (x >> 4) * 8;
		for (int row = 0; row < sh; ++row)
		{
			const uint32_t *sl = (const uint32_t *)sp;
			uint32_t *dl = (uint32_t *)dp;
			int longs = rowBytes >> 2;
			while (longs--)
				*dl++ = *sl++;
			sp += ss->stride;
			dp += ds->stride;
		}
		return;
	}

	// Anything else - shifted, masked or clipped - goes through STDL.
	STDL_BlitSurface(ss, &srect, ds, &drect);
}

// Overlapping copy within one surface, used by the refresh manager to
// scroll its tile buffer by a tile. Coordinates round to groups.
static void VL_STDL_SurfaceToSelf(void *surface, int x, int y, int sx, int sy, int sw, int sh)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);

	int dg = x >> 4, sg = sx >> 4;
	int ng = (sw + 15) >> 4;
	if (dg + ng > surf->groups)
		ng = surf->groups - dg;
	if (sg + ng > surf->groups)
		ng = surf->groups - sg;
	if (y + sh > surf->h)
		sh = surf->h - y;
	if (sy + sh > surf->h)
		sh = surf->h - sy;
	if (ng <= 0 || sh <= 0)
		return;

	// A whole-surface shift by one tile is how the refresh manager
	// scrolls the tile buffer. Moving the origin gives the identical
	// result - reading position p afterwards yields what was at
	// p + (src - dest) either way - without touching 37KB of memory.
	// The exposed edge holds stale pixels, exactly as the screen pages
	// do, and RFL_NewRowVert/Horz redraws it before anything reads it.
	if (surf->canDrift && sw >= surf->w - 16 && sh >= surf->h - 16
		&& (x == 0 || sx == 0) && (y == 0 || sy == 0))
	{
		ptrdiff_t shift = (ptrdiff_t)((sx >> 4) - (dg)) * 8
			+ (ptrdiff_t)(sy - y) * surf->stride;
		uint8_t *newBase = s->pixels + shift;
		uint8_t *hi = surf->lo[0] + 2 * VL_STDL_SLACK;

		if (newBase < surf->lo[0] || newBase > hi)
		{
			// Out of slack: put the page back at the far end and carry on.
			uint8_t *target = (shift < 0) ? hi : surf->lo[0];
			memmove(target, s->pixels, (size_t)surf->stride * surf->h);
			newBase = target + shift;
		}
		s->pixels = newBase;
		return;
	}

	size_t rowBytes = (size_t)ng * 8;
	uint8_t *dbase = s->pixels + y * surf->stride + dg * 8;
	uint8_t *sbase = s->pixels + sy * surf->stride + sg * 8;

	if (dg == sg && ng == surf->groups)
	{
		// Whole rows: one move covers the block.
		memmove(dbase, sbase, (size_t)surf->stride * (sh - 1) + rowBytes);
		return;
	}
	if (y <= sy)
	{
		for (int _y = 0; _y < sh; ++_y)
			memmove(dbase + _y * surf->stride, sbase + _y * surf->stride, rowBytes);
	}
	else
	{
		for (int _y = sh - 1; _y >= 0; --_y)
			memmove(dbase + _y * surf->stride, sbase + _y * surf->stride, rowBytes);
	}
}

// ===================================================================
// Planar blits from converted chunks
// ===================================================================

// Source groups k = 0..ng-1 land on destination groups gx+k; at phase 8
// (x = 8 mod 16) the source is shifted right by a byte across the group
// boundaries, so destination groups gx..gx+ng each take the low byte of
// source group k-1 and the high byte of source group k.

// Unmasked converted source (4 words per group). tailKeep is the mask of
// destination bits to preserve in the last source group (0x00FF when the
// source width is an odd number of bytes).
static void VL_STDL_BlitUnmasked(VL_STDL_Surface *surf, const uint16_t *src, int x, int y, int ng, int h, uint16_t tailKeep)
{
	STDL_Surface *s = VL_STDL_Target(surf);
	int gx = x >> 4;
	int phase8 = x & 8;
	int rowWords = ng * 4;

	// Vertical clip.
	int row0 = 0;
	if (y < 0)
	{
		row0 = -y;
		y = 0;
	}
	if (y + (h - row0) > surf->h)
		h = surf->h - y + row0;
	if (h <= row0)
		return;
	src += row0 * rowWords;

	// Destination group range this blit touches, clipped to the surface.
	int jEnd = gx + ng + (phase8 ? 1 : 0);
	int j0 = gx < 0 ? 0 : gx;
	int j1 = jEnd > surf->groups ? surf->groups : jEnd;
	if (j0 >= j1)
		return;

	uint8_t *drow = s->pixels + y * surf->stride;
	for (int _y = row0; _y < h; ++_y, drow += surf->stride, src += rowWords)
	{
		if (!phase8)
		{
			uint16_t *d = (uint16_t *)drow + j0 * 4;
			const uint16_t *sp = src + (j0 - gx) * 4;
			for (int j = j0; j < j1; ++j, d += 4, sp += 4)
			{
				if (j == gx + ng - 1 && tailKeep)
				{
					uint16_t keep = tailKeep, vis = (uint16_t)~tailKeep;
					d[0] = (uint16_t)((d[0] & keep) | (sp[0] & vis));
					d[1] = (uint16_t)((d[1] & keep) | (sp[1] & vis));
					d[2] = (uint16_t)((d[2] & keep) | (sp[2] & vis));
					d[3] = (uint16_t)((d[3] & keep) | (sp[3] & vis));
				}
				else
				{
					d[0] = sp[0];
					d[1] = sp[1];
					d[2] = sp[2];
					d[3] = sp[3];
				}
			}
		}
		else
		{
			uint16_t *d = (uint16_t *)drow + j0 * 4;
			for (int j = j0; j < j1; ++j, d += 4)
			{
				int k = j - gx; // source group whose high byte lands here
				// Which destination bytes are covered by real source data.
				uint16_t keep = 0;
				if (k == 0)
					keep |= 0xFF00;
				if (k == ng)
					keep |= 0x00FF;
				if (k == ng && tailKeep)
					keep |= 0xFF00; // the padding byte of the last source group
				uint16_t vis = (uint16_t)~keep;
				const uint16_t *prev = (k > 0) ? src + (k - 1) * 4 : NULL;
				const uint16_t *cur = (k < ng) ? src + k * 4 : NULL;
				for (int p = 0; p < 4; ++p)
				{
					uint16_t v = (uint16_t)(((prev ? prev[p] : 0) << 8) | ((cur ? cur[p] : 0) >> 8));
					d[p] = (uint16_t)((d[p] & keep) | (v & vis));
				}
			}
		}
	}
}

// Masked converted source (5 words per group, mask first).
static void VL_STDL_BlitMasked(VL_STDL_Surface *surf, const uint16_t *src, int x, int y, int ng, int h)
{
	STDL_Surface *s = VL_STDL_Target(surf);
	int gx = x >> 4;
	int phase8 = x & 8;
	int rowWords = ng * 5;

	int row0 = 0;
	if (y < 0)
	{
		row0 = -y;
		y = 0;
	}
	if (y + (h - row0) > surf->h)
		h = surf->h - y + row0;
	if (h <= row0)
		return;
	src += row0 * rowWords;

	int jEnd = gx + ng + (phase8 ? 1 : 0);
	int j0 = gx < 0 ? 0 : gx;
	int j1 = jEnd > surf->groups ? surf->groups : jEnd;
	if (j0 >= j1)
		return;

	uint8_t *drow = s->pixels + y * surf->stride;
	for (int _y = row0; _y < h; ++_y, drow += surf->stride, src += rowWords)
	{
		uint16_t *d = (uint16_t *)drow + j0 * 4;
		if (!phase8)
		{
			const uint16_t *sp = src + (j0 - gx) * 5;
			for (int j = j0; j < j1; ++j, d += 4, sp += 5)
			{
				uint16_t m = sp[0];
				if (m == 0xFFFF)
					continue;
				if (m == 0)
				{
					d[0] = sp[1];
					d[1] = sp[2];
					d[2] = sp[3];
					d[3] = sp[4];
				}
				else
				{
					d[0] = (uint16_t)((d[0] & m) | sp[1]);
					d[1] = (uint16_t)((d[1] & m) | sp[2]);
					d[2] = (uint16_t)((d[2] & m) | sp[3]);
					d[3] = (uint16_t)((d[3] & m) | sp[4]);
				}
			}
		}
		else
		{
			// Half-group phase: each destination group merges the tail of
			// one source group with the head of the next. Only the first
			// and last destination group can be missing a source, so they
			// are peeled off and the interior runs without a test - the
			// old form asked "is there a previous group" twice per plane
			// per group per row, which cost more than the merging.
			int kStart = j0 - gx;
			int kEnd = j1 - gx;
			int k = kStart;

			if (k == 0)
			{
				// No previous group: the tail reads as opaque mask, zero data.
				uint16_t m = (uint16_t)(0xFF00 | (src[0] >> 8));
				if (m != 0xFFFF)
				{
					d[0] = (uint16_t)((d[0] & m) | (uint16_t)(src[1] >> 8));
					d[1] = (uint16_t)((d[1] & m) | (uint16_t)(src[2] >> 8));
					d[2] = (uint16_t)((d[2] & m) | (uint16_t)(src[3] >> 8));
					d[3] = (uint16_t)((d[3] & m) | (uint16_t)(src[4] >> 8));
				}
				d += 4;
				k = 1;
			}
			{
				const uint16_t *prev = src + (k - 1) * 5;
				int kLast = (kEnd < ng) ? kEnd : ng;
				for (; k < kLast; ++k, d += 4, prev += 5)
				{
					const uint16_t *cur = prev + 5;
					uint16_t m = (uint16_t)((prev[0] << 8) | (cur[0] >> 8));
					if (m == 0xFFFF)
						continue;
					d[0] = (uint16_t)((d[0] & m) | (uint16_t)((prev[1] << 8) | (cur[1] >> 8)));
					d[1] = (uint16_t)((d[1] & m) | (uint16_t)((prev[2] << 8) | (cur[2] >> 8)));
					d[2] = (uint16_t)((d[2] & m) | (uint16_t)((prev[3] << 8) | (cur[3] >> 8)));
					d[3] = (uint16_t)((d[3] & m) | (uint16_t)((prev[4] << 8) | (cur[4] >> 8)));
				}
			}
			if (k < kEnd)
			{
				// Past the last source group: the head reads as opaque.
				const uint16_t *prev = src + (ng - 1) * 5;
				uint16_t m = (uint16_t)((prev[0] << 8) | 0x00FF);
				if (m != 0xFFFF)
				{
					d[0] = (uint16_t)((d[0] & m) | (uint16_t)(prev[1] << 8));
					d[1] = (uint16_t)((d[1] & m) | (uint16_t)(prev[2] << 8));
					d[2] = (uint16_t)((d[2] & m) | (uint16_t)(prev[3] << 8));
					d[3] = (uint16_t)((d[3] & m) | (uint16_t)(prev[4] << 8));
				}
			}
		}
	}
}

// 8x8 tiles stay in EGA format: 4 planes of 8 bytes (masked: mask first).
static void VL_STDL_BlitTile8(VL_STDL_Surface *surf, const uint8_t *src, int x, int y, bool masked)
{
	STDL_Surface *s = VL_STDL_Target(surf);
	int gx = x >> 4;
	int half = (x & 8) ? 1 : 0;
	if (gx < 0 || gx >= surf->groups)
		return;
	int row0 = 0, h = 8;
	if (y < 0)
	{
		row0 = -y;
		y = 0;
	}
	if (y + (h - row0) > surf->h)
		h = surf->h - y + row0;
	if (h <= row0)
		return;

	uint8_t *drow = s->pixels + y * surf->stride + gx * 8 + half;
	const uint8_t *m = masked ? src : NULL;
	const uint8_t *d0 = masked ? src + 8 : src;
	for (int r = row0; r < h; ++r, drow += surf->stride)
	{
		if (m)
		{
			uint8_t mb = m[r];
			drow[0] = (uint8_t)((drow[0] & mb) | d0[r]);
			drow[2] = (uint8_t)((drow[2] & mb) | d0[8 + r]);
			drow[4] = (uint8_t)((drow[4] & mb) | d0[16 + r]);
			drow[6] = (uint8_t)((drow[6] & mb) | d0[24 + r]);
		}
		else
		{
			drow[0] = d0[r];
			drow[2] = d0[8 + r];
			drow[4] = d0[16 + r];
			drow[6] = d0[24 + r];
		}
	}
}

static void VL_STDL_UnmaskedToSurface(void *src, void *dst_surface, int x, int y, int w, int h)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	VL_STDL_Writable(surf);
	x &= ~7;
	if (w == 8 && h == 8)
	{
		VL_STDL_BlitTile8(surf, (const uint8_t *)src, x, y, false);
		return;
	}
	int bw = w >> 3;
	VL_STDL_BlitUnmasked(surf, (const uint16_t *)src, x, y, (bw + 1) >> 1, h, (bw & 1) ? 0x00FF : 0);
}

// Only some planes: used by the intro sequences, which write monochrome
// pictures into single planes. Rare, so it goes pixel-group by group.
static void VL_STDL_UnmaskedToSurface_PM(void *src, void *dst_surface, int x, int y, int w, int h, int mapmask)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);
	mapmask &= 0xF;
	if (mapmask == 0xF)
	{
		VL_STDL_UnmaskedToSurface(src, dst_surface, x, y, w, h);
		return;
	}
	x &= ~7;
	int bw = w >> 3;
	int ng = (bw + 1) >> 1;
	int gx = x >> 4;
	const uint16_t *sp = (const uint16_t *)src;
	for (int _y = 0; _y < h; ++_y)
	{
		int yy = y + _y;
		if (yy < 0 || yy >= surf->h)
		{
			sp += ng * 4;
			continue;
		}
		for (int k = 0; k < ng; ++k, sp += 4)
		{
			// Phase 8 is not needed by any caller of the plane-masked path.
			int j = gx + k;
			if (j < 0 || j >= surf->groups)
				continue;
			uint16_t vis = (k == ng - 1 && (bw & 1)) ? 0xFF00 : 0xFFFF;
			uint16_t *d = (uint16_t *)(s->pixels + yy * surf->stride + j * 8);
			for (int p = 0; p < 4; ++p)
				if (mapmask & (1 << p))
					d[p] = (uint16_t)((d[p] & ~vis) | (sp[p] & vis));
		}
	}
}

// "This is not the function you are looking for": copies the planes of a
// masked chunk without masking (id_vl.c). Nothing in Keen calls it.
static void VL_STDL_MaskedToSurface(void *src, void *dst_surface, int x, int y, int w, int h)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);
	x &= ~7;
	int bw = w >> 3;
	int ng = (bw + 1) >> 1;
	int gx = x >> 4;
	const uint16_t *sp = (const uint16_t *)src;
	for (int _y = 0; _y < h; ++_y)
	{
		int yy = y + _y;
		for (int k = 0; k < ng; ++k, sp += 5)
		{
			int j = gx + k;
			if (yy < 0 || yy >= surf->h || j < 0 || j >= surf->groups)
				continue;
			uint16_t *d = (uint16_t *)(s->pixels + yy * surf->stride + j * 8);
			d[0] = sp[1];
			d[1] = sp[2];
			d[2] = sp[3];
			d[3] = sp[4];
		}
	}
}

static void VL_STDL_MaskedBlitToSurface(void *src, void *dst_surface, int x, int y, int w, int h)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	VL_STDL_Writable(surf);
	x &= ~7;
	if (w == 8 && h == 8)
	{
		VL_STDL_BlitTile8(surf, (const uint8_t *)src, x, y, true);
		return;
	}
	VL_STDL_BlitMasked(surf, (const uint16_t *)src, x, y, ((w >> 3) + 1) >> 1, h);
}

// ===================================================================
// 1bpp sources (fonts, intro pictures, sprite masks)
// ===================================================================

// The 16 bits of a 1bpp row starting at bit offset off (which may be
// negative or run past the end; those bits read as zero). w is the row's
// width in bits, so the bits in the last byte beyond w are ignored.
static uint16_t VL_STDL_Bits16(const uint8_t *row, int w, int off)
{
	int nbytes = (w + 7) >> 3;
	int byte = off >> 3; // floor
	int sh = off & 7;
	uint32_t v = 0;
	for (int i = 0; i < 3; ++i)
	{
		int b = byte + i;
		v <<= 8;
		if (b >= 0 && b < nbytes)
			v |= row[b];
	}
	uint16_t bits = (uint16_t)(v >> (8 - sh));
	// Trim bits past the width.
	int lastOff = w - off; // bits of this word that are inside the source
	if (lastOff < 16)
		bits &= (lastOff <= 0) ? 0 : (uint16_t)(0xFFFF << (16 - lastOff));
	return bits;
}

// Bits of the group that fall inside [x, x + w) on the destination.
static uint16_t VL_STDL_Cover(int j, int x, int w)
{
	int gs = j * 16;
	int a = x - gs, b = x + w - gs; // in group coordinates
	if (a < 0)
		a = 0;
	if (b > 16)
		b = 16;
	if (a >= b)
		return 0;
	return (uint16_t)((0xFFFF >> a) & (0xFFFF << (16 - b)));
}

typedef enum
{
	VL_STDL_Bit_Set,  // pixel = bit ? colour : 0, planes in mapmask
	VL_STDL_Bit_Xor,  // planes with a colour bit ^= pattern
	VL_STDL_Bit_Blit, // pixel = colour where bit set
} VL_STDL_BitOp;

static void VL_STDL_BitOpToSurface(void *src, void *dst_surface, int x, int y, int w, int h, int colour, int mapmask, VL_STDL_BitOp op)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);
	int pitch = (w + 7) >> 3;
	int j0 = x >> 4, j1 = (x + w - 1) >> 4;
	if (j0 < 0)
		j0 = 0;
	if (j1 >= surf->groups)
		j1 = surf->groups - 1;
	if (w <= 0 || j0 > j1)
		return;
	mapmask &= 0xF;

	for (int _y = 0; _y < h; ++_y)
	{
		int yy = y + _y;
		if (yy < 0 || yy >= surf->h)
			continue;
		const uint8_t *row = (const uint8_t *)src + _y * pitch;
		uint16_t *d = (uint16_t *)(s->pixels + yy * surf->stride + j0 * 8);
		for (int j = j0; j <= j1; ++j, d += 4)
		{
			uint16_t cover = VL_STDL_Cover(j, x, w);
			uint16_t pat = (uint16_t)(VL_STDL_Bits16(row, w, j * 16 - x) & cover);
			for (int p = 0; p < 4; ++p)
			{
				bool cbit = (colour >> p) & 1;
				switch (op)
				{
				case VL_STDL_Bit_Set:
					if (mapmask & (1 << p))
						d[p] = (uint16_t)((d[p] & ~cover) | (cbit ? pat : 0));
					break;
				case VL_STDL_Bit_Xor:
					if (cbit)
						d[p] ^= pat;
					break;
				case VL_STDL_Bit_Blit:
					if (cbit)
						d[p] |= pat;
					else
						d[p] &= (uint16_t)~pat;
					break;
				}
			}
		}
	}
}

static void VL_STDL_BitToSurface(void *src, void *dst_surface, int x, int y, int w, int h, int colour)
{
	// No count here: VL_STDL_BitOpToSurface counts on our behalf.
	VL_STDL_BitOpToSurface(src, dst_surface, x, y, w, h, colour, 0xF, VL_STDL_Bit_Set);
}

static void VL_STDL_BitToSurface_PM(void *src, void *dst_surface, int x, int y, int w, int h, int colour, int mapmask)
{
	// No count here: VL_STDL_BitOpToSurface counts on our behalf.
	VL_STDL_BitOpToSurface(src, dst_surface, x, y, w, h, colour, mapmask, VL_STDL_Bit_Set);
}

static void VL_STDL_BitXorWithSurface(void *src, void *dst_surface, int x, int y, int w, int h, int colour)
{
	VL_STDL_BitOpToSurface(src, dst_surface, x, y, w, h, colour, 0xF, VL_STDL_Bit_Xor);
}

static void VL_STDL_BitBlitToSurface(void *src, void *dst_surface, int x, int y, int w, int h, int colour)
{
	VL_STDL_BitOpToSurface(src, dst_surface, x, y, w, h, colour, 0xF, VL_STDL_Bit_Blit);
}

// Draws a sprite's silhouette: the source is a converted masked sprite
// (VH_DrawSpriteMask and VH_DrawShiftedSpriteMask are its only callers)
// and every opaque pixel becomes colour.
static void VL_STDL_BitInvBlitToSurface(void *src, void *dst_surface, int x, int y, int w, int h, int colour)
{
	VL_STDL_COUNTBLIT();
	VL_STDL_Surface *surf = (VL_STDL_Surface *)dst_surface;
	STDL_Surface *s = VL_STDL_Target(surf);
	VL_STDL_Writable(surf);
	x &= ~7;
	int ng = ((w >> 3) + 1) >> 1;
	int gx = x >> 4;
	int phase8 = x & 8;
	int rowWords = ng * 5;
	const uint16_t *sp = (const uint16_t *)src;

	int jEnd = gx + ng + (phase8 ? 1 : 0);
	int j0 = gx < 0 ? 0 : gx;
	int j1 = jEnd > surf->groups ? surf->groups : jEnd;
	if (j0 >= j1)
		return;

	for (int _y = 0; _y < h; ++_y, sp += rowWords)
	{
		int yy = y + _y;
		if (yy < 0 || yy >= surf->h)
			continue;
		uint16_t *d = (uint16_t *)(s->pixels + yy * surf->stride + j0 * 8);
		for (int j = j0; j < j1; ++j, d += 4)
		{
			int k = j - gx;
			uint16_t m;
			if (!phase8)
				m = sp[k * 5];
			else
			{
				uint16_t prev = (k > 0) ? sp[(k - 1) * 5] : 0xFFFF;
				uint16_t cur = (k < ng) ? sp[k * 5] : 0xFFFF;
				m = (uint16_t)((prev << 8) | (cur >> 8));
			}
			uint16_t pat = (uint16_t)~m;
			if (!pat)
				continue;
			for (int p = 0; p < 4; ++p)
			{
				if (colour & (1 << p))
					d[p] |= pat;
				else
					d[p] &= m;
			}
		}
	}
}

// ===================================================================
// Scrolling and presenting
// ===================================================================

static void VL_STDL_ScrollSurface(void *surface, int x, int y)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;

	if (surf->use == VL_SurfaceUsage_FrontBuffer)
	{
		// Move the logical origin of both pages by moving their base
		// pointers, copying a page back to the other end of its block
		// when the pointer would leave it.
		ptrdiff_t shift = (ptrdiff_t)(x >> 4) * 8 + (ptrdiff_t)y * surf->stride;
		size_t pageBytes = (size_t)surf->stride * surf->h;
		for (int p = 0; p < 2; ++p)
		{
			uint8_t *newBase = surf->page[p]->pixels + shift;
			uint8_t *hi = surf->lo[p] + 2 * VL_STDL_SLACK;
			if (newBase < surf->lo[p] || newBase > hi)
			{
				uint8_t *target = (shift < 0) ? hi : surf->lo[p];
				VL_STDL_Writable(surf);
				memmove(target, surf->page[p]->pixels, pageBytes);
				newBase = target + shift;
			}
			surf->page[p]->pixels = newBase;
		}
	}
	else
	{
		// Moving a surface doesn't preserve the new bits (id_vl_dos.c).
		STDL_Surface *s = surf->page[0];
		int dest_x = (x < 0) ? -x : 0;
		int dest_y = (y < 0) ? -y : 0;
		int src_x = (x > 0) ? x : 0;
		int src_y = (y > 0) ? y : 0;
		int wOffset = (x) ? -16 : 0;
		int hOffset = (y) ? -16 : 0;
		ptrdiff_t srcOffset = (src_x >> 4) * 8 + (ptrdiff_t)src_y * surf->stride;
		ptrdiff_t destOffset = (dest_x >> 4) * 8 + (ptrdiff_t)dest_y * surf->stride;
		size_t size = (size_t)surf->stride * (surf->h + hOffset) + ((surf->w + wOffset) >> 4) * 8 - surf->stride;
		memmove(s->pixels + destOffset, s->pixels + srcOffset, size);
	}
}

static void VL_STDL_Present(void *surface, int scrlX, int scrlY, bool singleBuffered)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	if (surf->use != VL_SurfaceUsage_FrontBuffer)
		return;

	if (!singleBuffered)
		surf->activePage ^= 1;

#ifdef CK_STDL_PROFILE
	{
		static uint32_t presents, lastMs, lastBlits;
		if ((++presents & 63) == 0)
		{
			uint32_t now = STDL_GetTicks();
			uint32_t ms = now - lastMs;
			static uint32_t lastTics;
			uint32_t tics = SD_GetTimeCount();
			// The game clock should advance at 70Hz. tics*1000/ms is its
			// real rate: if it is low the whole game runs slow.
			CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "FPS: 64 frames in %lu ms = %lu.%02lu fps, %lu blits/frame, gameclock %lu.%luHz\n",
				(unsigned long)ms, ms ? (unsigned long)(6400000UL / ms / 100) : 0UL,
				ms ? (unsigned long)(6400000UL / ms % 100) : 0UL,
				(unsigned long)((vl_stdl_blits - lastBlits) / 64),
				ms ? (unsigned long)((tics - lastTics) * 1000UL / ms) : 0UL,
				ms ? (unsigned long)((tics - lastTics) * 10000UL / ms % 10) : 0UL);
			lastTics = tics;
			lastMs = now;
			lastBlits = vl_stdl_blits;
		}
	}
#endif

	int shown = VL_STDL_ShownPage(surf);
	if (scrlX < 0)
		scrlX = 0;
	if (scrlY < 0)
		scrlY = 0;
	uint8_t *base = surf->page[shown]->pixels + scrlY * surf->stride + (scrlX >> 4) * 8;
	if (STDL_SetScrollWindow(base, surf->stride, scrlX & 15) < 0)
	{
		static bool warned = false;
		if (!warned)
		{
			warned = true;
			CK_Cross_LogMessage(CK_LOG_MSG_ERROR, "VL_STDL_Present: %s\n", STDL_GetError());
		}
		return;
	}
	// The old front page becomes the drawing target, but it stays on
	// screen until STDL has programmed the new window; the first write
	// to it waits (VL_STDL_Writable). Meanwhile the game gets on with its
	// tic accounting and logic.
	if (!singleBuffered)
		vl_stdl_flipPending = true;
}

static int VL_STDL_GetActiveBufferId(void *surface)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	return surf->activePage;
}

static int VL_STDL_GetNumBuffers(void *surface)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	return (surf->use == VL_SurfaceUsage_FrontBuffer) ? 2 : 1;
}

// Copy the page on screen into the page being drawn.
static void VL_STDL_SyncBuffers(void *surface)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	if (surf->use != VL_SurfaceUsage_FrontBuffer)
		return;
	VL_STDL_Writable(surf);
	STDL_BlitSurface(surf->page[VL_STDL_ShownPage(surf)], NULL, surf->page[surf->activePage], NULL);
}

// Copy a rectangle of the page being drawn into the page on screen, so
// that both pages carry it.
static void VL_STDL_UpdateRect(void *surface, int x, int y, int w, int h)
{
	VL_STDL_Surface *surf = (VL_STDL_Surface *)surface;
	STDL_Rect srect, drect;
	if (surf->use != VL_SurfaceUsage_FrontBuffer)
		return;
	srect.x = (int16_t)x;
	srect.y = (int16_t)y;
	srect.w = (uint16_t)w;
	srect.h = (uint16_t)h;
	drect.x = (int16_t)x;
	drect.y = (int16_t)y;
	drect.w = 0;
	drect.h = 0;
	STDL_BlitSurface(surf->page[surf->activePage], &srect, surf->page[VL_STDL_ShownPage(surf)], &drect);
}

static void VL_STDL_FlushParams()
{
}

static void VL_STDL_WaitVBLs(int vbls)
{
	for (int i = 0; i < vbls; ++i)
		STDL_WaitVBL();
}

VL_Backend vl_stdl_backend =
	{
		/*.setVideoMode =*/&VL_STDL_SetVideoMode,
		/*.createSurface =*/&VL_STDL_CreateSurface,
		/*.destroySurface =*/&VL_STDL_DestroySurface,
		/*.getSurfaceMemUse =*/&VL_STDL_GetSurfaceMemUse,
		/*.getSurfaceDimensions =*/&VL_STDL_GetSurfaceDimensions,
		/*.refreshPaletteAndBorderColor =*/&VL_STDL_RefreshPaletteAndBorderColor,
		/*.surfacePGet =*/&VL_STDL_SurfacePGet,
		/*.surfaceRect =*/&VL_STDL_SurfaceRect,
		/*.surfaceRect_PM =*/&VL_STDL_SurfaceRect_PM,
		/*.surfaceToSurface =*/&VL_STDL_SurfaceToSurface,
		/*.surfaceToSelf =*/&VL_STDL_SurfaceToSelf,
		/*.unmaskedToSurface =*/&VL_STDL_UnmaskedToSurface,
		/*.unmaskedToSurface_PM =*/&VL_STDL_UnmaskedToSurface_PM,
		/*.maskedToSurface =*/&VL_STDL_MaskedToSurface,
		/*.maskedBlitToSurface =*/&VL_STDL_MaskedBlitToSurface,
		/*.bitToSurface =*/&VL_STDL_BitToSurface,
		/*.bitToSurface_PM =*/&VL_STDL_BitToSurface_PM,
		/*.bitXorWithSurface =*/&VL_STDL_BitXorWithSurface,
		/*.bitBlitToSurface =*/&VL_STDL_BitBlitToSurface,
		/*.bitInvBlitToSurface =*/&VL_STDL_BitInvBlitToSurface,
		/*.scrollSurface =*/&VL_STDL_ScrollSurface,
		/*.present =*/&VL_STDL_Present,
		/*.getActiveBufferId =*/&VL_STDL_GetActiveBufferId,
		/*.getNumBuffers =*/&VL_STDL_GetNumBuffers,
		/*.syncBuffers =*/&VL_STDL_SyncBuffers,
		/*.updateRect =*/&VL_STDL_UpdateRect,
		/*.flushParams =*/&VL_STDL_FlushParams,
		/*.waitVBLs =*/&VL_STDL_WaitVBLs};

VL_Backend *VL_Impl_GetBackend()
{
	return &vl_stdl_backend;
}
