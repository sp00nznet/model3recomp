"""Assemble a flat Model 3 program image from a MAME romset.

  python tools/rom_loader.py <romset.zip|romdir> <out.bin> [--verify]

The four program EPROMs interleave as 16-bit words into one image: EPROM n
supplies bytes 2n and 2n+1 of every 8-byte group, each 16-bit word
byte-swapped (MAME's ROM_LOAD64_WORD_SWAP). Lane order is the EPROM number
ascending.

Which four are the program ROMs is not guessed. Every candidate set is
interleaved and checked against the PowerPC exception vector table, which is
self-validating: the vector at 0xFFF00000 + 0x100*n opens by loading its own
number, `li r7, n`. Only the correct interleave produces that, so a romset
this tool accepts is a romset it has actually verified.

No ROM is written into the repository -- the output path is yours, and the
repo's .gitignore excludes the usual spellings.
"""

import argparse
import itertools
import os
import struct
import sys
import zipfile

VEC_CHECK = (0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x900)


def load_files(src):
    """Return {name: bytes} from a zip or a directory."""
    if os.path.isdir(src):
        out = {}
        for n in os.listdir(src):
            p = os.path.join(src, n)
            if os.path.isfile(p):
                out[n] = open(p, "rb").read()
        return out
    with zipfile.ZipFile(src) as z:
        return {i.filename: z.read(i) for i in z.infolist()}


def interleave(bufs):
    """bufs[0..3] -> one image, 16-bit word interleaved and byte-swapped."""
    n = len(bufs[0])
    out = bytearray(n * 4)
    for lane, buf in enumerate(bufs):
        for i in range(0, n, 2):
            out[i * 4 + lane * 2] = buf[i + 1]
            out[i * 4 + lane * 2 + 1] = buf[i]
    return bytes(out)


def vector_score(img):
    """How many exception vectors open with `li r7, <their own number>`.

    The image is 2 MB and sits at 0xFFE00000, so the vector page at
    0xFFF00000 is at offset 0x100000.
    """
    vbase = len(img) - 0x100000
    if vbase < 0:
        return 0
    hits = 0
    for v in VEC_CHECK:
        off = vbase + v
        if off + 4 > len(img):
            continue
        w = struct.unpack_from(">I", img, off)[0]
        # li r7, imm  ==  addi r7, 0, imm  ==  0x38E0____
        if (w & 0xFFFF0000) == 0x38E00000 and (w & 0xFFFF) == (v >> 8):
            hits += 1
    return hits


def interleave_n(bufs, lanes, word=2):
    """Interleave `lanes` chips, `word` bytes at a time, byte-swapped.

    The program and banked CROMs use four chips two bytes apart; VROM uses
    sixteen. Same shape, different width.
    """
    n = len(bufs[0])
    out = bytearray(n * lanes)
    for lane, buf in enumerate(bufs):
        for i in range(0, n, word):
            out[i * lanes + lane * word] = buf[i + 1]
            out[i * lanes + lane * word + 1] = buf[i]
    return bytes(out)


def build_region(files, names, lanes, out_path, label):
    """Assemble `names` (a multiple of `lanes`) into one interleaved image."""
    if len(names) % lanes:
        # The SCSP sample ROMs are the same size as the VROM chips and sort
        # after them, so trim to a whole number of lanes rather than refusing.
        keep = len(names) // lanes * lanes
        if not keep:
            print("%s: %d chips, need a multiple of %d; skipping"
                  % (label, len(names), lanes), file=sys.stderr)
            return 0
        print("%s: using the first %d of %d chips (the rest are another "
              "region)" % (label, keep, len(names)), file=sys.stderr)
        names = names[:keep]
    parts = []
    for g in range(0, len(names), lanes):
        parts.append(interleave_n([files[n] for n in names[g:g + lanes]], lanes))
    img = b"".join(parts)
    with open(out_path, "wb") as f:
        f.write(img)
    print("%-14s : %s  (0x%X bytes from %d chips)"
          % (label, out_path, len(img), len(names)))
    return len(img)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="romset .zip or a directory of chip dumps")
    ap.add_argument("out")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=0x80000,
                    help="program EPROM size (default 0x80000)")
    ap.add_argument("--banked", help="also write the banked CROM (CROM0..3)")
    ap.add_argument("--vrom", help="also write the VROM (texture/model data)")
    a = ap.parse_args()

    files = load_files(a.src)
    cands = sorted(n for n, b in files.items() if len(b) == a.size)
    if len(cands) < 4:
        print("found %d chips of size 0x%X; need at least 4"
              % (len(cands), a.size), file=sys.stderr)
        return 2

    best = None
    for combo in itertools.combinations(cands, 4):
        for order in itertools.permutations(combo):
            img = interleave([files[n] for n in order])
            s = vector_score(img)
            if best is None or s > best[0]:
                best = (s, order, img)
            if s == len(VEC_CHECK):
                break
        if best[0] == len(VEC_CHECK):
            break

    score, order, img = best
    if score < len(VEC_CHECK):
        print("could not find a program-ROM interleave that validates "
              "(best %d/%d vectors matched)." % (score, len(VEC_CHECK)),
              file=sys.stderr)
        print("candidates were: %s" % ", ".join(cands), file=sys.stderr)
        return 1

    with open(a.out, "wb") as f:
        f.write(img)

    base = 0x100000000 - len(img)
    print("program EPROMs : %s" % ", ".join(order))
    print("image          : %s  (0x%X bytes)" % (a.out, len(img)))
    print("maps at        : 0x%08X" % base)
    print("reset vector   : 0x%08X" % (base + len(img) - 0x100000 + 0x100))
    print("verified       : %d/%d exception vectors" % (score, len(VEC_CHECK)))

    # The banked CROM holds the bulk of the game's data and the VROM its
    # models and textures. A game whose banked CROM reads as zero boots and
    # then has nothing to show.
    if a.banked:
        names = sorted(n for n, b in files.items()
                       if len(b) == 0x200000 and n not in order)
        build_region(files, names, 4, a.banked, "banked CROM")
    if a.vrom:
        names = sorted(n for n, b in files.items() if len(b) == 0x400000)
        build_region(files, names, 16, a.vrom, "VROM")
    return 0


if __name__ == "__main__":
    sys.exit(main())
