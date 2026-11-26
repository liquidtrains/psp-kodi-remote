#include "app.h"

Item g_items[128];

int g_n, g_sel, g_scroll, g_screen = S_MAIN;
char g_live_keep[72];

void add(const char *label, const char *tag, int id, int flag) {
    if (g_n >= 128) return;
    strncpy(g_items[g_n].label, label, 71); g_items[g_n].label[71] = 0;
    strncpy(g_items[g_n].tag, tag, 11); g_items[g_n].tag[11] = 0;
    g_items[g_n].sub[0] = 0; g_items[g_n].id = id; g_items[g_n].flag = flag; g_n++;
}

void menu_set(int screen, const char *title) { g_screen = screen; g_title = title; g_n = 0; g_sel = 0; g_scroll = 0; }

void load_lines(int screen, const char *path, const char *title) {
    menu_set(screen, title);
    toast(C_DIM, "Loading...");
    job_push("GET", path, 20, "load", screen + 1);
}

static void builtin_shows(void) {                 /* used when the server has no show list to give */
    add("Bob's Burgers - random", "bobs", 0, 0);
    add("House - random", "house", 1, 0);
    add("King of the Hill - random", "koth", 2, 0);
    add("Rick and Morty - random", "rick", 3, 0);
    add("Firefly - in order", "firefly", 4, 0);
}

