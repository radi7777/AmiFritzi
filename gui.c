/* AmiFritzi - MUI-Oberflaeche.
 *
 * Zwei Fenster: erst die Anmeldung (Box, Benutzer, Passwort), danach die
 * Uebersicht mit Geraeteinfo (TR-064) und den Steckdosen (AHA).
 *
 * Die Oberflaeche selbst spricht nie mit dem Netz. Jeder Zugriff ist ein
 * Auftrag an den Netzwerkprozess (worker.c); dessen Antwort kommt als
 * Signal in derselben Wait()-Schleife an wie MUIs Eingaben. Solange ein
 * Auftrag laeuft, sind die Knoepfe gesperrt - Fenster verschieben,
 * Groesse aendern und Beenden gehen weiter.
 */

#include <exec/types.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <libraries/mui.h>
#include <libraries/gadtools.h>
#include <clib/alib_protos.h>
#include <devices/timer.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAKE_ID
#define MAKE_ID(a,b,c,d) \
    ((ULONG)(a)<<24 | (ULONG)(b)<<16 | (ULONG)(c)<<8 | (ULONG)(d))
#endif

#include "fritz.h"
#include "jobs.h"
#include "mem.h"
#include "worker.h"
#include "stack.h"
#include "logo_data.h"
#include "debug.h"
#ifndef IPTR
typedef ULONG IPTR;
#endif

struct IntuitionBase *IntuitionBase = NULL;
struct Library *MUIMasterBase = NULL;

#define APP_VERSION "0.5"
#define APP_DATE    "12.10.2026"

const char *VERSTAG = "$VER: AmiFritzi " APP_VERSION " (" APP_DATE ")";

/* Steckdosen werden so oft neu gelesen, solange das Fenster offen ist. */
#define REFRESH_SECS 30

#define PREFS_ENV    "ENV:AmiFritzi.prefs"
#define PREFS_ENVARC "ENVARC:AmiFritzi.prefs"
/* Bis 0.25 hiess das Programm AmiFritz - alte Einstellungen weiter lesen. */
#define PREFS_OLD    "ENVARC:AmiFritz.prefs"

enum {
    ID_CONNECT = 1, ID_REFRESH, ID_LOGOUT,
    ID_ABOUT, ID_PREFS, ID_CLOSE_LOGIN, ID_QUIT,
    ID_PLUG = 1000              /* + Nummer der Steckdose */
};

