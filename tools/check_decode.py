"""Conformance harness: our decoder vs capstone, over a whole ROM image.

Reports a pass/fail count against a fixed corpus, per repo_rules.md S8.
Capstone is the reference because it is independent of this codebase; it is a
development dependency only -- the shipped runtime does not use it.

  python tools/check_decode.py <image.bin> [--base 0xFFE00000] [--max-report 20]

Exit codes: 0 all decoded, 1 gaps found, 2 capstone unavailable.
"""
import sys, struct, argparse, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc_disasm as D
import re

# Capstone knows the whole PowerPC lineage. A 603e does not. Anything from the
# 64-bit, AltiVec, VSX or POWER8+ categories cannot execute on this CPU, so a
# capstone decode of one is capstone reading data as code -- not a gap in us.
_NOT_ON_603E = re.compile(
    r"^("
    r"v[a-z0-9_]+"                       # AltiVec
    r"|x[svx][a-z0-9_]+|lxv[a-z0-9_]*|stxv[a-z0-9_]*|lxs[a-z0-9_]*|stxs[a-z0-9_]*"  # VSX
    r"|ld|ldu|ldx|ldux|ldarx|lwa|lwax|lwaux|std|stdu|stdx|stdux|stdcx\.?"          # 64-bit ld/st
    r"|rld[a-z]*|rotld[a-z]*|sld|srd|srad|sradi|cntlzd|cnttzd|extsw|popcntd"        # 64-bit shift/rot
    r"|mulld|mulhd|mulhdu|divd|divdu|divde|divdeu|modsd|modud"                      # 64-bit arith
    r"|cmpd|cmpdi|cmpld|cmpldi|td|tdi|td[a-z]+i?"                                   # 64-bit cmp/trap
    r"|maddhd|maddhdu|maddld|addpcis|darn|attn|bcdadd|bcdsub|cmpb|cmpeqb|cmprb"     # POWER8/9
    r"|lq|stq|mtvsr[a-z]*|mfvsr[a-z]*|slbia|slbie|slbmte|slbmfee|tlbiel"
    r")$")

# Capstone renames a stdlib module on import; keep our own 'dis' out of the way.
try:
    from capstone import Cs, CS_ARCH_PPC, CS_MODE_32, CS_MODE_BIG_ENDIAN
except Exception as e:                                    # pragma: no cover
    print(f"capstone unavailable: {e}", file=sys.stderr)
    sys.exit(2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--base", default="0xFFE00000")
    ap.add_argument("--max-report", type=int, default=20)
    a = ap.parse_args()
    base = int(a.base, 0)
    buf = open(a.image, "rb").read()

    # Per-word, not a linear sweep: capstone's disasm() stops dead at the
    # first word it cannot decode, and a ROM is full of data.
    md = Cs(CS_ARCH_PPC, CS_MODE_32 | CS_MODE_BIG_ENDIAN)
    ref = {}
    for addr, raw in D.words(buf, base):
        w = struct.pack(">I", raw)
        for i in md.disasm(w, addr):
            ref[addr] = i.mnemonic

    total = ours = theirs = both = excluded = 0
    gaps = []
    for addr, raw in D.words(buf, base):
        if raw in (0x00000000, 0xFFFFFFFF):
            continue                       # padding, not an instruction
        total += 1
        mine = D.decode(raw, addr).mn
        cap = ref.get(addr)
        if mine: ours += 1
        if cap:  theirs += 1
        if mine and cap: both += 1
        if cap and not mine:
            if _NOT_ON_603E.match(cap):
                excluded += 1
            else:
                gaps.append((addr, raw, cap))

    from collections import Counter
    by_mn = Counter(m for _, _, m in gaps)

    print(f"image                  : {a.image} ({len(buf)} bytes @ {base:#010x})")
    print(f"non-padding words      : {total}")
    print(f"capstone decoded       : {theirs}")
    print(f"ppc_disasm decoded     : {ours}")
    print(f"both agree a decode    : {both}")
    ref603 = theirs - excluded
    cov = 100.0 * (ref603 - len(gaps)) / ref603 if ref603 else 0.0
    print(f"  of which not on 603e : {excluded}  (AltiVec/VSX/64-bit: data read as code)")
    print(f"603e-valid reference   : {ref603}")
    print(f"decoded by us and ref  : {both}")
    print(f"we decode, capstone no : {ours - both}  (informational)")
    print(f"coverage vs 603e ref   : {cov:.2f}%")
    print(f"decode gaps            : {len(gaps)}")
    for mn, n in by_mn.most_common(a.max_report):
        ex = next(g for g in gaps if g[2] == mn)
        print(f"  {mn:<12s} x{n:<6d}   e.g. {ex[0]:08X}  {ex[1]:08x}")
    return 0 if not gaps else 1


if __name__ == "__main__":
    sys.exit(main())
