/*
Omnispeak: A Commander Keen Reimplementation
Atari ST input backend on STDL
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

// Keyboard and joystick through STDL's IKBD driver. Keen thinks in PC
// scancodes; the ST keyboard was modelled on the PC XT's, so most keys
// map one to one and only the keypad and the ST's own keys need a table.

#include "id_in.h"
#include "id_cfg.h"

#include <stdl/stdl.h>

// IKBD scancode -> PC scancode. 0 = no equivalent.
static const uint8_t in_stdl_scanTable[128] = {
	/* 00 */ 0x00, IN_SC_Escape, IN_SC_One, IN_SC_Two, IN_SC_Three, IN_SC_Four, IN_SC_Five, IN_SC_Six,
	/* 08 */ IN_SC_Seven, IN_SC_Eight, IN_SC_Nine, IN_SC_Zero, IN_SC_Minus, IN_SC_Equals, IN_SC_Backspace, IN_SC_Tab,
	/* 10 */ IN_SC_Q, IN_SC_W, IN_SC_E, IN_SC_R, IN_SC_T, IN_SC_Y, IN_SC_U, IN_SC_I,
	/* 18 */ IN_SC_O, IN_SC_P, IN_SC_LeftBracket, IN_SC_RightBracket, IN_SC_Enter, IN_SC_Control, IN_SC_A, IN_SC_S,
	/* 20 */ IN_SC_D, IN_SC_F, IN_SC_G, IN_SC_H, IN_SC_J, IN_SC_K, IN_SC_L, IN_SC_SemiColon,
	/* 28 */ IN_SC_SingleQuote, IN_SC_Grave, IN_SC_LeftShift, IN_SC_BackSlash, IN_SC_Z, IN_SC_X, IN_SC_C, IN_SC_V,
	/* 30 */ IN_SC_B, IN_SC_N, IN_SC_M, IN_SC_Comma, IN_SC_Period, IN_SC_Slash, IN_SC_RightShift, 0x00,
	/* 38 */ IN_SC_Alt, IN_SC_Space, IN_SC_CapsLock, IN_SC_F1, IN_SC_F2, IN_SC_F3, IN_SC_F4, IN_SC_F5,
	/* 40 */ IN_SC_F6, IN_SC_F7, IN_SC_F8, IN_SC_F9, IN_SC_F10, 0x00, 0x00, IN_SC_Home,
	/* 48 */ IN_SC_UpArrow, 0x00, IN_KP_Minus, IN_SC_LeftArrow, 0x00, IN_SC_RightArrow, IN_KP_Plus, 0x00,
	/* 50 */ IN_SC_DownArrow, 0x00, IN_SC_Insert, IN_SC_Delete, IN_SC_F1, IN_SC_F2, IN_SC_F3, IN_SC_F4,
	/* 58 */ IN_SC_F5, IN_SC_F6, IN_SC_F7, IN_SC_F8, IN_SC_F9, IN_SC_F10, 0x00, 0x00,
	/* 60 */ IN_SC_SecondaryBackSlash, IN_SC_Escape /* Undo */, IN_SC_F1 /* Help */, 0x00, 0x00, IN_SC_Slash, IN_KP_Multiply, IN_SC_Home,
	/* 68 */ IN_SC_UpArrow, IN_SC_PgUp, IN_SC_LeftArrow, IN_KP_Center, IN_SC_RightArrow, IN_SC_End, IN_SC_DownArrow, IN_SC_PgDown,
	/* 70 */ IN_SC_Insert, IN_SC_Delete, IN_SC_Enter, 0x00, 0x00, 0x00, 0x00, 0x00,
	/* 78 */ 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static bool in_stdl_joyEnabled = true;

static void IN_STDL_HandleKey(uint8_t ikbd, bool down)
{
	uint8_t sc = in_stdl_scanTable[ikbd & 0x7F];
	if (!sc)
		return;
	// The keypad's slash and enter are "special" (E0-prefixed) on a PC.
	bool special = (ikbd == 0x65 || ikbd == 0x72);
	if (down)
		IN_HandleKeyDown(sc, special);
	else
		IN_HandleKeyUp(sc, special);
}

static void IN_STDL_PumpEvents()
{
	STDL_Event ev;
	while (STDL_PollEvent(&ev))
	{
		switch (ev.type)
		{
		case STDL_KEYDOWN:
			IN_STDL_HandleKey(ev.key.keysym.scancode, true);
			break;
		case STDL_KEYUP:
			IN_STDL_HandleKey(ev.key.keysym.scancode, false);
			break;
		default:
			break;
		}
	}
}

