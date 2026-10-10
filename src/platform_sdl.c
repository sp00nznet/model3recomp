/* model3recomp -- SDL2 platform layer: the window, its menu, the controls.
 *
 * Settings live in an ini file (m3_config_t.ini_path) and are written back
 * whenever they change. On Windows the window carries a menu bar; elsewhere
 * the same settings are reachable through the ini and the hotkeys.
 *
 *   F5 save state   F6 next slot   F7 load state   F11 / Alt+Enter fullscreen
 *   Tab (held) run flat out        Esc leave fullscreen, or quit
 */
#include "model3recomp/platform.h"
#include "model3recomp/model3recomp.h"
#include "model3recomp/savestate.h"
#include "model3recomp/netplay.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static SDL_Texture  *g_tex;
static SDL_GameController *g_pad;
static uint32_t     *g_fb;
static int           g_w, g_h;
static char          g_title[128];
static char          g_net_status[160];
static char          g_note[160];
static uint64_t      g_note_until;

/* ---- settings --------------------------------------------------------- */
enum { CUR_HIDDEN, CUR_CROSSHAIR, CUR_SYSTEM };

static struct {
    int scale;          /* window size, multiples of the board's 496x384 */
    int fullscreen;
    int smooth;         /* linear filtering rather than sharp pixels */
    int scanlines;
    int aspect43;       /* 4:3, as the cabinet's monitor showed it */
    int cursor;         /* CUR_* */
    int mouse;          /* the mouse aims player 1 */
    int pad;            /* gamepad: 0 off, 1 player 1, 2 player 2 */
    int sinden;         /* white border for a Sinden gun: 0 off, 1 thin, 2 thick */
    int slot;           /* save state slot, 1..9 */
    int net_delay;      /* fields of input delay when hosting */
    int net_port;
    char net_join[128]; /* last address joined */
    int volume;         /* percent */
    int mute;
} opt = { 2, 0, 0, 0, 1, CUR_CROSSHAIR, 1, 2, 0, 1, 3, 7777, "", 100, 0 };

static const struct { const char *key; int *v; } k_int[] = {
    { "scale", &opt.scale }, { "fullscreen", &opt.fullscreen },
    { "smooth", &opt.smooth }, { "scanlines", &opt.scanlines },
    { "aspect43", &opt.aspect43 }, { "cursor", &opt.cursor },
    { "mouse", &opt.mouse }, { "pad", &opt.pad }, { "sinden", &opt.sinden },
    { "slot", &opt.slot }, { "net_delay", &opt.net_delay },
    { "net_port", &opt.net_port }, { "volume", &opt.volume }, { "mute", &opt.mute },
};

static const char *ini_path(void)
{
    const char *p = model3recomp_config()->ini_path;
    return p ? p : "model3recomp.ini";
}

static void settings_load(void)
{
    char line[256];
    FILE *f = fopen(ini_path(), "r");
    unsigned i;
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '='), *nl;
        if (!eq) continue;
        *eq++ = 0;
        if ((nl = strpbrk(eq, "\r\n"))) *nl = 0;
        for (i = 0; i < sizeof k_int / sizeof k_int[0]; i++)
            if (!strcmp(line, k_int[i].key)) *k_int[i].v = atoi(eq);
        if (!strcmp(line, "net_join"))
            snprintf(opt.net_join, sizeof opt.net_join, "%s", eq);
        if (!strcmp(line, "game_options"))
            m3_options_set_local_all((uint32_t)strtoul(eq, NULL, 0));
    }
    fclose(f);
    if (opt.scale < 1 || opt.scale > 4) opt.scale = 2;
    if (opt.slot < 1 || opt.slot > 9) opt.slot = 1;
    if (opt.net_delay < 1 || opt.net_delay > 10) opt.net_delay = 3;
    if (opt.volume < 0 || opt.volume > 100) opt.volume = 100;
}

static void settings_save(void)
{
    FILE *f = fopen(ini_path(), "w");
    unsigned i;
    if (!f) return;
    for (i = 0; i < sizeof k_int / sizeof k_int[0]; i++)
        fprintf(f, "%s=%d\n", k_int[i].key, *k_int[i].v);
    fprintf(f, "net_join=%s\n", opt.net_join);
    fprintf(f, "game_options=0x%08X\n", (unsigned)m3_options_local_all());
    fclose(f);
}

/* ---- title bar -------------------------------------------------------- */
static void update_title(void)
{
    char t[512];
    const char *note = SDL_GetTicks64() < g_note_until ? g_note : "";
    snprintf(t, sizeof t, "%s%s%s%s%s", g_title,
             g_net_status[0] ? "  -  " : "", g_net_status,
             note[0] ? "  -  " : "", note);
    if (g_win) SDL_SetWindowTitle(g_win, t);
}

void platform_set_status(const char *s)
{
    /* Netplay's line stays; everything else is a note that fades. */
    if (!strncmp(s, "netplay", 7)) {
        snprintf(g_net_status, sizeof g_net_status, "%s", s);
    } else {
        snprintf(g_note, sizeof g_note, "%s", s);
        g_note_until = SDL_GetTicks64() + 3000;
    }
    update_title();
}