void parse_lines(int screen, int code) {
    char *p;
    if (screen == S_SHOWS && code != 200) { g_n = 0; builtin_shows(); toast(C_DIM, "Built-in show list"); return; }
    if (code != 200) { toast(C_ERR, (code && !g_wbuf[0]) ? "API error" : g_wbuf); g_toast_frames = 300; return; }
    g_toast_frames = 0;
    if (screen == S_QUEUE || screen == S_TRACKS) g_n = 0;      /* a refresh re-reads the whole list in place */
    g_n = 0;                                        /* every list is read whole; a second reply for the same screen must not append (the list once doubled)  */
    p = g_wbuf;
    while (*p) {
        char *eol = strchr(p, '\n');
        if (eol) *eol = 0;
        if (screen == S_LIVE) {                       /* platform|name|live */
            char *a = p, *b = strchr(a, '|'), *c = b ? strchr(b + 1, '|') : NULL;
            if (b && c) {
                char *t = strchr(c + 1, '|'), *cat = t ? strchr(t + 1, '|') : NULL;
                *b = 0; *c = 0; if (t) *t = 0; if (cat) *cat = 0;
                add(b + 1, a, 0, c[1] == '1' ? 1 : 0);
                if (t) {                                       /* "category: title", the category cut short so the title still shows */
                    int has_cat = cat && cat[1];
                    snprintf(g_items[g_n - 1].sub, sizeof(g_items[0].sub), "%.14s%s%s", has_cat ? cat + 1 : "", has_cat && t[1] ? ": " : "", t + 1);
                }
            }
        } else if (screen == S_LIB) {                 /* kind|id|label|flag|extra */
            char *f[5] = { p, NULL, NULL, NULL, NULL };
            int k;
            for (k = 1; k < 5; k++) { char *b = f[k - 1] ? strchr(f[k - 1], '|') : NULL; if (b) { *b = 0; f[k] = b + 1; } }
            if (f[3] && f[2]) {
                char tag[12];
                snprintf(tag, sizeof(tag), "%c%.10s", f[0][0], f[4] ? f[4] : "");
                add(f[2], tag, atoi(f[1]), atoi(f[3]));
            }
        } else if (screen == S_SHOWS) {               /* r|key|label (random) or o|key|label (in order) */
            char *k = strchr(p, '|'), *l = k ? strchr(k + 1, '|') : NULL;
            if (l) { *k = 0; *l = 0; add(l + 1, k + 1, p[0] == 'o' ? 4 : 0, 0); }
        } else if (screen == S_FAVS) {                /* id|name */
            char *b = strchr(p, '|');
            if (b) { *b = 0; add(b + 1, "", atoi(p), 0); }
        } else if (screen == S_TRACKS) {              /* a|index|label|current  or  s|index|label|current (s|off first) */
            char *f1 = strchr(p, '|'), *f2 = f1 ? strchr(f1 + 1, '|') : NULL, *f3 = f2 ? strchr(f2 + 1, '|') : NULL;
            if (f3) {
                char lab[72], kind = p[0];
                *f1 = 0; *f2 = 0; *f3 = 0;
                snprintf(lab, sizeof(lab), "%s: %.58s", kind == 'a' ? "Audio" : "Subtitles", f2 + 1);
                add(lab, kind == 'a' ? "a" : "s", strcmp(f1 + 1, "off") ? atoi(f1 + 1) : -1, f3[1] == '1' ? 1 : 0);
            }
        } else if (screen == S_QUEUE) {               /* cur=N, then pos|title */
            if (!strncmp(p, "cur=", 4)) { g_sel = atoi(p + 4); if (g_sel < 0) g_sel = 0; }
            else { char *b = strchr(p, '|'); if (b) { *b = 0; add(b + 1, "", atoi(p), 0); } }
        }
        if (!eol) break;
        p = eol + 1;
    }
    if (screen == S_LIVE) {                           /* live channels first, keep list order otherwise */
        Item tmp[128]; int i, n = 0;
        for (i = 0; i < g_n; i++) if (g_items[i].flag == 1) tmp[n++] = g_items[i];
        for (i = 0; i < g_n; i++) if (g_items[i].flag != 1) tmp[n++] = g_items[i];
        memcpy(g_items, tmp, sizeof(Item) * n);
    }
    if (screen == S_LIB && g_want_sel >= 0) { g_sel = g_want_sel < g_n ? g_want_sel : (g_n ? g_n - 1 : 0); g_want_sel = -1; g_scroll = g_sel >= 8 ? g_sel - 4 : 0; }
    if (screen == S_LIVE && g_live_keep[0]) {          /* a refresh in place: keep the cursor on the same channel */
        int i;
        for (i = 0; i < g_n; i++) if (!strcmp(g_items[i].label, g_live_keep)) { g_sel = i; break; }
        g_live_keep[0] = 0;
        if (g_sel >= g_n) g_sel = g_n ? g_n - 1 : 0;
        if (g_sel < g_scroll) g_scroll = g_sel;
        if (g_sel >= g_scroll + 8) g_scroll = g_sel - 7;
    }
    if (screen == S_QUEUE) { int i; g_qcur = -1; g_qn = g_n; for (i = 0; i < g_n; i++) if (g_items[i].id == g_sel) { g_items[i].flag = 1; g_qcur = g_items[i].id; }
        if (g_want_sel >= 0) { g_sel = g_want_sel < g_n ? g_want_sel : g_n - 1; if (g_sel < 0) g_sel = 0; g_want_sel = -1; g_scroll = g_sel >= 8 ? g_sel - 4 : 0; }
        if (g_sel >= 8 && g_scroll == 0) g_scroll = g_sel - 3; if (g_scroll > g_n - 8) g_scroll = g_n > 8 ? g_n - 8 : 0; }
}

void open_main(void) {
    menu_set(S_MAIN, "Kodi Remote");
    if (g_direct) {                                   /* what plain Kodi can do, no server needed */
        add("Show video fullscreen", "", 8, 0);
        add("Queue", "", 4, 0);
        add("Library: continue, movies, TV shows", "", 50, 0);
        add("Kodi home screen", "", 9, 0);
        add("Mute / unmute", "", 10, 0);
        add("Next item", "", 11, 0);
        add("Previous item", "", 12, 0);
        add("Audio and subtitles", "", 30, 0);
        add("Type text into Kodi", "", 31, 0);
        add("Settings", "", 33, 0);
        add("Power: restart Kodi, reboot, shut down", "", 32, 0);
        add("Controls (help)", "", 6, 0);
        add("Stop playback", "", 7, 0);
        add("Kodi connection (find again)", "", 13, 0);
        return;
    }
    add("Shows: random episode / in order", "", 1, 0);
    add("Live: Twitch and Kick", "", 2, 0);
    add("TV favourites (IPTV)", "", 3, 0);
    add("Queue", "", 4, 0);
    add("Library: continue, movies, TV shows", "", 50, 0);
    add("Seek, shuffle, repeat", "", 40, 0);
    add("Audio and subtitles", "", 30, 0);
    add("Show video fullscreen", "", 8, 0);
    add("Sleep timer", "", 5, 0);
    add("Type text into Kodi", "", 31, 0);
    add("Settings", "", 33, 0);
    add("Power: restart Kodi, reboot, shut down", "", 32, 0);
    add("Controls (help)", "", 6, 0);
    add("Stop playback", "", 7, 0);
}

