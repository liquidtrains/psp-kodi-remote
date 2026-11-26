#!/usr/bin/env python3
"""tvplay-api: a small LAN HTTP service in front of tvplay and the Kodi remote keys, for a PSP/handheld remote.

Why it exists: the remote should hold a limited token, not Kodi's web password. The token can only run the verbs below;
it cannot queue arbitrary URLs, read files or reach Kodi's full JSON-RPC. Kodi's web password stays in a curl config file (KODI_CURLRC, default ~/.config/tvplay-api/kodi-curlrc, mode 600, one line:
`user = "kodi:PASSWORD"`).

Auth: header `X-Token: <token>` (token file ~/.config/tvplay-api/token, mode 0600, created on first start; never logged).
Bind: TVPLAY_API_BIND (default 127.0.0.1; set it to your LAN address) port TVPLAY_API_PORT (default 8791). Plain HTTP: LAN only, never port-forward it.

Prefix any path with /t/ for a plain-text reply (the PSP client uses these): lists are one item per line, fields split by '|'.

  GET  /status            now playing, paused, percent, position, queue length, volume   (text: key=value lines)
  GET  /shows             shows usable with random/order
  GET  /channels          Twitch/Kick channels from ~/.config/tvplay-api/channels.txt with live flag   (text: platform|name|live|title|category)
  GET  /showlist          the Shows menu: mode|key|label lines (mode r random, o in order); from ~/.config/tvplay-api/shows.txt, built-in list if absent
  POST /seek/<seconds>    jump forward (or back with a minus) from the current time; /seek/pct/<0-99> jumps to a percentage; refused for live streams
  POST /q/clear/keep      empty the queue except the playing item;  /q/clear/all empties it completely (playback stops). Both save the queue first;
                          POST /q/restore/clear brings back the queue as it was before the last clear (a snapshot the one-minute loop never overwrites)
  GET  /lib/movies[/<page>] | shows | seasons/<show> | eps/<show>/<season> | continue | recent   the Kodi video library as kind|id|label|flag|extra lines
                          (kind m movie, s show, n season, e episode, a add-the-season row, p next page; flag 1 watched, 2 partly watched)
  POST /lib/play|next|add/<m|e>/<id>   play now, play next, add to the end of the queue; /lib/addseason/<show>/<season> adds a whole season in order
  POST /shuffle | /repeat toggle shuffle (a queue snapshot is taken first) | cycle repeat off, all, one
  GET  /favs              Kodi IPTV Favourites group                                       (text: id|name)
  GET  /queue             Kodi playlist 1                                                  (text: pos|title, first line cur=N)
  POST /random/<show>     tvplay random <show>        (bobs house koth rick)
  POST /order/<show>      tvplay order <show>         (bobs house koth rick firefly)
  POST /live/<platform>/<name>   tvplay twitch|kick <name>, only for names in channels.txt
  POST /fav/<channelid>   play that IPTV favourite, only ids in the Favourites group
  POST /goto/<pos>        jump to a queue position
  POST /stop | /sleep/<1-300|off>
  POST /key/<up|down|left|right|select|back|home|info|context>
  POST /do/<playpause|next|prev|volup|voldown|mute|fullscreen>
  POST /q/remove/<pos>   /q/move/<from>/<to>   edit Kodi's video queue (the playing item cannot be removed)
  POST /q/undo   /q/restore   undo the last edit / bring the queue back from the newest snapshot (inserts and swaps only)
  GET /tracks   POST /track/a/<n>  /track/s/<n|off>   audio and subtitle tracks of the playing item
  POST /text?s=...   type into Kodi's active text box   POST /power/<refresh|restartkodi|reboot|shutdown>[/confirm]   GET /healthz (no login)
Env for tests: TVPLAY_API_DRYRUN=1 makes actions report what they WOULD run without touching Kodi.
"""
import hmac, json, os, re, secrets, shutil, subprocess, sys, threading, time
from concurrent.futures import ThreadPoolExecutor
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import urllib.error, urllib.parse, urllib.request

HOME = os.path.expanduser("~")
CURLRC = os.environ.get("KODI_CURLRC", f"{HOME}/.config/tvplay-api/kodi-curlrc")
TOKEN_FILE = os.environ.get("TVPLAY_API_TOKEN_FILE", f"{HOME}/.config/tvplay-api/token")
CHANNELS_FILE = f"{HOME}/.config/tvplay-api/channels.txt"
TVPLAY = os.environ.get("TVPLAY_BIN", f"{HOME}/bin/tvplay")      # optional helper script used by /random /order /live /stop /sleep
KODI = os.environ.get("KODI_URL", "http://127.0.0.1:8080/jsonrpc")
BIND = os.environ.get("TVPLAY_API_BIND", "127.0.0.1")
PORT = int(os.environ.get("TVPLAY_API_PORT", "8791"))
DRY = os.environ.get("TVPLAY_API_DRYRUN") == "1"
KODI = os.environ.get("KODI_URL", KODI)                  # tests point these at a fake Kodi
CURLRC = os.environ.get("KODI_CURLRC", CURLRC)
STATE = os.environ.get("TVPLAY_STATE_DIR", f"{HOME}/.local/state/tvplay")
SNAP_DIR = f"{STATE}/queue-snapshots"
START = time.time()
VERSION = "2026.11.24"
SHOWS = {"bobs": "bobs", "house": "house", "koth": "koth", "rick": "rick", "firefly": "firefly"}
RANDOM_OK = {"bobs", "house", "koth", "rick"}          # example: one show offered in order only
KEYS = {"up": "Input.Up", "down": "Input.Down", "left": "Input.Left", "right": "Input.Right",
        "select": "Input.Select", "back": "Input.Back", "home": "Input.Home", "info": "Input.Info",
        "context": "Input.ContextMenu"}