/* ---- where the picture goes ------------------------------------------- */
static SDL_Rect view_rect(void)
{
    SDL_Rect r;
    int ow = 1, oh = 1;
    double aspect = opt.aspect43 ? 4.0 / 3.0 : (double)g_w / g_h;
    SDL_GetRendererOutputSize(g_ren, &ow, &oh);
    if ((double)ow / oh > aspect) { r.h = oh; r.w = (int)(oh * aspect + 0.5); }
    else                          { r.w = ow; r.h = (int)(ow / aspect + 0.5); }
    r.x = (ow - r.w) / 2;
    r.y = (oh - r.h) / 2;
    return r;
}

static void apply_video(void)
{
    if (!g_win) return;
    SDL_SetWindowFullscreen(g_win, opt.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    if (!opt.fullscreen) {
        int h = g_h * opt.scale;
        int w = opt.aspect43 ? h * 4 / 3 : g_w * opt.scale;
        SDL_SetWindowSize(g_win, w, h);
    }
    SDL_SetTextureScaleMode(g_tex, opt.smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    SDL_ShowCursor(opt.cursor == CUR_SYSTEM ? SDL_ENABLE : SDL_DISABLE);
}

/* ---- menu (Windows) ---------------------------------------------------- */
#ifdef _WIN32
enum {
    ID_SAVE = 100, ID_LOAD, ID_RESET, ID_QUIT,
    ID_SLOT = 110,                      /* +1..9 */
    ID_SCALE = 130,                     /* +1..4 */
    ID_FULL = 140, ID_SHARP, ID_SMOOTH, ID_SCAN, ID_ASPECT43, ID_ASPECTSQ,
    ID_MOUSE = 160, ID_PAD = 170, ID_CURSOR = 180, ID_SINDEN = 190,
    ID_CHEAT = 200,                     /* +bit */
    ID_OPTION = 240,                    /* +slot*16+choice */
    ID_MUTE = 380, ID_VOL = 381,        /* +0..4: 100, 75, 50, 25 */
    ID_HOST = 400, ID_JOIN, ID_DISCONNECT,
    ID_DELAY = 410                      /* +1..10 */
};

static HWND hwnd(void)
{
    SDL_SysWMinfo i;
    SDL_VERSION(&i.version);
    return SDL_GetWindowWMInfo(g_win, &i) ? i.info.win.window : NULL;
}

static void item(HMENU m, UINT id, const char *label, int checked)
{
    AppendMenuA(m, MF_STRING | (checked ? MF_CHECKED : 0), id, label);
}

static void note_item(HMENU m, const char *label)
{
    AppendMenuA(m, MF_STRING | MF_GRAYED, 0, label);
}

static void build_menu(void)
{
    HWND h = hwnd();
    HMENU bar, file, slot, video, size, sound, ctl, pad, cur, sind, dbg, mp, dly;
    HMENU old;
    char b[64];
    int i, any = 0;

    if (!h) return;
    bar = CreateMenu();

    file = CreatePopupMenu(); slot = CreatePopupMenu();
    item(file, ID_SAVE, "&Save state\tF5", 0);
    item(file, ID_LOAD, "&Load state\tF7", 0);
    for (i = 1; i <= 9; i++) {
        snprintf(b, sizeof b, "Slot %d%s", i, m3_state_exists(i) ? "" : "  (empty)");
        item(slot, ID_SLOT + i, b, opt.slot == i);
    }
    AppendMenuA(file, MF_POPUP, (UINT_PTR)slot, "Sl&ot\tF6");
    AppendMenuA(file, MF_SEPARATOR, 0, NULL);
    item(file, ID_RESET, "&Reset", 0);
    item(file, ID_QUIT, "&Quit\tEsc", 0);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)file, "&File");

    video = CreatePopupMenu(); size = CreatePopupMenu();
    for (i = 1; i <= 4; i++) {
        snprintf(b, sizeof b, "%dx  (%d x %d)", i, opt.aspect43 ? g_h * i * 4 / 3 : g_w * i, g_h * i);
        item(size, ID_SCALE + i, b, opt.scale == i);
    }
    AppendMenuA(video, MF_POPUP, (UINT_PTR)size, "&Window size");
    item(video, ID_FULL, "&Fullscreen\tF11", opt.fullscreen);
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    item(video, ID_SHARP, "Sharp pixels", !opt.smooth);
    item(video, ID_SMOOTH, "Smooth (bilinear)", opt.smooth);
    item(video, ID_SCAN, "Scanlines", opt.scanlines);
    AppendMenuA(video, MF_SEPARATOR, 0, NULL);
    item(video, ID_ASPECT43, "4:3, as on the cabinet", opt.aspect43);
    item(video, ID_ASPECTSQ, "Square pixels (496 x 384)", !opt.aspect43);
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)video, "&Video");

    sound = CreatePopupMenu();
    item(sound, ID_MUTE, "&Mute\tF9", opt.mute);
    AppendMenuA(sound, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < 4; i++) {
        static const int lv[4] = { 100, 75, 50, 25 };
        snprintf(b, sizeof b, "Volume %d%%", lv[i]);
        item(sound, ID_VOL + i, b, opt.volume == lv[i]);
    }
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)sound, "&Sound");

    ctl = CreatePopupMenu(); pad = CreatePopupMenu(); cur = CreatePopupMenu(); sind = CreatePopupMenu();
    item(ctl, ID_MOUSE, "&Mouse aims player 1", opt.mouse);
    item(pad, ID_PAD + 0, "Off", opt.pad == 0);
    item(pad, ID_PAD + 1, "Player 1", opt.pad == 1);
    item(pad, ID_PAD + 2, "Player 2", opt.pad == 2);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)pad, g_pad ? "&Gamepad" : "&Gamepad  (none connected)");
    item(cur, ID_CURSOR + CUR_HIDDEN, "Hidden, as on the cabinet", opt.cursor == CUR_HIDDEN);
    item(cur, ID_CURSOR + CUR_CROSSHAIR, "Crosshair", opt.cursor == CUR_CROSSHAIR);
    item(cur, ID_CURSOR + CUR_SYSTEM, "Mouse pointer", opt.cursor == CUR_SYSTEM);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)cur, "&Cursor");
    item(sind, ID_SINDEN + 0, "Off", opt.sinden == 0);
    item(sind, ID_SINDEN + 1, "Thin", opt.sinden == 1);
    item(sind, ID_SINDEN + 2, "Thick", opt.sinden == 2);
    AppendMenuA(ctl, MF_POPUP, (UINT_PTR)sind, "&Sinden light gun border");
    AppendMenuA(ctl, MF_SEPARATOR, 0, NULL);
    note_item(ctl, "Mouse: left fires, right reloads (points off screen)");
    note_item(ctl, "Gamepad: left stick aims, A/RT fire, B/LT reload, Start, Back = coin");
    note_item(ctl, "Keys: 5/6 coin, 1/2 start, Space fire, Shift reload, F2 test, F3 service");
    note_item(ctl, "Sinden: run its software in mouse mode, off-screen reload = right click");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)ctl, "&Controls");

    dbg = CreatePopupMenu();
    for (i = 0; i < 32; i++) {
        const char *l = m3_cheat_label((unsigned)i);
        if (!l) continue;
        item(dbg, ID_CHEAT + i, l, !m3_cheat_is_action((unsigned)i) && ((m3_cheats_local() >> i) & 1u));
        any = 1;
    }
    if (!any) note_item(dbg, "This game offers no cheats");
    if (netplay_role() == 2) note_item(dbg, "(during netplay the host's cheats apply)");
    {
        unsigned sl, c;
        for (sl = 0; sl < 8u; sl++) {
            HMENU sub;
            char lbl[96];
            if (!m3_option_label(sl)) continue;
            sub = CreatePopupMenu();
            for (c = 0; c < m3_option_count(sl); c++)
                AppendMenuA(sub, MF_STRING | (m3_option_local(sl) == c ? MF_CHECKED : 0)
                                 | (netplay_active() ? MF_GRAYED : 0),
                            ID_OPTION + sl * 16u + c, m3_option_choice(sl, c));
            if (netplay_active()) note_item(sub, "(not during netplay: changing it restarts the game)");
            snprintf(lbl, sizeof lbl, "%s  (restarts the game)", m3_option_label(sl));
            if (sl == 0) AppendMenuA(dbg, MF_SEPARATOR, 0, NULL);
            AppendMenuA(dbg, MF_POPUP, (UINT_PTR)sub, lbl);
        }
    }
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)dbg, "&Debug");

    mp = CreatePopupMenu(); dly = CreatePopupMenu();
    AppendMenuA(mp, MF_STRING | (netplay_active() ? MF_GRAYED : 0), ID_HOST, "&Host a game...");
    AppendMenuA(mp, MF_STRING | (netplay_active() ? MF_GRAYED : 0), ID_JOIN, "&Join a game...");
    AppendMenuA(mp, MF_STRING | (netplay_active() ? 0 : MF_GRAYED), ID_DISCONNECT, "&Disconnect");
    for (i = 1; i <= 10; i++) {
        snprintf(b, sizeof b, "%d field%s%s", i, i == 1 ? "" : "s", i <= 2 ? "  (LAN)" : i >= 5 ? "  (far away)" : "");
        item(dly, ID_DELAY + i, b, opt.net_delay == i);
    }
    AppendMenuA(mp, MF_POPUP, (UINT_PTR)dly, "Input &delay when hosting");
    AppendMenuA(mp, MF_SEPARATOR, 0, NULL);
    note_item(mp, netplay_active() ? netplay_status() : "Not connected");
    note_item(mp, "Host plays player 1; the game restarts for both when you connect");
    note_item(mp, "LAN, a Tailscale address, or a forwarded TCP port all work");
    AppendMenuA(bar, MF_POPUP, (UINT_PTR)mp, "&Multiplayer");

    old = GetMenu(h);
    SetMenu(h, bar);
    if (old) DestroyMenu(old);
}

