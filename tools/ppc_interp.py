"""PowerPC 603e boot interpreter -- a build-time tool, not a runtime.

Model 3 games copy themselves out of banked CROM into RAM and execute there,
so the program ROM alone does not tell you what code exists or where. This
runs the boot path until the guest branches into RAM, then snapshots RAM for
the lifter to consume.

  python tools/ppc_interp.py <prog.bin> [options]

    --base ADDR        guest address of image[0]      (default 0xFFE00000)
    --crom-bank FILE   banked CROM image (CROM0..3)
    --entry ADDR       start address                  (default 0xFFF00100)
    --max N            instruction budget             (default 200000000)
    --snapshot FILE    write the RAM image here when it stops
    --trace N          print the first N instructions
    --trace-at ADDR    print instructions once PC first reaches ADDR
    --stop-in-ram      stop as soon as PC enters RAM  (the default)
    --watch ADDR       report every write to this address

This only has to cover the instructions the boot path uses. It must NOT grow
into a general Model 3 emulator -- if it does, the project has failed at its
own premise. See docs/technical/execution-model.md.
"""

import argparse
import os
import struct
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppc_disasm as D

M = 0xFFFFFFFF
RAM_SIZE = 0x00800000


def swap32(v):
    return ((v >> 24) & 0xFF) | ((v >> 8) & 0xFF00) |            ((v << 8) & 0xFF0000) | ((v << 24) & 0xFF000000)


def s32(v):
    v &= M
    return v - 0x100000000 if v & 0x80000000 else v