ACTIONS = {"volup": "volumeup", "voldown": "volumedown", "mute": "mute", "next": "skipnext", "prev": "skipprevious"}
PLAT = {"twitch": "https://www.twitch.tv/", "kick": "https://kick.com/"}
NAME_RE = re.compile(r"^[A-Za-z0-9_]{2,25}$")
LOCK = threading.Lock()                                 # one tvplay at a time; they all touch the same playlist
LAST_LIVE = ""                                          # "twitch/name" of the last channel started from here, to label a live stream
CAT = {}                                                # (platform, name) -> category (game) of a live channel
TITLE = {}                                              # (platform, name) -> stream title of a live channel, plain ASCII, from streamlink metadata
LIVE = {}                                               # (platform, name) -> True / False / None (unknown), refreshed in the background


def token():
    os.makedirs(os.path.dirname(TOKEN_FILE), mode=0o700, exist_ok=True)
    if not os.path.exists(TOKEN_FILE):
        fd = os.open(TOKEN_FILE, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(secrets.token_urlsafe(24) + "\n")
    return open(TOKEN_FILE).read().strip()


TOKEN = token()


def clean(s, n=60):
    """Plain printable ASCII, no field separators: safe for the PSP's line parser."""
    s = re.sub(r"[^\x20-\x7e]", "?", str(s or "")).replace("|", "/").strip()
    return s[:n]


def kodi(method, params=None):
    b = {"jsonrpc": "2.0", "id": 1, "method": method}
    if params is not None:
        b["params"] = params
    p = subprocess.run(["curl", "-s", "--max-time", "8", "-K", CURLRC, "-H", "Content-Type: application/json",
                        "-d", json.dumps(b), KODI], capture_output=True, text=True)
    try:
        return json.loads(p.stdout).get("result")
    except ValueError:
        return None


def run_tvplay(*args):
    if DRY:
        return 200, {"dry_run": True, "would_run": ["tvplay", *args]}
    with LOCK:
        try:
            p = subprocess.run([TVPLAY, *args], capture_output=True, text=True, timeout=45, env=dict(os.environ))
        except subprocess.TimeoutExpired:
            return 504, {"error": "tvplay timed out"}
    out = (p.stdout + p.stderr).strip().splitlines()
    return (200 if p.returncode == 0 else 502), {"rc": p.returncode, "out": out[-3:]}


# ---- Twitch / Kick list -------------------------------------------------------------------------------------------
def channels():
    out = []
    try:
        for line in open(CHANNELS_FILE):
            line = line.split("#")[0].strip()
            if ":" not in line:
                continue
            p, n = [x.strip().lower() for x in line.split(":", 1)]
            if p in PLAT and NAME_RE.match(n) and (p, n) not in out:
                out.append((p, n))
    except OSError:
        pass
    return out


def check_live(pn):
    p, n = pn
    sl = shutil.which("streamlink", path=os.environ.get("PATH", "") + f":{HOME}/.local/bin")
    if not sl:
        return None
    try:
        r = subprocess.run([sl, "--json", PLAT[p] + n], capture_output=True, text=True, timeout=25)
        d = json.loads(r.stdout or "{}")
        live = bool(d.get("streams"))
        t = re.sub(r"\s+", " ", re.sub(r"[^\x20-\x7e]", "", str((d.get("metadata") or {}).get("title") or ""))).replace("|", "/").strip()
        c = re.sub(r"\s+", " ", re.sub(r"[^\x20-\x7e]", "", str((d.get("metadata") or {}).get("category") or ""))).replace("|", "/").strip()
        if live and t:
            TITLE[pn] = t[:70]
        else:
            TITLE.pop(pn, None)
        if live and c:
            CAT[pn] = c[:40]
        else:
            CAT.pop(pn, None)
        return live
    except (subprocess.TimeoutExpired, ValueError):
        return None


def live_loop():
    while True:
        chs = channels()
        if chs:
            with ThreadPoolExecutor(6) as ex:
                for pn, v in zip(chs, ex.map(check_live, chs)):
                    LIVE[pn] = v
        time.sleep(90)


# ---- read-only views -------------------------------------------------------------------------------------------------
_ST = (0.0, None)


def status():
    """cached for 1.5 s so a PSP, a phone and a watcher polling together cost Kodi one set of calls"""
    global _ST
    if _ST[1] is not None and time.time() - _ST[0] < 1.5:
        return _ST[1]
    d = _status()
    _ST = (time.time(), d)
    return d


def live_tag():
    """"twitch/name" of the live channel: tvplay writes it when it starts one, from any entry point (PSP, phone, shell)"""
    try:
        p = f"{STATE}/last-live"
        if time.time() - os.path.getmtime(p) < 12 * 3600:
            t = open(p).read().strip()
            if re.fullmatch(r"(twitch|kick)/[A-Za-z0-9_]{2,25}", t):
                return t
    except OSError:
        pass
    return LAST_LIVE


def chat_url(tag):
    plat, _, name = tag.partition("/")
    return {"twitch": f"https://www.twitch.tv/popout/{name}/chat", "kick": f"https://kick.com/popout/{name}/chat"}.get(plat, "")


def hms_secs(t):
    """Kodi's {hours, minutes, seconds} time object as whole seconds"""
    t = t or {}
    try:
        return int(t.get("hours", 0)) * 3600 + int(t.get("minutes", 0)) * 60 + int(t.get("seconds", 0))
    except (TypeError, ValueError):
        return 0


def _status():
    players = kodi("Player.GetActivePlayers")
    if players is None:
        return {"error": "kodi unreachable"}
    q = (kodi("Playlist.GetItems", {"playlistid": 1}) or {}).get("items") or []
    app = kodi("Application.GetProperties", {"properties": ["volume", "muted"]}) or {}
    res = {"playing": bool(players), "queue_len": len(q), "volume": app.get("volume"), "muted": bool(app.get("muted"))}
    if players:
        item = (kodi("Player.GetItem", {"playerid": 1, "properties": ["title", "showtitle", "season", "episode", "file"]}) or {}).get("item", {})
        pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["position", "percentage", "speed", "time", "totaltime", "shuffled", "repeat"]}) or {}
        label = item.get("label") or item.get("title") or ""
        show = item.get("showtitle")
        f = item.get("file", "")
        if any(h in f for h in ("ttvnw.net", "live-video.net", ".m3u8", "kick.com")) or label.startswith("videoplayback"):
            tag = live_tag()
            label, show = (f"LIVE {tag}" if tag else "Live stream"), None
            if tag:
                res.update(live=tag, chat=chat_url(tag))
        res.update(title=(show + " - " if show else "") + label, paused=pr.get("speed") == 0,
                   percent=round(pr.get("percentage", 0), 1), position=pr.get("position"),
                   time=hms_secs(pr.get("time")), total=hms_secs(pr.get("totaltime")), shuffled=bool(pr.get("shuffled")), repeat=str(pr.get("repeat") or "off"))
    return res


