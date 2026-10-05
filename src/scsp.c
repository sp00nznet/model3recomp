/* model3recomp -- Yamaha YMF292-F SCSP. See scsp.h.
 *
 * Ported to C from MAME's src/devices/sound/scsp.cpp and scspdsp.cpp, which
 * carry this licence:
 *
 *   license:BSD-3-Clause
 *   copyright-holders:ElSemi, R. Belmont   (thanks-to: kingshriek)
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions are
 *   met: (1) Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer. (2)
 *   Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 *   (3) Neither the name of the copyright holder nor the names of its
 *   contributors may be used to endorse or promote products derived from
 *   this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 *   IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 *   TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 *   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 *   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 *   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * What changed in the port, and nothing else did: timers count output
 * samples instead of scheduler time; MIDI in and out are byte queues, not
 * bit-level serial; the noise source is a seeded generator; the output gain
 * (MVOL) is applied here; the tables are built once and shared.
 */
#include "scsp.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define SHIFT       12
#define LFO_SHIFT   8
#define FIX(v)      ((uint32_t)((float)(1 << SHIFT) * (v)))
#define EG_SHIFT    16

enum { ATTACK, DECAY1, DECAY2, RELEASE };

typedef struct {
    int volume, state, step;
    int AR, D1R, D2R, RR, DL;
    uint8_t EGHOLD, LPLINK;
} eg_t;

typedef struct {
    uint16_t phase;
    uint32_t phase_step;
    const int *table, *scale;
} lfo_t;

typedef struct {
    uint16_t data[0x10];
    uint8_t  Backwards, active;
    uint32_t cur_addr, nxt_addr, step;
    eg_t     EG;
    lfo_t    PLFO, ALFO;
    int      slot;
    int16_t  Prev;
} slot_t;

typedef struct {
    uint32_t RBP, RBL;
    int16_t  COEF[64];
    uint16_t MADRS[32];
    uint16_t MPRO[128 * 4];
    int32_t  TEMP[128];
    int32_t  MEMS[32];
    uint32_t DEC;
    int32_t  MIXS[16];
    int16_t  EXTS[2];
    int16_t  EFREG[16];
    int      Stopped, LastStep;
} dsp_t;

struct scsp {
    /* ---- state: everything from here to `tail` is saved ---- */
    uint16_t data[0x30 / 2];
    slot_t   Slots[32];
    int16_t  RINGBUF[128];
    uint8_t  BUFPTR;
    uint32_t IrqTimA, IrqTimBC, IrqMidi, IrqCPU, IrqDMA;
    uint8_t  latched_MSLC;
    uint16_t latched_MSLC_data;
    uint8_t  MidiStack[32];
    uint8_t  MidiW, MidiR;
    int      TimPris[3];
    int      TimCnt[3];
    uint32_t timer_left[3];     /* samples until each timer fires, 0 = off */
    struct { uint32_t dmea; uint16_t drga, dtlg; uint8_t dgate, ddir; } dma;
    uint16_t mcieb, mcipd;
    dsp_t    DSP;
    uint32_t noise;
    int      tail;
    /* ---- wiring, not state ---- */
    uint8_t *ram;
    size_t   ram_size;
    void   (*irq)(int level, int state);
    void   (*midi_out)(uint8_t byte);
    int16_t *RBUFDST;
};

/* Slot parameters */
#define KEYONEX(s)  ((s->data[0x0] >> 0x0) & 0x1000)
#define KEYONB(s)   ((s->data[0x0] >> 0x0) & 0x0800)
#define SBCTL(s)    ((s->data[0x0] >> 0x9) & 0x0003)
#define SSCTL(s)    ((s->data[0x0] >> 0x7) & 0x0003)
#define LPCTL(s)    ((s->data[0x0] >> 0x5) & 0x0003)
#define PCM8B(s)    ((s->data[0x0] >> 0x0) & 0x0010)
#define SA(s)       (((s->data[0x0] & 0xF) << 16) | (s->data[0x1]))
#define LSA(s)      (s->data[0x2])
#define LEA(s)      (s->data[0x3])
#define D2R(s)      ((s->data[0x4] >> 0xB) & 0x001F)
#define D1R(s)      ((s->data[0x4] >> 0x6) & 0x001F)
#define EGHOLD(s)   ((s->data[0x4] >> 0x0) & 0x0020)
#define AR(s)       ((s->data[0x4] >> 0x0) & 0x001F)
#define LPSLNK(s)   ((s->data[0x5] >> 0x0) & 0x4000)
#define KRS(s)      ((s->data[0x5] >> 0xA) & 0x000F)
#define DL(s)       ((s->data[0x5] >> 0x5) & 0x001F)
#define RR(s)       ((s->data[0x5] >> 0x0) & 0x001F)
#define STWINH(s)   ((s->data[0x6] >> 0x0) & 0x0200)
#define SDIR(s)     ((s->data[0x6] >> 0x0) & 0x0100)
#define TL(s)       ((s->data[0x6] >> 0x0) & 0x00FF)
#define MDL(s)      ((s->data[0x7] >> 0xC) & 0x000F)
#define MDXSL(s)    ((s->data[0x7] >> 0x6) & 0x003F)
#define MDYSL(s)    ((s->data[0x7] >> 0x0) & 0x003F)
#define OCT(s)      ((s->data[0x8] >> 0xB) & 0x000F)
#define FNS(s)      ((s->data[0x8] >> 0x0) & 0x03FF)
#define LFOF(s)     ((s->data[0x9] >> 0xA) & 0x001F)
#define PLFOWS(s)   ((s->data[0x9] >> 0x8) & 0x0003)
#define PLFOS(s)    ((s->data[0x9] >> 0x5) & 0x0007)
#define ALFOWS(s)   ((s->data[0x9] >> 0x3) & 0x0003)
#define ALFOS(s)    ((s->data[0x9] >> 0x0) & 0x0007)
#define ISEL(s)     ((s->data[0xA] >> 0x3) & 0x000F)
#define IMXL(s)     ((s->data[0xA] >> 0x0) & 0x0007)
#define DISDL(s)    ((s->data[0xB] >> 0xD) & 0x0007)
#define DIPAN(s)    ((s->data[0xB] >> 0x8) & 0x001F)
#define EFSDL(s)    ((s->data[0xB] >> 0x5) & 0x0007)
#define EFPAN(s)    ((s->data[0xB] >> 0x0) & 0x001F)