class Machine:
    def __init__(self, crom, base, crom_bank=None):
        self.crom = crom
        self.crom_size = len(crom)
        self.crom_bank = crom_bank or b""
        self.bank = 0

        self.ram = bytearray(RAM_SIZE)
        self.vram = bytearray(0x120000)      # tilegen VRAM + palette
        self.cull_lo = bytearray(0x400000)
        self.cull_hi = bytearray(0x100000)
        self.poly = bytearray(0x400000)
        self.backup = bytearray(0x20000)

        self.r = [0] * 32
        self.f = [0.0] * 32
        self.lr = self.ctr = self.xer = self.cr = self.fpscr = 0
        self.msr = 0x40 | 0x1000             # IP | ME, the 603e reset state
        self.sr = [0] * 16
        self.spr = {}

        self.pc = 0
        self.icount = 0
        self.irq_pending = 0
        self.irq_enable = 0
        self.io_ctrl = 0          # 0xF0040000 strobe latch; see src/bus.c
        self.io_ready = 0         # 0xF0040004 serial ready line; see src/bus.c
        self.field_period = 0     # instructions per field; 0 disables
        self.next_field = 0
        self.fields = 0
        self.irq_taken = 0
        self.in_handler = False
        self.scsi = bytearray(0x40)   # 53C810 register file
        self.scsi_log = []
        self.scsi_trace = False
        self.scsi_int = False
        self.scsi_moves = []
        self.io_log_f = None       # matches src/bus.c's M3_TRACE_IO format
        self.io_budget = 0
        self.dcache = {}                     # decoded-instruction cache
        self.hw_reads = Counter()
        self.hw_writes = Counter()
        self.watch = set()
        self.watch_hits = []
        self.calls = Counter()  # call-target histogram: what the game does
        self.hist = []          # ring of recent taken branches
        self.hist_max = 0
        self.ram_writes = 0
        self.ram_lo = RAM_SIZE
        self.ram_hi = 0

    # ---- memory -----------------------------------------------------------
    def _region(self, a):
        """Return (buffer, offset) for plain memory, else None."""
        if a < RAM_SIZE:
            return self.ram, a
        if 0x8C000000 <= a < 0x8C400000:
            return self.cull_lo, a - 0x8C000000
        if 0x8E000000 <= a < 0x8E100000:
            return self.cull_hi, a - 0x8E000000
        if 0x98000000 <= a < 0x98400000:
            return self.poly, a - 0x98000000
        if 0xF1000000 <= a < 0xF1120000:
            return self.vram, a - 0xF1000000
        if 0xF00C0000 <= a < 0xF00E0000:
            return self.backup, a - 0xF00C0000
        return None

    def read(self, a, n):
        a &= M
        reg = self._region(a)
        if reg is not None:
            buf, off = reg
            return int.from_bytes(buf[off:off + n], "big")
        # Fixed CROM window, mirrored to fill 8 MB.
        if a >= 0xFF800000:
            off = (a - 0xFF800000) % self.crom_size
            return int.from_bytes(self.crom[off:off + n], "big")
        # Banked CROM window.
        if 0xFF000000 <= a < 0xFF800000 and self.crom_bank:
            off = (a - 0xFF000000) + self.bank
            if off + n <= len(self.crom_bank):
                return int.from_bytes(self.crom_bank[off:off + n], "big")
            return 0
        return self.dev_read(a, n)

    def write(self, a, n, v):
        a &= M
        v &= (1 << (8 * n)) - 1
        if a in self.watch:
            self.watch_hits.append((self.pc, a, v))
        reg = self._region(a)
        if reg is not None:
            buf, off = reg
            buf[off:off + n] = v.to_bytes(n, "big")
            if buf is self.ram:
                self.ram_writes += 1
                if off < self.ram_lo:
                    self.ram_lo = off
                if off + n > self.ram_hi:
                    self.ram_hi = off + n
            return
        self.dev_write(a, n, v)

    # ---- 53C810 -----------------------------------------------------------
    # Step 1.x moves bulk data with the SCSI controller's DMA engine. Log what
    # the game actually writes before modelling any of it.
    def scsi_read(self, a, n):
        o = a & 0x3F
        v = int.from_bytes(bytes(self.scsi[o:o + n]), "big")
        if self.scsi_trace and o != 0x14:
            self.scsi_log.append(("R", self.pc, o, n, v))
        return v

    def scsi_write(self, a, n, v):
        o = a & 0x3F
        self.scsi[o:o + n] = v.to_bytes(n, "big")
        if self.scsi_trace:
            self.scsi_log.append(("W", self.pc, o, n, v))
        # Writing the top byte of DSP starts a SCRIPTS program.
        if o <= 0x2C < o + n:
            dsp = swap32(int.from_bytes(bytes(self.scsi[0x2C:0x30]), "big"))
            if dsp:
                self.scsi_run(dsp)

    def scsi_run(self, dsp):
        """Minimal 53C810 SCRIPTS engine: memory moves and transfer control.

        Step 1.x uses the SCSI DMA engine to shift bulk data. The device sits
        on the PCI side and is little-endian, so every dword is byte-reversed
        relative to the big-endian PowerPC.
        """
        for _ in range(4096):
            d0 = swap32(self.read(dsp, 4))
            d1 = swap32(self.read(dsp + 4, 4))
            cmd = d0 >> 24
            if (cmd & 0xC0) == 0xC0:                 # memory move
                cnt = d0 & 0xFFFFFF
                src, dst = d1, swap32(self.read(dsp + 8, 4))
                dsp += 12
                if cnt and cnt <= 0x400000:
                    self.scsi_moves.append((src, dst, cnt))
                    for k in range(cnt):
                        self.write(dst + k, 1, self.read(src + k, 1))
                continue
            if (cmd & 0xC0) == 0x80:                 # transfer control
                op = (cmd >> 3) & 7
                if op == 0:                          # JUMP
                    dsp = d1
                    continue
                if op == 3:                          # INT
                    self.scsi_int = True
                    self.scsi[0x14] |= 0x01          # ISTAT.DIP
                    self.scsi[0x0C] |= 0x04          # DSTAT.SIR
                    return
                return
            return                                    # block move: not modelled

    # ---- devices ----------------------------------------------------------
    def io_log(self, rw, a, n, v):
        if not self.io_log_f or self.io_budget <= 0:
            return
        self.io_budget -= 1
        self.io_log_f.write("%s %08X %u %08X\n" % (rw, a, n, v & 0xFFFFFFFF))
        if self.io_budget == 0:
            self.io_log_f.close()
            self.io_log_f = None

    def dev_read(self, a, n):
        """Device read with big-endian byte lanes, matching src/bus.c.

        Registers are defined as 32-bit words but the guest reaches several of
        them with lbz/stb. Ignoring the lane -- which this did at first -- makes
        a byte write of 0x20 to 0xF0100014 read back as 0x00000020 instead of
        0x20000000, so the interrupt-enable mask ends up with entirely
        different bits set from the ones the hardware would have.
        """
        if (a & 0xFE000000) == 0xC0000000:
            v = self.scsi_read(a, n)
            self.io_log("R", a, n, v)
            return v
        self.hw_reads[a & 0xFFFFFFFC] += 1
        word = self.dev_read_word(a & ~3, True)
        if n == 4:
            v = word
        else:
            sh = 8 * (4 - n - (a & 3))
            v = (word >> sh) & ((1 << (8 * n)) - 1)
        self.io_log("R", a, n, v)
        return v

    def dev_write(self, a, n, v):
        self.io_log("W", a, n, v)
        if (a & 0xFE000000) == 0xC0000000:
            self.scsi_write(a, n, v)
            return
        self.hw_writes[a & 0xFFFFFFFC] += 1
        w = a & ~3
        if n == 4:
            self.dev_write_word(w, v)
            return
        sh = 8 * (4 - n - (a & 3))
        mask = (((1 << (8 * n)) - 1) << sh) & M
        cur = self.dev_read_word(w, False)
        self.dev_write_word(w, (cur & ~mask & M) | ((v << sh) & mask))

    def dev_read_word(self, w, side_effects):
        if (w & 0xFFFFFFC0) == 0xF0100000:
            o = w & 0x3C
            if o == 0x14:
                return self.irq_enable
            if o == 0x18:
                return self.irq_pending
            if o == 0x08:
                return (self.bank >> 20) << 24
            return 0
        if (w & 0xFF000000) == 0x84000000:
            return 1                        # Real3D ready; see src/bus.c
        if (w & 0xFFFFFFC0) == 0xF0040000:
            o = w & 0x3C
            if o == 0x00:
                return self.io_ctrl
            if o == 0x04:
                self.io_ready ^= 0x20000000
                return M ^ self.io_ready
            return M
        return 0

    def dev_write_word(self, w, v):
        if (w & 0xFFFFFFC0) == 0xF0100000:
            o = w & 0x3C
            if o == 0x14:
                self.irq_enable = v
            elif o == 0x08:
                self.bank = ((v >> 24) & 0xFF) << 20
            return
        if (w & 0xFFFFFFC0) == 0xF0040000:
            if (w & 0x3C) == 0x00:
                self.io_ctrl = v
            return
        if (w & 0xFFFF0000) == 0xF1180000 and (w & 0xFC) == 0x10:
            self.irq_pending &= ~v & M
            return

    # ---- condition register ----------------------------------------------
    def cr_set(self, fld, v):
        sh = 28 - 4 * fld
        self.cr = (self.cr & ~(0xF << sh)) | ((v & 0xF) << sh)

    def crbit(self, b):
        return (self.cr >> (31 - b)) & 1

    def crbit_set(self, b, v):
        m = 1 << (31 - b)
        self.cr = (self.cr | m) if v else (self.cr & ~m)

    def cmp_s(self, fld, a, b):
        a, b = s32(a), s32(b)
        v = 8 if a < b else 4 if a > b else 2
        if self.xer & 0x80000000:
            v |= 1
        self.cr_set(fld, v)

    def cmp_u(self, fld, a, b):
        a &= M
        b &= M
        v = 8 if a < b else 4 if a > b else 2
        if self.xer & 0x80000000:
            v |= 1
        self.cr_set(fld, v)

    def cr0(self, v):
        self.cmp_s(0, v, 0)

    def set_ca(self, c):
        self.xer = (self.xer | 0x20000000) if c else (self.xer & ~0x20000000)

    @property
    def ca(self):
        return 1 if self.xer & 0x20000000 else 0

    def fetch(self, pc):
        i = self.dcache.get(pc)
        if i is None:
            i = D.decode(self.read(pc, 4), pc)
            self.dcache[pc] = i
        return i


