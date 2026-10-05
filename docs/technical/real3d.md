# The Real3D scene

How the game describes a frame of 3D, and how much of that description is
actually known rather than assumed.

There is no oracle for any of this. MAME does not render Model 3 3D at all,
so the trick that settled the tilemaps -- put our picture next to MAME's and
look -- is not available. A 3D pipeline decoded wrongly does not fail, it
draws: it produces confident, plausible, wrong geometry, and nothing about
the output says which. So every structure here is paired with a check that
could only come out right if the decode is right, and the ones that have no
such check are marked as guesses.

## Where it lives

    0x8C000000   culling RAM low    4 MB    empty in every scene dumped so far
    0x8E000000   culling RAM high   1 MB    the scene graph and the matrices
    0x98000000   polygon RAM        4 MB    models the game builds per frame
    VROM        32 MB                       models and textures, fixed

A culling address is a **word index, not a byte offset**, and the top byte is
flags. `0x800000..0x83FFFF` is culling RAM high, below `0x100000` is culling
RAM low. The first viewport links to `0x800030` and the second viewport node
begins exactly at word `0x30`, which is what fixes this.

Everything except VROM is read little-endian, like the rest of the graphics
side. VROM is the exception -- see below.

## Viewports

A list starting at culling address `0x800000`, each `0x20` words, linked
through word `0x01`. This game builds three, all full-screen, with priorities
3, 1 and 0.

    [0x01]  next viewport
    [0x02]  root culling node
    [0x0C]  sin of the horizontal half-angle
    [0x0E]  sin of the vertical half-angle
    [0x14]  (w & 0x3FFF) >> 2 = width, (w >> 16) >> 2 = height
    [0x16]  base of the matrix table
    [0x1A]  viewport x and y, same packing as [0x14]

**Check:** `[0x14]` decodes to 496x384, the exact display size. `[0x0C]` and
`[0x0E]` are 0.33175 and 0.258819 -- the sines of 19.38 and 15 degrees --
and the ratio of their tangents is 1.3125, which is the aspect of a 496x384
screen. Two independent quantities agreeing on the display geometry is not
something a wrong reading produces.

## Culling nodes

Ten words.

    [0x00]  flags
    [0x03]  matrix index in the low 12 bits
    [0x04]  x, y, z translation, floats
    [0x07]  first child pointer
    [0x08]  second child pointer

A child pointer's top byte says what it points at: bit 24 a model, bit 26 a
pointer list, otherwise another culling node.

**Check:** nodes appear in runs at stride `0x0A` where `[0x00]` rises by
`0x400` and the matrix index in `[0x03]` by exactly 1, node after node. A
wrong stride or a wrong field offset breaks that immediately.

## Pointer lists

An array of addresses, each with flags in the top byte:

    bit 24 and bit 25 together   skip -- not an entry, and not an end either
    bit 25 alone                 last entry: process it, then stop

The skip case is worth its own line because getting it wrong is silent and
expensive. Attract mode's lists *begin* with `0x03000000`, so treating bit 25
as an end wherever it appears stops every list at its first element and loses
every model in it. That produced an empty scene that looked exactly like a
scene the game had not built yet.

**Check:** every address a correctly-walked list yields is a valid culling
node, and the unused entries are all the same dummy value, `0x800800`.

## Matrices

Twelve floats at `matrix_base + index * 12`: the translation first, then a
3x3 by rows.

    [ m3 m4  m5  | m0 ]
    [ m6 m7  m8  | m1 ]
    [ m9 m10 m11 | m2 ]

**Check:** matrix 1 is `[0,0,0, 1,0,0, 0,1,0, 0,0,1]`, which is exactly
identity under this reading and nothing under any other.

Matrix 0 is the camera. That one is weaker: it is the only one of the table's
first four that puts any of the scene in front of the eye, and it is a plain
rotation, which is what a view matrix should be. It has not been checked
against a scene whose correct appearance is known.

## Models and polygons