/* Common registers */
#define MVOL(c)     ((c->data[0] >> 0x0) & 0x000F)
#define RBL(c)      ((c->data[1] >> 0x7) & 0x0003)
#define RBP(c)      ((c->data[1] >> 0x0) & 0x003F)
#define SCILV0(c)   ((c->data[0x24 / 2] >> 0x0) & 0xff)
#define SCILV1(c)   ((c->data[0x26 / 2] >> 0x0) & 0xff)
#define SCILV2(c)   ((c->data[0x28 / 2] >> 0x0) & 0xff)

#define SCIMID  3
#define SCIDMA  4
#define SCIIRQ  5
#define SCITMA  6
#define SCITMB  7

/* ---- shared tables ---------------------------------------------------- */
static const double ARTimes[64] = {100000,100000,8100.0,6900.0,6000.0,4800.0,4000.0,3400.0,3000.0,2400.0,2000.0,1700.0,1500.0,
    1200.0,1000.0,860.0,760.0,600.0,500.0,430.0,380.0,300.0,250.0,220.0,190.0,150.0,130.0,110.0,95.0,
    76.0,63.0,55.0,47.0,38.0,31.0,27.0,24.0,19.0,15.0,13.0,12.0,9.4,7.9,6.8,6.0,4.7,3.8,3.4,3.0,2.4,
    2.0,1.8,1.6,1.3,1.1,0.93,0.85,0.65,0.53,0.44,0.40,0.35,0.0,0.0};
static const double DRTimes[64] = {100000,100000,118200.0,101300.0,88600.0,70900.0,59100.0,50700.0,44300.0,35500.0,29600.0,25300.0,22200.0,17700.0,
    14800.0,12700.0,11100.0,8900.0,7400.0,6300.0,5500.0,4400.0,3700.0,3200.0,2800.0,2200.0,1800.0,1600.0,1400.0,1100.0,
    920.0,790.0,690.0,550.0,460.0,390.0,340.0,270.0,230.0,200.0,170.0,140.0,110.0,98.0,85.0,68.0,57.0,49.0,43.0,34.0,
    28.0,25.0,22.0,18.0,14.0,12.0,11.0,8.5,7.1,6.1,5.4,4.3,3.6,3.1};
static const float SDLT[8] = {-1000000.0f,-36.0f,-30.0f,-24.0f,-18.0f,-12.0f,-6.0f,0.0f};
static const float LFOFreq[32] = {
    0.17f,0.19f,0.23f,0.27f,0.34f,0.39f,0.45f,0.55f,0.68f,0.78f,0.92f,1.10f,1.39f,1.60f,1.87f,2.27f,
    2.87f,3.31f,3.92f,4.79f,6.15f,7.18f,8.60f,10.8f,14.4f,17.2f,21.5f,28.7f,43.1f,57.4f,86.1f,172.3f};
static const float ASCALE[8] = {0.0f,0.4f,0.8f,1.5f,3.0f,6.0f,12.0f,24.0f};
static const float PSCALE[8] = {0.0f,7.0f,13.5f,27.0f,55.0f,112.0f,230.0f,494.0f};

static int32_t EG_TABLE[0x400];
static int LPANTABLE[0x10000], RPANTABLE[0x10000];
static int ARTABLE[64], DRTABLE[64];
static int PLFO_TRI[256], PLFO_SQR[256], PLFO_SAW[256], PLFO_NOI[256];
static int ALFO_TRI[256], ALFO_SQR[256], ALFO_SAW[256], ALFO_NOI[256];
static int PSCALES[8][256], ASCALES[8][256];
static int g_tables;

#define LFIX(v)  ((uint32_t)((float)(1 << LFO_SHIFT) * (v)))
#define DB(v)    LFIX(powf(10.0f, (v) / 20.0f))
#define CENTS(v) LFIX(powf(2.0f, (v) / 1200.0f))

static void build_tables(void)
{
    int i, s;
    uint32_t seed = 12345u;
    if (g_tables) return;
    g_tables = 1;

    for (i = 0; i < 0x400; ++i) {
        float envDB = ((float)(3 * (i - 0x3ff))) / 32.0f;
        EG_TABLE[i] = (int32_t)(powf(10.0f, envDB / 20.0f) * (float)(1 << SHIFT));
    }
    for (i = 0; i < 0x10000; ++i) {
        int iTL = i & 0xff, iPAN = (i >> 8) & 0x1f, iSDL = (i >> 0xD) & 7;
        float SegaDB = 0.0f, TLv, fSDL, PAN, LPAN, RPAN;
        if (iTL & 0x01) SegaDB -= 0.4f;
        if (iTL & 0x02) SegaDB -= 0.8f;
        if (iTL & 0x04) SegaDB -= 1.5f;
        if (iTL & 0x08) SegaDB -= 3.0f;
        if (iTL & 0x10) SegaDB -= 6.0f;
        if (iTL & 0x20) SegaDB -= 12.0f;
        if (iTL & 0x40) SegaDB -= 24.0f;
        if (iTL & 0x80) SegaDB -= 48.0f;
        TLv = powf(10.0f, SegaDB / 20.0f);
        SegaDB = 0;
        if (iPAN & 0x1) SegaDB -= 3.0f;
        if (iPAN & 0x2) SegaDB -= 6.0f;
        if (iPAN & 0x4) SegaDB -= 12.0f;
        if (iPAN & 0x8) SegaDB -= 24.0f;
        PAN = ((iPAN & 0xf) == 0xf) ? 0.0f : powf(10.0f, SegaDB / 20.0f);
        if (iPAN < 0x10) { LPAN = PAN; RPAN = 1.0f; }
        else             { RPAN = PAN; LPAN = 1.0f; }
        fSDL = iSDL ? powf(10.0f, SDLT[iSDL] / 20.0f) : 0.0f;
        LPANTABLE[i] = (int)FIX(4.0f * LPAN * TLv * fSDL);
        RPANTABLE[i] = (int)FIX(4.0f * RPAN * TLv * fSDL);
    }
    ARTABLE[0] = DRTABLE[0] = 0;
    ARTABLE[1] = DRTABLE[1] = 0;
    for (i = 2; i < 64; ++i) {
        double t = ARTimes[i], scale = (double)(1 << EG_SHIFT);
        if (t != 0.0) ARTABLE[i] = (int)(((1023 * 1000.0) / (44100.0 * t)) * scale);
        else          ARTABLE[i] = 1024 << EG_SHIFT;
        t = DRTimes[i];
        DRTABLE[i] = (int)(((1023 * 1000.0) / (44100.0 * t)) * scale);
    }
    for (i = 0; i < 256; ++i) {
        int a, p;
        a = 255 - i; p = i < 128 ? i : i - 256;
        ALFO_SAW[i] = a; PLFO_SAW[i] = p;
        if (i < 128) { a = 255; p = 127; } else { a = 0; p = -128; }
        ALFO_SQR[i] = a; PLFO_SQR[i] = p;
        a = i < 128 ? 255 - i * 2 : i * 2 - 256;
        if (i < 64) p = i * 2;
        else if (i < 128) p = 255 - i * 2;
        else if (i < 192) p = 256 - i * 2;
        else p = i * 2 - 511;
        ALFO_TRI[i] = a; PLFO_TRI[i] = p;
        seed = seed * 1103515245u + 12345u;
        a = (int)((seed >> 16) & 0xff);
        ALFO_NOI[i] = a; PLFO_NOI[i] = 128 - a;
    }
    for (s = 0; s < 8; ++s) {
        float limit = PSCALE[s];
        for (i = -128; i < 128; ++i) PSCALES[s][i + 128] = (int)CENTS((limit * (float)i) / 128.0f);
        limit = -ASCALE[s];
        for (i = 0; i < 256; ++i) ASCALES[s][i] = (int)DB((limit * (float)i) / 256.0f);
    }
}

