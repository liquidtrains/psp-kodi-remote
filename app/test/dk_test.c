/* Host test for direct.c. "dk_test self" checks canned Kodi replies; "dk_test live HOST PORT   (login from the DK_USER and DK_PASS environment variables)" runs every UI request against a real Kodi. */
#include "../direct.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <sys/socket.h>

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("FAIL %s\n", what); fails++; } else printf("ok   %s\n", what); } while (0)

static int has(const char *h, const char *n) { return strstr(h, n) != NULL; }

static void self(void) {
    char out[4096], body[2048]; int kind, code;
    /* requests */
    CHECK(dk_request("POST", "/t/key/up", body, sizeof body, &kind) == 0 && has(body, "Input.Up") && kind == DK_PLAIN, "key up -> Input.Up");
    CHECK(dk_request("POST", "/t/key/context", body, sizeof body, &kind) == 0 && has(body, "Input.ContextMenu"), "key context");
    CHECK(dk_request("POST", "/t/key/bogus", body, sizeof body, &kind) == -1, "unknown key refused");
    CHECK(dk_request("POST", "/t/do/volup", body, sizeof body, &kind) == 0 && kind == DK_VOL && has(body, "increment"), "volup");
    CHECK(dk_request("POST", "/t/do/fullscreen", body, sizeof body, &kind) == 0 && kind == DK_FULL && has(body, "GUI.SetFullscreen"), "fullscreen");
    CHECK(dk_request("POST", "/t/do/playpause", body, sizeof body, &kind) == 0 && has(body, "playpause"), "playpause");
    CHECK(dk_request("POST", "/t/stop", body, sizeof body, &kind) == 0 && has(body, "\"stop\""), "stop");
    CHECK(dk_request("POST", "/t/goto/7", body, sizeof body, &kind) == 0 && has(body, "\"position\":7"), "goto 7");
    CHECK(dk_request("POST", "/t/random/bobs", body, sizeof body, &kind) == -1, "random needs the server");
    CHECK(dk_request("GET", "/t/channels", body, sizeof body, &kind) == -1, "channels needs the server");
    CHECK(dk_request("POST", "/t/sleep/30", body, sizeof body, &kind) == -1, "sleep needs the server");
    /* replies: status, playing a paused local episode on the video player */
    const char *st = "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[{\"playerid\":1,\"type\":\"video\"}]},"
        "{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"muted\":true,\"volume\":35}},"
        "{\"id\":3,\"jsonrpc\":\"2.0\",\"result\":{\"item\":{\"label\":\"S01E02\",\"title\":\"The \\\"Pilot\\\" \\u00e9\",\"type\":\"episode\"}}},"
        "{\"id\":4,\"jsonrpc\":\"2.0\",\"result\":{\"percentage\":42.55,\"speed\":0}},"
        "{\"id\":5,\"jsonrpc\":\"2.0\",\"result\":{\"items\":[{\"label\":\"a\"}],\"limits\":{\"end\":1,\"start\":0,\"total\":9}}},"
        "{\"error\":{\"code\":-32100,\"message\":\"Failed to execute method.\"},\"id\":6,\"jsonrpc\":\"2.0\"},"
        "{\"error\":{\"code\":-32100,\"message\":\"x\"},\"id\":7,\"jsonrpc\":\"2.0\"}]";
    code = dk_reply(DK_STATUS, 200, st, out, sizeof out);
    CHECK(code == 200 && has(out, "playing=1\n") && has(out, "paused=1\n") && has(out, "muted=1\n") && has(out, "volume=35\n")
          && has(out, "queue_len=9\n") && (has(out, "percent=42.5\n") || has(out, "percent=42.6\n")), "status fields (playing, paused, muted, volume, queue, percent)");
    CHECK(has(out, "title=The \"Pilot\" ?\n"), "status title unescaped, non-ASCII becomes ?");
    /* nothing playing */
    const char *idle = "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[]},{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"muted\":false,\"volume\":80}},"
        "{\"id\":3,\"jsonrpc\":\"2.0\",\"result\":{\"item\":{\"label\":\"\",\"title\":\"\",\"type\":\"unknown\"}}},"
        "{\"id\":4,\"jsonrpc\":\"2.0\",\"result\":{\"percentage\":0.0,\"speed\":0}},"
        "{\"id\":5,\"jsonrpc\":\"2.0\",\"result\":{\"items\":[],\"limits\":{\"end\":0,\"start\":0,\"total\":0}}}]";
    dk_reply(DK_STATUS, 200, idle, out, sizeof out);
    CHECK(has(out, "playing=0\n") && has(out, "paused=0\n") && has(out, "volume=80\n") && has(out, "queue_len=0\n"), "status when idle");
    /* audio player (id 0) */
    const char *au = "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[{\"playerid\":0,\"type\":\"audio\"}]},{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"muted\":false,\"volume\":50}},"
        "{\"error\":{\"code\":-32100,\"message\":\"x\"},\"id\":3,\"jsonrpc\":\"2.0\"},{\"error\":{\"code\":-32100,\"message\":\"x\"},\"id\":4,\"jsonrpc\":\"2.0\"},"
        "{\"id\":5,\"jsonrpc\":\"2.0\",\"result\":{\"items\":[],\"limits\":{\"total\":0}}},"
        "{\"id\":6,\"jsonrpc\":\"2.0\",\"result\":{\"item\":{\"label\":\"Some Song\",\"title\":\"Some Song\",\"type\":\"song\"}}},"
        "{\"id\":7,\"jsonrpc\":\"2.0\",\"result\":{\"percentage\":10.0,\"speed\":1}}]";
    dk_reply(DK_STATUS, 200, au, out, sizeof out);
    CHECK(has(out, "playing=1\n") && has(out, "paused=0\n") && has(out, "title=Some Song\n"), "status for the audio player");
    /* volume, mute, fullscreen, errors, auth */
    dk_reply(DK_VOL, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":85}", out, sizeof out); CHECK(!strcmp(out, "Volume 85"), "volume reply");
    dk_reply(DK_MUTE, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":true}", out, sizeof out); CHECK(!strcmp(out, "Muted"), "mute reply");
    dk_reply(DK_FULL, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":false}", out, sizeof out); CHECK(!strcmp(out, "Nothing playing"), "fullscreen with nothing playing");
    dk_reply(DK_PLAIN, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":\"OK\"}", out, sizeof out); CHECK(!strcmp(out, "ok"), "plain ok");
    code = dk_reply(DK_PLAIN, 200, "{\"error\":{\"code\":-32602,\"message\":\"Invalid params.\"},\"id\":1,\"jsonrpc\":\"2.0\"}", out, sizeof out);
    CHECK(code == 502 && has(out, "Invalid params."), "JSON-RPC error surfaces");
    CHECK(dk_reply(DK_PLAIN, 401, "", out, sizeof out) == 401 && has(out, "user name or password"), "401 -> password message");
    CHECK(dk_reply(DK_PLAIN, 200, "<html>router login</html>", out, sizeof out) == 502, "non-Kodi reply refused");
    CHECK(dk_request("POST", "/t/q/remove/4", body, sizeof body, &kind) == 0 && has(body, "Playlist.Remove") && has(body, "\"position\":4"), "queue remove");
    CHECK(dk_request("POST", "/t/q/move/3/1", body, sizeof body, &kind) == 0 && kind == DK_BATCH && has(body, "\"position1\":3,\"position2\":2") && has(body, "\"position1\":2,\"position2\":1") && !has(body, "\"position1\":1"), "queue move 3 to 1 = swaps 3-2, 2-1");
    CHECK(dk_request("POST", "/t/q/move/1/4", body, sizeof body, &kind) == 0 && has(body, "\"id\":3,") && !has(body, "\"id\":4,"), "queue move 1 to 4 = three swaps");
    { static char big[16000]; CHECK(dk_request("POST", "/t/q/move/0/127", big, sizeof big, &kind) == 0 && strlen(big) < 15900, "longest move (127 swaps, queue of 128) fits the 16000-byte buffer"); }
    CHECK(dk_request("POST", "/t/q/move/5/5", body, sizeof body, &kind) == 0 && has(body, "JSONRPC.Ping"), "move to the same place is a no-op");
    CHECK(dk_request("POST", "/t/q/move/64/1", body, sizeof body, &kind) == -1 && dk_request("POST", "/t/q/remove/x", body, sizeof body, &kind) == -1, "bad queue arguments refused");
    dk_reply(DK_BATCH, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":\"OK\"},{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":\"OK\"}]", out, sizeof out); CHECK(!strcmp(out, "ok"), "batch ok");
    code = dk_reply(DK_BATCH, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":\"OK\"},{\"error\":{\"code\":-32602,\"message\":\"Invalid params.\"},\"id\":2,\"jsonrpc\":\"2.0\"}]", out, sizeof out); CHECK(code == 502 && has(out, "Invalid params."), "batch error surfaces");
    /* tracks, text, power */
    CHECK(dk_request("POST", "/t/track/a/1", body, sizeof body, &kind) == 0 && has(body, "SetAudioStream") && has(body, "\"stream\":1"), "audio track request");
    CHECK(dk_request("POST", "/t/track/s/2", body, sizeof body, &kind) == 0 && has(body, "SetSubtitle") && has(body, "\"subtitle\":2") && has(body, "\"enable\":true"), "subtitle track request");
    CHECK(dk_request("POST", "/t/track/s/off", body, sizeof body, &kind) == 0 && has(body, "\"subtitle\":\"off\""), "subtitles off request");
    CHECK(dk_request("POST", "/t/text?s=he%22llo%20w%5Corld", body, sizeof body, &kind) == 0 && has(body, "Input.SendText") && has(body, "he\\\"llo w\\\\orld"), "text is decoded and JSON-escaped");
    CHECK(dk_request("POST", "/t/text?s=", body, sizeof body, &kind) == -1, "empty text refused");
    CHECK(dk_request("POST", "/t/power/reboot", body, sizeof body, &kind) == -1, "reboot without confirm is not translated");
    CHECK(dk_request("POST", "/t/power/reboot/confirm", body, sizeof body, &kind) == 0 && has(body, "System.Reboot"), "reboot with confirm");
    CHECK(dk_request("POST", "/t/power/shutdown/confirm", body, sizeof body, &kind) == 0 && has(body, "System.Shutdown"), "shutdown with confirm");
    CHECK(dk_request("POST", "/t/power/restartkodi/confirm", body, sizeof body, &kind) == 0 && has(body, "Application.Quit"), "restart Kodi with confirm");
    CHECK(dk_request("POST", "/t/power/refresh", body, sizeof body, &kind) == 0 && has(body, "VideoLibrary.Scan"), "refresh needs no confirm");
    const char *tr = "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[{\"playerid\":1,\"type\":\"video\"}]},{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{"
        "\"audiostreams\":[{\"channels\":2,\"codec\":\"aac\",\"index\":0,\"language\":\"eng\",\"name\":\"AAC\"},{\"channels\":6,\"codec\":\"ac3\",\"index\":1,\"language\":\"jpn\",\"name\":\"AC3\"}],"
        "\"currentaudiostream\":{\"channels\":6,\"index\":1,\"language\":\"jpn\",\"name\":\"AC3\"},\"currentsubtitle\":{\"index\":1,\"language\":\"spa\",\"name\":\"Spanish\"},"
        "\"subtitleenabled\":true,\"subtitles\":[{\"index\":0,\"language\":\"eng\",\"name\":\"English\"},{\"index\":1,\"language\":\"spa\",\"name\":\"Spanish\"}]}},"
        "{\"error\":{\"code\":-32100,\"message\":\"x\"},\"id\":3,\"jsonrpc\":\"2.0\"}]";
    code = dk_reply(DK_TRACKS, 200, tr, out, sizeof out);
    CHECK(code == 200 && has(out, "a|0|ENG AAC 2ch|0\n") && has(out, "a|1|JPN AC3 6ch|1\n") && has(out, "s|off|Subtitles off|0\n") && has(out, "s|0|ENG English|0\n") && has(out, "s|1|SPA Spanish|1\n"), "tracks list with the current ones marked");
    code = dk_reply(DK_TRACKS, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[]},{\"error\":{\"code\":1,\"message\":\"x\"},\"id\":2,\"jsonrpc\":\"2.0\"},{\"error\":{\"code\":1,\"message\":\"x\"},\"id\":3,\"jsonrpc\":\"2.0\"}]", out, sizeof out);
    CHECK(code == 409 && has(out, "nothing is playing"), "tracks with nothing playing");
    dk_reply(DK_STATUS, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":[{\"playerid\":1}]},{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"muted\":false,\"volume\":50}},{\"id\":3,\"jsonrpc\":\"2.0\",\"result\":{\"item\":{\"label\":\"x\",\"title\":\"x\"}}},{\"id\":4,\"jsonrpc\":\"2.0\",\"result\":{\"percentage\":1.0,\"position\":7,\"speed\":1}},{\"id\":5,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"total\":9}}}]", out, sizeof out);
    CHECK(has(out, "position=7\n") && has(out, "queue_len=9\n"), "status carries the queue position");
    /* library lists, from replies in the shape Kodi sends (limits before the list for movies, after it for episodes) */
    CHECK(dk_request("POST", "/t/lib/movies/2", body, sizeof body, &kind) == 0 && kind == DK_LIB_MOVIES && has(body, "GetMovies") && has(body, "\"start\":80") && has(body, "\"end\":120"), "movies page 2 asks for items 80 to 120");
    code = dk_reply(DK_LIB_MOVIES, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":2,\"start\":0,\"total\":100},\"movies\":[{\"label\":\"Alpha\",\"movieid\":1,\"playcount\":1,\"resume\":{\"position\":0.0,\"total\":0.0},\"year\":2001},"
                   "{\"label\":\"Beta \\u00e9 | x\",\"movieid\":2,\"playcount\":0,\"resume\":{\"position\":300.0,\"total\":6000.0},\"year\":2002},{\"label\":\"Gamma\",\"movieid\":3,\"playcount\":0,\"resume\":{\"position\":0.0,\"total\":0.0},\"year\":0}]}}", out, sizeof out);
    CHECK(code == 200 && has(out, "m|1|Alpha (2001)|1|\n") && has(out, "m|2|Beta ? / x (2002)|2|\n") && has(out, "m|3|Gamma|0|\n") && has(out, "p|1|More movies (60 left)|0|movies\n"), "movies: watched 1, partly watched 2, year, odd characters, a next-page row");
    CHECK(dk_request("POST", "/t/lib/shows", body, sizeof body, &kind) == 0 && kind == DK_LIB_SHOWS && has(body, "GetTVShows"), "shows request");
    code = dk_reply(DK_LIB_SHOWS, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":3,\"start\":0,\"total\":3},\"tvshows\":[{\"episode\":275,\"label\":\"Bob's Burgers\",\"tvshowid\":1,\"watchedepisodes\":9},"
                   "{\"episode\":0,\"label\":\"Empty\",\"tvshowid\":4,\"watchedepisodes\":0},{\"episode\":3,\"label\":\"Done\",\"tvshowid\":6,\"watchedepisodes\":3}]}}", out, sizeof out);
    CHECK(code == 200 && has(out, "s|1|Bob's Burgers - 275 episodes, 9 seen|0|\n") && !has(out, "Empty") && has(out, "s|6|Done - 3 episodes, 3 seen|1|\n") && !has(out, "p|"), "shows: counts, a show with no episodes left out, all seen is 1");
    CHECK(dk_request("POST", "/t/lib/seasons/7", body, sizeof body, &kind) == 0 && kind == DK_LIB_SEASONS && has(body, "\"tvshowid\":7"), "seasons request");
    code = dk_reply(DK_LIB_SEASONS, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":2,\"start\":0,\"total\":2},\"seasons\":[{\"episode\":3,\"label\":\"Specials\",\"season\":0,\"seasonid\":9,\"watchedepisodes\":0},{\"episode\":13,\"label\":\"Season 1\",\"season\":1,\"seasonid\":10,\"watchedepisodes\":13}]}}", out, sizeof out);
    CHECK(code == 200 && has(out, "n|0|Specials - 3 episodes, 0 seen|0|7\n") && has(out, "n|1|Season 1 - 13 episodes, 13 seen|1|7\n"), "seasons carry the show id; Specials; fully seen is 1");
    CHECK(dk_request("POST", "/t/lib/eps/7/1", body, sizeof body, &kind) == 0 && kind == DK_LIB_EPS && has(body, "\"season\":1"), "episodes request");
    code = dk_reply(DK_LIB_EPS, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"episodes\":[{\"episode\":1,\"episodeid\":16,\"label\":\"1x01. Pilot\",\"playcount\":1,\"resume\":{\"position\":0.0,\"total\":0.0},\"season\":1,\"title\":\"Pilot\"},"
                   "{\"episode\":2,\"episodeid\":17,\"label\":\"1x02. Second\",\"playcount\":0,\"resume\":{\"position\":50.0,\"total\":1300.0},\"season\":1,\"title\":\"Second\"}],\"limits\":{\"end\":2,\"start\":0,\"total\":13}}}", out, sizeof out);
    CHECK(code == 200 && has(out, "a|1|Add all 13 episodes to the queue|0|7\n") && has(out, "e|16|E01  Pilot|1|\n") && has(out, "e|17|E02  Second|2|\n"), "episodes: add-all row first (count read from limits after the list), watched and partly watched");
    CHECK(dk_request("POST", "/t/lib/continue", body, sizeof body, &kind) == 0 && kind == DK_LIB_CONT && has(body, "inprogress") && has(body, "\"value\":\"\""), "continue request uses Kodi's inprogress filter with the empty value");
    code = dk_reply(DK_LIB_CONT, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"episodes\":[{\"episodeid\":67,\"episode\":14,\"label\":\"11x14. Mr. Lonely\",\"season\":11,\"showtitle\":\"Bob's Burgers\",\"title\":\"Mr. Lonely\"}],\"limits\":{\"end\":1,\"start\":0,\"total\":1}}},"
                   "{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":1,\"start\":0,\"total\":1},\"movies\":[{\"label\":\"Movie X\",\"movieid\":80,\"year\":2026}]}}]", out, sizeof out);
    CHECK(code == 200 && has(out, "e|67|Bob's Burgers S11E14 Mr. Lonely|2|\n") && has(out, "m|80|Movie X (2026)|2|\n"), "continue: episodes then movies, all marked partly watched");
    CHECK(dk_request("POST", "/t/lib/recent", body, sizeof body, &kind) == 0 && kind == DK_LIB_RECENT && has(body, "RecentlyAddedEpisodes") && has(body, "RecentlyAddedMovies"), "recent request");
    code = dk_reply(DK_LIB_RECENT, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"episodes\":[{\"episodeid\":648,\"episode\":4,\"label\":\"x\",\"playcount\":0,\"season\":3,\"showtitle\":\"The Boys\",\"title\":\"Plan\"}],\"limits\":{\"end\":1,\"start\":0,\"total\":1}}},"
                   "{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":0,\"start\":0,\"total\":0}}}]", out, sizeof out);
    CHECK(code == 200 && has(out, "e|648|The Boys S03E04 Plan|0|\n"), "recent: an empty movie list is fine");
    code = dk_reply(DK_LIB_MOVIES, 200, "{\"id\":1,\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32602,\"message\":\"Invalid params.\"}}", out, sizeof out);
    CHECK(code == 502 && has(out, "Invalid params"), "a Kodi error is passed on");
    /* library actions */
    CHECK(dk_request("POST", "/t/lib/add/m/5", body, sizeof body, &kind) == 0 && has(body, "Playlist.Add") && has(body, "\"movieid\":5"), "add a movie");
    CHECK(dk_request("POST", "/t/lib/next/e/12/3", body, sizeof body, &kind) == 0 && has(body, "Playlist.Insert") && has(body, "\"position\":4") && has(body, "\"episodeid\":12"), "play next goes after the playing position");
    CHECK(dk_request("POST", "/t/lib/next/e/12/-1", body, sizeof body, &kind) == 0 && has(body, "Playlist.Add") && !has(body, "Insert"), "play next with nothing playing adds at the end");
    CHECK(dk_request("POST", "/t/lib/play/m/9/3/10", body, sizeof body, &kind) == 0 && kind == DK_BATCH && has(body, "Playlist.Insert") && has(body, "\"position\":4") && has(body, "Player.GoTo"), "play now while playing: insert after it and step on");
    CHECK(dk_request("POST", "/t/lib/play/m/9/-1/10", body, sizeof body, &kind) == 0 && kind == DK_BATCH && has(body, "Playlist.Add") && has(body, "Player.Open") && has(body, "\"position\":10"), "play now with nothing playing: add and open at the old queue length");
    CHECK(dk_request("POST", "/t/lib/addseason/7/2", body, sizeof body, &kind) == 0 && has(body, "videodb://tvshows/titles/7/2/") && has(body, "\"directory\""), "add a whole season through Kodi's season folder");
    CHECK(dk_request("POST", "/t/lib/add/x/1", body, sizeof body, &kind) == -1 && dk_request("POST", "/t/lib/eps/0/1", body, sizeof body, &kind) == -1 && dk_request("POST", "/t/lib/nonsense", body, sizeof body, &kind) == -1 && dk_request("POST", "/t/lib/movies/9999", body, sizeof body, &kind) == -1, "bad library requests are refused");
    { static char big[16000], o2[12288]; int i, n = 0;                              /* a full page of 40 movies fits the 12 KB list buffer */
      n += snprintf(big + n, sizeof big - n, "{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"limits\":{\"end\":40,\"start\":0,\"total\":200},\"movies\":[");
      for (i = 0; i < 40; i++) n += snprintf(big + n, sizeof big - n, "%s{\"label\":\"2020-11-04 - A fairly long movie title number %d with words\",\"movieid\":%d,\"playcount\":0,\"resume\":{\"position\":0.0,\"total\":0.0},\"year\":2020}", i ? "," : "", i, i + 1);
      snprintf(big + n, sizeof big - n, "]}}");
      code = dk_reply(DK_LIB_MOVIES, 200, big, o2, sizeof o2);
      CHECK(code == 200 && strlen(o2) < 6000 && has(o2, "m|40|") && has(o2, "p|1|More movies (160 left)"), "a full page of 40 movies and the next-page row fit"); }
    CHECK(dk_request("POST", "/t/q/clear/all", body, sizeof body, &kind) == 0 && has(body, "Playlist.Clear"), "clear all request");
    CHECK(dk_request("POST", "/t/q/clear/keep/1/4", body, sizeof body, &kind) == 0 && kind == DK_BATCH && has(body, "\"position\":3") && has(body, "\"position\":2")
          && has(body, "\"position\":0") && !has(body, "\"position\":1}"), "clear keep: removes every position except the playing one, highest first");
    CHECK(dk_request("POST", "/t/q/clear/keep/0/1", body, sizeof body, &kind) == 0 && has(body, "Ping"), "clear keep with only the playing item does nothing");
    CHECK(dk_request("POST", "/t/q/clear/keep/-1/3", body, sizeof body, &kind) == 0 && has(body, "\"position\":2") && has(body, "\"position\":0"), "clear keep with nothing playing removes all");
    { static char big[16000]; CHECK(dk_request("POST", "/t/q/clear/keep/5/128", big, sizeof big, &kind) == 0 && strlen(big) < 15900, "clearing a queue of 128 fits the buffer"); }
    /* queue */
    const char *q = "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"items\":[{\"label\":\"One\",\"type\":\"unknown\"},{\"label\":\"Two | pipe\",\"type\":\"unknown\"},{\"label\":\"Three\",\"type\":\"unknown\"}],\"limits\":{\"end\":3,\"start\":0,\"total\":3}}},"
        "{\"id\":2,\"jsonrpc\":\"2.0\",\"result\":{\"playlistid\":1,\"position\":1}}]";
    dk_reply(DK_QUEUE, 200, q, out, sizeof out);
    CHECK(!strcmp(out, "cur=1\n0|One\n1|Two / pipe\n2|Three\n"), "queue lines");
    dk_reply(DK_QUEUE, 200, "[{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":{\"items\":[],\"limits\":{\"total\":0}}},{\"error\":{\"code\":1,\"message\":\"x\"},\"id\":2,\"jsonrpc\":\"2.0\"}]", out, sizeof out);
    CHECK(!strcmp(out, "cur=-1\n"), "empty queue");
    /* probes and framing */
    CHECK(dk_probe("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Kodi\"\r\nContent-Length: 0\r\n\r\n") == 2, "probe: Kodi 21 login");
    CHECK(dk_probe("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"XBMC\"\r\n\r\n") == 2, "probe: old XBMC realm");
    CHECK(dk_probe("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"Router\"\r\n\r\n") == 0, "probe: another device with a login");
    CHECK(dk_probe("HTTP/1.1 200 OK\r\n\r\n{\"id\":1,\"jsonrpc\":\"2.0\",\"result\":\"pong\"}") == 1, "probe: open Kodi");
    CHECK(dk_probe("HTTP/1.1 200 OK\r\n\r\n<html>hello</html>") == 0, "probe: ordinary web page");
    CHECK(dk_probe("garbage") == 0, "probe: garbage");
    dk_http(out, sizeof out, "10.0.0.5", 8080, "kodi", "secret", "{}");
    CHECK(has(out, "POST /jsonrpc HTTP/1.0\r\n") && has(out, "Authorization: Basic a29kaTpzZWNyZXQ=\r\n") && has(out, "Content-Length: 2\r\n") && has(out, "\r\n\r\n{}"), "http request with Basic auth (kodi:secret)");
    dk_http(out, sizeof out, "10.0.0.5", 8080, "", "", "{}");
    CHECK(!has(out, "Authorization"), "no Authorization header without a user");
}

static int exchange(const char *host, int port, const char *req, char *resp, int respsz) {
    struct addrinfo hints = {0}, *ai; char ps[8]; int fd, n, len = 0;
    hints.ai_socktype = SOCK_STREAM; snprintf(ps, sizeof ps, "%d", port);
    if (getaddrinfo(host, ps, &hints, &ai)) return -1;
    fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) { close(fd); freeaddrinfo(ai); return -1; }
    freeaddrinfo(ai);
    send(fd, req, strlen(req), 0);
    while ((n = recv(fd, resp + len, respsz - 1 - len, 0)) > 0) len += n;
    resp[len] = 0; close(fd); return len;
}

