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
STATE = {"audio": 0, "sub": None, "volume": 80, "muted": False}


def item(f):
    return {"file": f, "label": os.path.basename(f), "type": "unknown", "title": ""}


def call(method, p):
    p = p or {}
    CALLS.append((method, p))
    if method == "Player.GetActivePlayers":
        return [{"playerid": 1, "type": "video"}]
    if method == "Player.GetProperties":
        r = {"playlistid": 1, "position": CUR[0], "speed": 1, "percentage": 12.5}
        want = p.get("properties", [])
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
    if method == "Playlist.Insert":
        pos = p["position"]
        PLAYLIST.insert(pos, p["item"]["file"])
        if pos <= CUR[0]:
            CUR[0] += 1
        return "OK"
    if method == "Playlist.Remove":
        pos = p["position"]
        del PLAYLIST[pos]
        if pos < CUR[0]:
            CUR[0] -= 1
        return "OK"
    if method == "Playlist.Swap":
        a, b = p["position1"], p["position2"]
        PLAYLIST[a], PLAYLIST[b] = PLAYLIST[b], PLAYLIST[a]
        if CUR[0] == a:
            CUR[0] = b
        elif CUR[0] == b:
            CUR[0] = a
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
