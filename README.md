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

**This is the board, not a game.** One toolkit per board, one repository per
title: [`lostworld`](https://github.com/sp00nznet/lostworld) is the bring-up of
*The Lost World: Jurassic Park* on top of this, the way `virtuacop` and
`daytonausa` sit on `model2recomp`. Start there if you want to run something.

**Title-agnostic.** Nothing here knows what game it is running. The memory map,
interrupt controller, tilemap and Real3D interfaces all come from the board,
not the title. *The Lost World: Jurassic Park* (1997) is the reference game it
is being built against, so the worked examples are its.

## Status

**v0.1.0 — _"Reset Vector"_ (September 2026). Alpha: the toolchain is real and
verified end to end on a ROM; the board is a skeleton.**

What that means concretely: you can take a Lost World romset, assemble it,
recompile 342,753 PowerPC instructions to C, compile the result cleanly, and
**run it**. The game boots, passes its own memory test, enumerates PCI,
configures the SCSI DMA engine itself, drives the tilemap generator and fills
culling RAM. It reaches its operator service menu and holds there.

It does not reach attract mode yet, and there is no renderer, so there is no
picture to show. No screenshots here until there is one.

The strongest evidence the lift is correct is not that it runs — it is that
the interpreter and the recompiled binary **agree**. Over 30,000 ordered
device accesses the two traces are identical, and both produce byte-identical
buffers: 24,525 words of VRAM and 8,306 words of culling RAM. A lifter bug
would show up as the first differing line.

### What is blocking attract mode

The game installs its per-frame tasks through a table of ten callback slots at
RAM `0x001EED7C..0x001EEDA0`. In a run that reaches attract mode, slot
`0x001EED80` holds the frame task `0x1578`. Here it holds the null stub
`0x00117864` for every field observed — the installer at `0x00117C44` is never
called with it.

The install site is `0x000019A8`, reached from `0x00001934` when
`[0x001A3474] == 0`. Both of those facts are observed to hold: execution
provably reaches `0x000018FC` (it writes `0x001A3474`), and the byte reads back
0. So the stall is in the span between them — three calls: `0x0002E9BC`, the
epilogue of `0x000018BC`, and `0x00001984`. `tools/ppc_interp.py --break-at`
exists to say which of those is the last one reached; that run is the next
step.

Ruled out by measurement, not by argument: the sound board (non-blocking —
forcing data-ready changes nothing), PCI enumeration, the SCSI engine, the
Real3D ready bit, both polarities of the input registers at `0xF0040008` and
`0xF004000C` (byte-identical results), and the two interrupt lines the guest
enables but this runtime never asserts, `0x20000000` and `0x08000000`
(asserting them produced 212,812 handler entries against a normal 482, which
is what an unacknowledged level-triggered line looks like — the guest is not
waiting for them).

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
| CPU context | **Done** | GPR/FPR/CR/XER/LR/CTR/MSR/SPR, with a semantics self-test |
| Memory bus | **Done** | Full 32-bit map: RAM, CROM fixed and banked, VRAM, culling/polygon RAM, devices |
| Function dispatch | **Done** | Hash table from guest address to native function |
| Interrupt controller | **Done** | Pending/enable/ack, and the ack-spin the guest depends on |
| Execution model | **Done** | Field sync at the guest's `0xF0100018` poll; IRQ dispatched there |
| Platform | **Done** | SDL2 window and timing; headless fallback when SDL2 is absent |
| Boot interpreter | **Done** | Runs the reset path, recovers the CROM->RAM segment map, snapshots RAM |
| Computed-jump handling | **Done** | `mtlr`+`blr` told apart from a real return -- the boot handoff depends on it |
| Indirect-target feedback | **Done** | Runtime func-table misses feed back through `--entries` |
| Spin diagnosis | **Done** | The bus names the register a stalled guest is polling |
| Differential conformance | **Done** | Interpreter and lifted code agree over 30,000 device accesses |
| I/O board handshake | **Partial** | Strobe latch is right; the serial ready line is toggled, not driven |
| 53C810 SCSI DMA | **Partial** | SCRIPTS memory move / jump / interrupt, manual-start mode |
| Tilemap renderer | **Partial** | Draws real pixels from the game's own VRAM; scroll and priority not wired |
| PCI configuration | **Done** | The game finds the 53C810 at device 14 and assigns its base address |
| Real3D | **Not implemented** | Viewport and LOD table are set up by the guest; no scene is submitted yet |
| Sound | **Not implemented** | No 68000, no SCSP, no audio |
| Inputs | **Stub** | Reads as "nothing pressed" |
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
lifted. That interpreter is the next milestone, and it doubles as the
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

### The input register is multiplexed, and that is what picks the menu

`0xF0040004` returns different things depending on what was last written to
`0xF0040000`, and two callers want two different things from it. The input path
at RAM `0x00117890` strobes with the clock bit *low* and wants button state;
the serial path at `0x0011A650` bit-bangs a byte through the primitive at
`0x0011A338`, which leaves the clock bit *high*, and wants the I/O board's
reply. Serving either one unconditionally breaks the other: the reply line
returned to the input path reads as a phantom button press, and button state
returned to the serial path hangs a handshake that waits for the line to
change.

The clock bit tells them apart. Worth recording because the failure is silent
in both directions — one puts the game in a menu, the other stops it dead, and
neither logs anything.

Inputs are active low. `nor r0, r0, r11` in the input path means `0xF000` is
the correct idle value, not a stuck bus — a thing this project got wrong once
and had to correct.

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

    if (!model3recomp_init(&cfg)) return 1;
    lw_register_all();                   /* generated */
    model3recomp_run();                  /* enters at 0xFFF00100 */
    model3recomp_shutdown();
    return 0;
}
```

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

## License

MIT — see [LICENSE](LICENSE).

No ROMs, BIOS images, game assets or recompiled output are included in this
repository, and none ever will be. The tool ships; the output never does.
