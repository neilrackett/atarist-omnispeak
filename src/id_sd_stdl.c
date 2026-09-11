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

// The DOS game runs its sound service from the PIT interrupt at 140Hz
// (560Hz with AdLib music) and derives the 70Hz game clock from it. Here
// the service runs from STDL's 50Hz VBL callback, paced by the 200Hz
// system counter so the clock stays exact whatever the display rate:
// the ticks due since the last VBL are run in a burst. That keeps the
// game speed right; sound-effect steps are quantised to 20ms.
//
// Sound comes out of the YM2149 through STDL's speaker: PC speaker
// effects are square waves already, and AdLib effects are followed by
// their channel-0 key-on frequency, which makes them square waves too.
// Music is not played yet.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "id_sd.h"
#include "ck_cross.h"

#include <stdl/stdl.h>

#define PC_PIT_RATE 1193182
#define SD_STDL_MAX_BURST 64
#define SD_STDL_VOLUME 12

void SDL_t0Service(void);

static bool sd_stdl_up;
static volatile bool sd_stdl_locked;
static volatile uint32_t sd_stdl_lastHz;
static volatile uint32_t sd_stdl_acc; // tick fraction, in 1/200ths
static volatile uint32_t sd_stdl_rate = 140;

// AdLib channel 0 tracking for the square-wave approximation.
static uint8_t sd_stdl_alFreqLo;
static bool sd_stdl_alKeyOn;
static int sd_stdl_alFreq;

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

#ifdef CK_STDL_PROFILE
static int sd_stdl_traced;
#endif

static void SD_STDL_PCSpkOn(bool on, int freq)
{
#ifdef CK_STDL_PROFILE
	if (sd_stdl_traced < 8)
	{
		sd_stdl_traced++;
		CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "SD_STDL: speaker %s %dHz\n", on ? "on" : "off", freq);
	}
#endif
	if (on && freq > 0)
		STDL_SpeakerOn(freq, SD_STDL_VOLUME);
	else
		STDL_SpeakerOff();
}

static void SD_STDL_alOut(uint8_t reg, uint8_t val)
{
	// Channel 0 carries the sound effects: follow its frequency and
	// key-on bit (registers A0 and B0) onto the speaker voice.
	if (reg == SD_ADLIB_REG_NOTE_LO)
	{
		sd_stdl_alFreqLo = val;
	}
	else if (reg == SD_ADLIB_REG_NOTE_HI)
	{
		bool keyOn = (val & 0x20) != 0;
		int block = (val >> 2) & 7;
		int fnum = ((val & 3) << 8) | sd_stdl_alFreqLo;
		// f = fnum * 49716 / 2^(20 - block)
		int freq = (int)(((uint32_t)fnum * 49716UL) >> (20 - block));
		if (keyOn)
		{
#ifdef CK_STDL_PROFILE
			if (sd_stdl_traced < 8)
			{
				sd_stdl_traced++;
				CK_Cross_LogMessage(CK_LOG_MSG_NORMAL, "SD_STDL: adlib key-on %dHz\n", freq);
			}
#endif
			if (!sd_stdl_alKeyOn || freq != sd_stdl_alFreq)
				STDL_SpeakerOn(freq, SD_STDL_VOLUME);
		}
		else if (sd_stdl_alKeyOn)
		{
			STDL_SpeakerOff();
		}
		sd_stdl_alKeyOn = keyOn;
		sd_stdl_alFreq = freq;
	}
}

static void SD_STDL_Startup(void)
{
	if (sd_stdl_up)
		return;
	// Installs STDL's sound tick from the main program (only a speaker
	// "on" claims it), so that later speaker calls from the VBL callback
	// only set state. Volume 0 keeps it silent; the tick turns it off.
	STDL_SpeakerOn(440, 0);
	STDL_SpeakerOff();
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
	STDL_SpeakerOff();
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