static struct NewMenu g_menu[] = {
    { NM_TITLE, (STRPTR)"Projekt",           NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Ueber...",          (STRPTR)"?",  0, 0, (APTR)ID_ABOUT },
    { NM_ITEM,  (STRPTR)"Einstellungen...",  (STRPTR)"E",  0, 0, (APTR)ID_PREFS },
    { NM_ITEM,  (STRPTR)NM_BARLABEL,         NULL, 0, 0, NULL },
    { NM_ITEM,  (STRPTR)"Beenden",           (STRPTR)"Q",  0, 0, (APTR)ID_QUIT },
    { NM_END,   NULL,                        NULL, 0, 0, NULL }
};

static Object *app;
static Object *win_login, *str_host, *str_user, *str_pass, *bt_connect;
static Object *txt_login;
static Object *win_main, *bt_refresh, *bt_logout, *txt_status;

/* FRITZ!Box & Internet */
static Object *txt_model, *txt_fw, *txt_uptime;
static Object *txt_conn, *txt_ip, *txt_speed;

/* WLAN & Heimnetz */
static Object *txt_wlan, *txt_hosts;

/* Smart Home */
static Object *grp_plugs, *obj_plugs = NULL;

/* Eine Steckdose, wie sie gerade angezeigt wird. */
struct PlugUI {
    char    ain[FB_AIN_LEN];
    char    name[48];
    char    power_str[32];
    int     state;
    long    power_mw;
    Object *txt_state;
    Object *txt_power;
    Object *btn;
};

static struct PlugUI *g_plugs = NULL;
static int            g_nplugs = 0;

struct GuiState {
    char model[64];
    char fwver[32];
    char uptime[64];
    char conn[64];
    char ip[64];
    char speed[64];
    char wlan[64];
    char hosts[64];
    char status[256];
    char login_msg[160];
};
static struct GuiState g_state;

static struct JobMsg g_job;         /* hoechstens ein Auftrag unterwegs */
static BOOL          g_busy = FALSE;
static BOOL          g_connected = FALSE;

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static void say(Object *obj, const char *text)
{
    if (obj && text) {
        set(obj, MUIA_Text_Contents, (ULONG)text);
    }
}

static void set_status(const char *msg)
{
    strncpy(g_state.status, msg, sizeof(g_state.status) - 1);
    g_state.status[sizeof(g_state.status) - 1] = '\0';
    say(txt_status, g_state.status);
}

static Object *button(const char *label)
{
    return MUI_MakeObject(MUIO_Button, (ULONG)label);
}

static Object *label(const char *text)
{
    return MUI_MakeObject(MUIO_Label, (ULONG)text, 0);
}

static Object *text(const char *contents)
{
    return MUI_NewObject(MUIC_Text,
        MUIA_Text_Contents, (ULONG)contents,
        TAG_DONE);
}

static const char bg_blue[] = "2:0c0c0c0c,5c5c5c5c,a4a4a4a4";

static Object *make_logo(void)
{
    return MUI_NewObject(MUIC_Bodychunk,
        MUIA_FixWidth,              LOGO_WIDTH,
        MUIA_FixHeight,             LOGO_HEIGHT,
        MUIA_Bitmap_Width,          LOGO_WIDTH,
        MUIA_Bitmap_Height,         LOGO_HEIGHT,
        MUIA_Bodychunk_Depth,       LOGO_DEPTH,
        MUIA_Bodychunk_Body,        (UBYTE *)logo_body,
        MUIA_Bodychunk_Compression, 0,
        MUIA_Bodychunk_Masking,     0,
        MUIA_Bitmap_SourceColors,   (ULONG *)logo_colors,
        MUIA_Bitmap_Transparent,    0,
        TAG_DONE);
}

static Object *string(int maxlen, BOOL secret)
{
    return MUI_NewObject(MUIC_String,
        MUIA_Frame,          MUIV_Frame_String,
        MUIA_String_MaxLen,  maxlen,
        MUIA_String_Secret,  secret,
        MUIA_CycleChain,     TRUE,
        TAG_DONE);
}

static void string_get(Object *obj, char *out, int outsize)
{
    char *s = NULL;

    get(obj, MUIA_String_Contents, &s);
    strncpy(out, s ? s : "", outsize - 1);
    out[outsize - 1] = '\0';
}

static void set_busy(BOOL busy)
{
    g_busy = busy;
    set(bt_connect, MUIA_Disabled, busy);
    set(bt_refresh, MUIA_Disabled, busy);
    set(bt_logout,  MUIA_Disabled, busy);
    if (obj_plugs) {
        set(obj_plugs, MUIA_Disabled, busy);
    }
}

/* ------------------------------------------------------------------ */
/* Einstellungen: Box, Benutzer und Passwort in ENVARC: / ENV:        */
/* ------------------------------------------------------------------ */

static void prefs_load(char *host, int hsize, char *user, int usize, char *pass, int psize)
{
    char line[200];
    FILE *f = fopen(PREFS_ENV, "r");

    if (!f) {
        f = fopen(PREFS_ENVARC, "r");
    }
    if (!f) {
        f = fopen(PREFS_OLD, "r");
    }

    strcpy(host, "fritz.box");
    user[0] = '\0';
    if (pass && psize > 0) {
        pass[0] = '\0';
    }
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");

        if (nl) {
            *nl = '\0';
        }
        if (strncmp(line, "HOST=", 5) == 0 && line[5]) {
            strncpy(host, line + 5, hsize - 1);
            host[hsize - 1] = '\0';
        } else if (strncmp(line, "USER=", 5) == 0) {
            strncpy(user, line + 5, usize - 1);
            user[usize - 1] = '\0';
        } else if (pass && strncmp(line, "PASS=", 5) == 0 && line[5]) {
            strncpy(pass, line + 5, psize - 1);
            pass[psize - 1] = '\0';
        }
    }
    fclose(f);
}