void open_shows(void) {                           /* the list comes from the server (shows.txt there); parse_lines falls back to the built-in one */
    load_lines(S_SHOWS, "/t/showlist", "Shows");
}

void open_play(void) {
    menu_set(S_PLAY, "Seek and playback");
    add("Back 10 minutes", "d", -600, 0); add("Back 1 minute", "d", -60, 0); add("Back 30 seconds", "d", -30, 0);
    add("Forward 30 seconds", "d", 30, 0); add("Forward 1 minute", "d", 60, 0); add("Forward 10 minutes", "d", 600, 0);
    add("Jump to the start", "p", 0, 0); add("Jump to 25%", "p", 25, 0); add("Jump to 50%", "p", 50, 0);
    add("Jump to 75%", "p", 75, 0); add("Jump to 90%", "p", 90, 0);
    add("Shuffle on / off", "s", 0, 0);
    add("Repeat: off, all, one", "r", 0, 0);
}

void open_sleep(void) {
    menu_set(S_SLEEP, "Sleep timer");
    add("15 minutes", "", 15, 0); add("30 minutes", "", 30, 0); add("60 minutes", "", 60, 0); add("90 minutes", "", 90, 0); add("Cancel timer", "", 0, 0);
}

void open_qact(const Item *it) {
    g_qpos = it->id;
    snprintf(g_qtitle, sizeof(g_qtitle), "%.60s", it->label);
    menu_set(S_QACT, g_qtitle);
    add("Play now", "", 1, 0);
    add("Play next", "", 2, 0);
    add("Move with the stick...", "", 7, 0);
    add("Move up", "", 3, 0);
    add("Move down", "", 4, 0);
    add("Move to the end", "", 5, 0);
    add("Remove from queue", "", 6, 0);
    add("Clear all but the playing item", "", 10, 2);
    add("Clear the whole queue (stops playback)", "", 11, 2);
    if (!g_direct) {                                   /* these need the server, which keeps the snapshots */
        add("Undo the last queue change", "", 8, 0);
        add("Restore the queue from a snapshot", "", 9, 0);
        add("Bring back the queue from before the clear", "", 12, 0);
    }
}

/* ---- audio and subtitles, typing, power, settings */
static void urlenc(const char *in, char *out, int outsz) {
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;
    for (; *in && n < outsz - 4; in++) {
        unsigned char c = (unsigned char)*in;
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') out[n++] = (char)c;
        else { out[n++] = '%'; out[n++] = hex[c >> 4]; out[n++] = hex[c & 15]; }
    }
    out[n] = 0;
}

static void type_text(void) {                          /* the PSP keyboard types into whatever text box Kodi has open */
    char buf[64], enc[200], path[224];
    if (osk_input("Text to type into Kodi", "", buf, sizeof(buf)) != 1 || !buf[0]) return;
    urlenc(buf, enc, sizeof(enc));
    snprintf(path, sizeof(path), "/t/text?s=%s", enc);
    act("POST", path, 10, "Typing");
}

void open_power(void) {
    menu_set(S_POWER, "Power");
    add("Refresh video library", "refresh", 1, 0);
    add("Restart Kodi (about 30 s)", "restartkodi", 2, 1);
    add("Reboot LibreELEC", "reboot", 3, 2);              /* flag 2 = drawn red */
    add("Shut down LibreELEC", "shutdown", 4, 2);
}

