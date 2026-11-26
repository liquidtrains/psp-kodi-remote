#include "direct.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

const char DK_PING[] = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"JSONRPC.Ping\"}";

/* ------------------------------------------------------------ tiny JSON helpers (enough for Kodi's replies) */
typedef struct { const char *s, *e; } Seg;

/* the value after "key": at depth 1 of the object s..e, or NULL */
static const char *obj_key1(const char *s, const char *e, const char *key) {
    int depth = 0, expect = 0;
    size_t kl = strlen(key);
    const char *p;
    for (p = s; p < e; p++) {
        char c = *p;
        if (c == '"') {
            const char *q = p + 1;
            while (q < e && *q != '"') { if (*q == '\\') q++; q++; }
            if (depth == 1 && expect) {
                const char *r = q + 1;
                while (r < e && *r == ' ') r++;
                if (r < e && *r == ':' && (size_t)(q - (p + 1)) == kl && !memcmp(p + 1, key, kl)) {
                    r++;
                    while (r < e && *r == ' ') r++;
                    return r;
                }
                expect = 0;
            }
            p = q;
            continue;
        }
        if (c == '{' || c == '[') { depth++; if (depth == 1) expect = 1; }
        else if (c == '}' || c == ']') depth--;
        else if (c == ',' && depth == 1) expect = 1;
    }
    return NULL;
}

/* the value after "key": anywhere inside s..e */
static const char *keyp(const char *s, const char *e, const char *key) {
    char pat[48];
    size_t pl;
    const char *p;
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    pl = strlen(pat);
    for (p = s; p + pl <= e; p++)
        if (*p == '"' && !memcmp(p, pat, pl)) { p += pl; while (p < e && *p == ' ') p++; return p; }
    return NULL;
}

static int jstr(const char *p, const char *e, char *out, int outsz) {
    int n = 0;
    out[0] = 0;
    if (!p || p >= e || *p != '"') return 0;
    p++;
    while (p < e && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\' && p < e) {
            c = (unsigned char)*p++;
            if (c == 'u') { p += 4; c = '?'; }
            else if (c == 'n' || c == 'r' || c == 't') c = ' ';
        }
        if ((c & 0xC0) == 0x80) continue;          /* UTF-8 continuation byte: the lead byte already became '?' */
        if (c >= 0xC0) c = '?';
        if (c < 0x20) c = ' ';
        if (n < outsz - 1) out[n++] = (char)c;
    }
    out[n] = 0;
    return 1;
}

/* split a reply (one object or a batch array) into its objects, indexed by their id */
static void split(const char *json, Seg *seg, int maxid) {
    int depth = 0, instr = 0;
    const char *p, *start = NULL;
    for (p = json; *p; p++) {
        if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
        if (*p == '"') { instr = 1; continue; }
        if (*p == '{') { if (depth == 0) start = p; depth++; }
        else if (*p == '}') {
            depth--;
            if (depth == 0 && start) {
                const char *idp = obj_key1(start, p + 1, "id");
                int id = idp ? atoi(idp) : -1;
                if (id >= 0 && id <= maxid) { seg[id].s = start; seg[id].e = p + 1; }
                start = NULL;
            }
        }
    }
}

/* ------------------------------------------------------------ text helpers */
static void urldecode(const char *in, char *out, int outsz) {
    int n = 0;
    for (; *in && n < outsz - 1; in++) {
        if (*in == '%' && in[1] && in[2]) {
            int hi = in[1] <= '9' ? in[1] - '0' : (in[1] | 32) - 'a' + 10, lo = in[2] <= '9' ? in[2] - '0' : (in[2] | 32) - 'a' + 10;
            out[n++] = (char)(hi * 16 + lo); in += 2;
        } else out[n++] = *in == '+' ? ' ' : *in;
    }
    out[n] = 0;
}
static void jescape(const char *in, char *out, int outsz) {
    int n = 0;
    for (; *in && n < outsz - 3; in++) {
        unsigned char c = (unsigned char)*in;
        if (c < 0x20 || c > 0x7e) continue;                         /* printable ASCII only */
        if (c == '"' || c == '\\') out[n++] = '\\';
        out[n++] = (char)c;
    }
    out[n] = 0;
}

/* ------------------------------------------------------------ requests */
static void one(char *body, int bodysz, const char *method, const char *params) {
    if (params) snprintf(body, bodysz, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\",\"params\":%s}", method, params);
    else snprintf(body, bodysz, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"}", method);
}


/* ------------------------------------------------------------ library (the same lists tvplay-api gives in server mode) */
#define LIB_PAGE 40
static int g_lib_show, g_lib_season;                                /* set by the request, used by the reply that follows it (one request at a time) */
static const char SORT_LABEL[] = "\"sort\":{\"method\":\"label\",\"order\":\"ascending\"}";
static const char FILT_PROGRESS[] = "\"filter\":{\"field\":\"inprogress\",\"operator\":\"true\",\"value\":\"\"}";