def mask(mb, me):
    a = M >> mb
    b = M if me >= 31 else (M << (31 - me)) & M
    return (a & b) if mb <= me else (a | b)


def rotl(v, n):
    n &= 31
    return ((v << n) | (v >> (32 - n))) & M if n else v & M


def _note(m, frm, to, kind):
    if not m.hist_max:
        return
    m.hist.append((frm, to, kind))
    if len(m.hist) > m.hist_max:
        m.hist.pop(0)


# VBlank start, as the game's own interrupt library tests it (upper 16 bits).
IRQ_VBLANK_START = 0x02000000


def maybe_interrupt(m):
    """Deliver the external interrupt the way the processor would.

    The main loop does not poll the interrupt controller -- it waits on a RAM
    flag that only the handler sets -- so nothing advances unless interrupts
    actually fire. See docs/technical/execution-model.md.
    """
    if not m.field_period or m.in_handler:
        return
    if m.icount < m.next_field:
        return
    m.next_field = m.icount + m.field_period
    m.fields += 1
    m.irq_pending |= IRQ_VBLANK_START
    if not (m.irq_pending & m.irq_enable):
        return
    if not (m.msr & 0x8000):          # MSR[EE] clear: interrupts disabled
        return
    m.spr[26] = m.pc                  # SRR0
    m.spr[27] = m.msr                 # SRR1
    m.msr &= ~0x8000                  # EE off while the handler runs
    m.in_handler = True
    m.irq_taken += 1
    m.pc = (0xFFF00000 if (m.msr & 0x40) else 0x00000000) + 0x500


