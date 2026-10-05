# model3recomp

```
 #   #   ###   ####   #####  #          ###
 ## ##  #   #  #   #  #      #         #   #
 # # #  #   #  #   #  ####   #             #
 #   #  #   #  #   #  #      #          ###
 #   #   ###   ####   #####  #####     #####

 Sega Model 3 Hardware Runtime for Static Recompilation
```

> Link a statically recompiled Model 3 game against native C hardware. No
> emulator, no interpreter — the PowerPC program becomes your executable, and
> this library is the arcade board it runs on.

Part of the sp00nznet recomp family, and conforming to its house style:
`model2recomp`, `cps1recomp`, `pacrecomp`, `lindberghrecomp` and successors.

**Title-agnostic.** Nothing here knows what game it is running. The memory map,
interrupt controller, tilemap and Real3D interfaces all come from the board,
not the title. *The Lost World: Jurassic Park* (1997) is the reference game it
is being built against, so the worked examples are its.

## Status

**Alpha, and it plays a game.** *The Lost World: Jurassic Park* runs on this
board from the reset vector through its whole attract cycle and through stage
one to the T-Rex, textured, lit and translucent in 3D, with the tilemaps
scrolling and fading over it, with its music and sound effects, aimed and
fired with a mouse or a gamepad, and two players over a network.