static int item_json(char *out, int outsz, char k, int id) {         /* {"movieid":N} or {"episodeid":N} */
    if (k != 'm' && k != 'e') return -1;
    snprintf(out, outsz, "{\"%s\":%d}", k == 'm' ? "movieid" : "episodeid", id);
    return 0;
}

static int lib_request(const char *arg, char *body, int bodysz, int *kind) {
    int a = 0, b = 0, c = 0, id = 0;
    char k = 0, item[40], params[400];
    if (!strcmp(arg, "movies") || sscanf(arg, "movies/%d", &a) == 1) {
        *kind = DK_LIB_MOVIES;
        if (a < 0 || a > 500) return -1;
        snprintf(params, sizeof(params), "{\"properties\":[\"year\",\"playcount\",\"resume\"],%s,\"limits\":{\"start\":%d,\"end\":%d}}", SORT_LABEL, a * LIB_PAGE, a * LIB_PAGE + LIB_PAGE);
        one(body, bodysz, "VideoLibrary.GetMovies", params);
        return 0;
    }
    if (!strcmp(arg, "shows") || sscanf(arg, "shows/%d", &a) == 1) {
        *kind = DK_LIB_SHOWS;
        if (a < 0 || a > 500) return -1;
        snprintf(params, sizeof(params), "{\"properties\":[\"episode\",\"watchedepisodes\"],%s,\"limits\":{\"start\":%d,\"end\":%d}}", SORT_LABEL, a * LIB_PAGE, a * LIB_PAGE + LIB_PAGE);
        one(body, bodysz, "VideoLibrary.GetTVShows", params);
        return 0;
    }
    if (sscanf(arg, "seasons/%d", &a) == 1 && a > 0) {
        *kind = DK_LIB_SEASONS;
        g_lib_show = a;
        snprintf(params, sizeof(params), "{\"tvshowid\":%d,\"properties\":[\"season\",\"episode\",\"watchedepisodes\"]}", a);
        one(body, bodysz, "VideoLibrary.GetSeasons", params);
        return 0;
    }
    if (sscanf(arg, "eps/%d/%d", &a, &b) == 2 && a > 0 && b >= 0) {
        *kind = DK_LIB_EPS;
        g_lib_show = a; g_lib_season = b;
        snprintf(params, sizeof(params), "{\"tvshowid\":%d,\"season\":%d,\"properties\":[\"title\",\"season\",\"episode\",\"playcount\",\"resume\"],"
                 "\"sort\":{\"method\":\"episode\",\"order\":\"ascending\"},\"limits\":{\"start\":0,\"end\":40}}", a, b);
        one(body, bodysz, "VideoLibrary.GetEpisodes", params);
        return 0;
    }
    if (!strcmp(arg, "continue")) {
        *kind = DK_LIB_CONT;
        snprintf(body, bodysz, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"VideoLibrary.GetEpisodes\",\"params\":{%s,\"properties\":[\"title\",\"showtitle\",\"season\",\"episode\"],\"limits\":{\"start\":0,\"end\":20}}},"
                 "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"VideoLibrary.GetMovies\",\"params\":{%s,\"properties\":[\"year\"],\"limits\":{\"start\":0,\"end\":10}}}]", FILT_PROGRESS, FILT_PROGRESS);
        return 0;
    }
    if (!strcmp(arg, "recent")) {
        *kind = DK_LIB_RECENT;
        snprintf(body, bodysz, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"VideoLibrary.GetRecentlyAddedEpisodes\",\"params\":{\"properties\":[\"title\",\"showtitle\",\"season\",\"episode\",\"playcount\",\"resume\"],\"limits\":{\"start\":0,\"end\":20}}},"
                 "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"VideoLibrary.GetRecentlyAddedMovies\",\"params\":{\"properties\":[\"year\",\"playcount\",\"resume\"],\"limits\":{\"start\":0,\"end\":10}}}]");
        return 0;
    }
    /* actions. The app knows the position of the playing item and the queue length from its status poll and sends them along:
     *   add/K/ID   next/K/ID/POS   play/K/ID/POS/QLEN   (POS -1 = nothing playing)   addseason/SHOW/SEASON */
    *kind = DK_PLAIN;
    if (sscanf(arg, "add/%c/%d", &k, &id) == 2 && item_json(item, sizeof(item), k, id) == 0) {
        snprintf(params, sizeof(params), "{\"playlistid\":1,\"item\":%s}", item);
        one(body, bodysz, "Playlist.Add", params);
        return 0;
    }
    if (sscanf(arg, "next/%c/%d/%d", &k, &id, &a) == 3 && a >= -1 && a < 1000 && item_json(item, sizeof(item), k, id) == 0) {
        if (a >= 0) snprintf(params, sizeof(params), "{\"playlistid\":1,\"position\":%d,\"item\":%s}", a + 1, item), one(body, bodysz, "Playlist.Insert", params);
        else snprintf(params, sizeof(params), "{\"playlistid\":1,\"item\":%s}", item), one(body, bodysz, "Playlist.Add", params);
        return 0;
    }
    if (sscanf(arg, "play/%c/%d/%d/%d", &k, &id, &a, &c) == 4 && a >= -1 && a < 1000 && c >= 0 && c < 1000 && item_json(item, sizeof(item), k, id) == 0) {
        *kind = DK_BATCH;
        if (a >= 0)                                          /* something is playing: put the item after it and step to it */
            snprintf(body, bodysz, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Playlist.Insert\",\"params\":{\"playlistid\":1,\"position\":%d,\"item\":%s}},"
                     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Player.GoTo\",\"params\":{\"playerid\":1,\"to\":\"next\"}}]", a + 1, item);
        else                                                 /* nothing playing: add it at the end and open that position */
            snprintf(body, bodysz, "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Playlist.Add\",\"params\":{\"playlistid\":1,\"item\":%s}},"
                     "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Player.Open\",\"params\":{\"item\":{\"playlistid\":1,\"position\":%d}}}]", item, c);
        return 0;
    }
    if (sscanf(arg, "addseason/%d/%d", &a, &b) == 2 && a > 0 && b >= 0) {        /* Kodi's own season folder: every episode, in order */
        snprintf(params, sizeof(params), "{\"playlistid\":1,\"item\":{\"directory\":\"videodb://tvshows/titles/%d/%d/\"}}", a, b);
        one(body, bodysz, "Playlist.Add", params);
        return 0;
    }
    return -1;
}

