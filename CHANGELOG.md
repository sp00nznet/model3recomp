# Changelog

All notable changes to this project are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- **Boot interpreter** (`tools/ppc_interp.py`): runs the reset path against the
  same memory map as the runtime, recovers the CROM-to-RAM segment table, and
  snapshots RAM for the lifter. Also the conformance oracle.
- **53C810 SCSI DMA** (`src/scsi.c`): SCRIPTS memory move, jump and interrupt,
  with manual-start mode and range guards.
- **Tilemap renderer** (`src/tilegen.c`): four 8x8 layers, 4bpp, 1-5-5-5
  palette. Name-table base and entry bit layout still unverified.
- **Screenshot capture** (`src/screenshot.c`) and a per-field frame hook, since
  a recompiled game never returns and that is the only way to see it.
- **Headless platform** (`src/platform_null.c`) so the library builds and runs
  without SDL2.
- Spin diagnosis in the bus: when the guest stops making progress it names the
  register being polled, ranked by traffic.
- `M3_TRACE_CALLS` to print dispatched guest addresses.

### Fixed
- **Interrupts are delivered on a timer, not on a status poll.** The frame was
  originally hung off a read of `0xF0100018`, by analogy with Model 2. The Lost
  World's main loop never reads it -- it waits on a RAM flag only the VBlank
  handler sets -- so nothing ever advanced.
- **`blr` is not always a return.** PowerPC uses `mtlr`+`blr` as a computed
  jump, and the boot's handoff into RAM is exactly that. Emitting `return;`
  dropped out of the guest silently. Now told apart by whether LR was restored
  from the stack or built from an immediate.
- **Functions are no longer truncated at the next discovered entry.** Only a
  `bl` target or a prologue is a function boundary; a jump-table arm or an
  address built by `lis`/`addi` is an extra way *into* existing code. The game's
  entry point is nine consecutive `bl`s, and bounding it at a soft entry
  emitted five of them and returned.
- Function discovery walks control flow from seeds instead of scanning the
  whole image for `bl`. A mostly-data ROM yields plenty of data words that
  decode as branches; that inflated the lift from 340 K instructions to 2.2 M.
- Device registers are byte-addressable: `0xF0100008` is a byte register the
  boot writes and reads back, and the broken lane handling hung it there.
- `0xF0040000` must read back what was written, and `0xF0040004` must toggle
  its ready line, or I/O init spins forever.
- Out-of-image branch targets are no longer emitted as C labels.
- Headless frame pacing: a virtual clock that advances per query, rather than
  reporting a field as always due, which starved the guest's main loop.

## [0.1.0] — 2026-09-15 — _"Reset Vector"_

First entry. The toolchain is verified end to end against a real romset; the
board itself is a skeleton.

### Added
- **PowerPC 603e decoder** (`tools/ppc_disasm.py`) covering the integer,
  floating-point, branch, condition-register and system instruction sets,
  including the 603e-specific software TLB reload (`tlbld`, `tlbli`).
- **Static recompiler** (`tools/ppc_lifter.py`): PPC 603e to C, with function
  discovery from call targets, frame prologues and the exception vectors, and
  an optional code-pointer scan. Emits the house layout — `<prefix>_code_NNN.c`,
  `<prefix>_funcs.h`, `<prefix>_register.c`.
- **ROM loader** (`tools/rom_loader.py`) that assembles the four interleaved
  program EPROMs and *proves* the layout against the PowerPC exception vector
  table rather than assuming a chip order.
- **Decode conformance harness** (`tools/check_decode.py`), scoring the decoder
  against capstone over a whole ROM and excluding instructions a 603e cannot
  execute.
- **Runtime**: PPC 603e context, memory bus with the full Step 1.x address map,
  function dispatch table, interrupt controller, frame pacing, SDL2 platform
  layer.
- **Semantics self-test** (`tests/test_ppc.c`) for the rotate-mask wrap case,
  shift counts of 32 or more, carry and overflow rules, CR bit numbering and
  func-table growth.
- `docs/technical/execution-model.md`, documenting the frame boundary, the
  interrupt acknowledge spin, and the RAM-execution problem, with the
  disassembly each conclusion was drawn from.

### Verified against *The Lost World: Jurassic Park* (`lostwsga`)
- ROM loader: 7/7 exception vectors matched, output byte-identical to a
  hand-assembled image.
- Decoder: 100.00% of the 603e-valid instruction set across the 2 MB program
  ROM; the 12 remaining capstone decodes are 64-bit and AltiVec encodings a
  603e cannot execute, found in data.
- Lifter: 2,014 functions, 342,753 instructions, 164 untranslated (0.048%).
- Generated C compiles with zero errors and zero warnings at MSVC `/W3`.

### Known gaps
- The game executes from RAM, not ROM, so the boot interpreter and RAM
  snapshot are required before the main program body can be lifted at all.
- No renderer: the tilemap and Real3D layers store and read back correctly but
  nothing draws.
- No 53C810 SCSI DMA, no sound, inputs are stubbed.
