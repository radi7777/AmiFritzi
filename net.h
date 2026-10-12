/* AmiFritzi - TCP und minimales HTTP ueber bsdsocket.library.
 *
 * Laeuft unveraendert auch auf einem Unix-Rechner (BSD-Sockets). Das ist
 * Absicht: so lassen sich Protokoll und Login gegen tests/mockbox.py
 * pruefen, ohne Amiga und ohne echte FRITZ!Box. */

#ifndef FB_NET_H
#define FB_NET_H

#define FB_OK      0
#define FB_ENET   -1    /* Namensaufloesung, Verbindung, Zeitgrenze */
#define FB_EMEM   -2
#define FB_EHTTP  -3    /* Antwort kaputt oder Status nicht 2xx */
#define FB_EAUTH  -4    /* Login abgelehnt / SID ungueltig */

struct HttpResponse {
    int   status;           /* 200, 403, ... */
    long  content_length;   /* -1, wenn der Server keine Laenge nennt */
    char *raw;              /* ganzer Puffer, gehoert der Struktur */
    char *status_line;      /* "HTTP/1.1 200 OK" */
    char *headers;          /* Kopfzeilen ohne Statuszeile, \r\n-getrennt */
    char *body;             /* NUL-terminiert */
    long  body_len;
};

/* Ein Request. extra_headers darf NULL sein, sonst fertige Zeilen mit
 * abschliessendem \r\n. body darf NULL sein.
 * Liefert FB_OK auch bei Status 4xx/5xx - den prueft der Aufrufer, weil die
 * FRITZ!Box ihre Fehler (403 = SID abgelaufen, 500 = SOAP-Fault) im Status
 * mitteilt und die Rumpfe dann trotzdem gebraucht werden. */
int http_request(const char *host, int port, const char *method,
                 const char *path, const char *extra_headers,
                 const char *body, long body_len, struct HttpResponse *r);

int http_get(const char *host, int port, const char *path,
             struct HttpResponse *r);

void http_free(struct HttpResponse *r);

/* Sucht eine Kopfzeile (Gross/Klein egal) und kopiert ihren Wert. */
int http_header(const struct HttpResponse *r, const char *name,
                char *out, int outsize);

/* Prozent-Kodierung fuer Query-Parameter (RFC 3986, unreserved bleibt). */
void url_encode(const char *in, char *out, int outsize);

/* Schliesst bsdsocket.library - am Ende jedes Tasks, der Netz benutzt. */
void net_cleanup(void);

const char *net_last_error(void);
int net_fail(int code, const char *msg);

#endif
