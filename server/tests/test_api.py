#!/usr/bin/env python3
"""Runs tvplay-api against tests/fake_kodi.py (never a real Kodi): queue edits, undo, restore, tracks, text, power, health, live label.
Usage: python3 -I tests/test_api.py   (exits non-zero on any failure)"""
import json, os, subprocess, sys, tempfile, time, urllib.error, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
API = next(p for p in (os.path.join(os.path.dirname(HERE), n) for n in ("tvplay-api", "tvplay-api.py")) if os.path.exists(p))
FAKE, APORT, TOKEN = 18080, 18791, "test-token"
tmp = tempfile.mkdtemp(prefix="tvplay-test-")
open(f"{tmp}/curlrc", "w").write("")
open(f"{tmp}/token", "w").write(TOKEN + "\n")
os.makedirs(f"{tmp}/state", exist_ok=True)
env = dict(os.environ, HOME=tmp, KODI_URL=f"http://127.0.0.1:{FAKE}/jsonrpc", KODI_CURLRC=f"{tmp}/curlrc", TVPLAY_API_TOKEN_FILE=f"{tmp}/token",
           TVPLAY_STATE_DIR=f"{tmp}/state", TVPLAY_API_BIND="127.0.0.1", TVPLAY_API_PORT=str(APORT))
procs = [subprocess.Popen([sys.executable, "-I", f"{HERE}/fake_kodi.py", str(FAKE)]), subprocess.Popen([sys.executable, "-I", API], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)]
fails = 0


def check(cond, what):
    global fails
    print(("ok   " if cond else "FAIL ") + what)
    fails += 0 if cond else 1


def api(method, path, expect=None):
    req = urllib.request.Request(f"http://127.0.0.1:{APORT}{path}", method=method, headers={"X-Token": TOKEN}, data=b"" if method == "POST" else None)
    try:
        r = urllib.request.urlopen(req, timeout=10)
        return r.status, r.read().decode()
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()


