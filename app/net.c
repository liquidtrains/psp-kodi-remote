#include "app.h"

int find_profile(void) {
    netData d;
    int i, first = 0;
    for (i = 1; i < 100; i++) {
        if (sceUtilityCheckNetParam(i) != 0) continue;
        if (!first) first = i;
        memset(&d, 0, sizeof(d));
        if (sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &d) == 0 && !strcmp(d.asString, g_ssid)) return i;
    }
    dprintf("no saved connection with SSID '%s'; using entry %d\n", g_ssid, first);
    return first;
}

/* shown on the plain text screen when the first Wi-Fi join fails (server mode) */
void wifi_help(void) {
    dprintf("\nWi-Fi did not connect. Check, in this order:\n");
    dprintf(" 1. The WLAN switch on the PSP is on.\n");
    dprintf(" 2. The router has 2.4 GHz on (the PSP cannot use 5 GHz).\n");
    dprintf(" 3. A WPA2 network needs the ARK-4 'wpa2' setting (wpa2psp\n");
    dprintf("    plugin) switched on, then restart the PSP.\n");
    dprintf(" 4. The router must be WPA2-AES only: no TKIP, no\n");
    dprintf("    WPA/WPA2 mixed, no WPA3.\n");
    dprintf(" 5. The network is saved in Settings > Network Settings\n");
    dprintf("    and its connection test passes.\n");
}

int net_up(void) {
    int err, state = 0, last = -1, t;
    sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if ((err = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024)) < 0) { dprintf("sceNetInit %08x\n", err); return -1; }
    sceNetInetInit();
    if ((err = sceNetApctlInit(0x8000, 48)) < 0) { dprintf("sceNetApctlInit %08x\n", err); return -1; }
    int prof = find_profile();
    g_prof = prof;
    if (prof <= 0) { dprintf("no network settings saved on this PSP\n"); return -1; }
    dprintf("connecting with saved entry %d ...\n", prof);
    if ((err = sceNetApctlConnect(prof)) != 0) { dprintf("sceNetApctlConnect %08x\n", err); return -1; }
    for (t = 0; t < 600; t++) {
        sceNetApctlGetState(&state);
        if (state != last) { dprintf("  wifi state %d\n", state); last = state; }
        if (state == PSP_NET_APCTL_STATE_GOT_IP) break;
        sceKernelDelayThread(50 * 1000);
    }
    if (state != PSP_NET_APCTL_STATE_GOT_IP) { dprintf("no IP address\n"); return -1; }
    union SceNetApctlInfo info;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) { dprintf("got IP %s\n", info.ip); logf_("ip %s", info.ip); }
    return 0;
}

/* rejoin the saved network (after a sleep or a dropped link); runs on the poll thread */
void net_reconnect(void) {
    int t, st = 0;
    sceNetApctlDisconnect();
    sceKernelDelayThread(500 * 1000);
    if (g_prof > 0) sceNetApctlConnect(g_prof);
    for (t = 0; t < 400; t++) {                                  /* up to about 20 s */
        sceNetApctlGetState(&st);
        if (st == PSP_NET_APCTL_STATE_GOT_IP) break;
        sceKernelDelayThread(50 * 1000);
    }
    logf_("wifi reconnect state %s", st == PSP_NET_APCTL_STATE_GOT_IP ? "got ip" : "failed");
}

