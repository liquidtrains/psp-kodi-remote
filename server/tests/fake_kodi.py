#!/usr/bin/env python3
"""A small fake Kodi JSON-RPC server for testing tvplay-api without touching a real queue.
Keeps one video playlist (id 1) and a current position, and models Kodi's habit of keeping the playing item playing when
items are inserted, removed or swapped around it. Calls it cannot answer are recorded in CALLS."""
import json, os, sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PLAYLIST = []
CUR = [0]
CALLS = []
AUDIO = [{"index": 0, "language": "eng", "name": "AAC", "codec": "aac", "channels": 2}, {"index": 1, "language": "jpn", "name": "AC3", "codec": "ac3", "channels": 6}]
SUBS = [{"index": 0, "language": "eng", "name": "English"}, {"index": 1, "language": "spa", "name": "Spanish"}]
STATE = {"audio": 0, "sub": None, "volume": 80, "muted": False, "time": 600, "total": 2700, "shuffled": False, "repeat": "off"}


def hms(s):
    return {"hours": s // 3600, "minutes": s % 3600 // 60, "seconds": s % 60, "milliseconds": 0}


LIB_MOVIES = [{"movieid": 1, "label": "Alpha Movie", "year": 2001, "playcount": 1, "file": "/lib/alpha.mkv"},
              {"movieid": 2, "label": "Beta Movie", "year": 2002, "playcount": 0, "file": "/lib/beta.mkv", "resume": {"position": 300, "total": 6000}}]
LIB_EPS = [{"episodeid": 11, "tvshowid": 5, "season": 1, "episode": 1, "title": "Pilot", "showtitle": "Show Five", "playcount": 1, "file": "/lib/s5/s01e01.mkv"},
           {"episodeid": 12, "tvshowid": 5, "season": 1, "episode": 2, "title": "Second", "showtitle": "Show Five", "playcount": 0, "file": "/lib/s5/s01e02.mkv", "resume": {"position": 50, "total": 1300}},
           {"episodeid": 13, "tvshowid": 5, "season": 2, "episode": 1, "title": "Return", "showtitle": "Show Five", "playcount": 0, "file": "/lib/s5/s02e01.mkv"}]


def lib_file(it):
    if "file" in it:
        return it["file"]
    for m in LIB_MOVIES:
        if it.get("movieid") == m["movieid"]:
            return m["file"]
    for e in LIB_EPS:
        if it.get("episodeid") == e["episodeid"]:
            return e["file"]
    return ""


def item(f):
    return {"file": f, "label": os.path.basename(f), "type": "unknown", "title": ""}


def call(method, p):
    p = p or {}
    CALLS.append((method, p))
    if method == "Player.GetActivePlayers":
        return [{"playerid": 1, "type": "video"}]
    if method == "Player.GetProperties":
        r = {"playlistid": 1 if PLAYLIST else -1, "position": CUR[0], "speed": 1, "percentage": 12.5}
        want = p.get("properties", [])
        if "time" in want:
            r.update(time=hms(STATE["time"]), totaltime=hms(STATE["total"]))
        if "shuffled" in want:
            r.update(shuffled=STATE["shuffled"], repeat=STATE["repeat"])
        if "audiostreams" in want:
            r.update(audiostreams=AUDIO, currentaudiostream=AUDIO[STATE["audio"]], subtitles=SUBS,
                     currentsubtitle=SUBS[STATE["sub"]] if STATE["sub"] is not None else {}, subtitleenabled=STATE["sub"] is not None)
        return r
    if method == "Player.GetItem":
        f = PLAYLIST[CUR[0]] if PLAYLIST else ""
        return {"item": {"label": os.path.basename(f), "title": "", "type": "video", "file": f}}
    if method == "Playlist.GetItems":
        n = len(PLAYLIST)
        return {"items": [item(f) for f in PLAYLIST], "limits": {"start": 0, "end": n, "total": n}}
    if method == "VideoLibrary.GetMovies":
        ms = [dict(m, label=m["label"]) for m in LIB_MOVIES if not p.get("filter") or m.get("resume")]
        lim = p.get("limits") or {"start": 0, "end": 100}
        return {"movies": ms[lim["start"]:lim["end"]], "limits": {"start": lim["start"], "end": lim["end"], "total": len(ms) if not os.environ.get("FAKE_MANY") else 250}}
    if method == "VideoLibrary.GetTVShows":
        return {"tvshows": [{"tvshowid": 5, "label": "Show Five", "episode": 3, "watchedepisodes": 1}, {"tvshowid": 6, "label": "Empty Show", "episode": 0, "watchedepisodes": 0}]}
    if method == "VideoLibrary.GetSeasons":
        return {"seasons": [{"season": 1, "episode": 2, "watchedepisodes": 1}, {"season": 2, "episode": 1, "watchedepisodes": 0}]}
    if method == "VideoLibrary.GetEpisodes":
        es = [e for e in LIB_EPS if (p.get("tvshowid") is None or e["tvshowid"] == p["tvshowid"]) and (p.get("season") is None or e["season"] == p["season"])
              and (not p.get("filter") or e.get("resume"))]
        return {"episodes": es}
    if method == "VideoLibrary.GetRecentlyAddedEpisodes":
        return {"episodes": LIB_EPS[::-1]}
    if method == "VideoLibrary.GetRecentlyAddedMovies":
        return {"movies": LIB_MOVIES[::-1]}
    if method == "VideoLibrary.GetMovieDetails":
        m = next((x for x in LIB_MOVIES if x["movieid"] == p["movieid"]), None)
        return {"moviedetails": {"title": m["label"], "movieid": m["movieid"]}} if m else None
    if method == "VideoLibrary.GetEpisodeDetails":
        e = next((x for x in LIB_EPS if x["episodeid"] == p["episodeid"]), None)
        return {"episodedetails": {"title": e["title"], "episodeid": e["episodeid"]}} if e else None
    if method == "Playlist.Add":
        PLAYLIST.append(lib_file(p["item"]))
        return "OK"
    if method == "Player.GoTo":
        CUR[0] = min(CUR[0] + 1, len(PLAYLIST) - 1)
        return "OK"
    if method == "Player.Open":
        it = p.get("item", {})
        if "position" in it:
            CUR[0] = it["position"]
        return "OK"
    if method == "Playlist.Insert":
        pos = p["position"]
        PLAYLIST.insert(pos, lib_file(p["item"]))
        if pos <= CUR[0]:
            CUR[0] += 1
        return "OK"
    if method == "Playlist.Remove":
        pos = p["position"]
        del PLAYLIST[pos]
        if pos < CUR[0]:
            CUR[0] -= 1
        return "OK"
    if method == "Playlist.Clear":
        PLAYLIST.clear()
        CUR[0] = 0
        return "OK"
    if method == "Playlist.Swap":
        a, b = p["position1"], p["position2"]
        PLAYLIST[a], PLAYLIST[b] = PLAYLIST[b], PLAYLIST[a]
        if CUR[0] == a:
            CUR[0] = b
        elif CUR[0] == b:
            CUR[0] = a
        return "OK"
    if method == "Player.Seek":
        t = p["value"]["time"]
        STATE["time"] = t["hours"] * 3600 + t["minutes"] * 60 + t["seconds"]
        return {"percentage": 0, "time": t}
    if method == "Player.SetShuffle":
        STATE["shuffled"] = not STATE["shuffled"]
        return "OK"
    if method == "Player.SetRepeat":
        STATE["repeat"] = {"off": "all", "all": "one", "one": "off"}[STATE["repeat"]]
        return "OK"
    if method == "Application.GetProperties":
        return {"volume": STATE["volume"], "muted": STATE["muted"]}
    if method == "Player.SetAudioStream":
        STATE["audio"] = p["stream"]
        return "OK"
    if method == "Player.SetSubtitle":
        STATE["sub"] = None if p["subtitle"] == "off" else p["subtitle"]
        return "OK"
    return "OK"                                              # Input.SendText, Application.Quit, System.Reboot, ... are only recorded


class H(BaseHTTPRequestHandler):
    def log_message(self, format, *args):  # noqa: A002 - silence the request log
        pass

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers.get("Content-Length", 0))) or b"{}")
        if self.path == "/__state":                          # test control: read or replace the playlist
            if body.get("state"):
                STATE.update(body["state"])
            if body.get("set") is not None:
                PLAYLIST[:] = body["set"]
                CUR[0] = body.get("cur", 0)
                CALLS.clear()
            out = {"playlist": PLAYLIST, "cur": CUR[0], "calls": [c[0] for c in CALLS], "last": CALLS[-1][1] if CALLS else None, "state": STATE}
        else:
            out = {"id": body.get("id"), "jsonrpc": "2.0", "result": call(body.get("method"), body.get("params"))}
        b = json.dumps(out).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)


if __name__ == "__main__":
    ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1])), H).serve_forever()