/* A one-line text prompt, built in memory so there is no resource file. */
static WCHAR g_prompt_buf[256];
static const WCHAR *g_prompt_label;

static INT_PTR CALLBACK prompt_proc(HWND d, UINT m, WPARAM w, LPARAM l)
{
    (void)l;
    switch (m) {
    case WM_INITDIALOG:
        SetDlgItemTextW(d, 10, g_prompt_label);
        SetDlgItemTextW(d, 11, g_prompt_buf);
        return TRUE;
    case WM_COMMAND:
        if (LOWORD(w) == IDOK) {
            GetDlgItemTextW(d, 11, g_prompt_buf, 256);
            EndDialog(d, 1);
            return TRUE;
        }
        if (LOWORD(w) == IDCANCEL) { EndDialog(d, 0); return TRUE; }
        break;
    }
    return FALSE;
}

static WORD *dlg_str(WORD *p, const WCHAR *s)
{
    while ((*p++ = (WORD)*s++) != 0) {}
    return p;
}

static WORD *dlg_item(WORD *p, DWORD style, short x, short y, short cx, short cy,
                      WORD id, WORD cls, const WCHAR *text)
{
    DLGITEMTEMPLATE *it;
    p = (WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    it = (DLGITEMTEMPLATE *)p;
    it->style = style | WS_CHILD | WS_VISIBLE;
    it->dwExtendedStyle = 0;
    it->x = x; it->y = y; it->cx = cx; it->cy = cy; it->id = id;
    p = (WORD *)(it + 1);
    *p++ = 0xFFFF; *p++ = cls;
    p = dlg_str(p, text);
    *p++ = 0;
    return p;
}

static int prompt(const WCHAR *title, const WCHAR *label, char *io, size_t n)
{
    static WORD buf[1024];
    DLGTEMPLATE *t = (DLGTEMPLATE *)buf;
    WORD *p;
    INT_PTR r;

    memset(buf, 0, sizeof buf);
    t->style = DS_MODALFRAME | DS_CENTER | DS_SETFONT | WS_POPUP | WS_CAPTION | WS_SYSMENU;
    t->cdit = 4;
    t->x = 0; t->y = 0; t->cx = 240; t->cy = 64;
    p = (WORD *)(t + 1);
    *p++ = 0; *p++ = 0;                     /* no menu, default class */
    p = dlg_str(p, title);
    *p++ = 9;                               /* font size */
    p = dlg_str(p, L"Segoe UI");
    p = dlg_item(p, SS_LEFT, 8, 6, 224, 10, 10, 0x0082, L"");
    p = dlg_item(p, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, 8, 20, 224, 13, 11, 0x0081, L"");
    p = dlg_item(p, BS_DEFPUSHBUTTON | WS_TABSTOP, 128, 42, 50, 14, IDOK, 0x0080, L"OK");
    (void)dlg_item(p, BS_PUSHBUTTON | WS_TABSTOP, 182, 42, 50, 14, IDCANCEL, 0x0080, L"Cancel");

    g_prompt_label = label;
    MultiByteToWideChar(CP_UTF8, 0, io, -1, g_prompt_buf, 256);
    r = DialogBoxIndirectW(GetModuleHandleW(NULL), t, hwnd(), prompt_proc);
    if (r != 1) return 0;
    WideCharToMultiByte(CP_UTF8, 0, g_prompt_buf, -1, io, (int)n, NULL, NULL);
    return 1;
}

/* Start over as a new process: the guest cannot be reset in place, and a
 * netplay session has to begin at power-on on both sides. */
static void relaunch(const char *netplay, int delay)
{
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    WCHAR *cmd = _wcsdup(GetCommandLineW());
    char d[16];

    SetEnvironmentVariableA("M3_NETPLAY", netplay && *netplay ? netplay : NULL);
    SetEnvironmentVariableA("M3_RELAUNCHED", "1");   /* ignore --host/--join now */
    snprintf(d, sizeof d, "%d", delay);
    SetEnvironmentVariableA("M3_NET_DELAY", d);
    settings_save();
    netplay_shutdown();
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    /* Battery RAM first, so the new process boots with it. */
    model3recomp_save_nvram();
    if (CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        model3recomp_quit(0);
    }
    free(cmd);
    platform_set_status("could not restart");
}

static int on_command(unsigned id)
{
    if (id == ID_MUTE) opt.mute = !opt.mute;
    else if (id >= ID_VOL && id < ID_VOL + 4) {
        static const int lv[4] = { 100, 75, 50, 25 };
        opt.volume = lv[id - ID_VOL]; opt.mute = 0;
    }
    else if (id == ID_SAVE) m3_state_request_save(opt.slot);
    else if (id == ID_LOAD) m3_state_request_load(opt.slot);
    else if (id > ID_SLOT && id <= ID_SLOT + 9) opt.slot = (int)(id - ID_SLOT);
    else if (id == ID_RESET) relaunch(NULL, opt.net_delay);
    else if (id == ID_QUIT) return 0;
    else if (id > ID_SCALE && id <= ID_SCALE + 4) { opt.scale = (int)(id - ID_SCALE); opt.fullscreen = 0; }
    else if (id == ID_FULL) opt.fullscreen = !opt.fullscreen;
    else if (id == ID_SHARP) opt.smooth = 0;
    else if (id == ID_SMOOTH) opt.smooth = 1;
    else if (id == ID_SCAN) opt.scanlines = !opt.scanlines;
    else if (id == ID_ASPECT43) opt.aspect43 = 1;
    else if (id == ID_ASPECTSQ) opt.aspect43 = 0;
    else if (id == ID_MOUSE) opt.mouse = !opt.mouse;
    else if (id >= ID_PAD && id <= ID_PAD + 2) opt.pad = (int)(id - ID_PAD);
    else if (id >= ID_CURSOR && id <= ID_CURSOR + 2) opt.cursor = (int)(id - ID_CURSOR);
    else if (id >= ID_SINDEN && id <= ID_SINDEN + 2) opt.sinden = (int)(id - ID_SINDEN);
    else if (id >= ID_CHEAT && id < ID_CHEAT + 32) {
        unsigned bit = id - ID_CHEAT;
        if (m3_cheat_is_action(bit)) m3_cheat_pulse(bit);
        else m3_cheats_set_local(m3_cheats_local() ^ (1u << bit));
    }
    else if (id >= ID_OPTION && id < ID_OPTION + 128 && !netplay_active()) {
        unsigned sl = (id - ID_OPTION) / 16u, v = (id - ID_OPTION) % 16u;
        /* A game option is the board's configuration, so it is applied the
         * way an operator would: change it and power the machine back on.
         * relaunch() saves the settings first. */
        if (m3_option_local(sl) != v) {
            m3_option_set_local(sl, v);
            relaunch(NULL, opt.net_delay);
        }
    }
    else if (id > ID_DELAY && id <= ID_DELAY + 10) opt.net_delay = (int)(id - ID_DELAY);
    else if (id == ID_DISCONNECT) { netplay_shutdown(); platform_set_status("netplay: disconnected"); }
    else if (id == ID_HOST) {
        char port[32], spec[64];
        snprintf(port, sizeof port, "%d", opt.net_port);
        if (prompt(L"Host a game", L"Port to listen on (TCP). The game restarts and waits for player 2.", port, sizeof port)) {
            opt.net_port = atoi(port);
            snprintf(spec, sizeof spec, "host:%d", opt.net_port);
            relaunch(spec, opt.net_delay);
        }
    } else if (id == ID_JOIN) {
        char addr[128], spec[160];
        snprintf(addr, sizeof addr, "%s", opt.net_join[0] ? opt.net_join : "127.0.0.1:7777");
        if (prompt(L"Join a game", L"Host address and port, e.g. 192.168.1.20:7777 or a Tailscale name.", addr, sizeof addr)) {
            if (!strchr(addr, ':')) strncat(addr, ":7777", sizeof addr - strlen(addr) - 1);
            snprintf(opt.net_join, sizeof opt.net_join, "%s", addr);
            snprintf(spec, sizeof spec, "join:%s", addr);
            relaunch(spec, opt.net_delay);
        }
    }
    settings_save();
    apply_video();
    build_menu();
    return 1;
}
#endif /* _WIN32 */

/* ---- lifecycle -------------------------------------------------------- */
int platform_init(int w, int h, const char *title)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[model3recomp] SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    settings_load();
    g_w = w; g_h = h;
    snprintf(g_title, sizeof g_title, "%s", title);
    g_win = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             w * opt.scale, h * opt.scale, SDL_WINDOW_RESIZABLE);
    if (!g_win) return 0;
    g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED);
    if (!g_ren) g_ren = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_SOFTWARE);
    if (!g_ren) return 0;
    g_tex = SDL_CreateTexture(g_ren, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, w, h);
    if (!g_tex) return 0;
    g_fb = calloc((size_t)w * h, sizeof(uint32_t));
    SDL_EventState(SDL_SYSWMEVENT, SDL_ENABLE);
