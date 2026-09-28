/* AmiFritz - Zugriff auf die FRITZ!Box: Login, AHA-Smart-Home, TR-064-Auth.
 *
 * Kennt nur Protokoll und Box, keine Oberflaeche. CLI und spaeter die
 * MUI-Oberflaeche benutzen ausschliesslich diese Schnittstelle. */

#ifndef FB_FRITZ_H
#define FB_FRITZ_H

#include "net.h"

#define FB_HOST_LEN  64
#define FB_USER_LEN  64
#define FB_SID_LEN   17         /* 16 Hex-Ziffern + NUL */
#define FB_AIN_LEN   24         /* "08761 0000434" */
#define FB_NAME_LEN  64

#define FB_SID_NONE  "0000000000000000"

struct FritzSession {
    char host[FB_HOST_LEN];
    int  port;                  /* 80 fuer login_sid.lua und AHA */
    char user[FB_USER_LEN];     /* nach dem Login: tatsaechlich benutzter Name */
    char sid[FB_SID_LEN];
    long blocktime;             /* Sekunden Sperre nach Fehlversuchen */
};

struct FritzSwitch {
    char ain[FB_AIN_LEN];
    char name[FB_NAME_LEN];
    int  state;                 /* 1 an, 0 aus, -1 unbekannt */
    long power_mw;              /* Milliwatt, -1 unbekannt */
};

void fb_session_init(struct FritzSession *s, const char *host);

/* Login ueber /login_sid.lua. user darf leer sein: dann nimmt die Funktion
 * den zuletzt angemeldeten Benutzer, den die Box in <Users> nennt (ab
 * FRITZ!OS 7.24 gibt es kein Login mehr nur mit Passwort).
 * pass ist ISO-8859-1, wie auf dem Amiga ueblich. */
int fb_login(struct FritzSession *s, const char *user, const char *pass);
int fb_logout(struct FritzSession *s);

/* Ein AHA-Kommando. ain darf NULL sein. *out bekommt den Rumpf (malloc,
 * vom Aufrufer freizugeben), Zeilenende abgeschnitten. */
int fb_aha(struct FritzSession *s, const char *cmd, const char *ain,
           char **out);

/* Liest alle Steckdosen samt Name, Zustand und Leistung.
 * *list per malloc, vom Aufrufer freizugeben. */
int fb_switch_list(struct FritzSession *s, struct FritzSwitch **list,
                   int *count);

/* cmd: "setswitchon", "setswitchoff" oder "setswitchtoggle".
 * *newstate bekommt den Zustand, den die Box danach meldet. */
int fb_switch_set(struct FritzSession *s, const char *ain, const char *cmd,
                  int *newstate);

/* ---- reine Rechnungen, ohne Netz (von tests/test_crypto.c geprueft) ---- */

/* AHA / login_sid.lua: challenge + "-" + MD5(UTF-16LE(challenge-pass)).
 * out braucht Platz fuer strlen(challenge) + 34. */
void fb_aha_response(const char *challenge, const char *pass, char *out,
                     int outsize);

/* TR-064: secret = MD5(uid:realm:pwd), response = MD5(secret:sn), beides
 * als Hex-String. */
void fb_tr064_response(const char *uid, const char *realm, const char *pwd,
                       const char *sn, char out[33]);

/* Inhalt von <tag>...</tag> ab src. 1 = gefunden. */
int xml_tag(const char *src, const char *tag, char *out, int outsize);

#endif