/* ---- memory ----------------------------------------------------------- */
static uint8_t ram_r8(scsp_t *c, uint32_t a)
{
    a &= 0xFFFFF;
    return a < c->ram_size ? c->ram[a] : 0;
}
static uint16_t ram_r16(scsp_t *c, uint32_t a)
{
    a &= 0xFFFFE;
    return a + 1 < c->ram_size ? (uint16_t)((c->ram[a] << 8) | c->ram[a + 1]) : 0;
}
static void ram_w16(scsp_t *c, uint32_t a, uint16_t v)
{
    a &= 0xFFFFE;
    if (a + 1 < c->ram_size) { c->ram[a] = (uint8_t)(v >> 8); c->ram[a + 1] = (uint8_t)v; }
}

/* ---- interrupts ------------------------------------------------------- */
static void irq_line(scsp_t *c, uint32_t level, int state)
{
    if (c->irq) c->irq((int)level, state);
}

static uint8_t DecodeSCI(scsp_t *c, uint8_t irq)
{
    uint8_t SCI = 0;
    SCI |= (SCILV0(c) & (1 << irq)) ? 1 : 0;
    SCI |= ((SCILV1(c) & (1 << irq)) ? 1 : 0) << 1;
    SCI |= ((SCILV2(c) & (1 << irq)) ? 1 : 0) << 2;
    return SCI;
}

static void CheckPendingIRQ(scsp_t *c)
{
    uint32_t pend = c->data[0x20 / 2], en = c->data[0x1e / 2];
    if (c->MidiW != c->MidiR) { c->data[0x20 / 2] |= 8; pend |= 8; }
    if (!pend) return;
    if (pend & en & 0x20) { irq_line(c, c->IrqCPU, 1); return; }
    if ((pend & 0x40) && (en & 0x40)) { irq_line(c, c->IrqTimA, 1); return; }
    if ((pend & 0x80) && (en & 0x80)) { irq_line(c, c->IrqTimBC, 1); return; }
    if ((pend & 0x100) && (en & 0x100)) { irq_line(c, c->IrqTimBC, 1); return; }
    if ((pend & 8) && (en & 8)) { irq_line(c, c->IrqMidi, 1); return; }
    irq_line(c, 0, 0);
}

static void MainCheckPendingIRQ(scsp_t *c, uint16_t irq_type)
{
    c->mcipd |= irq_type;   /* the main CPU's line: not wired on Model 3 */
}

static void ResetInterrupts(scsp_t *c)
{
    uint32_t reset = c->data[0x22 / 2];
    if (reset & 0x40) irq_line(c, c->IrqTimA, 0);
    if (reset & 0x180) irq_line(c, c->IrqTimBC, 0);
    if (reset & 0x20) { c->data[0x20 / 2] &= ~0x20; irq_line(c, c->IrqCPU, 0); }
    if (reset & 0x8) irq_line(c, c->IrqMidi, 0);
    CheckPendingIRQ(c);
}

static void timer_fire(scsp_t *c, int t)
{
    static const uint16_t bit[3] = { 0x40, 0x80, 0x100 };
    static const int reg[3] = { 0x18 / 2, 0x1a / 2, 0x1c / 2 };
    c->TimCnt[t] = 0xFFFF;
    c->data[0x20 / 2] |= bit[t];
    c->data[reg[t]] &= 0xff00;
    c->data[reg[t]] |= c->TimCnt[t] >> 8;
    CheckPendingIRQ(c);
    if (t == 0) MainCheckPendingIRQ(c, 0x40);
}

/* ---- envelope, pitch, LFO --------------------------------------------- */
static int clamp63(int v) { return v < 0 ? 0 : v > 63 ? 63 : v; }
static int Get_AR(int base, int R) { return ARTABLE[clamp63(base + (R << 1))]; }
static int Get_DR(int base, int R) { return DRTABLE[clamp63(base + (R << 1))]; }

static void StopSlot(slot_t *slot, int keyoff)
{
    if (keyoff) slot->EG.state = RELEASE;
    else        slot->active = 0;
    slot->data[0] &= ~0x800;
}

static void Compute_EG(slot_t *slot)
{
    int octave = (OCT(slot) ^ 8) - 8;
    int rate = KRS(slot) != 0xf ? octave + 2 * KRS(slot) + ((FNS(slot) >> 9) & 1) : 0;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->EG.AR = Get_AR(rate, AR(slot));
    slot->EG.D1R = Get_DR(rate, D1R(slot));
    slot->EG.D2R = Get_DR(rate, D2R(slot));
    slot->EG.RR = Get_DR(rate, RR(slot));
    slot->EG.DL = 0x1f - DL(slot);
    slot->EG.EGHOLD = (uint8_t)EGHOLD(slot);
}