/* one HTTP exchange with a host:port; returns bytes read, 0 for no reply, -1 bad host or no socket, -2 cannot connect */
int tcp_exchange(const char *host, int port, const char *req, int reqlen, int timeout_s, char *resp, int respsz) {
    struct sockaddr_in sa;
    int fd, n, len = 0, a, b, c, d, to = timeout_s * 1000 * 1000;
    if (sscanf(host, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return -1;
    fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = (unsigned)a | ((unsigned)b << 8) | ((unsigned)c << 16) | ((unsigned)d << 24);
    if (sceNetInetConnect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { sceNetInetClose(fd); return -2; }
    sceNetInetSend(fd, req, reqlen, 0);
    while ((n = sceNetInetRecv(fd, resp + len, respsz - 1 - len, 0)) > 0 && len < respsz - 1) len += n;
    resp[len] = 0;
    sceNetInetClose(fd);
    return len;
}

/* the app's /t/* requests, answered by Kodi itself (see direct.c); same return convention as http() */
int http_direct(const char *method, const char *path, int timeout_s, char *out, int outsz) {
    enum { REQSZ = 16600, RAWSZ = 24000 };
    char body[16000], *req = malloc(REQSZ), *raw = malloc(RAWSZ), *jb;
    int kind = 0, code = 0, n, r = 0;
    out[0] = 0;
    if (!req || !raw) { free(req); free(raw); snprintf(out, outsz, "out of memory"); return 0; }
    if (dk_request(method, path, body, sizeof(body), &kind)) { snprintf(out, outsz, "needs the server (not in Kodi-alone mode)"); free(req); free(raw); return 404; }
    if (!g_dhost[0]) { snprintf(out, outsz, "no Kodi chosen yet"); free(req); free(raw); return 0; }
    dk_http(req, REQSZ, g_dhost, g_dport, g_duser, g_dpass, body);
    n = tcp_exchange(g_dhost, g_dport, req, (int)strlen(req), timeout_s, raw, RAWSZ);
    if (n < 0) snprintf(out, outsz, "cannot reach Kodi at %s:%d", g_dhost, g_dport);
    else if (n == 0) snprintf(out, outsz, "no reply from Kodi (timeout?)");
    else {
        sscanf(raw, "HTTP/1.%*d %d", &code);
        jb = strstr(raw, "\r\n\r\n");
        r = dk_reply(kind, code, jb ? jb + 4 : "", out, outsz);
    }
    free(req); free(raw);
    return n > 0 ? r : 0;
}

int http(const char *method, const char *path, int timeout_s, char *out, int outsz) {
    struct sockaddr_in sa;
    if (g_direct) return http_direct(method, path, timeout_s, out, outsz);
    char req[512], buf[1024];
    static char raw_dummy;
    int fd, n, len = 0, a, b, c, d, code = 0;
    (void)raw_dummy;
    out[0] = 0;
    if (sscanf(g_host, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) { snprintf(out, outsz, "bad host in config"); return 0; }
    fd = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(out, outsz, "socket failed"); return 0; }
    int to = timeout_s * 1000 * 1000;
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    sceNetInetSetsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &to, sizeof(to));
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(g_port);
    sa.sin_addr.s_addr = (unsigned)a | ((unsigned)b << 8) | ((unsigned)c << 16) | ((unsigned)d << 24);
    if (sceNetInetConnect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        snprintf(out, outsz, "cannot reach %s:%d (errno %d)", g_host, g_port, sceNetInetGetErrno());
        sceNetInetClose(fd); return 0;
    }
    snprintf(req, sizeof(req), "%s %s HTTP/1.0\r\nHost: %s\r\nX-Token: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", method, path, g_host, g_token);
    sceNetInetSend(fd, req, strlen(req), 0);
    static char hdrbuf[8];                    /* only used to keep the compiler quiet about unused paths */
    (void)hdrbuf;
    char *raw = out;                          /* read straight into out, then strip the headers in place */
    while ((n = sceNetInetRecv(fd, buf, sizeof(buf), 0)) > 0 && len < outsz - 1) {
        if (len + n > outsz - 1) n = outsz - 1 - len;
        memcpy(raw + len, buf, n); len += n;
    }
    raw[len] = 0;
    sceNetInetClose(fd);
    if (len == 0) { snprintf(out, outsz, "no reply (timeout?)"); return 0; }
    sscanf(raw, "HTTP/1.%*d %d", &code);
    char *body = strstr(raw, "\r\n\r\n");
    if (body) memmove(out, body + 4, strlen(body + 4) + 1); else out[0] = 0;
    return code;
}

const char *kv(const char *body, const char *key, char *dst, int dstsz) {   /* "key=value" line -> dst, or NULL */
    char pat[40];
    snprintf(pat, sizeof(pat), "%s=", key);
    const char *p = body;
    while (p && *p) {
        if (!strncmp(p, pat, strlen(pat))) {
            const char *v = p + strlen(pat);
            int n = strcspn(v, "\r\n");
            if (n >= dstsz) n = dstsz - 1;
            memcpy(dst, v, n); dst[n] = 0;
            return dst;
        }
        p = strchr(p, '\n'); if (p) p++;
    }
    return NULL;
}

Stat g_stat[2];

volatile int g_cur = 0, g_poll_now = 0;

int poll_thread(SceSize args, void *argp) {
    static char body[1024];
    char v[100];
    while (1) {
        Stat s;
        memset(&s, 0, sizeof(s));
        /* In use: keep everything awake. Idle: only stop the auto-suspend, so the PSP's own Backlight Auto-Off (XMB > Settings > Power Save) can
         * turn the screen off while Wi-Fi and the remote keep working; any button wakes it. */
        scePowerTick(idle_seconds() >= IDLE_SLOW_S ? PSP_POWER_TICK_SUSPEND : PSP_POWER_TICK_ALL);
        { int st = 0; sceNetApctlGetState(&st);
          if (g_resumed || st != PSP_NET_APCTL_STATE_GOT_IP || g_fails >= 2) { g_reconnecting = 1; net_reconnect(); g_resumed = 0; g_fails = 0; g_reconnecting = 0; } }
        int code = http("GET", "/t/status", 8, body, sizeof(body));
        g_fails = code ? 0 : g_fails + 1;
        if (code == 200) {
            s.ok = 1;
            s.playing = kv(body, "playing", v, sizeof(v)) && v[0] == '1';
            s.paused = kv(body, "paused", v, sizeof(v)) && v[0] == '1';
            s.muted = kv(body, "muted", v, sizeof(v)) && v[0] == '1';
            s.vol = kv(body, "volume", v, sizeof(v)) ? atoi(v) : -1;
            s.qlen = kv(body, "queue_len", v, sizeof(v)) ? atoi(v) : 0;
            s.pos = kv(body, "position", v, sizeof(v)) ? atoi(v) : -1;
            { static int last_q = -2, last_p = -2;                         /* Kodi moved on, or the queue changed there: refresh an open queue screen once */
              if (s.qlen != last_q || s.pos != last_p) { if (g_screen == S_QUEUE && !g_grab) g_queue_dirty = 1; last_q = s.qlen; last_p = s.pos; } }
            s.pct = kv(body, "percent", v, sizeof(v)) ? (float)atof(v) : 0;
            s.time = kv(body, "time", v, sizeof(v)) ? atoi(v) : 0;
            s.total = kv(body, "total", v, sizeof(v)) ? atoi(v) : 0;
            s.shuffled = kv(body, "shuffled", v, sizeof(v)) && v[0] == '1';
            if (kv(body, "repeat", v, sizeof(v))) { strncpy(s.repeat, v, sizeof(s.repeat) - 1); }
            if (kv(body, "live", v, sizeof(v))) { strncpy(s.live, v, sizeof(s.live) - 1); }
            if (kv(body, "title", v, sizeof(v))) strncpy(s.title, v, sizeof(s.title) - 1);
        } else {
            if (code) snprintf(s.err, sizeof(s.err), "API error %d", code);
            else snprintf(s.err, sizeof(s.err), "%.55s", body);
        }
        g_stat[1 - g_cur] = s;
        g_cur = 1 - g_cur;
        int i;
        { int n = idle_seconds() >= IDLE_SLOW_S ? 100 : 40;                          /* 4 s while in use, 10 s when idle; sooner after an action */
          for (i = 0; i < n && !g_poll_now; i++) sceKernelDelayThread(100 * 1000); }
        g_poll_now = 0;
    }
    return 0;
}

Job g_jobs[NJOBS];

volatile int g_jh, g_jt;

char g_wbuf[12288];                      /* a queue of 128 titles is about 9 KB of text */
volatile int g_queue_dirty;

volatile int g_load_ready, g_load_code, g_load_screen;

int job_push(const char *method, const char *path, int timeout_s, const char *what, int load) {
    int nt = (g_jt + 1) % NJOBS;
    if (nt == g_jh) { toast(C_ERR, "Busy: the last request has not finished"); return 0; }
    Job *j = &g_jobs[g_jt];
    strncpy(j->method, method, sizeof(j->method) - 1); j->method[sizeof(j->method) - 1] = 0;
    strncpy(j->path, path, sizeof(j->path) - 1); j->path[sizeof(j->path) - 1] = 0;
    strncpy(j->what, what, sizeof(j->what) - 1); j->what[sizeof(j->what) - 1] = 0;
    j->timeout = timeout_s; j->load = load;
    g_jt = nt;
    return 1;
}

int worker_thread(SceSize args, void *argp) {
    char msg[120];
    for (;;) {
        if (g_jh == g_jt || g_load_ready) { sceKernelDelayThread(idle_seconds() >= IDLE_SLOW_S ? 100 * 1000 : 30 * 1000); continue; }
        Job j = g_jobs[g_jh];
        unsigned t0 = sceKernelGetSystemTimeLow();
        int code = http(j.method, j.path, j.timeout, g_wbuf, sizeof(g_wbuf));
        unsigned ms = (sceKernelGetSystemTimeLow() - t0) / 1000;
        if (j.load) { g_load_code = code; g_load_screen = j.load; g_load_ready = 1; }
        else {
            g_wbuf[strcspn(g_wbuf, "\r\n")] = 0;
            snprintf(msg, sizeof(msg), "%.90s  (%u ms)", g_wbuf[0] ? g_wbuf : (code == 200 ? "ok" : "request failed"), ms);
            logf_("%s", j.path); logf_("  -> %s", msg);
            toast(code == 200 ? C_OK : C_ERR, msg);
            g_poll_now = 1;
        }
        g_jh = (g_jh + 1) % NJOBS;
    }
    return 0;
}

int act(const char *method, const char *path, int timeout_s, const char *what) {
    char msg[120];
    snprintf(msg, sizeof(msg), "%s ...", what);
    toast(C_ACC, msg);
    return job_push(method, path, timeout_s, what, 0);       /* returns at once; the worker shows the result */
}

int profile_for(const char *ssid) {              /* saved network entry with this SSID, 0 if none */
    netData d;
    int i;
    if (!ssid[0]) return 0;
    for (i = 1; i < 100; i++) {
        if (sceUtilityCheckNetParam(i) != 0) continue;
        memset(&d, 0, sizeof(d));
        if (sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &d) == 0 && !strcmp(d.asString, ssid)) return i;
    }
    return 0;
}

int wait_ip(int ms, const char *msg) {
    int st = 0, t;
    const char *l[2] = { msg, "" };
    for (t = 0; t < ms / 50; t++) {
        sceNetApctlGetState(&st);
        if (st == PSP_NET_APCTL_STATE_GOT_IP) return 1;
        if (t % 4 == 0) dlg_page("Wi-Fi", l, 1, NULL, -1);
        sceKernelDelayThread(50 * 1000);
    }
    return 0;
}

/* 0 = not Kodi or not reachable, 1 = Kodi answered the ping (login accepted or none needed), 2 = Kodi wants a (different) login */
int probe_host(const char *ip, int port, const char *user, const char *pass) {
    char req[600], raw[1500];
    int n, r = 0;
    dk_http(req, sizeof(req), ip, port, user, pass, DK_PING);
    n = tcp_exchange(ip, port, req, (int)strlen(req), 2, raw, sizeof(raw));
    if (n > 0) r = dk_probe(raw);
    return r;
}
