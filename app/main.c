#include "app.h"

PSP_MODULE_INFO("KodiRemote", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_HEAP_SIZE_KB(4096);

int main(int argc, char *argv[]) {
    SceCtrlData pad;
    int online;

    setup_callbacks();
    scePowerSetClockFrequency(111, 111, 55);          /* a remote does not need full speed: saves battery */
    pspDebugScreenInit();
    dprintf("Kodi Remote for PSP  v%s\n\n", APP_VERSION_STR);
    load_config(argc > 0 ? argv[0] : NULL);
    { char p[300]; snprintf(p, sizeof(p), "%skodiremote.log", g_dir); g_log = fopen(p, "w"); }
    g_direct = (g_mode_cfg == 2) || (g_mode_cfg == 0 && !g_token[0]);      /* a config with a token and no mode= line stays in server mode */
    if (g_direct) {
        g_ssid[0] = 0;                                                       /* the server-mode defaults are not Kodi's */
        online = 1;
    } else {
    dprintf("target %s:%d  token %s\n", g_host, g_port, g_token[0] ? "set" : "MISSING");
    online = (net_up() == 0);
    if (!online) wifi_help();
    if (!online || !g_token[0]) { dprintf("\nCannot start: %s. Press HOME to exit.\n", online ? "token missing in config.txt" : "no Wi-Fi"); while (1) sceKernelDelayThread(500000); }
    }

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    app_gu_init();
    intraFontInit();
    const char *fonts[] = { "flash0:/font/ltn8.pgf", "flash0:/font/ltn0.pgf", "flash0:/font/ltn4.pgf", NULL };
    int fi;
    for (fi = 0; fonts[fi] && !g_font; fi++) g_font = intraFontLoad(fonts[fi], INTRAFONT_CACHE_ALL);
    if (!g_font) {
        sceGuTerm();
        pspDebugScreenInit();
        dprintf("Font load failed (flash0:/font). Press HOME to exit.\n");
        logf_("%s", "font load failed");
        while (1) sceKernelDelayThread(500000);
    }
    logf_("font %s", fonts[fi - 1]);

    if (g_direct) direct_boot();
    int th = sceKernelCreateThread("poll", poll_thread, 0x20, 0x10000, 0, 0);
    if (th >= 0) sceKernelStartThread(th, 0, 0);
    int th2 = sceKernelCreateThread("worker", worker_thread, 0x20, 0x10000, 0, 0);
    if (th2 >= 0) sceKernelStartThread(th2, 0, 0);
    open_main();
    toast(C_DIM, "Connected");

    while (1) {
        if (g_load_ready) { if (g_screen == g_load_screen - 1) parse_lines(g_screen, g_load_code); g_load_ready = 0; }
        if (g_queue_dirty) {                                   /* reload the open queue screen without blanking it, keeping the cursor */
            g_queue_dirty = 0;
            if (g_screen == S_QUEUE && !g_grab && g_want_sel < 0) { g_want_sel = g_sel; job_push("GET", "/t/queue", 20, "load", S_QUEUE + 1); }
        }
        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned bt = pad.Buttons;
        int up = pressed(bt, PSP_CTRL_UP, 0, 1), dn = pressed(bt, PSP_CTRL_DOWN, 1, 1);
        int lf = pressed(bt, PSP_CTRL_LEFT, 2, 1), rt = pressed(bt, PSP_CTRL_RIGHT, 3, 1);
        int x = pressed(bt, PSP_CTRL_CROSS, 4, 0), o = pressed(bt, PSP_CTRL_CIRCLE, 5, 0);
        int tri = pressed(bt, PSP_CTRL_TRIANGLE, 6, 0);
        int back = pressed(bt, PSP_CTRL_SELECT, 11, 0), choose = pressed(bt, PSP_CTRL_START, 10, 0);   /* START chooses, SELECT backs out */
        /* the analog stick moves inside the PSP menus (up/down one row, left/right one page) */
        int su = pressed_on(pad.Ly < 70, 12, 1), sd = pressed_on(pad.Ly > 186, 13, 1);
        int sl = pressed_on(pad.Lx < 70, 14, 1), sr = pressed_on(pad.Lx > 186, 15, 1);

        if (g_debug) snprintf(g_dbg, sizeof(g_dbg), "stick %d,%d  btn %04x  row %d  m%d  job%d", pad.Lx, pad.Ly, bt & 0xFFFF, g_sel, g_menu_mode, (g_jt - g_jh + NJOBS) % NJOBS);
        /* L+R together toggles Menu mode. A single L or R is volume, fired after 3 frames so a chord does not change the volume first. */
        static int chord_prev, lcnt, rcnt;
        int lh = (bt & PSP_CTRL_LTRIGGER) != 0, rh = (bt & PSP_CTRL_RTRIGGER) != 0, chord = lh && rh;
        if (chord && !chord_prev) { g_menu_mode = !g_menu_mode; toast(C_ACC, g_menu_mode ? "Menu mode: d-pad, X, O drive this list" : "Kodi mode: d-pad, X, O drive the TV"); }
        chord_prev = chord;
        lcnt = (lh && !rh) ? lcnt + 1 : 0;
        rcnt = (rh && !lh) ? rcnt + 1 : 0;
        int lfire = (lcnt == 3) || (lcnt > 25 && lcnt % 6 == 0), rfire = (rcnt == 3) || (rcnt > 25 && rcnt % 6 == 0);
        if (lfire) act("POST", "/t/do/voldown", 8, "Volume down");
        if (rfire) act("POST", "/t/do/volup", 8, "Volume up");
        if (tri) act("POST", "/t/do/playpause", 8, "Play / pause");
        if (bt & PSP_CTRL_SQUARE) {                   /* tap = Kodi info (on release), hold half a second = fullscreen video */
            g_sq_cnt++;
            if (g_sq_cnt == 30 && !g_sq_fired) { g_sq_fired = 1; act("POST", "/t/do/fullscreen", 8, "Fullscreen video"); }
        } else {
            if (g_sq_cnt > 0 && !g_sq_fired) act("POST", "/t/key/info", 8, "Kodi: info");
            g_sq_cnt = 0; g_sq_fired = 0;
        }

        /* Kodi mode (default): d-pad arrows, X = OK, O = back go to the TV. */
        if (!g_menu_mode) {
            if (up) act("POST", "/t/key/up", 8, "Kodi: up");
            if (dn) act("POST", "/t/key/down", 8, "Kodi: down");
            if (lf) act("POST", "/t/key/left", 8, "Kodi: left");
            if (rt) act("POST", "/t/key/right", 8, "Kodi: right");
            if (x) act("POST", "/t/key/select", 8, "Kodi: OK");
            if (o) act("POST", "/t/key/back", 8, "Kodi: back");
        }

        /* PSP menu: the stick always moves; START / SELECT choose / go back; in Menu mode d-pad, X and O do the same. */
        int mup = su || (g_menu_mode && up), mdn = sd || (g_menu_mode && dn);
        int mlf = sl || (g_menu_mode && lf), mrt = sr || (g_menu_mode && rt);
        int mchoose = choose || (g_menu_mode && x), mback = back || (g_menu_mode && o);
        if (g_grab && g_screen == S_QUEUE && g_n > 0) {           /* move the item under the cursor, one place per stick push */
            int dir = mup ? -1 : (mdn ? 1 : 0);
            if (dir && g_sel + dir >= 0 && g_sel + dir < g_n) {
                char mp[64];
                snprintf(mp, sizeof(mp), "/t/q/move/%d/%d", g_sel, g_sel + dir);
                if (act("POST", mp, 20, "Moving")) {
                    Item a = g_items[g_sel], b = g_items[g_sel + dir];
                    strcpy(g_items[g_sel].label, b.label); g_items[g_sel].flag = b.flag;       /* swap what is shown; the numbers stay */
                    strcpy(g_items[g_sel + dir].label, a.label); g_items[g_sel + dir].flag = a.flag;
                    g_sel += dir;
                    if (g_sel < g_scroll) g_scroll = g_sel;
                    if (g_sel >= g_scroll + 8) g_scroll = g_sel - 7;
                }
            }
            mup = mdn = 0; mlf = mrt = 0;
        }
        if (g_screen != S_REMOTE && g_n > 0) {
            if (mup && g_sel > 0) g_sel--;
            if (mdn && g_sel < g_n - 1) g_sel++;
            if (mlf) g_sel -= 8;
            if (mrt) g_sel += 8;
            if (g_sel < 0) g_sel = 0;
            if (g_sel > g_n - 1) g_sel = g_n - 1;
            if (g_sel < g_scroll) g_scroll = g_sel;
            if (g_sel >= g_scroll + 8) g_scroll = g_sel - 7;
        }
        if (mchoose && g_screen != S_REMOTE) select_item();
        if (mback && g_grab) { g_grab = 0; g_want_sel = g_sel; load_lines(S_QUEUE, "/t/queue", "Queue"); toast(C_OK, "Dropped"); }
        else if (mback && g_screen == S_QACT) { g_want_sel = g_qpos; load_lines(S_QUEUE, "/t/queue", "Queue"); }
        else if (mback && g_screen != S_MAIN) open_main();
        draw_frame();
    }
    return 0;
}
