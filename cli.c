/* AmiFritzCLI - Shell-Werkzeug fuer Phase 1 und 2, noch ohne MUI.
 *
 *   AmiFritzCLI [-h host] [-P port] [-u user] [-p pass] <kommando> [arg]
 *
 *   get <pfad>         roher GET, Statuszeile und Kopf ausgeben (Phase 1)
 *   login              anmelden, SID ausgeben, wieder abmelden
 *   list               Steckdosen: AIN, Zustand, Leistung, Name
 *   on|off|toggle <AIN>
 *
 * Das Passwort darf auch in der Umgebungsvariable FRITZPASS stehen, damit
 * es nicht in der Shell-History landet. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fritz.h"

static const char VERSTAG[] = "\0$VER: AmiFritzCLI 0.1 (28.09.2026)";

static void usage(void)
{
    printf("Usage: AmiFritzCLI [-h host] [-P port] [-u user] [-p pass] "
           "<command> [arg]\n"
           "  get <path>          raw HTTP GET, print status and headers\n"
           "  login               log in, print SID, log out\n"
           "  list                list smart plugs\n"
           "  on|off|toggle <AIN> switch a smart plug\n"
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
    free(l);
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

int main(int argc, char **argv)
{
    struct FritzSession s;
    const char *host = "fritz.box";
    const char *user = "";
    const char *pass = getenv("FRITZPASS");
    const char *cmd = NULL;
    const char *arg = NULL;
    int port = -1;
    int i, rc, ret;

    (void)VERSTAG;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !argv[i][2] && i + 1 < argc) {
            switch (argv[i][1]) {
            case 'h': host = argv[++i]; continue;
            case 'P': port = atoi(argv[++i]); continue;
            case 'u': user = argv[++i]; continue;
            case 'p': pass = argv[++i]; continue;
            }
        }
        if (!cmd) {
            cmd = argv[i];
        } else if (!arg) {
            arg = argv[i];
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
        pass = "";
    }

    if (strcmp(cmd, "get") == 0) {
        return cmd_get(host, port > 0 ? port : 80, arg ? arg : "/");
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
