# Testing gameplay under Proton

Add the portable `Nier Reincarnation.exe` to Steam as a non-Steam game and
select Proton in **Properties → Compatibility**. Start the shortcut, import
your resources and select **Deploy**. A restored save can prompt for additional
field resources after the title screen. Use the same shortcut to relaunch the
installed game.

## Check keyboard movement

1. With the game closed, set the shortcut's launch options to
   `REINCOMP_TRACE_DESKTOP_INPUT=1 %command%`. If a previous
   `.bunker-player/game/desktop-input.log` exists, rename it before starting.
2. Enter a playable scene with room to move. Finish any dialogue, turn off
   auto-run and stay still for a few seconds.
3. In a **3D field**, hold **W**, release, then hold **S** and release.
   In a **2D storybook**, use **D** followed by **A**. Hold each key for about
   six seconds and pause briefly between keys. Stay in that scene for a few
   more seconds, then close the game.
4. From the repository root, run the checker with Python 3.8 or newer, using
   `2D` or `3D` to match the scene. Repeat from step 1 for the other scene.

```sh
python3 build/native_player/tests/check_proton_gameplay.py 3D "/path/to/NieR Reincarnation/.bunker-player/game"
```

The checker reads the last two movement gestures, verifies the scene type and
reports the world positions of actors that were still before input, moved, and
reversed direction. Remove the tracing launch option after testing.

## Tested configuration

The 0.2.7 portable player ran from a folder named `NieR Reincarnation` under
GE-Proton 11-12 on Ubuntu 24.04.4 in WSL2, using Xvfb and Mesa software
rendering at 960 × 540. The 3D field (`tw002001`) passed after **Deploy** and
again after relaunching the same EXE. The 2D storybook (`pb_ma0100001`)
passed after its intro. Both gameplay checks used a populated field asset cache.