#ifdef _WIN32
    build_menu();
#endif
    apply_video();
    return g_fb != NULL;
}

void platform_shutdown(void)
{
    settings_save();
    free(g_fb); g_fb = NULL;
    if (g_pad) SDL_GameControllerClose(g_pad);
    if (g_tex) SDL_DestroyTexture(g_tex);
    if (g_ren) SDL_DestroyRenderer(g_ren);
    if (g_win) SDL_DestroyWindow(g_win);
    SDL_Quit();
}

uint32_t *platform_framebuffer(int *w, int *h)
{
    if (w) *w = g_w;
    if (h) *h = g_h;
    return g_fb;
}

int platform_poll(void)
{
    SDL_Event e;
    int changed = 0;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT:
            return 0;
        case SDL_KEYDOWN:
            if (e.key.repeat) break;
            switch (e.key.keysym.sym) {
            case SDLK_ESCAPE:
                if (!opt.fullscreen) return 0;
                opt.fullscreen = 0; changed = 1; break;
            case SDLK_F11:
                opt.fullscreen = !opt.fullscreen; changed = 1; break;
            case SDLK_RETURN:
                if (e.key.keysym.mod & KMOD_ALT) { opt.fullscreen = !opt.fullscreen; changed = 1; }
                break;
            case SDLK_F5: m3_state_request_save(opt.slot); break;
            case SDLK_F7: m3_state_request_load(opt.slot); break;
            case SDLK_F9: opt.mute = !opt.mute; changed = 1;
                          platform_set_status(opt.mute ? "sound muted" : "sound on"); break;
            case SDLK_F6: {
                char b[64];
                opt.slot = opt.slot % 9 + 1;
                snprintf(b, sizeof b, "slot %d%s", opt.slot, m3_state_exists(opt.slot) ? "" : " (empty)");
                platform_set_status(b);
                changed = 1;
                break;
            }
            default: break;
            }
            break;
        case SDL_CONTROLLERDEVICEADDED:
            if (!g_pad) { g_pad = SDL_GameControllerOpen(e.cdevice.which); changed = 1; }
            break;
        case SDL_CONTROLLERDEVICEREMOVED:
            if (g_pad && e.cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                SDL_GameControllerClose(g_pad); g_pad = NULL; changed = 1;
            }
            break;
#ifdef _WIN32
        case SDL_SYSWMEVENT:
            if (e.syswm.msg->msg.win.msg == WM_COMMAND &&
                !on_command(LOWORD(e.syswm.msg->msg.win.wParam)))
                return 0;
            break;
#endif
        default: break;
        }
    }
    if (changed) {
        settings_save();
        apply_video();
#ifdef _WIN32
        build_menu();
#endif
    }
    if (g_note_until && SDL_GetTicks64() >= g_note_until) { g_note_until = 0; update_title(); }
    return 1;
}

