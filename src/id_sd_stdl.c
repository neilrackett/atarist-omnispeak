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
// The service's OPL register writes go to STDL's OPL translator, which
// keys the nine channels as notes in tone slots 0-8; the PC speaker is
// slot 9. STDL plays the three most recently keyed on the chip: a
// three-voice square-wave cover of the score, not the OPL sound. STDL
// owns the chip, so its terminate-vector cleanup silences it on any
// exit.

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

void SDL_t0Service(void);

// The PC speaker: one tone slot above the nine STDL_Opl uses.
#define SD_STDL_SPEAKER_SLOT STDL_OPL_CHANNELS

static uint16_t pc_period; // 0 = off

static bool sd_stdl_up;
static volatile bool sd_stdl_locked;
static volatile uint32_t sd_stdl_lastHz;
static volatile uint32_t sd_stdl_acc; // tick fraction, in 1/200ths
static volatile uint32_t sd_stdl_rate = 140;

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

// The AdLib register writes go to STDL's OPL translator, which keys
// tone slots 0-8 for the nine channels.
static void SD_STDL_alOut(uint8_t reg, uint8_t val)
{
	STDL_OplWrite(reg, val);
}

static void SD_STDL_Startup(void)
{
	if (sd_stdl_up)
		return;
	// Open the sound service from the main line: every register write
	// after this arrives from the VBL sound service, and the first
	// key-on must not be the call that installs it.
	STDL_OplReset();
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
	STDL_OplReset();
	STDL_ToneOff(SD_STDL_SPEAKER_SLOT);
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
