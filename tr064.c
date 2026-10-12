/* AmiFritzi - TR-064 (SOAP) mit AVMs SOAP-Auth. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tr064.h"
#include "fritz.h"
#include "mem.h"

#define TR_AUTH_NS "http://soap-authentication.org/digest/2001/10/"

void tr_init(struct Tr064 *t, const char *host, const char *user,
             const char *pass)
{
    memset(t, 0, sizeof(*t));
    strncpy(t->host, host, sizeof(t->host) - 1);
    t->port = 49000;
    latin1_to_utf8(user, t->user, sizeof(t->user));
    latin1_to_utf8(pass, t->pass, sizeof(t->pass));
}

void tr_free(struct Tr064 *t)
{
    fb_free(t->desc);
    memset(t, 0, sizeof(*t));
}

const char *tr_fault_text(int code)
{
    switch (code) {
    case 401: return "nicht angemeldet - Benutzer oder Passwort falsch";
    case 402: return "ungueltige Argumente";
    case 501: return "Aktion fehlgeschlagen";
    case 503: return "Box sperrt nach zu vielen Fehlversuchen";
    case 606: return "angemeldet, aber keine Rechte fuer diese Aktion";
    case 714: return "Eintrag nicht vorhanden";
    case 820: return "interner Fehler der Box";
    case 866:
    case 867:
    case 868: return "Zwei-Faktor-Bestaetigung an der Box noetig";
    }
    return "unbekannter SOAP-Fehler";
}

/* ------------------------------------------------------------------ */
/* Dienstbeschreibung                                                  */
/* ------------------------------------------------------------------ */

static int load_desc(struct Tr064 *t)
{
    struct HttpResponse r;
    char msg[64];
    int rc;

    if (t->desc) {
        return FB_OK;
    }
    rc = http_get(t->host, t->port, "/tr64desc.xml", &r);
    if (rc != FB_OK) {
        return rc;
    }
    if (r.status != 200) {
        sprintf(msg, "tr64desc.xml: HTTP-Status %d (TR-064 aus?)", r.status);
        http_free(&r);
        return net_fail(FB_EHTTP, msg);
    }
    memmove(r.raw, r.body, r.body_len + 1);
    t->desc = r.raw;
    return FB_OK;
}

int tr_control_url(struct Tr064 *t, const char *service, char *url,
                   int urlsize)
{
    char want[128];
    const char *p, *end;

    if (load_desc(t) != FB_OK) {
        return 0;
    }
    if (strlen(service) + 30 > sizeof(want)) {
        return 0;
    }
    sprintf(want, "<serviceType>%s</serviceType>", service);
    p = strstr(t->desc, want);
    if (!p) {
        return 0;
    }
    /* controlURL steht im selben <service>-Block - nicht ueber dessen Ende
     * hinaus suchen, sonst erwischt man die des naechsten Dienstes. */
    end = strstr(p, "</service>");
    p = strstr(p, "<controlURL>");
    if (!p || (end && p > end)) {
        return 0;
    }
    return xml_tag(p, "controlURL", url, urlsize);
}

/* ------------------------------------------------------------------ */
/* Aufruf                                                              */
/* ------------------------------------------------------------------ */

static char *build_envelope(struct Tr064 *t, const char *service,
                            const char *action, const char *args)
{
    char uid[3 * sizeof(t->user)];
    char hdr[512];
    char *env;

    xml_escape(t->user, uid, sizeof(uid));

    if (t->nonce[0]) {
        char auth[33];

        fb_tr064_response(t->user, t->realm, t->pass, t->nonce, auth);
        sprintf(hdr,
                "<h:ClientAuth xmlns:h=\"" TR_AUTH_NS "\" "
                "s:mustUnderstand=\"1\">"
                "<Nonce>%s</Nonce><Auth>%s</Auth>"
                "<UserID>%s</UserID><Realm>%s</Realm>"
                "</h:ClientAuth>",
                t->nonce, auth, uid, t->realm);
    } else {
        sprintf(hdr,
                "<h:InitChallenge xmlns:h=\"" TR_AUTH_NS "\" "
                "s:mustUnderstand=\"1\">"
                "<UserID>%s</UserID>"
                "</h:InitChallenge>",
                uid);
    }

    env = fb_malloc(512 + strlen(hdr) + 2 * strlen(action) + strlen(service)
                 + (args ? strlen(args) : 0));
    if (!env) {
        return NULL;
    }
    sprintf(env,
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<s:Envelope "
            "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\" "
            "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
            "<s:Header>%s</s:Header>"
            "<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body>"
            "</s:Envelope>\r\n",
            hdr, action, service, args ? args : "", action);
    return env;
}

