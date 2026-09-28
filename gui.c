/* AmiFritz - MUI-Oberflaeche.
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

struct IntuitionBase *IntuitionBase = NULL;
struct Library *MUIMasterBase = NULL;

const char *VERSTAG = "$VER: AmiFritz 0.1 (28.09.2026)";

/* Steckdosen werden so oft neu gelesen, solange das Fenster offen ist. */
#define REFRESH_SECS 30

#define PREFS_ENV    "ENV:AmiFritz.prefs"
#define PREFS_ENVARC "ENVARC:AmiFritz.prefs"

enum {
    ID_CONNECT = 1, ID_REFRESH, ID_LOGOUT,
    ID_PLUG = 1000              /* + Nummer der Steckdose */
};

static Object *app;
static Object *win_login, *str_host, *str_user, *str_pass, *bt_connect;
static Object *txt_login;
static Object *win_main, *txt_model, *txt_fw, *txt_ip, *txt_uptime;
static Object *grp_plugs, *bt_refresh, *bt_logout, *txt_status;
static Object *obj_plugs = NULL;    /* aktueller Inhalt von grp_plugs */

/* Eine Steckdose, wie sie gerade angezeigt wird. */
struct PlugUI {
    char    ain[FB_AIN_LEN];
    int     state;
    long    power_mw;
    Object *txt_state;
    Object *txt_power;
    Object *btn;
};

static struct PlugUI *g_plugs = NULL;
static int            g_nplugs = 0;

static struct JobMsg g_job;         /* hoechstens ein Auftrag unterwegs */
static BOOL          g_busy = FALSE;
static BOOL          g_connected = FALSE;

/* ------------------------------------------------------------------ */
/* Kleinkram                                                           */
/* ------------------------------------------------------------------ */