A model is a run of polygons. Each polygon is a seven-word header followed by
four words for every vertex that is **not** reused from the previous polygon.

    [0]  bits 0..3  reuse vertex n from the previous polygon
         bit 6      four vertices rather than three
    [1]  normal x, and bit 2 marks the last polygon of the model
    [2]  normal y
    [3]  normal z
    [4]  colour, r/g/b in bits 31..8

The normals are 24-bit fixed point over 2^22, taken from the top 24 bits of
each word. Vertices are the same: the top 24 bits are the coordinate, the low
byte is a per-vertex normal, and word 3 of each vertex is u and v.

**Check, and it is the strongest one here:** the whole of polygon RAM tiles
into models under this rule with *zero slack* -- two models, back to back,
the second ending on the word after the last non-zero word -- and every one
of the 1260 header normals in that block comes out exactly unit length, worst
deviation 0.000000. That is what pinned the sign extension and the 2^22
scale; nothing else does both at once.

## VROM

VROM is the one region read **big-endian**, and it is built as a **16-lane**
interleave -- all sixteen chips in one group, two bytes each, 32 bytes per
group.

This was wrong for a long time and nothing complained. Built as two groups of
eight, the image is the right size, the culling tree still walks, the models
still have seven-word headers, and the whole thing is nonsense: the chips
supplying the high half of every word end up 16 MB from the ones supplying
the low half, so each word the Real3D reads is half of one model and half of
another.

The check is the normals again. A correct image has 31265 models whose every
header normal is unit length. The eight-lane image has **none at all** --
not few, none -- which is about as clear a signal as a ROM layout ever gives.

## The reference frame

Field 2080 of attract mode. The game hands the renderer 4525 triangles
across 55 models, and they resolve into rocks in the foreground with a cliff
face behind. That frame is what settled the vertex scale and the camera, and
it is what to render first after changing anything here -- everything before
it in attract mode is 2D over an empty or parked scene, which is exactly the
condition under which a broken renderer looks fine.

    M3_REAL3D_STATS=1 ./lostworld 2080

`M3_REAL3D_STATS` prints the triangle count and how many pixels reached the
framebuffer, on change. That is how field 2080 was found in the first place:
4525 triangles submitted and zero drawn, for twenty fields running.

## Two wrong answers, kept on purpose

The vertex scale is 1/256. It was committed twice before that, wrongly, and
both times with an argument:

- **1/1024**, from taking a flat 4:3 model for a backdrop that filled the
  screen and reading two matrices as independently agreeing on the size that
  implied. They were not agreeing on anything; they were being asked a
  leading question, with arithmetic on top to make it look derived.
- **1/65536**, from the observation that model extents crowd the top of the
  24-bit coordinate field. The observation is true and it is worthless: the
  raw integers fill the field whatever divisor is applied afterwards, so it
  says nothing whatever about the divisor. It looked like evidence.

The camera went the same way. Matrix 0 is a plain rotation sitting exactly
where a view transform belongs, and using it turned the only scene then
available from nothing into something -- which was taken as confirmation. On
a frame that actually has a scene in it, matrix 0 pushes everything off the
left of the screen and leaving it out gives the rocks. The node matrices
already carry geometry into view space.

Both failures have the same shape, and it is the shape to watch for here: a
3D pipeline never reports a wrong constant, it just draws. Only a frame with
a recognisable subject in it can say no.

## Why so much of attract mode is still black

Roughly a tenth of a sampled attract cycle shows substantial 3D: the rock
faces around fields 1500-1520 and 2020-2080, and a further sequence at
3220-3400. The rest is 2D over a scene that is genuinely nearly empty (the
warning and title screens submit a few hundred triangles, most of them a flat
backdrop) or over geometry that falls outside the frustum.

That last case is worth being precise about, because it looks like a bug and
may not be one. At field 1500 the scene's models sit a median 50 degrees off
the view axis, and rendering it at a 70-degree half-angle shows a coherent
landscape off to the side. At field 2080, with the identical viewport
frustum, the models sit a median 4 degrees off axis and the frame looks
right. So the transform is not obviously wrong -- 2080 is evidence that it
works -- and the scene at 1500 really does extend past the edges of a
21-degree view, which terrain does.