def status_text(d):
    if "error" in d:
        return "error=" + clean(d["error"]) + "\n"
    lines = [f"playing={int(d['playing'])}", f"queue_len={d['queue_len']}", f"volume={d.get('volume')}", f"muted={int(d['muted'])}"]
    if d["playing"]:
        lines += [f"title={clean(d['title'], 90)}", f"paused={int(d['paused'])}", f"percent={d['percent']}", f"position={d['position']}",
                  f"time={d.get('time', 0)}", f"total={d.get('total', 0)}", f"shuffled={int(d.get('shuffled', False))}", f"repeat={d.get('repeat', 'off')}"]
        if d.get("live"):
            lines += [f"live={d['live']}", f"chat={d['chat']}"]
    return "\n".join(lines) + "\n"


SHOWS_FILE = f"{HOME}/.config/tvplay-api/shows.txt"
SHOWS_BUILTIN = [("random", "bobs", "Bob's Burgers"), ("random", "house", "House"), ("random", "koth", "King of the Hill"),
                 ("random", "rick", "Rick and Morty"), ("order", "firefly", "Firefly")]


def show_rows():
    """[(mode, key, label)] from shows.txt (lines: key|Label|random or order), the built-in list when the file is missing or has no usable line.
    The keys must be names tvplay itself understands: this file chooses what the menu offers, tvplay still decides what exists."""
    rows = []
    try:
        for line in open(SHOWS_FILE):
            f = [x.strip() for x in line.split("#")[0].split("|")]
            if len(f) >= 3 and re.fullmatch(r"[a-z0-9_]{2,11}", f[0]) and f[2] in ("random", "order") and f[1]:
                rows.append((f[2], f[0], clean(f[1], 40)))
    except OSError:
        pass
    return rows[:40] or list(SHOWS_BUILTIN)


def refresh_shows():
    rows = show_rows()
    SHOWS.clear()
    SHOWS.update({k: k for _, k, _ in rows})
    RANDOM_OK.clear()
    RANDOM_OK.update(k for m, k, _ in rows if m == "random")
    return rows


def showlist_text():
    return "".join(f"{'r' if m == 'random' else 'o'}|{k}|{l} - {'random' if m == 'random' else 'in order'}\n" for m, k, l in refresh_shows())


