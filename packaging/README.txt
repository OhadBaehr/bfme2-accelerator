BFME2 Accelerator 2.0
=====================

A smooth 60 frames a second at the stock game speed, even scrolling, faster loading and a
higher frame rate in big battles - for The Rise of the Witch-king 2.02 and the mods built on
it (Age of the Ring, Edain and the rest). Same game, same rules.

The engine was written in 2006 for a single CPU core, and it still runs that way: one thread
doing all the work while the rest of your processor sits idle. The accelerator is a DLL that a
small loader puts into the game when it starts. It moves the Direct3D work to a second core,
replaces the engine's slowest routines with ones that produce the same results, and draws the
picture more often than the game's own 30 frames.

It does not write to your game folder or modify game.dat, so mod launchers and their
checksums still pass. Close the game and it is gone.


Running it
----------
1. Keep the files together in one folder - anywhere you like:
      bfme2_accel_loader.exe   bfme2_accel.dll   bfme2_accel.ini
2. Run bfme2_accel_loader.exe
3. Click the game you want.

That's it. The window disappears, the game starts, and the loader closes itself when you quit.

A game it cannot find is greyed out - click it and point at its .exe. Paths are kept in
bfme2_accel.ini next to the loader, and the "Paths..." button lets you edit them.

Coming from 1.0: replace all the files, the settings file included. Without its [Engine] part
the 60-frame picture, the in-between frames, the even scrolling and the shared draws stay
switched off.


What it does
------------
All of this is on as shipped. Every item is a line in bfme2_accel.ini with its explanation
above it, and can be switched off there.

- Direct3D on its own core. Everything the game sends to the graphics card is queued and
  carried out by a second thread. (Scroll Lock switches it off and on while you play, for a
  before/after look at the frame rate.)
- 60 pictures a second at the stock game speed. The game's own cap is 30 because its logic
  runs once per frame; here the logic stays at 30 steps a second and the picture is drawn
  twice as often, with animation carried on between the steps.
- In-between frames. In a big battle the game itself cannot draw 60 frames, so the render
  thread draws an extra one between every two of the game's - every unit, object and the
  camera halfway. Real geometry through the game's own shaders. (Ctrl+F10)
- Even scrolling. While the camera scrolls, exactly one picture goes up for each refresh of
  your screen, shown from where the camera is at that moment, so a hitch of the game is not a
  hitch of the scroll; and the scrolling speed no longer depends on the frame rate. A frame
  counter reads your screen's rate while you scroll. (Ctrl+F8)
- Fewer draw calls: runs of the same unit model are drawn in one call. (Ctrl+F11)
- Faster loading: textures are prepared on all cores and kept on disk for the next time (the
  texcache folder next to the DLL, 6 GB at most - TextureCacheMB), files are read ahead and
  opened before the game asks for them, ground textures are built on several cores.
- No faction limit for computer players: a computer opponent of any faction plays on any map
  (FactionAI - see "Multiplayer and saves").
- The game's own crash when a skirmish is left through Exit no longer happens.

Not switched on as shipped, because they change the picture. If your graphics card is from
the last ten years they are worth having - set them in bfme2_accel.ini:

- SmoothShadows = 1    shadow edges as an even gradient instead of visible squares
- ShadowMapScale = 2   a shadow map twice as fine on every map
- ShadowMapMatch = 1   maps that stretch their shadows over a long view (Helm's Deep) get a
                       proportionally larger shadow map
- TearFree = 1         with vsync off under DXVK: no tear line (DXVK's own tear-free mode)


If something is wrong
---------------------
Antivirus: the loader puts a DLL into another program, which is what malware does too, and
scanners flag it for that. If the loader says the DLL is missing when you can see it, that is
what happened - add the folder to your exclusions.

The two programs are signed (see "Checking the files"), but with a certificate made by the
author, not one bought from a certificate authority: Windows shows the signature without
vouching for it, and a scanner does not trust it either.

Otherwise read bfme2_accel.log next to the DLL. The first lines say which game it recognised
and what was switched on; a line that says "Please report" is one the author wants to see.

To find out whether a problem comes from one part, switch that part off in bfme2_accel.ini
(InBetweenFrames, PanPictures, MergeDraws, FactionAI ...), or set FpsLimit = 0 and LogicHz = 0
for the game's own 30 frames. Start the game without the loader and none of this is there.


Multiplayer and saves
---------------------
Multiplayer has not been tested.

Everything except one item changes pictures and timing only: the simulation computes what it
always did. The exception is FactionAI, which gives a computer player a side the map did not
carry: in a network game every player would need it on, and a saved game made with such a
computer player needs it on to load. Set FactionAI = 0 if you want none of that.


What it was tested on
---------------------
One machine: Windows 11, an Intel ten-core, an NVIDIA card, Age of the Ring on The Rise of the
Witch-king 2.02 with DXVK as the game's Direct3D 9 (a d3d9.dll in the game folder). Windows'
own Direct3D 9, other graphics cards, and processors with two or four cores are untested with
2.0. The Battle for Middle-earth II without the expansion: the loader lists it; nothing more
is known.

A game build it does not know gets only the parts that do not depend on that build; a program
that is not this engine gets nothing at all. The log says which.


Checking the files
------------------
bfme2_accel_loader.exe and bfme2_accel.dll carry a digital signature (right-click, Properties,
Digital Signatures): "OH1A". The certificate's fingerprint and the SHA-256 of the
download are on the project page:

    https://github.com/OhadBaehr/bfme2-accelerator

The source is there too, under the MIT licence. This is an unofficial fan project with no
connection to Electronic Arts.