static int EG_Update(slot_t *slot)
{
    switch (slot->EG.state) {
    case ATTACK:
        slot->EG.volume += slot->EG.AR;
        if (slot->EG.volume >= (0x3ff << EG_SHIFT)) {
            if (!LPSLNK(slot)) {
                slot->EG.state = DECAY1;
                if (slot->EG.D1R >= (1024 << EG_SHIFT)) slot->EG.state = DECAY2;
            }
            slot->EG.volume = 0x3ff << EG_SHIFT;
        }
        if (slot->EG.EGHOLD) return 0x3ff << (SHIFT - 10);
        break;
    case DECAY1:
        slot->EG.volume -= slot->EG.D1R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        if (slot->EG.volume >> (EG_SHIFT + 5) <= slot->EG.DL) slot->EG.state = DECAY2;
        break;
    case DECAY2:
        if (D2R(slot) == 0) return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
        slot->EG.volume -= slot->EG.D2R;
        if (slot->EG.volume <= 0) slot->EG.volume = 0;
        break;
    case RELEASE:
        slot->EG.volume -= slot->EG.RR;
        if (slot->EG.volume <= 0) { slot->EG.volume = 0; StopSlot(slot, 0); }
        break;
    default:
        return 1 << SHIFT;
    }
    return (slot->EG.volume >> EG_SHIFT) << (SHIFT - 10);
}

static uint32_t Step(slot_t *slot)
{
    int octave = (OCT(slot) ^ 8) - 8 + SHIFT - 10;
    uint32_t Fn = FNS(slot) + (1 << 10);
    return octave >= 0 ? Fn << octave : Fn >> -octave;
}

static void LFO_ComputeStep(lfo_t *L, uint32_t lfof, uint32_t ws, uint32_t s, int alfo)
{
    float step = LFOFreq[lfof] * 256.0f / 44100.0f;
    L->phase_step = (uint32_t)((float)(1 << LFO_SHIFT) * step);
    if (alfo) {
        L->table = ws == 0 ? ALFO_SAW : ws == 1 ? ALFO_SQR : ws == 2 ? ALFO_TRI : ALFO_NOI;
        L->scale = ASCALES[s];
    } else {
        L->table = ws == 0 ? PLFO_SAW : ws == 1 ? PLFO_SQR : ws == 2 ? PLFO_TRI : PLFO_NOI;
        L->scale = PSCALES[s];
    }
}

static void Compute_LFO(slot_t *slot)
{
    if (PLFOS(slot) != 0) LFO_ComputeStep(&slot->PLFO, LFOF(slot), PLFOWS(slot), PLFOS(slot), 0);
    if (ALFOS(slot) != 0) LFO_ComputeStep(&slot->ALFO, LFOF(slot), ALFOWS(slot), ALFOS(slot), 1);
}

static int32_t PLFO_Step(lfo_t *L)
{
    int p;
    L->phase = (uint16_t)(L->phase + L->phase_step);
    p = L->table[L->phase >> LFO_SHIFT];
    p = L->scale[p + 128];
    return p << (SHIFT - LFO_SHIFT);
}

static int32_t ALFO_Step(lfo_t *L)
{
    int p;
    L->phase = (uint16_t)(L->phase + L->phase_step);
    p = L->table[L->phase >> LFO_SHIFT];
    p = L->scale[p];
    return p << (SHIFT - LFO_SHIFT);
}

static void StartSlot(slot_t *slot)
{
    slot->active = 1;
    slot->cur_addr = 0;
    slot->nxt_addr = 1 << SHIFT;
    slot->step = Step(slot);
    Compute_EG(slot);
    slot->EG.state = ATTACK;
    slot->EG.volume = 0x17F << EG_SHIFT;
    slot->Prev = 0;
    slot->Backwards = 0;
    Compute_LFO(slot);
}

/* ---- the DSP ---------------------------------------------------------- */
static int32_t sext(int32_t v, int bits) { return (int32_t)((uint32_t)v << (32 - bits)) >> (32 - bits); }
static int32_t clamp24(int32_t v) { return v < -0x00800000 ? -0x00800000 : v > 0x007fffff ? 0x007fffff : v; }

static uint16_t PACK(int32_t val)
{
    int sign = (val >> 23) & 1, exponent = 0, k;
    uint32_t temp = (uint32_t)(val ^ (val << 1)) & 0xFFFFFF;
    for (k = 0; k < 12; k++) {
        if (temp & 0x800000) break;
        temp <<= 1;
        exponent += 1;
    }
    if (exponent < 12) val = (val << exponent) & 0x3FFFFF;
    else               val <<= 11;
    val >>= 11;
    val &= 0x7FF;
    val |= sign << 15;
    val |= exponent << 11;
    return (uint16_t)val;
}

static int32_t UNPACK(uint16_t val)
{
    int sign = (val >> 15) & 1, exponent = (val >> 11) & 0xF, mantissa = val & 0x7FF;
    int32_t uval = mantissa << 11;
    if (exponent > 11) { exponent = 11; uval |= sign << 22; }
    else               uval |= (sign ^ 1) << 22;
    uval |= sign << 23;
    uval = sext(uval, 24);
    uval >>= exponent;
    return uval;
}

static void dsp_start(dsp_t *d)
{
    int i;
    d->Stopped = 0;
    for (i = 127; i >= 0; --i) {
        const uint16_t *I = d->MPRO + i * 4;
        if (I[0] || I[1] || I[2] || I[3]) break;
    }
    d->LastStep = i + 1;
}