static const char STATUS_BATCH[] =
    "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetActivePlayers\"},"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Application.GetProperties\",\"params\":{\"properties\":[\"volume\",\"muted\"]}},"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"Player.GetItem\",\"params\":{\"playerid\":1,\"properties\":[\"title\"]}},"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":1,\"properties\":[\"percentage\",\"speed\",\"position\"]}},"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"Playlist.GetItems\",\"params\":{\"playlistid\":1,\"limits\":{\"start\":0,\"end\":1}}},"
    "{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"Player.GetItem\",\"params\":{\"playerid\":0,\"properties\":[\"title\"]}},"
    "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":0,\"properties\":[\"percentage\",\"speed\"]}}]";

static const char QUEUE_BATCH[] =
    "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Playlist.GetItems\",\"params\":{\"playlistid\":1,\"properties\":[\"title\"],\"limits\":{\"start\":0,\"end\":128}}},"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":1,\"properties\":[\"playlistid\",\"position\"]}}]";

int dk_request(const char *method, const char *path, char *body, int bodysz, int *kind) {
    char verb[16] = "", arg[24] = "";
    const char *p = path;
    int n;
    (void)method;
    if (strncmp(p, "/t/", 3)) return -1;
    if (!strncmp(path, "/t/text?s=", 10)) {         /* /t/text?s=hello%20there */
        char raw[130], dec[130], esc[260], params[300];
        const char *q = path + 7;
        strncpy(raw, q + 3, sizeof(raw) - 1); raw[sizeof(raw) - 1] = 0;
        urldecode(raw, dec, sizeof(dec));
        jescape(dec, esc, sizeof(esc));
        if (!esc[0]) return -1;
        snprintf(params, sizeof(params), "{\"text\":\"%s\",\"done\":false}", esc);
        *kind = DK_PLAIN;
        one(body, bodysz, "Input.SendText", params);
        return 0;
    }
    p += 3;
    n = (int)strcspn(p, "/");
    if (n <= 0 || n >= (int)sizeof(verb)) return -1;
    memcpy(verb, p, n); verb[n] = 0;
    if (p[n] == '/') { strncpy(arg, p + n + 1, sizeof(arg) - 1); arg[sizeof(arg) - 1] = 0; }
    *kind = DK_PLAIN;

    if (!strcmp(verb, "lib")) return lib_request(arg, body, bodysz, kind);
    if (!strcmp(verb, "tracks")) {
        *kind = DK_TRACKS;
        snprintf(body, bodysz, "%s",
            "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Player.GetActivePlayers\"},"
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":1,\"properties\":[\"audiostreams\",\"currentaudiostream\",\"subtitles\",\"currentsubtitle\",\"subtitleenabled\"]}},"
            "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"Player.GetProperties\",\"params\":{\"playerid\":0,\"properties\":[\"audiostreams\",\"currentaudiostream\",\"subtitles\",\"currentsubtitle\",\"subtitleenabled\"]}}]");
        return 0;
    }
    if (!strcmp(verb, "track")) {
        int n;
        char params[100];
        if (sscanf(arg, "a/%d", &n) == 1) { snprintf(params, sizeof(params), "{\"playerid\":1,\"stream\":%d}", n); one(body, bodysz, "Player.SetAudioStream", params); return 0; }
        if (!strcmp(arg, "s/off")) { one(body, bodysz, "Player.SetSubtitle", "{\"playerid\":1,\"subtitle\":\"off\"}"); return 0; }
        if (sscanf(arg, "s/%d", &n) == 1) { snprintf(params, sizeof(params), "{\"playerid\":1,\"subtitle\":%d,\"enable\":true}", n); one(body, bodysz, "Player.SetSubtitle", params); return 0; }
        return -1;
    }
    if (!strcmp(verb, "power")) {                                      /* the app asks for a second press first; Kodi has no guard of its own */
        if (!strncmp(arg, "refresh", 7)) { one(body, bodysz, "VideoLibrary.Scan", "{}"); return 0; }
        if (strstr(arg, "/confirm")) {
            if (!strncmp(arg, "restartkodi", 11)) { one(body, bodysz, "Application.Quit", NULL); return 0; }
            if (!strncmp(arg, "reboot", 6)) { one(body, bodysz, "System.Reboot", NULL); return 0; }
            if (!strncmp(arg, "shutdown", 8)) { one(body, bodysz, "System.Shutdown", NULL); return 0; }
        }
        return -1;
    }
    if (!strcmp(verb, "status")) { *kind = DK_STATUS; snprintf(body, bodysz, "%s", STATUS_BATCH); return 0; }
    if (!strcmp(verb, "queue")) { *kind = DK_QUEUE; snprintf(body, bodysz, "%s", QUEUE_BATCH); return 0; }
    if (!strcmp(verb, "stop")) { one(body, bodysz, "Input.ExecuteAction", "{\"action\":\"stop\"}"); return 0; }
    if (!strcmp(verb, "key")) {
        static const char *const keys[][2] = { {"up", "Input.Up"}, {"down", "Input.Down"}, {"left", "Input.Left"}, {"right", "Input.Right"},
            {"select", "Input.Select"}, {"back", "Input.Back"}, {"home", "Input.Home"}, {"info", "Input.Info"}, {"context", "Input.ContextMenu"} };
        unsigned i;
        for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(arg, keys[i][0])) { one(body, bodysz, keys[i][1], NULL); return 0; }
        return -1;
    }
    if (!strcmp(verb, "do")) {
        if (!strcmp(arg, "playpause")) { one(body, bodysz, "Input.ExecuteAction", "{\"action\":\"playpause\"}"); return 0; }
        if (!strcmp(arg, "next")) { one(body, bodysz, "Input.ExecuteAction", "{\"action\":\"skipnext\"}"); return 0; }
        if (!strcmp(arg, "prev")) { one(body, bodysz, "Input.ExecuteAction", "{\"action\":\"skipprevious\"}"); return 0; }
        if (!strcmp(arg, "volup")) { *kind = DK_VOL; one(body, bodysz, "Application.SetVolume", "{\"volume\":\"increment\"}"); return 0; }
        if (!strcmp(arg, "voldown")) { *kind = DK_VOL; one(body, bodysz, "Application.SetVolume", "{\"volume\":\"decrement\"}"); return 0; }
        if (!strcmp(arg, "mute")) { *kind = DK_MUTE; one(body, bodysz, "Application.SetMute", "{\"mute\":\"toggle\"}"); return 0; }
        if (!strcmp(arg, "fullscreen")) { *kind = DK_FULL; one(body, bodysz, "GUI.SetFullscreen", "{\"fullscreen\":true}"); return 0; }
        return -1;
    }
    if (!strcmp(verb, "q")) {                                       /* queue editing: remove, or move as a chain of adjacent swaps */
        int a, b;
        if (sscanf(arg, "remove/%d", &a) == 1 && a >= 0 && a < 1000) {
            char params[80];
            snprintf(params, sizeof(params), "{\"playlistid\":1,\"position\":%d}", a);
            one(body, bodysz, "Playlist.Remove", params);
            return 0;
        }
        if (!strcmp(arg, "clear/all")) { one(body, bodysz, "Playlist.Clear", "{\"playlistid\":1}"); return 0; }
        if (sscanf(arg, "clear/keep/%d/%d", &a, &b) == 2 && a >= -1 && a < 128 && b > 0 && b <= 128) {      /* a = playing position or -1, b = queue length */
            int i, id = 1, len = 0;
            *kind = DK_BATCH;
            len += snprintf(body + len, bodysz - len, "[");
            for (i = b - 1; i >= 0; i--) {
                if (i == a) continue;
                if (len > bodysz - 160) return -1;
                len += snprintf(body + len, bodysz - len, "%s{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"Playlist.Remove\",\"params\":{\"playlistid\":1,\"position\":%d}}", id > 1 ? "," : "", id, i);
                id++;
            }
            if (id == 1) { one(body, bodysz, "JSONRPC.Ping", NULL); return 0; }
            snprintf(body + len, bodysz - len, "]");
            return 0;
        }
        if (sscanf(arg, "move/%d/%d", &a, &b) == 2 && a >= 0 && b >= 0 && a < 128 && b < 128) {
            int i, step = b < a ? -1 : 1, len = 0, id = 1;
            if (a == b) { one(body, bodysz, "JSONRPC.Ping", NULL); return 0; }
            *kind = DK_BATCH;
            len += snprintf(body + len, bodysz - len, "[");
            for (i = a; i != b; i += step, id++) {
                if (len > bodysz - 160) return -1;
                len += snprintf(body + len, bodysz - len, "%s{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"Playlist.Swap\",\"params\":{\"playlistid\":1,\"position1\":%d,\"position2\":%d}}",
                                id > 1 ? "," : "", id, i, i + step);
            }
            snprintf(body + len, bodysz - len, "]");
            return 0;
        }
        return -1;
    }
    if (!strcmp(verb, "goto") && arg[0] >= '0' && arg[0] <= '9') {
        char params[96];
        snprintf(params, sizeof(params), "{\"item\":{\"playlistid\":1,\"position\":%d}}", atoi(arg));
        one(body, bodysz, "Player.Open", params);
        return 0;
    }
    return -1;
}

