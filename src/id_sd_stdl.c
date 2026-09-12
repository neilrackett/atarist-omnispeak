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

// Sound on the YM2149 through STDL's tone device. Keen's AdLib music
// and effects are streams of OPL register writes; its PC-speaker
// effects are single tones. The DOS game runs a 140Hz sound service
// (560Hz with AdLib music) from the PIT and derives its 70Hz game
// clock from it. Here that service runs from STDL's 50Hz VBL callback,
// paced by the 200Hz system counter so the clock stays exact: the
// ticks due since the last VBL are run in a burst.
//
// The service's OPL and PC-speaker writes reach this backend through
// alOut() and pcSpkOn(). Each OPL channel's key-on, frequency and
// carrier level become a note in one of STDL_Tone's slots (channels
// 0-8, the speaker in slot 9), and the device plays the three most
// recently keyed on the chip: a three-voice square-wave cover of the
// score, not the OPL sound. STDL owns the chip, so its terminate-vector
// cleanup silences it on any exit.

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
#define SD_STDL_SPEAKER_VOLUME 12

#define SD_OPL_CHANNELS 9
#define SD_STDL_SPEAKER_SLOT SD_OPL_CHANNELS

void SDL_t0Service(void);

// ---- OPL channel state (written from the sound service) --------------

static uint8_t opl_fnLo[SD_OPL_CHANNELS]; // low byte of the F-number
static uint8_t opl_blockHi[SD_OPL_CHANNELS]; // last B0-B8 value (block, F-number high, key-on)
static uint8_t opl_level[SD_OPL_CHANNELS]; // carrier total level (0 loud .. 63 silent)
static uint8_t opl_on[SD_OPL_CHANNELS];

static uint16_t pc_period; // 0 = off

static bool sd_stdl_up;
static volatile bool sd_stdl_locked;
static volatile uint32_t sd_stdl_lastHz;
static volatile uint32_t sd_stdl_acc; // tick fraction, in 1/200ths
static volatile uint32_t sd_stdl_rate = 140;

// YM tone period for an OPL F-number and block:
// f = fnum * 49716 / 2^(20 - block), period = 125000 / f.
static uint16_t SD_STDL_Period(int fnum, int block)
{
	uint32_t freq = ((uint32_t)fnum * 49716UL) >> (20 - block);
	uint32_t p;
	if (freq == 0)
		return 0;
	p = 125000UL / freq;
	if (p < 1)
		p = 1;
	if (p > 0x0FFF)
		p = 0x0FFF;
	return (uint16_t)p;
}

// YM volume for an OPL total level: 0.75dB per OPL step against the
// YM's roughly 1.5dB per step, so four OPL steps lose one YM step.
static uint8_t SD_STDL_Volume(uint8_t level)
{
	int v = 15 - (level >> 2);
	return v < 0 ? 0 : (uint8_t)v;
}

// The carrier operator's register offset for a channel: operators
// are numbered in threes with a gap of eight per row, the carrier
// three above the modulator.
static int SD_STDL_CarrierChannel(int op)
{
	int row = op >> 3, col = op & 7;
	if (col < 3 || col > 5)
		return -1; // a modulator, or an unused offset
	return row * 3 + (col - 3);
}

static void SD_STDL_Note(int ch)
{
	uint8_t hi = opl_blockHi[ch];
	int block = (hi >> 2) & 7;
	int fnum = ((hi & 3) << 8) | opl_fnLo[ch];
	bool keyOn = (hi & 0x20) != 0;
	uint16_t period = SD_STDL_Period(fnum, block);
	uint8_t vol = SD_STDL_Volume(opl_level[ch]);

	if (!keyOn || period == 0)
	{
		if (opl_on[ch])
		{
			opl_on[ch] = 0;
			STDL_ToneOff(ch);
		}
	}
	else if (!opl_on[ch])
	{
		opl_on[ch] = 1;
		STDL_ToneOn(ch, period, vol);
	}
	else
	{
		STDL_ToneSet(ch, period, vol); // a bend, or the effect's next step
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
		uint16_t period = STDL_YM_PERIOD(freq);
		if (period == 0)
			period = 1;
		if (pc_period == 0)
			STDL_ToneOn(SD_STDL_SPEAKER_SLOT, period, SD_STDL_SPEAKER_VOLUME);
		else if (period != pc_period)
			STDL_ToneSet(SD_STDL_SPEAKER_SLOT, period, SD_STDL_SPEAKER_VOLUME);
		pc_period = period;
	}
	else if (pc_period != 0)
	{
		pc_period = 0;
		STDL_ToneOff(SD_STDL_SPEAKER_SLOT);
	}
}

// The AdLib register writes. Only three groups matter for a square-wave
// cover: A0-A8 (F-number low), B0-B8 (F-number high, block, key-on) and
// 40-55 (operator total level, of which the carrier's sets the note's
// volume). The rest set timbre the YM cannot reproduce.
static void SD_STDL_alOut(uint8_t reg, uint8_t val)
{
	if (reg >= SD_ADLIB_REG_NOTE_LO && reg < SD_ADLIB_REG_NOTE_LO + SD_OPL_CHANNELS)
	{
		int ch = reg - SD_ADLIB_REG_NOTE_LO;
		opl_fnLo[ch] = val;
		if (opl_on[ch])
			SD_STDL_Note(ch);
	}
	else if (reg >= SD_ADLIB_REG_NOTE_HI && reg < SD_ADLIB_REG_NOTE_HI + SD_OPL_CHANNELS)
	{
		int ch = reg - SD_ADLIB_REG_NOTE_HI;
		opl_blockHi[ch] = val;
		SD_STDL_Note(ch);
	}
	else if (reg >= SD_ADLIB_REG_VOLUME && reg < SD_ADLIB_REG_VOLUME + 0x16)
	{
		int ch = SD_STDL_CarrierChannel(reg - SD_ADLIB_REG_VOLUME);
		if (ch >= 0)
		{
			opl_level[ch] = val & 0x3F;
			if (opl_on[ch])
				SD_STDL_Note(ch);
		}
	}
}

static void SD_STDL_Startup(void)
{
	if (sd_stdl_up)
		return;
	sd_stdl_lastHz = STDL_GetHz200();
	sd_stdl_acc = 0;
	if (STDL_AddVBL(SD_STDL_VBL) < 0)
		CK_Cross_LogMessage(CK_LOG_MSG_ERROR, "SD_STDL_Startup: %s\n", STDL_GetError());
	sd_stdl_up = true;
}

static void SD_STDL_Shutdown(void)
{
	if (!sd_stdl_up)
		return;
	STDL_RemoveVBL(SD_STDL_VBL);
	STDL_ToneOff(-1);
	memset(opl_on, 0, sizeof(opl_on));
	pc_period = 0;
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