def run(m, entry, max_insns, stop_in_ram, trace=0, trace_at=None):
    m.pc = entry
    traced = 0
    tracing = trace > 0
    armed = trace_at is None
    r = m.r

    while m.icount < max_insns:
        pc = m.pc
        if stop_in_ram and pc < RAM_SIZE and m.icount:
            return "entered RAM at %08X" % pc
        if trace_at is not None and pc == trace_at:
            armed = True
            tracing = True
        maybe_interrupt(m)
        if m.pc != pc:
            continue
        i = m.fetch(pc)
        if i.mn is None:
            return "undecodable %08X at %08X" % (i.raw, pc)
        if tracing and armed and traced < trace:
            print("  %08X  %08x  %-8s r3=%08X r4=%08X r1=%08X"
                  % (pc, i.raw, i.mn, r[3], r[4], r[1]))
            traced += 1
            if traced >= trace:
                tracing = False

        m.icount += 1
        nxt = (pc + 4) & M
        mn = i.mn
        rd, ra, rb = i.rd, i.ra, i.rb

        # ---- branches ----
        if mn == "b":
            if i.lk:
                m.lr = nxt
                m.calls[i.target] += 1
            _note(m, pc, i.target, "bl" if i.lk else "b")
            m.pc = i.target
            continue
        if mn == "bc":
            take = True
            if not (i.bo & 0x04):
                m.ctr = (m.ctr - 1) & M
                take = (m.ctr == 0) if (i.bo & 0x02) else (m.ctr != 0)
            if take and not (i.bo & 0x10):
                want = 1 if (i.bo & 0x08) else 0
                take = m.crbit(i.bi) == want
            if i.lk:
                m.lr = nxt
            m.pc = i.target if take else nxt
            continue
        if mn in ("bclr", "bcctr"):
            take = True
            if not (i.bo & 0x04):
                m.ctr = (m.ctr - 1) & M
                take = (m.ctr == 0) if (i.bo & 0x02) else (m.ctr != 0)
            if take and not (i.bo & 0x10):
                want = 1 if (i.bo & 0x08) else 0
                take = m.crbit(i.bi) == want
            tgt = (m.lr if mn == "bclr" else m.ctr) & ~3 & M
            if i.lk:
                m.lr = nxt
                m.calls[tgt] += 1
            if take:
                _note(m, pc, tgt, mn)
            m.pc = tgt if take else nxt
            continue
        if mn == "rfi":
            m.msr = m.spr.get(27, 0)
            m.pc = m.spr.get(26, 0) & ~3 & M
            m.in_handler = False
            continue

        # ---- everything else ----
        try:
            step(m, i, mn, rd, ra, rb)
        except Exception as e:                      # noqa: BLE001
            return "fault %s at %08X (%s)" % (type(e).__name__, pc, e)
        m.pc = nxt

    return "instruction budget exhausted"