static void dsp_step(scsp_t *c)
{
    dsp_t *d = &c->DSP;
    int32_t ACC = 0, MEMVAL = 0, FRC_REG = 0, Y_REG = 0;
    uint32_t ADRS_REG = 0;
    int step;

    if (d->Stopped) return;
    memset(d->EFREG, 0, sizeof d->EFREG);

    for (step = 0; step < d->LastStep; ++step) {
        const uint16_t *I = d->MPRO + step * 4;
        uint32_t TRA = (I[0] >> 8) & 0x7f, TWT = (I[0] >> 7) & 1, TWA = I[0] & 0x7f;
        uint32_t XSEL = (I[1] >> 15) & 1, YSEL = (I[1] >> 13) & 3, IRA = (I[1] >> 6) & 0x3f;
        uint32_t IWT = (I[1] >> 5) & 1, IWA = I[1] & 0x1f;
        uint32_t TABLE = (I[2] >> 15) & 1, MWT = (I[2] >> 14) & 1, MRD = (I[2] >> 13) & 1;
        uint32_t EWT = (I[2] >> 12) & 1, EWA = (I[2] >> 8) & 0x0f, ADRL = (I[2] >> 7) & 1;
        uint32_t FRCL = (I[2] >> 6) & 1, SHF = (I[2] >> 4) & 3, YRL = (I[2] >> 3) & 1;
        uint32_t NEGB = (I[2] >> 2) & 1, ZERO = (I[2] >> 1) & 1, BSEL = I[2] & 1;
        uint32_t NOFL = (I[3] >> 15) & 1, COEF = (I[3] >> 9) & 0x3f;
        uint32_t MASA = (I[3] >> 2) & 0x1f, ADREB = (I[3] >> 1) & 1, NXADR = I[3] & 1;
        int32_t INPUTS, B, X, Y = 0, SHIFTED = 0;
        int64_t v;

        if (IRA <= 0x1f)      INPUTS = d->MEMS[IRA];
        else if (IRA <= 0x2F) INPUTS = d->MIXS[IRA - 0x20] << 4;
        else if (IRA <= 0x31) INPUTS = d->EXTS[IRA - 0x30] << 8;
        else return;
        INPUTS = sext(INPUTS, 24);

        if (IWT) {
            d->MEMS[IWA] = MEMVAL;
            if (IRA == IWA) INPUTS = MEMVAL;
        }

        if (!ZERO) {
            B = BSEL ? ACC : sext(d->TEMP[(TRA + d->DEC) & 0x7f], 24);
            if (NEGB) B = 0 - B;
        } else
            B = 0;

        X = XSEL ? INPUTS : sext(d->TEMP[(TRA + d->DEC) & 0x7f], 24);

        if (YSEL == 0)      Y = FRC_REG;
        else if (YSEL == 1) Y = d->COEF[COEF] >> 3;
        else if (YSEL == 2) Y = (Y_REG >> 11) & 0x1fff;
        else                Y = (Y_REG >> 4) & 0x0fff;

        if (YRL) Y_REG = INPUTS;

        if (SHF == 0)      SHIFTED = clamp24(ACC);
        else if (SHF == 1) SHIFTED = clamp24(ACC * 2);
        else if (SHF == 2) SHIFTED = sext(ACC * 2, 24);
        else               SHIFTED = sext(ACC, 24);

        Y = sext(Y, 13);
        v = ((int64_t)X * (int64_t)Y) >> 12;
        ACC = (int32_t)(v + B);

        if (TWT) d->TEMP[(TWA + d->DEC) & 0x7f] = SHIFTED;

        if (FRCL) FRC_REG = SHF == 3 ? (SHIFTED & 0x0fff) : ((SHIFTED >> 11) & 0x1fff);

        if (MRD || MWT) {
            uint32_t ADDR = d->MADRS[MASA];
            if (!TABLE) ADDR += d->DEC;
            if (ADREB) ADDR += ADRS_REG & 0x0FFF;
            if (NXADR) ADDR++;
            if (!TABLE) ADDR &= d->RBL - 1;
            else        ADDR &= 0xffff;
            ADDR += d->RBP << 12;
            ADDR <<= 1;
            if (MRD && (step & 1))
                MEMVAL = NOFL ? (int32_t)ram_r16(c, ADDR) << 8 : UNPACK(ram_r16(c, ADDR));
            if (MWT && (step & 1))
                ram_w16(c, ADDR, NOFL ? (uint16_t)(SHIFTED >> 8) : PACK(SHIFTED));
        }

        if (ADRL) ADRS_REG = SHF == 3 ? ((SHIFTED >> 12) & 0xfff) : (uint32_t)(INPUTS >> 16);

        if (EWT) d->EFREG[EWA] = (int16_t)(d->EFREG[EWA] + (SHIFTED >> 8));
    }
    --d->DEC;
    memset(d->MIXS, 0, sizeof d->MIXS);
}

/* ---- registers -------------------------------------------------------- */
static void UpdateSlotReg(scsp_t *c, int s, int r)
{
    slot_t *slot = c->Slots + s;
    switch (r & 0x3f) {
    case 0: case 1:
        if (KEYONEX(slot)) {
            int sl;
            for (sl = 0; sl < 32; ++sl) {
                slot_t *s2 = c->Slots + sl;
                if (KEYONB(s2) && s2->EG.state == RELEASE) StartSlot(s2);
                if (!KEYONB(s2)) StopSlot(s2, 1);
            }
            slot->data[0] &= ~0x1000;
        }
        break;
    case 0x10: case 0x11:
        slot->step = Step(slot);
        break;
    case 0xA: case 0xB:
        slot->EG.RR = Get_DR(0, RR(slot));
        slot->EG.DL = 0x1f - DL(slot);
        break;
    case 0x12: case 0x13:
        Compute_LFO(slot);
        break;
    }
}

static void exec_dma(scsp_t *c);

static void timer_set(scsp_t *c, int t, int reg)
{
    if (!c->irq) return;
    c->TimPris[t] = 1 << ((c->data[reg / 2] >> 8) & 0x7);
    c->TimCnt[t] = (c->data[reg / 2] & 0xff) << 8;
    /* MAME: (clock / pris) / (255 - count) as a rate, 512 ticks of it --
     * which is pris * (255 - count) samples. */
    if ((c->data[reg / 2] & 0xff) != 255)
        c->timer_left[t] = (uint32_t)(c->TimPris[t] * (255 - (c->data[reg / 2] & 0xff)));
}

