# Execution Model

**Read this before you write a line of integration code.** Model 3 differs from
Model 2 in exactly one way that matters, and it is the way that decides whether
your port runs or hangs.

Everything below was read out of *The Lost World: Jurassic Park* (`lostwsga`)
rather than assumed. Addresses are that game's; the structure is the board's.

## The ROM

The four program EPROMs interleave as 16-bit words into one 2 MB image:

| EPROM | Byte lane in each 8-byte group |
|---|---|
| `epr-19936.20` | 0–1 |
| `epr-19937.19` | 2–3 |
| `epr-19938.18` | 4–5 |
| `epr-19939.17` | 6–7 |

...with each 16-bit word byte-swapped (MAME's `ROM_LOAD64_WORD_SWAP`). The
image occupies the top of the address space, `0xFFE00000`–`0xFFFFFFFF`, inside
the 8 MB fixed-CROM window at `0xFF800000`.

You can check you got it right without a disassembler. The PowerPC exception
vectors land at `0xFFF00100` + `0x100 × n`, and each one opens by loading its
own vector number:

```
0xFFF00200   li r7, 2 ; b 0xFFF01404
0xFFF00300   li r7, 3 ; b 0xFFF01404
0xFFF00500   li r7, 5 ; b 0xFFF01404
```

If the immediate matches the vector offset, the interleave is correct.

## The ROM's exception vectors are a dead end

Every vector above branches to `0xFFF01404`, and that routine clears `HID0`,
writes `0xAC` to `0xF010003C`, and **spins forever**. It is the fatal-error
handler. The external-interrupt vector at `0xFFF00500` goes there too.

So the vectors in ROM are not the game's interrupt handling. The game clears
`MSR[IP]` to move the vector base to `0x00000000` and installs real handlers in
RAM. A lifter that discovers functions by following `bl` targets will never find
them, because nothing ever branches to a vector — the processor jumps there.

## The game runs from RAM, not from ROM

This is the fact that shapes the whole tool.

The interrupt library at `0xFFF37BD0` installs its handler like this:

```
lis  r9, 0x11
addi r9, r9, 0x7864          ; r9 = 0x00117864
lis  r10, 0x1f
stw  r9, -0x1284(r10)        ; -> [0x001EED7C]
```

and later calls it:

```
lis  r9, 0x1f
lwz  r0, -0x1284(r9)         ; [0x001EED7C]
mtlr r0
blrl                         ; call 0x00117864
```

`0x00117864` is **RAM**. The bulk of the game is copied out of the 32 MB of
banked CROM into RAM at boot — that is what the 53C810 SCSI DMA engine is for
on Step 1.x — and executes there.

The copy is *not* verbatim: CROM offset `0x117864` is a lookup table, not code.
So the RAM layout cannot be recovered by inspecting the ROM alone.

### The boot loader, in full

`tools/ppc_interp.py` runs the reset path and the whole sequence is now known:

1. `0xFFF00100` calls `0xFFF00508`, a table-driven hardware initialiser. It
   reads a count at `0xFFF00408` and a base at `0xFFF0040A`, then walks a
   byte-code stream of `(opcode, offset, value)` items, writing bytes,
   halfwords and words relative to that base. It programs the MPC105 host
   bridge at `0xF8FFF000`.
2. `0xFFF0032C` runs the **segment copy loop** at `0xFFF00354`, which walks a
   table of `(src, dst, count)` triples:

   ```
   0xFFF00354  lwzu r4, 4(r3)     ; src
               cmpwi r4, 0        ; 0 terminates the table
               beq   done
               lwzu r5, 4(r3)     ; dst
               lwzu r6, 4(r3)     ; count
               ...
               mtctr r6
   0xFFF00374  lwzu r0, 4(r4)
               stwu r0, 4(r5)
               bdnz  0xFFF00374
               b     0xFFF00354   ; next entry
   ```

3. For *The Lost World* that table has two entries:

   | RAM | from CROM | size |
   |---|---|---|
   | `0x000000..0x0FFFFC` | `0x000000` | 1 MB, verbatim |
   | `0x100000..0x13B2D4` | `0x120004` | 237 KB |

   The first carries the exception vectors and their handlers, which is why
   `RAM 0x500` is byte-for-byte the external-interrupt handler shown above.
   The second carries the interrupt library and the game.

4. Finally, the handoff:

   ```
   0xFFF0013C  mfspr r0, HID0
               andi. r0, r0, 0x3FFF
               isync ; sync ; mtspr HID0, r0
   0xFFF00150  lis   r0, 0
   0xFFF00154  mtlr  r0
   0xFFF00158  blr                ; jump to RAM 0x00000000
   ```

### `blr` is not always a return

That last `blr` is the whole game. PowerPC uses `mtlr` + `blr` as a **computed
jump**, and a lifter that translates every unconditional `blr` to `return;`
turns the boot's handoff into a no-op: the port falls out of the guest and
does nothing, silently, with no crash and no missing function to point at.

The two cases are told apart by where LR's value came from:

| Sequence | Meaning | Emit |
|---|---|---|
| `lwz r0, N(r1)` ... `mtlr r0` ... `blr` | LR restored from the stack | `return;` |
| `lis r0, X` ... `mtlr r0` ... `blr` | LR built from an immediate | `CALL(PPC_LR); return;` |

`tools/ppc_lifter.py` tracks which registers hold immediate-derived values and
picks accordingly. Across the whole 2 MB program ROM this fires exactly
**once** -- on the boot handoff -- so the rule is narrow enough not to corrupt
ordinary function epilogues.

### What that means for the lifter

There is no static answer for the RAM layout. The lifter therefore boots the
machine before it lifts anything:

1. A small PPC interpreter runs from the reset vector, against the *same* bus,
   SCSI and system-control code the runtime uses.
2. It stops when the guest first branches into RAM.
3. RAM is snapshotted.
4. The snapshot is what gets lifted, at its RAM addresses.

This is less code than a static analysis that would have to prove what the DMA
engine does, and it is exact rather than approximate. It also gives the
conformance harness its oracle for free: the interpreter and the lifted code
are two implementations of the same instruction set, and they must agree.

The interpreter only has to cover the instructions the boot path actually uses
— it is not a general Model 3 emulator and must not grow into one.

## The frame boundary

Model 2 busy-waits on a video status bit, and answering that read *is* the
frame. **Model 3 does not, and the first design here got this wrong.**

The obvious hook looks right: `0xF0100018` is read seventeen times across the
program ROM and never written, so it is read-only interrupt status. Hanging the
field off it is the exact analogue of the Model 2 design.

It does not work, and the interpreter is what proved it. The Lost World's main
loop never reads `0xF0100018` at all:

```
0x00118284  lwz  r0, -0x126C(r29)   ; service-routine pointer in RAM
            mtlr r0
            blrl                    ; call it
0x00118290  lbz  r0, 1(r30)         ; flag byte at RAM 0x000006ED
            cmpwi cr1, r0, 0
0x001182A0  bc   0x00118284         ; spin until the flag changes
```

Only the VBlank handler sets that flag. A frame hook that waits to be asked
will never fire, and the game spins forever having rendered nothing.

**So interrupts are delivered on a timer, which is what the hardware does
anyway.** `irq_tick()` advances the field when one is due and dispatches the
handler. It is called from `func_table_call()`, because a main loop that waits
on an interrupt-set flag still has to dispatch its service routine through a
pointer every iteration -- that is the one hook the runtime reliably gets.
`irq_status_read()` remains wired to `0xF0100018` so that a game which *does*
poll the controller still gets a field out of it.

### Two ways to get this wrong

Both were hit here, and neither announces itself:

1. **Pacing the field off nothing.** The headless platform first reported a
   field as always due. Since `irq_tick()` runs from `func_table_call()`, the
   guest then took an interrupt on *every indirect call*, its main loop never
   ran between them, and the game ticked 17,000 fields without writing a
   single byte of tilemap VRAM. A virtual clock that advances per query fixed
   it.

2. **Dispatching with interrupts masked.** The handler must only be entered
   when the guest has `MSR[EE]` set and the pending bit is enabled, or it
   re-enters itself.

### The acknowledge spin is the trap

This is the single most important routing in the whole library. The game's
handler acknowledges an interrupt by writing the bit to the **tilegen** at
`0xF1180010`, then re-reads `0xF0100018` and spins until the bit clears:

```
0xFFF37EB8   stw   r8, 0(r11)      ; r11 = 0xF1180010, r8 = 0x01000000
0xFFF37EBC   lwz   r0, 0x18(r10)   ; r10 = 0xF0100000
0xFFF37EC0   andis. r9, r0, 0x100
0xFFF37EC4   bne   0xFFF37EB8      ; spin
```

If a write to `0xF1180010` does not clear the matching bit in `0xF0100018`
before the next read, the game hangs there forever. Note the asymmetry --
pending is read from the system controller, acknowledged at the tilegen. That
is genuinely how the board is wired.

## The I/O board

Two more registers hang the boot if they return a constant, and both were
found the same way: the bus counts what it is asked for and names the address
a stalled guest is polling.

**`0xF0040000` is a latch.** The boot strobes a bit, reads the register back,
and waits for it to mirror -- first for the bit to set, then for it to clear:

```
0x001178A8  stw r10, 0(r11)    ; r11 = 0xF0040000, r10 = 0x01000000
            lwz r0,  0(r11)
            andis. r9, r0, 0x100
            beq 0x001178A8              ; spin until it SETS
            ...
0x001178CC  stw r10, 0(r11)    ; r10 = 0
            lwz r0,  0(r11)
            andis. r9, r0, 0x100
            bne 0x001178CC              ; spin until it CLEARS
```

Return 0 and the first loop never exits; return 0xFFFFFFFF and the second
never does. It has to read back what was written.

**`0xF0040004` carries a serial ready line.** The boot bit-bangs a byte to
`0xF0040000` with delay loops, then waits on bit `0x20000000` here, again for
both edges:

```
0x0011A650  li r4, 0x51 ; bl <bit-bang>
            lwz r0, 0(r30)        ; r30 = 0xF0040004
            andis. r9, r0, 0x2000
            bne 0x0011A650        ; wait for CLEAR
            ... same again waiting for SET
```

The runtime toggles that bit rather than modelling the protocol, which is
enough to get through I/O init. Buttons and coins will need the real 315-5649
serial protocol.

## Bulk data: the 53C810

Step 1.x moves bulk data with the SCSI controller's SCRIPTS processor, not the
host bridge. No SCSI bus and no target device are involved -- the game uses it
as a memory-to-memory DMA engine, and without it the boot polls ISTAT at
`0xC1000014` about twelve million times and stops.

The chip is on the PCI side and is **little-endian**, which the game's own
traffic shows plainly: it writes `DSP = 0x4C2C1A00`, and that is the address
`0x001A2C4C`. Every 32-bit quantity, including the SCRIPTS instructions
themselves, is byte-reversed relative to the PowerPC.

Only three SCRIPTS operations are modelled, because only three are used:
memory move, jump, and interrupt. The game's first real program, sitting in
RAM at `0x001A2C4C`, is:

```
C000000C   memory move, 12 bytes
001BAA50   from work RAM
9C000000   to the Real3D texture port
98080000   INT
```

Two things about that are worth keeping in mind. It writes to the **Real3D**,
not to RAM, so a DMA engine whose idea of a valid destination covers only
memory will reject it. And it ends in `INT`, which is what sets `ISTAT[DIP]` --
the bit the guest is polling. Reject the move and return, and the interrupt
never fires and the guest reads `0xC1000014` about twenty million times. So a
move the engine declines is skipped, not fatal: the program runs on to its
`INT`.

The boot also sweeps the whole register file with a test pattern, DSP
included. Without the chip's manual-start mode (`DMODE[MAN]`), every one of
those writes launches a "program" that is really whatever PowerPC code happens
to live at that address -- the giveaway being a move from `1A00603D`, which
byte-reversed is `3D60001A`, `lis r11, 0x1A`.

See `src/scsi.c`.

## Interrupt bits

Status and enable both live in the **upper 16 bits** of the word. The game
tests them with `andis.` and sets them with `oris`, never with `andi.`/`ori`,
which is the giveaway:

| Bit | Meaning |
|---|---|
| `0x02000000` | Start of VBlank |
| `0x01000000` | End of VBlank |
| `0x04000000` | SCSI DMA complete |
| `0x08000000` | Sound |
| `0x10000000` | Network board |

`0xF0100014` is the enable mask, read-modify-written with `oris`/`rlwinm`.

## Dispatch

There is nowhere to preempt — lifted code owns the C stack — so interrupts are
delivered at the field boundary, which is where VBlank arrives on hardware
anyway. For the interrupt that matters this is not an approximation.

`irq_dispatch()` walks the guest's own path rather than hardcoding anything:

1. Read the installed handler pointer out of guest RAM.
2. `func_table_call()` it.

Because the pointer is read from RAM at dispatch time, a game that swaps
handlers mid-run keeps working, and a handler the lifter missed shows up as a
named func-table miss instead of a silent wrong branch.

## Boot

The reset vector at `0xFFF00100` is three calls into the spare space inside the
exception-vector slots — the vectors only use 8 bytes of their 256, so the boot
code lives in the gaps:

```
0xFFF00100   bl 0xFFF00508
0xFFF00104   bl 0xFFF00208
0xFFF00108   bl 0xFFF00908
```

`0xFFF00508` is a table-driven hardware initialiser. It reads a count at
`0xFFF00408` and a base at `0xFFF0040A`, then walks a byte-code stream of
`(opcode, offset, value)` triples, writing bytes, halfwords and words to
registers relative to that base. It is not code you need to understand — it is
code you need to *run*, which the boot interpreter does.


## The clock, and why lifted code has to carry it

A recompiled program has no instruction counter. Nothing counts cycles, and
there is no interpreter loop to hang a timer on -- so the runtime's only idea
of how much the guest has done is what the guest tells it.

The runtime gets control in exactly three places: a dispatched call
(`func_table_call`), a device access (the bus), and whatever lifted code
explicitly hands it. The first two are not enough, and the reason is worth
stating plainly because it cost a long time to find:

* A main loop that waits on an interrupt-set flag does dispatch through a
  pointer each iteration, so `func_table_call` catches it.
* A loop that polls a hardware register reaches the bus.
* **A loop that only touches work RAM reaches neither** -- especially once
  work RAM is resolved inline for speed.

In that third case the field never arrives, and the guest waits forever for an
interrupt that only a field could deliver. It presents as a hang with no
evidence at all: full CPU, no func-table miss, no device traffic, and no
watchdog firing, because every instrument lives in the runtime the guest has
stopped visiting. The silence *is* the diagnosis.

So lifted basic blocks retire their instructions through `WORK(n)`:

```c
#define WORK(n)                                                    do {                                                               m3_work += (n);                                                if (m3_work >= m3_next_tick) m3_tick();                    } while (0)
```

an inline add, a compare that is almost always false, and a call about once
per 64 K instructions. `m3_work` is then a real instruction count, and the
headless platform divides it by the 66 MHz bus clock, so a field lands every
1,147,413 instructions -- 66 MHz / 57.52 Hz, which is what a field is.

Two ways to get the counting itself wrong, both hit here:

| Counted | Symptom |
|---|---|
| per function entry | a loop inside one function advances the clock once; fields stop dead |
| off device traffic | a compute-heavy frame barely advances; 900 fields took over half an hour |

## What the guest actually builds

With all of the above right, *The Lost World* runs to 300 fields and fills:

| Buffer | Non-zero words |
|---|---|
| tilegen VRAM | 24,525 |
| Real3D culling RAM (high) | 8,306 |
| Real3D culling RAM (low) | 0 |
| polygon RAM | 0 |

and the interpreter agrees exactly. The culling RAM holds the scene: recurring
`0000803F` words are `3F800000` byte-reversed -- 1.0f -- so those are
transformation matrices, stored little-endian like everything else on the PCI
side of this board. Polygon RAM staying empty is consistent with the geometry
living in VROM and being referenced through the culling tree rather than built
per frame.

The game therefore describes a complete 3D scene every field. What is missing
is something to draw it.


## Where the bring-up stands, and what is in the way

The recompiled game boots, copies itself into RAM, completes I/O init,
enumerates PCI and configures the 53C810, runs SCSI DMA, takes VBlank
interrupts, and runs its frame loop indefinitely. The interpreter and the
lifted code agree over 30,000 device accesses and produce identical buffers.

It does not draw. The useful detail is *where* it stops, and the hot-dispatch
histogram is what found it -- a recompiled game has no stack and no program
counter, so the routines its idle loop keeps calling are the closest thing to
knowing where it is:

```
[model3recomp] most-dispatched guest addresses:
    00117864  x868120     <- the null service stub, spun on by the main loop
    00000500  x462        <- the external interrupt handler
```

The frame routines themselves do run, once per field each, including the
Real3D driver at 0x0010BD58, 0x0010C0BC and 0x00102E60. So the graphics driver
is called every frame and simply declines to emit anything.

What it tests is an input bit:

```
0x0010C0D8  lhz   r29, 0x0E9A(r9)    ; r9 = 0, the input state block
            lbz   r0, 0x20(r9)       ; 0x12F4, must be zero -- it is
            bne   0x0010C228
0x0010C0F0  andi. r29, 0x0800        ; <- taken as CLEAR, every frame
            beq   0x0010C1FC         ; so the geometry path is skipped
```

RAM 0x0E9A reads as 0x0000, and the surrounding input block is full of 0xF000
patterns. That block is filled from the I/O board, whose serial protocol this
runtime does not implement: the ready line at 0xF0040004 is toggled so the
boot's handshake loops terminate, which is enough to get past initialisation
and is not enough to deliver real button state.

So the next piece is the 315-5649 I/O board's serial protocol, modelled
properly rather than stubbed. The board is clocked over 0xF0040000 -- the
routine at RAM 0x0011A338 writes a data value, delays, sets bit 0x80 to clock
it, and delays again -- and answers on bit 0x20000000 of 0xF0040004.

It is worth saying plainly that this is the fifth thing to look like the last
obstacle. Each of the previous four was real and had to be fixed, and none of
them was final. The difference here is that the evidence is specific: a named
branch, on a named bit, of a word whose contents are known to be wrong.
