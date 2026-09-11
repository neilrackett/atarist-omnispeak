/*
Omnispeak: A Commander Keen Reimplementation
Atari ST sound backend on STDL
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

// Sound on the YM2149. Keen's AdLib music and effects are streams of OPL
// register writes; its PC-speaker effects are single tones. The DOS game
// runs a 140Hz sound service (560Hz with AdLib music) from the PIT and
// derives its 70Hz game clock from it. Here that service runs from
// STDL's 50Hz VBL callback, paced by the 200Hz system counter so the
// clock stays exact: the ticks due since the last VBL are run in a burst.
//
// The service's OPL and PC-speaker writes reach this backend through
// alOut() and pcSpkOn(). They are turned into a three-voice square-wave
// cover: each OPL channel's key-on/off and frequency are tracked, and
// once per frame the three most recently keyed channels (last-note
// priority, the policy stdlconv's MIDI converter uses) are played on the
// YM's three tone voices, with a PC-speaker tone competing on equal
// terms. It is a chiptune version, not the OPL sound.
//
// The backend drives the YM2149 directly rather than through STDL's
// speaker, because that gives only one voice: it never calls
// STDL_Music/Sfx/Speaker, so STDL's own YM service stays out of the way.
// The cost is that STDL's terminate-vector cleanup does not cover the
// chip; an abnormal exit can leave a voice sounding (normal exit and
// atexit silence it).

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "id_sd.h"
#include "ck_cross.h"

#include <stdl/stdl.h>

#define PC_PIT_RATE 1193182
#define SD_STDL_MAX_BURST 64
#define SD_STDL_VOLUME 11

void SDL_t0Service(void);

// ---- YM2149 access --------------------------------------------------
// The library runs in supervisor mode throughout, so the sound chip and
// low memory are directly addressable.

#ifdef __m68k__
#define YM_SELECT (*(volatile uint8_t *)0xFFFF8800UL)
#define YM_DATA   (*(volatile uint8_t *)0xFFFF8802UL)
#define CONTERM   (*(volatile uint8_t *)0x484UL)
#else
static uint8_t ym_host_regs[16];
static uint8_t ym_host_sel;
static uint8_t ym_host_conterm;
#define YM_SELECT ym_host_sel
#define YM_DATA   ym_host_regs[ym_host_sel & 15]
#define CONTERM   ym_host_conterm
#endif

// Mixer register (r7) shadow: bits 0-2 tone enable (0 = on), bits 3-5
// noise enable, bits 6-7 the I/O port directions TOS relies on. All
// tones and noise off to start.
static uint8_t ym_mix = 0x3F;
static int ym_old_conterm = -1;

static void ym_write(int reg, int val)
{
	YM_SELECT = (uint8_t)reg;
	YM_DATA = (uint8_t)val;
}

// Write the mixer, preserving the port-direction bits, and never touch
// registers 14/15 (the ports TOS uses for floppy select).
static void ym_write_mixer(uint8_t mix)
{
	YM_SELECT = 7;
	ym_write(7, (uint8_t)((YM_SELECT & 0xC0) | (mix & 0x3F)));
}

static void ym_set_voice(int voice, uint16_t period, uint8_t volume)
{
	ym_write(2 * voice, period & 0xFF);
	ym_write(2 * voice + 1, (period >> 8) & 0x0F);
	ym_write(8 + voice, volume);
}

static void ym_silence(void)
{
	for (int v = 0; v < 3; ++v)
		ym_write(8 + v, 0);
	ym_mix = 0x3F;
	ym_write_mixer(ym_mix);
}

// YM tone period for a frequency (2MHz master / 16).
static uint16_t ym_period(int freq)
{
	long p;
	if (freq < 1)
		return 0;
	p = 125000L / freq;
	if (p < 1)
		p = 1;
	if (p > 0x0FFF)
		p = 0x0FFF;
	return (uint16_t)p;
}

// ---- OPL and PC-speaker state (written from the sound service) -------

#define SD_OPL_CHANNELS 9

static uint16_t opl_freq[SD_OPL_CHANNELS];  // Hz, from F-number and block
static uint8_t opl_fnLo[SD_OPL_CHANNELS];   // low byte of the F-number
static uint8_t opl_on[SD_OPL_CHANNELS];     // keyed on
static uint16_t opl_order[SD_OPL_CHANNELS]; // recency of the last key-on

static uint16_t pc_freq;
static uint8_t pc_on;
static uint16_t pc_order;

static uint16_t sd_stdl_clock; // recency counter

static bool sd_stdl_up;
static volatile bool sd_stdl_locked;
static volatile uint32_t sd_stdl_lastHz;
static volatile uint32_t sd_stdl_acc; // tick fraction, in 1/200ths
static volatile uint32_t sd_stdl_rate = 140;

// Recompute the three tone voices from the OPL channels and the PC
// speaker, newest sources first. Runs at the end of the VBL, after the
// sound service has updated the state above.
static void ym_update(void)
{
	uint16_t bestOrder[3] = {0, 0, 0};
	uint16_t bestFreq[3] = {0, 0, 0};
	int found = 0;

	for (int src = 0; src <= SD_OPL_CHANNELS; ++src)
	{
		uint16_t order, freq;
		if (src < SD_OPL_CHANNELS)
		{
			if (!opl_on[src] || opl_freq[src] == 0)
				continue;
			order = opl_order[src];
			freq = opl_freq[src];
		}
		else
		{
			if (!pc_on || pc_freq == 0)
				continue;
			order = pc_order;
			freq = pc_freq;
		}
		// Insert into the top three, ordered by recency (newest first).
		for (int i = 0; i < 3; ++i)
		{
			if (found <= i || order > bestOrder[i])
			{
				for (int j = 2; j > i; --j)
				{
					bestOrder[j] = bestOrder[j - 1];
					bestFreq[j] = bestFreq[j - 1];
				}
				bestOrder[i] = order;
				bestFreq[i] = freq;
				if (found < 3)
					found++;
				break;
			}
		}
	}

	uint8_t mix = 0x3F;
	for (int v = 0; v < 3; ++v)
	{
		if (v < found && bestFreq[v])
		{
			ym_set_voice(v, ym_period(bestFreq[v]), SD_STDL_VOLUME);
			mix &= (uint8_t)~(1u << v); // tone on
		}
		else
		{
			ym_write(8 + v, 0);
		}
	}
	if (mix != ym_mix)
	{
		ym_mix = mix;
		ym_write_mixer(mix);
	}
}

static void SD_STDL_VBL(void)
{
	uint32_t now = STDL_GetHz200();
	uint32_t elapsed = now - sd_stdl_lastHz;
	sd_stdl_lastHz = now;
	sd_stdl_acc += elapsed * sd_stdl_rate;
	if (sd_stdl_locked)
		return; // ticks accumulate until the lock is released
	int burst = 0;
	while (sd_stdl_acc >= 200 && burst < SD_STDL_MAX_BURST)
	{
		sd_stdl_acc -= 200;
		SDL_t0Service();
		burst++;
	}
	if (sd_stdl_acc >= 200)
		sd_stdl_acc = 0; // a long stall: don't replay it
	ym_update();
}

static void SD_STDL_SetTimer0(int16_t int_8_divisor)
{
	int div = (uint16_t)int_8_divisor;
	if (div <= 0)
		div = 65536;
	sd_stdl_rate = (uint32_t)(PC_PIT_RATE / div);
	if (sd_stdl_rate < 1)
		sd_stdl_rate = 1;
}

static void SD_STDL_PCSpkOn(bool on, int freq)
{
	if (on && freq > 0)
	{
		if (!pc_on)
			pc_order = ++sd_stdl_clock;
		pc_on = 1;
		pc_freq = (uint16_t)freq;
	}
	else
	{
		pc_on = 0;
	}
}

// The AdLib register writes: only the note registers (A0-A8 F-number
// low, B0-B8 F-number high + block + key-on) matter for a square-wave
// cover; the instrument registers set timbre we cannot reproduce.
static void SD_STDL_alOut(uint8_t reg, uint8_t val)
{
	if (reg >= SD_ADLIB_REG_NOTE_LO && reg < SD_ADLIB_REG_NOTE_LO + SD_OPL_CHANNELS)
	{
		opl_fnLo[reg - SD_ADLIB_REG_NOTE_LO] = val;
	}
	else if (reg >= SD_ADLIB_REG_NOTE_HI && reg < SD_ADLIB_REG_NOTE_HI + SD_OPL_CHANNELS)
	{
		int ch = reg - SD_ADLIB_REG_NOTE_HI;
		int block = (val >> 2) & 7;
		int fnum = ((val & 3) << 8) | opl_fnLo[ch];
		bool keyOn = (val & 0x20) != 0;
		// f = fnum * 49716 / 2^(20 - block)
		uint32_t freq = ((uint32_t)fnum * 49716UL) >> (20 - block);
		if (freq > 0xFFFF)
			freq = 0xFFFF;
		opl_freq[ch] = (uint16_t)freq;
		if (keyOn)
		{
			if (!opl_on[ch])
				opl_order[ch] = ++sd_stdl_clock;
			opl_on[ch] = 1;
		}
		else
		{
			opl_on[ch] = 0;
		}
	}
}

static void SD_STDL_Exit(void)
{
	if (sd_stdl_up)
		ym_silence();
}

static void SD_STDL_Startup(void)
{
	if (sd_stdl_up)
		return;
	// Silence the console key click, which also uses the sound chip, and
	// keep the chip quiet to start.
	ym_old_conterm = CONTERM;
	CONTERM = (uint8_t)(ym_old_conterm & ~1);
	ym_silence();
	sd_stdl_lastHz = STDL_GetHz200();
	sd_stdl_acc = 0;
	if (STDL_AddVBL(SD_STDL_VBL) < 0)
		CK_Cross_LogMessage(CK_LOG_MSG_ERROR, "SD_STDL_Startup: %s\n", STDL_GetError());
	sd_stdl_up = true;
	atexit(SD_STDL_Exit);
}

static void SD_STDL_Shutdown(void)
{
	if (!sd_stdl_up)
		return;
	STDL_RemoveVBL(SD_STDL_VBL);
	ym_silence();
	if (ym_old_conterm >= 0)
	{
		CONTERM = (uint8_t)ym_old_conterm;
		ym_old_conterm = -1;
	}
	sd_stdl_up = false;
}

static void SD_STDL_Lock()
{
	if (sd_stdl_locked)
		CK_Cross_LogMessage(CK_LOG_MSG_ERROR, "Tried to lock the audio system when it was already locked!\n");
	sd_stdl_locked = true;
}

static void SD_STDL_Unlock()
{
	if (!sd_stdl_locked)
		CK_Cross_LogMessage(CK_LOG_MSG_ERROR, "Tried to unlock the audio system when it was already unlocked!\n");
	sd_stdl_locked = false;
}

static void SD_STDL_WaitTick()
{
	STDL_WaitVBL();
}

static unsigned int SD_STDL_Detect()
{
	return SD_CARD_PC_SPEAKER | SD_CARD_OPL2;
}

static void SD_STDL_SetOPL3(bool on)
{
	(void)on;
}

SD_Backend sd_stdl_backend = {
	.startup = SD_STDL_Startup,
	.shutdown = SD_STDL_Shutdown,
	.lock = SD_STDL_Lock,
	.unlock = SD_STDL_Unlock,
	.alOut = SD_STDL_alOut,
	.pcSpkOn = SD_STDL_PCSpkOn,
	.setTimer0 = SD_STDL_SetTimer0,
	.waitTick = SD_STDL_WaitTick,
	.detect = SD_STDL_Detect,
	.setOPL3 = SD_STDL_SetOPL3};

SD_Backend *SD_Impl_GetBackend()
{
	return &sd_stdl_backend;
}
