/* AmiFritzi - Zugriff auf die FRITZ!Box. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fritz.h"
#include "md5.h"
#include "mem.h"

void fb_session_init(struct FritzSession *s, const char *host)
{
    memset(s, 0, sizeof(*s));
    strncpy(s->host, host, FB_HOST_LEN - 1);
    s->port = 80;
    strcpy(s->sid, FB_SID_NONE);
}

/* ------------------------------------------------------------------ */
/* Rechnungen                                                          */
/* ------------------------------------------------------------------ */

void fb_aha_response(const char *challenge, const char *pass, char *out,
                     int outsize)
{
    struct MD5Ctx c;
    unsigned char d[16];
    char hex[33];
    const unsigned char *p;
    unsigned char u16[2];

    /* Gehasht wird "<challenge>-<passwort>" als UTF-16LE. Der Amiga
     * arbeitet in ISO-8859-1, und Latin-1 ist genau der Bereich
     * U+0000..U+00FF - jedes Zeichen wird also einfach Byte, 0x00.
     * Zeichen jenseits davon gibt es in Latin-1 nicht; AVM verlangt, sie
     * durch '.' zu ersetzen, das Problem stellt sich hier also nicht. */
    md5_init(&c);
    u16[1] = 0;
    for (p = (const unsigned char *)challenge; *p; p++) {
        u16[0] = *p;
        md5_update(&c, u16, 2);
    }
    u16[0] = '-';
    md5_update(&c, u16, 2);
    for (p = (const unsigned char *)pass; *p; p++) {
        u16[0] = *p;
        md5_update(&c, u16, 2);
    }
    md5_final(&c, d);
    md5_digest_hex(d, hex);

    /* Die Challenge ist 8 Zeichen lang; die Grenze schuetzt nur vor einer
     * Box, die Unsinn schickt. */
    if ((int)strlen(challenge) + 34 > outsize) {
        out[0] = '\0';
        return;
    }
    sprintf(out, "%s-%s", challenge, hex);
}

void fb_tr064_response(const char *uid, const char *realm, const char *pwd,
                       const char *sn, char out[33])
{
    struct MD5Ctx c;
    unsigned char d[16];
    char secret[33];

    /* Zweistufig und auf Hex-Ebene: die zweite Runde hasht den
     * Hex-STRING des ersten Ergebnisses, nicht die 16 Rohbytes. */
    md5_init(&c);
    md5_update(&c, uid, strlen(uid));
    md5_update(&c, ":", 1);
    md5_update(&c, realm, strlen(realm));
    md5_update(&c, ":", 1);
    md5_update(&c, pwd, strlen(pwd));
    md5_final(&c, d);
    md5_digest_hex(d, secret);

    md5_init(&c);
    md5_update(&c, secret, 32);
    md5_update(&c, ":", 1);
    md5_update(&c, sn, strlen(sn));
    md5_final(&c, d);
    md5_digest_hex(d, out);
}