![The Lost World on model3recomp](https://raw.githubusercontent.com/sp00nznet/lostworld/main/docs/hero.gif)

What it does not do yet: Step 2.x boards. Everything the reference title
needs from a Step 1.5 board works.
The hardware is implemented from MAME's Model 3 driver and its
documentation of the Real3D and tilemap chips (BSD-3-Clause), and from
measuring what the game itself does.

## What Is This?

Static recompilation replaces an arcade CPU with native code: you lift the
machine code to C, compile it for the host, and it runs directly on your
processor. That gets you a CPU — it does not get you a *machine*. The lifted
code still expects to write a display list at `0x98000000` and have something
draw it.

**model3recomp is that something.** It is the rest of a Sega Model 3 board as a
plain C library, plus the lifter that produces the game half.

## The Board

Model 3 (1996) is the hardware behind *Virtua Fighter 3*, *Scud Race*,
*Sega Rally 2*, *Daytona USA 2* and *The Lost World*.

| Part | Chip | Role |
|---|---|---|
| Main CPU | PowerPC 603e @ 66—166 MHz | Game logic. **This is what gets recompiled.** |
| Graphics | Real3D Pro-1000 | Geometry, lighting, texturing — descended from Lockheed Martin flight simulators |
| Tilemaps | Sega custom | Four scroll layers, HUD and 2D backgrounds |
| Sound | 68EC000 + 2× SCSP | Separate board, driven over a latch |
| DMA | NCR 53C810 SCSI | Step 1.x moves bulk data with it |
| Bridge | Motorola MPC105/106 | Host-to-PCI |
| Security | 315-5881 | Step 2.x challenge/response |

## Hardware Coverage

Honest status. "Done" means it does what the reference title needs so far.

| Subsystem | Status | Notes |
|---|---|---|
| PPC 603e decoder | **Done** | 100% of the 603e ISA vs capstone across a 2 MB ROM |
| PPC 603e -> C lifter | **Done** | 0.048% untranslated; output compiles clean at `/W3` |
| ROM loader | **Done** | Self-verifying interleave — proves itself against the vector table |
| CPU context | **Done** | GPR/FPR/CR/XER/LR/CTR/MSR/SPR, decrementer and time base |
| Memory bus | **Done** | Full 32-bit map: RAM, CROM fixed, data ROM and banked, VRAM, culling/polygon RAM, devices |
| Function dispatch | **Done** | Hash table from guest address to native function |
| Boot interpreter | **Done** | Runs the reset path, recovers the CROM->RAM segment map, snapshots RAM |
| Computed-jump handling | **Done** | `mtlr`+`blr` told apart from a real return -- the boot handoff depends on it |
| Indirect-target feedback | **Done** | Runtime func-table misses feed back through `--entries` |
| Spin diagnosis | **Done** | The bus names the register a stalled guest is polling |
| Differential conformance | **Done** | Interpreter and lifted code agree over 26,000+ device accesses |
| PCI configuration | **Done** | The Real3D and the 53C810 answer their probes |
| Interrupt controller | **Done** | Pending/enable/ack, VBlank on a work-paced field clock |
| I/O board handshake | **Partial** | Strobe latch is right; the serial ready line is toggled, not driven |
| 53C810 SCSI DMA | **Done** | SCRIPTS memory move / jump / interrupt — what Step 1.x uses to feed the Real3D |
| Tilemap generator | **Done** | Four layers, 4 and 8 bpp, X/Y and per-line scroll, A/A' stencil, priority, colour offsets |
| Real3D: scene | **Done** | Viewports in priority order with their own frustum and rectangle, culling-node tree with siblings, LOD tables, matrices, texture offsets, colour tables |
| Real3D: polygons | **Done** | Lighting, Gouraud, translucency, discard and double-sided bits, perspective-correct |
| Real3D: rasteriser | **Done** | Software, perspective-correct, banded across every core -- byte-identical to one thread |
| Real3D: textures | **Done** | FIFO and VROM uploads, 8- and 16-bit, all twelve texel formats, bilinear, mirroring |
| Real3D: not yet | **Partial** | No spotlight (The Lost World never turns it on) |
| Inputs | **Done** | Buttons, two light guns (mouse, gamepad, Sinden), latched once a field |
| Light gun board | **Done** | Serial protocol, both guns, off-screen reload |
| Save states | **Done** | At a game-supplied safe point; exact across processes |
| Netplay | **Done** | Two players, lockstep over TCP, desync detection |
| Battery RAM | **Done** | Loaded at boot, saved on quit |
| 93C46 EEPROM | **Done** | Bit-banged through the I/O board, kept with battery RAM -- where a game keeps its settings and its country |
| Sound board | **Done** | 68000 (Musashi, interpreted) and two SCSPs (ported from MAME), on their own thread, deterministic; output matches MAME |
| Security board | **Not implemented** | Step 2.x only; the reference title does not use it |

### Boot: why there is an interpreter in a static recompiler

`tools/ppc_interp.py` is a build-time tool, not a runtime. It exists because
the RAM layout cannot be recovered statically -- see below -- and it pays for
itself three times over: it recovers the segment map, it snapshots RAM for the
lifter, and it is the conformance oracle, since it and the lifted code are two
implementations of one instruction set and must agree.

It must not grow into a general Model 3 emulator. If it does, the project has
failed at its own premise.

### The thing that shapes the whole design

Model 3 games do not run from ROM. The Lost World's interrupt library installs
a handler at `0x00117864` — a **RAM** address — and the bulk of the game is
copied out of the 32 MB of banked CROM into RAM at boot.

The copy is not verbatim, so the RAM layout cannot be recovered by inspecting
the ROM. The answer is to boot the machine before lifting it: a small PowerPC
interpreter runs the reset path against the same bus this library provides,
stops when the guest branches into RAM, and the RAM snapshot is what gets
lifted. That interpreter is `tools/ppc_interp.py`, and it doubles as the
conformance oracle — it and the lifted code are two implementations of one
instruction set, and they must agree.

Full detail, with the disassembly it was derived from, is in
[docs/technical/execution-model.md](docs/technical/execution-model.md).

## What the bring-up found

None of this came from documentation. Each one was read out of the game, and
each one is the kind of bug that produces silence rather than a crash — which
is why they are written down here and in
[docs/technical/execution-model.md](docs/technical/execution-model.md) rather
than just fixed.

### The game runs from RAM

The Lost World's interrupt library installs its handler at `0x00117864` — a RAM
address. The boot copies the game out of CROM into RAM through a table-driven
loop at `0xFFF00354` that walks `(src, dst, count)` triples:

| RAM | from CROM | size |
|---|---|---|
| `0x000000..0x0FFFFC` | `0x000000` | 1 MB, verbatim |
| `0x100000..0x13B2D4` | `0x120004` | 237 KB |

So the program ROM alone never tells you what code exists or where. That is
why a static recompiler here ships with an interpreter.

### `blr` is not always a return

The boot hands control to the game with:

```
lis  r0, 0
mtlr r0
blr              ; jump to RAM 0x00000000
```

Translating every unconditional `blr` as `return;` turns that into a no-op: the
port drops out of the guest, renders frames forever, and never reports
anything. The two cases are told apart by where LR came from — restored from
the stack means a return, built from an immediate means a jump. Across the
whole 2 MB ROM this fires exactly once.

### Interrupts are on a timer, not a poll

Model 2 busy-waits on a video-status bit, so answering that read *is* the
frame. Model 3 does not. The main loop waits on a RAM flag that only the VBlank
handler sets:

```
0x00118284  lwz r0, -0x126C(r29) ; mtlr r0 ; blrl
0x00118290  lbz r0, 1(r30)       ; flag at RAM 0x6ED
0x001182A0  bc  0x00118284       ; spin
```

A frame hook that waits to be asked never fires. And the runtime has to take
control somewhere the guest actually goes — `func_table_call()` alone is not
enough, because the guest spends long stretches polling a device without
dispatching through a pointer, so the bus is a hook point too.

### Three registers that hang the boot if they lie

| Register | What it must do | Symptom if not |
|---|---|---|
| `0xF0100008` | byte register, reads back what was written | spins forever on CROM bank select |
| `0xF0040000` | latch; the strobe bit must mirror, both edges | one of two spin loops never exits |
| `0xF0040004` | serial ready line must change state | I/O init never completes |

### The SCSI DMA writes to the Real3D

Step 1.x moves bulk data with the 53C810's SCRIPTS processor. It is
little-endian — the game writes `DSP = 0x4C2C1A00`, meaning address
`0x001A2C4C` — and the first real program there is:

```
C000000C   memory move, 12 bytes
001BAA50   from work RAM
9C000000   to the Real3D texture port
98080000   INT
```

A DMA engine whose idea of a valid destination covers only memory rejects that,
the `INT` never fires, and the guest polls `0xC1000014` about twenty million
times.

### An entry is not a function boundary

The game's entry point is nine consecutive `bl` instructions — an init chain.
Bounding a lifted function at the next *discovered* entry cut it off after five
of them. Only a `bl` target or a stack-frame prologue starts a function; a
jump-table arm or an address built by `lis`/`addi` is an extra way *into* code
that already belongs to one.

## Getting Started

### Prerequisites

- **CMake** 3.20+
- A **C17** compiler (MSVC 19.3+, GCC 11+, Clang 14+)
- **SDL2** (optional — without it the library builds without the platform
  layer, and the tests still run)
- **Python** 3.9+ for the tools
- **capstone** (`pip install capstone`) — only for the decode conformance
  harness, never for the shipped runtime
- A Model 3 romset you dumped or own. **None is included, and none ever will
  be.**

### 1. Build the library and run the tests

```
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release
```

Expected:

```
all PPC semantic checks passed
```

### 2. Assemble the program ROM

```
python tools/rom_loader.py /path/to/lostwsga.zip build/lw_prog.bin
```

Expected:

```
program EPROMs : epr-19936.20, epr-19937.19, epr-19938.18, epr-19939.17
image          : build/lw_prog.bin  (0x200000 bytes)
maps at        : 0xFFE00000
reset vector   : 0xFFF00100
verified       : 7/7 exception vectors
```

The loader works out which chips are the program ROMs by trying interleaves
and checking the PowerPC exception vector table, so "verified 7/7" means it
proved the layout rather than assumed it.

### 3. Check decoder coverage (optional)

```
python tools/check_decode.py build/lw_prog.bin --base 0xFFE00000
```

Expected:

```
603e-valid reference   : 315587
coverage vs 603e ref   : 100.00%
decode gaps            : 12
```

The remaining gaps are data words that capstone reads as 64-bit or AltiVec
instructions a 603e cannot execute.

### 4. Recompile

```
python tools/ppc_lifter.py build/lw_prog.bin build/lifted lw \
    --base 0xFFE00000 --stats
```

Expected:

```
functions      : 2014
instructions   : 342753
files          : 11 + funcs.h + register.c
unimplemented  : 164  (0.048%)
```

`build/lifted/` now holds C source that compiles against this library. **That
generated source is not distributable and is gitignored** — you run the tool
on your own dump.

## Usage

The generated code calls into the runtime; your `main()` sets the board up and
hands over:

```c
#include "model3recomp/model3recomp.h"
#include "lw_funcs.h"

int main(void)
{
    m3_config_t cfg = {0};
    cfg.step  = M3_STEP_1_5;
    cfg.title = "The Lost World";
    cfg.roms.crom = crom_image;          /* from rom_loader.py */
    cfg.roms.crom_size = crom_size;

    /* All optional. */
    cfg.ini_path     = "lostworld.ini";  /* window and control settings     */
    cfg.nvram_path   = "lostworld.nv";   /* battery RAM, kept between runs  */
    cfg.state_prefix = "lostworld";      /* save states: lostworld.<n>.m3s  */
    cfg.safepoint_pc = 0x00002100u;      /* see "Save states" below         */

    if (!model3recomp_init(&cfg)) return 1;
    lw_register_all();                   /* generated */
    model3recomp_run();                  /* enters at 0xFFF00100 */
    return 0;
}
```

### The window

On Windows the window has a menu bar; the same settings live in the ini file
and the hotkeys work everywhere.

| Menu | |
|---|---|
| File | save and load state (F5 / F7, slot F6), reset, quit |
| Video | window size 1–4x, fullscreen (F11, Alt+Enter), sharp or bilinear, scanlines, 4:3 or square pixels |
| Sound | mute (F9), volume |
| Controls | mouse aims player 1; gamepad as player 1 or 2; cursor hidden, crosshair or pointer; Sinden border |
| Debug | the game's own cheats: toggles (`m3_cheat_add()`) and one-shot actions (`m3_cheat_add_action()`); and its options (`m3_option_add()`, The Lost World's region), which restart the game when changed |
| Multiplayer | host, join, disconnect, input delay |