def mmss(s):
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}" if s >= 3600 else f"{s // 60}:{s % 60:02d}"


def seek(kind, val):
    """kind 'delta': val seconds from now (negative goes back); kind 'pct': val 0-99. Absolute seek, so a mistimed press cannot overshoot the end."""
    if not re.fullmatch(r"[+-]?\d{1,5}", str(val)):
        return 400, {"error": "bad seek value"}
    n = int(val)
    if kind == "pct" and not 0 <= n <= 99:
        return 400, {"error": "percent must be 0 to 99"}
    if DRY:
        return 200, {"dry_run": True, "would_call": f"Player.Seek {kind} {n}"}
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["time", "totaltime"]}) or {}
    cur, total = hms_secs(pr.get("time")), hms_secs(pr.get("totaltime"))
    if total <= 0:
        return 409, {"error": "cannot seek: nothing playing or a live stream"}
    target = total * n // 100 if kind == "pct" else cur + n
    target = max(0, min(total - 3, target))
    r = kodi("Player.Seek", {"playerid": 1, "value": {"time": {"hours": target // 3600, "minutes": target % 3600 // 60, "seconds": target % 60, "milliseconds": 0}}})
    if r is None:
        return 502, {"error": "Kodi refused the seek"}
    return 200, {"result": f"{mmss(target)} / {mmss(total)}"}


def playback_toggle(which):
    if DRY:
        return 200, {"dry_run": True, "would_call": f"Player.Set{which}"}
    if which == "shuffle":
        snapshot("before shuffle")                       # Kodi reorders the queue when it shuffles; this is the way back
        r = kodi("Player.SetShuffle", {"playerid": 1, "shuffle": "toggle"})
    else:
        r = kodi("Player.SetRepeat", {"playerid": 1, "repeat": "cycle"})
    if r is None:
        return 409, {"error": "nothing is playing"}
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["shuffled", "repeat"]}) or {}
    return 200, {"result": ("shuffle " + ("on" if pr.get("shuffled") else "off")) if which == "shuffle" else "repeat " + str(pr.get("repeat") or "off")}


def channels_view():
    return [(p, n, LIVE.get((p, n))) for p, n in channels()]


def favs_view():
    r = kodi("PVR.GetChannels", {"channelgroupid": 1, "properties": [], "limits": {"start": 0, "end": 80}}) or {}
    return [(c["channelid"], c.get("label", "")) for c in (r.get("channels") or [])]


STREAM_HINTS = ("ttvnw.net", "live-video.net", ".m3u8", "googlevideo.com", "kick.com")
UNDO = []                       # inverse operations of recent queue edits, newest last
QLOCK = threading.Lock()


def is_stream(f):
    return f.startswith(("http://", "https://")) and any(h in f for h in STREAM_HINTS)


def queue_files():
    items = (kodi("Playlist.GetItems", {"playlistid": 1, "properties": ["file"]}) or {}).get("items") or []
    return [i.get("file", "") for i in items]


def snapshot(reason):
    """save the non-stream queue; skipped when it equals the newest snapshot. Keeps the newest 30."""
    files = [f for f in queue_files() if f and not is_stream(f)]
    if not files:
        return None
    os.makedirs(SNAP_DIR, exist_ok=True)
    names = sorted(os.listdir(SNAP_DIR))
    if names:
        try:
            if json.load(open(f"{SNAP_DIR}/{names[-1]}")).get("files") == files:
                return names[-1]
        except (OSError, ValueError):
            pass
    name = time.strftime("%Y%m%d-%H%M%S") + ".json"
    with open(f"{SNAP_DIR}/{name}", "w") as f:
        json.dump({"time": time.strftime("%F %T"), "reason": reason, "files": files}, f)
    for old in sorted(os.listdir(SNAP_DIR))[:-30]:
        os.remove(f"{SNAP_DIR}/{old}")
    return name


def snap_loop():
    while True:
        try:
            snapshot("periodic")
        except Exception:
            pass
        time.sleep(60)


def swap_chain(a, b):
    step = -1 if b < a else 1
    for i in range(a, b, step):
        kodi("Playlist.Swap", {"playlistid": 1, "position1": i, "position2": i + step})


def restore_queue(target):
    """bring the queue back to `target` (a list of files) with inserts and swaps only: never clears, never removes,
    never touches stream entries. Anything not in the snapshot stays where it is."""
    ops = 0
    for _ in range(len(target) + 25):
        real = queue_files()
        view = [(i, f) for i, f in enumerate(real) if not is_stream(f)]
        flat = [f for _, f in view]
        mism = next((k for k in range(len(target)) if k >= len(flat) or flat[k] != target[k]), None)
        if mism is None:
            break
        want = target[mism]
        if mism < len(view):
            at = view[mism][0]
            later = [i for i, f in view[mism + 1:] if f == want]
            if later:
                swap_chain(later[0], at)
            else:
                kodi("Playlist.Insert", {"playlistid": 1, "position": at, "item": {"file": want}})
        else:
            kodi("Playlist.Insert", {"playlistid": 1, "position": len(real), "item": {"file": want}})
        ops += 1
    return ops


def lib_label(s):
    return clean(s, 58)


def lib_flag(playcount, resume=None):
    if resume and (resume.get("position") or 0) > 0:
        return 2
    return 1 if (playcount or 0) > 0 else 0


def lib_view(what, a="", b=""):
    """GET /lib/...: returns (code, text). Lines: kind|id|label|flag|extra"""
    out = []
    PAGE = 100
    if what == "movies":
        page = int(a) if a.isdigit() else 0
        r = kodi("VideoLibrary.GetMovies", {"properties": ["year", "playcount", "resume"], "sort": {"method": "label", "order": "ascending"},
                                            "limits": {"start": page * PAGE, "end": page * PAGE + PAGE}})
        if r is None:
            return 502, "error: kodi unreachable"
        for m in r.get("movies") or []:
            y = f" ({m['year']})" if m.get("year") else ""
            out.append(f"m|{m['movieid']}|{lib_label(m.get('label', '') + y)}|{lib_flag(m.get('playcount'), m.get('resume'))}|")
        total = (r.get("limits") or {}).get("total", 0)
        if total > page * PAGE + PAGE:
            out.append(f"p|{page + 1}|More movies ({total - page * PAGE - PAGE} left)|0|movies")
    elif what == "shows":
        r = kodi("VideoLibrary.GetTVShows", {"properties": ["episode", "watchedepisodes"], "sort": {"method": "label", "order": "ascending"}})
        if r is None:
            return 502, "error: kodi unreachable"
        for s in r.get("tvshows") or []:
            n, w = s.get("episode") or 0, s.get("watchedepisodes") or 0
            if n:
                out.append(f"s|{s['tvshowid']}|{lib_label(s.get('label', ''))} - {n} episodes, {w} seen|{1 if w >= n else 0}|")
    elif what == "seasons" and a.isdigit():
        r = kodi("VideoLibrary.GetSeasons", {"tvshowid": int(a), "properties": ["season", "episode", "watchedepisodes"]})
        if r is None:
            return 502, "error: kodi unreachable"
        for s in r.get("seasons") or []:
            n, w = s.get("episode") or 0, s.get("watchedepisodes") or 0
            name = "Specials" if s.get("season") == 0 else f"Season {s.get('season')}"
            out.append(f"n|{s.get('season')}|{name} - {n} episodes, {w} seen|{1 if n and w >= n else 0}|{a}")
    elif what == "eps" and a.isdigit() and b.lstrip("-").isdigit():
        r = kodi("VideoLibrary.GetEpisodes", {"tvshowid": int(a), "season": int(b), "properties": ["title", "season", "episode", "playcount", "resume"],
                                              "sort": {"method": "episode", "order": "ascending"}})
        if r is None:
            return 502, "error: kodi unreachable"
        eps = r.get("episodes") or []
        if eps:
            out.append(f"a|{int(b)}|Add all {len(eps)} episodes to the queue|0|{a}")
        for e in eps:
            out.append(f"e|{e['episodeid']}|{lib_label('E%02d  %s' % (e.get('episode') or 0, e.get('title') or e.get('label', '')))}|{lib_flag(e.get('playcount'), e.get('resume'))}|")
    elif what == "continue":
        flt = {"field": "inprogress", "operator": "true", "value": ""}       # Kodi insists on the empty value
        for kind, method, key, props in (("e", "VideoLibrary.GetEpisodes", "episodes", ["title", "showtitle", "season", "episode", "resume", "playcount"]),
                                         ("m", "VideoLibrary.GetMovies", "movies", ["year", "resume", "playcount"])):
            r = kodi(method, {"filter": flt, "properties": props, "limits": {"start": 0, "end": 30}})
            if r is None:
                return 502, "error: kodi unreachable"
            for x in r.get(key) or []:
                if kind == "e":
                    lab = f"{x.get('showtitle', '')} S{x.get('season') or 0:02d}E{x.get('episode') or 0:02d} {x.get('title', '')}"
                    out.append(f"e|{x['episodeid']}|{lib_label(lab)}|2|")
                else:
                    out.append(f"m|{x['movieid']}|{lib_label(x.get('label', ''))}|2|")
    elif what == "recent":
        r = kodi("VideoLibrary.GetRecentlyAddedEpisodes", {"properties": ["title", "showtitle", "season", "episode", "playcount", "resume"], "limits": {"start": 0, "end": 25}})
        r2 = kodi("VideoLibrary.GetRecentlyAddedMovies", {"properties": ["year", "playcount", "resume"], "limits": {"start": 0, "end": 15}})
        if r is None and r2 is None:
            return 502, "error: kodi unreachable"
        for x in (r or {}).get("episodes") or []:
            lab = f"{x.get('showtitle', '')} S{x.get('season') or 0:02d}E{x.get('episode') or 0:02d} {x.get('title', '')}"
            out.append(f"e|{x['episodeid']}|{lib_label(lab)}|{lib_flag(x.get('playcount'), x.get('resume'))}|")
        for x in (r2 or {}).get("movies") or []:
            out.append(f"m|{x['movieid']}|{lib_label(x.get('label', ''))}|{lib_flag(x.get('playcount'), x.get('resume'))}|")
    else:
        return 404, "error: unknown library list"
    return 200, "".join(l + "\n" for l in out)


def lib_item(kind, ident):
    """({'movieid': n} or {'episodeid': n}, label) when Kodi has that item, else None"""
    if kind not in ("m", "e") or not str(ident).isdigit():
        return None
    if kind == "m":
        r = (kodi("VideoLibrary.GetMovieDetails", {"movieid": int(ident), "properties": ["title"]}) or {}).get("moviedetails")
        return ({"movieid": int(ident)}, r.get("title", "movie")) if r else None
    r = (kodi("VideoLibrary.GetEpisodeDetails", {"episodeid": int(ident), "properties": ["title"]}) or {}).get("episodedetails")
    return ({"episodeid": int(ident)}, r.get("title", "episode")) if r else None


def lib_queue(mode, kind, ident):
    """play now / play next / add to the end. The queue is saved first; add and next can be undone."""
    found = lib_item(kind, ident)
    if not found:
        return 404, {"error": "no such item in the library"}
    item, label = found
    if DRY:
        return 200, {"dry_run": True, "would_call": f"library {mode} {kind} {ident}"}
    files = queue_files()
    n = len(files)
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["playlistid", "position"]}) or {}
    cur = pr["position"] if pr.get("playlistid") == 1 else -1
    with QLOCK:
        snapshot(f"before library {mode}")
        if mode == "add":
            kodi("Playlist.Add", {"playlistid": 1, "item": item})
            UNDO.append(("remove", n))
            del UNDO[:-20]
            return 200, {"result": f"added to the end: {clean(label, 40)}"}
        if mode == "next":
            pos = cur + 1 if cur >= 0 else n
            kodi("Playlist.Insert", {"playlistid": 1, "position": pos, "item": item})
            UNDO.append(("remove", pos))
            del UNDO[:-20]
            return 200, {"result": f"plays next: {clean(label, 40)}"}
        if cur >= 0:                                     # play now while something is playing: put it right after, then step to it
            kodi("Playlist.Insert", {"playlistid": 1, "position": cur + 1, "item": item})
            kodi("Player.GoTo", {"playerid": 1, "to": "next"})
        else:
            kodi("Playlist.Add", {"playlistid": 1, "item": item})
            kodi("Player.Open", {"item": {"playlistid": 1, "position": n}})
        return 200, {"result": f"playing: {clean(label, 40)}"}