/* net-zero live check on a real Kodi: swap the last two queue items and swap them back, comparing a snapshot of the queue before and after */
static void qswap(const char *host, int port) {
    static char req[9000], resp[65536], body[8192], out[4096], snap1[8192], snap2[8192];
    int kind, n, i, code;
    const char *user = getenv("DK_USER") ? getenv("DK_USER") : "", *pass = getenv("DK_PASS") ? getenv("DK_PASS") : "";
    const char *get = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"Playlist.GetItems\",\"params\":{\"playlistid\":1,\"properties\":[\"file\"]}}";
    const char *steps[] = { NULL, NULL };
    char p1[40], p2[40];
    dk_http(req, sizeof req, host, port, user, pass, get); exchange(host, port, req, resp, sizeof resp);
    { char *b = strstr(resp, "\r\n\r\n"); snprintf(snap1, sizeof snap1, "%s", b ? b + 4 : resp); }
    { const char *t = strstr(snap1, "\"total\":"); n = t ? atoi(t + 8) : 0; }
    printf("queue has %d items\n", n);
    if (n < 3) { printf("skip: need at least 3 queue items to swap the last two\n"); return; }
    snprintf(p1, sizeof p1, "/t/q/move/%d/%d", n - 1, n - 2); snprintf(p2, sizeof p2, "/t/q/move/%d/%d", n - 2, n - 1);
    steps[0] = p1; steps[1] = p2;
    for (i = 0; i < 2; i++) {
        char *b; dk_request("POST", steps[i], body, sizeof body, &kind);
        dk_http(req, sizeof req, host, port, user, pass, body); exchange(host, port, req, resp, sizeof resp);
        sscanf(resp, "HTTP/1.%*d %d", &code); b = strstr(resp, "\r\n\r\n");
        code = dk_reply(kind, code, b ? b + 4 : "", out, sizeof out);
        printf("%s -> %d %s\n", steps[i], code, out);
        if (code != 200) fails++;
        if (i == 0) {
            dk_http(req, sizeof req, host, port, user, pass, get); exchange(host, port, req, resp, sizeof resp);
            b = strstr(resp, "\r\n\r\n"); snprintf(snap2, sizeof snap2, "%s", b ? b + 4 : resp);
            CHECK(strcmp(snap1, snap2) != 0, "the swap really changed the order");
        }
    }
    dk_http(req, sizeof req, host, port, user, pass, get); exchange(host, port, req, resp, sizeof resp);
    { char *b = strstr(resp, "\r\n\r\n"); snprintf(snap2, sizeof snap2, "%s", b ? b + 4 : resp); }
    CHECK(!strcmp(snap1, snap2), "queue is back exactly as it was");
}