What has been ruled out: the traversal is complete (all 59 nodes that exist
in culling RAM are visited, and none of them is an LOD table); node
translations are all zero, so placement comes entirely from the matrix table;
and no single matrix serves as a per-frame camera -- the best candidate for
one scene scores 1358 triangles on screen and 1 on the next scene along.

## Textures

They work, and follow MAME's `sega/model3_v.cpp` (BSD-3-Clause): the guest
streams entries into the FIFO at 0x94000000, each one a length word, a header
word and then texels; an entry occupies `(2 + length/2)/4` words. Texels
arrive in 8x8 tiles in the order of MAME's decode tables (`TILE16` and
`TILE8` in `src/real3d.c`) and are written into a 2048x2048 sheet, two
2048x1024 pages stacked. 16-bit texels are 1-5-5-5, little-endian, red in
the top field.

The header's top byte says what an upload carries: 0x00 a texture and its
mip chain, 0x01 the texture alone, 0x02 the chain alone. Each mip level
lives in the bottom-right corner of its page, level i at
`(2048 - 2048/2^i, 1024 - 1024/2^i)` plus the texture's position over 2^i.
Uploading a chain as though it were the base turns every surface into
coloured static.

The polygon header fields, which had been wrong in three places:

    enabled   word 6 bit 10       (not bit 26)
    width     word 3 bits 3..5    (not word 6)
    height    word 3 bits 0..2    (not word 6)
    page      word 4 bit 6        (not word 5)
    x         32 * ((word4 & 0x1F) << 1 | (word5 >> 7) & 1)
    y         32 * (word5 & 0x1F)

Width and height are the low bits of word 3, whose top 24 bits are the
polygon normal, which is why looking at word 6 never found them.

## What actually unblocked this, and it was not the renderer

The guest could not read its own data ROM.

The banked CROM window at 0xFF000000 selects one of eight 8 MB banks, and the
select is **active low**: the bank is the written byte inverted, three bits of
it. What this did instead was shift the byte into a byte offset, and for the
values The Lost World writes -- 0xF7 above all -- that lands outside the
image, so every read through the window returned zero.

The comment in `src/bus.c` had said exactly that for a long time. It was left
alone because a game reading zeros where its data should be does not crash.
It draws less.

It drew a very great deal less. With the window returning zeros the guest
streamed 321 KB to the Real3D across an entire run, and that 321 KB is a
gamma ramp and a handful of 32x32 uploads to the same corner of the sheet --
which is why the FIFO looked like it carried no textures, and why the scene
had no environment in it, and why five parts of the model pointed at a
buffer nothing filled. With the window working the guest streams 3.4 MB,
464 texture entries with 461 distinct headers, and the tilemap gains
content too: the Sega copyright line on the attract screen was simply never
being read.

Three separate investigations here -- textures, the missing environment, the
missing model parts -- were all the same bug, one address decode away.

## There is no environment in the scene, and that is upstream

The renderer draws what it is given, and what it is given is not a scene.
Field 2040, every model in it:

    matrices 400-420   the figure, 21 parts          ~3500 triangles
    matrix  421        one object in polygon RAM        764
    matrices 422-445   the same 8-triangle model,       192
                       instanced 24 times along a
                       curve running out to 36000 units
    matrices 393,394,  two 2-triangle and two            12
            446,447    4-triangle quads

That is the entire 4525. No terrain, no foliage, no ground -- a figure and
some scattered markers. Every shot sampled has the same shape: around 55
models and around 4500 triangles.

The traversal is not at fault. A loose scan of culling RAM for anything that
could be a node finds 62 addresses, and the only three the walk does not
reach through the node run are the viewport roots, which it reaches directly.
Culling RAM high is 3% populated -- 7782 non-zero words of 262144, nothing
past 0x352F. Culling RAM low is completely empty, 0 of 4194304 bytes, and no
DMA in an entire run targets it.

So the game is describing a small scene, and the place to look is what the
game is executing, not how the scene is walked. Worth stating plainly because
the renderer has been the suspect for everything up to now and here it is
not: it is drawing all of what arrives.

