# Roadmap

The first milestone was one frame of *The Lost World*'s attract mode. That
is long past: the reference title plays through stage one, two players can
play it over a network, and it saves and loads its state. What is left is
ordered by what a player notices.

## Next: the rest of the picture

- **What still goes missing.** The Lost World's T-Rex is meant to have target
  circles on it; they are not drawn. Find out whether that is a polygon flag
  the renderer ignores or a 2D layer.

## Then: more of the host

- Higher internal resolution for the 3D. The rasteriser is software and
  fixed at 496x384; rendering the 3D at a multiple and compositing the
  tilemaps over it is the obvious shape.
- Netplay over UDP with rollback, if lockstep's input delay proves too much
  over the internet. Lockstep over TCP is right for a LAN and for Tailscale.
- Netplay across the internet rather than a LAN, and over Tailscale: two
  machines on a LAN stay identical; a NAT'd pair is next.

## Deferred

- **Step 2.x support.** Different CPU clock, Real3D revision and DMA path.
- **Security board (315-5881).** Step 2.x only.
- **Network board.** Used by the linked-cabinet titles.
- **Other titles.** *Scud Race*, *Virtua Fighter 3*, *Sega Rally 2* and the
  rest should follow, since none of this is title-specific -- each needs its
  lift, its safe point and whatever its board revision differs in.

## Out of scope

- Being an emulator. If you want to run Model 3 games, use an emulator.
- Interpreting the game at runtime. The point is native code; the boot
  interpreter is a build-time tool and stays one.
- Distributing anything derived from a ROM. The tool ships; the output never
  does.