static void live(const char *host, int port, const char *user, const char *pass) {
    static char req[4096], resp[65536], out[4096], body[2048];
    const char *paths[] = { "/t/status", "/t/queue", "/t/do/mute", "/t/do/mute", "/t/do/volup", "/t/do/voldown", "/t/do/fullscreen", NULL };
    int i, kind;
    dk_http(req, sizeof req, host, port, "", "", DK_PING);
    if (exchange(host, port, req, resp, sizeof resp) < 0) { printf("FAIL cannot reach %s:%d\n", host, port); fails++; return; }
    printf("probe without login: %d (1 = open Kodi, 2 = Kodi wants a login)\n", dk_probe(resp));
    CHECK(dk_probe(resp) != 0, "device identified as Kodi");
    dk_http(req, sizeof req, host, port, user, pass, DK_PING);
    exchange(host, port, req, resp, sizeof resp);
    CHECK(dk_probe(resp) == 1, "ping with the login answers pong");
    dk_http(req, sizeof req, host, port, user, "wrong-password-x", DK_PING);
    exchange(host, port, req, resp, sizeof resp);
    CHECK(strstr(resp, " 401 ") != NULL, "wrong password is refused with 401");
    for (i = 0; paths[i]; i++) {
        char *b; int code;
        if (dk_request("POST", paths[i], body, sizeof body, &kind)) { printf("FAIL no translation for %s\n", paths[i]); fails++; continue; }
        dk_http(req, sizeof req, host, port, user, pass, body);
        exchange(host, port, req, resp, sizeof resp);
        b = strstr(resp, "\r\n\r\n"); b = b ? b + 4 : resp;
        sscanf(resp, "HTTP/1.%*d %d", &code);
        code = dk_reply(kind, code, b, out, sizeof out);
        for (char *c = out; *c; c++) if (*c == '\n') *c = ' ';
        printf("%s -> %d  %.150s\n", paths[i], code, out);
        if (code != 200) fails++;
    }
}

