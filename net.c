/* AmiFritz - TCP und minimales HTTP.
 *
 * Aufbau und Zeitgrenzen stammen aus AmiHomeassist, wo sie im Alltag
 * erprobt sind: nicht blockierender connect() mit WaitSelect(), damit eine
 * Box, die gerade neu startet, nicht 75 Sekunden lang alles einfriert. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "net.h"
#include "mem.h"

#if defined(__amigaos__) || defined(AMIGA) || defined(__AMIGA__)

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/socket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netdb.h>

/* Die Basis ist PRO TASK. Wer spaeter die Netzwerkaufrufe in einen eigenen
 * Prozess verlegt (Phase 4), muss sie dort oeffnen - deshalb oeffnet jeder
 * Request sie selbst, statt sie einmal in main() zu holen. */
struct Library *SocketBase = NULL;

/* bsdsocket.library zaehlt Fehler wie BSD, NICHT wie das errno.h von
 * libnix. Also die Werte hier selbst. */
#define NET_EWOULDBLOCK 35
#define NET_EINPROGRESS 36

static int lib_open(void)
{
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    return SocketBase != NULL;
}

static void lib_close(void)
{
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

#define sock_close(s)   CloseSocket(s)
#define sock_errno()    Errno()

static void sock_nonblock(int s)
{
    LONG nb = 1;
    IoctlSocket(s, FIONBIO, (APTR)&nb);
}

static long sock_select(int s, fd_set *rd, fd_set *wr, struct timeval *tv)
{
    /* bsdsocket erwartet 'struct __timeval' - Zeiger daher als APTR. */
    return WaitSelect(s + 1, (APTR)rd, (APTR)wr, NULL, (APTR)tv, NULL);
}

static unsigned long resolve_numeric(const char *host)
{
    return inet_addr((STRPTR)host);
}

static struct hostent *resolve_name(const char *host)
{
    return gethostbyname((UBYTE *)host);
}

#else   /* Unix-Host fuer Tests */

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define NET_EWOULDBLOCK EWOULDBLOCK
#define NET_EINPROGRESS EINPROGRESS

static int  lib_open(void)  { return 1; }
static void lib_close(void) { }

#define sock_close(s)   close(s)
#define sock_errno()    errno

static void sock_nonblock(int s)
{
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
}

static long sock_select(int s, fd_set *rd, fd_set *wr, struct timeval *tv)
{
    return select(s + 1, rd, wr, NULL, tv);
}

static unsigned long resolve_numeric(const char *host)
{
    return inet_addr(host);
}

static struct hostent *resolve_name(const char *host)
{
    return gethostbyname(host);
}

#endif

#define NET_CONNECT_SECS  5
#define NET_IO_SECS      15

static char g_error[160] = "";

const char *net_last_error(void)
{
    return g_error[0] ? g_error : "kein Fehler";
}

int net_fail(int code, const char *msg)
{
    strncpy(g_error, msg, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
    return code;
}

/* 1 = bereit, 0 = Zeit abgelaufen, -1 = Fehler */
static int sock_wait(int s, int forwrite, long secs)
{
    fd_set fds;
    struct timeval tv;
    long n;

    FD_ZERO(&fds);
    FD_SET(s, &fds);
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    n = sock_select(s, forwrite ? NULL : &fds, forwrite ? &fds : NULL, &tv);
    if (n < 0) {
        return -1;
    }
    return n > 0 ? 1 : 0;
}

static int connect_timeout(int s, struct sockaddr_in *sa)
{
    int err = 0;
    socklen_t errlen = sizeof(err);
    int w;

    sock_nonblock(s);
    if (connect(s, (struct sockaddr *)sa, sizeof(*sa)) == 0) {
        return FB_OK;
    }
    if (sock_errno() != NET_EINPROGRESS && sock_errno() != NET_EWOULDBLOCK) {
        return net_fail(FB_ENET, "Verbindung abgewiesen");
    }
    w = sock_wait(s, 1, NET_CONNECT_SECS);
    if (w == 0) {
        return net_fail(FB_ENET, "Zeitueberschreitung beim Verbinden");
    }
    /* Schreibbar heisst nur "fertig", nicht "geglueckt". */
    if (w < 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, (void *)&err, &errlen) < 0
            || err != 0) {
        return net_fail(FB_ENET, "Verbindung abgewiesen");
    }
    return FB_OK;
}

static int send_all(int s, const char *buf, long len)
{
    long sent = 0;

    while (sent < len) {
        int w = sock_wait(s, 1, NET_IO_SECS);
        long n;

        if (w == 0) {
            return net_fail(FB_ENET, "Zeitueberschreitung beim Senden");
        }
        if (w < 0) {
            return net_fail(FB_ENET, "Senden fehlgeschlagen");
        }
        n = send(s, (void *)(buf + sent), len - sent, 0);
        if (n < 0 && sock_errno() == NET_EWOULDBLOCK) {
            continue;
        }
        if (n <= 0) {
            return net_fail(FB_ENET, "Senden fehlgeschlagen");
        }
        sent += n;
    }
    return FB_OK;
}

static char *find_hdr_end(char *buf, long len)
{
    long i;

    for (i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n'
                && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return buf + i;
        }
    }
    return NULL;
}