def lib_addseason(show, season):
    if not (str(show).isdigit() and str(season).lstrip("-").isdigit()):
        return 400, {"error": "bad show or season"}
    r = kodi("VideoLibrary.GetEpisodes", {"tvshowid": int(show), "season": int(season), "properties": ["episode"], "sort": {"method": "episode", "order": "ascending"}})
    eps = (r or {}).get("episodes") or []
    if not eps:
        return 404, {"error": "no episodes found for that season"}
    eps = eps[:80]
    if DRY:
        return 200, {"dry_run": True, "would_call": f"add {len(eps)} episodes"}
    n = len(queue_files())
    with QLOCK:
        snapshot("before adding a season")
        for e in eps:
            kodi("Playlist.Add", {"playlistid": 1, "item": {"episodeid": e["episodeid"]}})
        UNDO.append(("removerange", n, len(eps)))
        del UNDO[:-20]
    return 200, {"result": f"added {len(eps)} episodes to the end"}


CLEAR_FILE = f"{STATE}/queue-before-clear.json"


def queue_clear(mode):
    """mode 'keep': remove everything except the playing item (highest position first, so positions stay valid); 'all': Playlist.Clear.
    The queue is saved to CLEAR_FILE first, so 'restore/clear' can put it back; the periodic snapshots would be overwritten by the cleared queue within a minute."""
    items = queue_files()
    if not items:
        return 409, {"error": "the queue is already empty"}
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["playlistid", "position"]}) or {}
    cur = pr["position"] if pr.get("playlistid") == 1 else -1
    if DRY:
        return 200, {"dry_run": True, "would_call": f"clear {mode}"}
    with QLOCK:
        os.makedirs(STATE, exist_ok=True)
        with open(CLEAR_FILE, "w") as f:
            json.dump({"time": time.strftime("%F %T"), "reason": f"before clear {mode}", "files": [x for x in items if x and not is_stream(x)], "cur": cur}, f)
        snapshot(f"before clear {mode}")
        UNDO.clear()
        if mode == "all":
            kodi("Playlist.Clear", {"playlistid": 1})
            return 200, {"result": f"cleared all {len(items)} items"}
        for pos in range(len(items) - 1, -1, -1):
            if pos != cur:
                kodi("Playlist.Remove", {"playlistid": 1, "position": pos})
    return 200, {"result": f"cleared {len(items) - (1 if cur >= 0 else 0)} items, the playing one stays"}


