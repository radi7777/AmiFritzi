/* AmiFritz - Auftraege an den Netzwerkprozess, siehe jobs.h. */

#include <stdlib.h>
#include <string.h>

#include "jobs.h"
#include "mem.h"

void job_state_init(struct JobState *st)
{
    memset(st, 0, sizeof(*st));
    fb_session_init(&st->fs, "");
}

void job_state_free(struct JobState *st)
{
    if (st->connected) {
        fb_logout(&st->fs);
    }
    tr_free(&st->tr);
    memset(st->pass, 0, sizeof(st->pass));
    st->connected = 0;
}

void job_result_free(struct JobData *j)
{
    fb_free(j->sw);
    j->sw = NULL;
    j->swcount = 0;
}

static void take_error(char *dst, int size)
{
    strncpy(dst, net_last_error(), size - 1);
    dst[size - 1] = '\0';
}

/* Die SID verfaellt nach 20 Minuten ohne Zugriff, und die Box startet auch
 * mal neu. Beides zeigt sich als 403 (FB_EAUTH) - dann einmal still neu
 * anmelden und den Aufruf wiederholen, statt den Anwender zu fragen. */
static int relogin(struct JobState *st)
{
    return fb_login(&st->fs, st->fs.user, st->pass);
}

static int list_switches(struct JobState *st, struct JobData *j)
{
    int rc = fb_switch_list(&st->fs, &j->sw, &j->swcount);

    if (rc == FB_EAUTH && relogin(st) == FB_OK) {
        rc = fb_switch_list(&st->fs, &j->sw, &j->swcount);
    }
    return rc;
}

static void tr064_info(struct JobState *st, struct JobData *j)
{
    char up[16];
    char *resp;
    int rc;

    j->uptime = -1;
    tr_free(&st->tr);
    tr_init(&st->tr, st->fs.host, st->fs.user, st->pass);
    if (st->trport > 0) {
        st->tr.port = st->trport;
    }

    rc = tr_call(&st->tr, "urn:dslforum-org:service:DeviceInfo:1", "GetInfo",
                 NULL, &resp);
    if (rc != FB_OK) {
        take_error(j->tr_error, sizeof(j->tr_error));
        return;
    }
    xml_tag(resp, "NewModelName", j->model, sizeof(j->model));
    xml_tag(resp, "NewSoftwareVersion", j->fwver, sizeof(j->fwver));
    if (xml_tag(resp, "NewUpTime", up, sizeof(up))) {
        j->uptime = atol(up);
    }
    fb_free(resp);
    utf8_to_latin1(j->model);
    j->tr_ok = 1;

    /* Ohne Internetverbindung ist das kein Fehler der Anmeldung - die
     * Adresse bleibt dann eben leer. */
    if (tr_external_ip(&st->tr, j->wanip, sizeof(j->wanip)) != FB_OK) {
        j->wanip[0] = '\0';
    }
}

void job_run(struct JobState *st, struct JobData *j)
{
    j->rc = FB_OK;
    j->error[0] = '\0';
    j->sw = NULL;
    j->swcount = 0;

    switch (j->type) {
    case JOB_CONNECT:
        job_state_free(st);
        fb_session_init(&st->fs, j->host);
        if (j->port > 0) {
            st->fs.port = j->port;
        }
        st->trport = j->trport;
        memcpy(st->pass, j->pass, sizeof(st->pass));
        st->pass[sizeof(st->pass) - 1] = '\0';
        memset(j->pass, 0, sizeof(j->pass));   /* nicht zurueckschicken */

        j->tr_ok = 0;
        j->tr_error[0] = j->model[0] = j->fwver[0] = j->wanip[0] = '\0';
        j->uptime = -1;

        j->rc = fb_login(&st->fs, j->user, st->pass);
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
            memset(st->pass, 0, sizeof(st->pass));
            return;
        }
        st->connected = 1;
        strcpy(j->user_used, st->fs.user);

        /* Fehlen dem Benutzer die Smart-Home-Rechte, ist die Anmeldung
         * trotzdem gelungen - die Meldung kommt mit, rc bleibt FB_OK. */
        if (list_switches(st, j) != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        tr064_info(st, j);
        return;

    case JOB_REFRESH:
        if (!st->connected) {
            j->rc = FB_EAUTH;
            strcpy(j->error, "Nicht angemeldet");
            return;
        }
        j->rc = list_switches(st, j);
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        return;

    case JOB_SWITCH: {
        const char *cmd = j->on ? "setswitchon" : "setswitchoff";
        int state = -1;

        if (!st->connected) {
            j->rc = FB_EAUTH;
            strcpy(j->error, "Nicht angemeldet");
            return;
        }
        j->rc = fb_switch_set(&st->fs, j->ain, cmd, &state);
        if (j->rc == FB_EAUTH && relogin(st) == FB_OK) {
            j->rc = fb_switch_set(&st->fs, j->ain, cmd, &state);
        }
        if (j->rc != FB_OK) {
            take_error(j->error, sizeof(j->error));
        }
        j->on = state;          /* was die Box danach meldet, -1 unbekannt */
        return;
    }

    case JOB_LOGOUT:
    case JOB_QUIT:
        job_state_free(st);
        return;
    }
}