static void say(Object *obj, const char *text)
{
    set(obj, MUIA_Text_Contents, (ULONG)text);
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
/* Einstellungen: nur Box und Benutzer. Das Passwort wird nie gespeichert. */
/* ------------------------------------------------------------------ */

static void prefs_load(char *host, int hsize, char *user, int usize)
{
    char line[160];
    FILE *f = fopen(PREFS_ENV, "r");

    strcpy(host, "fritz.box");
    user[0] = '\0';
    if (!f) {
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');

        if (nl) {
            *nl = '\0';
        }
        if (strncmp(line, "HOST=", 5) == 0 && line[5]) {
            strncpy(host, line + 5, hsize - 1);
            host[hsize - 1] = '\0';
        } else if (strncmp(line, "USER=", 5) == 0) {
            strncpy(user, line + 5, usize - 1);
            user[usize - 1] = '\0';
        }
    }
    fclose(f);
}

static void prefs_save(const char *host, const char *user)
{
    static const char *files[] = { PREFS_ENV, PREFS_ENVARC };
    int i;

    for (i = 0; i < 2; i++) {
        FILE *f = fopen(files[i], "w");

        if (f) {
            fprintf(f, "HOST=%s\nUSER=%s\n", host, user);
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

/* MUIs Text-Klasse kopiert den Inhalt - ein Puffer auf dem Stapel reicht. */
static void show_power(struct PlugUI *p)
{
    char buf[24];

    if (p->power_mw < 0) {
        strcpy(buf, "\33r-");
    } else {
        sprintf(buf, "\33r%ld,%02ld W", p->power_mw / 1000,
                (p->power_mw % 1000) / 10);
    }
    say(p->txt_power, buf);
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

    if (n == 0) {
        grp = text("\33cKeine Steckdosen gefunden.");
    } else {
        grp = MUI_NewObject(MUIC_Group,
            MUIA_Group_Columns, 4,
            TAG_DONE);
    }
    if (!grp) {
        return;
    }

    fb_free(g_plugs);
    g_plugs = n ? fb_calloc(n, sizeof(*g_plugs)) : NULL;
    g_nplugs = g_plugs ? n : 0;

    for (i = 0; i < g_nplugs; i++) {
        struct PlugUI *p = &g_plugs[i];

        strcpy(p->ain, sw[i].ain);
        p->state = sw[i].state;
        p->power_mw = sw[i].power_mw;

        /* Ohne Namen die AIN - die steht auch auf dem Geraet. */
        DoMethod(grp, OM_ADDMEMBER, text(sw[i].name[0] ? sw[i].name
                                                        : sw[i].ain));
        DoMethod(grp, OM_ADDMEMBER, p->txt_state = text(""));
        DoMethod(grp, OM_ADDMEMBER, p->txt_power = text(""));
        DoMethod(grp, OM_ADDMEMBER, p->btn = button(""));
        set(p->btn, MUIA_CycleChain, TRUE);
        DoMethod(p->btn, MUIM_Notify, MUIA_Pressed, FALSE,
                 app, 2, MUIM_Application_ReturnID, ID_PLUG + i);
        show_plug(p);
    }

    DoMethod(grp_plugs, MUIM_Group_InitChange);
    if (obj_plugs) {
        DoMethod(grp_plugs, OM_REMMEMBER, obj_plugs);
        MUI_DisposeObject(obj_plugs);
    }
    DoMethod(grp_plugs, OM_ADDMEMBER, grp);
    obj_plugs = grp;
    DoMethod(grp_plugs, MUIM_Group_ExitChange);
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

static void start_connect(void)
{
    char pass[128];

    if (g_busy) {
        return;
    }
    job_result_free(&g_job.d);          /* vor dem memset, sonst Leck */
    memset(&g_job.d, 0, sizeof(g_job.d));
    string_get(str_host, g_job.d.host, sizeof(g_job.d.host));
    string_get(str_user, g_job.d.user, sizeof(g_job.d.user));
    string_get(str_pass, pass, sizeof(pass));
    if (!g_job.d.host[0]) {
        strcpy(g_job.d.host, "fritz.box");
    }
    strcpy(g_job.d.pass, pass);
    memset(pass, 0, sizeof(pass));

    prefs_save(g_job.d.host, g_job.d.user);
    say(txt_login, "Verbinde ...");
    send_job(JOB_CONNECT);
}

static void start_refresh(void)
{
    if (g_busy || !g_connected) {
        return;
    }
    say(txt_status, "Lese Steckdosen ...");
    send_job(JOB_REFRESH);
}

static void start_switch(int n)
{
    if (g_busy || n < 0 || n >= g_nplugs) {
        return;
    }
    strcpy(g_job.d.ain, g_plugs[n].ain);
    g_job.d.on = g_plugs[n].state == 1 ? 0 : 1;
    say(txt_status, "Schalte ...");
    send_job(JOB_SWITCH);
}

static void show_login(const char *msg)
{
    g_connected = FALSE;
    timer_stop();
    set(win_main, MUIA_Window_Open, FALSE);
    set(win_login, MUIA_Window_Open, TRUE);
    say(txt_login, msg);
    set(win_login, MUIA_Window_ActiveObject, str_pass);
}

static void show_info(const struct JobData *d)
{
    char buf[64];

    if (!d->tr_ok) {
        say(txt_model, "-");
        say(txt_fw, "-");
        say(txt_ip, "-");
        say(txt_uptime, "-");
        return;
    }
    say(txt_model, d->model);
    say(txt_fw, d->fwver);
    say(txt_ip, d->wanip[0] ? d->wanip : "keine Verbindung");
    if (d->uptime >= 0) {
        sprintf(buf, "%ld Tage, %ld:%02ld h", d->uptime / 86400,
                (d->uptime / 3600) % 24, (d->uptime / 60) % 60);
        say(txt_uptime, buf);
    } else {
        say(txt_uptime, "-");
    }
}

static void job_finished(struct JobMsg *m)
{
    struct JobData *d = &m->d;
    char buf[240];

    set_busy(FALSE);

    switch (d->type) {
    case JOB_CONNECT:
        if (d->rc != FB_OK) {
            say(txt_login, d->error);
            break;
        }
        g_connected = TRUE;
        set(str_pass, MUIA_String_Contents, (ULONG)"");
        show_info(d);
        plugs_build(d->sw, d->swcount);
        set(win_login, MUIA_Window_Open, FALSE);
        set(win_main, MUIA_Window_Open, TRUE);

        if (d->error[0]) {
            sprintf(buf, "Smart Home: %.200s", d->error);
        } else if (!d->tr_ok) {
            sprintf(buf, "Angemeldet. TR-064: %.200s", d->tr_error);
        } else {
            sprintf(buf, "Angemeldet als %.60s", d->user_used);
        }
        say(txt_status, buf);
        timer_start(REFRESH_SECS);
        break;

    case JOB_REFRESH:
        if (d->rc == FB_OK) {
            plugs_update(d->sw, d->swcount);
            say(txt_status, "");
        } else if (d->rc == FB_EAUTH) {
            show_login(d->error);   /* Neuanmeldung ist auch gescheitert */
        } else {
            say(txt_status, d->error);
        }
        break;

    case JOB_SWITCH: {
        int i;

        if (d->rc != FB_OK) {
            say(txt_status, d->error);
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
        say(txt_status, "");
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

int main(void)
{
    char host[FB_HOST_LEN], user[FB_USER_LEN];
    ULONG sigs = 0;
    ULONG id;

    IntuitionBase = (struct IntuitionBase *)
                        OpenLibrary((STRPTR)"intuition.library", 37);
    if (!IntuitionBase) {
        printf("AmiFritz braucht AmigaOS 2.04 oder neuer.\n");
        return 20;
    }
    MUIMasterBase = OpenLibrary((STRPTR)MUIMASTER_NAME, MUIMASTER_VMIN);
    if (!MUIMasterBase) {
        printf("AmiFritz braucht MUI 3.8 oder neuer.\n");
        cleanup();
        return 20;
    }

    prefs_load(host, sizeof(host), user, sizeof(user));

    app = MUI_NewObject(MUIC_Application,
        MUIA_Application_Title,       (ULONG)"AmiFritz",
        MUIA_Application_Version,     (ULONG)VERSTAG,
        MUIA_Application_Copyright,   (ULONG)"2026",
        MUIA_Application_Author,      (ULONG)"Radi",
        MUIA_Application_Description, (ULONG)"FRITZ!Box am Amiga",
        MUIA_Application_Base,        (ULONG)"AMIFRITZ",

        MUIA_Application_Window, win_login = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title, (ULONG)"AmiFritz - Anmeldung",
            MUIA_Window_ID,    MAKE_ID('A','F','R','1'),
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,
                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Group_Columns, 2,
                    MUIA_Group_Child, label("FRITZ!Box"),
                    MUIA_Group_Child, str_host = string(FB_HOST_LEN - 1, FALSE),
                    MUIA_Group_Child, label("Benutzer"),
                    MUIA_Group_Child, str_user = string(FB_USER_LEN - 1, FALSE),
                    MUIA_Group_Child, label("Passwort"),
                    MUIA_Group_Child, str_pass = string(127, TRUE),
                    TAG_DONE),
                MUIA_Group_Child, text("\33cBenutzer leer lassen = zuletzt "
                                       "an der Box angemeldeter."),
                MUIA_Group_Child, txt_login = MUI_NewObject(MUIC_Text,
                    MUIA_Frame,         MUIV_Frame_Text,
                    MUIA_Text_Contents, (ULONG)"",
                    TAG_DONE),
                MUIA_Group_Child, bt_connect = button("Verbinden"),
                TAG_DONE),
            TAG_DONE),

        MUIA_Application_Window, win_main = MUI_NewObject(MUIC_Window,
            MUIA_Window_Title,  (ULONG)"AmiFritz",
            MUIA_Window_ID,     MAKE_ID('A','F','R','2'),
            MUIA_Window_Width,  MUIV_Window_Width_Visible(35),
            MUIA_Window_Height, MUIV_Window_Height_Visible(45),
            MUIA_Window_RootObject, MUI_NewObject(MUIC_Group,

                MUIA_Group_Child, MUI_NewObject(MUIC_Group,
                    MUIA_Frame,        MUIV_Frame_Group,
                    MUIA_FrameTitle,   (ULONG)"FRITZ!Box",
                    MUIA_Group_Columns, 2,
                    MUIA_VertWeight,   0,
                    MUIA_Group_Child, label("Modell"),
                    MUIA_Group_Child, txt_model = text("-"),
                    MUIA_Group_Child, label("FRITZ!OS"),
                    MUIA_Group_Child, txt_fw = text("-"),
                    MUIA_Group_Child, label("Internet"),
                    MUIA_Group_Child, txt_ip = text("-"),
                    MUIA_Group_Child, label("Laufzeit"),
                    MUIA_Group_Child, txt_uptime = text("-"),
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
        printf("AmiFritz: MUI-Oberflaeche liess sich nicht anlegen.\n");
        cleanup();
        return 20;
    }

    /* Ein leerer Platzhalter, damit die Virtgroup nie ohne Kind ist. */
    obj_plugs = MUI_NewObject(MUIC_Rectangle, TAG_DONE);
    DoMethod(grp_plugs, OM_ADDMEMBER, obj_plugs);

    DoMethod(win_login, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(win_main, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
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

    if (!worker_start()) {
        printf("AmiFritz: Netzwerkprozess liess sich nicht starten.\n");
        cleanup();
        return 20;
    }
    timer_open();               /* ohne Zeitgeber eben nur von Hand */

    show_login("");

    while ((id = DoMethod(app, MUIM_Application_NewInput, &sigs))
                != (ULONG)MUIV_Application_ReturnID_Quit) {

        if (id >= ID_PLUG) {
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
            }
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