/* ------------------------------------------------------------ replies */

/* ------------------------------------------------------------ library replies */
typedef struct { char *out; int outsz, len, force; } LibOut;      /* force > 0: every line gets that flag (Continue watching) */

/* calls fn for each object of the array after "key": inside s..e */
static void each_obj(const char *s, const char *e, const char *key, void (*fn)(const char *, const char *, LibOut *), LibOut *lo) {
    const char *p = keyp(s, e, key), *start = NULL;
    int depth = 0, instr = 0;
    if (!p || *p != '[') return;
    for (; p < e; p++) {
        if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
        if (*p == '"') { instr = 1; continue; }
        if (*p == '[') depth++;
        else if (*p == ']') { if (--depth == 0) return; }
        else if (*p == '{') { if (++depth == 2) start = p; }
        else if (*p == '}') { if (depth-- == 2 && start) { fn(start, p + 1, lo); start = NULL; } }
    }
}

static int num_of(const char *s, const char *e, const char *key) { const char *v = keyp(s, e, key); return v ? atoi(v) : 0; }

static int partly(const char *s, const char *e) {                   /* a resume point past the start */
    const char *r = keyp(s, e, "resume"), *v;
    if (!r || *r != '{') return 0;
    v = keyp(r, e, "position");
    return v && atof(v) > 0.5;
}

