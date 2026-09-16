"""PowerPC 603e -> C static recompiler for Sega Model 3.

  python tools/ppc_lifter.py <image.bin> <outdir> <prefix> [options]

    --base ADDR        guest address of image[0]   (default 0xFFE00000)
    --entry ADDR       extra entry point; repeatable
    --entries FILE     file of hex addresses, one per line
    --funcs-per-file N default 200
    --stats            print the unimplemented-instruction histogram

Emits, matching the house layout:
    <prefix>_code_000.c ...   lifted functions, N per file
    <prefix>_funcs.h          one declaration per function
    <prefix>_register.c       <prefix>_register_all(), one register call each

Nothing here knows what game it is. Base address, seeds and entry points all
come from the command line.
"""

import argparse
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc_disasm as D

# PowerPC exception vectors. Nothing ever *branches* to these -- the processor
# jumps to them -- so a lifter that discovers functions by following calls will
# never find an interrupt handler unless they are seeded by hand.
VECTORS = (0x100, 0x200, 0x300, 0x400, 0x500, 0x600, 0x700, 0x800,
           0x900, 0xC00, 0xD00, 0xF00,
           0x1000, 0x1100, 0x1200)   # 603e software TLB miss


def R(n):
    return "PPC_R(%d)" % n


def F(n):
    return "PPC_F(%d)" % n


def ea(i):
    """Effective address for a D-form load/store: (rA|0) + simm."""
    return "%s + %d" % (R(i.ra), i.simm) if i.ra else "(uint32_t)(%d)" % i.simm


def eax(i):
    """Effective address for an X-form (indexed) load/store: (rA|0) + rB."""
    return "%s + %s" % (R(i.ra), R(i.rb)) if i.ra else R(i.rb)


