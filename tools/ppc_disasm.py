"""PowerPC 603e instruction decoder.

Board-level: every Model 3 game runs the same CPU, so this lives with the
runtime rather than with any one game.

The 603e is plain 32-bit PowerPC. No paired singles (that is Gekko), no
AltiVec, no 64-bit. Decode is a switch on the 6-bit primary opcode and, for
the three compound opcodes (19, 31, 63), a second switch on the extended
opcode.

Fields are named as the PowerPC Programming Environments Manual names them.
Bit numbering below is the normal little-end-is-31 C convention, NOT the
manual's MSB-is-0 convention -- except for MB/ME and the CR bit indices,
which stay in the manual's numbering because that is how the mask and
condition-register semantics are defined. See m3_mask() in ppc.h.
"""

import struct
from collections import namedtuple

Insn = namedtuple("Insn", "addr raw op xo mn rd ra rb rc_reg simm uimm "
                          "bo bi bd li aa lk rc oe sh mb me spr crfd crfs "
                          "nb to target")


def _f(raw, hi, lo):
    return (raw >> lo) & ((1 << (hi - lo + 1)) - 1)


def _sext(v, bits):
    m = 1 << (bits - 1)
    return (v ^ m) - m


# Primary-opcode table for the simple (non-compound) forms.
_D_FORM = {
     3: "twi",   7: "mulli",  8: "subfic", 10: "cmpli", 11: "cmpi",
    12: "addic", 13: "addic.", 14: "addi", 15: "addis",
    24: "ori",  25: "oris",  26: "xori",  27: "xoris",
    28: "andi.", 29: "andis.",
    32: "lwz",  33: "lwzu",  34: "lbz",   35: "lbzu",
    36: "stw",  37: "stwu",  38: "stb",   39: "stbu",
    40: "lhz",  41: "lhzu",  42: "lha",   43: "lhau",
    44: "sth",  45: "sthu",  46: "lmw",   47: "stmw",
    48: "lfs",  49: "lfsu",  50: "lfd",   51: "lfdu",
    52: "stfs", 53: "stfsu", 54: "stfd",  55: "stfdu",
}

# opcode 31, extended opcode -> mnemonic. oe/rc variants are folded in by the
# caller reading insn.oe / insn.rc rather than multiplying the table by four.
_X31 = {
      0: "cmp",    4: "tw",     32: "cmpl",
      8: "subfc",  10: "addc",  11: "mulhwu", 75: "mulhw",
     40: "subf",   104: "neg",  136: "subfe", 138: "adde",
    200: "subfze", 202: "addze", 232: "subfme", 234: "addme",
    235: "mullw",  266: "add",  459: "divwu", 491: "divw",
     23: "lwzx",   55: "lwzux", 87: "lbzx",   119: "lbzux",
    151: "stwx",   183: "stwux", 215: "stbx", 247: "stbux",
    279: "lhzx",   311: "lhzux", 343: "lhax", 375: "lhaux",
    407: "sthx",   439: "sthux",
    533: "lswx",   597: "lswi", 661: "stswx", 725: "stswi",
    535: "lfsx",   567: "lfsux", 599: "lfdx", 631: "lfdux",
    663: "stfsx",  695: "stfsux", 727: "stfdx", 759: "stfdux",
    983: "stfiwx",
     20: "lwarx",  150: "stwcx.",
    790: "lhbrx",  534: "lwbrx", 918: "sthbrx", 662: "stwbrx",
     26: "cntlzw", 28: "and",   60: "andc",  124: "nor",
    284: "eqv",    316: "xor",  412: "orc",  444: "or",   476: "nand",
    954: "extsb",  922: "extsh",
     24: "slw",    536: "srw",  792: "sraw", 824: "srawi",
     19: "mfcr",   144: "mtcrf", 339: "mfspr", 467: "mtspr",
     83: "mfmsr",  146: "mtmsr", 595: "mfsr", 210: "mtsr",
    659: "mfsrin", 242: "mtsrin", 371: "mftb",
    306: "tlbie",  566: "tlbsync", 854: "eieio", 598: "sync",
    978: "tlbld",  1010: "tlbli",   # 603e software TLB reload
    982: "icbi",   470: "dcbi", 54: "dcbst", 86: "dcbf",
    278: "dcbt",   246: "dcbtst", 1014: "dcbz",
    310: "eciwx",  438: "ecowx",
}