static void UpdateReg(scsp_t *c, int reg)
{
    switch (reg & 0x3f) {
    case 0x2: case 0x3:
        c->DSP.RBL = (8 * 1024) << RBL(c);
        c->DSP.RBP = RBP(c);
        break;
    case 0x6: case 0x7:
        if (c->midi_out) c->midi_out((uint8_t)(c->data[0x6 / 2] & 0xff));
        break;
    case 8: case 9:
        c->latched_MSLC = (uint8_t)((c->data[0x8 / 2] & 0xf800) >> 11);
        break;
    case 0x12: case 0x13:
        c->dma.dmea = (c->data[0x12 / 2] & 0xfffe) | (c->dma.dmea & 0xf0000);
        break;
    case 0x14: case 0x15:
        c->dma.dmea = ((uint32_t)(c->data[0x14 / 2] & 0xf000) << 4) | (c->dma.dmea & 0xfffe);
        c->dma.drga = c->data[0x14 / 2] & 0x0ffe;
        break;
    case 0x16: case 0x17:
        c->dma.dtlg = c->data[0x16 / 2] & 0x0ffe;
        c->dma.ddir = (c->data[0x16 / 2] & 0x2000) >> 13;
        c->dma.dgate = (c->data[0x16 / 2] & 0x4000) >> 14;
        if (c->data[0x16 / 2] & 0x1000) exec_dma(c);
        break;
    case 0x18: case 0x19: timer_set(c, 0, 0x18); break;
    case 0x1a: case 0x1b: timer_set(c, 1, 0x1a); break;
    case 0x1c: case 0x1d: timer_set(c, 2, 0x1c); break;
    case 0x1e: case 0x1f:
        if (c->irq) CheckPendingIRQ(c);
        break;
    case 0x20: case 0x21:
        if (c->irq && (c->data[0x1e / 2] & c->data[0x20 / 2] & 0x20)) CheckPendingIRQ(c);
        break;
    case 0x22: case 0x23:
        if (c->irq) {
            c->data[0x20 / 2] &= ~c->data[0x22 / 2];
            ResetInterrupts(c);
            if (c->TimCnt[0] == 0xffff) c->data[0x20 / 2] |= 0x40;
            if (c->TimCnt[1] == 0xffff) c->data[0x20 / 2] |= 0x80;
            if (c->TimCnt[2] == 0xffff) c->data[0x20 / 2] |= 0x100;
        }
        break;
    case 0x24: case 0x25: case 0x26: case 0x27: case 0x28: case 0x29:
        if (c->irq) {
            c->IrqTimA = DecodeSCI(c, SCITMA);
            c->IrqTimBC = DecodeSCI(c, SCITMB);
            c->IrqMidi = DecodeSCI(c, SCIMID);
            c->IrqCPU = DecodeSCI(c, SCIIRQ);
            c->IrqDMA = DecodeSCI(c, SCIDMA);
        }
        break;
    case 0x2a: case 0x2b:
        c->mcieb = c->data[0x2a / 2];
        break;
    case 0x2c: case 0x2d:
        if (c->data[0x2c / 2] & 0x20) MainCheckPendingIRQ(c, 0x20);
        break;
    case 0x2e: case 0x2f:
        c->mcipd &= ~c->data[0x2e / 2];
        break;
    }
}

static void UpdateRegR(scsp_t *c, int reg)
{
    switch (reg & 0x3f) {
    case 4: case 5: {
        uint16_t v = c->data[0x4 / 2] & 0xff00;
        v |= c->MidiStack[c->MidiR];
        if (c->MidiR != c->MidiW) c->MidiR = (c->MidiR + 1) & 31;
        if (c->MidiR == c->MidiW) {
            irq_line(c, c->IrqMidi, 0);
            c->data[0x20 / 2] &= ~8;
        }
        c->data[0x4 / 2] = v;
        break;
    }
    case 8: case 9:
        c->data[0x8 / 2] = c->latched_MSLC_data;
        break;
    case 0x2a: case 0x2b:
        c->data[0x2a / 2] = c->mcieb;
        break;
    case 0x2c: case 0x2d:
        c->data[0x2c / 2] = c->mcipd;
        break;
    }
}

static void w16(scsp_t *c, uint32_t addr, uint16_t val)
{
    addr &= 0xffff;
    if (addr < 0x400) {
        int slot = addr / 0x20;
        addr &= 0x1f;
        c->Slots[slot].data[addr / 2] = val;
        UpdateSlotReg(c, slot, addr & 0x1f);
    } else if (addr < 0x600) {
        if (addr < 0x430) {
            if (addr == 0x420 || addr == 0x42e)
                c->data[(addr & 0x3f) / 2] |= val & 0x20;
            else
                c->data[(addr & 0x3f) / 2] = val;
            UpdateReg(c, addr & 0x3f);
        }
    } else if (addr < 0x700) {
        c->RINGBUF[(addr - 0x600) / 2] = (int16_t)val;
    } else if (addr < 0x780) {
        c->DSP.COEF[(addr - 0x700) / 2] = (int16_t)val;
    } else if (addr < 0x7c0) {
        c->DSP.MADRS[(addr - 0x780) / 2] = val;
    } else if (addr < 0x800) {
        c->DSP.MADRS[(addr - 0x7c0) / 2] = val;
    } else if (addr < 0xC00) {
        c->DSP.MPRO[(addr - 0x800) / 2] = val;
        if (addr == 0xBF0) dsp_start(&c->DSP);
    }
}

static uint16_t r16(scsp_t *c, uint32_t addr)
{
    addr &= 0xffff;
    if (addr < 0x400) return c->Slots[addr / 0x20].data[(addr & 0x1f) / 2];
    if (addr < 0x600) {
        if (addr < 0x430) {
            UpdateRegR(c, addr & 0x3f);
            return c->data[(addr & 0x3f) / 2];
        }
        return 0;
    }
    if (addr < 0x700) return (uint16_t)c->RINGBUF[(addr - 0x600) / 2];
    if (addr < 0x780) return (uint16_t)c->DSP.COEF[(addr - 0x700) / 2];
    if (addr < 0x7c0) return c->DSP.MADRS[(addr - 0x780) / 2];
    if (addr < 0x800) return c->DSP.MADRS[(addr - 0x7c0) / 2];
    if (addr < 0xC00) return c->DSP.MPRO[(addr - 0x800) / 2];
    if (addr < 0xE00) {
        int32_t t = c->DSP.TEMP[(addr >> 2) & 0x7f];
        return (uint16_t)((addr & 2) ? (t & 0xffff) : (t >> 16));
    }
    if (addr < 0xE80) {
        int32_t t = c->DSP.MEMS[(addr >> 2) & 0x1f];
        return (uint16_t)((addr & 2) ? (t & 0xffff) : (t >> 16));
    }
    if (addr < 0xEC0) {
        int32_t t = c->DSP.MIXS[(addr >> 2) & 0xf];
        return (uint16_t)((addr & 2) ? (t & 0xffff) : (t >> 16));
    }
    if (addr < 0xEE0) return (uint16_t)c->DSP.EFREG[(addr - 0xec0) / 2];
    if (addr < 0xEE4) return (uint16_t)c->DSP.EXTS[(addr - 0xee0) / 2];
    return 0;
}

