/* AmiFritziCLI - Shell-Werkzeug fuer Phase 1 und 2, noch ohne MUI.
 *
 *   AmiFritziCLI [-h host] [-P port] [-T port] [-u user] [-p pass]
 *               <kommando> [arg...]
 *
 *   -P  Port fuer login_sid.lua/AHA (80), -T Port fuer TR-064 (49000)
 *
 *   get <pfad>         roher GET, Statuszeile und Kopf ausgeben (Phase 1)
 *   login              anmelden, SID ausgeben, wieder abmelden
 *   list               Steckdosen: AIN, Zustand, Leistung, Name
 *   on|off|toggle <AIN>
 *   info               Modell, FRITZ!OS-Version, Laufzeit (TR-064)
 *   wanip              aktuelle Internet-Adresse (TR-064)
 *   soap <dienst> <aktion>  beliebige TR-064-Aktion ohne Argumente,
 *                      Antwort roh ausgeben
 *
 * Das Passwort darf auch in der Umgebungsvariable FRITZPASS stehen, damit
 * es nicht in der Shell-History landet. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fritz.h"
#include "tr064.h"
#include "mem.h"

static const char VERSTAG[] __attribute__((used)) = "$VER: AmiFritziCLI 0.5 (12.10.2026)";

static void usage(void)
{
    printf("Usage: AmiFritziCLI [-h host] [-P port] [-T tr064port] "
           "[-u user] [-p pass] <command> [arg...]\n"
           "  get <path>          raw HTTP GET, print status and headers\n"
           "  login               log in, print SID, log out\n"
           "  list                list smart plugs\n"
           "  on|off|toggle <AIN> switch a smart plug\n"
           "  info                model, FRITZ!OS version, uptime (TR-064)\n"
           "  wanip               current internet address (TR-064)\n"
           "  soap <service> <action>  call a TR-064 action, print reply\n"
           "Password may also come from the environment variable "
           "FRITZPASS.\n");
}

static int report(int rc)
{
    if (rc != FB_OK) {
        printf("Error: %s\n", net_last_error());
        return 10;              /* RETURN_ERROR */
    }
    return 0;
}

static int cmd_get(const char *host, int port, const char *path)
{
    struct HttpResponse r;
    int rc = http_get(host, port, path, &r);

    if (rc != FB_OK) {
        return report(rc);
    }
    printf("%s\n", r.status_line);
    if (r.headers[0]) {
        printf("%s\n", r.headers);
    }
    printf("-- %ld bytes body", r.body_len);
    if (r.content_length >= 0) {
        printf(" (Content-Length %ld)", r.content_length);
    }
    printf(" --\n");
    http_free(&r);
    return 0;
}

static int cmd_list(struct FritzSession *s)
{
    struct FritzSwitch *l;
    int n, i, rc;

    rc = fb_switch_list(s, &l, &n);
    if (rc != FB_OK) {
        return report(rc);
    }
    if (n == 0) {
        printf("No smart plugs found.\n");
    }
    for (i = 0; i < n; i++) {
        const char *st = l[i].state == 1 ? "on "
                       : l[i].state == 0 ? "off" : "?  ";

        if (l[i].power_mw >= 0) {
            printf("%-16s %s %5ld.%03ld W  %s\n", l[i].ain, st,
                   l[i].power_mw / 1000, l[i].power_mw % 1000, l[i].name);
        } else {
            printf("%-16s %s          -  %s\n", l[i].ain, st, l[i].name);
        }
    }
    fb_free(l);
    return 0;
}

static int cmd_switch(struct FritzSession *s, const char *what,
                      const char *ain)
{
    const char *cmd;
    int state = -1;
    int rc;

    if (strcmp(what, "on") == 0) {
        cmd = "setswitchon";
    } else if (strcmp(what, "off") == 0) {
        cmd = "setswitchoff";
    } else {
        cmd = "setswitchtoggle";
    }
    rc = fb_switch_set(s, ain, cmd, &state);
    if (rc != FB_OK) {
        return report(rc);
    }
    printf("%s: %s\n", ain, state == 1 ? "on" : state == 0 ? "off" : "?");
    return 0;
}

static int cmd_tr064(struct Tr064 *t, const char *cmd, const char *a1,
                     const char *a2)
{
    char *resp;
    int rc;

    if (strcmp(cmd, "wanip") == 0) {
        char ip[48];

        rc = tr_external_ip(t, ip, sizeof(ip));
        if (rc != FB_OK) {
            return report(rc);
        }
        printf("%s\n", ip);
        return 0;
    }

    if (strcmp(cmd, "info") == 0) {
        char model[64], ver[32], up[16];
        long secs;

        rc = tr_call(t, "urn:dslforum-org:service:DeviceInfo:1", "GetInfo",
                     NULL, &resp);
        if (rc != FB_OK) {
            return report(rc);
        }
        model[0] = ver[0] = up[0] = '\0';
        xml_tag(resp, "NewModelName", model, sizeof(model));
        xml_tag(resp, "NewSoftwareVersion", ver, sizeof(ver));
        xml_tag(resp, "NewUpTime", up, sizeof(up));
        fb_free(resp);
        utf8_to_latin1(model);
        secs = atol(up);
        printf("Model:   %s\nFRITZ!OS: %s\nUptime:  %ldd %02ld:%02ld\n",
               model, ver, secs / 86400, (secs / 3600) % 24,
               (secs / 60) % 60);
        return 0;
    }

    /* soap <dienst> <aktion>; "DeviceInfo:1" wird zum vollen Namen. */
    {
        char svc[128];

        if (strncmp(a1, "urn:", 4) == 0) {
            strncpy(svc, a1, sizeof(svc) - 1);
            svc[sizeof(svc) - 1] = '\0';
        } else {
            sprintf(svc, "urn:dslforum-org:service:%.90s", a1);
        }
        rc = tr_call(t, svc, a2, NULL, &resp);
        if (rc != FB_OK) {
            return report(rc);
        }
        utf8_to_latin1(resp);
        printf("%s\n", resp);
        fb_free(resp);
        return 0;
    }
}

