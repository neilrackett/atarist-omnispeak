# Omnispeak for the Atari STE

Commander Keen 4-6 (Omnispeak) on the Atari STE and Mega STE, built on
[STDL](https://github.com/neilrackett/atarist-stdl), the planar-native
SDL 1.2 subset for the ST. Work in progress: Keen 4 plays from the
intro through the menus, the world map and the levels; sound is a
square-wave approximation and there is no music yet.

## Requirements

- An Atari STE or Mega STE. The plain ST has no hardware scrolling
  (the port uses the STE's video base, LINEWIDTH and HSCROLL registers
  for Keen's smooth scrolling) and is refused at start-up.
- 4MB recommended; 1MB is untested.
- A hard disk or emulated GEMDOS drive holding the game files.

## Installing

1. Copy `KEEN.TOS` to a folder on the ST.
2. Copy the Keen data files next to it, with uppercase names:
   `EGAGRAPH.CK4`, `GAMEMAPS.CK4`, `AUDIO.CK4` (Keen 4 v1.4 EGA; the
   shareware episode is at https://davidgow.net/keen/4keen14.zip), and
   the Omnispeak files from `data/keen4/` (`ACTION.CK4`, `EGAHEAD.CK4`,
   `GFXINFOE.CK4` and friends). The build copies those into `dist/`.
3. Run `KEEN.TOS` from the desktop. Options go in `OMNISPK.CFG` next to
   it (string values quoted: `logLevel = "normal"`); the game writes
   `OMNISPK.LOG` and its saves there.

The episode is chosen by which data files are present, as on other
platforms.

## Controls

The DOS keys: arrows move, Ctrl jumps, Alt pogos, Space fires, Enter
for the status screen, Esc for the menu, F1 help. Keypad 7/9/1/3 are
the diagonals (Home/PgUp/End/PgDn). Undo is Esc and Help is F1.

A joystick in port 1 is Keen's joystick 1 (fire = jump; pogo and fire
stay on the keyboard). A pad through an Xpad provider adds B (pogo),
X (fire), Y, Start and Select as Keen's buttons 1-5.

## Building

Cross-compiles with `m68k-atari-mint-gcc` via
[atarist-toolkit-docker](https://github.com/sidecartridge/atarist-toolkit-docker).
STDL comes from the `stdl/` submodule (pinned at v1.6.0, which has
the hardware-scroll module, the tone device and the OPL translator) and is built into `stdl/libstdl.a`
automatically:

```
git submodule update --init --recursive
STCMD_NO_TTY=1 stcmd make -C src -f Makefile.atarist
```

Without the submodule the Makefile falls back to a sibling checkout
`../atarist-stdl`; `stcmd` mounts one directory, so that layout needs
the parent of both mounted
(`ST_WORKING_FOLDER=$PWD stcmd make -C atarist-omnispeak/src ...`).

`DEBUG=1` builds `KEEND.TOS` with `-g -O1` and traces log messages to
the console;
`EXTRA_CFLAGS=-DCK_STDL_PROFILE` adds start-up and level-load timing
to the log. `make -f Makefile.atarist run` launches the result in
Hatari on the host.

## How it works

- `src/id_vl_stdl.c` is the video backend, shaped like the DOS EGA one:
  the screen is a pair of 336x224 pages whose base pointers drift as
  Keen scrolls tile by tile, and `STDL_SetScrollWindow` shows the
  320x200 window at the pixel offset the refresh manager asks for. All
  drawing is planar; 16x16 tiles, bitmaps and sprites are converted to
  ST interleaved planar format once when the cache manager expands
  them (`src/id_vl_stdl.h`), so the hot blits are word operations.
- `src/id_in_stdl.c` maps IKBD scancodes onto Keen's PC scancodes.
- `src/id_sd_stdl.c` runs Keen's 140/560Hz sound service from STDL's
  VBL callback paced by the 200Hz system counter, so the 70Hz game
  clock is exact. The AdLib music and effects (streams of OPL
  register writes) go to STDL's OPL translator one write at a time;
  it keys the nine channels as notes in STDL's tone device, with each
  note's volume from the OPL carrier level, the PC speaker has a slot
  of its own, and STDL plays the three most recently keyed on the
  YM2149's tone voices.
- Engine changes for the 68000: a table-driven Huffman decoder with a
  68000 assembly inner loop, batched graphics-chunk reads (one GEMDOS
  read per run of chunks), a hashed block index in the memory manager,
  a tokenizer that walks its buffer directly, and the big-endian font
  fix (the cache manager was byte-swapping a header the font reader
  already read byte-wise).

## Known limitations

- Music and effects are a three-voice square-wave cover of the AdLib
  score, not the OPL sound: the nine OPL channels and the PC speaker
  share the YM2149's three tone voices by last-note priority (STDL's
  tone device), so chords beyond three notes lose their oldest note.
  Verified in Hatari's sound capture.
- Start-up parses the Omnispeak data files: about 4 s on a Mega STE,
  8 s on an STE. Level 1 of Keen 4 takes about 6 s to load on a Mega
  STE and 12 s on an STE (Huffman expansion, sprite pre-shifting and
  chunk reads, in that order); a batched reader and an assembly
  decoder are the obvious next steps.
- In the level the game runs at about 28 frames per second on an
  emulated Mega STE, drawing-bound; the display itself updates every
  vertical blank (STDL arms the STE video base early so a scroll is
  on screen at the next blank rather than the one after).
- The border colour tricks of the DOS version are ignored (the ST
  border is always colour 0).
- None of this has run on real hardware yet, only in Hatari. The one
  part with a known hardware risk is the smooth scrolling: STDL times
  it against the STE video counter reading back during the vertical
  blank, which emulators and real silicon have been known to differ
  on. If it were wrong the picture would scroll in whole 16-pixel
  steps with the fine offset stuck, rather than tearing.