/* ---- controls --------------------------------------------------------- */

/* Reload is a shot off the screen. The reload control does it in one go:
 * the gun reads off screen from the press for as long as it is held, and
 * never less than RELOAD_FIELDS, and the trigger is pulled for the middle
 * of that, so the game has seen the gun leave the screen before the shot
 * and sees the trigger let go before the gun comes back. */
#define RELOAD_FIELDS 8u
static unsigned reload(int player, int held)
{
    static unsigned left[2], age[2];
    unsigned b = 0;
    if (held && !left[player] && !age[player]) left[player] = RELOAD_FIELDS;
    if (!held && !left[player]) age[player] = 0;
    if (left[player] || held) {
        unsigned n = age[player]++;
        b |= player ? M3_BTN_OFFSCR2 : M3_BTN_OFFSCR1;
        if (n >= 2u && n < RELOAD_FIELDS - 2u) b |= player ? M3_BTN_TRIG2 : M3_BTN_TRIG1;
        if (left[player]) left[player]--;
    }
    return b;
}

static float g_pad_x = -1, g_pad_y = -1;    /* the gamepad's aim, framebuffer px */

static float axis(SDL_GameControllerAxis a)
{
    float v = SDL_GameControllerGetAxis(g_pad, a) / 32767.0f;
    if (v > -0.15f && v < 0.15f) return 0;
    return v;
}