Holding **Tab** runs flat out. A **Sinden** light gun works as an absolute
mouse: turn on the border so it can find the screen, and map its off-screen
reload to the right button.

### Inputs

Inputs are latched once a field into an `m3_input_t` (`input.h`) and every
read the guest makes in that field sees the same record. The platform layer
reports the host's devices; scripted input for harnesses sits on top
(`M3_COIN_AT`, `M3_START_AT`, `M3_FIRE_EVERY`, `M3_GUN_X`/`Y`, `M3_BUTTONS`,
`M3_CHEATS`), and netplay merges the two machines' records into one.

### Save states

A recompiled game keeps half its state on the host's call stack, so a
snapshot can only be taken where that stack is the same every time: a
**safe point**, a guest address the game passes once a field from the same
caller. The lifted code reports function entries (`M3_FN`) and backward
branches (`M3_LOOPED`); when one matches `safepoint_pc`, a pending save or
load happens right there. A state is everything the guest can observe -- RAM,
battery RAM, VRAM, culling and polygon RAM, the texture sheet, the CPU and
device registers -- packed by zero runs to about 13 MB, and a state saved in
one process and loaded in another reaches byte-identical frames.

Finding the safe point is a per-title job; The Lost World's is the branch back
to the top of its state loop. `M3_STATE_SAVE_AT=<field>[,slot]` and
`M3_STATE_LOAD=<slot>` drive it from a script.

