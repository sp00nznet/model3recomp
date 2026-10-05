# Changelog

All notable changes to this project are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/);
this project adheres to [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Removed
- **All code derived from Supermodel** (GPL), which earlier commits carried
  without saying so -- reported in issue #1. Each part was rewritten from
  MAME's Model 3 driver (BSD-3-Clause, now credited in `THIRD_PARTY.md`) or
  from measurements of the game:
  - texture upload, tile decoding and the mip layout: MAME's upload code and
    decode tables; the chain now stops at 8x8, as MAME's does;
  - texel formats: MAME's eight. Contour on an 8-bit luminance format keys
    on full white, measured from the stage-one foliage (38758 of 65536
    texels are 0xFF);
  - the node walk: MAME's link types, sibling links and LOD tables;
  - the frustum: words 8..0xB, measured against the clip planes;
  - fog: a simpler model on MAME's word documentation; the background
    "ambient fog" darkening is gone, and scroll fog is a plain per-viewport
    haze -- the intro's storm is the only user;
  - specular: a Blinn highlight on MAME's bits, in place of Supermodel's
    model, so it is subtler than before;
  - tilemaps: MAME's layer pairs, stencil and scroll; the fade registers
    read from what the game writes; per-line scroll dropped (never used);
  - the 93C46 EEPROM: rewritten from the part's datasheet protocol;
  - the light gun: calibration measured from the game's crosshair
    (X 147..652, Y 79..464), and a reload of our own.
  Save states from before this change will not load.

### Added
- **Specular highlights** (word 0 bit 7 and coefficient, word 6
  shininess). `M3_NO_SPECULAR` turns them off.
- **Mipmaps.** Texture uploads store their mip chain (type 0x00 the
  texture and chain, 0x01 the texture, 0x02 the chain), each level in its
  quarter of the page. The rasteriser picks a level per pixel from the
  screen-space gradient of the perspective-correct UVs and blends two
  (trilinear), so distant ground and roofs stop shimmering. About 0.7 ms a
  field; `M3_NO_MIPS` samples the base level only.
- **Fog**: per-polygon distance fog (the viewport's colour, density, offset
  and ambient in words 0x22-0x25, each polygon's share in header word 6)
  and scroll fog (word 0x20). The Lost World uses it for dusty haze, the
  T-Rex's night and the intro's storm; `M3_NO_FOG` turns it off,
  `M3_FOG_TRACE` prints each viewport's settings.
- **Sound.** The sound board: a 68000 (Musashi, interpreted), two SCSPs
  (a C port of MAME's, timers counting samples so it is deterministic) and
  the i8251 UART between the PowerPC and SCSP 1's MIDI port. It runs a field
  at a time on its own thread, a field behind the guest, trading UART bytes
  only at the boundary, so two runs -- and two netplay machines -- produce
  the same audio and the same game. The Lost World's output matches MAME's
  section for section. SDL audio is fed a 100 ms queue, held there by
  nudging the frame limiter, and refills rather than crackles if the host
  stalls. Sound menu: mute (F9) and volume. `M3_WAV`, `M3_NO_SOUND`,
  `M3_SOUND_THREAD=0`, `M3_AUDIO_TRACE`.
- `THIRD_PARTY.md`: the licences of the code the sound board brings in.
- `M3_PRESS=field:mask,...` presses buttons for a moment at given fields --
  how a script walks a game's test menu.
- **The 93C46 serial EEPROM**: bit-banged through
  the I/O board's register 0x00 and read on bit 5 of bank 1, kept in the
  battery RAM file and shared at netplay connect. It replaces a toggle that
  only satisfied the boot's handshake; games keep their settings in it.
- **Game options** (`m3_option_add()`, under Debug; changing one restarts the game) and **one-shot cheat
  actions** (`m3_cheat_add_action()`), both carried in the input record so a
  netplay host's choices hold on both machines.
- Netplay logs its RAM hash every ten seconds, for comparing two machines'
  logs.
- **A menu bar** on Windows: File (save/load state, slots, reset, quit),
  Video (window size, fullscreen, sharp or bilinear, scanlines, 4:3),
  Sound (a placeholder until there is sound), Controls (mouse, gamepad,
  cursor, Sinden border), Debug (the game's cheats) and Multiplayer (host,
  join, disconnect, input delay). Settings persist in an ini file.
- **Cursor options.** Hidden by default, as on the cabinet; a crosshair or
  the system pointer if wanted. Gamepads and netplay partners always get a
  crosshair, having nothing else to aim by.
- **Gamepads**, as player 1 or 2: the left stick aims, A or the right
  trigger fires, B or the left trigger reloads, Start, and Back for a coin.
- **Sinden light gun support**: a white border for the gun to find the
  screen by; the gun itself is an absolute mouse.
- **Save states** at a game-supplied safe point (`m3_config_t.safepoint_pc`,
  `savestate.h`). Exact: a state loaded in a new process reaches
  byte-identical frames. F5, F6 and F7, or `M3_STATE_SAVE_AT` /
  `M3_STATE_LOAD` from a script.
- **Netplay**: two players in lockstep over TCP (`netplay.h`), on a LAN,
  over Tailscale or through a forwarded port. Battery RAM is shared at
  connect, cheats come from the host, and a RAM hash every two seconds
  catches a desync.
- **Battery RAM persistence** (`m3_config_t.nvram_path`).
- **Cheats** a title registers with `m3_cheat_add()`; they travel in the
  input record so both netplay machines apply them.
- `M3_FIRE_EVERY`, `M3_CHEATS` and `M3_PICK=x,y` (which polygon painted a
  pixel) for harnesses and debugging.

### Changed
- **Inputs are latched once a field** into an `m3_input_t` (`input.h`),
  and the bus reads only that. Every read in a field agrees, which lockstep
  netplay requires. `platform_buttons()` and `platform_pointer()` are gone;
  a platform implements `platform_sample()` instead.
- Quitting goes through `model3recomp_quit()`, which saves battery RAM and
  leaves a netplay session before it exits; Escape used to stop the field
  clock and leave the window hanging.
- The SDL build holds the display to 57.524 Hz; `M3_NOTHROTTLE` or holding
  Tab runs flat out.

- **The rasteriser uses every core.** The screen is cut into bands, one
  per thread, each drawing every triangle clipped to its rows in the same
  order, so the picture is byte-identical to one thread. A busy round went
  from 30 ms of 3D a field -- past the 17.4 ms a field has -- to 7.
  `M3_RENDER_THREADS` sets the count; `M3_PROFILE` prints where a field's
  time goes. Bilinear sampling moved to fixed point, with power-of-two
  wrapping and an opaque fast path.

### Fixed
- **The PowerPC's sound interrupt was the wrong bit.** `M3_IRQ_SOUND` was
  0x08 (a video line); MAME and the board say 0x40.
- **Scripted runs read whoever was at the machine.** With `M3_COIN_AT`,
  `M3_FIRE_EVERY` and the like set, the window's mouse still aimed the gun,
  so two runs of one script parted company. A scripted run now ignores the
  host's own controls.
- **Viewports were drawn in list order, not priority order.** The Lost
  World's HUD is a viewport of priority 3 that comes first in the list, so
  the scene was painted over it: the ammunition counter, the RELOAD prompt,
  pickups and cinematic bars all came and went. Viewports are now drawn
  lowest priority first (word 0 bits 3..4), a disabled one (bit 5) is
  skipped, and each projects through its own frustum (words 8..0xB) into its
  own rectangle (0x14, 0x1A).
- **Start 2 never reached the game.** Bit 5 of the input register is start 2
  in bank 0 and the EEPROM data line in bank 1; the handshake toggle was
  laid over both, so start 2 flickered on every read and a second player
  could never join.
- **Mip-only texture loads overwrote the textures they belong to.** The
  upload's type byte was ignored. FIFO entries were screened by size, but a
  VROM load has the rest of the ROM after it, so mip chains landed on the
  base level -- the noise and stripes over a stage's scenery.
- **VROM texture addresses count 32-bit words**, and the 32 MB image is
  mirrored. Read as 16-bit units, every stage texture was noise.
- **Tilemap depth and priority** were the wrong nibbles of register 0x20;
  scroll, per-line scroll, the stencil and the colour offsets were missing.
- **Translucent polygons** were drawn opaque.
- **Culling-node siblings** took the node's own transform; texture offsets,
  LOD tables, node discard and colour tables were ignored.
- Eleven texel formats were missing; textures were not tinted by the polygon
  colour; no filtering or mirroring.

- **Tilemap layers can be eight bits a pixel, and half of them are.** The
  renderer decoded everything as four, which turns a photograph into
  stripes. Register 0x20 carries a nibble of depth flags -- set means
  four-bit -- and another nibble of priority flags saying whether each layer
  is drawn in front of the 3D or behind it. With both read, attract mode has
  its skies and the high score table has the Jurassic Park logo behind it.
- **Layer enable is bit 31 of each layer's scroll register**, not a field in
  register 0x20. The old reading was fitted to four observed values and was
  a coincidence.
- **Fields are paced by guest work, not the wall clock.** `field_due()`
  asked the platform layer for the time; headless that returns `m3_work` and
  was right by accident, but against SDL it returns real microseconds, so
  fields arrived at 60 Hz whether the guest had progressed or not. The same
  build headless walked into attract mode while the windowed one sat on the
  Sega logo for 30000 fields.
- **The attract-mode cross-fade.** Viewport word 0x07 is a global brightness
  multiplier -- it holds at exactly 1.0 for a thousand fields, ramps to 0.0
  in even steps, holds through the gap between demo shots, and ramps back.
  Ignoring it made the renderer look frozen for twenty-six fields at a time:
  the game was fading a shot to black and the picture was drawn at full
  brightness all the way down, byte-identical, while the scene underneath it
  changed every field.
- **Backface culling.** Safe to do because the winding turns out to be
  consistent: in a dumped model all 588 polygons agree in sign between their
  header normal and the normal computed from their own vertices, every one.
  So there is no need to guess at a double-sided flag. It removes five
  triangles in six while the pixel count barely moves, which is what culling
  only hidden geometry looks like -- and the surfaces that used to show
  through a dinosaur are gone.
- **Gouraud shading.** The low byte of each vertex coordinate word is that
  vertex's own normal. They are not unit length, which is why they did not
  look like normals at first; what identifies them is that they sit a median
  25 degrees from the face normal, against the 90 that random directions
  would give.
- **One-sided diffuse.** Taking the absolute value of the lighting term lit
  both sides of every surface equally. With the real light and backfaces
  culled the term runs from -0.72 to +0.93 across a frame, so two triangles
  in five are now in shadow.
- **The rasteriser works in double.** A projected triangle here reaches
  coordinates in the hundreds of thousands, and in float the barycentric
  denominator lost enough precision that pixels inside a triangle tested as
  outside. It did not look like a precision bug -- it looked like a
  deliberate stipple, a neat cross-hatch over a large surface, which is a
  plausible enough way to fake translucency that it was nearly left alone.
- **A depth buffer per viewport.** They are separate layers, not one scene.
  Sharing one buffer let whichever viewport was walked first set the depth of
  the whole screen, and the first one holds a flat quad that covers it.
- **The light direction comes from the viewport** rather than from an
  invention: words 0x04..0x06 are a unit vector.

### Added
- **The light gun board.** Registers 0x24/0x28 take a command and data,
  0x2C/0x30 return the answers and 0x34 reports that a board is fitted.
  Command 0x00 latches which gun register to read and 0x87 fetches it;
  registers 0 to 7 are the two guns' X and Y, and 8 is the offscreen flags.
  Without it the attract panel asking the player to shoot never goes away.
- **Gun triggers** at bit 0 of registers 0x08 and 0x0C, active low, one per
  player, on space or left control. Shooting is how this game starts a
  credit; it ignores the start button entirely.
- `M3_COIN_AT` / `M3_START_AT` press a coin and start at a given field, in
  both platform layers, so a run can be scripted. A coin slot is an edge and
  not a level, so a button held from the first field to the last may be
  counted once or not at all.
- `M3_ADC` chooses what the ADC register reads back, and `M3_IO_TRACE` logs
  reads of the I/O block with the bank latch. The game reads register 0x3C
  more than any other in that block and 0x04 next; what it wants from the
  ADC is not yet known.
- **Cabinet buttons.** `platform_buttons()` reports coins, starts, test and
  service; the SDL layer reads them off the keyboard (5/6 coin, 1/2 start,
  F2 test, F3 service) and the headless layer takes `M3_BUTTONS`. They are
  presented at I/O register 0x04 bank 0, active low in the top byte.
- **Texture sampling**, behind `M3_TEXTURES=1` and off by default. The
  pipeline is complete -- descriptor from the polygon header, perspective-
  correct UVs carried through the near clip, 1-5-5-5 texels with
  transparency, wrapping, format gating, shading modulation -- and the VROM
  address mapping is not solved, so what it draws is real texels from the
  wrong places. It looks like texturing and is not, which is why the switch
  defaults to off. `docs/technical/real3d.md` lists every layout tried and
  what it scored.
- `M3_R3D_TRACE`: logs writes to the Real3D command trigger. It answered one
  question quickly -- the trigger is written with 0xDEADDEAD, so it is a
  doorbell and not the texture upload path.
- Texture groundwork in `docs/technical/real3d.md`: where the texture data
  is in VROM and how it is packed, what the polygon header holds, and the
  two specific things that block using any of it.
- `M3_SHOT_EVERY=N[,DIR]`: write a screenshot every N fields. An attract
  cycle is some 4200 fields and its 3D runs in bursts of a few dozen, so
  sampling it one run per field is not a way to look at it.
- **Honour the tilegen's per-layer enable.** Layer 2 spends attract mode
  holding a solid full-screen rectangle -- one flat-colour tile repeated 2492
  times -- and the game simply stops displaying the layer rather than clearing
  it. Drawing every layer that has something in it painted that rectangle over
  the whole picture, and earlier, while the rectangle was still being
  uploaded, left a small grey artifact in the corner that looked like a stray
  blit.

  The enable bits are at bit 9 of register 0x20 read PCI-side. That position
  is derived rather than documented, and the derivation is weak: see the
  comment in `src/tilegen.c`, which says so.

### Added
- `M3_REAL3D_STATS`: prints triangles submitted and pixels drawn, on change.
  This is what found the first frame of attract mode that actually has a 3D
  scene in it -- field 2080, 4525 triangles submitted and zero drawn.

### Fixed
- **The 3D layer draws.** The vertex scale is 1/256 and there is no camera
  matrix. Both had been wrong, and both wrong answers came with arguments --
  see `docs/technical/real3d.md`, which keeps them written down, because
  they have the same shape and it is the shape to watch for: a 3D pipeline
  never reports a wrong constant, it draws. Field 2080 renders rocks and a
  cliff face under the attract text.
- `tests/test_real3d.py` and `tools/model_view.py`. The test asserts the
  invariants that settled the decode -- models chain back to back through
  VROM, and the same image read the other way has none. The tool renders one
  model on its own, which is the quickest way to tell a good VROM image from
  a bad one.

### Fixed
- **Clip triangles against the near plane instead of dropping them.** A
  triangle that straddles the eye plane cannot be projected, and discarding
  it throws away exactly the large near-camera polygons that would fill the
  screen -- so a scene comes out empty while every individual step looks
  correct.
- **The vertex scale is 1/65536**, not the 1/1024 first committed. The
  earlier figure came from assuming a flat 4:3 model was a backdrop filling
  the screen, and reading two matrices as agreeing on the result; they were
  not agreeing on anything. What is actually established is narrower and is
  written down as such in `src/real3d.c`: models are normalised to fill the
  24-bit coordinate field, which fixes the field's meaning but not the
  divisor.
- **A Real3D scene renderer** (`src/real3d.c`). Walks the viewport list, the
  culling-node tree and the models the game builds each field, and rasterises
  them with a depth buffer under the tilemaps. See
  `docs/technical/real3d.md`, which pairs every structure with the check that
  settled it -- and names the three that are still guesses.

### Fixed
- **VROM is a 16-lane interleave, not two groups of eight.** Both spellings
  produce a 32 MB image, and both look like data. Built as two groups, the
  chips supplying the high half of every word sit 16 MB from the ones
  supplying the low half, so every word the Real3D reads is half of one model
  and half of another -- and nothing complains, because the culling tree
  still walks and the polygon headers still have seven words.

  The check is that a Model 3 polygon header carries its own normal as three
  24-bit fixed-point fields over 2^22. A correct image has 31265 models whose
  every normal comes out unit length. The 8-lane image has none at all.
- `M3_TILEGEN_DUMP`: prints the tilegen registers and per-layer occupancy
  whenever register 0x20 changes. Per field it is noise; on change it is four
  lines for a whole attract cycle.
- **A name-table entry is little-endian too, and it has a palette field.**
  Reading the two entries in a word in the right order but each entry's bytes
  in the wrong one still drew legible text, because it only scrambled bits
  that the previous tile-number formula happened to put back. What it did
  lose was the colour: bits 14..4 of the entry are the top eleven bits of the
  colour index, and dropping them pinned everything to palette 0.

  The boot report is the wrong thing to check this against -- all of its
  entries carry palette 0x20, so losing the field entirely still renders the
  report in one flat colour that looks deliberate.
- **The palette's 1-5-5-5 fields are blue, green, red from the top**, not red,
  green, blue. Reversed, the only thing that changes is red against blue, so
  greys and the boot report's white are identical either way and the picture
  always looks plausible. Attract mode's text came out `0x00A5FF` where MAME
  has `0xFFA500`, which is the same three bytes backwards.
- **A tilemap tile number is ten bits, not nine, and they are not
  contiguous.** The low nine are the entry's top nine; the tenth is the
  entry's *bit 0*. The boot report hid this completely -- its entries are a
  character's ASCII code shifted up by seven with bit 0 clear, so nine bits
  are enough and the tenth never moves. Attract mode draws its credit line
  from a second, double-height font higher up in the pattern area and sets
  bit 0 on every entry: `0x3081` is tile `0x261`, and read as nine bits it
  renders the letter "a" where the top half of a "C" belongs.

  Which is what it did: the credit line came out as `abcdef` / `ghijkl` in
  the right place, in the right colours' worth of pixels, looking enough like
  a font problem to be mistaken for one.
- **The time base was not a clock.** `mftb` advanced one tick per *read*, so
  its value depended on how often a guest looked at it rather than on how much
  time had passed. A guest that only wants monotonicity does not notice; one
  that *measures* with it gets nonsense. The Lost World times an interval and
  divides to get its decrementer period, and computed -26 where the hardware
  gives 26,926 -- so its decrementer never fired again, its main loop waited
  forever on a counter only that interrupt writes, and it ran its own state
  handler once instead of once a field. It now runs off `m3_work`, and writes
  to TBL/TBU re-base it, because timing an interval means zeroing it, waiting,
  and reading back.
- **The Real3D frame flag flipped per read instead of per frame.** The other
  end of the same measurement: the guest times the interval between two flips
  of bit 1 at `0x84000000`, and flipping on demand makes that interval
  nothing. The shortcut was deliberate and marked "upgrade path: flip it when
  the Real3D actually retires a frame"; this is that upgrade.
- **A frame was presented before the guest drew it.** `irq_tick()` rendered and
  presented and then ran the guest's VBlank handler, so what reached the
  screen was the state before that field's drawing -- and a guest that clears
  its tilemap and then prints into it was caught in the gap. Hardware scans
  the tilemap out continuously and the guest draws during blanking precisely
  so the picture is whole when it is not; one render per field belongs after
  the drawing, and now is.
- **The tilemap renderer drew one tile over the whole screen.** It read the
  name table from 0x0F6000, which is a scroll table, not a map -- a guest
  fills it with a single entry and the result is a screen of that one tile
  forever. The four name tables are at 0x0F8000 and 0x2000 apart. Two more
  orderings were wrong with it, and neither announces itself: the two 16-bit
  entries in each 32-bit word are stored low half first, so every pair of
  characters traded places, and a pattern row is a little-endian word with the
  leftmost pixel in the top nibble, so every glyph came out mirrored -- which
  is invisible on T, H, O and X. The whole graphics side is little-endian,
  being on the PCI side of the board, and all three follow from that.

  The decode is checked against text whose wording is known before it is
  rendered rather than against a document, which is the only way to tell a
  right answer from a plausible one here.
- **The field clock stopped for a guest looping in pure lifted code.** The
  runtime took its turn at `func_table_call()` and at a device access, and a
  loop that waits on a RAM word written only by an interrupt handler does
  neither -- so no field fell due, so no interrupt arrived, so the word never
  changed. The lifter now marks backward branches and the runtime gets a turn
  every 1024 of them.
- **The sound board never reported itself ready.** A guest's command loop
  waits on bit `0x01000000` of `0xF0080004` before writing each byte, and that
  loop runs inside the VBlank handler. A board that is never ready therefore
  did not merely lose sound: `irq_tick()` will not re-enter while a dispatch
  is in progress, so no further field ever fell due and the machine wedged.
  With no 68000 to be busy, the board now answers "always ready to accept,
  never anything to say" -- the second bit, `0x02000000`, stays clear so the
  reply poll returns "no data" rather than reading a byte that is not there.
- **Real3D status at `0x84000000` was a constant.** The boot copies nine dwords
  of it into RAM with `stwbrx` — the chip is on the PCI side, so the cached
  copy is byte-reversed — and then waits for bit `0x02000000` of that copy to
  change. Byte-reversed that is bit 1 of the register, and a constant never
  changes, so the guest never left the wait. Toggled per read now.
- **`tools/ppc_interp.py` performed SCRIPTS memory moves that `src/scsi.c`
  already rejected.** The runtime had a `mapped()` guard on both source and
  destination; the interpreter had only a size check, so the two disagreed
  about what a malformed script does. A garbage DSP value is routine — the
  register image is written a word at a time and one word lands on offset
  `0x2C` — and one resulting move zeroed 8 KB of a guest's work RAM. The guard
  is now the same on both sides.

### Added
- **The Real3D's upload FIFO at 0x94000000** (`src/bus.c`, `src/scsi.c`). It is
  one port rather than a window, so the DMA guard's range check threw every
  transfer to it away as out of range -- 464 of them in a two-thousand-field
  run of The Lost World, which is most of what that guest sends its graphics
  processor. The bytes are now kept in arrival order and `bus_texfifo()` hands
  them to whatever comes to render them. (In this title they turn out to be a
  gamma ramp rather than geometry, but they were being discarded unseen, and
  "discarded unseen" is the part that mattered.)
- **A guest function-entry histogram** and **a device-write watch**, both under
  `M3_LOOP_GUARD`. `M3_TRACE_CALLS` only sees indirect dispatch, so a frame
  task's whole call tree was invisible; counting every function entry is the
  difference between "the frame task ran" and "here is what it did". The
  device watch names the guest function behind a register write, which the RAM
  watch cannot see because device writes leave the inline path.
- **The decrementer** (`src/irq.c`). SPR 22 counts down and its 0 -> -1
  crossing takes an exception at vector 0x900 -- a separate exception from
  the external interrupt at 0x500, gated only by MSR[EE]. The Lost World's
  timing calibration waits on a counter that only the handler it installs
  there ever writes, and the pointer to that handler appears nowhere in RAM
  because the processor calls it, not the game. Without this the wait cannot
  end, and the interpreter and the recompiled binary sat in it alike.
- **A watch on a guest RAM address** (`M3_WATCH`). `tools/ppc_interp.py` has
  `--watch` and the runtime had no equivalent, because lifted code writes work
  RAM through the inline path in `lift.h` without the bus ever seeing it. Set
  it to an address at build time and every write is reported with the guest
  function that made it. Compiles to nothing otherwise.
- **`model3recomp_dump_scene()`** (`src/scenedump.c`): writes Real3D culling
  and polygon RAM, tilemap VRAM and work RAM to files from the frame hook, so
  a renderer can be written against what a game really put there.
- **A loop guard for locating a spinning guest** (`M3_LOOP_GUARD`). A
  recompiled game has no program counter to inspect; when one wedges in a
  loop that touches no device and dispatches through no pointer, nothing
  outside it knows where "there" is. Built with this defined, the lifter's
  marks let the runtime name the guest address it is looping at and the
  function it entered to get there. Compiles to nothing otherwise.
- **PCI configuration space** (`src/bus.c`, `tools/ppc_interp.py`): the MPC105
  port pair at `0xF0800CF8` / `0xF0C00CFC`, answering for the Real3D at device
  13 (`0x16C311DB`) and the 53C810 at device 14 (`0x00011000`). A game that
  probes for these will not finish booting without them. Config writes are
  dropped: both devices are at fixed addresses in this memory map, so there is
  nothing to relocate.
- **Floating point in the boot interpreter**: loads, stores, arithmetic,
  compares and conversions. FPRs stay raw 64-bit patterns so the boot path's
  `lfd`/`stfd` block copies remain bit-exact, and arithmetic converts at the
  edges. `tests/test_fp.py` covers the round trip and the cases where the
  hardware answers with an infinity and Python would raise.
- `tools/ppc_interp.py --break-at ADDR`: report whether execution ever reaches
  an address, without stopping the run. Repeatable, so one run answers a whole
  call chain — which is how the boot stalls above were located. Watch hits now
  carry an instruction count and field number.
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
- `M3_TRACE_IO` records device accesses in the same format the interpreter's
  `--trace-io` writes, so the two can be diffed line for line.
- ROM loader assembles the banked CROM and the VROM, not just the program
  image. A port without them boots and then has nothing to show, because every
  read of either returns zero.
- Accessors for tilemap VRAM, Real3D culling and polygon RAM, and the VROM.
- Inline fast path for guest memory: work RAM is handled in `lift.h` instead of
  calling into the bus for every load and store.

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