static void exec_dma(scsp_t *c)
{
    uint16_t tmp_dma[3];
    int i;
    if (!c->dma.ddir)
        for (i = 0; i < 3; i++) tmp_dma[i] = c->data[(0x12 + i * 2) / 2];
    if (c->dma.ddir) {
        for (i = 0; i < c->dma.dtlg; i += 2) {
            ram_w16(c, c->dma.dmea, c->dma.dgate ? 0 : r16(c, c->dma.drga));
            c->dma.dmea += 2;
            if (!c->dma.dgate) c->dma.drga += 2;
        }
    } else {
        for (i = 0; i < c->dma.dtlg; i += 2) {
            if (c->dma.dgate) w16(c, c->dma.drga, 0);
            else { w16(c, c->dma.drga, ram_r16(c, c->dma.dmea)); c->dma.dmea += 2; }
            c->dma.drga += 2;
        }
        for (i = 0; i < 3; i++) c->data[(0x12 + i * 2) / 2] = tmp_dma[i];
    }
    c->data[0x16 / 2] &= ~0x1000;
}

/* ---- one sample ------------------------------------------------------- */
static int32_t UpdateSlot(scsp_t *c, slot_t *slot)
{
    int32_t sample = 0;
    int step = (int)slot->step;
    uint32_t addr1, addr2, sel;
    uint32_t *addr[2], *slot_addr[2];

    if (SSCTL(slot) == 3) return 0;
    addr[0] = &addr1; addr[1] = &addr2;
    slot_addr[0] = &slot->cur_addr; slot_addr[1] = &slot->nxt_addr;

    if (PLFOS(slot) != 0) {
        step = step * PLFO_Step(&slot->PLFO);
        step >>= SHIFT;
    }

    if (PCM8B(slot)) {
        addr1 = slot->cur_addr >> SHIFT;
        addr2 = slot->nxt_addr >> SHIFT;
    } else {
        addr1 = (slot->cur_addr >> (SHIFT - 1)) & ~1u;
        addr2 = (slot->nxt_addr >> (SHIFT - 1)) & ~1u;
    }

    if (MDL(slot) != 0 || MDXSL(slot) != 0 || MDYSL(slot) != 0) {
        int32_t smp = (c->RINGBUF[(c->BUFPTR + MDXSL(slot)) & 63] + c->RINGBUF[(c->BUFPTR + MDYSL(slot)) & 63]) / 2;
        smp <<= 0xA;
        smp >>= 0x1A - MDL(slot);
        if (!PCM8B(slot)) smp <<= 1;
        addr1 += smp; addr2 += smp;
    }

    if (SSCTL(slot) == 0) {
        int32_t fpart = slot->cur_addr & ((1 << SHIFT) - 1), s;
        if (PCM8B(slot)) {
            int8_t p1 = (int8_t)ram_r8(c, SA(slot) + addr1);
            int8_t p2 = (int8_t)ram_r8(c, SA(slot) + addr2);
            s = (int)(p1 << 8) * ((1 << SHIFT) - fpart) + (int)(p2 << 8) * fpart;
        } else {
            int16_t p1 = (int16_t)ram_r16(c, SA(slot) + addr1);
            int16_t p2 = (int16_t)ram_r16(c, SA(slot) + addr2);
            s = (int)p1 * ((1 << SHIFT) - fpart) + (int)p2 * fpart;
        }
        sample = s >> SHIFT;
    } else if (SSCTL(slot) == 1) {
        c->noise = c->noise * 1103515245u + 12345u;
        sample = (int16_t)(c->noise >> 16);
    } else
        sample = 0;

    if (SBCTL(slot) & 0x1) sample ^= 0x7FFF;
    if (SBCTL(slot) & 0x2) sample = (int16_t)(sample ^ 0x8000);

    if (slot->Backwards) slot->cur_addr -= step;
    else                 slot->cur_addr += step;
    slot->nxt_addr = slot->cur_addr + (1 << SHIFT);

    addr1 = slot->cur_addr >> SHIFT;
    addr2 = slot->nxt_addr >> SHIFT;

    if (addr1 >= LSA(slot) && !slot->Backwards)
        if (LPSLNK(slot) && slot->EG.state == ATTACK) slot->EG.state = DECAY1;

    for (sel = 0; sel < 2; sel++) {
        int32_t rem;
        switch (LPCTL(slot)) {
        case 0:
            if (*addr[sel] >= LSA(slot) && *addr[sel] >= LEA(slot)) StopSlot(slot, 0);
            break;
        case 1:
            if (*addr[sel] >= LEA(slot)) {
                rem = (int32_t)(*slot_addr[sel] - ((uint32_t)LEA(slot) << SHIFT));
                *slot_addr[sel] = ((uint32_t)LSA(slot) << SHIFT) + rem;
            }
            break;
        case 2:
            if (*addr[sel] >= LSA(slot) && !slot->Backwards) {
                rem = (int32_t)(*slot_addr[sel] - ((uint32_t)LSA(slot) << SHIFT));
                *slot_addr[sel] = ((uint32_t)LEA(slot) << SHIFT) - rem;
                slot->Backwards = 1;
            } else if ((*addr[sel] < LSA(slot) || (*slot_addr[sel] & 0x80000000u)) && slot->Backwards) {
                rem = (int32_t)(((uint32_t)LSA(slot) << SHIFT) - *slot_addr[sel]);
                *slot_addr[sel] = ((uint32_t)LEA(slot) << SHIFT) - rem;
            }
            break;
        case 3:
            if (*addr[sel] >= LEA(slot)) {
                rem = (int32_t)(*slot_addr[sel] - ((uint32_t)LEA(slot) << SHIFT));
                *slot_addr[sel] = ((uint32_t)LEA(slot) << SHIFT) - rem;
                slot->Backwards = 1;
            } else if ((*addr[sel] < LSA(slot) || (*slot_addr[sel] & 0x80000000u)) && slot->Backwards) {
                rem = (int32_t)(((uint32_t)LSA(slot) << SHIFT) - *slot_addr[sel]);
                *slot_addr[sel] = ((uint32_t)LSA(slot) << SHIFT) + rem;
                slot->Backwards = 0;
            }
            break;
        }
    }

    if (!SDIR(slot)) {
        if (ALFOS(slot) != 0) {
            sample = sample * ALFO_Step(&slot->ALFO);
            sample >>= SHIFT;
        }
        if (slot->EG.state == ATTACK)
            sample = (sample * EG_Update(slot)) >> SHIFT;
        else
            sample = (sample * EG_TABLE[EG_Update(slot) >> (SHIFT - 10)]) >> SHIFT;
    }

    if (!STWINH(slot)) {
        uint16_t Enc = SDIR(slot) ? (uint16_t)(0x7 << 0xd) : (uint16_t)(TL(slot) | (0x7 << 0xd));
        *c->RBUFDST = (int16_t)((sample * LPANTABLE[Enc]) >> (SHIFT + 1));
    }
    return sample;
}