void platform_sample(m3_input_t *in)
{
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    int mx, my;
    Uint32 m = SDL_GetMouseState(&mx, &my);

    if (k) {
        if (k[SDL_SCANCODE_5]) in->buttons |= M3_BTN_COIN1;
        if (k[SDL_SCANCODE_6]) in->buttons |= M3_BTN_COIN2;
        if (k[SDL_SCANCODE_1]) in->buttons |= M3_BTN_START1;
        if (k[SDL_SCANCODE_2]) in->buttons |= M3_BTN_START2;
        if (k[SDL_SCANCODE_F2]) in->buttons |= M3_BTN_TEST;
        if (k[SDL_SCANCODE_F3]) in->buttons |= M3_BTN_SERVICE;
        if (k[SDL_SCANCODE_SPACE] || k[SDL_SCANCODE_LCTRL]) in->buttons |= M3_BTN_TRIG1;
        if (k[SDL_SCANCODE_RCTRL]) in->buttons |= M3_BTN_TRIG2;
    }

    if (opt.mouse) {
        SDL_Rect r = view_rect();
        int px = (int)((double)(mx - r.x) * g_w / (r.w > 0 ? r.w : 1));
        int py = (int)((double)(my - r.y) * g_h / (r.h > 0 ? r.h : 1));
        m3_gun_from_screen(px, py, g_w, g_h, &in->gun_x[0], &in->gun_y[0]);
        if (m & SDL_BUTTON(SDL_BUTTON_LEFT)) in->buttons |= M3_BTN_TRIG1;
    }
    in->buttons |= reload(0, (opt.mouse && (m & SDL_BUTTON(SDL_BUTTON_RIGHT))) ||
                             (k && k[SDL_SCANCODE_LSHIFT]));

    if (g_pad && opt.pad) {
        int p = opt.pad - 1;
        float speed = 6.0f;                 /* pixels a field at full tilt */
        if (g_pad_x < 0) { g_pad_x = g_w / 2.0f; g_pad_y = g_h / 2.0f; }
        g_pad_x += axis(SDL_CONTROLLER_AXIS_LEFTX) * speed;
        g_pad_y += axis(SDL_CONTROLLER_AXIS_LEFTY) * speed;
        if (g_pad_x < 0) g_pad_x = 0;
        if (g_pad_y < 0) g_pad_y = 0;
        if (g_pad_x > g_w - 1) g_pad_x = (float)(g_w - 1);
        if (g_pad_y > g_h - 1) g_pad_y = (float)(g_h - 1);
        m3_gun_from_screen((int)g_pad_x, (int)g_pad_y, g_w, g_h, &in->gun_x[p], &in->gun_y[p]);
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_A) ||
            axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 0.5f)
            in->buttons |= p ? M3_BTN_TRIG2 : M3_BTN_TRIG1;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START))
            in->buttons |= p ? M3_BTN_START2 : M3_BTN_START1;
        if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK))
            in->buttons |= p ? M3_BTN_COIN2 : M3_BTN_COIN1;
        in->buttons |= reload(p, SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_B) ||
                                 axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 0.5f);
    }
}

