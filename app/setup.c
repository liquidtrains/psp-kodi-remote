#include "app.h"

/* Wi-Fi: closes the app (with a prompt) when there is none; otherwise joins a saved network or lets the user pick one */
void direct_wifi(void) {
    static const char *off[] = { "The Wi-Fi switch on the PSP is off.", "", "Slide the WLAN switch on the top edge of the", "PSP to the on position, then start", "Kodi Remote again.", "", "The app will close now." };
    static const char *pick[] = { "The PSP is not connected to Wi-Fi.", "", "Next you will see the PSP's own list of saved", "networks. Pick the one your Kodi is on.", "", "If you pick nothing, the app will close." };
    static const char *fail[] = { "No Wi-Fi connection was made. Check:", "1  The router has 2.4 GHz on (no 5 GHz on a PSP).", "2  WPA2 needs the ARK-4 'wpa2' setting switched on", "    (wpa2psp plugin), then restart the PSP.", "3  The router must be WPA2-AES only: no TKIP,", "    no WPA/WPA2 mixed, no WPA3.", "4  The network is saved in Network Settings and", "    its connection test passes.", "The app will close now." };
    int prof;
    if (!sceWlanGetSwitchState()) dlg_exit("Wi-Fi is off", off, 7);
    sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
    sceNetInetInit();
    sceNetApctlInit(0x8000, 48);
    prof = profile_for(g_ssid);
    if (prof > 0) {
        g_prof = prof;
        sceNetApctlConnect(prof);
        if (wait_ip(20000, "Joining the saved Wi-Fi network...")) return;
        sceNetApctlDisconnect();
    }
    if (!dlg_choice("Connect to Wi-Fi", pick, 6, "X  Choose a network      O  Close the app", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE)) sceKernelExitGame();
    run_netconf();
    if (!wait_ip(4000, "Checking the connection...")) dlg_exit("Not connected", fail, 9);
    {
        union SceNetApctlInfo info;
        if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_SSID, &info) == 0) { strncpy(g_ssid, (char *)info.ssid, sizeof(g_ssid) - 1); g_ssid[sizeof(g_ssid) - 1] = 0; }
        g_prof = profile_for(g_ssid);
    }
}