def queue_restore_cleared():
    try:
        snap = json.load(open(CLEAR_FILE))
    except (OSError, ValueError):
        return 404, {"error": "no cleared queue saved"}
    with QLOCK:
        n = restore_queue(snap["files"])
    return 200, {"result": f"restored the queue from before the clear ({len(snap['files'])} items, {n} fixes)"}


def queue_undo():
    with QLOCK:
        if not UNDO:
            return 404, {"error": "nothing to undo"}
        op = UNDO.pop()
        if op[0] == "insert":
            kodi("Playlist.Insert", {"playlistid": 1, "position": op[1], "item": {"file": op[2]}})
            return 200, {"result": "put the removed item back"}
        if op[0] in ("remove", "removerange"):
            first, count = op[1], (op[2] if op[0] == "removerange" else 1)
            pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["playlistid", "position"]}) or {}
            cur = pr["position"] if pr.get("playlistid") == 1 else -1
            if first <= cur < first + count:
                UNDO.append(op)
                return 409, {"error": "that item is playing now; it cannot be taken out"}
            for pos in range(first + count - 1, first - 1, -1):
                kodi("Playlist.Remove", {"playlistid": 1, "position": pos})
            return 200, {"result": f"took out the {count} item{'s' if count != 1 else ''} just added"}
        swap_chain(op[1], op[2])
        return 200, {"result": f"moved {op[1]} back to {op[2]}"}


def queue_restore():
    names = sorted(os.listdir(SNAP_DIR)) if os.path.isdir(SNAP_DIR) else []
    if not names:
        return 404, {"error": "no snapshot yet"}
    with QLOCK:
        snap = json.load(open(f"{SNAP_DIR}/{names[-1]}"))
        n = restore_queue(snap["files"])
    return 200, {"result": f"restored from {snap['time']} ({n} fixes)"}


def queue_edit(action, nums):
    """POST /q/remove/<pos> and /q/move/<from>/<to>. Moves are chains of adjacent swaps, which Kodi keeps in step with the
    playing item. The playing item itself cannot be removed from here."""
    items = (kodi("Playlist.GetItems", {"playlistid": 1}) or {}).get("items") or []
    n = len(items)
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["playlistid", "position"]}) or {}
    cur = pr["position"] if pr.get("playlistid") == 1 else -1
    try:
        vals = [int(x) for x in nums]
    except ValueError:
        return 400, {"error": "positions must be numbers"}
    if any(v < 0 or v >= n for v in vals):
        return 400, {"error": f"position out of range (queue has {n})"}
    if action == "remove" and len(vals) == 1:
        if vals[0] == cur:
            return 409, {"error": "that item is playing; stop playback first"}
        if DRY:
            return 200, {"dry_run": True, "would_call": f"Playlist.Remove {vals[0]}"}
        with QLOCK:
            snapshot("before remove")
            UNDO.append(("insert", vals[0], items[vals[0]].get("file", "")))
            del UNDO[:-20]
            kodi("Playlist.Remove", {"playlistid": 1, "position": vals[0]})
        return 200, {"result": f"removed {vals[0]}"}
    if action == "move" and len(vals) == 2:
        a, b = vals
        if a == b:
            return 200, {"result": "already there"}
        if DRY:
            return 200, {"dry_run": True, "would_call": f"Playlist.Swap chain {a}->{b}"}
        with QLOCK:
            snapshot("before move")
            UNDO.append(("move", b, a))
            del UNDO[:-20]
            swap_chain(a, b)
        return 200, {"result": f"moved {a} to {b}"}
    return 404, {"error": "unknown queue verb"}


def active_pid():
    players = kodi("Player.GetActivePlayers") or []
    return players[0]["playerid"] if players else None


def track_label(t):
    lang = (t.get("language") or "").upper()
    name = t.get("name") or t.get("codec") or ""
    ch = f" {t['channels']}ch" if t.get("channels") else ""
    return clean(" ".join(x for x in (lang, name) if x) + ch or "track", 40)


def tracks_view():
    """text: a|index|label|current  and  s|index|label|current (s|off first). None when nothing plays."""
    pid = active_pid()
    if pid is None:
        return None
    pr = kodi("Player.GetProperties", {"playerid": pid, "properties": ["audiostreams", "currentaudiostream", "subtitles", "currentsubtitle", "subtitleenabled"]}) or {}
    cur_a = (pr.get("currentaudiostream") or {}).get("index")
    cur_s = (pr.get("currentsubtitle") or {}).get("index")
    en = bool(pr.get("subtitleenabled"))
    rows = [f"a|{a['index']}|{track_label(a)}|{int(a['index'] == cur_a)}" for a in pr.get("audiostreams") or []]
    rows.append(f"s|off|Subtitles off|{int(not en)}")
    rows += [f"s|{t['index']}|{track_label(t)}|{int(en and t['index'] == cur_s)}" for t in pr.get("subtitles") or []]
    return "".join(r + "\n" for r in rows)


def set_track(kind, idx):
    pid = active_pid()
    if pid is None:
        return 409, {"error": "nothing is playing"}
    if DRY:
        return 200, {"dry_run": True, "would_call": f"track {kind} {idx}"}
    if kind == "a" and idx.isdigit():
        kodi("Player.SetAudioStream", {"playerid": pid, "stream": int(idx)})
        return 200, {"result": "audio track set"}
    if kind == "s" and idx == "off":
        kodi("Player.SetSubtitle", {"playerid": pid, "subtitle": "off"})
        return 200, {"result": "subtitles off"}
    if kind == "s" and idx.isdigit():
        kodi("Player.SetSubtitle", {"playerid": pid, "subtitle": int(idx), "enable": True})
        return 200, {"result": "subtitles on"}
    return 400, {"error": "bad track"}