/* ---- presenting ------------------------------------------------------- */
static void crosshair(const SDL_Rect *r, int gx, int gy, Uint8 cr, Uint8 cg, Uint8 cb)
{
    int x = r->x + (gx - M3_GUN_X0) * r->w / (M3_GUN_X1 - M3_GUN_X0);
    int y = r->y + (gy - M3_GUN_Y0) * r->h / (M3_GUN_Y1 - M3_GUN_Y0);
    int s = r->h / 40 + 4, i;
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 160);
    for (i = -1; i <= 1; i += 2) {
        SDL_RenderDrawLine(g_ren, x - s, y + i, x + s, y + i);
        SDL_RenderDrawLine(g_ren, x + i, y - s, x + i, y + s);
    }
    SDL_SetRenderDrawColor(g_ren, cr, cg, cb, 255);
    SDL_RenderDrawLine(g_ren, x - s, y, x - 2, y);
    SDL_RenderDrawLine(g_ren, x + 2, y, x + s, y);
    SDL_RenderDrawLine(g_ren, x, y - s, x, y - 2);
    SDL_RenderDrawLine(g_ren, x, y + 2, x, y + s);
}

/* Hold the display to the board's 57.524 Hz. Fields are paced by guest
 * work, not the clock, so without this the game runs as fast as the host
 * can draw it. M3_NOTHROTTLE turns it off for scripted captures; holding
 * Tab does the same for a moment. */
static SDL_AudioDeviceID g_audio;
#define AUDIO_BYTES_PER_MS 176u          /* 44.1 kHz, stereo, 16-bit */
#define AUDIO_TARGET_MS    100u

static void throttle(void)
{
    static int probed, off;
    static uint64_t next;
    const Uint8 *k = SDL_GetKeyboardState(NULL);
    uint64_t now;
    if (!probed) { probed = 1; off = getenv("M3_NOTHROTTLE") != NULL; }
    if (off || (k && k[SDL_SCANCODE_TAB])) { next = 0; return; }
    now = platform_ticks_us();
    if (!next || now > next + 100000u)  /* first field, or fell far behind */
        next = now;
    /* 1e6 / 57.524 us a field, nudged by up to 2% to hold the audio queue
     * at its target: the sound card's clock and this one never quite
     * agree, and a queue left to drift either runs dry or lags. */
    {
        int64_t period = 17384;
        if (g_audio && SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_PLAYING) {
            int64_t q = (int64_t)(SDL_GetQueuedAudioSize(g_audio) / AUDIO_BYTES_PER_MS);
            int64_t adj = (q - (int64_t)AUDIO_TARGET_MS) * 17384 / 2000;   /* 1% per 20 ms off */
            if (adj > 348) adj = 348;
            if (adj < -348) adj = -348;
            period += adj;
        }
        next += (uint64_t)period;
    }
    while ((now = platform_ticks_us()) < next) {
        if (next - now > 2000u)
            SDL_Delay((Uint32)((next - now) / 1000u - 1u));
    }
}

