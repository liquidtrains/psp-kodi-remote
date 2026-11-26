#include "app.h"

/* Library browsing, in both modes: Continue watching, Recently added, Movies, TV shows > seasons > episodes.
 * Server mode: tvplay-api sends lines "kind|id|label|flag|extra" (GET /t/lib/...). Kodi alone: direct.c builds the same lines from Kodi's own JSON-RPC.
 * An item's kind and extra go in its tag. */

typedef struct { char path[96]; char title[48]; int sel; } LibLevel;
static LibLevel g_stk[6];
static int g_depth;
char g_lib_path[96] = "";                       /* "" = the top menu of the library */
char g_lib_title[48] = "Library";
static int g_lib_sel;                           /* the cursor row of the list an action menu was opened from */
static char g_lkind;                            /* 'm' or 'e' of the item in the action menu */
static int g_lid;
static char g_ltitle[72];

void lib_open_root(void) {
    g_depth = 0;
    g_lib_path[0] = 0;
    snprintf(g_lib_title, sizeof(g_lib_title), "Library");
    menu_set(S_LIB, g_lib_title);
    add("Continue watching", "c", 0, 0);
    add("Recently added", "r", 0, 0);
    add("Movies", "M", 0, 0);
    add("TV shows", "S", 0, 0);
}

static void lib_show(const char *path_in, const char *title_in, int want) {
    char path[96], title[48];                                    /* copies: the callers often pass g_lib_path and g_lib_title themselves */
    snprintf(path, sizeof(path), "%s", path_in);
    snprintf(title, sizeof(title), "%.46s", title_in);
    snprintf(g_lib_path, sizeof(g_lib_path), "%s", path);
    snprintf(g_lib_title, sizeof(g_lib_title), "%s", title);
    g_want_sel = want;
    load_lines(S_LIB, g_lib_path, g_lib_title);
}

static void lib_go(const char *path, const char *title) {       /* one level down: remember where we were */
    if (g_depth < 6) {
        snprintf(g_stk[g_depth].path, sizeof(g_stk[0].path), "%s", g_lib_path);
        snprintf(g_stk[g_depth].title, sizeof(g_stk[0].title), "%s", g_lib_title);
        g_stk[g_depth].sel = g_sel;
        g_depth++;
    }
    lib_show(path, title, -1);
}

void lib_back(void) {                                            /* SELECT inside the library */
    if (g_depth <= 0) { open_main(); return; }
    g_depth--;
    if (!g_stk[g_depth].path[0]) {
        int s = g_stk[g_depth].sel;
        lib_open_root();
        g_sel = s < g_n ? s : 0;
        return;
    }
    {
        char path[96], title[48];
        int s = g_stk[g_depth].sel;
        snprintf(path, sizeof(path), "%s", g_stk[g_depth].path);
        snprintf(title, sizeof(title), "%s", g_stk[g_depth].title);
        lib_show(path, title, s);
    }
}

static void lib_actions(const Item *it) {                        /* a movie or an episode was chosen */
    g_lib_sel = g_sel;
    g_lkind = it->tag[0];
    g_lid = it->id;
    snprintf(g_ltitle, sizeof(g_ltitle), "%.70s", it->label);
    menu_set(S_LACT, g_ltitle);
    add("Play now", "", 1, 0);
    add("Play next (after the one playing)", "", 2, 0);
    add("Add to the end of the queue", "", 3, 0);
}

void lib_actions_back(void) {                                    /* SELECT in the action menu: back to the list, same row */
    lib_show(g_lib_path, g_lib_title, g_lib_sel);
}

void lib_select(Item *it) {
    char path[120], title[60];
    switch (it->tag[0]) {
    case 'c': lib_go("/t/lib/continue", "Continue watching"); break;
    case 'r': lib_go("/t/lib/recent", "Recently added"); break;
    case 'M': lib_go("/t/lib/movies", "Movies"); break;
    case 'S': lib_go("/t/lib/shows", "TV shows"); break;
    case 's':
        snprintf(path, sizeof(path), "/t/lib/seasons/%d", it->id);
        snprintf(title, sizeof(title), "%.50s", it->label);
        { char *dash = strstr(title, " - "); if (dash) *dash = 0; }       /* the show name, without the counts */
        lib_go(path, title);
        break;
    case 'n':
        snprintf(path, sizeof(path), "/t/lib/eps/%s/%d", it->tag + 1, it->id);
        snprintf(title, sizeof(title), "%s", it->id == 0 ? "Specials" : "Season");
        if (it->id) snprintf(title, sizeof(title), "Season %d", it->id);
        lib_go(path, title);
        break;
    case 'a':
        snprintf(path, sizeof(path), "/t/lib/addseason/%s/%d", it->tag + 1, it->id);
        act("POST", path, 40, "Adding the season");
        break;
    case 'p':                                                    /* more of a long list: replace this page, not a new level */
        snprintf(path, sizeof(path), "/t/lib/%s/%d", it->tag + 1, it->id);
        lib_show(path, g_lib_title, 0);
        break;
    case 'm':
    case 'e':
        lib_actions(it);
        break;
    }
}

void lib_act(Item *it) {                                         /* choice in the action menu */
    char path[96];
    const char *verb = it->id == 1 ? "play" : (it->id == 2 ? "next" : "add");
    if (g_direct && it->id != 3) {                                /* Kodi alone: no server to look up the queue, so send what the status poll knows */
        const Stat *s = &g_stat[g_cur];
        int pos = (s->ok && s->playing && s->pos >= 0) ? s->pos : -1;
        if (it->id == 1) snprintf(path, sizeof(path), "/t/lib/play/%c/%d/%d/%d", g_lkind, g_lid, pos, s->qlen);
        else snprintf(path, sizeof(path), "/t/lib/next/%c/%d/%d", g_lkind, g_lid, pos);
    } else {
        snprintf(path, sizeof(path), "/t/lib/%s/%c/%d", verb, g_lkind, g_lid);
    }
    act("POST", path, 30, it->id == 1 ? "Playing" : (it->id == 2 ? "Playing next" : "Added to the queue"));
    if (it->id != 1) lib_show(g_lib_path, g_lib_title, g_lib_sel);   /* back to the list for the next pick; play now stays put */
}