static void xml_unescape(char *s)
{
    static const char *ent[][2] = {
        { "&amp;", "&" }, { "&lt;", "<" }, { "&gt;", ">" },
        { "&quot;", "\"" }, { "&apos;", "'" },
    };
    char *r = s, *w = s;
    unsigned i;

    while (*r) {
        if (*r == '&') {
            for (i = 0; i < sizeof(ent) / sizeof(ent[0]); i++) {
                int n = (int)strlen(ent[i][0]);
                if (strncmp(r, ent[i][0], n) == 0) {
                    *w++ = ent[i][1][0];
                    r += n;
                    break;
                }
            }
            if (i < sizeof(ent) / sizeof(ent[0])) {
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = '\0';
}

int xml_tag(const char *src, const char *tag, char *out, int outsize)
{
    char open[48];
    const char *a, *e;
    int n;

    if (strlen(tag) + 3 > sizeof(open)) {
        return 0;
    }
    sprintf(open, "<%s>", tag);
    a = strstr(src, open);
    if (!a) {
        return 0;
    }
    a += strlen(open);
    e = strstr(a, "</");
    if (!e) {
        return 0;
    }
    n = (int)(e - a);
    if (n > outsize - 1) {
        n = outsize - 1;
    }
    memcpy(out, a, n);
    out[n] = '\0';
    xml_unescape(out);
    return 1;
}

void xml_escape(const char *in, char *out, int outsize)
{
    int n = 0;

    while (*in && n < outsize - 7) {
        switch (*in) {
        case '&': strcpy(out + n, "&amp;");  n += 5; break;
        case '<': strcpy(out + n, "&lt;");   n += 4; break;
        case '>': strcpy(out + n, "&gt;");   n += 4; break;
        case '"': strcpy(out + n, "&quot;"); n += 6; break;
        default:  out[n++] = *in;
        }
        in++;
    }
    out[n] = '\0';
}

void utf8_to_latin1(char *s)
{
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;

    while (*r) {
        if (*r < 0x80) {
            *w++ = *r++;
        } else if ((*r & 0xE0) == 0xC0 && (r[1] & 0xC0) == 0x80) {
            unsigned c = ((unsigned)(*r & 0x1F) << 6) | (r[1] & 0x3F);
            *w++ = (c < 256) ? (unsigned char)c : '?';
            r += 2;
        } else {
            /* Laengere Folgen haben in Latin-1 keine Entsprechung. */
            *w++ = '?';
            r++;
            while ((*r & 0xC0) == 0x80) {
                r++;
            }
        }
    }
    *w = '\0';
}

void latin1_to_utf8(const char *in, char *out, int outsize)
{
    const unsigned char *r = (const unsigned char *)in;
    int n = 0;

    while (*r && n < outsize - 2) {
        if (*r < 0x80) {
            out[n++] = (char)*r;
        } else {
            out[n++] = (char)(0xC0 | (*r >> 6));
            out[n++] = (char)(0x80 | (*r & 0x3F));
        }
        r++;
    }
    out[n] = '\0';
}

/* Der Benutzer mit last="1" aus <Users>: <User last="1">name</User> */
static int last_user(const char *xml, char *out, int outsize)
{
    const char *a = strstr(xml, "last=\"1\">");
    const char *e;
    int n;

    if (!a) {
        return 0;
    }
    a += 9;
    e = strchr(a, '<');
    if (!e) {
        return 0;
    }
    n = (int)(e - a);
    if (n > outsize - 1) {
        n = outsize - 1;
    }
    memcpy(out, a, n);
    out[n] = '\0';
    return n > 0;
}

/* ------------------------------------------------------------------ */
/* Login                                                               */
/* ------------------------------------------------------------------ */

static int get_ok(struct FritzSession *s, const char *path,
                  struct HttpResponse *r)
{
    char msg[64];
    int rc = http_get(s->host, s->port, path, r);

    if (rc != FB_OK) {
        return rc;
    }
    if (r->status == 403) {
        http_free(r);
        return net_fail(FB_EAUTH, "Zugriff verweigert (SID ungueltig?)");
    }
    if (r->status < 200 || r->status > 299) {
        sprintf(msg, "HTTP-Status %d", r->status);
        http_free(r);
        return net_fail(FB_EHTTP, msg);
    }
    return FB_OK;
}

int fb_login(struct FritzSession *s, const char *user, const char *pass)
{
    struct HttpResponse r;
    char challenge[80];
    char response[128];
    char uuser[2 * FB_USER_LEN];
    char euser[3 * sizeof(uuser) + 1];
    char path[640];         /* euser bis 385 + response bis 127 */
    char tmp[24];
    int rc;

    strcpy(s->sid, FB_SID_NONE);
    s->blocktime = 0;

    /* Ohne version=2 liefert die Box die MD5-Challenge. Die PBKDF2-Variante
     * ("2$...") wuerde SHA-256 brauchen - auf einem 68020 obendrein
     * zigtausend Iterationen. MD5 bleibt laut AVM weiter erlaubt. */
    rc = get_ok(s, "/login_sid.lua", &r);
    if (rc != FB_OK) {
        return rc;
    }
    if (!xml_tag(r.body, "Challenge", challenge, sizeof(challenge))) {
        http_free(&r);
        return net_fail(FB_EHTTP, "Keine Challenge in login_sid.lua");
    }
    if (user && *user) {
        strncpy(s->user, user, FB_USER_LEN - 1);
        s->user[FB_USER_LEN - 1] = '\0';
    } else if (last_user(r.body, s->user, FB_USER_LEN)) {
        utf8_to_latin1(s->user);
    } else {
        s->user[0] = '\0';      /* sehr alte Firmware: Login ohne Namen */
    }
    if (xml_tag(r.body, "BlockTime", tmp, sizeof(tmp))) {
        s->blocktime = atol(tmp);
    }
    http_free(&r);

    fb_aha_response(challenge, pass, response, sizeof(response));
    /* s->user steht in Latin-1, die Box erwartet UTF-8. */
    latin1_to_utf8(s->user, uuser, sizeof(uuser));
    url_encode(uuser, euser, sizeof(euser));
    sprintf(path, "/login_sid.lua?username=%s&response=%s", euser, response);

    rc = get_ok(s, path, &r);
    if (rc != FB_OK) {
        return rc;
    }
    if (!xml_tag(r.body, "SID", s->sid, FB_SID_LEN)) {
        http_free(&r);
        strcpy(s->sid, FB_SID_NONE);
        return net_fail(FB_EHTTP, "Keine SID in login_sid.lua");
    }
    if (xml_tag(r.body, "BlockTime", tmp, sizeof(tmp))) {
        s->blocktime = atol(tmp);
    }
    http_free(&r);

    if (strcmp(s->sid, FB_SID_NONE) == 0) {
        char msg[96];

        if (s->blocktime > 0) {
            sprintf(msg, "Login abgelehnt - Box sperrt fuer %ld s",
                    s->blocktime);
        } else {
            strcpy(msg, "Login abgelehnt - Benutzer oder Passwort falsch");
        }
        return net_fail(FB_EAUTH, msg);
    }
    return FB_OK;
}

void fb_default_user(const char *host, int port, char *user, int usize)
{
    struct HttpResponse r;

    user[0] = '\0';
    if (http_get(host, port, "/login_sid.lua", &r) != FB_OK) {
        return;
    }
    if (r.status == 200 && last_user(r.body, user, usize)) {
        utf8_to_latin1(user);
    }
    http_free(&r);
}

int fb_logout(struct FritzSession *s)
{
    struct HttpResponse r;
    char path[64];
    int rc;

    if (strcmp(s->sid, FB_SID_NONE) == 0) {
        return FB_OK;
    }
    sprintf(path, "/login_sid.lua?logout=1&sid=%s", s->sid);
    rc = get_ok(s, path, &r);
    if (rc == FB_OK) {
        http_free(&r);
    }
    strcpy(s->sid, FB_SID_NONE);
    return rc;
}

/* ------------------------------------------------------------------ */
/* AHA                                                                 */
/* ------------------------------------------------------------------ */

static void chomp(char *s)
{
    int n = (int)strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'
                     || s[n - 1] == ' ')) {
        s[--n] = '\0';
    }
}

int fb_aha(struct FritzSession *s, const char *cmd, const char *ain,
           char **out)
{
    struct HttpResponse r;
    char eain[3 * FB_AIN_LEN + 1];
    char path[256];
    int rc;

    *out = NULL;
    if (ain) {
        /* Die AIN enthaelt ein Leerzeichen ("08761 0000434"). */
        url_encode(ain, eain, sizeof(eain));
        sprintf(path, "/webservices/homeautoswitch.lua?sid=%s&switchcmd=%s"
                "&ain=%s", s->sid, cmd, eain);
    } else {
        sprintf(path, "/webservices/homeautoswitch.lua?sid=%s&switchcmd=%s",
                s->sid, cmd);
    }

    rc = get_ok(s, path, &r);
    if (rc != FB_OK) {
        return rc;
    }
    chomp(r.body);
    /* Rumpf an den Anfang des Puffers, damit der Aufrufer nur fb_free()
     * braucht. */
    memmove(r.raw, r.body, strlen(r.body) + 1);
    *out = r.raw;
    return FB_OK;
}

static int parse_state(const char *v)
{
    if (v[0] == '1' && v[1] == '\0') {
        return 1;
    }
    if (v[0] == '0' && v[1] == '\0') {
        return 0;
    }
    return -1;                  /* "inval": Geraet nicht erreichbar */
}

int fb_switch_list(struct FritzSession *s, struct FritzSwitch **list,
                   int *count)
{
    char *ains, *p, *v;
    struct FritzSwitch *l;
    int n, i, rc;

    *list = NULL;
    *count = 0;

    rc = fb_aha(s, "getswitchlist", NULL, &ains);
    if (rc != FB_OK) {
        return rc;
    }

    /* Kommagetrennt, leer wenn keine Steckdose angelernt ist. */
    n = ains[0] ? 1 : 0;
    for (p = ains; *p; p++) {
        if (*p == ',') {
            n++;
        }
    }
    if (n == 0) {
        fb_free(ains);
        return FB_OK;
    }

    l = fb_calloc(n, sizeof(*l));
    if (!l) {
        fb_free(ains);
        return net_fail(FB_EMEM, "Kein Speicher");
    }

    p = ains;
    for (i = 0; i < n; i++) {
        char *comma = strchr(p, ',');

        if (comma) {
            *comma = '\0';
        }
        strncpy(l[i].ain, p, FB_AIN_LEN - 1);
        l[i].state = -1;
        l[i].power_mw = -1;
        p = comma ? comma + 1 : p + strlen(p);
    }
    fb_free(ains);

    for (i = 0; i < n; i++) {
        if (fb_aha(s, "getswitchname", l[i].ain, &v) == FB_OK) {
            utf8_to_latin1(v);
            strncpy(l[i].name, v, FB_NAME_LEN - 1);
            fb_free(v);
        }
        if (fb_aha(s, "getswitchstate", l[i].ain, &v) == FB_OK) {
            l[i].state = parse_state(v);
            fb_free(v);
        }
        rc = fb_aha(s, "getswitchpower", l[i].ain, &v);
        if (rc == FB_OK) {
            if (v[0] >= '0' && v[0] <= '9') {
                l[i].power_mw = atol(v);
            }
            fb_free(v);
        }
        if (rc == FB_EAUTH) {
            fb_free(l);
            return rc;
        }
    }

    *list = l;
    *count = n;
    return FB_OK;
}

int fb_switch_set(struct FritzSession *s, const char *ain, const char *cmd,
                  int *newstate)
{
    char *v;
    int rc = fb_aha(s, cmd, ain, &v);

    if (rc != FB_OK) {
        return rc;
    }
    *newstate = parse_state(v);
    fb_free(v);
    return FB_OK;
}
