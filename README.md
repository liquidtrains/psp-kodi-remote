# Kodi Remote for PSP

A PSP homebrew (EBOOT) that controls Kodi over Wi-Fi. It has a dark graphical interface, a now-playing bar with progress, a volume bar, and menus for the play queue, audio and subtitle tracks, typing into Kodi, power actions, IPTV favourites and a sleep timer. A bridge server adds live Twitch and Kick channels and random or in-order episodes.

Version 0.3.0. Tested on a PSP-3000 running ARK-4 (6.61) in infrastructure mode, against Kodi 21 (LibreELEC). It is written for one person's setup, so read "What you need" first.

## Two ways to connect

| | Server + Kodi | Kodi alone |
|---|---|---|
| Needs | the small bridge server (`server/tvplay-api.py`) on a machine on your LAN | nothing but Kodi |
| Kodi's web password | stays on the server | typed on the PSP, saved on the card |
| Gives you | everything below, including live channels, episodes, queue undo and restore | keys, volume, play and pause, stop, fullscreen, queue (move, remove, play next), audio and subtitles, typing, power actions, sleep timer |

The app starts in server mode when `config.txt` has a token and in Kodi-alone mode otherwise, or as `mode=server` or `mode=direct` says. In Kodi-alone mode the app can scan your network for Kodi and asks for the login. Full setup for both: [docs/SETUP.md](docs/SETUP.md).

```
server mode:  PSP --HTTP, X-Token--> server/tvplay-api.py --JSON-RPC--> Kodi
Kodi alone:   PSP --HTTP, Basic auth--> Kodi web server (JSON-RPC)
```

## What you need

- A PSP with custom firmware that runs homebrew, and a saved Wi-Fi connection (Network Settings) on a network the PSP can join. The PSP only does 2.4 GHz. For a WPA2 network the ARK-4 `wpa2` setting (the `wpa2psp` plugin) must be on and the router must be WPA2 with AES only (no TKIP, no WPA/WPA2 mixed mode, no WPA3); with the plugin on, WEP and WPA networks stop working. If the join fails the app prints a checklist.
- Kodi with **Settings > Services > Control > Allow remote control via HTTP** on, port 8080, authentication on, and a username and password. Never forward that port to the internet.
- Server mode only: a machine on the LAN with Python 3.9+ and `curl` (`streamlink` for the live-channel list).
- The menus for random episodes, in-order episodes, Twitch/Kick and the sleep timer call an optional helper script (`tvplay`, path in `TVPLAY_BIN`) that is **not** included. Those entries need a script of your own. The show names in the Shows menu (`app/menus.c`, `open_shows`) are examples: edit them.

## Install

1. Server mode only: copy `server/tvplay-api.py` to the LAN machine, put Kodi's credentials in a curl config file (`~/.config/tvplay-api/kodi-curlrc`, mode 600, one line: `user = "kodi:PASSWORD"`), then run:

   ```
   KODI_URL=http://KODI-IP:8080/jsonrpc TVPLAY_API_BIND=YOUR-LAN-IP python3 server/tvplay-api.py
   ```

   The first start creates `~/.config/tvplay-api/token`. Optional: `~/.config/tvplay-api/channels.txt` with one `twitch:name` or `kick:name` per line.
2. Build the app (needs the PSPDEV toolchain, for example its Docker image). one EBOOT serves both modes.

   ```
   cd app && docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/src -w /src pspdev/pspdev:latest make
   ```

3. Copy `EBOOT.PBP` to `ms0:/PSP/GAME/KodiRemote/`. Server mode: create `config.txt` next to it from `config.txt.example` (server address, port, saved Wi-Fi profile name, token). **Do not publish or commit `config.txt` or `direct.txt`: they hold the token or the Kodi login.** Both can also be edited on the PSP under Settings.

## Controls

| Control | Does |
|---|---|
| D-pad | Kodi arrows |
| X / O | Kodi OK / back |
| Square | Kodi info (hold: show the playing video fullscreen) |
| L / R | volume down / up |
| Triangle | play / pause |
| Analog stick | move in the PSP menus (left / right = page) |
| START / SELECT | choose / back in the PSP menus |
| L + R together | Menu mode: d-pad, X, O drive the PSP menus |

The music-note button cannot be read from a normal EBOOT (the system keeps it), which is why fullscreen is on a Square hold.

## Menus

Queue (move, remove, play next, and in server mode undo and restore from snapshots), Audio and subtitles, Type text into Kodi, Settings (edit the connection on the PSP), Power (refresh library, restart Kodi, reboot or shut down the Kodi machine, each asking for START twice), Sleep timer, and in server mode Shows, Live and IPTV favourites.

## Server

All requests need the `X-Token` header; the `/t/` prefix gives plain-text replies (the PSP uses these). The server accepts a fixed list of actions only: it cannot queue arbitrary URLs or pass raw JSON-RPC through. It keeps a snapshot of the queue every minute, refuses a second power action within a minute, and requires `/confirm` on reboot and shutdown. See the docstring in `server/tvplay-api.py` for the routes. Tests: `python3 -I server/tests/test_api.py` runs it against a fake Kodi and never touches a real one.

## Security notes

Plain HTTP with a shared token (server mode) or Basic auth (Kodi alone): home LAN only, never port-forward, bind the server to a LAN address. Anyone who can read the memory card has the token or the Kodi login. Rotate the token by deleting the server's token file and restarting it.

## Known limits

- Written and tested on one PSP and one Kodi 21 box. Text on screen is Latin only, in the PSP's font.
- The Kodi-alone network scan covers one /24 and port 8080; otherwise type the address.
- Server mode needs the server reachable from the PSP's Wi-Fi network.

## Credits and licence

- Code written for this project: MIT, see `LICENSE`.
- [PSPDEV](https://github.com/pspdev/pspdev) toolchain and SDK.
- **intraFont 0.31 by BenHur** (parts by InsertWittyName, G-spec code by Geecko), licensed Creative Commons Attribution-Share Alike 3.0. The binary links it; the MIT licence covers only the code written for this project.
- Kodi is a trademark of the XBMC Foundation; this project is not affiliated with it.
