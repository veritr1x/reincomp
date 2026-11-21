# ANGLE as this port ships it

The game draws with OpenGL ES; ANGLE turns that into Vulkan. The `libEGL.dll`
and `libGLESv2.dll` in a release's deps zip are ANGLE at commit `890b5d8f`,
built with MinGW-w64 GCC 15.2 for the Vulkan backend only, from:

- the MSYS2 `mingw-w64-angleproject` recipe and its patches, at the commit in
  `sources.json`
- `standalone-gn-fixes.patch`: lets that recipe build outside MSYS2
- `profile-correct-astc.patch`: ASTC textures are decoded by the profile the
  game asks for
- `native-present-scaling.patch`: a window surface can keep a fixed-size
  buffer and be scaled to the window when presented, which is how the game
  renders at one resolution and shows at another

The local performance build also applies `client-vertex-arrays.patch`. It
lets the Windows host check all enabled vertex attributes under one ANGLE
context lock before submitting a draw. Older DLLs use the public GL queries.

`sources.json` pins every repository the build takes (ANGLE and the 14 it
pulls in), and `args.gn` is the build's configuration. Apply the patches in
the order above, after the recipe's own.

There is no script here that runs the build; these files are what it takes to
repeat it by hand.