## Five parts of the model never draw

Prompted by someone looking at a screenshot and saying it was half a model
in a clump rather than a scene. It is worth writing down how right that was.

The attract scene is flat: three root nodes, fifty-five leaves, no nesting,
no node with two live children. Each leaf is one matrix and one model. Eleven
of those leaves are parts of a single articulated object -- matrices 400 to
414, translations spread over about two hundred units, rotation rows all
unit length. That is an assembled figure, not a pile.

Five consecutive parts of it never render, every frame, in every scene
dumped:

    matrix 395   -> 0100003C   polygon RAM word 0x3C, which is empty.
                                Polygon RAM only ever has content from word
                                0x10000 up.
    matrix 396   -> 0FFF5488   top byte 0x0F
    matrix 397   -> 0FFF6028
    matrix 398   -> 0FFF65F8
    matrix 399   -> 0FFF6BC8

The four with top byte 0x0F have bits 24, 25, 26 and 27 all set, which is not
a combination the rest of the scene uses -- every model pointer that works
has 0x01. Read as models they resolve to nothing: masked to 23, 24, 22, 21
and 28 bits, in VROM and in polygon RAM, nothing parses.

They are clearly a related run. The gaps between their addresses are 0xBA0,
0x5D0, 0x5D0, and the gaps between their node[0x09] fields are 0xBA0, 0x5D0,
0x5D0 -- identical. So they are consecutive models in some buffer, and
node[0x09] is their offset within it. Whatever fills that buffer, this
runtime does not see it: the FIFO at 0x94000000 contains no polygon header
anywhere in it (zero unit-normal triples in 80390 words, read either way
round), and no DMA in a whole run targets polygon RAM at all.

So the figure is drawn with a run of its middle missing, which is exactly
what it looks like.

## Things that look wrong and are not

- **Long thin slivers.** Some polygons rasterise as near-degenerate spikes
  across the screen. They are real: all 53 in one frame are thin in three
  dimensions as well as on screen, with aspect ratios from 85 to 512. They
  are terrain strips seen at a grazing angle, not a decode fault.

## What is still not settled

- **The light's intensity.** The direction comes from the viewport now --
  words 0x04..0x06 are a unit vector, (-0.72288, -0.16226, -0.67156), length
  1.0000 to five places and identical in every scene dumped, which is what
  identifies them. Word 0x07 sits with them and looks like an intensity, but
  it reads 0.93 in one attract shot and 0.06 in another that is plainly not
  six per cent as bright, so it is left alone.
- **Textures.** Not done. Polygons draw in their header colour, which is why
  the reference frame is grey.
- **Layer priority.** The tilemaps are all drawn over the 3D. Model 3 can put
  a tilemap layer behind it.
- **Culling.** The node bounding data is read but not used; everything
  submitted is transformed.


## The in-game scene: singular matrices, and where they come from

Attract mode renders. A round does not: the picture is two full-screen quads
and a couple of thin white lines. The renderer is not at fault, and it is
worth writing down how that was established, because every step of it looked
like a renderer bug first.

`M3_NO_2D` draws the 3D on its own -- during a round register 0x20 reads
`0xF300`, so all four tilemap layers sit above the 3D and the sky layer is
opaque, and there is otherwise nothing to see. `M3_REAL3D_STATS` now reports
per viewport, and the round says:

    vp0 @800000  42 tris      0 px
    vp1 @800040   4 tris      0 px
    vp2 @800080  21114 tris  421715 px  back 9964 near 7117 degen 0 off 0

So 4033 triangles rasterise, and `M3_TRI_MAX` -- which drops any triangle
whose screen bounding box exceeds a given area and says what it was -- shows
two of them cover the screen on their own, black, luminous, at a constant
z of 1280. Dropping those leaves an empty frame. The scene is not being
hidden; it is not there.

