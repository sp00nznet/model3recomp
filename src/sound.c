/* model3recomp -- the sound board. See sound.h.
 *
 * Wiring from MAME's sega/model3.cpp (BSD-3-Clause; R. Belmont, Ville
 * Linde): the 68000's map, the sample bank, and the hack it uses for the
 * PowerPC's sound interrupt.
 *
 *   68000   0x000000  512 KB RAM, also SCSP 1's sample RAM
 *           0x100000  SCSP 1 registers
 *           0x200000  512 KB RAM, also SCSP 2's sample RAM
 *           0x300000  SCSP 2 registers
 *           0x400001  bank select: bit 4 picks the 8 MB sample bank
 *           0x600000  the program, 512 KB
 *           0x800000  the sample ROM, banked
 *
 * The PowerPC talks to SCSP 1's MIDI port through an i8251 UART. Here the
 * UART is two byte queues: what the PowerPC writes goes to SCSP 1 at the
 * line's own rate (31250 baud, a byte every 14 samples), and what SCSP 1
 * sends back waits for the PowerPC to read it.
 */
#include "model3recomp/sound.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/irq.h"
#include "model3recomp/savestate.h"
#include "scsp.h"
#include "m68k/m68k.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#define SND_RAM      0x80000u
#define RATE         44100
#define CYCLES       256            /* 11 289 600 Hz / 44 100 */
#define BYTE_SAMPLES 14             /* 31250 baud, 10 bits a byte */
#define QSIZE        256

static const uint8_t *g_prog, *g_samples;
static size_t g_prog_size, g_samples_size;
static uint8_t *g_ram1, *g_ram2;
static scsp_t *g_scsp1, *g_scsp2;

static struct {
    uint32_t bank;                  /* sample ROM offset of the window */
    uint32_t irq_lines;             /* 68000 levels SCSP 1 holds up */
    uint8_t  tx[QSIZE]; unsigned tx_r, tx_w;   /* PowerPC -> SCSP 1 */
    uint8_t  rx[QSIZE]; unsigned rx_r, rx_w;   /* SCSP 1 -> PowerPC */
    unsigned tx_clock;
    uint64_t sample_acc;            /* field -> sample count, exactly */
} g_s;

/* The PowerPC's side of the UART. It is touched only by the PowerPC's
 * thread, and trades bytes with the board's side (g_s.tx / g_s.rx) only at
 * a field boundary -- which is what lets the board run a field behind on
 * its own thread and still be deterministic: what the guest reads depends
 * on the field count and what it sent, never on how far the board got. */
static struct {
    uint8_t  tx[QSIZE]; unsigned tx_r, tx_w;   /* waiting to go to the board */
    uint8_t  rx[QSIZE]; unsigned rx_r, rx_w;   /* arrived from the board */
    int      ppc_irq;
} g_p;

int sound_present(void) { return g_scsp1 != NULL; }