static void lib_put(LibOut *lo, char kind, int id, const char *label, int flag, const char *extra) {
    char clean[100];
    int i;
    if (lo->len >= lo->outsz - 130) return;
    snprintf(clean, sizeof(clean), "%s", label);
    for (i = 0; clean[i]; i++) if (clean[i] == '|') clean[i] = '/';
    if (lo->force) flag = lo->force;
    lo->len += snprintf(lo->out + lo->len, lo->outsz - lo->len, "%c|%d|%s|%d|%s\n", kind, id, clean, flag, extra ? extra : "");
}

static void ob_movie(const char *s, const char *e, LibOut *lo) {
    char title[80], label[100];
    const char *t = keyp(s, e, "label");
    int year = num_of(s, e, "year");
    if (!t || !jstr(t, e, title, sizeof(title))) return;
    if (year > 0) snprintf(label, sizeof(label), "%.70s (%d)", title, year); else snprintf(label, sizeof(label), "%s", title);
    lib_put(lo, 'm', num_of(s, e, "movieid"), label, partly(s, e) ? 2 : (num_of(s, e, "playcount") > 0), "");
}

static void ob_show(const char *s, const char *e, LibOut *lo) {
    char title[80], label[110];
    const char *t = keyp(s, e, "label");
    int n = num_of(s, e, "episode"), w = num_of(s, e, "watchedepisodes");
    if (!t || !jstr(t, e, title, sizeof(title)) || n <= 0) return;
    snprintf(label, sizeof(label), "%.60s - %d episodes, %d seen", title, n, w);
    lib_put(lo, 's', num_of(s, e, "tvshowid"), label, w >= n, "");
}

static void ob_season(const char *s, const char *e, LibOut *lo) {
    char label[80], extra[16];
    int sn = num_of(s, e, "season"), n = num_of(s, e, "episode"), w = num_of(s, e, "watchedepisodes");
    if (sn < 0) return;
    if (sn == 0) snprintf(label, sizeof(label), "Specials - %d episodes, %d seen", n, w); else snprintf(label, sizeof(label), "Season %d - %d episodes, %d seen", sn, n, w);
    snprintf(extra, sizeof(extra), "%d", g_lib_show);
    lib_put(lo, 'n', sn, label, n > 0 && w >= n, extra);
}