void scsp_sample(scsp_t *c, int32_t *outl, int32_t *outr)
{
    int32_t smpl = 0, smpr = 0;
    int sl, i, t;

    for (sl = 0; sl < 32; ++sl) {
        c->RBUFDST = c->RINGBUF + c->BUFPTR;
        if (c->Slots[sl].active) {
            slot_t *slot = c->Slots + sl;
            int32_t sample = UpdateSlot(c, slot);
            uint16_t tl = SDIR(slot) ? 0 : (uint16_t)TL(slot), Enc;
            Enc = (uint16_t)(tl | (IMXL(slot) << 0xd));
            c->DSP.MIXS[ISEL(slot)] += (sample * LPANTABLE[Enc]) >> (SHIFT - 2);
            Enc = (uint16_t)(tl | (DIPAN(slot) << 0x8) | (DISDL(slot) << 0xd));
            smpl += (sample * LPANTABLE[Enc]) >> SHIFT;
            smpr += (sample * RPANTABLE[Enc]) >> SHIFT;
        }
        c->BUFPTR = (c->BUFPTR + 1) & 63;
    }

    dsp_step(c);

    for (i = 0; i < 16; ++i) {
        slot_t *slot = c->Slots + i;
        if (EFSDL(slot)) {
            uint16_t Enc = (uint16_t)((EFPAN(slot) << 0x8) | (EFSDL(slot) << 0xd));
            smpl += (c->DSP.EFREG[i] * LPANTABLE[Enc]) >> SHIFT;
            smpr += (c->DSP.EFREG[i] * RPANTABLE[Enc]) >> SHIFT;
        }
    }
    /* EXTS0/1, the external inputs, are silent on Model 3. */

    /* MAME scales the 18-bit DAC's output by 1/131072 and the 16-bit one's
     * (after >> 2) by 1/32768: the same thing, so DAC18B changes nothing
     * here but the clamp, which the caller does. */
    smpl >>= 2; smpr >>= 2;
    smpl = smpl * (int32_t)MVOL(c) / 15;
    smpr = smpr * (int32_t)MVOL(c) / 15;
    *outl += smpl;
    *outr += smpr;

    /* The slot monitor, latched every sample. */
    {
        slot_t *slot = c->Slots + c->latched_MSLC;
        uint32_t SGC = slot->EG.state & 3, CA = (slot->cur_addr >> (SHIFT + 12)) & 0xf;
        uint32_t EG = (0x1f - (slot->EG.volume >> (EG_SHIFT + 5))) & 0x1f;
        c->latched_MSLC_data = (uint16_t)((CA << 7) | (SGC << 5) | EG);
    }

    for (t = 0; t < 3; t++)
        if (c->timer_left[t] && --c->timer_left[t] == 0)
            timer_fire(c, t);
}

/* ---- the outside ------------------------------------------------------ */
scsp_t *scsp_create(uint8_t *ram, size_t ram_size,
                    void (*irq)(int, int), void (*midi_out)(uint8_t))
{
    scsp_t *c = (scsp_t *)calloc(1, sizeof *c);
    if (!c) return NULL;
    build_tables();
    c->ram = ram;
    c->ram_size = ram_size;
    c->irq = irq;
    c->midi_out = midi_out;
    scsp_reset(c);
    return c;
}

void scsp_reset(scsp_t *c)
{
    int i;
    size_t keep = offsetof(scsp_t, tail);
    memset(c, 0, keep);
    c->DSP.RBL = 8 * 1024;
    c->DSP.Stopped = 1;
    c->noise = 0x1234567u;
    for (i = 0; i < 32; ++i) {
        c->Slots[i].slot = i;
        c->Slots[i].EG.state = RELEASE;
    }
    c->TimCnt[0] = c->TimCnt[1] = c->TimCnt[2] = 0xffff;
}

uint16_t scsp_read16(scsp_t *c, uint32_t offset)
{
    return r16(c, offset & ~1u);
}

void scsp_write16(scsp_t *c, uint32_t offset, uint16_t data, uint16_t mask)
{
    uint16_t tmp = r16(c, offset & ~1u);
    tmp = (uint16_t)((tmp & ~mask) | (data & mask));
    w16(c, offset & ~1u, tmp);
}

int scsp_midi_in(scsp_t *c, uint8_t byte)
{
    if (((c->MidiW + 1) & 31) == c->MidiR) return 0;
    c->MidiStack[c->MidiW] = byte;
    c->MidiW = (c->MidiW + 1) & 31;
    CheckPendingIRQ(c);
    return 1;
}

int scsp_active(scsp_t *c)
{
    int i, n = 0;
    for (i = 0; i < 32; i++) n += c->Slots[i].active;
    return n;
}

size_t scsp_state_size(void) { return offsetof(scsp_t, tail); }

void scsp_state_save(scsp_t *c, void *buf) { memcpy(buf, c, offsetof(scsp_t, tail)); }

void scsp_state_load(scsp_t *c, const void *buf)
{
    int i;
    memcpy(c, buf, offsetof(scsp_t, tail));
    /* The LFO tables are pointers into this build's statics. */
    for (i = 0; i < 32; ++i) Compute_LFO(&c->Slots[i]);
}
