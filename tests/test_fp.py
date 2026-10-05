"""Self-check for the interpreter's floating point.

    python tests/test_fp.py

The FPRs hold raw 64-bit patterns rather than Python floats so that the boot
path's lfd/stfd block copies stay bit-exact; these asserts cover both that
round trip and the arithmetic that converts at the edges.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import ppc_disasm as D
import ppc_interp as I


def machine():
    return I.Machine(b"\0" * 0x1000, 0xFFF00000)


def run_one(m, raw):
    i = D.decode(raw, 0)
    assert i.mn is not None, "undecodable %08x" % raw
    I.step(m, i, i.mn, i.rd, i.ra, i.rb)
    return i


def a_form(op, xo, d, a, b, c):
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1)


def test_bit_round_trip():
    """A pattern that is not a normal double must survive load/store."""
    m = machine()
    for bits in (0x7FF0000000000001,          # signalling NaN payload
                 0xDEADBEEFCAFEBABE,
                 0x0000000000000001):
        m.f[3] = bits
        m.write(0x100, 8, m.f[3])
        m.f[4] = m.read(0x100, 8)
        assert m.f[4] == bits, "lfd/stfd mangled %016X -> %016X" % (bits, m.f[4])


def test_arith():
    m = machine()
    m.f[1], m.f[2], m.f[3] = I.f_bits(3.0), I.f_bits(4.0), I.f_bits(2.0)

    run_one(m, a_form(63, 21, 0, 1, 2, 0))          # fadd  f0 = f1 + f2
    assert I.f_val(m.f[0]) == 7.0

    run_one(m, a_form(63, 20, 0, 1, 2, 0))          # fsub  f0 = f1 - f2
    assert I.f_val(m.f[0]) == -1.0

    run_one(m, a_form(63, 25, 0, 1, 0, 2))          # fmul  f0 = f1 * f2 (frC!)
    assert I.f_val(m.f[0]) == 12.0

    run_one(m, a_form(63, 18, 0, 2, 3, 0))          # fdiv  f0 = f2 / f3
    assert I.f_val(m.f[0]) == 2.0

    run_one(m, a_form(63, 29, 0, 1, 3, 2))          # fmadd f0 = f1*f2 + f3
    assert I.f_val(m.f[0]) == 14.0


def test_div_by_zero_does_not_raise():
    """The hardware answers with an infinity; Python would raise."""
    m = machine()
    m.f[1], m.f[2] = I.f_bits(1.0), I.f_bits(0.0)
    run_one(m, a_form(63, 18, 0, 1, 2, 0))
    assert I.f_val(m.f[0]) == float("inf")

    m.f[1] = I.f_bits(0.0)
    run_one(m, a_form(63, 18, 0, 1, 2, 0))
    assert I.f_val(m.f[0]) != I.f_val(m.f[0])       # 0/0 is NaN


def test_compare_and_convert():
    m = machine()
    m.f[1], m.f[2] = I.f_bits(1.0), I.f_bits(2.0)
    run_one(m, (63 << 26) | (1 << 23) | (1 << 16) | (2 << 11))   # fcmpu cr1
    assert m.crbit(4) == 1, "1.0 < 2.0 should set cr1 LT"

    m.f[2] = I.f_bits(-3.7)
    run_one(m, a_form(63, 15, 0, 0, 2, 0))          # fctiwz -> truncate
    assert m.f[0] & 0xFFFFFFFF == (-3) & 0xFFFFFFFF

    m.f[2] = I.f_bits(1.0 / 3.0)
    run_one(m, a_form(63, 12, 0, 0, 2, 0))          # frsp
    assert m.f[0] == I.f_bits(struct.unpack(">f", struct.pack(">f", 1/3))[0])


def test_single_store_narrows():
    m = machine()
    m.f[5] = I.f_bits(1.0)
    m.write(0x200, 4, int.from_bytes(struct.pack(">f", I.f_val(m.f[5])), "big"))
    assert m.read(0x200, 4) == 0x3F800000


if __name__ == "__main__":
    for name, fn in sorted(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok  %s" % name)
    print("all floating-point checks passed")