static void ob_episode(const char *s, const char *e, LibOut *lo) {
    char title[70], show[50], label[110];
    const char *t = keyp(s, e, "title"), *st = keyp(s, e, "showtitle");
    int season = num_of(s, e, "season"), ep = num_of(s, e, "episode");
    if (!t || !jstr(t, e, title, sizeof(title))) return;
    if (st && jstr(st, e, show, sizeof(show)) && show[0]) snprintf(label, sizeof(label), "%.40s S%02dE%02d %.40s", show, season, ep, title);   /* lists that mix shows */
    else snprintf(label, sizeof(label), "E%02d  %.80s", ep, title);
    lib_put(lo, 'e', num_of(s, e, "episodeid"), label, partly(s, e) ? 2 : (num_of(s, e, "playcount") > 0), "");
}

static int lib_reply(int kind, Seg *seg, char *out, int outsz) {
    LibOut lo;
    const char *lim;
    int total = 0, start = 0, page = 0;
    lo.out = out; lo.outsz = outsz; lo.len = 0; lo.force = kind == DK_LIB_CONT ? 2 : 0; out[0] = 0;
    if (kind == DK_LIB_MOVIES || kind == DK_LIB_SHOWS) {
        lim = keyp(seg[1].s, seg[1].e, "limits");
        if (lim) { total = num_of(lim, seg[1].e, "total"); start = num_of(lim, seg[1].e, "start"); }
        page = start / LIB_PAGE;
        each_obj(seg[1].s, seg[1].e, kind == DK_LIB_MOVIES ? "movies" : "tvshows", kind == DK_LIB_MOVIES ? ob_movie : ob_show, &lo);
        if (total > start + LIB_PAGE) {
            char label[60];
            snprintf(label, sizeof(label), "More %s (%d left)", kind == DK_LIB_MOVIES ? "movies" : "shows", total - start - LIB_PAGE);
            lib_put(&lo, 'p', page + 1, label, 0, kind == DK_LIB_MOVIES ? "movies" : "shows");
        }
    } else if (kind == DK_LIB_SEASONS) {
        each_obj(seg[1].s, seg[1].e, "seasons", ob_season, &lo);
    } else if (kind == DK_LIB_EPS) {
        char label[70], extra[16];
        lim = keyp(seg[1].s, seg[1].e, "limits");
        if (lim) total = num_of(lim, seg[1].e, "total");
        if (total > 0) {                                              /* the first row adds the whole season (Kodi's own season folder, in order) */
            snprintf(label, sizeof(label), "Add all %d episodes to the queue", total);
            snprintf(extra, sizeof(extra), "%d", g_lib_show);
            lib_put(&lo, 'a', g_lib_season, label, 0, extra);
        }
        each_obj(seg[1].s, seg[1].e, "episodes", ob_episode, &lo);
    } else {                                                          /* DK_LIB_CONT and DK_LIB_RECENT: episodes first, then movies */
        each_obj(seg[1].s, seg[1].e, "episodes", ob_episode, &lo);
        if (seg[2].s) each_obj(seg[2].s, seg[2].e, "movies", ob_movie, &lo);
    }
    return 200;
}

static int put(char *out, int outsz, const char *s) { snprintf(out, outsz, "%s", s); return 0; }