static int edit_text(const char *prompt, char *dst, int dstsz) {      /* returns 1 when the user changed it */
    char buf[96];
    snprintf(buf, sizeof(buf), "%s", dst);
    if (osk_input(prompt, buf, buf, sizeof(buf)) != 1) return 0;
    strncpy(dst, buf, dstsz - 1); dst[dstsz - 1] = 0;
    return 1;
}

void open_settings(void) {
    char b[96];
    int keep = g_screen == S_SETTINGS ? g_sel : 0, next_direct = (g_mode_cfg == 2) || (g_mode_cfg == 0 && !g_token[0]);
    menu_set(S_SETTINGS, "Settings");
    if (g_direct) {
        snprintf(b, sizeof(b), "Kodi address: %s:%d", g_dhost[0] ? g_dhost : "(none)", g_dport); add(b, "", 11, 0);
        snprintf(b, sizeof(b), "Kodi user name: %s", g_duser[0] ? g_duser : "(none)"); add(b, "", 12, 0);
        snprintf(b, sizeof(b), "Kodi password: %s", g_dpass[0] ? "****** (set)" : "(empty)"); add(b, "", 13, 0);
        add("Find Kodi again (scan the network)", "", 14, 0);
    } else {
        snprintf(b, sizeof(b), "Server address: %s", g_host); add(b, "", 1, 0);
        snprintf(b, sizeof(b), "Server port: %d", g_port); add(b, "", 2, 0);
        snprintf(b, sizeof(b), "Server token: %s", g_token[0] ? "****** (set)" : "(not set)"); add(b, "", 3, 0);
    }
    snprintf(b, sizeof(b), "Wi-Fi network name: %s", g_ssid[0] ? g_ssid : "(choose at start)"); add(b, "", 4, 0);
    snprintf(b, sizeof(b), "Debug line: %s", g_debug ? "on" : "off"); add(b, "", 5, 0);
    snprintf(b, sizeof(b), "Next start: %s (X to switch)", next_direct ? "Kodi alone" : "server"); add(b, "", 6, 0);
    g_sel = keep < g_n ? keep : 0;
}