static int ci_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

/* Content-Length aus dem Kopfblock [start, end). -1 = nicht angegeben. */
static long parse_content_length(const char *start, const char *end)
{
    const char *p = start;

    while (p < end) {
        if (ci_prefix(p, "Content-Length:")) {
            return atol(p + 15);
        }
        while (p < end && *p != '\n') {
            p++;
        }
        p++;
    }
    return -1;
}

/* Liest bis Verbindungsende - oder bis Content-Length erreicht ist. Das
 * zweite zaehlt: haelt ein Server die Verbindung trotz "Connection: close"
 * offen, waere sonst die Zeitgrenze das einzige Ende. */
static int recv_response(int s, char **out, long *outlen)
{
    long cap = 8192;
    long len = 0;
    long hdrlen = -1;
    long clen = -1;
    char *buf = fb_malloc(cap);

    if (!buf) {
        return net_fail(FB_EMEM, "Kein Speicher");
    }

    for (;;) {
        int w;
        long n;

        if (len + 2048 >= cap) {
            char *nb = fb_realloc(buf, cap * 2);
            if (!nb) {
                fb_free(buf);
                return net_fail(FB_EMEM, "Kein Speicher");
            }
            buf = nb;
            cap *= 2;
        }

        w = sock_wait(s, 0, NET_IO_SECS);
        if (w == 0) {
            fb_free(buf);
            return net_fail(FB_ENET, "Zeitueberschreitung beim Empfang");
        }
        if (w < 0) {
            fb_free(buf);
            return net_fail(FB_ENET, "Empfang fehlgeschlagen");
        }

        n = recv(s, (void *)(buf + len), cap - len - 1, 0);
        if (n < 0 && sock_errno() == NET_EWOULDBLOCK) {
            continue;
        }
        if (n <= 0) {
            break;
        }
        len += n;

        if (hdrlen < 0) {
            char *e = find_hdr_end(buf, len);
            if (e) {
                hdrlen = (long)(e - buf) + 4;
                clen = parse_content_length(buf, e);
            }
        }
        if (hdrlen >= 0 && clen >= 0 && len - hdrlen >= clen) {
            break;
        }
    }

    buf[len] = '\0';
    *out = buf;
    *outlen = len;
    return FB_OK;
}

static int split_response(char *raw, long rawlen, struct HttpResponse *r)
{
    char *eol, *hdrend;

    r->raw = raw;
    if (sscanf(raw, "HTTP/%*d.%*d %d", &r->status) != 1) {
        return net_fail(FB_EHTTP, "Keine HTTP-Antwort");
    }
    hdrend = find_hdr_end(raw, rawlen);
    if (!hdrend) {
        return net_fail(FB_EHTTP, "HTTP-Kopf unvollstaendig");
    }
    r->content_length = parse_content_length(raw, hdrend);

    eol = strstr(raw, "\r\n");
    *eol = '\0';
    r->status_line = raw;
    r->headers = (eol == hdrend) ? eol : eol + 2;   /* leer, falls keine */
    hdrend[0] = '\0';
    r->body = hdrend + 4;
    r->body_len = rawlen - (long)(r->body - raw);
    if (r->content_length >= 0 && r->content_length < r->body_len) {
        r->body_len = r->content_length;
        r->body[r->body_len] = '\0';
    }
    return FB_OK;
}