# opcode 19
_X19 = {
     0: "mcrf",  16: "bclr",  528: "bcctr",
    33: "crnor", 129: "crandc", 150: "isync", 193: "crxor",
   225: "crnand", 257: "crand", 289: "creqv", 417: "crorc", 449: "cror",
    50: "rfi",
}

# opcode 63 (double-precision FP) and 59 (single-precision FP)
_X63 = {
     0: "fcmpu", 32: "fcmpo", 12: "frsp", 14: "fctiw", 15: "fctiwz",
    18: "fdiv",  20: "fsub",  21: "fadd", 23: "fsel", 25: "fmul",
    26: "frsqrte", 28: "fmsub", 29: "fmadd", 30: "fnmsub", 31: "fnmadd",
    40: "fneg",  72: "fmr",  136: "fnabs", 264: "fabs",
    38: "mtfsb1", 70: "mtfsb0", 134: "mtfsfi", 583: "mffs", 711: "mtfsf",
    64: "mcrfs",
}
_X59 = {
    18: "fdivs", 20: "fsubs", 21: "fadds", 24: "fres", 25: "fmuls",
    28: "fmsubs", 29: "fmadds", 30: "fnmsubs", 31: "fnmadds",
}


def decode(raw, addr):
    op = _f(raw, 31, 26)
    d = dict(addr=addr, raw=raw, op=op, xo=0, mn=None,
             rd=_f(raw, 25, 21), ra=_f(raw, 20, 16), rb=_f(raw, 15, 11),
             rc_reg=_f(raw, 10, 6),
             simm=_sext(raw & 0xFFFF, 16), uimm=raw & 0xFFFF,
             bo=_f(raw, 25, 21), bi=_f(raw, 20, 16), bd=0, li=0,
             aa=_f(raw, 1, 1), lk=raw & 1, rc=raw & 1, oe=_f(raw, 10, 10),
             sh=_f(raw, 15, 11), mb=_f(raw, 10, 6), me=_f(raw, 5, 1),
             spr=0, crfd=_f(raw, 25, 23), crfs=_f(raw, 20, 18),
             nb=_f(raw, 15, 11), to=_f(raw, 25, 21), target=None)

    if op in _D_FORM:
        d["mn"] = _D_FORM[op]
    elif op == 16:                                   # bc
        d["mn"] = "bc"
        d["bd"] = _sext(raw & 0xFFFC, 16)
        d["target"] = (d["bd"] if d["aa"] else addr + d["bd"]) & 0xFFFFFFFF
    elif op == 18:                                   # b
        d["mn"] = "b"
        d["li"] = _sext(raw & 0x03FFFFFC, 26)
        d["target"] = (d["li"] if d["aa"] else addr + d["li"]) & 0xFFFFFFFF
    elif op == 17:
        d["mn"] = "sc"
    elif op in (20, 21, 23):
        d["mn"] = {20: "rlwimi", 21: "rlwinm", 23: "rlwnm"}[op]
    elif op == 19:
        d["xo"] = _f(raw, 10, 1)
        d["mn"] = _X19.get(d["xo"])
    elif op == 31:
        d["xo"] = _f(raw, 10, 1)
        d["mn"] = _X31.get(d["xo"])
        if d["mn"] in ("mfspr", "mtspr"):
            d["spr"] = ((raw >> 16) & 0x1F) | (((raw >> 11) & 0x1F) << 5)
    elif op == 59:
        d["xo"] = _f(raw, 5, 1)
        d["mn"] = _X59.get(d["xo"])
    elif op == 63:
        d["xo"] = _f(raw, 5, 1)
        d["mn"] = _X63.get(d["xo"])
        if d["mn"] is None:
            d["xo"] = _f(raw, 10, 1)
            d["mn"] = _X63.get(d["xo"])
    return Insn(**d)


def is_branch(i):
    return i.mn in ("b", "bc", "bclr", "bcctr")


def is_call(i):
    return is_branch(i) and i.lk


def is_return(i):
    # blr with BO = 1z1zz (branch always) and no link
    return i.mn == "bclr" and not i.lk and (i.bo & 0x14) == 0x14


def is_uncond(i):
    if i.mn == "b":
        return True
    if i.mn in ("bc", "bclr", "bcctr"):
        return (i.bo & 0x14) == 0x14
    return False


def ends_block(i):
    return is_branch(i) or i.mn in ("rfi", "sc")


def words(buf, base):
    """Yield (addr, raw) over a big-endian image."""
    for off in range(0, len(buf) - 3, 4):
        yield base + off, struct.unpack_from(">I", buf, off)[0]
