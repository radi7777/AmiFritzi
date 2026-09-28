/* Prueft job_run() - die Logik des Netzwerkprozesses - gegen die Mock-Box.
 * Aufruf: test_jobs <ahaport> <tr064port>; tests/test_jobs.py startet die
 * Box und ruft das hier auf. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jobs.h"

static int failures = 0;

static void check(const char *what, int cond, const struct JobData *j)
{
    if (cond) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s (rc %d, error '%s', tr_error '%s')\n", what, j->rc,
               j->error, j->tr_error);
        failures++;
    }
}

static int port, trport;

/* Laesst die Box alle SIDs vergessen, als waeren 20 Minuten um. */
static void expire_sessions(void)
{
    struct HttpResponse r;

    if (http_get("127.0.0.1", port, "/__test/expire", &r) == FB_OK) {
        http_free(&r);
    }
}

static int state_of(const struct JobData *j, const char *ain)
{
    int i;

    for (i = 0; i < j->swcount; i++) {
        if (strcmp(j->sw[i].ain, ain) == 0) {
            return j->sw[i].state;
        }
    }
    return -2;
}

static void job(struct JobState *st, struct JobData *j, int type)
{
    job_result_free(j);
    j->type = type;
    job_run(st, j);
}

int main(int argc, char **argv)
{
    struct JobState st;
    struct JobData j;

    if (argc != 3) {
        return 2;
    }
    port = atoi(argv[1]);
    trport = atoi(argv[2]);

    job_state_init(&st);
    memset(&j, 0, sizeof(j));
    strcpy(j.host, "127.0.0.1");
    j.port = port;
    j.trport = trport;

    strcpy(j.pass, "falsch");
    job(&st, &j, JOB_CONNECT);
    check("connect, wrong password", j.rc == FB_EAUTH, &j);

    strcpy(j.pass, "gurkensalat");
    job(&st, &j, JOB_CONNECT);
    check("connect", j.rc == FB_OK && j.error[0] == '\0', &j);
    check("connect: password not sent back", j.pass[0] == '\0', &j);
    check("connect: user from box", strcmp(j.user_used, "amiga") == 0, &j);
    check("connect: plugs", j.swcount == 3
          && state_of(&j, "08761 0000434") == 1, &j);
    check("connect: umlaut name", j.swcount == 3
          && strcmp(j.sw[2].name, "K\xfc" "chenlicht") == 0, &j);
    check("connect: tr064", j.tr_ok
          && strcmp(j.model, "FRITZ!Box 7590 AX & Mock") == 0
          && strcmp(j.fwver, "154.08.00") == 0
          && j.uptime == 3 * 86400 + 4 * 3600 + 5 * 60, &j);
    check("connect: wan ip", strcmp(j.wanip, "203.0.113.7") == 0, &j);

    strcpy(j.ain, "08761 0000435");
    j.on = 1;
    job(&st, &j, JOB_SWITCH);
    check("switch on", j.rc == FB_OK && j.on == 1, &j);

    expire_sessions();
    job(&st, &j, JOB_REFRESH);
    check("refresh after SID expired: silent re-login",
          j.rc == FB_OK && state_of(&j, "08761 0000435") == 1, &j);

    expire_sessions();
    strcpy(j.ain, "08761 0000435");
    j.on = 0;
    job(&st, &j, JOB_SWITCH);
    check("switch after SID expired", j.rc == FB_OK && j.on == 0, &j);

    job(&st, &j, JOB_LOGOUT);
    job(&st, &j, JOB_REFRESH);
    check("refresh after logout refused", j.rc == FB_EAUTH, &j);

    job_result_free(&j);
    job_state_free(&st);
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
