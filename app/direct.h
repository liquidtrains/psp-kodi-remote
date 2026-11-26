/* Kodi-alone mode: translate the app's /t/ requests into Kodi JSON-RPC, and Kodi's replies into the plain text the
 * UI already understands (the same text tvplay-api sends in server mode). Pure C with no PSP calls, so it is unit-tested
 * on a normal computer (app/test/dk_test.c). Nothing here is used in server mode. */
#ifndef DIRECT_H
#define DIRECT_H

enum { DK_PLAIN = 1, DK_STATUS, DK_VOL, DK_MUTE, DK_QUEUE, DK_FULL, DK_BATCH, DK_TRACKS, DK_LIB_MOVIES, DK_LIB_SHOWS, DK_LIB_SEASONS, DK_LIB_EPS, DK_LIB_CONT, DK_LIB_RECENT };

/* method/path as the UI uses them ("POST", "/t/key/up"). Writes the JSON-RPC body and the reply kind.
 * Returns 0, or -1 when Kodi alone cannot do it (shows, live channels, favourites, sleep timer need the server). */
int dk_request(const char *method, const char *path, char *body, int bodysz, int *kind);

/* Kodi's HTTP status and JSON body -> the UI's text in out. Returns an HTTP-like code (200 ok). */
int dk_reply(int kind, int http_code, const char *json, char *out, int outsz);

/* Complete HTTP request for Kodi's /jsonrpc. user may be empty (no Authorization header). */
int dk_http(char *out, int outsz, const char *host, int port, const char *user, const char *pass, const char *body);

/* A whole raw HTTP response -> 0 not Kodi, 1 Kodi without login, 2 Kodi that wants a login (401, realm Kodi/XBMC). */
int dk_probe(const char *resp);

/* The JSON-RPC ping used to find and test Kodi. */
extern const char DK_PING[];

#endif