### Netplay

The board is deterministic -- fields are paced by guest work, not the clock --
so two machines that boot together and see the same inputs on the same
fields stay identical. Netplay trades inputs and nothing else: each side
sends its own a few fields ahead and waits for the other's before each field.
The host is player 1. Battery RAM goes from host to joiner at connect, cheats
come from the host, and every two seconds both sides compare a hash of guest
RAM, so a desync is reported the moment it happens.

```
M3_NETPLAY=host:7777                      # or the menu's Host
M3_NETPLAY=join:100.101.102.103:7777      # a LAN, Tailscale or forwarded address
M3_NET_DELAY=3                            # fields; the host's setting wins
```

A session starts at power-on, because the guest cannot be reset in place:
the menu's Host and Join restart the program. Game options and cheats travel
in the input record, so the host's choices hold on both machines.

It is tested across two machines on a LAN: both play a script, each logs
its RAM hash every ten seconds, and the two logs agree.

### The one routing you cannot get wrong

If you write your own bus layer, keep these two:

```c
/* 0xF0100018, read  -- the frame boundary */
case 0x18: return irq_status_read();
/* 0xF1180010, write -- the ack the guest spins on */
case 0x10: irq_ack(v); return;
```

Miss the second and the game never leaves its interrupt handler. See the
execution-model doc; it has the disassembly of the spin.