`M3_NODE_DUMP`/`M3_NODE_FROM` print each culling node with the matrix it
loads. The round's 55 object nodes sit at `0x801600`, ten words apart, node
*k* using matrix 393+*k*, and the first four are the 2D overlays -- their
matrices carry scales of 496 and 1/360, which is what a screen-space quad
looks like. The rest should be the world. In attract they are:

    attract  mo=400  |rows| 1 1 1                t -67 -37 209
    in-game  mo=400  |rows| 0.554 0.107 0.437    t -4.5e6 -8.0e4 -5.6e7

Every in-game matrix is **rank 1**. In matrix 440 the rows are exact
multiples of one another -- `r1 = 0.1995*r0`, `r2 = 0.8114*r0` -- and the
same ratios appear in its neighbours. A singular transform maps a model to a
line, which is the picture.

The chain back from there, with the instrument that found each step:

  * The matrices reach culling RAM by DMA, `001BAE54 -> 8E00C9B0, 2688
    bytes`, which is 56 matrices starting at index 393 (`M3_TRACE_DMA`).
  * The source buffer in RAM is already bad, so it is not byte order and not
    the DMA (read the dump both ways: little-endian is right).
  * `M3_WATCH` on the source names `0x0010F068`, a transposing byte-swap
    copier: destination words 3..5 are the source's *column* 0. Its source
    register points at `0x0010E5D8`.
  * `0x0010E5D8` is a 20-entry matrix stack -- 960 bytes of identity and
    zeros -- with the depth byte at RAM `0x1256` and the current-matrix
    pointer at `0x1258`. The routine at `0x0010E998` resets all three.
  * `M3_FN_ARGS` prints a function's arguments on entry, and `M3_FN_MAT`
    dumps the matrix it is handed (a value under 32 means "the matrix this
    register points at"). The primitives are fine: the scale at `0x0010EEB8`
    gets (496, 68, 1) and (1/360, ...) and (1, 1, 1); the rotate at
    `0x0010E584` gets ordinary angles; the multiply at `0x0010EC08` is an
    in-place 3x3 that the lifter translates instruction for instruction.
  * The multiply's *other* operand is where it goes wrong:

        mat@001FDC80
          r0 -0.75578  -0.15629  -0.63192     |r0| = 0.9905
          r1  3.87e-06 -2.11e-04  4.76e-05    |r1| = 2.2e-04
          r2  0.06136   0.01269   0.051309    |r2| = 0.0804  = 0.0812 * r0

    A rotation that has collapsed: row 0 is a unit vector, row 1 is zero,
    row 2 is parallel to row 0. That is what a basis built by cross products
    looks like when the two input vectors are parallel. Its direction is the
    one that then propagates into all 55 object matrices.

  * That buffer is on the guest stack and is filled by `0x0010E5CC`, which
    is `matrix_get`: it copies the *current* matrix out. So the current
    matrix was already collapsed. Its call history runs through `0x0010E9F8`
    -- a loop that scans a column for the largest |element|, i.e. partial
    pivoting, i.e. a matrix inverse.

Nothing in the runtime is implicated: no unimplemented instruction executes
during a round (all 746 are undecodable words in speculative functions, none
reached), the sine table the trig helpers read at `0xFFF10000` is correct in
the ROM image, and `fcmpu`, `fsel`, `fres` and `frsqrte` all translate
faithfully.

Open question, and the next thing to look at: what the game inverts, and
why the two vectors it builds the camera basis from are parallel during a
round and not during attract.

### Where it goes wrong: the overlay scale is in the camera matrix

Two more measurements narrow it further.

The routine the collapsed matrix comes out of, `0x0010E9F8`, is a 3x3
Gauss-Jordan inverse with partial pivoting, and **it is computing the right
answer**. Feeding its input through an independent inverse gives the output
it produced, to five figures:

    input   -12987     82.643   -1.5995e5      det 1.06e9
             3091     -4509.9    38073        (entries ~1e5, so a
            14767      1016.6    1.8189e5      well-conditioned matrix
                                               would have det ~1e15)

    inverse -0.8113   -0.1678   -0.6783    <- rows 0 and 2 parallel
             1.89e-06 -2.11e-04  4.59e-05     to one part in 1e4
             0.0659    0.0136    0.0551

So the inverse is fine and its input is already nearly singular. That input
is the camera's own matrix, stored in the camera object at RAM `0x0010616C`
(`0x1A616C`), and its row magnitudes give it away:

    |rows| = 1.99e4   4624   2.45e5
                      68^2   496^2

496 and 68 are the screen-space scales the 2D overlay matrices carry, and
applying them twice is normal -- attract does it too, and its matrix 394 has
|rows| 2.46e5, 2.7e3, 1 for exactly that reason. What is not normal is that
the scale is still in the current matrix when the camera matrix is built.
In attract, matrix 393 is the overlay (496, 52, 1), 394 is the overlay
scaled again, and 395 onwards are orthonormal -- the matrix is reset in
between. In a round it never is: the identity load at `0x0010E9B4` runs
three times a field in attract and once in a round, and `matrix_set`
(`0x0010EBB8`, which copies a saved matrix into the current one instead)
runs four times in a round and not at all in attract.

The camera position itself is worth recording. `M3_FN_ARGS` on the inverse
prints it, and it jumps between two fields:

    field 4282   -928.97    1048.66    -65.50
    field 4283    2.276e6  -4936.41     2.154e7
    field 4284    2.279e6  -4751.45     2.157e7   (+3185, +185, +3015 a field)

After the jump it moves at a constant velocity, which is a camera following
a path. Whether the jump is the stage-one start position or the same scale
leaking in has not been established.

### Resolved: the matrix stack push was never executed

The rank-1 matrices were not the game's arithmetic and not the renderer.
`emit_function()` writes a lifted function's body in address order, and the
matrix push at `0x0010ED58` ends with a backward branch into the copy
routine below it -- so the emitted function began with the copy, returned,
and left the push itself after a `return;`, unreachable. See
docs/technical/execution-model.md.

Nothing was ever pushed or popped, every matrix accumulated onto the last,
and the 2D overlay's screen-space scales ended up squared inside the camera
matrix. With the push restored:

    view bounds   x -1158..1060   y -225..1571   z -1066..669
    biggest |t|   423             (it was 63,090,096)

and the scene appears. Attract is a textured T-Rex roaring in the rain; a
round is a jungle floor with trees, a horizon and figures in the distance.

The vertex scale moved with it. 1/256 had been checked against an attract
frame that resolved into rocks and a cliff face -- but that check ran with
the push broken, so a wrong scale in the matrices and a wrong scale here
cancelled out. Both scenes now agree on **1/4096**, twelve fractional bits
and twelve integer ones, and both have a recognisable subject at it. At
1/1024 the same round is one black box filling the screen.

Still open: during a round all four tilemap layers sit above the 3D and
layer 1 paints a pixel checkerboard of one flat colour over the whole
screen -- stipple translucency in the tile data, not a depth-bit artefact --
so the scene is seen through a heavy blue haze.

### Texel channel order: red is the top field

Textures were being read blue-first, the same way round as the tilemap
palette. They are the other way round.

This was settled once against the game's green jeep, which is exactly the
wrong subject: swapping red and blue leaves green alone, so the jeep looks
right either way. Dumping the sheet to a PNG and looking at it settles it in
one glance -- Ian and Sarah's faces are natural skin one way round and
cyanotic the other, the dinosaur hide is brown rather than blue, and the
T-Rex in attract stops being a blue silhouette. Pick a subject with a colour
a swap can change.

The dump itself had to be fixed first: it fired after the hundredth upload,
which is during the boot, and made the sheet look as though only its first
256 columns were ever written. `M3_TEX_DUMP_AT` picks the field, and at
field 4640 the sheet is 29% full and is a coherent atlas -- jeeps, faces,
hide, tyres, INGEN signage, "SHOOT OUTSIDE of THE SCREEN!".

### What is still wrong during a round, and what has been ruled out

Attract finds every one of its 15384 texels in the sheet. A round finds
2228 of 96684: **94406 resolve to texture page 1, and nothing in a whole
boot is ever uploaded to page 1.** Those polygons fall back to their flat
header colour, which is the grey leaf canopies, the pale trunks and the
solid white background.

`M3_TEX_PAGE=0` forces the page and finds 89678 of them -- but the content
is wrong, because the polygon's texture size often disagrees with the upload
at the same origin:

    polygon 0,512  128x256      upload at 0,512  is 256x128
    polygon 0,256  256x256      upload at 0,256  is 256x128
    polygon 896,896 128x128     upload at 896,896 is  64x32

The origin matches an upload exactly in 12 of the 14 rectangles sampled, so
the origin decode is right and the size decode is not; the sampler's wrap
then pulls in whatever texture sits next door, which is the speckled bark.

Ruled out, each by measurement:

  * upload page from bit 6 (set in 460 of 464 uploads): no change.
  * upload page from bit 5 (set in none of the early uploads and 200 of the
    late ones): worse, 80026 missing.
  * y as six bits instead of five plus a page: fixes the round and breaks
    attract, which then finds none of its texels.
  * polygon origin x as six contiguous bits of word 4, the way the upload
    header carries it: 89678 found drops to 18841. The borrowed low bit from
    the top of word 5 is right.
  * swapping the two polygon size exponents: still wrong, differently.
  * a UV scale bit in polygon word 1, which MAME documents: not set on any
    polygon in this scene.

Also open, and not yet looked at: the human figures in a round are
fragmented -- torso, arms and legs drawn in the right colours but not joined
up, with loose pieces beside them.

### The vertex scale is 2048, and a jointed model is what proves it

"The frame looks like a scene" put the scale at 4096. That is a weak test:
a scene looks plausible over a wide range, because everything in it scales
together.

A model either joins up or it does not. Only the vertices scale -- the node
translations that place a model's parts are floats in world units and do
not -- so at the wrong scale a jointed model comes apart while the world
around it stays put. On the jungle floor:

    1024   camera inside the geometry, one flat wash
    2048   a man in a blue shirt standing beside a green jeep, whole
    4096   the same man in pieces, limbs spaced apart
    8192   confetti

The T-Rex in attract survives any of them, which is why it never showed the
fault: it is one rigid model, so the scale only changes how big it looks.
That is worth remembering when picking a subject to check against -- a
single-piece model cannot tell you about scale, and a green jeep cannot tell
you about red and blue.

### The attract scene really is that dark

Its viewport asks for an ambient of 10/255, which is 0.039, against 0.353
for the jungle, and a sun intensity of 0.913. Flipping the sun vector makes
it darker still -- mean frame brightness 2.0 against 0.6 -- so the direction
is right and the night scene is lit the way the game asks.

### The upper half of texture memory comes from VROM, not the FIFO

Every terrain and tree polygon in a round asked for texture page 1, and no
FIFO upload in a whole boot ever set the page bit. Six readings of the page
and origin fields were tried and each fixed one scene by breaking the other,
because the premise was wrong: **page 1 is not filled by the FIFO at all.**

`0x90000000..0x9000000B` is the Real3D VROM texture port. The game writes
three registers there -- an address, a header and a control word -- and the
chip fetches a texture that is already in VROM rather than having its pixels
pushed through the FIFO. That is how a stage's scenery is loaded, and it is
why the FIFO stops growing at field 1500 and never moves again.

Three things hid it:

  * The SCRIPTS DMA guard did not map `0x90000000`, so the twelve-byte
    transfers were rejected as out of range before anything saw them.
  * The port needs real storage. The guest writes it a byte at a time, a
    byte store is a read-modify-write, and a port that reads back as zero
    keeps only the last byte of each register.
  * The address counts 16-bit texels and the port is written byte-reversed
    like every other Real3D register. Read as bytes, or little-endian, the
    sheet fills with noise; read this way it fills with planks, gravel,
    foliage and rock, and the jungle floor has a floor.

    248727 texels from the sheet, 0 never uploaded (it was 240850).

Still wrong, and now the obvious next thing: the textures repeat many times
across each surface, so the size the polygon asks for does not match the
size that was uploaded.