static void IN_STDL_WaitKey()
{
	// Wait for a key press (the SDL backend does the same with its
	// event queue).
	for (;;)
	{
		STDL_Event ev;
		STDL_WaitVBL();
		while (STDL_PollEvent(&ev))
		{
			if (ev.type == STDL_KEYDOWN)
			{
				IN_STDL_HandleKey(ev.key.keysym.scancode, true);
				return;
			}
			if (ev.type == STDL_KEYUP)
				IN_STDL_HandleKey(ev.key.keysym.scancode, false);
		}
	}
}

static void IN_STDL_Startup(bool disableJoysticks)
{
	in_stdl_joyEnabled = !disableJoysticks;
	STDL_EnableUNICODE(0);
	STDL_JoyKeyEmulation(0);
}

static void IN_STDL_Shutdown(void)
{
}

static bool IN_STDL_JoyPresent(int joystick);

static bool IN_STDL_StartJoy(int joystick)
{
	return IN_STDL_JoyPresent(joystick);
}

static void IN_STDL_StopJoy(int joystick)
{
	(void)joystick;
}

static bool IN_STDL_JoyPresent(int joystick)
{
	// There is no way to tell whether a stick is plugged into port 1,
	// so it is always there; Keen reads keyboard and joystick together.
	return in_stdl_joyEnabled && joystick == 0;
}

static void IN_STDL_JoyGetAbs(int joystick, int *x, int *y)
{
	int vx = 0, vy = 0;
	(void)joystick;
	if (STDL_JoyInputHeld(STDL_JOYKEY_LEFT))
		vx = -1000;
	else if (STDL_JoyInputHeld(STDL_JOYKEY_RIGHT))
		vx = 1000;
	if (STDL_JoyInputHeld(STDL_JOYKEY_UP))
		vy = -1000;
	else if (STDL_JoyInputHeld(STDL_JOYKEY_DOWN))
		vy = 1000;
	if (x)
		*x = vx;
	if (y)
		*y = vy;
}

// Button numbering, matching Keen's default joystick configuration
// (jump = 0, pogo = 1, fire = 2, menu = 3, status = 4). An ST joystick
// has only the first; a pad through an Xpad provider has them all.
static const int in_stdl_buttonInputs[] = {
	STDL_JOYKEY_FIRE, STDL_JOYKEY_EAST, STDL_JOYKEY_WEST, STDL_JOYKEY_NORTH,
	STDL_JOYKEY_START, STDL_JOYKEY_SELECT, STDL_JOYKEY_TL, STDL_JOYKEY_TR};

static const char *in_stdl_buttonNames[] = {
	"Fire / A", "B", "X", "Y", "Start", "Select", "L", "R"};

#define IN_STDL_NUM_BUTTONS ((int)(sizeof(in_stdl_buttonInputs) / sizeof(in_stdl_buttonInputs[0])))

static uint16_t IN_STDL_JoyGetButtons(int joystick)
{
	uint16_t mask = 0;
	(void)joystick;
	for (int i = 0; i < IN_STDL_NUM_BUTTONS; ++i)
		if (STDL_JoyInputHeld(in_stdl_buttonInputs[i]))
			mask |= (uint16_t)(1 << i);
	return mask;
}

static const char *IN_STDL_JoyGetName(int joystick)
{
	(void)joystick;
	return STDL_HavePad() ? "Controller" : "Joystick";
}

static const char *IN_STDL_JoyGetButtonName(int joystick, int index)
{
	(void)joystick;
	if (index < 0 || index >= IN_STDL_NUM_BUTTONS)
		return NULL;
	return in_stdl_buttonNames[index];
}

IN_Backend in_stdl_backend = {
	.startup = IN_STDL_Startup,
	.shutdown = IN_STDL_Shutdown,
	.pumpEvents = IN_STDL_PumpEvents,
	.waitKey = IN_STDL_WaitKey,
	.joyStart = IN_STDL_StartJoy,
	.joyStop = IN_STDL_StopJoy,
	.joyPresent = IN_STDL_JoyPresent,
	.joyGetAbs = IN_STDL_JoyGetAbs,
	.joyGetButtons = IN_STDL_JoyGetButtons,
	.joyGetName = IN_STDL_JoyGetName,
	.joyGetButtonName = IN_STDL_JoyGetButtonName,
	.startTextInput = NULL,
	.stopTextInput = NULL,
	.joyAxisMin = -1000,
	.joyAxisMax = 1000,
	.supportsTextEvents = false,
};

IN_Backend *IN_Impl_GetBackend()
{
	return &in_stdl_backend;
}