def send_text(txt):
    txt = re.sub(r"[^\x20-\x7e]", "", txt)[:120]
    if not txt:
        return 400, {"error": "empty text"}
    if DRY:
        return 200, {"dry_run": True, "would_call": "Input.SendText"}
    kodi("Input.SendText", {"text": txt, "done": False})
    return 200, {"result": f"typed {len(txt)} characters"}


POWER = {"restartkodi": ("Application.Quit", None), "reboot": ("System.Reboot", None), "shutdown": ("System.Shutdown", None),
         "refresh": ("VideoLibrary.Scan", {})}
_LAST_POWER = [0.0]


def power(action, confirm):
    """restartkodi / reboot / shutdown need /confirm in the path and are limited to one per minute; refresh only rescans the library"""
    if action not in POWER:
        return 404, {"error": "unknown power action"}
    if action != "refresh" and confirm != "confirm":
        return 400, {"error": "needs /confirm"}
    if action != "refresh" and time.time() - _LAST_POWER[0] < 60:
        return 429, {"error": "one power action per minute"}
    if DRY:
        return 200, {"dry_run": True, "would_call": POWER[action][0]}
    if action != "refresh":
        _LAST_POWER[0] = time.time()
        try:
            snapshot(f"before {action}")
        except Exception:
            pass
        os.makedirs(STATE, exist_ok=True)
        with open(f"{STATE}/power.log", "a") as f:
            f.write(f"{time.strftime('%F %T')} {action}\n")
    method, params = POWER[action]
    kodi(method, params)
    return 200, {"result": {"restartkodi": "restarting Kodi", "reboot": "rebooting the box", "shutdown": "shutting the box down", "refresh": "scanning the video library"}[action]}


def queue_view():
    items = (kodi("Playlist.GetItems", {"playlistid": 1, "properties": ["title", "showtitle", "file"]}) or {}).get("items") or []
    pr = kodi("Player.GetProperties", {"playerid": 1, "properties": ["playlistid", "position"]}) or {}
    cur = pr["position"] if pr.get("playlistid") == 1 else -1
    rows = []
    for it in items:
        name = it.get("label") or it.get("title") or os.path.basename(it.get("file", ""))
        if name.startswith("videoplayback") or name.endswith(".m3u8") or not name:
            name = "(stream)"
        rows.append(name)
    return cur, rows