def step(m, i, mn, rd, ra, rb):
    r = m.r
    simm, uimm = i.simm, i.uimm

    # --- arithmetic ---
    if mn == "addi":
        r[rd] = ((r[ra] if ra else 0) + simm) & M
    elif mn == "addis":
        r[rd] = ((r[ra] if ra else 0) + (uimm << 16)) & M
    elif mn == "add":
        r[rd] = (r[ra] + r[rb]) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "subf":
        r[rd] = (r[rb] - r[ra]) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "addic" or mn == "addic.":
        t = r[ra] + (simm & M)
        r[rd] = t & M
        m.set_ca(t >> 32)
        if mn == "addic.":
            m.cr0(r[rd])
    elif mn == "subfic":
        t = (~r[ra] & M) + (simm & M) + 1
        r[rd] = t & M
        m.set_ca(t >> 32)
    elif mn == "addc":
        t = r[ra] + r[rb]
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "adde":
        t = r[ra] + r[rb] + m.ca
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "addze":
        t = r[ra] + m.ca
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "addme":
        t = r[ra] + m.ca + M
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "subfc":
        t = (~r[ra] & M) + r[rb] + 1
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "subfe":
        t = (~r[ra] & M) + r[rb] + m.ca
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "subfze":
        t = (~r[ra] & M) + m.ca
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "subfme":
        t = (~r[ra] & M) + m.ca + M
        r[rd] = t & M
        m.set_ca(t >> 32)
        if i.rc:
            m.cr0(r[rd])
    elif mn == "neg":
        r[rd] = (-s32(r[ra])) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "mulli":
        r[rd] = (s32(r[ra]) * simm) & M
    elif mn == "mullw":
        r[rd] = (s32(r[ra]) * s32(r[rb])) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "mulhw":
        r[rd] = ((s32(r[ra]) * s32(r[rb])) >> 32) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "mulhwu":
        r[rd] = ((r[ra] * r[rb]) >> 32) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "divw":
        b = s32(r[rb])
        a = s32(r[ra])
        r[rd] = 0 if b == 0 else (abs(a) // abs(b) *
                                  (1 if (a < 0) == (b < 0) else -1)) & M
        if i.rc:
            m.cr0(r[rd])
    elif mn == "divwu":
        r[rd] = 0 if r[rb] == 0 else (r[ra] // r[rb]) & M
        if i.rc:
            m.cr0(r[rd])

    # --- compare ---
    elif mn == "cmpi":
        m.cmp_s(i.crfd, r[ra], simm)
    elif mn == "cmp":
        m.cmp_s(i.crfd, r[ra], r[rb])
    elif mn == "cmpli":
        m.cmp_u(i.crfd, r[ra], uimm)
    elif mn == "cmpl":
        m.cmp_u(i.crfd, r[ra], r[rb])

    # --- logical ---
    elif mn == "ori":
        r[ra] = r[rd] | uimm
    elif mn == "oris":
        r[ra] = r[rd] | (uimm << 16)
    elif mn == "xori":
        r[ra] = r[rd] ^ uimm
    elif mn == "xoris":
        r[ra] = r[rd] ^ (uimm << 16)
    elif mn == "andi.":
        r[ra] = r[rd] & uimm
        m.cr0(r[ra])
    elif mn == "andis.":
        r[ra] = r[rd] & (uimm << 16)
        m.cr0(r[ra])
    elif mn in ("and", "or", "xor", "nand", "nor", "eqv", "andc", "orc"):
        a, b = r[rd], r[rb]
        v = {"and": a & b, "or": a | b, "xor": a ^ b,
             "nand": ~(a & b), "nor": ~(a | b), "eqv": ~(a ^ b),
             "andc": a & ~b, "orc": a | ~b}[mn] & M
        r[ra] = v
        if i.rc:
            m.cr0(v)
    elif mn == "extsb":
        v = r[rd] & 0xFF
        r[ra] = (v - 0x100) & M if v & 0x80 else v
        if i.rc:
            m.cr0(r[ra])
    elif mn == "extsh":
        v = r[rd] & 0xFFFF
        r[ra] = (v - 0x10000) & M if v & 0x8000 else v
        if i.rc:
            m.cr0(r[ra])
    elif mn == "cntlzw":
        v = r[rd]
        n = 32 if v == 0 else 31 - v.bit_length() + 1
        r[ra] = n
        if i.rc:
            m.cr0(n)

    # --- rotate / shift ---
    elif mn == "rlwinm":
        r[ra] = rotl(r[rd], i.sh) & mask(i.mb, i.me)
        if i.rc:
            m.cr0(r[ra])
    elif mn == "rlwnm":
        r[ra] = rotl(r[rd], r[rb] & 31) & mask(i.mb, i.me)
        if i.rc:
            m.cr0(r[ra])
    elif mn == "rlwimi":
        mk = mask(i.mb, i.me)
        r[ra] = (rotl(r[rd], i.sh) & mk) | (r[ra] & ~mk & M)
        if i.rc:
            m.cr0(r[ra])
    elif mn == "slw":
        n = r[rb]
        r[ra] = 0 if n & 0x20 else (r[rd] << (n & 31)) & M
        if i.rc:
            m.cr0(r[ra])
    elif mn == "srw":
        n = r[rb]
        r[ra] = 0 if n & 0x20 else (r[rd] >> (n & 31))
        if i.rc:
            m.cr0(r[ra])
    elif mn == "srawi":
        v = s32(r[rd])
        m.set_ca(1 if (v < 0 and (r[rd] & ((1 << i.sh) - 1))) else 0)
        r[ra] = (v >> i.sh) & M
        if i.rc:
            m.cr0(r[ra])
    elif mn == "sraw":
        v = s32(r[rd])
        n = r[rb] & 0x3F
        if n > 31:
            m.set_ca(1 if v < 0 else 0)
            r[ra] = (v >> 31) & M
        else:
            m.set_ca(1 if (v < 0 and (r[rd] & ((1 << n) - 1))) else 0)
            r[ra] = (v >> n) & M
        if i.rc:
            m.cr0(r[ra])

    # --- loads / stores ---
    elif mn in ("lbz", "lhz", "lwz", "lha", "lbzu", "lhzu", "lwzu", "lhau"):
        base = r[ra] if (ra or mn.endswith("u")) else 0
        a = (base + simm) & M
        n = {"lbz": 1, "lbzu": 1, "lhz": 2, "lhzu": 2,
             "lha": 2, "lhau": 2, "lwz": 4, "lwzu": 4}[mn]
        v = m.read(a, n)
        if mn in ("lha", "lhau") and v & 0x8000:
            v = (v - 0x10000) & M
        r[rd] = v
        if mn.endswith("u"):
            r[ra] = a
    elif mn in ("stb", "sth", "stw", "stbu", "sthu", "stwu"):
        base = r[ra] if (ra or mn.endswith("u")) else 0
        a = (base + simm) & M
        n = {"stb": 1, "stbu": 1, "sth": 2, "sthu": 2, "stw": 4, "stwu": 4}[mn]
        m.write(a, n, r[rd])
        if mn.endswith("u"):
            r[ra] = a
    elif mn in ("lbzx", "lhzx", "lwzx", "lhax",
                "lbzux", "lhzux", "lwzux", "lhaux"):
        a = ((r[ra] if (ra or mn.endswith("ux")) else 0) + r[rb]) & M
        n = 1 if mn.startswith("lbz") else (4 if mn.startswith("lwz") else 2)
        v = m.read(a, n)
        if mn.startswith("lha") and v & 0x8000:
            v = (v - 0x10000) & M
        r[rd] = v
        if mn.endswith("ux"):
            r[ra] = a
    elif mn in ("stbx", "sthx", "stwx", "stbux", "sthux", "stwux"):
        a = ((r[ra] if (ra or mn.endswith("ux")) else 0) + r[rb]) & M
        n = 1 if mn.startswith("stb") else (4 if mn.startswith("stw") else 2)
        m.write(a, n, r[rd])
        if mn.endswith("ux"):
            r[ra] = a
    elif mn == "lmw":
        a = ((r[ra] if ra else 0) + simm) & M
        for k in range(rd, 32):
            r[k] = m.read((a + (k - rd) * 4) & M, 4)
    elif mn == "stmw":
        a = ((r[ra] if ra else 0) + simm) & M
        for k in range(rd, 32):
            m.write((a + (k - rd) * 4) & M, 4, r[k])
    elif mn == "lwbrx":
        a = ((r[ra] if ra else 0) + r[rb]) & M
        v = m.read(a, 4)
        r[rd] = int.from_bytes(v.to_bytes(4, "big"), "little")
    elif mn == "stwbrx":
        a = ((r[ra] if ra else 0) + r[rb]) & M
        m.write(a, 4, int.from_bytes(r[rd].to_bytes(4, "big"), "little"))
    elif mn == "lwarx":
        r[rd] = m.read(((r[ra] if ra else 0) + r[rb]) & M, 4)
    elif mn == "stwcx.":
        m.write(((r[ra] if ra else 0) + r[rb]) & M, 4, r[rd])
        m.cr_set(0, 2)
    elif mn == "dcbz":
        a = (((r[ra] if ra else 0) + r[rb]) & M) & ~31
        for k in range(0, 32, 4):
            m.write((a + k) & M, 4, 0)

    # --- floating point (stored as raw bits; the boot path only moves them) ---
    elif mn in ("lfs", "lfd", "stfs", "stfd", "lfsx", "lfdx", "stfsx", "stfdx"):
        idx = "x" in mn
        a = (((r[ra] if ra else 0) + (r[rb] if idx else simm))) & M
        n = 8 if "fd" in mn else 4
        if mn.startswith("l"):
            m.f[rd] = m.read(a, n)
        else:
            m.write(a, n, int(m.f[rd]) if isinstance(m.f[rd], int) else 0)
    elif mn == "fmr":
        m.f[rd] = m.f[rb]

    # --- CR ---
    elif mn == "mfcr":
        r[rd] = m.cr
    elif mn == "mtcrf":
        crm = (i.raw >> 12) & 0xFF
        mk = 0
        for b in range(8):
            if crm & (0x80 >> b):
                mk |= 0xF << (28 - 4 * b)
        m.cr = (m.cr & ~mk & M) | (r[rd] & mk)
    elif mn == "mcrf":
        m.cr_set(i.crfd, (m.cr >> (28 - 4 * i.crfs)) & 0xF)
    elif mn in ("crand", "cror", "crxor", "crnand", "crnor",
                "creqv", "crandc", "crorc"):
        a, b = m.crbit(ra), m.crbit(rb)
        v = {"crand": a & b, "cror": a | b, "crxor": a ^ b,
             "crnand": 1 - (a & b), "crnor": 1 - (a | b),
             "creqv": int(a == b), "crandc": a & (1 - b),
             "crorc": a | (1 - b)}[mn]
        m.crbit_set(rd, v)

    # --- system ---
    elif mn == "mfspr":
        r[rd] = {1: m.xer, 8: m.lr, 9: m.ctr}.get(i.spr, m.spr.get(i.spr, 0))
    elif mn == "mtspr":
        if i.spr == 1:
            m.xer = r[rd]
        elif i.spr == 8:
            m.lr = r[rd]
        elif i.spr == 9:
            m.ctr = r[rd]
        else:
            m.spr[i.spr] = r[rd]
    elif mn == "mfmsr":
        r[rd] = m.msr
    elif mn == "mtmsr":
        m.msr = r[rd]
    elif mn == "mfsr":
        r[rd] = m.sr[(i.raw >> 16) & 0xF]
    elif mn == "mtsr":
        m.sr[(i.raw >> 16) & 0xF] = r[rd]
    elif mn == "mfsrin":
        r[rd] = m.sr[(r[rb] >> 28) & 0xF]
    elif mn == "mtsrin":
        m.sr[(r[rb] >> 28) & 0xF] = r[rd]
    elif mn == "mftb":
        r[rd] = m.icount & M
    elif mn in ("sync", "isync", "eieio", "icbi", "dcbi", "dcbf", "dcbst",
                "dcbt", "dcbtst", "tlbie", "tlbsync", "tlbld", "tlbli",
                "tw", "twi", "sc", "mtfsf", "mtfsb0", "mtfsb1", "mffs",
                "mtfsfi", "mcrfs", "stfiwx"):
        pass
    else:
        raise NotImplementedError(mn)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--base", default="0xFFE00000")
    ap.add_argument("--crom-bank")
    ap.add_argument("--entry", default="0xFFF00100")
    ap.add_argument("--max", type=lambda s: int(s, 0), default=200000000)
    ap.add_argument("--snapshot")
    ap.add_argument("--trace", type=int, default=0)
    ap.add_argument("--trace-at", default=None)
    ap.add_argument("--watch", action="append", default=[])
    ap.add_argument("--no-stop-in-ram", action="store_true")
    ap.add_argument("--field-period", type=lambda s: int(s, 0), default=0,
                    help="instructions per field; enables VBlank interrupts")
    ap.add_argument("--trace-io", help="record device accesses (diff against "
                                       "the runtime's M3_TRACE_IO log)")
    ap.add_argument("--trace-io-max", type=lambda s: int(s, 0), default=20000)
    ap.add_argument("--dump-vram", help="write tilegen VRAM to this file")
    ap.add_argument("--dump-cull", help="write Real3D culling RAM (hi) here")
    ap.add_argument("--scsi-trace", action="store_true",
                    help="log 53C810 register traffic")
    ap.add_argument("--history", type=int, default=0,
                    help="record the last N taken branches and print them")
    a = ap.parse_args()

    crom = open(a.image, "rb").read()
    bank = open(a.crom_bank, "rb").read() if a.crom_bank else None
    m = Machine(crom, int(a.base, 0), bank)
    m.watch = {int(x, 0) for x in a.watch}
    m.hist_max = a.history
    if a.trace_io:
        m.io_log_f = open(a.trace_io, "w")
        m.io_budget = a.trace_io_max
    m.scsi_trace = a.scsi_trace
    m.field_period = a.field_period
    m.next_field = a.field_period

    why = run(m, int(a.entry, 0), a.max, not a.no_stop_in_ram,
              a.trace, int(a.trace_at, 0) if a.trace_at else None)

    print("stopped         : %s" % why)
    print("instructions    : %d" % m.icount)
    print("pc              : %08X" % m.pc)
    print("fields          : %d" % m.fields)
    print("interrupts taken: %d" % m.irq_taken)
    print("ram writes      : %d" % m.ram_writes)
    for nm, buf in (("tilegen VRAM", m.vram), ("culling RAM lo", m.cull_lo),
                    ("culling RAM hi", m.cull_hi), ("polygon RAM", m.poly)):
        nz = sum(1 for k in range(0, len(buf), 4) if buf[k:k + 4] != bytes(4))
        print("%-16s: %d non-zero words of %d" % (nm, nz, len(buf) // 4))
    if m.ram_writes:
        print("ram touched     : %08X..%08X" % (m.ram_lo, m.ram_hi))
    print("distinct hw reads : %d" % len(m.hw_reads))
    for addr, n in m.hw_reads.most_common(12):
        print("    R %08X x%d" % (addr, n))
    print("distinct hw writes: %d" % len(m.hw_writes))
    for addr, n in m.hw_writes.most_common(12):
        print("    W %08X x%d" % (addr, n))
    if m.calls:
        print("most-called routines (target, times):")
        for a, c in m.calls.most_common(24):
            print("    %08X  x%d" % (a, c))
    if m.scsi_moves:
        print("SCRIPTS memory moves: %d" % len(m.scsi_moves))
        for src, dst, cnt in m.scsi_moves[:12]:
            print("    %08X -> %08X  %d bytes" % (src, dst, cnt))
    if m.scsi_log:
        print("53C810 traffic: %d accesses" % len(m.scsi_log))
        from collections import Counter as _C
        per = _C((rw, o) for rw, _, o, _, _ in m.scsi_log)
        for (rw, o), c in per.most_common(14):
            print("    %s reg 0x%02X  x%d" % (rw, o, c))
        print("  writes to DSP (0x2C) -- each starts a SCRIPTS program:")
        n = 0
        for rw, pc, o, sz, v in m.scsi_log:
            if rw == "W" and o == 0x2C and v:
                print("    pc %08X  DSP = %08X" % (pc, v))
                n += 1
                if n >= 12:
                    break
        if not n:
            print("    (none)")
        print("  last 12 accesses:")
        for rw, pc, o, sz, v in m.scsi_log[-12:]:
            print("    %s reg 0x%02X size %d = %08X   (pc %08X)" % (rw, o, sz, v, pc))
    if m.hist:
        print("last taken branches:")
        for frm, to, kind in m.hist:
            print("    %08X  %-5s -> %08X" % (frm, kind, to))
    if m.watch_hits:
        print("watch hits      :")
        for pc, addr, v in m.watch_hits[:20]:
            print("    %08X wrote %08X = %08X" % (pc, addr, v))

    if a.dump_vram:
        open(a.dump_vram, "wb").write(m.vram)
        print("vram dump       : %s (%d bytes)" % (a.dump_vram, len(m.vram)))
    if a.dump_cull:
        open(a.dump_cull, "wb").write(m.cull_hi)
        print("cull dump       : %s (%d bytes)" % (a.dump_cull, len(m.cull_hi)))
    if a.snapshot:
        with open(a.snapshot, "wb") as f:
            f.write(m.ram)
        print("snapshot        : %s (%d bytes)" % (a.snapshot, len(m.ram)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
