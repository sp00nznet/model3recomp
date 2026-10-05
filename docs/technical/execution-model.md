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


## Fields are paced by guest work, not by the wall clock

`model3recomp_field_due()` asks how much work the guest has done, not what
time it is. That distinction was learned the hard way.

It used to ask the platform layer for the time. Headless that returns
`m3_work`, so it was the right answer by accident. Against SDL it returns
real microseconds, so fields arrived sixty times a second whether the guest
had got anywhere or not -- and a recompiled 603e uploading three megabytes of
texture does not run at real time. The guest got a fraction of the
instructions per field it needed.

What that looked like was not a timing bug. Built headless, field 1500 was
the attract scene: 18688 lit pixels, 3774 colours, textured. Built against
SDL2, the same field was the Sega boot logo, 190296 pixels, 15 colours, and
it stayed there for 30000 fields. Two builds of the same emulator disagreeing
about what the game was doing.

Finding it meant ruling out everything that looked more likely first: the
cabinet buttons, the framebuffer, the video driver, and checking out both
`bus.c` and `real3d.c` from before the divergence. All of them behave the
same. What was left was the one function in the platform interface that
returns a number the rest of the runtime does arithmetic on.

A host that wants to run at the right speed throttles in
`platform_present()`. It does not get to decide when the guest's video
hardware retires a frame.


## Finding a frozen guest: `M3_TRACE_CALLS=N@F`

A recompiled game has no program counter to inspect. When it stops making
progress the only visible thing is the sequence of addresses going through
`func_table_call()`, so `M3_TRACE_CALLS=N` prints the first N of them and
`N@F` starts counting at field F -- which is what you want, because the
interesting moment is thousands of fields after boot.

Coin, credit, trigger, and then a screen that was byte-identical for 2750
fields looked like a hang in the game. It was not. Fifty fields of trace at
field 3400 were five addresses repeating:

    0001D68C  0007F67C   <-- NOT LIFTED
    00000900  00000500  00001578

`0x900` and `0x500` are the decrementer and external-interrupt vectors, so
interrupts were still being delivered and the runtime was still running. The
game was calling a function that did not exist. `func_table_call()` returns
0 for a miss and the caller spins.

The five addresses a credit reaches -- `0x2B6C0`, `0x2BD78`, `0x2C3E0`,
`0x2FCF8`, `0x7F67C` -- are all inside the lifted RAM range. They are
indirect targets the static walk never saw because nothing in attract mode
reaches them. Append them to the entries file and lift again.

Which is the general shape of this: entry coverage is not a property of the
program, it is a property of the paths you have run. Attract mode converging
on zero misses says nothing about the paths a coin opens.


## Entry discovery: what a credit needs that attract mode never asks for

Feeding runtime misses back with `--entries` converges, but each round only
buys one more state. Five addresses, lift, build, run, five more addresses one
step further along. The addresses themselves said why:

    00121860  0001C0E4      <- the game-state dispatch table
       ...    (21 entries)
    001218A8  0007F67C
    001218AC  0007F8EC
    001218B0  0001BFB0

Three separate holes in `discover()` were hiding the rest.

**Offset jump tables.** `scan_tables` knew one shape -- a table of addresses
read with `lwzx` -- and this compiler emits the other:

    0002BE24  addis r9,0,3 ; addi r11,r9,-0x41B8    ; r11 = 0x0002BE48
    0002BE2C  add   r9,r0,r11 ; lwz r0,0(r9)        ; the offset
    0002BE34  addis r11,0,3 ; addi r9,r11,-0x41B8   ; the base again
    0002BE3C  add   r8,r0,r9 ; mtctr r8 ; bctr      ; base + offset
    0002BE48  00000020 0000003C 000000BC ...        ; eight arms

Every word in that table is small and none of them decodes as code, so a scan
looking for addresses walks straight past it.

**`addi rD, rA, lo` where `rD != rA`.** The address-pair tracking required the
`addis` and the `addi` to name the same register. This compiler routinely
writes the low half into a different one -- `addis r9,0,3 ; addi r11,r9,lo` --
and every table built that way was invisible. Relaxing that one condition took
discovery from 2596 entries to 3229.

**Function prologues.** `stwu r1,-N(r1)` is one instruction, its immediate
must be negative, and every non-leaf C function opens with it. 1429 of them in
the RAM image. This matters because the handlers a credit runs are reached
through pointers that only exist in RAM *after an overlay loads*: they are in
no table the boot snapshot holds and no `bl` names them.

