# User playtest sign-off

Tests and gates prove that the port behaves like the original on recorded routes. They do not prove that it
*feels* right. A milestone is only "done" when the person who owns the game has played it and signed here.
Nobody else fills in a line: leave it unsigned until the player has actually played that milestone.

How to sign: play the build named in the line, then replace `[ ]` with `[x]`, and add your name, the date,
the build (`git describe` or the tarball name) and any remarks (what was odd, even if you accept it).

| Milestone | What to play / check | Signed | Who | Date | Build | Remarks |
| --- | --- | --- | --- | --- | --- | --- |
| M1 Boots | `./scooby` starts, title screen, music, no crash in 10 minutes of menus | [ ] | | | | |
| M2 Blake's Hotel | a mystery from START to the unmasking with keyboard / gamepad | [ ] | | | | |
| M3 Ha Ha Carnival | a mystery from START to the unmasking | [ ] | | | | |
| M4 Sound | music and effects sound like the original (no crackle, drift, missing sounds) | [ ] | | | | |
| M5 Mouse | whole mystery with the mouse only: walking, verbs, inventory, dialogue choices, menus | [ ] | | | | |
| M6 Widescreen / resolution (#26) | wide window / fullscreen looks right, nothing cut off, no stretched UI | [ ] | | | | |
| M7 Game saves (#28) | save at three places, quit, `--load-slot`, F8 menu with mouse and keyboard; the game is where you left it | [ ] | | | | |
| M8 Options (#28) | edit the ini, `--options`, rebind a key, change volume and filter, they take effect | [ ] | | | | |
| M9 First run (#28) | on a machine without the ROM next to the binary: picker or drop window, ROM remembered, a wrong file refused | [ ] | | | | |
| M10 Release tarball (#28) | unpack the tarball on your own Linux machine, run `./scooby`, read the README and LICENSES | [ ] | | | | |

Notes for the signer: the game's intro cut scene plays after loading a save (as after typing a password);
Start skips it. Quick states (F5/F9) are whole-machine snapshots and are not game saves.