class H(BaseHTTPRequestHandler):
    server_version = "tvplay-api"
    txt = False

    def log_message(self, fmt, *a):                     # method and path only, never headers or query strings
        sys.stderr.write("%s %s -> %s\n" % (self.client_address[0], self.path.split("?")[0], a[1] if len(a) > 1 else ""))

    def send(self, code, obj):
        if self.txt:
            if isinstance(obj, str):
                body = obj
            elif "error" in obj:
                body = "error: " + clean(obj["error"], 80)
            elif obj.get("out"):
                body = clean(obj["out"][-1], 80)
            elif obj.get("dry_run"):
                body = "dry run: " + clean(obj.get("would_run") or obj.get("would_call"), 80)
            else:
                body = "ok"
            b = (body if body.endswith("\n") else body + "\n").encode()
            ctype = "text/plain"
        else:
            b = json.dumps(obj).encode()
            ctype = "application/json"
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(b)

    def authed(self):
        if hmac.compare_digest(self.headers.get("X-Token", ""), TOKEN):
            return True
        time.sleep(1)                                   # slow down guessing
        self.send(401, {"error": "bad token"})
        return False

    def route(self, method):
        global _ST
        if method == "GET" and self.path == "/healthz":     # no login, nothing sensitive: for monitors
            self.txt = True
            return self.send(200, f"ok {VERSION} up {int(time.time() - START)}s\n")
        if not self.authed():
            return
        if method != "GET":
            _ST = (0.0, None)                                # an action may change what status shows
        parts = [p for p in self.path.split("?")[0].split("/") if p]
        self.txt = bool(parts) and parts[0] == "t"
        if self.txt:
            parts = parts[1:]
        v = parts[0] if parts else ""
        arg = parts[1] if len(parts) > 1 else ""
        arg2 = parts[2] if len(parts) > 2 else ""
        if method == "GET":
            if v == "status":
                d = status()
                return self.send(502 if "error" in d else 200, status_text(d) if self.txt else d)
            if v == "lib":
                code, text = lib_view(arg, arg2, parts[3] if len(parts) > 3 else "")
                return self.send(code, text)
            if v == "showlist":
                return self.send(200, showlist_text() if self.txt else [{"mode": m, "key": k, "label": l} for m, k, l in refresh_shows()])
            if v == "tracks":
                tv = tracks_view()
                return self.send(200 if tv is not None else 409, tv if tv is not None else "nothing is playing")
            if v == "shows":
                return self.send(200, "\n".join(sorted(SHOWS)) if self.txt else {"random": sorted(RANDOM_OK), "order": sorted(SHOWS)})
            if v == "channels":
                rows = channels_view()
                return self.send(200, "".join(f"{p}|{n}|{'?' if lv is None else int(lv)}|{TITLE.get((p, n), '') if lv else ''}|{CAT.get((p, n), '') if lv else ''}\n" for p, n, lv in rows) if self.txt
                                 else [{"platform": p, "name": n, "live": lv, "title": TITLE.get((p, n), "") if lv else "", "category": CAT.get((p, n), "") if lv else ""} for p, n, lv in rows])
            if v == "favs":
                rows = favs_view()
                return self.send(200, "".join(f"{i}|{clean(n)}\n" for i, n in rows) if self.txt else [{"id": i, "name": n} for i, n in rows])
            if v == "queue":
                cur, rows = queue_view()
                return self.send(200, f"cur={cur}\n" + "".join(f"{i}|{clean(n)}\n" for i, n in enumerate(rows)) if self.txt
                                 else {"cur": cur, "items": rows})
            return self.send(404, {"error": "unknown view"})
        if method != "POST":
            return self.send(405, {"error": "use POST for actions"})
        if v in ("random", "order"):
            refresh_shows()
        if v == "random" and arg in RANDOM_OK:
            return self.send(*run_tvplay("random", SHOWS[arg]))
        if v == "order" and arg in SHOWS:
            return self.send(*run_tvplay("order", SHOWS[arg]))
        if v == "live" and arg in PLAT and (arg, arg2.lower()) in channels():
            global LAST_LIVE
            code, res = run_tvplay(arg, arg2.lower())
            if code == 200 and not DRY:
                LAST_LIVE = f"{arg}/{arg2.lower()}"
            return self.send(code, res)
        if v == "fav" and arg.isdigit() and int(arg) in {i for i, _ in favs_view()}:
            if DRY:
                return self.send(200, {"dry_run": True, "would_call": f"Player.Open channelid {arg}"})
            return self.send(200, {"result": kodi("Player.Open", {"item": {"channelid": int(arg)}})})
        if v == "goto" and arg.isdigit():
            _, rows = queue_view()
            if int(arg) < len(rows):
                if DRY:
                    return self.send(200, {"dry_run": True, "would_call": f"goto {arg}"})
                return self.send(200, {"result": kodi("Player.Open", {"item": {"playlistid": 1, "position": int(arg)}})})
        if v == "q" and arg == "clear" and arg2 in ("keep", "all"):
            code, res = queue_clear(arg2)
            return self.send(code, (res.get("result") or res.get("error")) if self.txt else res)
        if v == "q" and arg in ("undo", "restore"):
            code, res = queue_undo() if arg == "undo" else (queue_restore_cleared() if arg2 == "clear" else queue_restore())
            return self.send(code, (res.get("result") or res.get("error")) if self.txt else res)
        if v == "lib" and arg in ("play", "next", "add") and len(parts) > 3:
            code, res = lib_queue(arg, arg2, parts[3])
            return self.send(code, (res.get("result") or res.get("error")) if self.txt else res)
        if v == "lib" and arg == "addseason" and len(parts) > 3:
            code, res = lib_addseason(arg2, parts[3])
            return self.send(code, (res.get("result") or res.get("error")) if self.txt else res)
        if v == "seek" and arg:
            code, res = seek("pct", arg2) if arg == "pct" else seek("delta", arg)
            return self.send(code, (res.get("result") or res.get("error") or "ok") if self.txt else res)
        if v in ("shuffle", "repeat"):
            code, res = playback_toggle(v)
            return self.send(code, (res.get("result") or res.get("error") or "ok") if self.txt else res)
        if v == "track" and arg in ("a", "s"):
            code, res = set_track(arg, arg2)
            return self.send(code, (res.get("result") or res.get("error") or "ok") if self.txt else res)
        if v == "text":
            q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
            code, res = send_text((q.get("s") or [""])[0])
            return self.send(code, (res.get("result") or res.get("error") or "ok") if self.txt else res)
        if v == "power":
            code, res = power(arg, arg2)
            return self.send(code, (res.get("result") or res.get("error") or "ok") if self.txt else res)
        if v == "q" and arg in ("remove", "move"):
            code, res = queue_edit(arg, [x for x in (arg2, parts[3] if len(parts) > 3 else "") if x != ""])
            if self.txt and isinstance(res, dict):
                res = (res.get("result") or res.get("error") or res.get("would_call") or "ok")
            return self.send(code, res)
        if v == "stop":
            return self.send(*run_tvplay("stop"))
        if v == "sleep" and (arg == "off" or (arg.isdigit() and 1 <= int(arg) <= 300)):
            return self.send(*run_tvplay("sleep", arg))
        if v == "key" and arg in KEYS:
            return self.send(200, {"dry_run": True, "would_call": KEYS[arg]} if DRY else {"result": kodi(KEYS[arg])})
        if v == "do" and arg == "fullscreen":      # show the playing video full screen from any Kodi menu (the "now playing" button)
            if DRY:
                return self.send(200, {"dry_run": True, "would_call": "GUI.SetFullscreen true"})
            res = kodi("GUI.SetFullscreen", {"fullscreen": True})
            if self.txt:
                return self.send(200, "Fullscreen\n" if res is True else "Nothing playing\n")
            return self.send(200, {"result": res})
        if v == "do" and arg == "playpause":
            return self.send(200, {"dry_run": True, "would_call": "Player.PlayPause"} if DRY else {"result": kodi("Player.PlayPause", {"playerid": 1})})
        if v == "do" and arg in ACTIONS:
            if DRY:
                return self.send(200, {"dry_run": True, "would_call": ACTIONS[arg]})
            res = kodi("Input.ExecuteAction", {"action": ACTIONS[arg]})
            if arg in ("volup", "voldown", "mute"):          # answer with the level Kodi actually has now: the PSP shows it itself
                time.sleep(0.08)
                app = kodi("Application.GetProperties", {"properties": ["volume", "muted"]}) or {}
                label = "Muted" if app.get("muted") else f"Volume {app.get('volume')}"
                return self.send(200, label + "\n" if self.txt else {"result": res, "volume": app.get("volume"), "muted": bool(app.get("muted"))})
            return self.send(200, {"result": res})
        return self.send(404, {"error": "unknown verb"})

    def do_GET(self):  self.route("GET")
    def do_POST(self): self.route("POST")


if __name__ == "__main__":
    threading.Thread(target=live_loop, daemon=True).start()
    threading.Thread(target=snap_loop, daemon=True).start()
    srv = ThreadingHTTPServer((BIND, PORT), H)
    srv.daemon_threads = True
    print(f"tvplay-api listening on {BIND}:{PORT} (dry run: {DRY})", flush=True)
    srv.serve_forever()