/* read-only check of the library lists against a real Kodi: nothing is added, played or changed */
static void livelib(const char *host, int port, const char *user, const char *pass) {
    static char body[4096], req[8192], resp[60000], out[12288];
    const char *paths[] = { "/t/lib/continue", "/t/lib/recent", "/t/lib/movies", "/t/lib/shows", "/t/lib/seasons/1", "/t/lib/eps/1/1", NULL };
    int i, kind, code;
    for (i = 0; paths[i]; i++) {
        char *b; int lines = 0; const char *c;
        if (dk_request("POST", paths[i], body, sizeof body, &kind)) { printf("FAIL no translation for %s\n", paths[i]); fails++; continue; }
        dk_http(req, sizeof req, host, port, user, pass, body);
        exchange(host, port, req, resp, sizeof resp);
        b = strstr(resp, "\r\n\r\n"); b = b ? b + 4 : resp;
        sscanf(resp, "HTTP/1.%*d %d", &code);
        code = dk_reply(kind, code, b, out, sizeof out);
        for (c = out; *c; c++) if (*c == '\n') lines++;
        printf("%s -> %d, %d lines, %d bytes of JSON\n", paths[i], code, lines, (int)strlen(b));
        { char first[160]; int n = 0; for (c = out; *c && *c != '\n' && n < 150; c++) first[n++] = *c; first[n] = 0; printf("    first: %s\n", first); }
        if (code != 200 || lines == 0) fails++;
    }
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "self")) self();
    else if (argc >= 4 && !strcmp(argv[1], "qswap")) qswap(argv[2], atoi(argv[3]));
    else if (argc >= 4 && !strcmp(argv[1], "livelib")) livelib(argv[2], atoi(argv[3]), getenv("DK_USER") ? getenv("DK_USER") : "", getenv("DK_PASS") ? getenv("DK_PASS") : "");
    else if (argc >= 4 && !strcmp(argv[1], "live")) live(argv[2], atoi(argv[3]), getenv("DK_USER") ? getenv("DK_USER") : "", getenv("DK_PASS") ? getenv("DK_PASS") : "");
    else { printf("usage: dk_test self | live HOST PORT   (login from the DK_USER and DK_PASS environment variables)\n"); return 2; }
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