def fake(set_=None, cur=0):
    body = json.dumps({"set": set_, "cur": cur} if set_ is not None else {}).encode()
    return json.load(urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{FAKE}/__state", data=body, method="POST"), timeout=5))


def names():
    return [os.path.basename(f) if not f.startswith("http") else "S" for f in fake()["playlist"]]


STREAM = "https://usw21.playlist.ttvnw.net/v1/playlist/AAA.m3u8"
EP = [f"/wd/Firefly/E{n:02d}.mkv" for n in range(1, 8)]
try:
    for _ in range(60):
        try:
            urllib.request.urlopen(f"http://127.0.0.1:{APORT}/healthz", timeout=1)
            break
        except Exception:
            time.sleep(0.25)
    st, body = api("GET", "/healthz")
    check(st == 200 and body.startswith("ok "), "healthz answers without a login")
    check(api("GET", "/t/status")[0] in (200, 502), "status route up")
    # ---- queue edits
    fake([STREAM] + EP, cur=0)
    st, _ = api("POST", "/t/q/move/3/1")
    check(st == 200 and names()[:4] == ["S", "E03.mkv", "E01.mkv", "E02.mkv"], "move 3 to 1 reorders (E03 now second)")
    check(api("POST", "/t/q/undo")[0] == 200 and names() == ["S"] + [os.path.basename(f) for f in EP], "undo puts it back")
    st, body = api("POST", "/t/q/remove/2")
    check(st == 200 and "E02.mkv" not in names(), "remove 2")
    check(api("POST", "/t/q/undo")[0] == 200 and names()[2] == "E02.mkv", "undo of a remove re-inserts at the same place")
    check(api("POST", "/t/q/remove/0")[0] == 409, "the playing item cannot be removed")
    check(api("POST", "/t/q/remove/99")[0] == 400, "out-of-range position refused")
    check(api("POST", "/t/q/undo")[0] in (200, 404), "undo with an empty stack does not crash")
    # ---- the playing item stays playing when items move around it
    fake([EP[0], EP[1], EP[2]], cur=1)
    api("POST", "/t/q/move/2/0")
    check(fake()["cur"] == 2 and fake()["playlist"][2] == EP[1], "moving an item past the playing one keeps it playing")
    # ---- restore from a snapshot after damage (the accident of 2026-11-24)
    fake([STREAM] + EP, cur=0)
    api("POST", "/t/q/move/1/1")                                  # no-op, but the snapshot exists from the periodic loop or earlier edits
    snaps = sorted(os.listdir(f"{tmp}/state/queue-snapshots")) if os.path.isdir(f"{tmp}/state/queue-snapshots") else []
    check(len(snaps) >= 1, "snapshots are being written")
    # build a known snapshot, then damage the queue three ways: two removals and one reorder
    fake([STREAM] + EP, cur=0)
    api("POST", "/t/q/move/2/2")
    for f in os.listdir(f"{tmp}/state/queue-snapshots"):
        os.remove(f"{tmp}/state/queue-snapshots/{f}")
    json.dump({"time": "t", "reason": "test", "files": EP}, open(f"{tmp}/state/queue-snapshots/20260101-000000.json", "w"))
    fake([STREAM, EP[2], EP[0], EP[4], EP[5], EP[6]], cur=0)      # E02, E04 lost; E03 before E01
    st, body = api("POST", "/t/q/restore")
    check(st == 200 and [f for f in fake()["playlist"] if not f.startswith("http")] == EP, "restore rebuilds the missing and mis-ordered items")
    check(fake()["playlist"][0] == STREAM and fake()["cur"] == 0, "restore leaves the stream entry and the playing item alone")
    check("Playlist.Clear" not in fake()["calls"] and "Playlist.Remove" not in fake()["calls"], "restore never clears or removes")
    # ---- tracks
    fake([STREAM] + EP, cur=0)
    st, body = api("GET", "/t/tracks")
    check(st == 200 and "a|0|EN" in body.replace("ENG", "EN") or "a|0|ENG AAC 2ch|1" in body, "tracks list audio")
    check("s|off|Subtitles off|1" in body and "s|0|ENG English|0" in body, "tracks list subtitles with 'off' current")
    api("POST", "/t/track/a/1")
    check(fake()["state"]["audio"] == 1, "audio track set")
    api("POST", "/t/track/s/1")
    check(fake()["state"]["sub"] == 1, "subtitle track set")
    api("POST", "/t/track/s/off")
    check(fake()["state"]["sub"] is None, "subtitles off")
    check(api("POST", "/t/track/a/x")[0] == 400, "bad track refused")
    # ---- text
    st, _ = api("POST", "/t/text?s=hello%20world")
    check(st == 200 and fake()["calls"][-1] == "Input.SendText" and fake()["last"]["text"] == "hello world", "text is typed (url-decoded)")
    check(api("POST", "/t/text?s=")[0] == 400, "empty text refused")
    # ---- power
    check(api("POST", "/t/power/reboot")[0] == 400, "reboot without /confirm refused")
    check("System.Reboot" not in fake()["calls"], "nothing was sent to Kodi")
    check(api("POST", "/t/power/refresh")[0] == 200 and fake()["calls"][-1] == "VideoLibrary.Scan", "refresh scans the library")
    st, _ = api("POST", "/t/power/restartkodi/confirm")
    check(st == 200 and fake()["calls"][-1] == "Application.Quit", "restartkodi with /confirm")
    check(api("POST", "/t/power/reboot/confirm")[0] == 429, "a second power action within a minute is refused")
    check(api("POST", "/t/power/bogus/confirm")[0] in (404, 400), "unknown power action refused")
    check(os.path.exists(f"{tmp}/state/power.log"), "power actions are logged")
    # ---- live label from the file tvplay writes
    open(f"{tmp}/state/last-live", "w").write("kick/somestreamer\n")
    fake([STREAM] + EP, cur=0)
    api("POST", "/t/key/info")                                    # any POST drops the status cache
    st, body = api("GET", "/t/status")
    check("title=LIVE kick/somestreamer" in body and "chat=https://kick.com/popout/somestreamer/chat" in body, "live stream shows platform, channel and chat link")
    open(f"{tmp}/state/last-live", "w").write("twitch/other_one\n")
    api("POST", "/t/key/info")
    body = api("GET", "/t/status")[1]
    check("live=twitch/other_one" in body and "twitch.tv/popout/other_one/chat" in body, "twitch label and chat link")
    # ---- status time fields, seek, shuffle, repeat
    fake([STREAM] + EP, cur=0)
    api("POST", "/t/key/info")
    body = api("GET", "/t/status")[1]
    check("time=600" in body and "total=2700" in body and "shuffled=0" in body and "repeat=off" in body, "status carries time, total, shuffle and repeat")
    check(api("POST", "/t/seek/30")[0] == 200 and fake()["state"]["time"] == 630, "seek forward 30 s")
    check(api("POST", "/t/seek/-60")[0] == 200 and fake()["state"]["time"] == 570, "seek back 60 s")
    api("POST", "/t/seek/-9999")
    check(fake()["state"]["time"] == 0, "seeking back past the start stops at 0")
    api("POST", "/t/seek/99999")
    check(fake()["state"]["time"] == 2697, "seeking past the end stops 3 s before it")
    check(api("POST", "/t/seek/pct/50")[0] == 200 and fake()["state"]["time"] == 1350, "jump to 50 percent")
    check(api("POST", "/t/seek/pct/100")[0] == 400 and api("POST", "/t/seek/abc")[0] == 400, "bad seek values refused")
    fake(None); urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{FAKE}/__state", data=json.dumps({"state": {"total": 0}}).encode(), method="POST"), timeout=5)
    check(api("POST", "/t/seek/30")[0] == 409, "seek refused on a live stream (no total time)")
    urllib.request.urlopen(urllib.request.Request(f"http://127.0.0.1:{FAKE}/__state", data=json.dumps({"state": {"total": 2700}}).encode(), method="POST"), timeout=5)
    st, body = api("POST", "/t/shuffle")
    check(st == 200 and "shuffle on" in body and fake()["state"]["shuffled"] is True, "shuffle toggles on")
    check("shuffle off" in api("POST", "/t/shuffle")[1], "shuffle toggles off")
    check("repeat all" in api("POST", "/t/repeat")[1] and "repeat one" in api("POST", "/t/repeat")[1] and "repeat off" in api("POST", "/t/repeat")[1], "repeat cycles all, one, off")
    # ---- shows list: built-in, then from a file
    body = api("GET", "/t/showlist")[1]
    check(body.count("\n") == 5 and "r|bobs|Bob's Burgers - random" in body and "o|firefly|Firefly - in order" in body, "built-in show list")
    os.makedirs(f"{tmp}/.config/tvplay-api", exist_ok=True)
    open(f"{tmp}/.config/tvplay-api/shows.txt", "w").write("# my shows\nrick|Rick and Morty|random\nrick|Rick and Morty|order\nbad key|x|random\nfirefly|Firefly|order\n")
    body = api("GET", "/t/showlist")[1]
    check(body == "r|rick|Rick and Morty - random\no|rick|Rick and Morty - in order\no|firefly|Firefly - in order\n", "show list read from shows.txt (bad line skipped)")
    check(api("POST", "/t/random/bobs")[0] in (404, 400, 405), "a show left out of the file is no longer offered")
    os.remove(f"{tmp}/.config/tvplay-api/shows.txt")
    check(api("GET", "/t/showlist")[1].count("\n") == 5, "built-in list again without the file")
    # ---- channels line has title and category fields
    open(f"{tmp}/.config/tvplay-api/channels.txt", "w").write("twitch:foo_bar\n")
    check(api("GET", "/t/channels")[1] == "twitch|foo_bar|?||\n", "channel line is platform|name|live|title|category")
    # ---- clear the queue
    fake([STREAM] + EP, cur=0)
    st, body = api("POST", "/t/q/clear/keep")
    check(st == 200 and fake()["playlist"] == [STREAM] and fake()["cur"] == 0, "clear keep leaves only the playing item")
    check("Playlist.Clear" not in fake()["calls"], "clear keep removes one by one, never clears")
    check(api("POST", "/t/q/restore/clear")[0] == 200 and [f for f in fake()["playlist"] if not f.startswith("http")] == EP, "the queue comes back from before the clear, even after the snapshot loop")
    fake(EP, cur=2)
    api("POST", "/t/q/clear/keep")
    check(fake()["playlist"] == [EP[2]] and fake()["cur"] == 0, "clear keep with the playing item in the middle keeps exactly it and its position stays valid")
    api("POST", "/t/q/restore/clear")
    check(fake()["playlist"].count(EP[2]) == 1 and sorted(fake()["playlist"]) == sorted(EP), "restore after a middle keep brings all back once")
    fake(EP, cur=1)
    st, body = api("POST", "/t/q/clear/all")
    check(st == 200 and fake()["playlist"] == [] and "Playlist.Clear" in fake()["calls"], "clear all empties it")
    check(api("POST", "/t/q/clear/all")[0] == 409, "clearing an empty queue says so")
    check(api("POST", "/t/q/restore/clear")[0] == 200 and fake()["playlist"] == EP, "restore after clear all brings every item back in order")
    check(api("POST", "/t/q/clear/bogus")[0] in (400, 404), "unknown clear mode refused")
    # ---- library
    st, body = api("GET", "/t/lib/movies")
    check(st == 200 and body == "m|1|Alpha Movie (2001)|1|\nm|2|Beta Movie (2002)|2|\n", "movies list: watched is 1, partly watched is 2")
    body = api("GET", "/t/lib/shows")[1]
    check(body == "s|5|Show Five - 3 episodes, 1 seen|0|\n", "shows list skips a show with no episodes")
    body = api("GET", "/t/lib/seasons/5")[1]
    check(body == "n|1|Season 1 - 2 episodes, 1 seen|0|5\nn|2|Season 2 - 1 episodes, 0 seen|0|5\n", "seasons list carries the show id")
    body = api("GET", "/t/lib/eps/5/1")[1]
    check(body.splitlines()[0] == "a|1|Add all 2 episodes to the queue|0|5" and "e|11|E01  Pilot|1|" in body and "e|12|E02  Second|2|" in body, "episodes list with the add-all row first")
    body = api("GET", "/t/lib/continue")[1]
    check("e|12|Show Five S01E02 Second|2|" in body and "m|2|Beta Movie|2|" in body and "Alpha" not in body and "Pilot" not in body, "continue watching lists only partly watched items")
    body = api("GET", "/t/lib/recent")[1]
    ls = body.splitlines()
    check(sum(l.startswith("e|") for l in ls) == 3 and sum(l.startswith("m|") for l in ls) == 2, "recently added lists episodes and movies")
    check(api("GET", "/t/lib/nonsense")[0] == 404 and api("GET", "/t/lib/seasons/x")[0] == 404, "unknown library list refused")
    fake([STREAM] + EP[:2], cur=0)
    st, body = api("POST", "/t/lib/add/m/1")
    check(st == 200 and fake()["playlist"][-1] == "/lib/alpha.mkv", "add a movie to the end of the queue")
    check(api("POST", "/t/lib/next/e/13")[0] == 200 and fake()["playlist"][1] == "/lib/s5/s02e01.mkv", "play next goes right after the playing item")
    check(api("POST", "/t/q/undo")[0] == 200 and "/lib/s5/s02e01.mkv" not in fake()["playlist"], "undo takes out the item just put next")
    check(api("POST", "/t/q/undo")[0] == 200 and "/lib/alpha.mkv" not in fake()["playlist"], "undo takes out the item just added")
    check(api("POST", "/t/lib/play/e/12")[0] == 200 and fake()["playlist"][1] == "/lib/s5/s01e02.mkv" and fake()["cur"] == 1 and "Player.GoTo" in fake()["calls"], "play now puts it after the playing item and steps to it")
    check(api("POST", "/t/lib/add/m/99")[0] == 404 and api("POST", "/t/lib/add/x/1")[0] == 404, "an item that is not in the library is refused")
    fake([STREAM], cur=0)
    api("POST", "/t/lib/addseason/5/1")
    check(fake()["playlist"] == [STREAM, "/lib/s5/s01e01.mkv", "/lib/s5/s01e02.mkv"], "add a whole season, in order, at the end")
    check(api("POST", "/t/q/undo")[0] == 200 and fake()["playlist"] == [STREAM], "undo takes the whole season back out")
    check(api("POST", "/t/lib/addseason/5/9")[0] == 404, "a season with no episodes is refused")
    fake([], cur=0)
    api("POST", "/t/lib/play/m/1")
    check(fake()["playlist"] == ["/lib/alpha.mkv"] and "Player.Open" in fake()["calls"], "play now with nothing in the queue adds it and opens it")
    # ---- auth
    req = urllib.request.Request(f"http://127.0.0.1:{APORT}/t/status", headers={"X-Token": "wrong"})
    try:
        urllib.request.urlopen(req, timeout=5); code = 200
    except urllib.error.HTTPError as e:
        code = e.code
    check(code == 401, "wrong token refused")
finally:
    for p in procs:
        p.terminate()
print("all passed" if not fails else f"{fails} FAILED")
sys.exit(1 if fails else 0)