static int cli_main(int argc, char **argv)
{
    struct FritzSession s;
    const char *host = "fritz.box";
    const char *user = "";
    const char *pass = getenv("FRITZPASS");
    const char *cmd = NULL;
    const char *arg = NULL;
    const char *arg2 = NULL;
    int port = -1;
    int trport = -1;
    int i, rc, ret;

    (void)VERSTAG;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2] && i + 1 < argc) {
            switch (argv[i][1]) {
            case 'h': host = argv[++i]; continue;
            case 'P': port = atoi(argv[++i]); continue;
            case 'T': trport = atoi(argv[++i]); continue;
            case 'u': user = argv[++i]; continue;
            case 'p': pass = argv[++i]; continue;
            }
        }
        if (!cmd) {
            cmd = argv[i];
        } else if (!arg) {
            arg = argv[i];
        } else if (!arg2) {
            arg2 = argv[i];
        } else {
            usage();
            return 5;
        }
    }
    if (!cmd) {
        usage();
        return 5;
    }
    if (!pass) {
        static char phost[64], puser[64], ppass[128];
        /* Dieselben Einstellungen wie die MUI-Oberflaeche (bis 0.25 unter
         * dem alten Namen AmiFritz). */
        FILE *f = fopen("ENV:AmiFritzi.prefs", "r");
        if (!f) f = fopen("ENVARC:AmiFritzi.prefs", "r");
        if (!f) f = fopen("ENVARC:AmiFritz.prefs", "r");
        if (f) {
            char line[200];
            while (fgets(line, sizeof(line), f)) {
                char *nl = strpbrk(line, "\r\n");
                if (nl) *nl = '\0';
                if (strncmp(line, "HOST=", 5) == 0 && line[5] && strcmp(host, "fritz.box") == 0) {
                    strncpy(phost, line + 5, sizeof(phost) - 1);
                    host = phost;
                } else if (strncmp(line, "USER=", 5) == 0 && line[5] && !*user) {
                    strncpy(puser, line + 5, sizeof(puser) - 1);
                    user = puser;
                } else if (strncmp(line, "PASS=", 5) == 0 && line[5]) {
                    strncpy(ppass, line + 5, sizeof(ppass) - 1);
                    pass = ppass;
                }
            }
            fclose(f);
        }
        if (!pass) {
            pass = "";
        }
    }

    if (strcmp(cmd, "get") == 0) {
        return cmd_get(host, port > 0 ? port : 80, arg ? arg : "/");
    }

    if (strcmp(cmd, "info") == 0 || strcmp(cmd, "wanip") == 0
            || (strcmp(cmd, "soap") == 0 && arg && arg2)) {
        struct Tr064 t;
        char tuser[64];

        /* TR-064 braucht den Namen schon fuer die Challenge. */
        if (!*user) {
            fb_default_user(host, port > 0 ? port : 80, tuser,
                            sizeof(tuser));
            user = tuser;
        }
        tr_init(&t, host, user, pass);
        if (trport > 0) {
            t.port = trport;
        }
        ret = cmd_tr064(&t, cmd, arg, arg2);
        tr_free(&t);
        return ret;
    }

    if (strcmp(cmd, "login") != 0 && strcmp(cmd, "list") != 0
            && !((strcmp(cmd, "on") == 0 || strcmp(cmd, "off") == 0
                  || strcmp(cmd, "toggle") == 0) && arg)) {
        usage();
        return 5;
    }

    fb_session_init(&s, host);
    if (port > 0) {
        s.port = port;
    }
    rc = fb_login(&s, user, pass);
    if (rc != FB_OK) {
        return report(rc);
    }

    if (strcmp(cmd, "login") == 0) {
        printf("Logged in as '%s', SID %s\n", s.user, s.sid);
        ret = 0;
    } else if (strcmp(cmd, "list") == 0) {
        ret = cmd_list(&s);
    } else {
        ret = cmd_switch(&s, cmd, arg);
    }

    /* Jede SID belegt auf der Box einen Platz, bis sie nach 20 Minuten
     * verfaellt - also ordentlich abmelden. */
    fb_logout(&s);
    return ret;
}

int main(int argc, char **argv)
{
    int ret = cli_main(argc, argv);

    net_cleanup();
    return ret;
}
