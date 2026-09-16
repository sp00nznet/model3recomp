# Roadmap

The goal for the first milestone is one specific frame: **The Lost World's
attract mode**. Everything below is ordered by what that actually requires,
not by what is most interesting to write.

## Next: find out why the game never starts its game task

The port runs. The interpreter and the lifted code agree over 30,000 device
accesses and produce identical buffers. The game boots, copies itself into RAM,
enumerates PCI, configures the 53C810, runs SCSI DMA, takes VBlank interrupts,
loads its 769-tile character set, programs the Real3D viewport and LOD table,
and then runs the same handful of system tasks every field, for ever.

The frame body is only ever input gathering (RAM 0x001180A8), a service call
through the pointer at 0x001EED94, and a wait. That pointer holds 0x00117864,
the null `blr` stub the boot installs, and nothing ever replaces it. The
routines that would talk to the Real3D -- the culling-RAM writer at
0x0010F3E4, the trigger at 0x0010F260, the texture port at 0x0010F214 -- are
never dispatched at all.

So the question is not "why does the renderer not draw", it is "what would
install a real per-frame task, and what is it waiting for". The interrupt
library's install routines are at RAM 0x00117BF4 and 0x00117C34, writing the
slots at 0x001EED7C onward; finding their callers and what gates them is the
thread to pull.

Ruled out by measurement, each of which was a real defect that had to be
fixed and none of which was the cause:

| Checked | Result |
|---|---|
| PCI enumeration | was finding no devices at all; fixed, game now configures the 53C810 itself |
| Real3D ready bit at 0x84000000 | the game spins on it with a branch-to-self; fixed |
| Interrupt delivery | was dropped whenever MSR[EE] was clear at the field boundary; fixed |
| Field pacing | three separate bugs, all presenting as silence; fixed |
| Sound board | polled non-blocking; reporting data ready changes nothing |
| I/O board framing | derived from the guest and modelled; the replies are still a stub |

## Then: the Real3D

Once the game submits a scene there is still nothing to draw it. Culling-RAM
node walk, display-list parse, VROM model and texture decode, transform,
light, clip and rasterise. That is board-level work and belongs here.

## Then: something on screen

- **Tilemap renderer.** Four scroll layers, palette, priority. This is what
  draws the 2D parts of attract mode and the service menu, and the service
  menu is the cheapest possible proof that the machine is alive.
- **Real3D Pro-1000.** The big one: culling-RAM node tree walk, display-list
  parse, transform and lighting, texture upload and decode from VROM, and a
  perspective-correct rasterizer. Attract mode is mostly 3D, so this is the
  gate on the milestone.

## Then: the rest of the board

- Inputs wired into `0xF0040000` so buttons and coins reach the game.
- Sound: 68EC000 plus two SCSPs. Attract mode has music; it is not needed for
  a picture.
- Backup SRAM persistence.
- Real-time clock.

## Deferred

- **Step 2.x support.** Different CPU clock, Real3D revision and DMA path. The
  reference title is Step 1.5; generalising before one game works is the
  classic way to get two half-working things.
- **Security board (315-5881).** Step 2.x only.
- **Network board.** Used by the linked-cabinet titles.
- **Other titles.** *Scud Race*, *Virtua Fighter 3*, *Sega Rally 2* and the
  rest should follow once the board is real, since none of this is
  title-specific. They are not a goal until one game runs.

## Out of scope

- Being an emulator. If you want to run Model 3 games, use Supermodel — it is
  excellent, and this project is built from the same public research.
- Interpreting the game at runtime. The point is native code; the boot
  interpreter is a build-time tool and stays one.
- Distributing anything derived from a ROM. The tool ships; the output never
  does.
