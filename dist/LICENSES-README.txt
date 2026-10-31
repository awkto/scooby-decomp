What is in the `scooby` binary and under which terms
====================================================

* Genesis Plus GX (commit 60bee4c724859710ce05672239e5b601ad01382e of https://github.com/libretro/Genesis-Plus-GX),
  compiled in statically with the patches in source/tools/patches/.  Licence: GPGX-LICENSE.txt, which also
  carries the licences of the parts bundled in it (Nuked OPN2 and OPLL: LGPL 2.1; minimp3: CC0; Tremor;
  zlib; libretro-common: MIT).  Its main terms:  NOT FOR SALE AND NOT FOR COMMERCIAL USE - redistributions
  may not be sold nor used in a commercial product or activity, and a modified version must come with the
  complete source (source/ here holds the patches and all of this port's source; build instructions are in
  source/port/CMakeLists.txt and the project README).
* SDL2: dynamic link to the system library, zlib licence (SDL2-LICENSE.txt).
* The port itself (source/port/): hand-written C that replaces the game's 68000 code function by function.
  It contains no ROM data.  Because it is combined with Genesis Plus GX it is distributed on the same
  non-commercial terms.
* The game "Scooby-Doo Mystery" is (c) 1995 Sunsoft / Acclaim / Illusions Gaming Company and is NOT included.
  You need your own copy of the ROM.

This package was scanned with tools/py/romscan.py (every 64-byte window of the ROM against every byte of
every file, plus decoded text data): no run of 256 bytes or more of the ROM is in it.