void platform_present(void)
{
    SDL_Rect r = view_rect();
    const m3_input_t *in = m3_input();

    SDL_UpdateTexture(g_tex, NULL, g_fb, g_w * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 255);
    SDL_RenderClear(g_ren);
    SDL_RenderCopy(g_ren, g_tex, NULL, &r);
    SDL_SetRenderDrawBlendMode(g_ren, SDL_BLENDMODE_BLEND);

    /* Scanlines: darken the lower part of every source line, once each
     * line is at least two pixels tall on screen. */
    if (opt.scanlines && r.h >= g_h * 2) {
        int y;
        SDL_SetRenderDrawColor(g_ren, 0, 0, 0, 96);
        for (y = 0; y < g_h; y++) {
            SDL_Rect s;
            int y0 = r.y + y * r.h / g_h, y1 = r.y + (y + 1) * r.h / g_h;
            s.x = r.x; s.w = r.w;
            s.h = (y1 - y0) / 2; s.y = y1 - s.h;
            SDL_RenderFillRect(g_ren, &s);
        }
    }

    /* A Sinden gun finds the screen by its white border. */
    if (opt.sinden) {
        int t = r.h * (opt.sinden == 1 ? 15 : 30) / 1000 + 1;
        SDL_Rect s[4];
        s[0].x = r.x; s[0].y = r.y; s[0].w = r.w; s[0].h = t;
        s[1].x = r.x; s[1].y = r.y + r.h - t; s[1].w = r.w; s[1].h = t;
        s[2].x = r.x; s[2].y = r.y; s[2].w = t; s[2].h = r.h;
        s[3].x = r.x + r.w - t; s[3].y = r.y; s[3].w = t; s[3].h = r.h;
        SDL_SetRenderDrawColor(g_ren, 255, 255, 255, 255);
        SDL_RenderFillRects(g_ren, s, 4);
    }

    /* Crosshairs: the cabinet had none, so they are an option for the
     * mouse, and always there for a gamepad or a netplay partner, who
     * have nothing else to aim by. */
    if (opt.cursor == CUR_CROSSHAIR || (g_pad && opt.pad == 1))
        crosshair(&r, in->gun_x[0], in->gun_y[0], 255, 64, 64);
    if ((g_pad && opt.pad == 2) || netplay_active())
        crosshair(&r, in->gun_x[1], in->gun_y[1], 64, 160, 255);

    SDL_RenderPresent(g_ren);
    throttle();
}

/* ---- audio -------------------------------------------------------------
 *
 * The sound board hands over a field's samples at a time, made in step with
 * the guest, not pulled by the audio device -- so the device is fed a queue.
 * Running at 57.524 Hz the queue holds steady; a little latency is kept in
 * hand (100 ms, held there by the frame limiter), anything beyond 300 ms
 * (running flat out) is dropped rather than heard late, and a queue that
 * runs dry pauses to refill rather than crackling.
 */
static int g_audio_failed;
static unsigned g_underruns, g_audio_fields;

void platform_audio(const int16_t *lr, int frames)
{
    static int16_t tmp[2 * 1024];
    Uint32 queued;
    int i, gain;

    if (g_audio_failed || frames <= 0 || frames > 1024) return;
    if (!g_audio) {
        SDL_AudioSpec want, have;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { g_audio_failed = 1; return; }
        memset(&want, 0, sizeof want);
        want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
        g_audio = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
        if (!g_audio) {
            fprintf(stderr, "[model3recomp] no audio device: %s\n", SDL_GetError());
            g_audio_failed = 1;
            return;
        }
    }
    queued = SDL_GetQueuedAudioSize(g_audio);
    if (queued > AUDIO_BYTES_PER_MS * 300u)     /* over 300 ms behind: drop */
        return;
    /* Ran dry (the host stalled): stop and refill rather than crackle. */
    if (queued < AUDIO_BYTES_PER_MS * 4u &&
        SDL_GetAudioDeviceStatus(g_audio) == SDL_AUDIO_PLAYING) {
        SDL_PauseAudioDevice(g_audio, 1);
        g_underruns++;
    }
    /* M3_AUDIO_TRACE: the queue's depth once a second, and how often it
     * ran dry -- the measure of whether the host keeps up. */
    if (getenv("M3_AUDIO_TRACE") && ++g_audio_fields % 58 == 0)
        fprintf(stderr, "[audio] queued %u ms  underruns %u\n",
                (unsigned)(queued / AUDIO_BYTES_PER_MS), g_underruns);
    gain = opt.mute ? 0 : opt.volume * 256 / 100;
    for (i = 0; i < 2 * frames; i++) tmp[i] = (int16_t)((lr[i] * gain) >> 8);
    SDL_QueueAudio(g_audio, tmp, (Uint32)(frames * 4));
    /* Play once the target is in hand, so a hitch has room to hide in. */
    if (SDL_GetAudioDeviceStatus(g_audio) != SDL_AUDIO_PLAYING &&
        SDL_GetQueuedAudioSize(g_audio) >= AUDIO_BYTES_PER_MS * AUDIO_TARGET_MS)
        SDL_PauseAudioDevice(g_audio, 0);
}

uint64_t platform_ticks_us(void)
{
    /* SDL_GetPerformanceCounter, not SDL_GetTicks: a 17.4 ms field needs
     * better than millisecond resolution or the pacing judders. */
    static uint64_t freq;
    if (!freq) freq = SDL_GetPerformanceFrequency();
    return SDL_GetPerformanceCounter() * 1000000ull / freq;
}
