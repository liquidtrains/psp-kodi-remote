#include "app.h"

unsigned int __attribute__((aligned(16))) g_list[262144];

intraFont *g_font;

void app_gu_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, g_list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)0x88000, BUF_W);
    sceGuDepthBuffer((void *)0x110000, BUF_W);
    sceGuOffset(2048 - (SCR_W / 2), 2048 - (SCR_H / 2));
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuDepthRange(65535, 0);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuFrontFace(GU_CW);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

void rect(float x, float y, float w, float h, unsigned int c) {
    Vtx *v = (Vtx *)sceGuGetMemory(2 * sizeof(Vtx));
    v[0].c = c; v[0].x = x; v[0].y = y; v[0].z = 0;
    v[1].c = c; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    /* intraFont leaves its own GU state behind after printing: put everything a flat coloured sprite needs back */
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void text(float x, float y, float size, unsigned int col, const char *s) {
    if (!g_font) return;
    sceGuEnable(GU_TEXTURE_2D);
    intraFontSetStyle(g_font, size, col, 0, 0.f, 0);
    intraFontPrint(g_font, x, y, s);
}

void text_fit(float x, float y, float size, unsigned int col, const char *s, float maxw) {
    char buf[160];
    strncpy(buf, s, sizeof(buf) - 4); buf[sizeof(buf) - 4] = 0;
    if (g_font) {
        intraFontSetStyle(g_font, size, col, 0, 0.f, 0);
        int n = strlen(buf);
        while (n > 1 && intraFontMeasureText(g_font, buf) > maxw) { n--; buf[n] = 0; if (n > 3) { buf[n - 1] = '.'; buf[n - 2] = '.'; } }
    }
    text(x, y, size, col, buf);
}

char g_toast[120] = "";

unsigned int g_toast_col;

int g_toast_frames;

const char *g_title = "Kodi Remote";

char g_dbg[64] = "";

void toast(unsigned int col, const char *msg) { strncpy(g_toast, msg, sizeof(g_toast) - 1); g_toast_col = col; g_toast_frames = 200; }

void draw_frame(void) {
    Stat s = g_stat[g_cur];
    char b[160];
    int i;
    g_frame++;
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(C_BG);
    sceGuClear(GU_COLOR_BUFFER_BIT);

    /* header */
    rect(0, 0, SCR_W, 30, C_BAR);
    rect(0, 30, SCR_W, 2, C_ACC);
    text_fit(10, 21, 1.0f, C_TEXT, g_title, 155);                  /* long queue titles must not run into the badge */
    text(170, 21, 0.8f, g_menu_mode ? C_ACC : C_DIM, g_menu_mode ? "[MENU]" : "[KODI]");
    if (g_screen != S_REMOTE && g_n > 0) { char pos[24]; snprintf(pos, sizeof(pos), "%d / %d", g_sel + 1, g_n); text(235, 21, 0.8f, RGB(255, 214, 64), pos); }
    if (s.ok) {
        snprintf(b, sizeof(b), s.muted ? "MUTE" : "VOL %d", s.vol);
        text(SCR_W - 150, 21, 0.85f, C_DIM, b);
        rect(SCR_W - 90, 12, 76, 8, RGB(52, 58, 80));
        if (s.vol > 0 && !s.muted) rect(SCR_W - 90, 12, 76.f * (s.vol > 100 ? 100 : s.vol) / 100.f, 8, C_ACC);
    } else {
        text(SCR_W - 200, 21, 0.8f, C_ERR, g_reconnecting ? "Wi-Fi reconnecting..." : (g_direct ? "no link to Kodi" : "no link to server"));
    }

    /* body */
    if (g_screen == S_REMOTE) {
        text(12, 54, 0.9f, C_ACC, "On the TV (Kodi), always:");
        text(12, 74, 0.8f, C_TEXT, "D-pad   arrows      X   OK      O   back");
        text(12, 92, 0.8f, C_TEXT, "SQUARE  info (hold: fullscreen)     TRIANGLE  play / pause");
        text(12, 110, 0.8f, C_TEXT, "L / R   volume down / up");
        text(12, 136, 0.9f, C_ACC, "On this screen (the PSP menus):");
        text(12, 156, 0.8f, C_TEXT, "Analog stick   up / down = move,  left / right = page");
        text(12, 174, 0.8f, C_TEXT, "START    choose        SELECT   back to main menu");
        text(12, 192, 0.8f, C_ACC, "L + R together: [MENU] mode, d-pad / X / O drive the list");
        text(12, 214, 0.75f, C_DIM, "Kodi Remote v" APP_VERSION_STR);
    } else {
        const int rows = 8, rh = 24, y0 = 38;
        if (g_n == 0) text(16, 80, 0.9f, C_DIM, "(nothing here)");
        for (i = 0; i < rows; i++) {
            int idx = g_scroll + i;
            if (idx >= g_n) break;
            float y = (float)(y0 + i * rh);
            rect(6, y, SCR_W - 12, rh - 2, idx == g_sel ? C_SEL : C_ROW);
            Item *it = &g_items[idx];
            float x = 16;
            if (g_screen == S_LIVE) {
                rect(12, y + 8, 8, 8, it->flag == 1 ? C_LIVE : RGB(70, 76, 96));
                x = 28;
                snprintf(b, sizeof(b), "%s", it->tag[0] == 't' ? "TW" : "KK");
                text(SCR_W - 52, y + 17, 0.75f, it->flag == 1 ? C_LIVE : C_DIM, it->flag == 1 ? "LIVE" : b);
                if (it->sub[0]) text_fit(178, y + 17, 0.7f, idx == g_sel ? RGB(220, 226, 240) : C_DIM, it->sub, SCR_W - 178 - 58);   /* stream title */
            }
            if ((g_screen == S_QUEUE || g_screen == S_TRACKS) && it->flag == 1) { rect(12, y + 8, 8, 8, C_OK); x = 28; }
            /* selection is shown with TEXT (yellow, larger, ">" marker) so it stays visible even if the row bars do not draw */
            int red = g_screen == S_POWER && it->flag == 2;                 /* shutdown and reboot are drawn red */
            if (idx == g_sel) text(3, y + 18, 1.0f, red ? RGB(255, 130, 120) : RGB(255, 214, 64), ">");
            text_fit(x + (idx == g_sel ? 4 : 0), y + 17, idx == g_sel ? 1.0f : 0.85f,
                     red ? (idx == g_sel ? RGB(255, 130, 120) : C_ERR) : (idx == g_sel ? RGB(255, 214, 64) : (g_screen == S_LIVE && it->flag != 1 ? C_DIM : RGB(176, 184, 204))),
                     it->label, (g_screen == S_LIVE && it->sub[0]) ? 140 : SCR_W - x - 70);
        }
        if (g_n > rows) {
            float th = (float)(rows * rh) * rows / g_n;
            rect(SCR_W - 5, y0 + (float)(rows * rh - th) * g_scroll / (g_n - rows), 3, th, C_ACC);
        }
    }

    /* toast */
    if (g_toast_frames > 0) {
        rect(0, 196, SCR_W, 26, RGB(10, 12, 18));
        rect(0, 196, 4, 26, g_toast_col);
        text_fit(12, 214, 0.85f, g_toast_col, g_toast, SCR_W - 24);
        g_toast_frames--;
    }

    /* now playing strip */
    rect(0, 224, SCR_W, 48, C_BAR);
    if (s.ok && s.playing) {
        text_fit(10, 243, 0.9f, C_TEXT, s.title[0] ? s.title : "(playing)", SCR_W - 90);
        text(SCR_W - 70, 243, 0.8f, s.paused ? C_DIM : C_OK, s.paused ? "PAUSED" : "PLAYING");
        rect(10, 254, SCR_W - 20, 6, RGB(52, 58, 80));
        if (s.pct > 0) rect(10, 254, (SCR_W - 20) * (s.pct > 100 ? 100 : s.pct) / 100.f, 6, C_ACC);
    } else if (s.ok) {
        text(10, 247, 0.9f, C_DIM, "Nothing playing");
    } else {
        text_fit(10, 247, 0.8f, C_ERR, s.err[0] ? s.err : "connecting...", SCR_W - 20);
    }
    text(SCR_W - 250, 270, 0.6f, C_DIM, g_dbg);        /* diagnostic: screen, selected row, buttons */
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

/* ---------------------------------------------------------------- Kodi-alone mode: Wi-Fi prompt, scan, login */
void dlg_begin(void) {
    g_frame++;
    sceGuStart(GU_DIRECT, g_list);
    sceGuClearColor(C_BG);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    rect(0, 0, SCR_W, 30, C_BAR);
    rect(0, 30, SCR_W, 2, C_ACC);
    text(SCR_W - 150, 21, 0.7f, C_DIM, "v" APP_VERSION_STR);          /* version and build, top right of every setup screen */
}

void dlg_end(void) { sceGuFinish(); sceGuSync(0, 0); sceDisplayWaitVblankStart(); sceGuSwapBuffers(); }

/* a page of text lines; sel >= 0 turns it into a list with a ">" marker on that line */
void dlg_page(const char *title, const char *const *lines, int n, const char *footer, int sel) {
    int i;
    dlg_begin();
    text(10, 21, 1.0f, C_TEXT, title);
    for (i = 0; i < n; i++) {
        float y = 54 + i * 20;
        if (sel >= 0) {
            if (i == sel) { text(8, y, 0.95f, RGB(255, 214, 64), ">"); text(26, y, 0.95f, RGB(255, 214, 64), lines[i]); }
            else text(26, y, 0.85f, RGB(176, 184, 204), lines[i]);
        } else text(14, y, 0.82f, C_TEXT, lines[i]);
    }
    if (footer) { rect(0, 240, SCR_W, 32, C_BAR); text(10, 261, 0.85f, C_ACC, footer); }
    dlg_end();
}

unsigned pad_now(void) { SceCtrlData p; sceCtrlPeekBufferPositive(&p, 1); return p.Buttons; }

/* returns 1 when a button in `yes` is pressed, 0 for one in `no`; buttons already held on entry are ignored */
int dlg_choice(const char *title, const char *const *lines, int n, const char *footer, unsigned yes, unsigned no) {
    unsigned prev = pad_now(), bt, fresh;
    for (;;) {
        dlg_page(title, lines, n, footer, -1);
        bt = pad_now(); fresh = bt & ~prev; prev = bt;
        if (fresh & yes) return 1;
        if (fresh & no) return 0;
        scePowerTick(PSP_POWER_TICK_ALL);
    }
}

void dlg_exit(const char *title, const char *const *lines, int n) {
    dlg_choice(title, lines, n, "X  Close the app", PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
    sceKernelExitGame();
    for (;;) sceKernelDelayThread(500000);
}

/* pick a line: returns its index, -1 for CIRCLE, -2 for SQUARE */
int dlg_pick(const char *title, const char *const *lines, int n, const char *footer) {
    unsigned prev = pad_now(), bt, fresh;
    int sel = 0;
    for (;;) {
        dlg_page(title, lines, n, footer, sel);
        bt = pad_now(); fresh = bt & ~prev; prev = bt;
        if ((fresh & PSP_CTRL_DOWN) && sel < n - 1) sel++;
        if ((fresh & PSP_CTRL_UP) && sel > 0) sel--;
        if (fresh & (PSP_CTRL_CROSS | PSP_CTRL_START)) return sel;
        if (fresh & PSP_CTRL_CIRCLE) return -1;
        if (fresh & PSP_CTRL_SQUARE) return -2;
        scePowerTick(PSP_POWER_TICK_ALL);
    }
}

void dlg_common(pspUtilityDialogCommon *c, int size) {
    memset(c, 0, size);
    c->size = size;
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_LANGUAGE, &c->language);
    sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_BUTTON_SWAP, &c->buttonSwap);
    c->graphicsThread = 17; c->accessThread = 19; c->fontThread = 18; c->soundThread = 16;
}

/* the PSP's own on-screen keyboard. Returns 1 with text in out, 0 if cancelled, -1 if it would not start */
int osk_input(const char *desc, const char *init, char *out, int outsz) {
    static unsigned short intext[64], outtext[64], dtext[64];
    SceUtilityOskData data;
    SceUtilityOskParams p;
    int i, done = 0;
    memset(intext, 0, sizeof(intext)); memset(outtext, 0, sizeof(outtext)); memset(dtext, 0, sizeof(dtext));
    for (i = 0; desc[i] && i < 62; i++) dtext[i] = (unsigned char)desc[i];
    for (i = 0; init[i] && i < 62; i++) intext[i] = (unsigned char)init[i];
    memset(&data, 0, sizeof(data));
    data.language = PSP_UTILITY_OSK_LANGUAGE_DEFAULT;
    data.inputtype = PSP_UTILITY_OSK_INPUTTYPE_ALL;
    data.lines = 1; data.unk_24 = 1;
    data.desc = dtext; data.intext = intext;
    data.outtextlength = 64; data.outtextlimit = 62; data.outtext = outtext;
    memset(&p, 0, sizeof(p));
    dlg_common(&p.base, sizeof(p));
    p.datacount = 1; p.data = &data;
    if (sceUtilityOskInitStart(&p) < 0) return -1;
    while (!done) {
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(C_BG);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFinish(); sceGuSync(0, 0);
        switch (sceUtilityOskGetStatus()) {
        case PSP_UTILITY_DIALOG_VISIBLE: sceUtilityOskUpdate(1); break;
        case PSP_UTILITY_DIALOG_QUIT: sceUtilityOskShutdownStart(); break;
        case PSP_UTILITY_DIALOG_NONE: done = 1; break;
        default: break;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        scePowerTick(PSP_POWER_TICK_ALL);
    }
    if (data.result == PSP_UTILITY_OSK_RESULT_CANCELLED) return 0;
    for (i = 0; outtext[i] && i < outsz - 1; i++) out[i] = outtext[i] < 128 ? (char)outtext[i] : '?';
    out[i] = 0;
    return 1;
}

/* the PSP's own "pick a Wi-Fi network" dialog */
int run_netconf(void) {
    pspUtilityNetconfData nc;
    struct pspUtilityNetconfAdhoc ad;
    int done = 0;
    memset(&ad, 0, sizeof(ad));
    dlg_common(&nc.base, sizeof(nc));
    nc.action = PSP_NETCONF_ACTION_CONNECTAP;
    nc.adhocparam = &ad; nc.hotspot = 0; nc.hotspot_connected = 0; nc.wifisp = 0;
    if (sceUtilityNetconfInitStart(&nc) < 0) return -1;
    while (!done) {
        sceGuStart(GU_DIRECT, g_list);
        sceGuClearColor(C_BG);
        sceGuClearDepth(0);
        sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
        sceGuFinish(); sceGuSync(0, 0);
        switch (sceUtilityNetconfGetStatus()) {
        case PSP_UTILITY_DIALOG_VISIBLE: sceUtilityNetconfUpdate(1); break;
        case PSP_UTILITY_DIALOG_QUIT: sceUtilityNetconfShutdownStart(); break;
        case PSP_UTILITY_DIALOG_NONE: done = 1; break;
        default: break;
        }
        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        scePowerTick(PSP_POWER_TICK_ALL);
    }
    return 0;
}
