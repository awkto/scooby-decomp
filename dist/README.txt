Scooby-Doo Mystery - native Linux port
======================================

A native port of "Scooby-Doo Mystery" (Sega Genesis / Mega Drive, USA, 1995): the game logic is
decompiled to C, the rest runs on the Genesis Plus GX core.  It is NOT a ROM: you need your own copy
of the game.  This package contains none of the game's data.

What you need
-------------
  * Linux x86-64 with SDL2 (libSDL2-2.0.so.0; Debian/Ubuntu: apt install libsdl2-2.0-0)
  * your ROM: "Scooby-Doo Mystery (USA)", sha1 ccc9542d964de5bdc1e9101620cae40dd98e7127
    (a .md / .bin / .gen file).  Any other file is refused.

Starting
--------
    ./scooby                 finds the ROM by itself and remembers it
    ./scooby --rom FILE      use this file once (it is remembered too)

Where the ROM is looked for, in this order: --rom; the "rom" entry of the ini file (below); a ROM file
in the same folder as the program; rom/baserom.md in the current folder.  If none is found a file
picker opens (zenity or kdialog, when installed); if that is not available either, a window opens and
you drop the ROM file onto it.  You can also drop a ROM onto the running game window to switch to it
(it is used the next time you start).

Controls
--------
  Keyboard  arrows = D-pad    Z or A = A    X or S = B    C or D = C    Enter = Start
  Gamepad   D-pad / left stick, X or Y = A, A = B, B or right shoulder = C, Start, Back = pause
  Mouse     move: the game's crosshair follows the mouse, objects are named as in the original
            left click on the floor: Shaggy walks there (around obstacles); on a door/exit: walks in
            left click on an object: uses the selected verb (or the object's default verb)
            left click on the verb bar / an inventory item / an arrow: selects it
            right click on an object: LOOK at it     right click elsewhere: leave pointer mode
            middle click: Start (pause menu, skip a cut scene)     wheel: inventory page / dialogue choices
            Any key or pad press hands control back to the pad at once.
  Game      F8 game saves   F5 / F9 quick save / load of the whole machine   F6 / F7 pick quick slot
            F3 output filter (nearest / smooth / crt)   F11 or Alt+Enter fullscreen   P pause
            N or . single frame   Tab (hold) fast forward   F12 screenshot   F1 debug overlay   Esc quit

Game saves instead of passwords
-------------------------------
The game normally saves your progress as a 50-letter password.  F8 opens GAME SAVES: 8 slots, each
showing mystery, room, date and play time.  Use the mouse (SAVE / LOAD / DEL buttons) or the keyboard
(Up/Down, Enter = load, S = save, Delete = erase, Esc = close).  Loading a slot restarts the game and
enters that slot's password for you, so you get exactly what typing the password would give you
(including the intro cut scene of the mystery, which Start skips).  The slot files are small text files in
~/.local/share/scooby-decomp/slotN.sav and contain the password, so you can also type one in by hand.
F5/F9 "quick states" are separate: they snapshot the whole machine, not just the story.
Command line:  ./scooby --load-slot N      ./scooby --list-slots

Options
-------
Settings live in ~/.config/scooby-decomp/scooby.ini (created when the ROM is first remembered; edit
freely).  Any option can be overridden for one run:

    ./scooby --options scale=4,filter=crt,volume=60
    ./scooby --options --json          list every option, its default and current value

  filter      nearest | smooth | crt     output scaling filter                   (default nearest)
  scale       1..8                       initial window size in multiples of 320x224       (3)
  fullscreen  on | off                   start fullscreen                                  (off)
  vsync       on | off                   sync to the display                               (on)
  renderer    native | gpgx              C renderer or the emulator core's picture    (native)
  mouse       on | off                   mouse control                                      (on)
  mute        on | off                   no audio                                          (off)
  volume      0..100                     audio volume in percent                           (100)
  rom         path                       the remembered ROM
  key_up key_down key_left key_right key_a key_b key_c key_start
              SDL key names joined with |, e.g.  key_a = Z|Space    key_start = Return|Keypad Enter

Options that change the game itself (cheats and the like) would be marked "game_changing" in the
--json listing and are always off by default; at the moment there are none.

Files
-----
  ~/.config/scooby-decomp/scooby.ini      options       ~/.local/share/scooby-decomp/   saves, quick states
  LICENSES/                               licences of everything in this package (Genesis Plus GX is for
                                          NON-COMMERCIAL use only)
  source/                                 the source of this port and the patches to Genesis Plus GX

Project: https://github.com/awkto/scooby-decomp   (decompilation, docs, tests)