/* Ein Request. *resp bekommt den Rumpf (malloc). */
static int post_once(struct Tr064 *t, const char *url, const char *service,
                     const char *action, const char *args, char **resp,
                     int *status)
{
    struct HttpResponse r;
    char *env, *extra;
    int rc;

    *resp = NULL;
    env = build_envelope(t, service, action, args);
    extra = fb_malloc(128 + strlen(service) + strlen(action));
    if (!env || !extra) {
        fb_free(env);
        fb_free(extra);
        return net_fail(FB_EMEM, "Kein Speicher");
    }
    sprintf(extra,
            "Content-Type: text/xml; charset=\"utf-8\"\r\n"
            "SOAPACTION: \"%s#%s\"\r\n",
            service, action);

    rc = http_request(t->host, t->port, "POST", url, extra, env,
                      (long)strlen(env), &r);
    fb_free(env);
    fb_free(extra);
    if (rc != FB_OK) {
        return rc;
    }
    *status = r.status;
    memmove(r.raw, r.body, r.body_len + 1);
    *resp = r.raw;
    return FB_OK;
}

/* Liest die Challenge (erste Antwort) bzw. NextChallenge (jede weitere).
 * Liefert 1, wenn die Box "Authenticated" meldet. */
static int take_challenge(struct Tr064 *t, const char *resp)
{
    char status[24];
    char nonce[TR_NONCE_LEN];

    if (xml_tag(resp, "Nonce", nonce, sizeof(nonce))) {
        strcpy(t->nonce, nonce);
        xml_tag(resp, "Realm", t->realm, sizeof(t->realm));
    }
    return xml_tag(resp, "Status", status, sizeof(status))
        && strcmp(status, "Authenticated") == 0;
}

static int fault_code(const char *resp)
{
    char code[12];

    if (xml_tag(resp, "errorCode", code, sizeof(code))) {
        return atoi(code);
    }
    return 0;
}

int tr_call(struct Tr064 *t, const char *service, const char *action,
            const char *args, char **out)
{
    char url[TR_URL_LEN];
    char *resp;
    int status = 0, rc, attempt;

    *out = NULL;
    t->last_fault = 0;

    if (!tr_control_url(t, service, url, sizeof(url))) {
        if (t->desc) {
            char msg[160];
            sprintf(msg, "Dienst nicht vorhanden: %.120s", service);
            return net_fail(FB_EHTTP, msg);
        }
        return FB_ENET;         /* Meldung von load_desc */
    }

    /* Hoechstens zwei Anlaeufe: ohne Nonce holt der erste nur die
     * Challenge. Mit einer Nonce aus dem letzten Aufruf reicht meist einer;
     * ist sie inzwischen verfallen, liefert die Box eine neue mit und der
     * zweite Anlauf klappt. */
    for (attempt = 0; attempt < 2; attempt++) {
        int had_nonce = t->nonce[0] != '\0';
        int authed;

        rc = post_once(t, url, service, action, args, &resp, &status);
        if (rc != FB_OK) {
            return rc;
        }
        authed = take_challenge(t, resp);

        if (status == 200 && authed) {
            *out = resp;
            return FB_OK;
        }
        t->last_fault = fault_code(resp);

        /* Aufrufe, die keine Anmeldung brauchen, kommen ohne
         * Challenge-Kopf mit 200 zurueck. */
        if (status == 200 && !strstr(resp, "Challenge>")) {
            *out = resp;
            return FB_OK;
        }
        fb_free(resp);

        /* Rechte (606) und Zwei-Faktor (866-868) sind endgueltig. Sperrt
         * die Box trotz mitgeschickter Anmeldung (503), wuerde ein weiterer
         * Versuch die Sperre nur verlaengern. Ohne Nonce dagegen ist der
         * Fault nur die Aufforderung, sich anzumelden. */
        if (t->last_fault == 606
                || (t->last_fault >= 866 && t->last_fault <= 868)
                || (had_nonce && t->last_fault == 503)) {
            break;
        }
        if (had_nonce && attempt == 1) {
            break;
        }
    }

    if (t->last_fault == 0) {
        t->last_fault = 401;
    }
    {
        char msg[128];
        sprintf(msg, "TR-064 %d: %s", t->last_fault,
                tr_fault_text(t->last_fault));
        return net_fail(t->last_fault == 401 || t->last_fault == 503
                        ? FB_EAUTH : FB_EHTTP, msg);
    }
}

int tr_external_ip(struct Tr064 *t, char *ip, int ipsize)
{
    static const char *svc[] = {
        "urn:dslforum-org:service:WANPPPConnection:1",
        "urn:dslforum-org:service:WANIPConnection:1",
    };
    char *resp;
    int i, rc = FB_EHTTP;

    ip[0] = '\0';
    for (i = 0; i < 2; i++) {
        rc = tr_call(t, svc[i], "GetExternalIPAddress", NULL, &resp);
        if (rc == FB_ENET || rc == FB_EMEM) {
            return rc;
        }
        if (rc != FB_OK) {
            continue;           /* Dienst fehlt, inaktiv oder nicht erlaubt */
        }
        xml_tag(resp, "NewExternalIPAddress", ip, ipsize);
        fb_free(resp);
        if (ip[0] && strcmp(ip, "0.0.0.0") != 0) {
            return FB_OK;
        }
    }
    if (rc == FB_OK) {
        return net_fail(FB_EHTTP, "Keine Internetverbindung");
    }
    return rc;
}