int dk_reply(int kind, int code, const char *json, char *out, int outsz) {
    Seg seg[9];
    char msg[120];
    int i;
    memset(seg, 0, sizeof(seg));
    out[0] = 0;
    if (code == 401) { put(out, outsz, "Kodi refused the user name or password"); return 401; }
    if (code != 200) { snprintf(out, outsz, "Kodi answered HTTP %d", code); return code ? code : 0; }
    if (kind == DK_BATCH) {                                         /* any error object in the array fails the whole edit */
        int depth = 0, instr = 0, seen = 0;
        const char *p, *start = NULL;
        for (p = json; *p; p++) {
            if (instr) { if (*p == '\\' && p[1]) p++; else if (*p == '"') instr = 0; continue; }
            if (*p == '"') { instr = 1; continue; }
            if (*p == '{') { if (depth == 0) start = p; depth++; }
            else if (*p == '}' && --depth == 0 && start) {
                const char *err = obj_key1(start, p + 1, "error");
                seen++;
                if (err) {
                    const char *mm = keyp(err, p + 1, "message");
                    if (mm && jstr(mm, p + 1, msg, sizeof(msg))) snprintf(out, outsz, "Kodi: %s", msg); else put(out, outsz, "Kodi refused that");
                    return 502;
                }
                start = NULL;
            }
        }
        if (!seen) { put(out, outsz, "unexpected reply (is this Kodi?)"); return 502; }
        put(out, outsz, "ok");
        return 200;
    }
    split(json, seg, 8);
    if (!seg[1].s) { put(out, outsz, "unexpected reply (is this Kodi?)"); return 502; }

    if (kind >= DK_LIB_MOVIES && kind <= DK_LIB_RECENT) {
        const char *err = obj_key1(seg[1].s, seg[1].e, "error");
        if (err) {
            const char *mm = keyp(err, seg[1].e, "message");
            if (mm && jstr(mm, seg[1].e, msg, sizeof(msg))) snprintf(out, outsz, "Kodi: %s", msg); else put(out, outsz, "Kodi refused that");
            return 502;
        }
        return lib_reply(kind, seg, out, outsz);
    }
    if (kind != DK_STATUS && kind != DK_QUEUE && kind != DK_TRACKS) {
        const char *err = obj_key1(seg[1].s, seg[1].e, "error");
        const char *res = obj_key1(seg[1].s, seg[1].e, "result");
        if (err) {
            const char *m = keyp(err, seg[1].e, "message");
            if (m && jstr(m, seg[1].e, msg, sizeof(msg))) snprintf(out, outsz, "Kodi: %s", msg);
            else put(out, outsz, "Kodi refused that");
            return 502;
        }
        if (kind == DK_VOL) { snprintf(out, outsz, "Volume %d", res ? atoi(res) : -1); return 200; }
        if (kind == DK_MUTE) { put(out, outsz, res && !strncmp(res, "true", 4) ? "Muted" : "Sound on"); return 200; }
        if (kind == DK_FULL) { put(out, outsz, res && !strncmp(res, "true", 4) ? "Fullscreen" : "Nothing playing"); return 200; }
        put(out, outsz, "ok");
        return 200;
    }

    if (kind == DK_STATUS) {
        const char *pid = keyp(seg[1].s, seg[1].e, "playerid");
        int playing = pid != NULL, player = pid ? atoi(pid) : -1, vol = -1, muted = 0, qlen = 0, paused = 0, position = -1;
        float pct = 0;
        char title[96] = "";
        if (seg[2].s) {
            const char *v = keyp(seg[2].s, seg[2].e, "volume"), *m = keyp(seg[2].s, seg[2].e, "muted");
            if (v) vol = atoi(v);
            muted = m && !strncmp(m, "true", 4);
        }
        if (seg[5].s) { const char *t = keyp(seg[5].s, seg[5].e, "total"); if (t) qlen = atoi(t); }
        if (playing && (player == 1 || player == 0)) {
            Seg *it = &seg[player == 1 ? 3 : 6], *pr = &seg[player == 1 ? 4 : 7];
            if (it->s) {
                const char *t = keyp(it->s, it->e, "title");
                if (!(t && jstr(t, it->e, title, sizeof(title)) && title[0])) {
                    const char *l = keyp(it->s, it->e, "label");
                    if (l) jstr(l, it->e, title, sizeof(title));
                }
            }
            if (pr->s) {
                const char *pc = keyp(pr->s, pr->e, "percentage"), *sp = keyp(pr->s, pr->e, "speed");
                if (pc) pct = (float)atof(pc);
                paused = sp && atoi(sp) == 0;
                if (player == 1) { const char *po = keyp(pr->s, pr->e, "position"); if (po) position = atoi(po); }
            }
        }
        snprintf(out, outsz, "playing=%d\npaused=%d\nmuted=%d\nvolume=%d\nqueue_len=%d\npercent=%.1f\nposition=%d\ntitle=%s\n",
                 playing, playing && paused, muted, vol, qlen, pct, position, title);
        return 200;
    }

    if (kind == DK_TRACKS) {
        const char *pid = keyp(seg[1].s, seg[1].e, "playerid");
        Seg *pr;
        int len = 0, cur_a = -1, cur_s = -1, en;
        const char *v, *e;
        if (!pid) { put(out, outsz, "nothing is playing"); return 409; }
        pr = &seg[atoi(pid) == 1 ? 2 : 3];
        if (!pr->s) { put(out, outsz, "nothing is playing"); return 409; }
        e = pr->e;
        v = keyp(pr->s, e, "currentaudiostream"); if (v && *v == '{') { const char *ix = keyp(v, e, "index"); if (ix) cur_a = atoi(ix); }
        v = keyp(pr->s, e, "currentsubtitle"); if (v && *v == '{' && v[1] != '}') { const char *ix = keyp(v, e, "index"); if (ix) cur_s = atoi(ix); }
        v = keyp(pr->s, e, "subtitleenabled"); en = v && !strncmp(v, "true", 4);
        for (i = 0; i < 2; i++) {                                         /* 0: audio streams, 1: subtitles */
            const char *arr = keyp(pr->s, e, i ? "subtitles" : "audiostreams"), *p, *ae;
            int depth = 0;
            if (i) len += snprintf(out + len, outsz - len, "s|off|Subtitles off|%d\n", !en);
            if (!arr || *arr != '[') continue;
            for (ae = arr; ae < e; ae++) { if (*ae == '[') depth++; else if (*ae == ']' && --depth == 0) break; }
            for (p = arr + 1; p < ae; ) {
                const char *os = memchr(p, '{', ae - p), *oe;
                int d2 = 0, idx;
                char lang[24], name[48], label[96];
                const char *lv, *nv, *cv, *xv;
                if (!os) break;
                for (oe = os; oe < ae; oe++) { if (*oe == '{') d2++; else if (*oe == '}' && --d2 == 0) break; }
                xv = keyp(os, oe, "index"); idx = xv ? atoi(xv) : 0;
                lv = keyp(os, oe, "language"); nv = keyp(os, oe, "name"); cv = keyp(os, oe, "channels");
                lang[0] = name[0] = 0;
                if (lv) jstr(lv, oe, lang, sizeof(lang));
                if (nv) jstr(nv, oe, name, sizeof(name));
                { int k; for (k = 0; lang[k]; k++) if (lang[k] >= 'a' && lang[k] <= 'z') lang[k] -= 32; }
                snprintf(label, sizeof(label), "%s%s%s%s", lang, lang[0] && name[0] ? " " : "", name, (!i && cv && atoi(cv) > 0) ? "" : "");
                if (!i && cv && atoi(cv) > 0) { int l2 = (int)strlen(label); snprintf(label + l2, sizeof(label) - l2, " %dch", atoi(cv)); }
                if (!label[0]) snprintf(label, sizeof(label), "track %d", idx);
                for (d2 = 0; label[d2]; d2++) if (label[d2] == '|') label[d2] = '/';
                if (len < outsz - 100) len += snprintf(out + len, outsz - len, "%c|%d|%s|%d\n", i ? 's' : 'a', idx, label, i ? (en && idx == cur_s) : (idx == cur_a));
                p = oe + 1;
            }
        }
        return 200;
    }

    /* DK_QUEUE: "cur=N" then "position|title" lines */
    {
        int cur = -1, n = 0, len;
        const char *pl, *pos, *p, *e = seg[1].e;
        if (seg[2].s) {
            pl = keyp(seg[2].s, seg[2].e, "playlistid");
            pos = keyp(seg[2].s, seg[2].e, "position");
            if (pl && pos && atoi(pl) == 1) cur = atoi(pos);
        }
        len = snprintf(out, outsz, "cur=%d\n", cur);
        for (p = seg[1].s; p && (p = keyp(p, e, "label")) != NULL; ) {
            char label[72];
            if (!jstr(p, e, label, sizeof(label))) break;
            for (i = 0; label[i]; i++) if (label[i] == '|') label[i] = '/';
            if (len >= outsz - 90) break;
            len += snprintf(out + len, outsz - len, "%d|%s\n", n++, label);
            while (p < e && *p == '"') { p++; while (p < e && *p != '"') { if (*p == '\\') p++; p++; } p++; }
        }
        return 200;
    }
}