void select_item(void) {
    char path[160], dbg[120];
    snprintf(dbg, sizeof(dbg), "select: screen=%d row=%d of %d", g_screen, g_sel, g_n);
    logf_("%s", dbg);
    if (g_n == 0) { toast(C_ERR, "nothing to select on this screen"); return; }
    Item *it = &g_items[g_sel];
    logf_("  item: %s", it->label);
    switch (g_screen) {
    case S_MAIN:
        if (it->id == 1) open_shows();
        else if (it->id == 2) load_lines(S_LIVE, "/t/channels", "Live");
        else if (it->id == 3) load_lines(S_FAVS, "/t/favs", "TV favourites");
        else if (it->id == 4) load_lines(S_QUEUE, "/t/queue", "Queue");
        else if (it->id == 5) open_sleep();
        else if (it->id == 8) act("POST", "/t/do/fullscreen", 8, "Fullscreen video");
        else if (it->id == 9) act("POST", "/t/key/home", 8, "Kodi: home");
        else if (it->id == 10) act("POST", "/t/do/mute", 8, "Mute");
        else if (it->id == 11) act("POST", "/t/do/next", 8, "Next item");
        else if (it->id == 12) act("POST", "/t/do/prev", 8, "Previous item");
        else if (it->id == 13) { if (direct_find()) toast(C_OK, "Connected to Kodi"); }
        else if (it->id == 30) load_lines(S_TRACKS, "/t/tracks", "Audio and subtitles");
        else if (it->id == 40) open_play();
        else if (it->id == 50) lib_open_root();
        else if (it->id == 31) type_text();
        else if (it->id == 32) open_power();
        else if (it->id == 33) open_settings();
        else if (it->id == 6) { g_screen = S_REMOTE; g_title = "Controls"; }
        else if (it->id == 7) {                       /* destructive: needs a second press within about 4 s */
            if (g_stop_armed && g_frame - g_stop_armed < 240) { g_stop_armed = 0; act("POST", "/t/stop", 15, "Stopping"); }
            else { g_stop_armed = g_frame; toast(C_ERR, "Press again to STOP playback"); }
        }
        break;
    case S_SHOWS:
        snprintf(path, sizeof(path), "/t/%s/%s", it->id == 4 ? "order" : "random", it->tag);
        act("POST", path, 30, it->label);
        break;
    case S_LIVE:
        snprintf(path, sizeof(path), "/t/live/%s/%s", it->tag, it->label);
        act("POST", path, 40, it->label);
        break;
    case S_FAVS:
        snprintf(path, sizeof(path), "/t/fav/%d", it->id);
        act("POST", path, 20, it->label);
        break;
    case S_QUEUE:
        if (g_grab) { g_grab = 0; g_want_sel = g_sel; load_lines(S_QUEUE, "/t/queue", "Queue"); toast(C_OK, "Dropped"); break; }
        open_qact(it);                                /* START on a queue item: choose what to do with it */
        break;
    case S_LIB: lib_select(it); break;
    case S_LACT: lib_act(it); break;
    case S_PLAY:
        if (it->tag[0] == 'd') snprintf(path, sizeof(path), "/t/seek/%d", it->id);
        else if (it->tag[0] == 'p') snprintf(path, sizeof(path), "/t/seek/pct/%d", it->id);
        else snprintf(path, sizeof(path), "%s", it->tag[0] == 's' ? "/t/shuffle" : "/t/repeat");
        act("POST", path, 15, it->label);
        break;
    case S_TRACKS:
        snprintf(path, sizeof(path), "/t/track/%s/%s", it->tag, it->id < 0 ? "off" : "0");
        if (it->id >= 0) snprintf(path, sizeof(path), "/t/track/%s/%d", it->tag, it->id);
        act("POST", path, 10, it->label);
        load_lines(S_TRACKS, "/t/tracks", "Audio and subtitles");        /* queued behind the change, so the marker moves */
        g_want_sel = -1;
        break;
    case S_POWER: {
        static int armed_id; static unsigned armed_at;
        if (it->id == 1) { act("POST", "/t/power/refresh", 20, "Refreshing the library"); break; }
        if (armed_id == it->id && g_frame - armed_at < 300) {                 /* second press within 5 s */
            snprintf(path, sizeof(path), "/t/power/%s/confirm", it->tag);
            armed_id = 0;
            act("POST", path, 20, it->label);
        } else {
            armed_id = it->id; armed_at = g_frame;
            snprintf(dbg, sizeof(dbg), "Press START again: %s", it->label);
            toast(C_ERR, dbg);
        }
        break;
    }
    case S_SETTINGS: {
        int ok = 0;
        switch (it->id) {
        case 1: ok = edit_text("Server address", g_host, sizeof(g_host)); break;
        case 2: { char t[12]; snprintf(t, sizeof(t), "%d", g_port); if (edit_text("Server port", t, sizeof(t)) && atoi(t) > 0) { g_port = atoi(t); ok = 1; } break; }
        case 3: ok = edit_text("Server token", g_token, sizeof(g_token)); break;
        case 4: ok = edit_text("Wi-Fi network name (as saved on the PSP)", g_ssid, sizeof(g_ssid)); break;
        case 5: g_debug = !g_debug; ok = 1; break;
        case 6: g_mode_cfg = ((g_mode_cfg == 2) || (g_mode_cfg == 0 && !g_token[0])) ? 1 : 2; ok = 1; toast(C_ACC, "Restart the app for the mode change"); break;
        case 11: { char t[60]; snprintf(t, sizeof(t), "%s:%d", g_dhost, g_dport); if (edit_text("Kodi address (host or host:port)", t, sizeof(t)) && t[0]) {
                       char *c = strchr(t, ':'); if (c) { *c = 0; if (atoi(c + 1) > 0) g_dport = atoi(c + 1); }
                       strncpy(g_dhost, t, sizeof(g_dhost) - 1); ok = 1; } break; }
        case 12: ok = edit_text("Kodi user name", g_duser, sizeof(g_duser)); break;
        case 13: ok = edit_text("Kodi password", g_dpass, sizeof(g_dpass)); break;
        case 14: if (direct_find()) toast(C_OK, "Connected to Kodi"); open_settings(); return;
        }
        if (ok) {
            if (g_direct && it->id != 6) save_direct(); else save_config();
            if (g_toast_frames < 60) toast(C_OK, "Saved on the card");
            open_settings();
        }
        break;
    }
    case S_QACT: {
        int pos = g_qpos, cur = g_qcur, n = g_qn, to = -1, want = pos;
        switch (it->id) {
        case 1: snprintf(path, sizeof(path), "/t/goto/%d", pos); act("POST", path, 20, "Playing"); want = pos; break;
        case 2:
            if (pos == cur) { toast(C_ERR, "That one is already playing"); return; }
            to = cur < 0 ? 0 : (pos < cur ? cur : cur + 1);
            if (to == pos) { toast(C_ERR, "It is already next"); return; }
            break;
        case 3: if (pos <= 0) { toast(C_ERR, "Already first"); return; } to = pos - 1; break;
        case 4: if (pos >= n - 1) { toast(C_ERR, "Already last"); return; } to = pos + 1; break;
        case 5: if (pos >= n - 1) { toast(C_ERR, "Already last"); return; } to = n - 1; break;
        case 6:
            if (pos == cur) { toast(C_ERR, "That one is playing: stop playback first"); return; }
            snprintf(path, sizeof(path), "/t/q/remove/%d", pos);
            act("POST", path, 20, "Removing");
            want = pos < n - 1 ? pos : n - 2;
            break;
        case 10: case 11: {                                /* destructive: START twice within 5 s, like the power menu */
            static int armed; static unsigned armed_at;
            if (armed == it->id && g_frame - armed_at < 300) {
                armed = 0;
                if (it->id == 11) snprintf(path, sizeof(path), "/t/q/clear/all");
                else snprintf(path, sizeof(path), "/t/q/clear/keep/%d/%d", g_stat[g_cur].playing ? g_stat[g_cur].pos : -1, g_qn);
                act("POST", path, 30, it->id == 11 ? "Clearing the queue" : "Clearing all but the playing item");
                want = 0;                                  /* the common tail below reloads the queue screen */
            } else {
                armed = it->id; armed_at = g_frame;
                snprintf(dbg, sizeof(dbg), g_direct ? "Press START again to clear (cannot be undone here)" : "Press START again to clear (restorable)");
                toast(C_ERR, dbg);
                return;                                    /* stay on this menu so the second press can arrive */
            }
            break;
        }
        case 12: act("POST", "/t/q/restore/clear", 40, "Restoring the queue"); want = 0; break;
        case 8: act("POST", "/t/q/undo", 20, "Undoing"); break;
        case 9: act("POST", "/t/q/restore", 30, "Restoring the queue"); break;
        case 7:                                       /* grab: the stick now moves this item */
            g_grab = 1;
            load_lines(S_QUEUE, "/t/queue", "Queue");
            g_want_sel = pos;
            toast(C_ACC, "MOVING: stick up/down moves it, START drops");
            return;
        }
        if (to >= 0) { snprintf(path, sizeof(path), "/t/q/move/%d/%d", pos, to); act("POST", path, 20, "Moving"); want = to; }
        g_want_sel = want;
        load_lines(S_QUEUE, "/t/queue", "Queue");
        break;
    }
    case S_SLEEP:
        if (it->id == 0) act("POST", "/t/sleep/off", 15, "Cancelling timer");
        else { snprintf(path, sizeof(path), "/t/sleep/%d", it->id); act("POST", path, 15, it->label); }
        break;
    }
}

/* ---------------------------------------------------------------- input */
unsigned g_hold[32];

int pressed_on(int on, int idx, int repeat) {
    if (!on) { g_hold[idx] = 0; return 0; }
    g_hold[idx]++;
    if (g_hold[idx] == 1) return 1;
    if (repeat && g_hold[idx] > 22 && (g_hold[idx] % 6) == 0) return 1;
    return 0;
}

int pressed(unsigned buttons, unsigned b, int idx, int repeat) { return pressed_on((buttons & b) != 0, idx, repeat); }
