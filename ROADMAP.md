# Roadmap

The goal for the first milestone is one specific frame: **The Lost World's
attract mode**. Everything below is ordered by what that actually requires,
not by what is most interesting to write.

## Next: close the interpreter/native divergence

The boot interpreter reaches the game's main loop and the game draws into
tilemap VRAM under it. The native build does not get that far, despite running
the same lifted instruction set. One of them is wrong and they can be compared.

- Step both from the same reset state and diff the register file and RAM at
  intervals until they part company. That is the conformance harness the repo
  owes anyway, and it pays for itself immediately here.
- Likely suspects, in order: an instruction whose C translation differs from
  the interpreter's Python, interrupt delivery timing, and the SCSI trigger
  conditions.

Once they agree, everything below is reachable.

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