class Emitter:
    """One PPC instruction -> one or more C statements.

    Returns None for anything it does not handle; the caller turns that into a
    counted UNIMPL() rather than a guess.
    """

    def __init__(self):
        self.unimpl = Counter()

    @staticmethod
    def _rc(i, expr):
        return " CR0(%s);" % expr if i.rc else ""

    def emit(self, i):
        if i.mn is None:
            return None
        fn = getattr(self, "i_" + i.mn.replace(".", "_"), None)
        if fn is None:
            return None
        return fn(i)

    # ---- integer arithmetic ---------------------------------------------
    def i_addi(self, i):
        if i.ra:
            return "%s = %s + %d;" % (R(i.rd), R(i.ra), i.simm)
        return "%s = (uint32_t)(%d);" % (R(i.rd), i.simm)

    def i_addis(self, i):
        v = (i.uimm << 16) & 0xFFFFFFFF
        if i.ra:
            return "%s = %s + 0x%08Xu;" % (R(i.rd), R(i.ra), v)
        return "%s = 0x%08Xu;" % (R(i.rd), v)

    def i_addic(self, i):
        return ("{ uint64_t t = (uint64_t)%s + (uint64_t)(uint32_t)(%d);"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), i.simm, R(i.rd)))

    def i_addic_(self, i):
        return self.i_addic(i) + " CR0(%s);" % R(i.rd)

    def i_add(self, i):
        return "%s = %s + %s;" % (R(i.rd), R(i.ra), R(i.rb)) + self._rc(i, R(i.rd))

    def i_addc(self, i):
        return ("{ uint64_t t = (uint64_t)%s + (uint64_t)%s;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rb), R(i.rd))) + self._rc(i, R(i.rd))

    def i_adde(self, i):
        return ("{ uint64_t t = (uint64_t)%s + (uint64_t)%s + CA;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rb), R(i.rd))) + self._rc(i, R(i.rd))

    def i_addze(self, i):
        return ("{ uint64_t t = (uint64_t)%s + CA;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rd))) + self._rc(i, R(i.rd))

    def i_addme(self, i):
        return ("{ uint64_t t = (uint64_t)%s + CA + 0xFFFFFFFFull;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rd))) + self._rc(i, R(i.rd))

    # subf family: rD = ~rA + rB + carry-in
    def i_subf(self, i):
        return "%s = %s - %s;" % (R(i.rd), R(i.rb), R(i.ra)) + self._rc(i, R(i.rd))

    def i_subfc(self, i):
        return ("{ uint64_t t = (uint64_t)(uint32_t)~%s + (uint64_t)%s + 1ull;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rb), R(i.rd))) + self._rc(i, R(i.rd))

    def i_subfe(self, i):
        return ("{ uint64_t t = (uint64_t)(uint32_t)~%s + (uint64_t)%s + CA;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rb), R(i.rd))) + self._rc(i, R(i.rd))

    def i_subfze(self, i):
        return ("{ uint64_t t = (uint64_t)(uint32_t)~%s + CA;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rd))) + self._rc(i, R(i.rd))

    def i_subfme(self, i):
        return ("{ uint64_t t = (uint64_t)(uint32_t)~%s + CA + 0xFFFFFFFFull;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), R(i.rd))) + self._rc(i, R(i.rd))

    def i_subfic(self, i):
        return ("{ uint64_t t = (uint64_t)(uint32_t)~%s"
                " + (uint64_t)(uint32_t)(%d) + 1ull;"
                " %s = (uint32_t)t; SET_CA(t >> 32); }"
                % (R(i.ra), i.simm, R(i.rd)))

    def i_neg(self, i):
        return ("%s = (uint32_t)(-(int32_t)%s);" % (R(i.rd), R(i.ra))
                + self._rc(i, R(i.rd)))

    def i_mulli(self, i):
        return "%s = (uint32_t)((int32_t)%s * %d);" % (R(i.rd), R(i.ra), i.simm)

    def i_mullw(self, i):
        return ("%s = (uint32_t)((int32_t)%s * (int32_t)%s);"
                % (R(i.rd), R(i.ra), R(i.rb))) + self._rc(i, R(i.rd))

    def i_mulhw(self, i):
        return ("%s = (uint32_t)(((int64_t)(int32_t)%s * (int64_t)(int32_t)%s) >> 32);"
                % (R(i.rd), R(i.ra), R(i.rb))) + self._rc(i, R(i.rd))

    def i_mulhwu(self, i):
        return ("%s = (uint32_t)(((uint64_t)%s * (uint64_t)%s) >> 32);"
                % (R(i.rd), R(i.ra), R(i.rb))) + self._rc(i, R(i.rd))

    # PPC leaves rD undefined on divide-by-zero and does NOT trap. Yielding 0
    # is defined for us and keeps the host from raising SIGFPE.
    def i_divw(self, i):
        return ("%s = %s ? (uint32_t)((int32_t)%s / (int32_t)%s) : 0u;"
                % (R(i.rd), R(i.rb), R(i.ra), R(i.rb))) + self._rc(i, R(i.rd))

    def i_divwu(self, i):
        return ("%s = %s ? (%s / %s) : 0u;"
                % (R(i.rd), R(i.rb), R(i.ra), R(i.rb))) + self._rc(i, R(i.rd))

    # ---- compare ---------------------------------------------------------
    def i_cmpi(self, i):
        return "CMPS(%d, %s, %d);" % (i.crfd, R(i.ra), i.simm)

    def i_cmp(self, i):
        return "CMPS(%d, %s, %s);" % (i.crfd, R(i.ra), R(i.rb))

    def i_cmpli(self, i):
        return "CMPU(%d, %s, 0x%Xu);" % (i.crfd, R(i.ra), i.uimm)

    def i_cmpl(self, i):
        return "CMPU(%d, %s, %s);" % (i.crfd, R(i.ra), R(i.rb))

    # ---- logical ---------------------------------------------------------
    def i_andi_(self, i):
        return "%s = %s & 0x%Xu; CR0(%s);" % (R(i.ra), R(i.rd), i.uimm, R(i.ra))

    def i_andis_(self, i):
        v = (i.uimm << 16) & 0xFFFFFFFF
        return "%s = %s & 0x%08Xu; CR0(%s);" % (R(i.ra), R(i.rd), v, R(i.ra))

    def i_ori(self, i):
        return "%s = %s | 0x%Xu;" % (R(i.ra), R(i.rd), i.uimm)

    def i_oris(self, i):
        return "%s = %s | 0x%08Xu;" % (R(i.ra), R(i.rd), (i.uimm << 16) & 0xFFFFFFFF)

    def i_xori(self, i):
        return "%s = %s ^ 0x%Xu;" % (R(i.ra), R(i.rd), i.uimm)

    def i_xoris(self, i):
        return "%s = %s ^ 0x%08Xu;" % (R(i.ra), R(i.rd), (i.uimm << 16) & 0xFFFFFFFF)

    def _log(self, i, expr):
        return "%s = %s;" % (R(i.ra), expr) + self._rc(i, R(i.ra))

    def i_and(self, i):
        return self._log(i, "%s & %s" % (R(i.rd), R(i.rb)))

    def i_or(self, i):
        return self._log(i, "%s | %s" % (R(i.rd), R(i.rb)))

    def i_xor(self, i):
        return self._log(i, "%s ^ %s" % (R(i.rd), R(i.rb)))

    def i_nand(self, i):
        return self._log(i, "~(%s & %s)" % (R(i.rd), R(i.rb)))

    def i_nor(self, i):
        return self._log(i, "~(%s | %s)" % (R(i.rd), R(i.rb)))

    def i_eqv(self, i):
        return self._log(i, "~(%s ^ %s)" % (R(i.rd), R(i.rb)))

    def i_andc(self, i):
        return self._log(i, "%s & ~%s" % (R(i.rd), R(i.rb)))

    def i_orc(self, i):
        return self._log(i, "%s | ~%s" % (R(i.rd), R(i.rb)))

    def i_extsb(self, i):
        return self._log(i, "(uint32_t)(int32_t)(int8_t)%s" % R(i.rd))

    def i_extsh(self, i):
        return self._log(i, "(uint32_t)(int32_t)(int16_t)%s" % R(i.rd))

    def i_cntlzw(self, i):
        return self._log(i, "CNTLZW(%s)" % R(i.rd))

    # ---- rotate / shift --------------------------------------------------
    def i_rlwinm(self, i):
        return ("%s = ROTL32(%s, %d) & MASK(%d, %d);"
                % (R(i.ra), R(i.rd), i.sh, i.mb, i.me)) + self._rc(i, R(i.ra))

    def i_rlwnm(self, i):
        return ("%s = ROTL32(%s, %s & 31) & MASK(%d, %d);"
                % (R(i.ra), R(i.rd), R(i.rb), i.mb, i.me)) + self._rc(i, R(i.ra))

    def i_rlwimi(self, i):
        return ("{ uint32_t m = MASK(%d, %d);"
                " %s = (ROTL32(%s, %d) & m) | (%s & ~m); }"
                % (i.mb, i.me, R(i.ra), R(i.rd), i.sh, R(i.ra))
                ) + self._rc(i, R(i.ra))

    # A PPC shift of 32..63 yields 0. C would be undefined, and x86 would
    # silently take the count mod 32 -- so the bit-5 test is load-bearing.
    def i_slw(self, i):
        return ("%s = (%s & 0x20) ? 0u : (%s << (%s & 31));"
                % (R(i.ra), R(i.rb), R(i.rd), R(i.rb))) + self._rc(i, R(i.ra))

    def i_srw(self, i):
        return ("%s = (%s & 0x20) ? 0u : (%s >> (%s & 31));"
                % (R(i.ra), R(i.rb), R(i.rd), R(i.rb))) + self._rc(i, R(i.ra))

    def i_srawi(self, i):
        return ("{ int32_t s = (int32_t)%s;"
                " SET_CA((s < 0) && ((uint32_t)s & ((1u << %d) - 1u)));"
                " %s = (uint32_t)(s >> %d); }"
                % (R(i.rd), i.sh, R(i.ra), i.sh)) + self._rc(i, R(i.ra))

    def i_sraw(self, i):
        return ("{ int32_t s = (int32_t)%s; uint32_t n = %s & 0x3F;"
                " if (n > 31) { SET_CA(s < 0); %s = (uint32_t)(s >> 31); }"
                " else { SET_CA((s < 0) && ((uint32_t)s & ((1u << n) - 1u)));"
                " %s = (uint32_t)(s >> n); } }"
                % (R(i.rd), R(i.rb), R(i.ra), R(i.ra))) + self._rc(i, R(i.ra))

    # ---- loads / stores --------------------------------------------------
    def i_lbz(self, i):
        return "%s = MEM_R8(%s);" % (R(i.rd), ea(i))

    def i_lhz(self, i):
        return "%s = MEM_R16(%s);" % (R(i.rd), ea(i))

    def i_lwz(self, i):
        return "%s = MEM_R32(%s);" % (R(i.rd), ea(i))

    def i_lha(self, i):
        return "%s = (uint32_t)(int32_t)(int16_t)MEM_R16(%s);" % (R(i.rd), ea(i))

    def i_stb(self, i):
        return "MEM_W8(%s, %s);" % (ea(i), R(i.rd))

    def i_sth(self, i):
        return "MEM_W16(%s, %s);" % (ea(i), R(i.rd))

    def i_stw(self, i):
        return "MEM_W32(%s, %s);" % (ea(i), R(i.rd))

    # Update forms write rA only after the access completes.
    def _u(self, i, body):
        return "{ uint32_t _a = %s + %d; %s %s = _a; }" % (R(i.ra), i.simm, body, R(i.ra))

    def i_lbzu(self, i):
        return self._u(i, "%s = MEM_R8(_a);" % R(i.rd))

    def i_lhzu(self, i):
        return self._u(i, "%s = MEM_R16(_a);" % R(i.rd))

    def i_lwzu(self, i):
        return self._u(i, "%s = MEM_R32(_a);" % R(i.rd))

    def i_lhau(self, i):
        return self._u(i, "%s = (uint32_t)(int32_t)(int16_t)MEM_R16(_a);" % R(i.rd))

    def i_stbu(self, i):
        return self._u(i, "MEM_W8(_a, %s);" % R(i.rd))

    def i_sthu(self, i):
        return self._u(i, "MEM_W16(_a, %s);" % R(i.rd))

    def i_stwu(self, i):
        return self._u(i, "MEM_W32(_a, %s);" % R(i.rd))

    def i_lbzx(self, i):
        return "%s = MEM_R8(%s);" % (R(i.rd), eax(i))

    def i_lhzx(self, i):
        return "%s = MEM_R16(%s);" % (R(i.rd), eax(i))

    def i_lwzx(self, i):
        return "%s = MEM_R32(%s);" % (R(i.rd), eax(i))

    def i_lhax(self, i):
        return "%s = (uint32_t)(int32_t)(int16_t)MEM_R16(%s);" % (R(i.rd), eax(i))

    def i_stbx(self, i):
        return "MEM_W8(%s, %s);" % (eax(i), R(i.rd))

    def i_sthx(self, i):
        return "MEM_W16(%s, %s);" % (eax(i), R(i.rd))

    def i_stwx(self, i):
        return "MEM_W32(%s, %s);" % (eax(i), R(i.rd))

    def _ux(self, i, body):
        return "{ uint32_t _a = %s + %s; %s %s = _a; }" % (R(i.ra), R(i.rb), body, R(i.ra))

    def i_lbzux(self, i):
        return self._ux(i, "%s = MEM_R8(_a);" % R(i.rd))

    def i_lhzux(self, i):
        return self._ux(i, "%s = MEM_R16(_a);" % R(i.rd))

    def i_lwzux(self, i):
        return self._ux(i, "%s = MEM_R32(_a);" % R(i.rd))

    def i_lhaux(self, i):
        return self._ux(i, "%s = (uint32_t)(int32_t)(int16_t)MEM_R16(_a);" % R(i.rd))

    def i_stbux(self, i):
        return self._ux(i, "MEM_W8(_a, %s);" % R(i.rd))

    def i_sthux(self, i):
        return self._ux(i, "MEM_W16(_a, %s);" % R(i.rd))

    def i_stwux(self, i):
        return self._ux(i, "MEM_W32(_a, %s);" % R(i.rd))

    # Byte-reversed forms: the guest is big-endian, these are its escape hatch.
    def i_lwbrx(self, i):
        return ("{ uint32_t _v = MEM_R32(%s); %s = ((_v >> 24) & 0xFFu)"
                " | ((_v >> 8) & 0xFF00u) | ((_v << 8) & 0xFF0000u)"
                " | ((_v << 24) & 0xFF000000u); }" % (eax(i), R(i.rd)))

    def i_lhbrx(self, i):
        return ("{ uint32_t _v = MEM_R16(%s);"
                " %s = ((_v >> 8) & 0xFFu) | ((_v << 8) & 0xFF00u); }"
                % (eax(i), R(i.rd)))

    def i_stwbrx(self, i):
        return ("{ uint32_t _v = %s; MEM_W32(%s, ((_v >> 24) & 0xFFu)"
                " | ((_v >> 8) & 0xFF00u) | ((_v << 8) & 0xFF0000u)"
                " | ((_v << 24) & 0xFF000000u)); }" % (R(i.rd), eax(i)))

    def i_sthbrx(self, i):
        return ("{ uint32_t _v = %s; MEM_W16(%s,"
                " ((_v >> 8) & 0xFFu) | ((_v << 8) & 0xFF00u)); }"
                % (R(i.rd), eax(i)))

    def i_lmw(self, i):
        return ("for (unsigned _n = %d; _n < 32; _n++)"
                " m3_ctx.r[_n] = MEM_R32((%s) + (_n - %d) * 4);"
                % (i.rd, ea(i), i.rd))

    def i_stmw(self, i):
        return ("for (unsigned _n = %d; _n < 32; _n++)"
                " MEM_W32((%s) + (_n - %d) * 4, m3_ctx.r[_n]);"
                % (i.rd, ea(i), i.rd))

    # Single-threaded, so a reservation once taken always holds.
    def i_lwarx(self, i):
        return "%s = MEM_R32(%s); m3_ctx.reserve = 1;" % (R(i.rd), eax(i))

    def i_stwcx_(self, i):
        return ("MEM_W32(%s, %s); CRF(0, 2u | (m3_ctx.xer >> 31));"
                " m3_ctx.reserve = 0;" % (eax(i), R(i.rd)))

    # ---- floating point --------------------------------------------------
    def i_lfs(self, i):
        return "%s = (double)m3_bits_to_f32(MEM_R32(%s));" % (F(i.rd), ea(i))

    def i_lfd(self, i):
        return "%s = m3_bits_to_f64(MEM_R64(%s));" % (F(i.rd), ea(i))

    def i_stfs(self, i):
        return "MEM_W32(%s, m3_f32_to_bits((float)%s));" % (ea(i), F(i.rd))

    def i_stfd(self, i):
        return "MEM_W64(%s, m3_f64_to_bits(%s));" % (ea(i), F(i.rd))

    def i_lfsx(self, i):
        return "%s = (double)m3_bits_to_f32(MEM_R32(%s));" % (F(i.rd), eax(i))

    def i_lfdx(self, i):
        return "%s = m3_bits_to_f64(MEM_R64(%s));" % (F(i.rd), eax(i))

    def i_stfsx(self, i):
        return "MEM_W32(%s, m3_f32_to_bits((float)%s));" % (eax(i), F(i.rd))

    def i_stfdx(self, i):
        return "MEM_W64(%s, m3_f64_to_bits(%s));" % (eax(i), F(i.rd))

    def i_stfiwx(self, i):
        return "MEM_W32(%s, (uint32_t)m3_f64_to_bits(%s));" % (eax(i), F(i.rd))

    def i_lfsu(self, i):
        return self._u(i, "%s = (double)m3_bits_to_f32(MEM_R32(_a));" % F(i.rd))

    def i_lfdu(self, i):
        return self._u(i, "%s = m3_bits_to_f64(MEM_R64(_a));" % F(i.rd))

    def i_stfsu(self, i):
        return self._u(i, "MEM_W32(_a, m3_f32_to_bits((float)%s));" % F(i.rd))

    def i_stfdu(self, i):
        return self._u(i, "MEM_W64(_a, m3_f64_to_bits(%s));" % F(i.rd))

    def _fp(self, i, expr):
        return "%s = %s;" % (F(i.rd), expr) + (" CRF(1, 0);" if i.rc else "")

    def i_fadd(self, i):
        return self._fp(i, "%s + %s" % (F(i.ra), F(i.rb)))

    def i_fsub(self, i):
        return self._fp(i, "%s - %s" % (F(i.ra), F(i.rb)))

    def i_fmul(self, i):
        return self._fp(i, "%s * %s" % (F(i.ra), F(i.rc_reg)))

    def i_fdiv(self, i):
        return self._fp(i, "%s / %s" % (F(i.ra), F(i.rb)))

    def i_fadds(self, i):
        return self._fp(i, "F32(%s + %s)" % (F(i.ra), F(i.rb)))

    def i_fsubs(self, i):
        return self._fp(i, "F32(%s - %s)" % (F(i.ra), F(i.rb)))

    def i_fmuls(self, i):
        return self._fp(i, "F32(%s * %s)" % (F(i.ra), F(i.rc_reg)))

    def i_fdivs(self, i):
        return self._fp(i, "F32(%s / %s)" % (F(i.ra), F(i.rb)))

    def i_fmadd(self, i):
        return self._fp(i, "%s * %s + %s" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fmsub(self, i):
        return self._fp(i, "%s * %s - %s" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fnmadd(self, i):
        return self._fp(i, "-(%s * %s + %s)" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fnmsub(self, i):
        return self._fp(i, "-(%s * %s - %s)" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fmadds(self, i):
        return self._fp(i, "F32(%s * %s + %s)" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fmsubs(self, i):
        return self._fp(i, "F32(%s * %s - %s)" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fnmadds(self, i):
        return self._fp(i, "F32(-(%s * %s + %s))" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fnmsubs(self, i):
        return self._fp(i, "F32(-(%s * %s - %s))" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    def i_fmr(self, i):
        return self._fp(i, F(i.rb))

    def i_fneg(self, i):
        return self._fp(i, "-%s" % F(i.rb))

    def i_fabs(self, i):
        return self._fp(i, "m3_fabs(%s)" % F(i.rb))

    def i_fnabs(self, i):
        return self._fp(i, "-m3_fabs(%s)" % F(i.rb))

    def i_frsp(self, i):
        return self._fp(i, "F32(%s)" % F(i.rb))

    def i_fres(self, i):
        return self._fp(i, "F32(1.0 / %s)" % F(i.rb))

    def i_frsqrte(self, i):
        return self._fp(i, "(1.0 / m3_sqrt(%s))" % F(i.rb))

    def i_fsel(self, i):
        return self._fp(i, "(%s >= 0.0) ? %s : %s" % (F(i.ra), F(i.rc_reg), F(i.rb)))

    # fctiw* leave the result in the low 32 bits of the FPR as a bit pattern,
    # which is why the game always follows them with stfd + lwz of the low half.
    def i_fctiw(self, i):
        return self._fp(i, "m3_bits_to_f64((uint64_t)(uint32_t)(int32_t)m3_round(%s))"
                        % F(i.rb))

    def i_fctiwz(self, i):
        return self._fp(i, "m3_bits_to_f64((uint64_t)(uint32_t)(int32_t)%s)" % F(i.rb))

    def i_fcmpu(self, i):
        return "FCMP(%d, %s, %s);" % (i.crfd, F(i.ra), F(i.rb))

    def i_fcmpo(self, i):
        return "FCMP(%d, %s, %s);" % (i.crfd, F(i.ra), F(i.rb))

    def i_mffs(self, i):
        return "%s = m3_bits_to_f64((uint64_t)m3_ctx.fpscr);" % F(i.rd)

    def i_mtfsf(self, i):
        return "m3_ctx.fpscr = (uint32_t)m3_f64_to_bits(%s);" % F(i.rb)

    def i_mtfsb0(self, i):
        return "m3_ctx.fpscr &= ~(1u << (31 - %d));" % i.rd

    def i_mtfsb1(self, i):
        return "m3_ctx.fpscr |= (1u << (31 - %d));" % i.rd

    # ---- condition register ----------------------------------------------
    def i_mfcr(self, i):
        return "%s = PPC_CR;" % R(i.rd)

    def i_mtcrf(self, i):
        crm = (i.raw >> 12) & 0xFF
        mask = 0
        for b in range(8):
            if crm & (0x80 >> b):
                mask |= 0xF << (28 - 4 * b)
        return ("PPC_CR = (PPC_CR & ~0x%08Xu) | (%s & 0x%08Xu);"
                % (mask, R(i.rd), mask))

    def i_mcrf(self, i):
        return "CRF(%d, m3_cr_get(&m3_ctx, %d));" % (i.crfd, i.crfs)

    def _cr(self, i, expr):
        return "CRB_SET(%d, %s);" % (i.rd, expr)

    def i_crand(self, i):
        return self._cr(i, "CRB(%d) & CRB(%d)" % (i.ra, i.rb))

    def i_cror(self, i):
        return self._cr(i, "CRB(%d) | CRB(%d)" % (i.ra, i.rb))

    def i_crxor(self, i):
        return self._cr(i, "CRB(%d) ^ CRB(%d)" % (i.ra, i.rb))

    def i_crnand(self, i):
        return self._cr(i, "!(CRB(%d) & CRB(%d))" % (i.ra, i.rb))

    def i_crnor(self, i):
        return self._cr(i, "!(CRB(%d) | CRB(%d))" % (i.ra, i.rb))

    def i_creqv(self, i):
        return self._cr(i, "CRB(%d) == CRB(%d)" % (i.ra, i.rb))

    def i_crandc(self, i):
        return self._cr(i, "CRB(%d) & !CRB(%d)" % (i.ra, i.rb))

    def i_crorc(self, i):
        return self._cr(i, "CRB(%d) | !CRB(%d)" % (i.ra, i.rb))

    # ---- system -----------------------------------------------------------
    _SPR = {1: "PPC_XER", 8: "PPC_LR", 9: "PPC_CTR"}

    def i_mfspr(self, i):
        s = self._SPR.get(i.spr)
        return ("%s = %s;" % (R(i.rd), s) if s
                else "%s = m3_ctx.spr[%d];" % (R(i.rd), i.spr))

    def i_mtspr(self, i):
        s = self._SPR.get(i.spr)
        return ("%s = %s;" % (s, R(i.rd)) if s
                else "m3_ctx.spr[%d] = %s;" % (i.spr, R(i.rd)))

    def i_mfmsr(self, i):
        return "%s = PPC_MSR;" % R(i.rd)

    def i_mtmsr(self, i):
        return "PPC_MSR = %s;" % R(i.rd)

    def i_mfsr(self, i):
        return "%s = m3_ctx.sr[%d];" % (R(i.rd), (i.raw >> 16) & 0xF)

    def i_mtsr(self, i):
        return "m3_ctx.sr[%d] = %s;" % ((i.raw >> 16) & 0xF, R(i.rd))

    def i_mfsrin(self, i):
        return "%s = m3_ctx.sr[(%s >> 28) & 0xF];" % (R(i.rd), R(i.rb))

    def i_mtsrin(self, i):
        return "m3_ctx.sr[(%s >> 28) & 0xF] = %s;" % (R(i.rb), R(i.rd))

    def i_mftb(self, i):
        return "%s = m3_timebase(%d);" % (R(i.rd), i.spr)

    # No cache and no TLB to maintain.
    def _nop(self, i):
        return "/* no-op on this target */"

    i_sync = i_isync = i_eieio = _nop
    i_icbi = i_dcbi = i_dcbf = i_dcbst = i_dcbt = i_dcbtst = _nop
    i_tlbie = i_tlbsync = i_tlbld = i_tlbli = _nop

    # dcbz is the exception: code uses it to zero memory, not just a cache.
    def i_dcbz(self, i):
        return ("{ uint32_t _a = (%s) & ~31u;"
                " for (unsigned _k = 0; _k < 32; _k += 4) MEM_W32(_a + _k, 0); }"
                % eax(i))

    def i_tw(self, i):
        return "/* tw: trap not modelled */"

    def i_twi(self, i):
        return "/* twi: trap not modelled */"

    def i_sc(self, i):
        return "/* sc: no supervisor mode on this target */"


def branch_cond(i):
    """C expression for 'this conditional branch is taken', or None if always.

    BO is 5 bits in the manual's MSB-first numbering:
      BO0  ignore the CR condition
      BO1  the CR bit value that means 'taken'
      BO2  do not decrement CTR
      BO3  the CTR==0 sense to test
    """
    bo = i.bo
    parts = []
    if not (bo & 0x04):                         # BO2 clear -> decrement CTR
        parts.append("(--PPC_CTR == 0)" if (bo & 0x02) else "(--PPC_CTR != 0)")
    if not (bo & 0x10):                         # BO0 clear -> test a CR bit
        parts.append("%sCRB(%d)" % ("" if (bo & 0x08) else "!", i.bi))
    return " && ".join(parts) if parts else None


class Function:
    def __init__(self, entry):
        self.entry = entry
        self.insns = []
        self.labels = set()
        self.truncated = False


# Real PowerPC functions are not this big. The cap exists so that an entry
# which turns out to point at data cannot walk the whole image before it
# notices; a genuine function hitting it would be reported as truncated.
MAX_FUNC_INSNS = 8000


def build(image, base, entry, hard, cap=MAX_FUNC_INSNS):
    """Recursive descent over one function's basic blocks.

    A function ends where control flow says it ends, not at the next address
    somebody guessed was an entry. That distinction is load-bearing:

      * `hard` holds addresses a `bl` names, or that open with a stack-frame
        prologue. Those are real function starts, so an unconditional branch
        to one is a tail call and ends the block.
      * Every other entry -- an exception vector, a jump-table arm, an address
        built by lis/addi, one fed back from a runtime miss -- is an extra way
        *into* code that may sit in the middle of another function. Truncating
        at one of those silently cuts the containing function short.

    The Lost World's entry point is exactly that case. RAM 0x00000000 is nine
    consecutive `bl` instructions, an init chain:

        00000000  bl 0x00000038
        00000004  bl 0x000000AC
        ...
        00000020  bl 0x00000404

    Address 0x14 was discovered as an entry in its own right, and bounding the
    function there emitted only the first five calls and then returned. The
    game booted, ran a fifth of its init, and quietly did nothing.
    """
    fn = Function(entry)
    size = len(image)
    seen = set()
    work = [entry]
    body = {}
    while work:
        pc = work.pop()
        while True:
            if pc in seen:
                fn.labels.add(pc)
                break
            off = pc - base
            if off < 0 or off + 4 > size or len(body) >= cap:
                fn.truncated = True
                break
            seen.add(pc)
            raw = struct.unpack_from(">I", image, off)[0]
            i = D.decode(raw, pc)
            body[pc] = i

            # An undecodable word is data, not a missing opcode. Stop: walking
            # on would lift a lookup table as if it were instructions.
            if i.mn is None:
                fn.truncated = True
                break

            if D.is_call(i):
                pc += 4
                continue
            if i.mn in ("bclr", "bcctr", "rfi"):
                if D.is_uncond(i):
                    break
                pc += 4
                continue
            if i.mn in ("b", "bc"):
                t = i.target
                # A branch to another function's start leaves this function,
                # conditionally or not. Following it merges the two and is
                # what turned a 2 MB ROM into 2.2 M lifted instructions.
                external = t is not None and t != entry and t in hard
                inside = t is not None and base <= t < base + size
                # A target outside the image cannot become a label: the walk
                # would refuse to emit code there and the C would not compile.
                # It is reached through the func table instead.
                if inside and not external:
                    fn.labels.add(t)
                    work.append(t)
                if D.is_uncond(i):
                    break
                pc += 4
                continue
            pc += 4
    fn.insns = sorted(body.items())
    return fn


def scan_reachable(image, base, seeds):
    """Walk control flow from the seeds; return (call targets, reached addrs).

    Scanning the whole image for `bl` instead looks tempting and is wrong: a
    2 MB ROM is mostly data, plenty of data words decode as a branch, and each
    bogus entry then walks thousands of "instructions" through a lookup table.
    On The Lost World that inflated the lift from 340 K instructions to 2.2 M,
    with a dozen functions each hitting the 60 K cap inside the float-constant
    pool. Only code the seeds actually reach is code.
    """
    size = len(image)
    lo, hi = base, base + size
    seen = set()
    calls = set()
    work = list(seeds)
    while work:
        pc = work.pop()
        while True:
            if pc in seen or not (lo <= pc < hi):
                break
            seen.add(pc)
            i = D.decode(struct.unpack_from(">I", image, pc - base)[0], pc)
            if i.mn is None:
                break
            if D.is_call(i):
                if i.target is not None and lo <= i.target < hi:
                    calls.add(i.target)
                    work.append(i.target)
                pc += 4
                continue
            if i.mn in ("bclr", "bcctr", "rfi"):
                if D.is_uncond(i):
                    break
                pc += 4
                continue
            if i.mn in ("b", "bc"):
                if i.target is not None and lo <= i.target < hi:
                    work.append(i.target)
                if D.is_uncond(i):
                    break
                pc += 4
                continue
            pc += 4
    return calls, seen


def discover(image, base, seeds, scan_pointers=False, scan_consts=True,
             scan_tables=True):
    """Find function entries, and which of them are real function starts."""
    size = len(image)
    lo, hi_ = base, base + size

    calls, seen = scan_reachable(image, base, seeds)
    entries = set(seeds) | calls
    hard = set(seeds) | calls          # a bl names a function; so does a seed

    # Reachability alone under-approximates badly: most of a Model 3 game is
    # entered through function pointers, and following 0x00000000 found only
    # 225 functions of the ~2600 that exist. So also take every `bl` target in
    # the image -- but only where it plausibly starts a function, meaning the
    # word before it is a return, an unconditional branch, or padding. Real
    # functions follow one of those; a float constant in a data pool does not,
    # which is what kept a dozen bogus entries from walking 60,000
    # "instructions" through the constant pool.
    def follows_boundary(addr):
        if addr - 4 < base:
            return True
        prev = D.decode(struct.unpack_from(">I", image, addr - 4 - base)[0],
                        addr - 4)
        if prev.mn is None:
            return False
        return D.is_return(prev) or (prev.mn in ("b", "bclr", "bcctr", "rfi")
                                     and D.is_uncond(prev) and not prev.lk)

    for addr, raw in D.words(image, base):
        i = D.decode(raw, addr)
        if not (D.is_call(i) and i.target is not None):
            continue
        t = i.target
        if not (lo <= t < hi_) or t in entries:
            continue
        if t in seen or follows_boundary(t):
            entries.add(t)
            hard.add(t)

    def code_at(addr):
        """Does this address hold something that decodes, inside the image?"""
        if addr & 3 or not (lo <= addr < hi_):
            return False
        w = struct.unpack_from(">I", image, addr - base)[0]
        return w not in (0, 0xFFFFFFFF) and D.decode(w, addr).mn is not None

    if scan_tables:
        # Switch statements compile to a jump table reached through bctr:
        #
        #     lis   rY, hi ; addi rY, rY, lo      ; table base
        #     lwzx  rW, rY, rZ                    ; rZ = index * 4
        #     mtctr rW ; bctr
        #
        # The arms sit inside the enclosing function and nothing names them
        # with a bl, so without this every one is a runtime func-table miss.
        base_of, tbl_reg = {}, {}
        for addr in sorted(seen):
            i = D.decode(struct.unpack_from(">I", image, addr - base)[0], addr)
            if i.mn == "addis" and i.ra == 0:
                base_of[i.rd] = i.uimm << 16
            elif i.mn == "addi" and i.ra in base_of and i.rd == i.ra:
                base_of[i.rd] = (base_of[i.ra] + i.simm) & 0xFFFFFFFF
            elif i.mn == "lwzx" and (i.ra in base_of or i.rb in base_of):
                tbl_reg[i.rd] = base_of.get(i.ra, base_of.get(i.rb))
            elif i.mn == "mtspr" and i.spr == 9 and i.rd in tbl_reg:
                tbl = tbl_reg.pop(i.rd)
                if tbl is None or not (lo <= tbl < hi_):
                    continue
                for k in range(256):          # bounded: tables are not huge
                    off = tbl - base + k * 4
                    if off + 4 > size:
                        break
                    t = struct.unpack_from(">I", image, off)[0]
                    if not code_at(t):
                        break
                    entries.add(t)
            elif i.mn in ("b", "bc", "bclr", "bcctr"):
                base_of.clear()
                tbl_reg.clear()

    if scan_consts:
        # lis rX,hi ; addi/ori rX,rX,lo -- how PowerPC code materialises a
        # function address to store in a table or call through LR. The Lost
        # World installs its interrupt handler exactly this way, and no bl
        # ever names it:
        #
        #     lis r9, 0x11 ; addi r9, r9, 0x7864   ; r9 = 0x00117864
        #     stw r9, -0x1284(r10)                 ; -> the handler slot
        pend = {}
        for addr in sorted(seen):
            i = D.decode(struct.unpack_from(">I", image, addr - base)[0], addr)
            if i.mn == "addis" and i.ra == 0:
                pend[i.rd] = i.uimm << 16
            elif i.mn in ("addi", "ori") and i.ra in pend and i.rd == i.ra:
                hi_half = pend.pop(i.ra)
                val = (hi_half + (i.simm if i.mn == "addi" else i.uimm)) & 0xFFFFFFFF
                if code_at(val):
                    entries.add(val)
            elif i.mn in ("b", "bc", "bclr", "bcctr"):
                pend.clear()

    if scan_pointers:
        # Aligned words in data pointing at decodable code: vtables and task
        # dispatch tables. Off by default -- on a mostly-data ROM this invents
        # far more entries than it finds.
        for addr, raw in D.words(image, base):
            if code_at(raw):
                entries.add(raw)

    return entries, hard


def fname(prefix, addr):
    return "%s_%08X" % (prefix, addr)


def emit_function(em, fn, prefix, entries):
    out = ["/* %s: function at 0x%08X (%d instructions) */"
           % (fname(prefix, fn.entry), fn.entry, len(fn.insns)),
           "void %s(void)" % fname(prefix, fn.entry),
           "{"]

    # `blr` is ambiguous. Usually it is a return, and the C call stack mirrors
    # the guest's, so `return;` is right. But PowerPC also uses mtlr+blr as a
    # computed jump, and The Lost World's boot ends exactly that way:
    #
    #     lis  r0, 0
    #     mtlr r0
    #     blr              ; jump to the game at RAM 0x00000000
    #
    # Translating that as `return;` drops out of the guest entirely and the
    # port silently does nothing. The two cases are told apart by where LR's
    # value came from: restored from the stack means a real return, built from
    # an immediate means a jump.
    imm_regs = set()
    lr_from_imm = False

    for addr, i in fn.insns:
        if addr in fn.labels:
            out.append("L_%08X: ;" % addr)
            imm_regs.clear()          # unknown path in: assume nothing
            lr_from_imm = False
        ret = "0x%08Xu" % (addr + 4)

        # Track which GPRs currently hold an immediate-derived value.
        if i.mn == "addis" and i.ra == 0:
            imm_regs.add(i.rd)
        elif i.mn == "addi" and i.ra == 0:
            imm_regs.add(i.rd)
        elif i.mn in ("addi", "ori", "oris") and i.ra in imm_regs and i.rd == i.ra:
            pass                       # still immediate-derived
        elif i.mn == "mtspr" and i.spr == 8:
            lr_from_imm = i.rd in imm_regs
        elif D.is_call(i):
            imm_regs.clear()           # a call clobbers LR and the volatiles
            lr_from_imm = False
        elif i.mn is not None and i.mn not in ("stw", "stb", "sth", "stwu",
                                               "cmp", "cmpi", "cmpl", "cmpli",
                                               "sync", "isync", "eieio"):
            imm_regs.discard(i.rd)     # conservatively: rD is no longer known
            if i.mn in ("and", "or", "xor", "nand", "nor", "eqv", "andc",
                        "orc", "rlwinm", "rlwimi", "rlwnm", "slw", "srw",
                        "sraw", "srawi", "extsb", "extsh", "cntlzw"):
                imm_regs.discard(i.ra)

        if i.mn == "b" and not i.lk:
            t = i.target
            if t in fn.labels:
                c = "goto L_%08X;" % t
            elif t in entries:
                c = "%s(); return; /* tail call */" % fname(prefix, t)
            else:
                c = "CALL(0x%08Xu); return; /* tail call, unresolved */" % t
        elif i.mn == "b" and i.lk:
            t = i.target
            c = ("PPC_LR = %s; %s();" % (ret, fname(prefix, t)) if t in entries
                 else "PPC_LR = %s; CALL(0x%08Xu);" % (ret, t))
        elif i.mn == "bc":
            cond = branch_cond(i)
            t = i.target
            if i.lk:
                act = "{ PPC_LR = %s; CALL(0x%08Xu); }" % (ret, t)
            elif t in fn.labels:
                act = "goto L_%08X;" % t
            elif t in entries:
                act = "{ %s(); return; }" % fname(prefix, t)
            else:
                act = "{ CALL(0x%08Xu); return; }" % t
            c = act if cond is None else "if (%s) %s" % (cond, act)
        elif i.mn == "bclr":
            cond = branch_cond(i)
            if i.lk:
                act = "{ uint32_t _t = PPC_LR; PPC_LR = %s; CALL(_t); }" % ret
            elif lr_from_imm:
                # mtlr from an immediate: this is a jump, not a return.
                act = "{ CALL(PPC_LR); return; } /* computed jump via LR */"
            else:
                act = "return;"
            c = act if cond is None else "if (%s) %s" % (cond, act)
        elif i.mn == "bcctr":
            cond = branch_cond(i)
            act = ("{ uint32_t _t = PPC_CTR; PPC_LR = %s; CALL(_t); }" % ret
                   if i.lk else "{ CALL(PPC_CTR); return; }")
            c = act if cond is None else "if (%s) %s" % (cond, act)
        elif i.mn == "rfi":
            c = "return; /* rfi */"
        else:
            c = em.emit(i)

        if c is None:
            em.unimpl[i.mn or "op%d" % i.op] += 1
            c = ('UNIMPL(0x%08Xu, 0x%08Xu, "%s"); /* NOT TRANSLATED */'
                 % (addr, i.raw, i.mn or "?"))
        out.append("    " + c + ("  /* %s */" % i.mn if i.mn else ""))
    out.append("}")
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("outdir")
    ap.add_argument("prefix")
    ap.add_argument("--base", default="0xFFE00000")
    ap.add_argument("--entry", action="append", default=[])
    ap.add_argument("--entries")
    ap.add_argument("--funcs-per-file", type=int, default=200)
    ap.add_argument("--no-scan-tables", action="store_true",
                    help="do not follow bctr jump tables")
    ap.add_argument("--no-scan-consts", action="store_true",
                    help="do not treat lis/addi address pairs as entries")
    ap.add_argument("--scan-pointers", action="store_true",
                    help="also treat code-looking pointers in data as entries")
    ap.add_argument("--stats", action="store_true")
    a = ap.parse_args()

    base = int(a.base, 0)
    image = open(a.image, "rb").read()
    lo, hi = base, base + len(image)

    seeds = {int(x, 0) for x in a.entry}
    seeds.add(base)          # a computed jump can land on the image's start
    if a.entries:
        for line in open(a.entries):
            line = line.split("#")[0].strip()
            if line:
                seeds.add(int(line, 0))

    # Seed the exception vectors both where MSR[IP]=1 puts them (0xFFF00000)
    # and where MSR[IP]=0 does (0x00000000), whichever lands in this image.
    for v in VECTORS:
        for vbase in (base, 0x00000000, 0xFFF00000):
            if lo <= vbase + v < hi:
                seeds.add(vbase + v)

    entries, hard = discover(image, base, seeds, a.scan_pointers,
                             not a.no_scan_consts, not a.no_scan_tables)
    entries = {e for e in entries if lo <= e < hi}
    hard = {e for e in hard if lo <= e < hi}

    em = Emitter()
    fns = [build(image, base, e, hard) for e in sorted(entries)]
    fns = [f for f in fns if f.insns]

    os.makedirs(a.outdir, exist_ok=True)
    chunks = [fns[i:i + a.funcs_per_file]
              for i in range(0, len(fns), a.funcs_per_file)]

    for n, chunk in enumerate(chunks):
        with open(os.path.join(a.outdir, "%s_code_%03d.c" % (a.prefix, n)), "w") as f:
            f.write("/* Generated by tools/ppc_lifter.py -- do not edit. */\n")
            f.write('#include "model3recomp/lift.h"\n')
            f.write('#include "%s_funcs.h"\n\n' % a.prefix)
            for fn in chunk:
                f.write(emit_function(em, fn, a.prefix, entries))
                f.write("\n\n")

    with open(os.path.join(a.outdir, "%s_funcs.h" % a.prefix), "w") as f:
        f.write("/* Generated by tools/ppc_lifter.py -- do not edit. */\n")
        f.write("#ifndef %s_FUNCS_H\n#define %s_FUNCS_H\n\n"
                % (a.prefix.upper(), a.prefix.upper()))
        for fn in fns:
            f.write("void %s(void);\n" % fname(a.prefix, fn.entry))
        f.write("\nvoid %s_register_all(void);\n\n#endif\n" % a.prefix)

    with open(os.path.join(a.outdir, "%s_register.c" % a.prefix), "w") as f:
        f.write("/* Generated by tools/ppc_lifter.py -- do not edit. */\n")
        f.write('#include "model3recomp/func_table.h"\n')
        f.write('#include "%s_funcs.h"\n\n' % a.prefix)
        f.write("void %s_register_all(void)\n{\n" % a.prefix)
        for fn in fns:
            f.write("    func_table_register(0x%08Xu, %s);\n"
                    % (fn.entry, fname(a.prefix, fn.entry)))
        f.write("}\n")

    total = sum(len(fn.insns) for fn in fns)
    bad = sum(em.unimpl.values())
    print("functions      : %d" % len(fns))
    print("instructions   : %d" % total)
    print("files          : %d + funcs.h + register.c" % len(chunks))
    print("unimplemented  : %d  (%.3f%%)"
          % (bad, 100.0 * bad / total if total else 0.0))
    if a.stats:
        for mn, n in em.unimpl.most_common(30):
            print("  %-12s x%d" % (mn, n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