## Conformance

Three layers, all of which have caught real bugs:

**Decoder.** `tools/check_decode.py` scores `tools/ppc_disasm.py` against
capstone over a whole ROM and fails on any gap in the 603e instruction set.

**Semantics.** `tests/test_ppc.c` covers the operations where a plausible C
translation is quietly wrong — the rotate mask when it wraps, shifts of 32 or
more, carry and overflow rules, CR bit numbering.

**Differential.** The interpreter and the recompiled game are two
implementations of one instruction set, so they can be run against each other.
Both write an ordered log of every device access — `M3_TRACE_IO=<path>` for the
runtime, `--trace-io` for the interpreter — and the first line that differs is
where they stop agreeing:

```
  #     interpreter              native
  7268  W F118000C 4 07000000    W F118000C 4 07000000
  7269  R F0100018 4 02000000    R F0100018 4 03000000   <<<
```

That one found the runtime raising both VBlank edges in the same tick. An
earlier run found the interpreter ignoring byte lanes on device registers. The
two now agree over 30,000 accesses.

## Building from source

Nothing but CMake and a C17 compiler. SDL2 is found if present and skipped if
not. The Python tools have no dependencies beyond the standard library, except
`check_decode.py`, which needs capstone.

### Sound

The sound board is a 68000 running a 512 KB driver, two Yamaha SCSPs, and
an i8251 UART carrying the PowerPC's commands to the first SCSP's MIDI port.
The 68000 is interpreted ([Musashi](https://github.com/kstenerud/Musashi)):
it is a separate 11.3 MHz processor and costs a couple of milliseconds a
field. The SCSPs are a C port of MAME's. The board runs a field at a time on
its own thread, one field behind the PowerPC, and the two sides trade UART
bytes only at the field boundary -- so it is as deterministic as the rest of
the machine, and save states and netplay carry it. Hand the runtime the
68000 program and the wave ROM in `m3_roms_t.sndrom` / `samples`.
`M3_WAV=<file>` writes the audio out, `M3_NO_SOUND` leaves the board out.

## License

MIT — see [LICENSE](LICENSE).

No ROMs, BIOS images, game assets or recompiled output are included in this
repository, and none ever will be. The tool ships; the output never does.

The sound board includes third-party code under its own licences (Musashi,
MIT; MAME's SCSP, BSD-3-Clause; SoftFloat) -- see
[THIRD_PARTY.md](THIRD_PARTY.md).