static void prefs_save(const char *host, const char *user, const char *pass)
{
    static const char *files[] = { PREFS_ENV, PREFS_ENVARC };
    int i;

    for (i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "w");

        if (f) {
            fprintf(f, "HOST=%s\nUSER=%s\nPASS=%s\n",
                    host ? host : "",
                    user ? user : "",
                    pass ? pass : "");
            fclose(f);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Zeitgeber fuer das Nachlesen - wie in AmiHomeassist                 */
/* ------------------------------------------------------------------ */

static struct MsgPort     *g_tport = NULL;
static struct timerequest *g_treq  = NULL;
static ULONG               g_tsig  = 0;
static BOOL                g_twait = FALSE;

static BOOL timer_open(void)
{
    g_tport = CreateMsgPort();
    if (!g_tport) {
        return FALSE;
    }
    g_treq = (struct timerequest *)
                 CreateIORequest(g_tport, sizeof(struct timerequest));
    if (!g_treq) {
        DeleteMsgPort(g_tport);
        g_tport = NULL;
        return FALSE;
    }
    if (OpenDevice((STRPTR)TIMERNAME, UNIT_VBLANK,
                   (struct IORequest *)g_treq, 0) != 0) {
        DeleteIORequest((struct IORequest *)g_treq);
        DeleteMsgPort(g_tport);
        g_treq = NULL;
        g_tport = NULL;
        return FALSE;
    }
    g_tsig = 1UL << g_tport->mp_SigBit;
    return TRUE;
}

static void timer_start(int secs)
{
    if (!g_treq || g_twait) {
        return;
    }
    g_treq->tr_node.io_Command = TR_ADDREQUEST;
    g_treq->tr_time.tv_secs    = secs;
    g_treq->tr_time.tv_micro   = 0;
    SendIO((struct IORequest *)g_treq);
    g_twait = TRUE;
}

static void timer_stop(void)
{
    if (g_treq && g_twait) {
        AbortIO((struct IORequest *)g_treq);
        WaitIO((struct IORequest *)g_treq);
        g_twait = FALSE;
    }
}

static void timer_close(void)
{
    timer_stop();
    if (g_treq) {
        CloseDevice((struct IORequest *)g_treq);
        DeleteIORequest((struct IORequest *)g_treq);
        g_treq = NULL;
    }
    if (g_tport) {
        DeleteMsgPort(g_tport);
        g_tport = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Steckdosen                                                          */
/* ------------------------------------------------------------------ */

static const char *state_text(int state)
{
    return state == 1 ? "\33ban" : state == 0 ? "aus" : "?";
}

static const char *action_text(int state)
{
    return state == 1 ? "Ausschalten" : "Einschalten";
}

static void show_power(struct PlugUI *p)
{
    if (p->power_mw < 0) {
        strcpy(p->power_str, "\33r-");
    } else {
        sprintf(p->power_str, "\33r%ld,%02ld W", p->power_mw / 1000,
                (p->power_mw % 1000) / 10);
    }
    say(p->txt_power, p->power_str);
}

static void show_plug(struct PlugUI *p)
{
    say(p->txt_state, state_text(p->state));
    say(p->btn, action_text(p->state));
    show_power(p);
}

/* Gleiche Steckdosen in gleicher Reihenfolge? Dann genuegt es, die Werte
 * umzuschreiben - kein Neuaufbau, kein Flackern. */
static BOOL same_plugs(const struct FritzSwitch *sw, int n)
{
    int i;

    if (n != g_nplugs) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        if (strcmp(sw[i].ain, g_plugs[i].ain) != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Baut die Tabelle neu: Name | Zustand | Leistung | Knopf. */
static void plugs_build(const struct FritzSwitch *sw, int n)
{
    Object *grp;
    int i;

    dbg_log("plugs_build: start n=%d\n", n);
    if (n <= 0 || !sw) {
        grp = text("\33cKeine Steckdosen gefunden.");
    } else {
        grp = MUI_NewObject(MUIC_Group,
            MUIA_Group_Columns, 4,
            TAG_DONE);
    }
    if (!grp) {
        dbg_log("plugs_build: grp is NULL!\n");
        return;
    }

    fb_free(g_plugs);
    g_plugs = n > 0 ? fb_calloc(n, sizeof(*g_plugs)) : NULL;
    g_nplugs = g_plugs ? n : 0;

    for (i = 0; i < g_nplugs; i++) {
        struct PlugUI *p = &g_plugs[i];

        strcpy(p->ain, sw[i].ain);
        strncpy(p->name, sw[i].name[0] ? sw[i].name : sw[i].ain, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';
        p->state = sw[i].state;
        p->power_mw = sw[i].power_mw;

        /* Ohne Namen die AIN - die steht auch auf dem Geraet. */
        DoMethod(grp, OM_ADDMEMBER, text(p->name));
        DoMethod(grp, OM_ADDMEMBER, p->txt_state = text(""));
        DoMethod(grp, OM_ADDMEMBER, p->txt_power = text(""));
        DoMethod(grp, OM_ADDMEMBER, p->btn = button(""));
        set(p->btn, MUIA_CycleChain, TRUE);
        DoMethod(p->btn, MUIM_Notify, MUIA_Pressed, FALSE,
                 app, 2, MUIM_Application_ReturnID, ID_PLUG + i);
        show_plug(p);
    }

    dbg_log("plugs_build: InitChange\n");
    DoMethod(grp_plugs, MUIM_Group_InitChange);
    if (obj_plugs) {
        dbg_log("plugs_build: RemMember obj_plugs=%p\n", obj_plugs);
        DoMethod(grp_plugs, OM_REMMEMBER, obj_plugs);
        dbg_log("plugs_build: DisposeObject obj_plugs\n");
        MUI_DisposeObject(obj_plugs);
    }
    dbg_log("plugs_build: AddMember grp\n");
    DoMethod(grp_plugs, OM_ADDMEMBER, grp);
    obj_plugs = grp;
    dbg_log("plugs_build: ExitChange\n");
    DoMethod(grp_plugs, MUIM_Group_ExitChange);
    dbg_log("plugs_build: finished\n");
}

static void plugs_update(const struct FritzSwitch *sw, int n)
{
    int i;

    if (!same_plugs(sw, n) || !obj_plugs) {
        plugs_build(sw, n);
        return;
    }
    for (i = 0; i < n; i++) {
        g_plugs[i].state = sw[i].state;
        g_plugs[i].power_mw = sw[i].power_mw;
        show_plug(&g_plugs[i]);
    }
}

/* ------------------------------------------------------------------ */
/* Auftraege                                                           */
/* ------------------------------------------------------------------ */

static void send_job(int type)
{
    job_result_free(&g_job.d);
    g_job.d.type = type;
    set_busy(TRUE);
    worker_send(&g_job);
}

/* Gespeichert wird erst, wenn die Anmeldung geklappt hat - sonst landet
 * ein vertipptes Passwort in ENVARC:. Bis dahin liegt es hier, denn der
 * Netzwerkprozess loescht seine Kopie im Auftrag. */
static char g_save_pass[128];

static void connect_with(const char *host, const char *user, const char *pass)
{
    job_result_free(&g_job.d);          /* vor dem memset, sonst Leck */
    memset(&g_job.d, 0, sizeof(g_job.d));
    strncpy(g_job.d.host, host[0] ? host : "fritz.box",
            sizeof(g_job.d.host) - 1);
    strncpy(g_job.d.user, user, sizeof(g_job.d.user) - 1);
    strncpy(g_job.d.pass, pass, sizeof(g_job.d.pass) - 1);
    strncpy(g_save_pass, pass, sizeof(g_save_pass) - 1);

    say(txt_login, "Verbinde ...");
    send_job(JOB_CONNECT);
}

static void start_connect(void)
{
    char host[FB_HOST_LEN], user[FB_USER_LEN], pass[128];

    if (g_busy) {
        return;
    }
    string_get(str_host, host, sizeof(host));
    string_get(str_user, user, sizeof(user));
    string_get(str_pass, pass, sizeof(pass));
    connect_with(host, user, pass);
    memset(pass, 0, sizeof(pass));
}

static void start_refresh(void)
{
    if (g_busy || !g_connected) {
        return;
    }
    set_status("Lese Steckdosen ...");
    send_job(JOB_REFRESH);
}

static void start_switch(int n)
{
    if (g_busy || n < 0 || n >= g_nplugs) {
        return;
    }
    strcpy(g_job.d.ain, g_plugs[n].ain);
    g_job.d.on = g_plugs[n].state == 1 ? 0 : 1;
    set_status("Schalte ...");
    send_job(JOB_SWITCH);
}

static void show_login(const char *msg)
{
    g_connected = FALSE;
    timer_stop();
    set(win_login, MUIA_Window_Open, TRUE);
    set(win_main, MUIA_Window_Open, FALSE);
    strncpy(g_state.login_msg, msg, sizeof(g_state.login_msg) - 1);
    g_state.login_msg[sizeof(g_state.login_msg) - 1] = '\0';
    say(txt_login, g_state.login_msg);
    set(win_login, MUIA_Window_ActiveObject, str_pass);
}

static void show_info(const struct JobData *d)
{
    dbg_log("show_info: start tr_ok=%d\n", d->tr_ok);
    if (!d->tr_ok) {
        say(txt_model, "-");
        say(txt_fw, "-");
        say(txt_uptime, "-");
        say(txt_conn, "-");
        say(txt_ip, "-");
        say(txt_speed, "-");
        say(txt_wlan, "-");
        say(txt_hosts, "-");
        dbg_log("show_info: tr_ok=0 finished\n");
        return;
    }

    /* Modell & Firmware */
    strncpy(g_state.model, d->model, sizeof(g_state.model) - 1);
    g_state.model[sizeof(g_state.model) - 1] = '\0';
    say(txt_model, g_state.model);

    strncpy(g_state.fwver, d->fwver, sizeof(g_state.fwver) - 1);
    g_state.fwver[sizeof(g_state.fwver) - 1] = '\0';
    say(txt_fw, g_state.fwver);

    /* Laufzeit */
    if (d->uptime >= 0) {
        sprintf(g_state.uptime, "%ld Tage, %ld:%02ld h", d->uptime / 86400,
                (d->uptime / 3600) % 24, (d->uptime / 60) % 60);
        say(txt_uptime, g_state.uptime);
    } else {
        say(txt_uptime, "-");
    }

    /* Verbindung & Signal (5G / Mobilfunk oder DSL) */
    if (d->has_mobile && d->mobile_tech[0]) {
        if (d->mobile_signal[0]) {
            char sig[32] = "";
            const char *m = strstr(d->mobile_signal, "main=");
            if (m) {
                int val = atoi(m + 5);
                sprintf(sig, " (%d dBm)", val);
            }
            sprintf(g_state.conn, "%s%s", d->mobile_tech, sig);
        } else {
            strncpy(g_state.conn, d->mobile_tech, sizeof(g_state.conn) - 1);
            g_state.conn[sizeof(g_state.conn) - 1] = '\0';
        }
    } else if (d->conn_status[0]) {
        strncpy(g_state.conn, d->conn_status, sizeof(g_state.conn) - 1);
        g_state.conn[sizeof(g_state.conn) - 1] = '\0';
    } else {
        strcpy(g_state.conn, "Verbunden");
    }
    say(txt_conn, g_state.conn);

    /* Internet-IP */
    if (d->wanip[0]) {
        strncpy(g_state.ip, d->wanip, sizeof(g_state.ip) - 1);
        g_state.ip[sizeof(g_state.ip) - 1] = '\0';
    } else {
        strcpy(g_state.ip, "-");
    }
    say(txt_ip, g_state.ip);

    /* Geschwindigkeit */
    if (d->max_down_bps > 0 || d->max_up_bps > 0) {
        sprintf(g_state.speed, "%lu Mbit/s / %lu Mbit/s",
                d->max_down_bps / 1000000, d->max_up_bps / 1000000);
    } else {
        strcpy(g_state.speed, "-");
    }
    say(txt_speed, g_state.speed);

    /* WLAN */
    if (d->wlan24_active && d->wlan24_ssid[0]) {
        sprintf(g_state.wlan, "2.4 GHz: \"%s\" (Kanal %d)",
                d->wlan24_ssid, d->wlan24_channel);
    } else if (d->wlan24_active) {
        strcpy(g_state.wlan, "2.4 GHz aktiv");
    } else {
        strcpy(g_state.wlan, "WLAN aus");
    }
    say(txt_wlan, g_state.wlan);

    /* Heimnetz Geraete */
    if (d->host_count > 0) {
        if (d->hosts_active >= 0) {
            sprintf(g_state.hosts, "%d Geraete (%d online)",
                    d->host_count, d->hosts_active);
        } else {
            sprintf(g_state.hosts, "%d Geraete", d->host_count);
        }
    } else {
        strcpy(g_state.hosts, "-");
    }
    say(txt_hosts, g_state.hosts);

    dbg_log("show_info: finished\n");
}

static void job_finished(struct JobMsg *m)
{
    struct JobData *d = &m->d;

    set_busy(FALSE);

    switch (d->type) {
    case JOB_CONNECT:
        if (d->rc != FB_OK) {
            memset(g_save_pass, 0, sizeof(g_save_pass));
            show_login(d->error[0] ? d->error : "Verbindung fehlgeschlagen.");
            break;
        }
        prefs_save(d->host, d->user, g_save_pass);
        memset(g_save_pass, 0, sizeof(g_save_pass));
        dbg_log("JOB_CONNECT: rc=%d tr_ok=%d swcount=%d hostcount=%d\n",
                d->rc, d->tr_ok, d->swcount, d->host_count);
        g_connected = TRUE;

        /* Erst alle Daten und dynamischen Gruppen fuellen, waehrend das
         * Fenster noch geschlossen ist. Unter MUI 3.8 loest InitChange/ExitChange
         * auf unsichtbaren Seiten eines offenen Fensters Layout-Fehler aus. */
        dbg_log("Showing info...\n");
        show_info(d);

        dbg_log("Building plugs...\n");
        plugs_build(d->sw, d->swcount);

        if (d->error[0]) {
            sprintf(g_state.status, "Smart Home: %.200s", d->error);
        } else if (!d->tr_ok) {
            sprintf(g_state.status, "Angemeldet. TR-064: %.200s", d->tr_error);
        } else {
            sprintf(g_state.status, "Angemeldet als %.60s", d->user_used);
        }
        say(txt_status, g_state.status);

        /* Login-Fenster schliessen, Hauptfenster oeffnen */
        set(win_login, MUIA_Window_Open, FALSE);
        dbg_log("Opening win_main...\n");
        set(win_main, MUIA_Window_Open, TRUE);
        {
            ULONG is_open = FALSE;
            get(win_main, MUIA_Window_Open, &is_open);
            dbg_log("win_main is_open = %lu\n", is_open);
            if (!is_open) {
                g_connected = FALSE;
                set(win_login, MUIA_Window_Open, TRUE);
                say(txt_login, "Hauptfenster konnte nicht geoeffnet werden.");
                break;
            }
        }
        dbg_log("Connect sequence finished successfully.\n");
        timer_start(REFRESH_SECS);
        break;

    case JOB_REFRESH:
        if (d->rc == FB_OK) {
            plugs_update(d->sw, d->swcount);
            set_status("");
        } else if (d->rc == FB_EAUTH) {
            show_login(d->error);   /* Neuanmeldung ist auch gescheitert */
        } else {
            set_status(d->error);
        }
        break;

    case JOB_SWITCH: {
        int i;

        if (d->rc != FB_OK) {
            set_status(d->error);
            if (d->rc == FB_EAUTH) {
                show_login(d->error);
            }
            break;
        }
        for (i = 0; i < g_nplugs; i++) {
            if (strcmp(g_plugs[i].ain, d->ain) == 0) {
                g_plugs[i].state = d->on;
                show_plug(&g_plugs[i]);
            }
        }
        set_status("");
        break;
    }

    case JOB_LOGOUT:
        show_login("Abgemeldet.");
        break;
    }
}

/* ------------------------------------------------------------------ */

static void cleanup(void)
{
    timer_close();
    if (app) {
        MUI_DisposeObject(app);
    }
    fb_free(g_plugs);
    if (MUIMasterBase) {
        CloseLibrary(MUIMasterBase);
    }
    if (IntuitionBase) {
        CloseLibrary((struct Library *)IntuitionBase);
    }
}

static int real_main(void)
{
    char host[FB_HOST_LEN], user[FB_USER_LEN], pass[128];
    ULONG sigs = 0;
    ULONG id;
    BOOL running = TRUE;

    IntuitionBase = (struct IntuitionBase *)
                        OpenLibrary((STRPTR)"intuition.library", 37);
    if (!IntuitionBase) {
        printf("AmiFritzi braucht AmigaOS 2.04 oder neuer.\n");
        return 20;
    }
    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, MUIMASTER_VMIN);
    if (!MUIMasterBase) {
        printf("AmiFritzi braucht MUI 3.8 oder neuer.\n");
        cleanup();
        return 20;
    }

    pass[0] = '\0';
    prefs_load(host, sizeof(host), user, sizeof(user), pass, sizeof(pass));

    app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title,       (ULONG)"AmiFritzi",
        MUIA_Application_Version,     (ULONG)VERSTAG,
        MUIA_Application_Copyright,   (ULONG)"2026",
        MUIA_Application_Author,      (ULONG)"Radi",
        MUIA_Application_Description, (ULONG)"FRITZ!Box am Amiga",
        MUIA_Application_Base,        (ULONG)"AMIFRITZI",
        MUIA_Application_Menustrip,   (ULONG)MUI_MakeObject(MUIO_MenustripNM, (ULONG)g_menu, 0),

        MUIA_Application_Window, win_login = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title, (ULONG)"AmiFritzi - Einstellungen & Anmeldung",
            MUIA_Window_ID,    MAKE_ID('A','F','R','1'),
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Columns, 2,
                    MUIA_Group_Child, label("FRITZ!Box (IP/Host)"),
                    MUIA_Group_Child, str_host = string(FB_HOST_LEN - 1, FALSE),
                    MUIA_Group_Child, label("Benutzer"),
                    MUIA_Group_Child, str_user = string(FB_USER_LEN - 1, FALSE),
                    MUIA_Group_Child, label("Passwort"),
                    MUIA_Group_Child, str_pass = string(127, TRUE),
                    TAG_DONE),
                MUIA_Group_Child, text("\33cIP (z.B. 192.168.178.1) oder Host (fritz.box)\nBenutzer leer lassen = Standard-Benutzer."),
                MUIA_Group_Child, txt_login = MUI_NewObject(MUIC_Text,
                    MUIA_Frame,         MUIV_Frame_Text,
                    MUIA_Text_Contents, (ULONG)"",
                    TAG_DONE),
                MUIA_Group_Child, bt_connect = button("Speichern & Verbinden"),
                TAG_DONE),
            TAG_DONE),

        MUIA_Application_Window, win_main = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title,  (ULONG)"AmiFritzi",
            MUIA_Window_ID,     MAKE_ID('A','F','R','2'),
            MUIA_Window_Width,  MUIV_Window_Width_Visible(55),
            MUIA_Window_Height, MUIV_Window_Height_Visible(70),
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,

                /* Blauer Header-Bereich mit AmiFritzi-Logo */
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,       MUIV_Frame_Group,
                    MUIA_Background,  (ULONG)bg_blue,
                    MUIA_VertWeight,  0,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_Group_Child, MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    MUIA_Group_Child, make_logo(),
                    MUIA_Group_Child, MUI_NewObject(MUIC_Rectangle, TAG_DONE),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,        MUIV_Frame_Group,
                    MUIA_FrameTitle,   (ULONG)"FRITZ!Box & Internet",
                    MUIA_Group_Columns, 2,
                    MUIA_VertWeight,   0,
                    MUIA_Group_Child, label("Modell"),
                    MUIA_Group_Child, txt_model = text("-"),
                    MUIA_Group_Child, label("FRITZ!OS"),
                    MUIA_Group_Child, txt_fw = text("-"),
                    MUIA_Group_Child, label("Verbindung"),
                    MUIA_Group_Child, txt_conn = text("-"),
                    MUIA_Group_Child, label("Internet-IP"),
                    MUIA_Group_Child, txt_ip = text("-"),
                    MUIA_Group_Child, label("Datenrate (Down/Up)"),
                    MUIA_Group_Child, txt_speed = text("-"),
                    MUIA_Group_Child, label("Laufzeit"),
                    MUIA_Group_Child, txt_uptime = text("-"),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,        MUIV_Frame_Group,
                    MUIA_FrameTitle,   (ULONG)"WLAN & Heimnetz",
                    MUIA_Group_Columns, 2,
                    MUIA_VertWeight,   0,
                    MUIA_Group_Child, label("Funknetz"),
                    MUIA_Group_Child, txt_wlan = text("-"),
                    MUIA_Group_Child, label("Netzwerkgeraete"),
                    MUIA_Group_Child, txt_hosts = text("-"),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,      MUIV_Frame_Group,
                    MUIA_FrameTitle, (ULONG)"Smart Home",
                    MUIA_Group_Child, MUI_NewObject(MUIC_Scrollgroup,
                        MUIA_Scrollgroup_FreeHoriz, FALSE,
                        MUIA_Scrollgroup_Contents,
                            grp_plugs = MUI_NewObject(MUIC_Virtgroup,
                                TAG_DONE),
                        TAG_DONE),
                    TAG_DONE),

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Horiz, TRUE,
                    MUIA_VertWeight,  0,
                    MUIA_Group_Child, bt_refresh = button("Aktualisieren"),
                    MUIA_Group_Child, bt_logout  = button("Abmelden"),
                    TAG_DONE),

                MUIA_Group_Child, txt_status = MUI_NewObject(MUIC_Text,
                    MUIA_Frame,         MUIV_Frame_Text,
                    MUIA_Text_Contents, (ULONG)"",
                    TAG_DONE),
                TAG_DONE),
            TAG_DONE),

        TAG_DONE);

    if (!app) {
        printf("AmiFritzi: MUI-Oberflaeche liess sich nicht anlegen.\n");
        cleanup();
        return 20;
    }

    /* Ein leerer Platzhalter, damit die Virtgroup nie ohne Kind ist. */
    obj_plugs = MUI_NewObject(MUIC_Rectangle, TAG_DONE);
    DoMethod(grp_plugs, OM_ADDMEMBER, obj_plugs);

    DoMethod(app, MUIM_Notify, MUIA_Application_MenuAction, MUIV_EveryTime,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_TriggerValue);

    DoMethod(win_login, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_CLOSE_LOGIN);
    DoMethod(win_main, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID, ID_QUIT);
    DoMethod(bt_connect, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_CONNECT);
    /* Return im Passwortfeld verbindet sofort. */
    DoMethod(str_pass, MUIM_Notify, MUIA_String_Acknowledge, MUIV_EveryTime,
             app, 2, MUIM_Application_ReturnID, ID_CONNECT);
    DoMethod(bt_refresh, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_REFRESH);
    DoMethod(bt_logout, MUIM_Notify, MUIA_Pressed, FALSE,
             app, 2, MUIM_Application_ReturnID, ID_LOGOUT);
    set(bt_connect, MUIA_CycleChain, TRUE);

    set(str_host, MUIA_String_Contents, (ULONG)host);
    set(str_user, MUIA_String_Contents, (ULONG)user);
    if (pass[0]) {
        set(str_pass, MUIA_String_Contents, (ULONG)pass);
    }

    if (!worker_start()) {
        printf("AmiFritzi: Netzwerkprozess liess sich nicht starten.\n");
        cleanup();
        return 20;
    }
    timer_open();               /* ohne Zeitgeber eben nur von Hand */

    if (pass[0]) {
        connect_with(host, user, pass); /* gespeichertes Passwort: gleich los */
        memset(pass, 0, sizeof(pass));
    } else {
        show_login("");
    }

    while (running && (id = DoMethod(app, MUIM_Application_NewInput, &sigs))
                != (ULONG)MUIV_Application_ReturnID_Quit) {

        if (id) {
            dbg_log("loop: id=%lu\n", id);
        }

        if (id >= ID_PLUG && id < (ULONG)(ID_PLUG + g_nplugs)) {
            start_switch((int)(id - ID_PLUG));
        } else {
            switch (id) {
            case ID_CONNECT: start_connect(); break;
            case ID_REFRESH: start_refresh(); break;
            case ID_LOGOUT:
                if (!g_busy) {
                    timer_stop();
                    send_job(JOB_LOGOUT);
                }
                break;
            case ID_ABOUT:
                MUI_Request(app, win_main, 0, (char *)"Ueber AmiFritzi", (char *)"_OK",
                            (char *)"\33cAmiFritzi " APP_VERSION " (" APP_DATE ")\n\n"
                            "FRITZ!Box am Amiga\n\n"
                            "TR-064 & Smart Home (AHA)\n"
                            "\xa9 2026 Radi\n\n"
                            "M68020+ / MUI 3.8");
                break;
            case ID_PREFS:
                say(txt_login, "");
                set(win_login, MUIA_Window_Open, TRUE);
                set(win_login, MUIA_Window_ActiveObject, str_host);
                break;
            case ID_CLOSE_LOGIN:
                if (g_connected) {
                    set(win_login, MUIA_Window_Open, FALSE);
                } else {
                    running = FALSE;
                }
                break;
            case ID_QUIT:
                running = FALSE;
                break;
            }
        }

        if (!running) {
            break;
        }

        if (sigs) {
            ULONG wsig = worker_signal();

            sigs = Wait(sigs | SIGBREAKF_CTRL_C | wsig | g_tsig);

            if (sigs & wsig) {
                struct JobMsg *m;

                while ((m = worker_done()) != NULL) {
                    job_finished(m);
                }
            }
            if (g_tsig && (sigs & g_tsig)) {
                while (GetMsg(g_tport)) {
                    ;
                }
                g_twait = FALSE;
                if (g_connected) {
                    start_refresh();
                    timer_start(REFRESH_SECS);
                }
            }
            if (sigs & SIGBREAKF_CTRL_C) {
                break;
            }
        }
    }

    /* Meldet an der Box ab und wartet, bis der Prozess weg ist. */
    set(win_main, MUIA_Window_Open, FALSE);
    set(win_login, MUIA_Window_Open, FALSE);
    worker_stop();
    job_result_free(&g_job.d);
    cleanup();
    return 0;
}

int main(void)
{
    return run_with_stack(real_main, 65536);
}
