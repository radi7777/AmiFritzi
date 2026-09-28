/* AmiFritz - Auftraege an den Netzwerkprozess.
 *
 * Die Oberflaeche fuellt eine JobData, schickt sie ab (worker.c) und
 * bekommt dieselbe Struktur mit den Ergebnissen zurueck. Was hier steht,
 * ist reines C ohne Exec - job_run() laeuft deshalb auch auf dem Unix-Host
 * und wird dort von tests/test_jobs.c gegen die Mock-Box geprueft.
 *
 * Die Sitzung (SID, TR-064-Nonce, Zugangsdaten) gehoert allein dem
 * Arbeitsprozess und steckt in JobState. Die Oberflaeche sieht nur
 * Ergebnisse, nie die Sitzung - so kann nichts gleichzeitig daran
 * schreiben. */

#ifndef FB_JOBS_H
#define FB_JOBS_H

#include "fritz.h"
#include "tr064.h"

enum JobType {
    JOB_CONNECT,    /* anmelden, dann wie JOB_REFRESH plus Geraeteinfo */
    JOB_REFRESH,    /* Steckdosen neu lesen */
    JOB_SWITCH,     /* eine Steckdose schalten */
    JOB_LOGOUT,
    JOB_QUIT        /* Arbeitsprozess beenden */
};

struct JobData {
    int type;

    /* Eingaben - JOB_CONNECT */
    char host[FB_HOST_LEN];
    int  port;                      /* 0 = 80 */
    int  trport;                    /* 0 = 49000 */
    char user[FB_USER_LEN];         /* leer = zuletzt angemeldeter */
    char pass[128];                 /* alles ISO-8859-1 */

    /* Eingaben - JOB_SWITCH */
    char ain[FB_AIN_LEN];
    int  on;                        /* 1 ein, 0 aus; zurueck kommt der
                                       Zustand laut Box, -1 unbekannt */

    /* Ergebnis, immer */
    int  rc;                        /* FB_OK oder FB_E... */
    char error[160];

    /* Ergebnis - JOB_CONNECT, JOB_REFRESH, JOB_SWITCH */
    struct FritzSwitch *sw;         /* fb_malloc; gehoert danach dem Empfaenger */
    int  swcount;

    /* Ergebnis - JOB_CONNECT */
    char user_used[FB_USER_LEN];
    int  tr_ok;                     /* TR-064 erreichbar und angemeldet */
    char tr_error[160];
    char model[64];
    char fwver[32];
    char wanip[48];
    long uptime;                    /* Sekunden, -1 unbekannt */
};

/* Gehoert dem Arbeitsprozess. */
struct JobState {
    struct FritzSession fs;
    struct Tr064 tr;
    char pass[128];                 /* fuer die erneute Anmeldung */
    int  trport;
    int  connected;
};

void job_state_init(struct JobState *st);
void job_state_free(struct JobState *st);

/* Arbeitet einen Auftrag ab. Blockiert, so lange das Netz braucht. */
void job_run(struct JobState *st, struct JobData *j);

/* Gibt die Ergebnisliste frei, die ein Auftrag mitgebracht hat. */
void job_result_free(struct JobData *j);

#endif