Two things about the prologue scan are load-bearing:

  * The addresses go in `entries` and **not** in `hard`. A prologue is an
    extra way into code, like an address fed back from a runtime miss; making
    it a boundary truncates whatever function branches across it, and the
    symptom is the machine resetting to the WARNING screen on the first coin.
  * It runs *before* the table scans and walks from its own finds, because
    those scans only look at code that `seen` reaches. A jump table inside a
    function nothing calls statically is exactly the case that matters.

Together: 1254 -> 3244 functions, and the game goes from freezing on the
credit to the stage-one intro and an in-game HUD.

The general shape is worth stating once. Entry coverage is not a property of
the program, it is a property of the paths that have been run. Attract mode
converging on zero misses says nothing at all about the paths a coin opens.


## The 53C810 is at 0xC1000000, and one page wider was fatal

Halfway through the first round the screen went black and stayed black for
seven thousand fields. The loop guard said where: one guest function entered
251,700,076 times, and every other count identical to a thousand fields
earlier, so nothing else had run at all.

    00118A80  x251700076      <- new
    0002E618  x11418712       <- same count at field 4600 and at 5600

`0x118A80` reads a byte at RAM `0x6FB` and returns a pointer if it is 1, else
zero; its caller is the Real3D frame sync and spins until it is non-zero.
That byte is set once at boot, after a PCI probe of device 13 answers
`0x16C311DB` -- Sega's vendor ID `0x11DB`, device `0x16C3`. It was 1 at field
4500 and 0 at 5600, and the only instruction in the image that writes it
writes 1.

`M3_WATCH` existed for exactly this and missed it twice. First because it
fired only on an exact address match, and a byte is most often cleared by a
word store two bytes below it -- it now fires on any store that *covers* the
address. Then because it lived in lift.h, which only sees stores from lifted
code: this one came through `bus_write8()`, from a SCRIPTS DMA.

The DMA was `0x68 -> 0x00000000, 262232 bytes`, which copies the low quarter
megabyte of RAM over itself. It came from a SCRIPTS program the game never
wrote. The SCSI decode was

    (a & 0xFE000000u) == 0xC0000000u

which is two 16 MB pages, and the game uploads 40 KB to a device at
`0xC0020000`. All 10272 of those words landed on the 53C810's 64-byte
register file, DSP among them, and every DSP write launched a "program" made
of whatever the upload happened to contain.

The game says where the chip really is. It writes DSP at `0xC100002C`, and in
a whole boot it touches `0x0C`, `0x14`, `0x2C`, `0x38` and `0x39` there and
nothing else in that space. One page, `0xFF000000`.

What is at `0xC0000000` is a device this port does not have: the game writes
`0xDEB00BED` there and reads it back to see whether it exists. With the SCSI
window narrowed the read no longer answers, the probe fails, the game sets
its own "absent" flag and skips the upload -- which is the honest model, and
costs a device that was never emulated anyway.

A SCRIPTS program that contains a move to nowhere is also now abandoned
rather than skipped over. That alone did not fix this -- the destructive move
was in range and perfectly plausible -- but walking on through 4096 more
"instructions" of garbage is how a bogus program finds a plausible move in
the first place.


## A lifted function does not always start at its entry

`emit_function()` writes the body in address order, and the entry is not
always the lowest address in it. A function that ends with a backward branch
into a block it shares with a routine below it gets that block first, so
calling the function falls into the wrong code.

The matrix push at `0x0010ED58` is exactly that shape:

    0010ED58  lbz  r4,0x1256(r2)   ; stack depth
              cmpi r4,20           ; full?
              ...                  ; depth++, pointer += 48
    0010ED78  b    0x0010EBBC      ; tail: copy current -> new slot

`0x0010EBBC` is the tail of the copy routine 0x6C bytes below, so the lifted
function came out as

    void lwram_0010ED58(void)
    {
        M3_FN(0x0010ED58u);
    L_0010EBBC: ;                  <- execution starts here
        ... the copy ...
        return;                    <- and returns
        PPC_R(4) = MEM_R8(...);    <- the push, unreachable
        ...
    }

So the stack depth never moved, nothing was ever pushed or popped, and every
matrix the game built accumulated onto the last one. In a round that put the
2D overlay's screen-space scales -- 496 and 68 -- into the camera matrix,
twice, which is why its rows measured 1.99e4, 4624 and 2.45e5: 68 squared
and 496 squared. The matrix inverse downstream was faithfully inverting an
ill-conditioned matrix, and every object in the scene came out rank 1.

94 functions in this game are shaped that way.

The fix is one line -- emit `goto L_<entry>;` when the entry is not the
lowest address -- and the tell was on the build the whole time:

    warning C4102: 'L_00001FDC': unreferenced label

A lifted function whose own entry label is never referenced is one that
nothing jumps to, including the function itself. Those warnings are worth
reading.