/* ---- 68000 memory ----------------------------------------------------- */
static unsigned rd8(unsigned a)
{
    a &= 0xFFFFFF;
    if (a < 0x080000) return g_ram1[a];
    if (a >= 0x200000 && a < 0x280000) return g_ram2[a - 0x200000];
    if ((a & 0xFFF000) == 0x100000) {
        uint16_t w = scsp_read16(g_scsp1, a & 0xFFF);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if ((a & 0xFFF000) == 0x300000) {
        uint16_t w = scsp_read16(g_scsp2, a & 0xFFF);
        return (a & 1) ? (w & 0xFF) : (w >> 8);
    }
    if (a >= 0x600000 && a < 0x680000) {
        a -= 0x600000;
        return a < g_prog_size ? g_prog[a] : 0xFF;
    }
    if (a >= 0x800000 && g_samples) {
        size_t o = (g_s.bank + (a - 0x800000)) % g_samples_size;
        return g_samples[o];
    }
    return 0xFF;
}

static unsigned rd16(unsigned a)
{
    a &= 0xFFFFFE;
    if ((a & 0xFFF000) == 0x100000) return scsp_read16(g_scsp1, a & 0xFFF);
    if ((a & 0xFFF000) == 0x300000) return scsp_read16(g_scsp2, a & 0xFFF);
    return (rd8(a) << 8) | rd8(a + 1);
}

static void wr8(unsigned a, unsigned v)
{
    a &= 0xFFFFFF;
    if (a < 0x080000) { g_ram1[a] = (uint8_t)v; return; }
    if (a >= 0x200000 && a < 0x280000) { g_ram2[a - 0x200000] = (uint8_t)v; return; }
    if ((a & 0xFFF000) == 0x100000 || (a & 0xFFF000) == 0x300000) {
        scsp_t *c = (a & 0xFFF000) == 0x100000 ? g_scsp1 : g_scsp2;
        if (a & 1) scsp_write16(c, a & 0xFFE, (uint16_t)(v & 0xFF), 0x00FF);
        else       scsp_write16(c, a & 0xFFE, (uint16_t)((v & 0xFF) << 8), 0xFF00);
        return;
    }
    if (a == 0x400001) {
        g_s.bank = (v & 0x10) ? 0x800000u : 0u;
        if (g_samples_size) g_s.bank %= (uint32_t)g_samples_size;
        return;
    }
}

static void wr16(unsigned a, unsigned v)
{
    a &= 0xFFFFFE;
    if ((a & 0xFFF000) == 0x100000) { scsp_write16(g_scsp1, a & 0xFFF, (uint16_t)v, 0xFFFF); return; }
    if ((a & 0xFFF000) == 0x300000) { scsp_write16(g_scsp2, a & 0xFFF, (uint16_t)v, 0xFFFF); return; }
    wr8(a, v >> 8);
    wr8(a + 1, v & 0xFF);
}

/* Musashi's callbacks. */
unsigned int m68k_read_memory_8(unsigned int a)  { return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return (rd16(a) << 16) | rd16(a + 2); }
void m68k_write_memory_8(unsigned int a, unsigned int v)  { wr8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { wr16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { wr16(a, v >> 16); wr16(a + 2, v & 0xFFFF); }
unsigned int m68k_read_disassembler_8(unsigned int a)  { return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return rd16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) { return m68k_read_memory_32(a); }

/* ---- interrupts and MIDI ---------------------------------------------- */
/* Musashi asks which vector an interrupt wants: the 68000's autovector.
 * Having a handler at all is the point -- see m68kconf.h. */
int sound_int_ack(int level)
{
    (void)level;
    return M68K_INT_ACK_AUTOVECTOR;
}

/* The SCSP holds a line per level until the driver clears it; the 68000
 * sees the highest one held. */
static void set_68k_irq(void)
{
    int level = 7;
    while (level > 0 && !(g_s.irq_lines & (1u << level))) level--;
    m68k_set_irq((unsigned)level);
}

static void scsp1_irq(int level, int state)
{
    if (level <= 0 || level > 7) return;
    if (state) g_s.irq_lines |= 1u << level;
    else       g_s.irq_lines &= ~(1u << level);
    set_68k_irq();
}

static void scsp1_midi_out(uint8_t b)
{
    if (((g_s.rx_w + 1) % QSIZE) != g_s.rx_r) {
        g_s.rx[g_s.rx_w] = b;
        g_s.rx_w = (g_s.rx_w + 1) % QSIZE;
    }
}

/* ---- the PowerPC's side ----------------------------------------------- */
uint8_t sound_uart_read(unsigned reg)
{
    if (!g_scsp1) return reg ? 0x05 : 0;      /* no board: always ready */
    if (reg == 0) {
        uint8_t b = 0;
        if (g_p.rx_r != g_p.rx_w) {
            b = g_p.rx[g_p.rx_r];
            g_p.rx_r = (g_p.rx_r + 1) % QSIZE;
        }
        return b;
    }
    /* i8251 status: TxRDY, RxRDY, TxEMPTY. */
    return (uint8_t)((((g_p.tx_w + 1) % QSIZE) != g_p.tx_r ? 0x01 : 0)
                   | (g_p.rx_r != g_p.rx_w ? 0x02 : 0)
                   | (g_p.tx_r == g_p.tx_w ? 0x04 : 0));
}

void sound_uart_write(unsigned reg, uint8_t v)
{
    if (getenv("M3_SND_TRACE"))
        fprintf(stderr, "[snd] field %llu ppc writes %s %02X\n",
                (unsigned long long)model3recomp_frame_count(), reg ? "ctl" : "data", v);
    if (reg == 0) {
        /* Sending a byte clears the sound interrupt (MAME). */
        irq_ack(M3_IRQ_SOUND);
        if (g_scsp1 && ((g_p.tx_w + 1) % QSIZE) != g_p.tx_r) {
            g_p.tx[g_p.tx_w] = v;
            g_p.tx_w = (g_p.tx_w + 1) % QSIZE;
        }
        return;
    }
    /* A UART command with bit 5 set turns on the PowerPC's sound interrupt,
     * which MAME raises every millisecond; here it is raised once a field.
     * That is how the guest paces its command stream. */
    g_p.ppc_irq = (v & 0x20) != 0;
    if (!g_p.ppc_irq) irq_ack(M3_IRQ_SOUND);
}

/* ---- running ---------------------------------------------------------- */
void sound_init(const uint8_t *prog, size_t prog_size,
                const uint8_t *samples, size_t samples_size)
{
    if (!prog || prog_size < 16 || getenv("M3_NO_SOUND")) return;
    g_prog = prog; g_prog_size = prog_size;
    g_samples = samples; g_samples_size = samples ? samples_size : 0;
    g_ram1 = (uint8_t *)calloc(1, SND_RAM);
    g_ram2 = (uint8_t *)calloc(1, SND_RAM);
    if (!g_ram1 || !g_ram2) return;
    /* The reset vectors are read from RAM at 0, so the board copies them
     * there first (MAME does the same). */
    memcpy(g_ram1, prog, 16);
    g_scsp1 = scsp_create(g_ram1, SND_RAM, scsp1_irq, scsp1_midi_out);
    g_scsp2 = scsp_create(g_ram2, SND_RAM, NULL, NULL);
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_pulse_reset();
}

/* M3_WAV=<file>: also write the audio to a WAV file, for listening to a
 * scripted run or checking it by eye. The header's sizes are patched in
 * as it grows. */
static void wav_out(const int16_t *lr, unsigned frames)
{
    static FILE *f;
    static int probed;
    static uint32_t bytes;
    if (!probed) {
        const char *e = getenv("M3_WAV");
        probed = 1;
        if (e && (f = fopen(e, "wb")) != NULL) {
            static const uint8_t h[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',
                16,0,0,0, 1,0, 2,0, 0x44,0xAC,0,0, 0x10,0xB1,2,0, 4,0, 16,0, 'd','a','t','a',0,0,0,0 };
            fwrite(h, 1, 44, f);
        }
    }
    if (!f) return;
    fwrite(lr, 4, frames, f);        /* little-endian host */
    bytes += frames * 4u;
    {
        uint32_t riff = bytes + 36;
        long here = ftell(f);
        fseek(f, 4, SEEK_SET);  fwrite(&riff, 4, 1, f);
        fseek(f, 40, SEEK_SET); fwrite(&bytes, 4, 1, f);
        fseek(f, here, SEEK_SET);
    }
}

static int16_t clip16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

/* One field of the board: the samples, the 68000 between them, and the
 * UART's line delivering a byte every 14 samples. */
static void board_field(void)
{
    /* 44100 Hz over the 57.524 Hz field (17385 us): 766.68 samples, kept
     * exact by carrying the remainder. */
    static int16_t buf[2 * 1024];
    unsigned n, i;

    g_s.sample_acc += (uint64_t)RATE * 17385u;
    n = (unsigned)(g_s.sample_acc / 1000000u);
    g_s.sample_acc %= 1000000u;
    if (n > 1024) n = 1024;

    for (i = 0; i < n; i++) {
        int32_t l = 0, r = 0;
        if (++g_s.tx_clock >= BYTE_SAMPLES) {
            g_s.tx_clock = 0;
            if (g_s.tx_r != g_s.tx_w && scsp_midi_in(g_scsp1, g_s.tx[g_s.tx_r]))
                g_s.tx_r = (g_s.tx_r + 1) % QSIZE;
        }
        m68k_execute(CYCLES);
        scsp_sample(g_scsp1, &l, &r);
        scsp_sample(g_scsp2, &l, &r);
        buf[2 * i] = clip16(l);
        buf[2 * i + 1] = clip16(r);
    }
    platform_audio(buf, (int)n);
    wav_out(buf, n);
}

/* ---- the board's thread ------------------------------------------------
 * The board costs a few milliseconds a field -- a third of the budget on
 * one core -- so it runs beside the guest: field N's sound is made while
 * the PowerPC runs field N+1. M3_SOUND_THREAD=0 runs it inline instead,
 * which gives the same result, only slower. */
#ifdef _WIN32
static SRWLOCK g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
#  define LOCK()   AcquireSRWLockExclusive(&g_lock)
#  define UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#  define WAIT()   SleepConditionVariableSRW(&g_cv, &g_lock, INFINITE, 0)
#  define WAKE()   WakeAllConditionVariable(&g_cv)
#else
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
#  define LOCK()   pthread_mutex_lock(&g_lock)
#  define UNLOCK() pthread_mutex_unlock(&g_lock)
#  define WAIT()   pthread_cond_wait(&g_cv, &g_lock)
#  define WAKE()   pthread_cond_broadcast(&g_cv)
#endif
static int g_threaded = -1, g_busy;
uint64_t g_board_us, g_wait_us;   /* M3_PROFILE: the board's cost, and the guest's wait for it */

#ifdef _WIN32
static DWORD WINAPI board_thread(LPVOID arg)
#else
static void *board_thread(void *arg)
#endif
{
    (void)arg;
    for (;;) {
        LOCK();
        while (!g_busy) WAIT();
        UNLOCK();
        {
            uint64_t t = platform_ticks_us();
            board_field();
            g_board_us += platform_ticks_us() - t;
        }
        LOCK();
        g_busy = 0;
        WAKE();
        UNLOCK();
    }
#ifdef _WIN32
    return 0;
#else
    return NULL;
#endif
}

/* Wait for the board to finish the field it is making. */
static void board_sync(void)
{
    if (g_threaded <= 0) return;
    LOCK();
    while (g_busy) WAIT();
    UNLOCK();
}

void sound_run_field(void)
{
    uint64_t t0;
    if (!g_scsp1) return;
    if (g_threaded < 0) {
        const char *e = getenv("M3_SOUND_THREAD");
        g_threaded = !(e && *e == '0');
        if (g_threaded) {
#ifdef _WIN32
            HANDLE h = CreateThread(NULL, 0, board_thread, NULL, 0, NULL);
            if (h) CloseHandle(h); else g_threaded = 0;
#else
            pthread_t t;
            if (pthread_create(&t, NULL, board_thread, NULL)) g_threaded = 0;
            else pthread_detach(t);
#endif
        }
    }
    t0 = platform_ticks_us();
    board_sync();
    g_wait_us += platform_ticks_us() - t0;

    /* The field boundary: bytes the guest sent go down the line, bytes the
     * board sent back become readable. */
    while (g_p.tx_r != g_p.tx_w && ((g_s.tx_w + 1) % QSIZE) != g_s.tx_r) {
        g_s.tx[g_s.tx_w] = g_p.tx[g_p.tx_r];
        g_s.tx_w = (g_s.tx_w + 1) % QSIZE;
        g_p.tx_r = (g_p.tx_r + 1) % QSIZE;
    }
    while (g_s.rx_r != g_s.rx_w && ((g_p.rx_w + 1) % QSIZE) != g_p.rx_r) {
        g_p.rx[g_p.rx_w] = g_s.rx[g_s.rx_r];
        g_p.rx_w = (g_p.rx_w + 1) % QSIZE;
        g_s.rx_r = (g_s.rx_r + 1) % QSIZE;
    }
    if (g_p.ppc_irq) irq_raise(M3_IRQ_SOUND);

    if (g_threaded) {
        LOCK();
        g_busy = 1;
        WAKE();
        UNLOCK();
    } else {
        board_field();
    }
}

void sound_state(m3_state_t *st)
{
    size_t n = scsp_state_size();
    uint8_t *tmp;
    if (!g_scsp1) return;
    board_sync();                   /* the board must be between fields */
    m3_state_io(st, g_ram1, SND_RAM);
    m3_state_io(st, g_ram2, SND_RAM);
    M3_STATE_VAR(st, g_s);
    M3_STATE_VAR(st, g_p);
    tmp = (uint8_t *)malloc(n);
    if (tmp) {
        scsp_state_save(g_scsp1, tmp); m3_state_io(st, tmp, n); scsp_state_load(g_scsp1, tmp);
        scsp_state_save(g_scsp2, tmp); m3_state_io(st, tmp, n); scsp_state_load(g_scsp2, tmp);
        free(tmp);
    }
    {
        unsigned cs = m68k_context_size();
        uint8_t *ctx = (uint8_t *)malloc(cs);
        if (ctx) {
            m68k_get_context(ctx);
            m3_state_io(st, ctx, cs);
            m68k_set_context(ctx);
            free(ctx);
        }
    }
}
