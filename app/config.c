#include "app.h"

char g_host[64] = "192.168.1.10", g_token[96] = "", g_ssid[40] = "", g_dir[256] = "ms0:/PSP/GAME/KodiRemote/";

int g_port = 8791;

int g_debug = 1;                                /* bottom-right line: stick, buttons, mode. config.txt: debug=0 hides it */

FILE *g_log;

int g_prof;                                     /* saved PSP network entry in use */

volatile int g_resumed, g_fails, g_reconnecting;

/* Kodi-alone mode ("direct"): the PSP talks to Kodi's own web server. Chosen by mode=direct in config.txt, or automatically when
 * config.txt has no token. Server mode (token present, no mode= line) never touches any of this. */
int g_direct, g_mode_cfg;                       /* g_mode_cfg: 0 = not set, 1 = server, 2 = direct */

char g_dhost[48] = "", g_duser[48] = "kodi", g_dpass[64] = "";

int g_dport = 8080;

int g_menu_mode;

int g_qpos, g_qcur = -1, g_qn, g_want_sel = -1, g_grab;      /* queue editing: picked item, playing item, length, selection to restore, grab mode */

int g_sq_cnt, g_sq_fired;                                    /* SQUARE: tap = Kodi info, hold = fullscreen */

char g_qtitle[72];                                /* 0 = d-pad/X/O drive Kodi, 1 = they drive the PSP menu (toggle: L+R) */

unsigned g_frame, g_stop_armed;                 /* frame counter; frame at which "Stop playback" was first pressed */

void logf_(const char *fmt, const char *a) { if (g_log) { fprintf(g_log, fmt, a); fputc('\n', g_log); fflush(g_log); } }

/* writes config.txt from the live settings; the old file is copied to config.txt.bak the first time in a session */
void save_config(void) {
    static int backed;
    char path[300], bak[310], buf[512];
    FILE *f, *g;
    size_t n;
    snprintf(path, sizeof(path), "%sconfig.txt", g_dir);
    if (!backed) {
        backed = 1;
        snprintf(bak, sizeof(bak), "%sconfig.txt.bak", g_dir);
        if ((f = fopen(path, "rb"))) {
            if ((g = fopen(bak, "wb"))) { while ((n = fread(buf, 1, sizeof(buf), f)) > 0) fwrite(buf, 1, n, g); fclose(g); }
            fclose(f);
        }
    }
    if (!(f = fopen(path, "w"))) { toast(C_ERR, "Could not write config.txt"); return; }
    fprintf(f, "host=%s\nport=%d\nssid=%s\ntoken=%s\ndebug=%d\n", g_host, g_port, g_ssid, g_token, g_debug);
    if (g_mode_cfg) fprintf(f, "mode=%s\n", g_mode_cfg == 2 ? "direct" : "server");
    fclose(f);
}

void load_config(const char *argv0) {
    char path[300], line[160];
    if (argv0) { strncpy(g_dir, argv0, sizeof(g_dir) - 20); g_dir[sizeof(g_dir) - 20] = 0; char *s = strrchr(g_dir, '/'); if (s) s[1] = 0; }
    snprintf(path, sizeof(path), "%sconfig.txt", g_dir);
    FILE *f = fopen(path, "r");
    if (!f) { dprintf("no config.txt at %s\n", path); return; }
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host=", 5)) { strncpy(g_host, line + 5, sizeof(g_host) - 1); }
        else if (!strncmp(line, "port=", 5)) g_port = atoi(line + 5);
        else if (!strncmp(line, "token=", 6)) { strncpy(g_token, line + 6, sizeof(g_token) - 1); }
        else if (!strncmp(line, "ssid=", 5)) { strncpy(g_ssid, line + 5, sizeof(g_ssid) - 1); }
        else if (!strncmp(line, "debug=", 6)) g_debug = atoi(line + 6);
        else if (!strncmp(line, "mode=", 5)) g_mode_cfg = !strncmp(line + 5, "direct", 6) ? 2 : 1;
    }
    fclose(f);
}

void load_direct(void) {
    char path[300], line[160];
    FILE *f;
    snprintf(path, sizeof(path), "%sdirect.txt", g_dir);
    if (!(f = fopen(path, "r"))) return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "host=", 5)) { strncpy(g_dhost, line + 5, sizeof(g_dhost) - 1); }
        else if (!strncmp(line, "port=", 5)) g_dport = atoi(line + 5);
        else if (!strncmp(line, "user=", 5)) { strncpy(g_duser, line + 5, sizeof(g_duser) - 1); }
        else if (!strncmp(line, "pass=", 5)) { strncpy(g_dpass, line + 5, sizeof(g_dpass) - 1); }
        else if (!strncmp(line, "ssid=", 5)) { strncpy(g_ssid, line + 5, sizeof(g_ssid) - 1); }
    }
    fclose(f);
    if (g_dport <= 0) g_dport = 8080;
}

void save_direct(void) {                         /* plain text on the memory card: use a Kodi login you use nowhere else */
    char path[300];
    FILE *f;
    snprintf(path, sizeof(path), "%sdirect.txt", g_dir);
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "host=%s\nport=%d\nuser=%s\npass=%s\nssid=%s\n", g_dhost, g_dport, g_duser, g_dpass, g_ssid);
    fclose(f);
}
