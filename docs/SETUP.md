# Setting up Kodi Remote for PSP

The app has two ways to reach Kodi. They share the same buttons and screens. With a `token=` in `config.txt` it uses the server mode; without one it uses Kodi alone. `mode=server` or `mode=direct` in `config.txt` overrides that.

| | Server + Kodi | Kodi alone |
|---|---|---|
| Status | Working, tested on hardware | Working, tested on a PSP against a real Kodi 21 |
| What runs besides Kodi | `server/tvplay-api.py` on an always-on machine | nothing |
| Menus | Remote, now-playing bar, volume, library, queue, audio and subtitles, typing, power, settings, plus what the server adds: random episodes, live channels with titles, favourites, sleep timer, seek, shuffle and repeat, queue undo and restore | Remote, now-playing bar, volume, fullscreen, library, queue, audio and subtitles, typing, power, settings, home, mute, next, previous, stop |
| Kodi password on the PSP card | no, a separate token | yes (Kodi's web user and password) |
| Finds Kodi by itself | no, you type the server address | yes: scans your network, or type the address |

## Before either mode

**Wi-Fi on a PSP.** The PSP only does 2.4 GHz. Out of the box it connects to open, WEP and WPA (TKIP) networks only. To use a normal WPA2 network you need the `wpa2psp` plugin, which current ARK-4 builds include as a setting: turn it on, then restart the PSP. The router must be WPA2 with AES only: switch off TKIP, WPA/WPA2 mixed mode and WPA3. While the plugin is on, old WEP and WPA networks stop working. The network must be saved under Settings > Network Settings and pass its connection test before this app can use it. If the join fails, the app prints this checklist before it closes (see the ARK-4 project for the current steps to enable the plugin).

1. **PSP:** custom firmware that runs homebrew, and a saved Wi-Fi connection (Settings > Network Settings) that works. The PSP only does 2.4 GHz.
2. **Kodi:** Settings > Services > Control > **Allow remote control via HTTP** on, port 8080 (the default), **Require authentication** on, with a username and password. Never forward this port to the internet. On Kodi 21 and later these settings are in the same place.
3. Build or download the EBOOT and copy it to `ms0:/PSP/GAME/KodiRemote/`. The README has the build command.

## Server + Kodi (works today)

Use this when you want more than a plain remote, or when you do not want Kodi's password on the memory card.

You need one machine that is always on and on the same network as Kodi: a Raspberry Pi, a mini PC, a NAS that runs Python, or a spare desktop. The PSP talks only to this machine. It talks to Kodi.

1. Install Python 3.9 or newer and `curl`.
2. Copy `server/tvplay-api.py` to the machine.
3. Create a curl config with Kodi's login (mode 600), for example `~/.config/tvplay-api/kodi-curlrc`, one line: `user = "kodiuser:kodipassword"`.
4. Start it, with Kodi's address and the machine's own LAN address:

   ```
   KODI_URL=http://KODI-IP:8080/jsonrpc TVPLAY_API_BIND=THIS-MACHINE-IP python3 tvplay-api.py
   ```

   The first start creates `~/.config/tvplay-api/token`. Treat it like a password.
5. To keep it running after a reboot, save this as `~/.config/systemd/user/tvplay-api.service` and run `systemctl --user enable --now tvplay-api` (and `loginctl enable-linger $USER` so it starts without a login):

   ```
   [Unit]
   Description=Kodi remote bridge for the PSP
   After=network-online.target

   [Service]
   Environment=KODI_URL=http://KODI-IP:8080/jsonrpc
   Environment=TVPLAY_API_BIND=THIS-MACHINE-IP
   ExecStart=/usr/bin/python3 %h/tvplay-api.py
   Restart=on-failure

   [Install]
   WantedBy=default.target
   ```
6. On the PSP card, create `config.txt` next to the EBOOT:

   ```
   host=THIS-MACHINE-IP
   port=8791
   ssid=NAME-OF-YOUR-SAVED-WIFI-PROFILE
   token=THE-TOKEN-FROM-STEP-4
   ```
7. Launch the app. The header shows the volume and the now-playing bar once it connects.

Problems: if the header says "no link to server", check that the PSP is on the same network (or that your router lets it reach the server's port 8791) and that the firewall on the server machine allows 8791 from the PSP.

Menus that need extras: random episodes, live channels and the sleep timer use an optional helper script (`TVPLAY_BIN`) that is not included. Keys, volume, play/pause, fullscreen, stop, the queue and IPTV favourites need only Kodi.

## Kodi alone

The PSP talks to Kodi's web server directly, with no second machine. Kodi's own phone remotes do the same. Status: tested against a real Kodi 21 on a computer (every request and reply) and on a PSP.

How it starts: with no `token=` in `config.txt` (or no `config.txt`) the app starts in this mode by itself. `mode=direct` in `config.txt` forces it, and `mode=server` forces the server mode. Its settings are kept in `direct.txt` next to the EBOOT, so a server-mode `config.txt` is never touched.

First start:

1. **Wi-Fi switch off:** the app says so and closes. Slide the WLAN switch on and start it again.
2. **Not connected to Wi-Fi:** the app explains, then opens the PSP's own list of saved networks. Pick yours. If nothing connects, the app says so and closes.
3. **Find Kodi:** the app shows how it will look: it takes the PSP's own address (for example 192.168.1.50) and tries port 8080 on every address from `.1` to `.254` in that network, 16 at a time, about 10 seconds, and asks each one that answers whether it is Kodi. Nothing is changed on any device. **X** scans, **O** lets you type the address instead.
4. **Choose:** if one or more Kodi answer, pick one. If none answer, the app lists what to check (Kodi's web server on, same Wi-Fi) and lets you scan again or type the address.
5. **Login:** Kodi's web server usually asks for a user name and password. The app asks with the PSP's on-screen keyboard. Kodi's default user name is `kodi`. (`root` is the LibreELEC SSH login and does not work here.) The password may be empty if Kodi has none.
6. The address and login are saved in `direct.txt` on the card. The next start connects by itself, and offers to scan again only if the saved Kodi does not answer. The main menu has "Kodi connection (find again)" to change it later.

How Kodi is recognised: Kodi's web server answers a request without a login with `401` and `WWW-Authenticate: Basic realm="Kodi"` (older versions say `XBMC`), or with `pong` if it has no login. Any other device is ignored.

What the menus do in this mode: show video fullscreen, the library, the queue, audio and subtitles, typing, power, settings, Kodi home screen, mute, next, previous, stop, controls help, and the buttons below. Random episodes, live channels, favourites, the sleep timer, seek, shuffle, repeat and queue undo need the server and are not shown.

Limits: the scan covers one /24 network and port 8080 only (type the address for another port, as `192.168.1.20:8081`). The Kodi password is stored in plain text on the memory card, so use a Kodi login you use nowhere else. If Kodi's "Allow remote control via HTTP" is off, nothing can find it.

## What the menus do

- **Library** (both modes): Continue watching, Recently added, Movies, and TV shows with seasons and episodes, read from Kodi's own video library. Watched items are dim and partly watched ones carry a marker. START on a movie or episode offers Play now, Play next (after the one playing) and Add to the end of the queue. The first row of an episode list adds the whole season in order. Lists of movies and shows load 40 at a time, with a "More" row at the end. In server mode an add or a play next can be undone with "Undo the last queue change"; Kodi alone has no undo, so remove the item from the queue instead.
- **Seek, shuffle, repeat** (server mode): jump 30 seconds, 1 minute or 10 minutes either way, or to a percentage; toggle shuffle (the server saves the queue first, because Kodi reorders it) and cycle repeat. The bar shows elapsed and total time, and SHUFFLE or REPEAT when on.
- **Queue:** the list shows Kodi's playlist (up to 128 items) and reloads by itself when Kodi moves to the next item or the list changes. START on an item opens its actions: play now, play next, move with the stick (the stick moves the item, START drops it), move up, move down, move to the end, remove. The playing item cannot be removed. The same menu can clear everything except the playing item, or the whole queue (red, START twice). With the server mode there is also "Undo the last queue change" and "Restore the queue from a snapshot" and "Bring back the queue from before the clear" (the server saves a snapshot every minute and before each edit, keeps the newest 30, saves the queue separately before every clear, and restores with inserts and swaps only).
- **Audio and subtitles:** lists the tracks of the playing item, the current ones marked; choose one to switch.
- **Type text into Kodi:** the PSP keyboard types into whatever text box Kodi has open (search, a login). Press OK on the remote afterwards.
- **Power:** refresh the video library; restart Kodi (about 30 seconds); restart or power off the Kodi machine (shown in red). The last three need START pressed twice within five seconds. In server mode the server also refuses a second power action within a minute.
- **Settings:** server address, port, token, Wi-Fi network name, debug line and the start mode, or in Kodi-alone mode the Kodi address, user name and password. Changes are saved to the card at once (`config.txt`, or `direct.txt`); the first save in a session keeps the old file as `config.txt.bak`. A mode change applies after restarting the app.
- **Live list** (server mode): the list starts empty and is yours. The rows at the bottom, "+ Add a Twitch channel", "+ Add a Kick channel" and "- Remove a channel", ask for a name on the PSP keyboard (the part after the last `/` in the channel's web address) and edit `~/.config/tvplay-api/channels.txt` on the server for you; comments and other lines in that file are kept, and you can still edit it by hand. Each live channel shows its stream category and title, the one you are watching is marked NOW, and the list refreshes itself every 30 seconds while it is open.
- **Version:** shown on the boot screen, every connect screen and the help screen, as `0.4.1 (commit)`.

## Optional: ARK-4 extras

On a PSP with ARK-4, a build made with `make ARK_EXTRAS=1` adds two things a normal EBOOT cannot do: the music-note button shows the playing video fullscreen, and after 30 seconds without a button the screen goes black (any button brings it back, and that first press is not passed on to Kodi). They work by looking up three kernel functions through ARK's SystemControl (`sctrlHENFindFunction`) and calling them with kubridge (`kuKernelCall`); nothing is called unless all three are found. The result and a heartbeat while the screen is dark go to `boot.txt` next to the EBOOT. This build needs ARK-4's SystemControl and kubridge: on other firmware it shows a load error at launch, which is why the released EBOOT is built without it.

## Server tests

`server/tests/test_api.py` starts the server against `fake_kodi.py` (a small stand-in for Kodi's JSON-RPC) and checks queue edits, clearing, undo, restore, the library lists and actions, seek, shuffle, repeat, tracks, text, power and the status fields. It never touches a real Kodi: run `python3 -I server/tests/test_api.py`.

## Controls (both modes)

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