/* ------------------------------------------------------------ HTTP framing and probing */
static const char *cifind(const char *h, const char *n) {       /* case-insensitive strstr (not every libc has the GNU one) */
    size_t nl = strlen(n), i;
    for (; *h; h++) {
        for (i = 0; i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i]); i++) ;
        if (i == nl) return h;
    }
    return NULL;
}

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static void b64(const char *in, char *out, int outsz) {
    int n = (int)strlen(in), i, o = 0;
    for (i = 0; i < n && o + 4 < outsz; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < n) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < n ? B64[v & 63] : '=';
    }
    out[o] = 0;
}

int dk_http(char *out, int outsz, const char *host, int port, const char *user, const char *pass, const char *body) {
    char cred[160], enc[240], auth[280] = "";
    if (user && user[0]) {
        snprintf(cred, sizeof(cred), "%s:%s", user, pass ? pass : "");
        b64(cred, enc, sizeof(enc));
        snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", enc);
    }
    return snprintf(out, outsz, "POST /jsonrpc HTTP/1.0\r\nHost: %s:%d\r\n%sContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
                    host, port, auth, (int)strlen(body), body);
}

int dk_probe(const char *resp) {
    int code = 0;
    if (strncmp(resp, "HTTP/1.", 7) || sscanf(resp, "HTTP/1.%*d %d", &code) != 1) return 0;
    if (code == 200) return strstr(resp, "pong") ? 1 : 0;
    if (code == 401) {
        const char *h = resp, *end = strstr(resp, "\r\n\r\n");
        size_t hl = end ? (size_t)(end - resp) : strlen(resp);
        char *copy = (char *)malloc(hl + 1);
        int ok = 0;
        (void)h;
        if (!copy) return 0;
        memcpy(copy, resp, hl); copy[hl] = 0;
        {
            const char *w = cifind(copy, "www-authenticate");
            if (w && (cifind(w, "realm=\"kodi\"") || cifind(w, "realm=\"xbmc\""))) ok = 1;
        }
        free(copy);
        return ok ? 2 : 0;
    }
    return 0;
}