/* try port g_scan_port on every address of the PSP's /24, 16 at a time; returns how many Kodi devices answered */
int scan_net(char found[][20], int maxn, int port, const char *own_ip) {
    int a, b, c, d, nfound = 0, start, own;
    char line[64];
    const char *l[3] = { "Looking for Kodi...", line, "Press O to stop." };
    if (sscanf(own_ip, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return 0;
    own = d;
    for (start = 1; start <= 254; start += 16) {
        int fds[16], k, cnt = 0, h, i;
        SceNetInetPollfd pf[16];
        int map[16];
        unsigned t0, deadline_ms = 700;
        snprintf(line, sizeof(line), "%d.%d.%d.%d - %d.%d.%d.%d      found %d", a, b, c, start, a, b, c, start + 15 > 254 ? 254 : start + 15, nfound);
        dlg_page("Find Kodi", l, 3, NULL, -1);
        if (pad_now() & PSP_CTRL_CIRCLE) break;
        for (k = 0; k < 16; k++) {
            struct sockaddr_in sa;
            int one = 1;
            h = start + k;
            fds[k] = -1;
            if (h > 254 || h == own) continue;
            fds[k] = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
            if (fds[k] < 0) continue;
            sceNetInetSetsockopt(fds[k], SOL_SOCKET, SO_NONBLOCK, &one, sizeof(one));
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET; sa.sin_port = htons(port);
            sa.sin_addr.s_addr = (unsigned)a | ((unsigned)b << 8) | ((unsigned)c << 16) | ((unsigned)h << 24);
            sceNetInetConnect(fds[k], (struct sockaddr *)&sa, sizeof(sa));       /* returns at once, in progress */
            pf[cnt].fd = fds[k]; pf[cnt].events = SCE_NET_INET_POLLOUT; pf[cnt].revents = 0; map[cnt] = h;
            cnt++;
        }
        t0 = sceKernelGetSystemTimeLow();
        while ((sceKernelGetSystemTimeLow() - t0) / 1000 < deadline_ms) {       /* poll in short slices: works whatever unit the timeout has */
            int ready = 0;
            sceNetInetPoll(pf, cnt, 20);
            for (i = 0; i < cnt; i++) if (pf[i].revents) ready++;
            if (ready == cnt) break;
            sceKernelDelayThread(15 * 1000);
        }
        for (i = 0; i < cnt; i++) {
            int err = 1;
            socklen_t el = sizeof(err);
            if ((pf[i].revents & SCE_NET_INET_POLLOUT) && sceNetInetGetsockopt(pf[i].fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0 && nfound < maxn) {
                char ip[20];
                snprintf(ip, sizeof(ip), "%d.%d.%d.%d", a, b, c, map[i]);
                strncpy(found[nfound], ip, 19); found[nfound][19] = 0;
                nfound++;                                                           /* port is open: identify it below */
            }
        }
        for (k = 0; k < 16; k++) if (fds[k] >= 0) sceNetInetClose(fds[k]);
        scePowerTick(PSP_POWER_TICK_ALL);
    }
    /* keep only the ones that really are Kodi */
    {
        int i, keep = 0;
        for (i = 0; i < nfound; i++) {
            logf_("port open at %s", found[i]);
            if (probe_host(found[i], port, "", "") != 0) { if (keep != i) strcpy(found[keep], found[i]); keep++; }
        }
        nfound = keep;
    }
    return nfound;
}

/* asks for a login until Kodi accepts it. Returns 1 ok, 0 cancelled, -1 not Kodi / not reachable */
int direct_login(const char *host, int port) {
    static const char *need[] = { "This Kodi asks for a user name and password.", "", "They are in Kodi under Settings > Services >", "Control (the web server section).", "Kodi's default user name is kodi.", "(root is LibreELEC's SSH login, not this.)", "", "You can leave the password empty if there is none." };
    int r, tries;
    r = probe_host(host, port, "", "");
    if (r == 0) return -1;
    if (r == 1) { g_duser[0] = 0; g_dpass[0] = 0; return 1; }                    /* no login needed */
    if (probe_host(host, port, g_duser, g_dpass) == 1) return 1;                 /* saved or default login works */
    for (tries = 0; tries < 6; tries++) {
        char u[48], pw[64];
        if (!dlg_choice(tries ? "Login was refused" : "Kodi login", need, 8, "X  Enter the login      O  Back", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE)) return 0;
        strncpy(u, g_duser[0] ? g_duser : "kodi", sizeof(u) - 1); u[sizeof(u) - 1] = 0;
        if (osk_input("Kodi user name", u, u, sizeof(u)) != 1) return 0;
        pw[0] = 0;
        if (osk_input("Kodi password (empty if none)", "", pw, sizeof(pw)) < 0) return 0;
        strncpy(g_duser, u, sizeof(g_duser) - 1); strncpy(g_dpass, pw, sizeof(g_dpass) - 1);
        if (probe_host(host, port, g_duser, g_dpass) == 1) return 1;
    }
    return 0;
}

/* explain, scan on request, choose, log in, remember. Returns 1 when connected, 0 if the user gave up */
int direct_find(void) {
    union SceNetApctlInfo info;
    char own[20] = "", found[8][20], intro3[80], intro4[80], intro5[80], labels_buf[8][32];
    const char *labels[8];
    int nfound, scanning = 1, i;
    if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) { strncpy(own, info.ip, 19); own[19] = 0; }
    {
        int a = 0, b = 0, c = 0, d = 0;
        sscanf(own, "%d.%d.%d.%d", &a, &b, &c, &d);
        snprintf(intro3, sizeof(intro3), "It tries port 8080 on every address from %d.%d.%d.1", a, b, c);
        snprintf(intro4, sizeof(intro4), "to %d.%d.%d.254, 16 at a time (about 10 seconds), and", a, b, c);
        snprintf(intro5, sizeof(intro5), "asks each one that answers: are you Kodi?");
    }
    {
        const char *intro[] = { "Kodi Remote can look for Kodi on this Wi-Fi", "network (this PSP is at the address below).", intro3, intro4, intro5, "Nothing is changed on any device.", "", own[0] ? own : "(no address yet)" };
        char title_ssid[60];
        snprintf(title_ssid, sizeof(title_ssid), "Find Kodi on %s", g_ssid[0] ? g_ssid : "this network");
        scanning = dlg_choice(title_ssid, intro, 8, "X  Yes, scan now      O  No, type the address", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE);
    }
    for (;;) {
        char host[48] = "";
        int port = 8080;
        if (scanning) {
            nfound = scan_net(found, 8, 8080, own);
            if (nfound == 0) {
                static const char *none[] = { "No Kodi answered.", "", "In Kodi: Settings > Services > Control >", "Allow remote control via HTTP must be on", "(port 8080), and Kodi and this PSP must be on", "the same Wi-Fi network." };
                if (dlg_choice("No Kodi found", none, 6, "X  Scan again      O  Type the address", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE)) continue;
                scanning = 0;
            } else {
                for (i = 0; i < nfound; i++) { snprintf(labels_buf[i], sizeof(labels_buf[i]), "%s", found[i]); labels[i] = labels_buf[i]; }
                i = dlg_pick("Kodi found", labels, nfound, "X  Choose      SQUARE  Scan again      O  Type the address");
                if (i == -2) continue;
                if (i < 0) scanning = 0;
                else strncpy(host, found[i], sizeof(host) - 1);
            }
        }
        if (!scanning && !host[0]) {
            char typed[48];
            strncpy(typed, g_dhost, sizeof(typed) - 1); typed[sizeof(typed) - 1] = 0;
            i = osk_input("Kodi address, for example 192.168.1.20", typed, typed, sizeof(typed));
            if (i != 1 || !typed[0]) {
                static const char *quit[] = { "No Kodi address was entered.", "", "X  closes the app, O goes back." };
                if (dlg_choice("Kodi Remote", quit, 3, "X  Close the app      O  Back", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE)) return 0;
                scanning = 1;
                continue;
            }
            { char *colon = strchr(typed, ':'); if (colon) { *colon = 0; port = atoi(colon + 1); if (port <= 0) port = 8080; } }
            strncpy(host, typed, sizeof(host) - 1);
        }
        if (host[0]) {
            int r = direct_login(host, port);
            if (r == 1) {
                static const char *okp[] = { "Connected to Kodi.", "", "The address and login are saved on the card", "(direct.txt) so next time it connects by itself." };
                strncpy(g_dhost, host, sizeof(g_dhost) - 1); g_dport = port;
                save_direct();
                dlg_page("Kodi Remote", okp, 4, NULL, -1);
                sceKernelDelayThread(1500 * 1000);
                return 1;
            }
            if (r < 0) {
                static const char *bad[] = { "That address did not answer like Kodi.", "", "Check the address, and that Kodi's web server", "is on (Settings > Services > Control)." };
                dlg_choice("Not Kodi", bad, 4, "X  Try again", PSP_CTRL_CROSS | PSP_CTRL_CIRCLE, PSP_CTRL_CROSS | PSP_CTRL_CIRCLE);
            }
            scanning = 1;
        }
    }
}

/* the boot sequence of Kodi-alone mode: Wi-Fi, then the saved Kodi, else find one */
void direct_boot(void) {
    load_direct();
    direct_wifi();
    if (g_dhost[0]) {
        if (probe_host(g_dhost, g_dport, g_duser, g_dpass) == 1) return;                 /* the saved Kodi is there */
        {
            char l2[70];
            const char *l[] = { "The Kodi saved on the card did not answer:", l2, "", "It may be switched off, or its address changed.", "", "X  looks for Kodi again.", "O  keeps the saved address and carries on." };
            snprintf(l2, sizeof(l2), "%s:%d", g_dhost, g_dport);
            if (!dlg_choice("Kodi not found", l, 7, "X  Find Kodi      O  Use the saved address", PSP_CTRL_CROSS, PSP_CTRL_CIRCLE)) return;
        }
    }
    if (!direct_find()) sceKernelExitGame();
}
