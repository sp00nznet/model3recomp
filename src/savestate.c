/* model3recomp -- save states. See savestate.h. */
#include "model3recomp/savestate.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/platform.h"
#include "model3recomp/netplay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STATE_MAGIC   0x5333334Du       /* "M33S" */
#define STATE_VERSION 3u          /* 2: the sound board; 3: the EEPROM rewritten */

uint32_t m3_safepoint_pc = 0xFFFFFFFFu;

struct m3_state {
    FILE *fp;
    int   saving;
    int   ok;
};

static int g_want_save = -1, g_want_load = -1;

static void path_for(int slot, char *out, size_t n)
{
    const char *pre = model3recomp_config()->state_prefix;
    snprintf(out, n, "%s.%d.m3s", pre ? pre : "model3recomp", slot);
}

int m3_state_exists(int slot)
{
    char p[512];
    FILE *f;
    path_for(slot, p, sizeof p);
    f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void status(const char *fmt, int slot)
{
    char b[128];
    snprintf(b, sizeof b, fmt, slot);
    fprintf(stderr, "[state] %s\n", b);
    platform_set_status(b);
}

void m3_state_request_save(int slot)
{
    if (m3_safepoint_pc == 0xFFFFFFFFu) { status("save states: this game has no safe point", slot); return; }
    if (netplay_active()) { status("save states are off during netplay", slot); return; }
    g_want_save = slot;
}

void m3_state_request_load(int slot)
{
    if (m3_safepoint_pc == 0xFFFFFFFFu) { status("save states: this game has no safe point", slot); return; }
    if (netplay_active()) { status("save states are off during netplay", slot); return; }
    if (!m3_state_exists(slot)) { status("slot %d is empty", slot); return; }
    g_want_load = slot;
}

/* Zero runs and literals: most of a board's memory is zeros, and this
 * takes a state from about 34 MB to a few. Per call: the raw length, then
 * (zeros, literals, literal bytes) until it is accounted for. */
static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static int get32(FILE *f, uint32_t *v) { return fread(v, 4, 1, f) == 1; }

void m3_state_io(m3_state_t *st, void *p, size_t n)
{
    uint8_t *b = (uint8_t *)p;
    size_t i = 0;
    if (!st->ok) return;
    if (st->saving == 1) {
        put32(st->fp, (uint32_t)n);
        while (i < n) {
            size_t z = i, l, zeros = 0;
            while (z < n && b[z] == 0) z++;
            /* Literals run to the end, or to the start of 16 zeros. */
            for (l = z; l < n; l++) {
                if (b[l] != 0) { zeros = 0; continue; }
                if (++zeros == 16) { l -= 15; break; }
            }
            put32(st->fp, (uint32_t)(z - i));
            put32(st->fp, (uint32_t)(l - z));
            fwrite(b + z, 1, l - z, st->fp);
            i = l;
        }
    } else {
        /* saving == 0 restores; saving == 2 only checks the file decodes,
         * so a bad one is refused before anything has been overwritten. */
        uint32_t len, zz, ll;
        if (!get32(st->fp, &len) || len != n) { st->ok = 0; return; }
        while (i < n) {
            if (!get32(st->fp, &zz) || !get32(st->fp, &ll) || i + zz + ll > n) { st->ok = 0; return; }
            if (st->saving == 0) memset(b + i, 0, zz);
            i += zz;
            if (st->saving == 0) {
                if (fread(b + i, 1, ll, st->fp) != ll) { st->ok = 0; return; }
            } else if (fseek(st->fp, (long)ll, SEEK_CUR) != 0) {
                st->ok = 0; return;
            }
            i += ll;
        }
    }
}

static void all(m3_state_t *st)
{
    ppc_state(st);
    model3recomp_state(st);
    bus_state(st);
    irq_state(st);
    scsi_state(st);
    real3d_state(st);
    input_state(st);
    sound_state(st);
}

/* saving: 1 write, 0 restore, 2 check only. */
static int run(int slot, int saving)
{
    char p[512];
    m3_state_t st;
    uint32_t hdr[4] = { STATE_MAGIC, STATE_VERSION, 0, 0 };

    hdr[2] = m3_safepoint_pc;
    path_for(slot, p, sizeof p);
    st.fp = fopen(p, saving == 1 ? "wb" : "rb");
    st.saving = saving;
    st.ok = st.fp != NULL;
    if (!st.ok) return 0;
    if (saving == 1) {
        fwrite(hdr, sizeof hdr, 1, st.fp);
    } else {
        uint32_t h[4];
        if (fread(h, sizeof h, 1, st.fp) != 1 || h[0] != hdr[0] || h[1] != hdr[1] || h[2] != hdr[2]) {
            fclose(st.fp);
            return 0;
        }
    }
    all(&st);
    fclose(st.fp);
    return st.ok;
}

void m3_safepoint(void)
{
    /* For harnesses: M3_STATE_SAVE_AT=<field>[,slot] saves once that field
     * is reached, M3_STATE_LOAD=<slot> loads at the first safe point. */
    static int probed, loaded;
    static unsigned long long save_at;
    static int save_slot = 1, load_slot;
    if (!probed) {
        const char *e = getenv("M3_STATE_SAVE_AT"), *c;
        probed = 1;
        if (e) { save_at = strtoull(e, NULL, 0); c = strchr(e, ','); if (c) save_slot = atoi(c + 1); }
        e = getenv("M3_STATE_LOAD");
        if (e) load_slot = atoi(e);
    }
    if (save_at && model3recomp_frame_count() >= save_at) { save_at = 0; g_want_save = save_slot; }
    if (load_slot && !loaded) { loaded = 1; g_want_load = load_slot; }

    if (g_want_save >= 0) {
        int s = g_want_save;
        g_want_save = -1;
        if (run(s, 1)) status("saved state %d", s);
        else           status("could not save state %d", s);
    }
    if (g_want_load >= 0) {
        int s = g_want_load;
        g_want_load = -1;
        if (!run(s, 2)) { status("state %d is damaged or from another build", s); return; }
        if (run(s, 0)) status("loaded state %d", s);
        else           status("state %d failed part way through loading", s);
    }
}