int http_request(const char *host, int port, const char *method,
                 const char *path, const char *extra_headers,
                 const char *body, long body_len, struct HttpResponse *r)
{
    struct sockaddr_in sa;
    struct hostent *he;
    unsigned long addr;
    char *req, *raw = NULL;
    long reqlen, rawlen = 0;
    int s, rc;

    memset(r, 0, sizeof(*r));
    r->content_length = -1;
    if (!body) {
        body_len = 0;
    }

    if (!lib_open()) {
        return net_fail(FB_ENET, "bsdsocket.library fehlt - TCP/IP-Stack gestartet?");
    }

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((unsigned short)port);

    /* Zahlenadresse spart den Resolver - die einzige Wartestelle, die
     * sich nicht begrenzen laesst. */
    addr = resolve_numeric(host);
    if (addr != (unsigned long)INADDR_NONE && addr != 0xFFFFFFFFUL) {
        sa.sin_addr.s_addr = addr;
    } else {
        he = resolve_name(host);
        if (!he) {
            lib_close();
            return net_fail(FB_ENET, "Rechnername nicht aufloesbar");
        }
        memcpy(&sa.sin_addr, he->h_addr_list[0], he->h_length);
    }

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        lib_close();
        return net_fail(FB_ENET, "Kein Socket");
    }

    rc = connect_timeout(s, &sa);
    if (rc != FB_OK) {
        sock_close(s);
        lib_close();
        return rc;
    }

    req = fb_malloc(512 + strlen(path) + strlen(host)
                 + (extra_headers ? strlen(extra_headers) : 0) + body_len);
    if (!req) {
        sock_close(s);
        lib_close();
        return net_fail(FB_EMEM, "Kein Speicher");
    }

    /* HTTP/1.0: dann schickt der Server nie chunked, und das Ende des
     * Rumpfs ist Content-Length oder Verbindungsende. */
    reqlen = sprintf(req,
                     "%s %s HTTP/1.0\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: AmiFritz\r\n"
                     "Connection: close\r\n"
                     "%s",
                     method, path, host, port,
                     extra_headers ? extra_headers : "");
    if (body) {
        reqlen += sprintf(req + reqlen, "Content-Length: %ld\r\n\r\n",
                          body_len);
        memcpy(req + reqlen, body, body_len);
        reqlen += body_len;
    } else {
        strcpy(req + reqlen, "\r\n");
        reqlen += 2;
    }

    rc = send_all(s, req, reqlen);
    fb_free(req);
    if (rc == FB_OK) {
        rc = recv_response(s, &raw, &rawlen);
    }
    sock_close(s);
    lib_close();
    if (rc != FB_OK) {
        return rc;
    }

    rc = split_response(raw, rawlen, r);
    if (rc != FB_OK) {
        http_free(r);
    }
    return rc;
}

int http_get(const char *host, int port, const char *path,
             struct HttpResponse *r)
{
    return http_request(host, port, "GET", path, NULL, NULL, 0, r);
}

void http_free(struct HttpResponse *r)
{
    fb_free(r->raw);
    memset(r, 0, sizeof(*r));
    r->content_length = -1;
}

int http_header(const struct HttpResponse *r, const char *name,
                char *out, int outsize)
{
    const char *p = r->headers;
    int nlen = (int)strlen(name);

    while (p && *p) {
        if (ci_prefix(p, name) && p[nlen] == ':') {
            const char *v = p + nlen + 1;
            int n = 0;

            while (*v == ' ' || *v == '\t') {
                v++;
            }
            while (v[n] && v[n] != '\r' && v[n] != '\n' && n < outsize - 1) {
                out[n] = v[n];
                n++;
            }
            out[n] = '\0';
            return 1;
        }
        p = strchr(p, '\n');
        if (p) {
            p++;
        }
    }
    return 0;
}

void url_encode(const char *in, char *out, int outsize)
{
    static const char hex[] = "0123456789ABCDEF";
    int n = 0;

    while (*in && n < outsize - 4) {
        unsigned char c = (unsigned char)*in++;

        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out[n++] = (char)c;
        } else {
            out[n++] = '%';
            out[n++] = hex[c >> 4];
            out[n++] = hex[c & 0x0F];
        }
    }
    out[n] = '\0';
}
