BFME2 Accelerator
=================

Two to three times the frame rate in big battles. Same graphics, same everything.

The engine was written in 2006 for a single CPU core, and it still runs that way: one thread
doing all the work while the rest of your processor sits idle. This moves the Direct3D work
to a second core, replaces the engine's slowest routines with modern ones that produce
identical results, and evens out the logic step that causes the stutter in large battles.

It does not write to your game folder or modify game.dat, so mod launchers and their
checksums still pass.


Running it
----------
1. Keep the two files together in one folder - anywhere you like:
      bfme2_accel_loader.exe   and   bfme2_accel.dll
2. Run bfme2_accel_loader.exe
3. Click the game you want.

That's it. The window disappears, the game starts, and the loader closes itself when you quit.

A game it cannot find is greyed out - click it and point at its .exe. Paths are kept in
bfme2_accel.ini next to the loader, and the "Paths..." button lets you edit them.


If something is wrong
---------------------
Antivirus: the loader injects a DLL, which looks like malware to Defender. If it says the DLL
is missing when you can see it, that is what happened - add the folder to your exclusions.

Otherwise read bfme2_accel.log next to the DLL. The first lines say what it recognised.


Known issues
------------
- The game crashes when you quit to desktop. That is a stock 2.02 bug, not this - it does it
  without the accelerator too.
- BFME2 support is new and untested.
- Multiplayer is untested.
