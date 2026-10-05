"""Checks on the Real3D structure decoding.

A 3D decode that is wrong does not fail, it draws -- so these assert the
invariants that told us the decode was right in the first place, and which
break loudly if anyone changes the polygon walk or the VROM interleave.

The VROM checks need an image, which is not in the repository. Point
M3_VROM at one (tools/rom_loader.py builds it) or they skip.

  python tests/test_real3d.py
"""

import math
import os
import struct
import sys

FAILED = []


def check(name, cond, detail=""):
    if cond:
        print("ok  " + name)
    else:
        FAILED.append(name)
        print("FAIL " + name + ("  " + detail if detail else ""))


def sx24(w):
    """The top 24 bits of a word, sign-extended."""
    v = (w >> 8) & 0xFFFFFF
    return v - (1 << 24) if v & 0x800000 else v


def walk_model(words, base, limit=6000):
    """Parse one model.

    A polygon is a seven-word header followed by four words for each vertex
    not reused from the previous one (header bits 0..3), and the model ends
    on bit 2 of header word 1. Returns (polygons, end, worst normal error,
    largest absolute coordinate); polygons is 0 if this is not a model.
    """
    i = base
    polys = 0
    worst = 0.0
    biggest = 0
    while polys < limit:
        if i + 7 > len(words):
            return 0, i, worst, biggest
        ph = words[i:i + 7]
        n = [sx24(ph[k]) / 4194304.0 for k in (1, 2, 3)]
        err = abs(math.sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) - 1.0)
        worst = max(worst, err)
        if err > 0.002:
            return 0, i, worst, biggest
        nverts = 4 if (ph[0] & 0x40) else 3
        new = nverts - bin(ph[0] & 0x0F).count("1")
        if new < 0:
            return 0, i, worst, biggest
        for k in range(new):
            o = i + 7 + 4 * k
            if o + 3 > len(words):
                return 0, i, worst, biggest
            for q in range(3):
                biggest = max(biggest, abs(sx24(words[o + q])))
        i += 7 + 4 * new
        polys += 1
        if ph[1] & 4:
            return polys, i, worst, biggest
    return 0, i, worst, biggest


def words_of(path, endian):
    with open(path, "rb") as f:
        d = f.read()
    return struct.unpack("%s%dI" % (endian, len(d) // 4), d)


def test_header_field_widths():
    """The header's vertex-count and reuse bits, on a hand-built header."""
    check("a header with bit 6 clear is three vertices",
          (0x00000410 & 0x40) == 0)
    check("a header with bit 6 set is four vertices",
          (0x00000450 & 0x40) != 0)
    check("reuse bits count from bits 0..3",
          bin(0x0000040B & 0x0F).count("1") == 3)
    check("bit 2 of header word 1 ends a model", (0x00000004 & 4) != 0)


def test_sign_extension():
    check("sx24 keeps a positive value", sx24(0x007FFFFF_00 & 0xFFFFFFFF) >= 0)
    check("sx24 sign-extends the top bit", sx24(0xFF000000) == -65536,
          "got %d" % sx24(0xFF000000))
    check("sx24 of zero is zero", sx24(0) == 0)


def test_vrom(path):
    """The facts that settled the VROM layout and the vertex scale.

    A correct image is a 16-lane interleave read big-endian, and it is full
    of models whose header normals are unit vectors. Read little-endian, or
    interleaved as two groups of eight, it has none at all -- not few, none.
    """
    be = words_of(path, ">")
    le = words_of(path, "<")

    def model_starts(words, want, cap):
        """Scan for headers whose normal is a unit vector and that parse."""
        out = []
        step = max(1, cap // (want * 400))
        for i in range(0, cap, step):
            if i + 4 > len(words):
                break
            n = [sx24(words[i + 1 + k]) / 4194304.0 for k in range(3)]
            L = n[0] * n[0] + n[1] * n[1] + n[2] * n[2]
            if abs(L - 1.0) > 1e-9:
                continue
            p, end, _, _ = walk_model(words, i)
            if p >= 20:
                out.append(end)
                if len(out) >= want:
                    break
        return out

    seeds = model_starts(be, 4, len(be))
    check("VROM read big-endian contains models", bool(seeds))
    if not seeds:
        return

    chained = 0
    extents = []
    for seed in seeds:
        cur = seed
        for _ in range(400):
            p, end, worst, biggest = walk_model(be, cur)
            if p == 0 or worst > 0.002:
                break
            chained += 1
            extents.append(biggest)
            cur = end
    check("models chain back to back in VROM", chained >= 100,
          "chained %d" % chained)

    # Deliberately NOT asserted here: that extents crowd the top of the
    # +/-128 range. It is true over the image as a whole -- 866 of 1600
    # models chained from four separated seeds land between 112 and 128 --
    # but it is a property of a region, not of every region, and the seeds
    # this scan happens to find are in an area of very small models. The
    # bound itself is worth nothing as a check: a 24-bit field read with 16
    # fractional bits cannot leave +/-128, whatever the data says.
    units = sorted(e / 65536.0 for e in extents)
    print("    (extents seen: %.3f .. %.3f units, median %.3f)"
          % (units[0], units[-1], units[len(units) // 2]))

    # The negative control: the same image read the other way has nothing.
    check("the same image read little-endian has no models",
          not model_starts(le, 1, 400000))


def main():
    test_header_field_widths()
    test_sign_extension()

    vrom = os.environ.get("M3_VROM", "roms/lw_vrom.bin")
    if os.path.exists(vrom):
        test_vrom(vrom)
    else:
        print("skip  VROM checks (no image; set M3_VROM)")

    if FAILED:
        print("\n%d check(s) failed: %s" % (len(FAILED), ", ".join(FAILED)))
        return 1
    print("\nall Real3D structure checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
