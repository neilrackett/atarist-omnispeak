/*
Omnispeak: A Commander Keen Reimplementation
Blit-timing probe for the Atari STE port
Copyright (C) 2026 Neil Rackett

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * How long does one 16x16 tile copy actually take on this machine?
 *
 * The port's dirty-tile scroll redraws each changed tile with
 * STDL_BlitSurface, copying a 16x16 block from the tile buffer into an
 * offscreen screen page. Both are four-plane maskless surfaces with the
 * port's 336-pixel page stride, and the destination x is tile-aligned.
 * This measures exactly that, then two variants that would explain a
 * slow result: an odd destination x (which forces the shift-and-merge
 * path instead of the aligned word path) and a destination that carries
 * a transparency mask (a fifth plane the copy would maintain).
 *
 * A healthy 16x16 four-plane copy is 64 words read and 64 written, on
 * the order of 100us on an 8MHz 68000 and well under half that at 16MHz
 * with the cache. A result near a millisecond means a slow path, and
 * the three timings below say which. Run it on real hardware: an
 * emulator's blit timing is only approximate.
 *
 * Build (from the port root, against the STDL submodule):
 *   STCMD_NO_TTY=1 stcmd m68k-atari-mint-gcc -O2 -fomit-frame-pointer \
 *     -std=gnu99 -I stdl/include -o dist/BLITTIME.TOS \
 *     tools/blittime.c stdl/libstdl.a -lm
 */

#include <stdio.h>
#include <stdint.h>
#include <stdl/stdl.h>

#define PAGE_W 336
#define PAGE_H 224
#define TILE 16
#define ITERS 6000

/* time ITERS copies of a wxh block from src to dst at dst x=dx */
static long run(STDL_Surface *src, STDL_Surface *dst, int dx, int w, int h)
{
	STDL_Rect s, d;
	uint32_t t0, t1;
	int i;

	s.x = 0; s.y = 0; s.w = (int16_t)w; s.h = (int16_t)h;
	d.x = (int16_t)dx; d.y = 0; d.w = (int16_t)w; d.h = (int16_t)h;

	/* warm the path, then time */
	STDL_BlitSurface(src, &s, dst, &d);
	t0 = STDL_GetTicks();
	for (i = 0; i < ITERS; i++)
		STDL_BlitSurface(src, &s, dst, &d);
	t1 = STDL_GetTicks();
	return (long)(t1 - t0);
}

static void report(const char *label, long ms)
{
	/* words moved per blit: 16 rows * 4 planes, read + write */
	long us_x10 = ms * 10000L / ITERS;   /* tenths of a us per blit */
	printf("%-22s %5ld ms / %d = %ld.%ld us/blit\n",
	       label, ms, ITERS, us_x10 / 10, us_x10 % 10);
}

int main(int argc, char *argv[])
{
	STDL_Surface *src, *dst, *dstmask;

	(void)argc; (void)argv;
	if (STDL_Init(STDL_INIT_VIDEO) < 0) {
		fprintf(stderr, "init: %s\n", STDL_GetError());
		return 1;
	}
	if (STDL_SetVideoMode(320, 200, 4, 0) == NULL) {
		fprintf(stderr, "video: %s\n", STDL_GetError());
		return 1;
	}

	/* two maskless offscreen pages at the port's page size */
	src = STDL_CreateSurface(PAGE_W, PAGE_H);
	dst = STDL_CreateSurface(PAGE_W, PAGE_H);
	dstmask = STDL_CreateSurface(PAGE_W, PAGE_H);
	if (!src || !dst || !dstmask) {
		fprintf(stderr, "surface: %s\n", STDL_GetError());
		STDL_Quit();
		return 1;
	}
	if (STDL_CreateMask(dstmask, 0) < 0)   /* force a mask plane */
		fprintf(stderr, "(no mask: %s)\n", STDL_GetError());

	printf("16x16 four-plane tile copy, %d-wide pages, %d iterations\n",
	       PAGE_W, ITERS);
	printf("64 words read + 64 written per copy\n\n");

	/* one row isolates the per-call setup; 16 rows adds the copy, so
	 * (16-row - 1-row) / 15 is the per-row cost and the 1-row figure
	 * is roughly the fixed overhead. If they are close, a tiny blit is
	 * dominated by setup, not by moving pixels. */
	printf("src pixels %p stride %u | dst pixels %p stride %u | align %d\n",
	       (void *)src->pixels, (unsigned)src->stride,
	       (void *)dst->pixels, (unsigned)dst->stride,
	       (int)((((uintptr_t)src->pixels | (uintptr_t)dst->pixels
	               | src->stride | dst->stride) & 3) == 0));
	report("16x8  (their case)",     run(src, dst, 0, TILE, 8));
	report("16x1  (setup + 1 row)", run(src, dst, 0, TILE, 1));
	report("32x1  (setup + 1 row)", run(src, dst, 0, 32, 1));
	report("64x1  (setup + 1 row)", run(src, dst, 0, 64, 1));
	report("16x16 aligned",         run(src, dst, 0, TILE, TILE));
	report("32x16 aligned",         run(src, dst, 0, 32, TILE));
	report("64x16 aligned",         run(src, dst, 0, 64, TILE));
	report("16x16 aligned",         run(src, dst, 0, TILE, TILE));
	report("16x16 odd x (shift)",   run(src, dst, 1, TILE, TILE));
	report("16x16 dest mask",       run(src, dstmask, 0, TILE, TILE));

	printf("\nESC or a key to quit.\n");
	{
		STDL_Event e;
		int done = 0;
		while (!done) {
			while (STDL_PollEvent(&e))
				if (e.type == STDL_KEYDOWN || e.type == STDL_QUIT)
					done = 1;
			STDL_Delay(20);
		}
	}
	STDL_Quit();
	return 0;
}
