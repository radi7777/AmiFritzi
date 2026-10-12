/* AmiFritzi - der Netzwerkprozess, siehe worker.h. */

#include <exec/types.h>
#include <exec/ports.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include <string.h>

#include "worker.h"
#include "net.h"

#define WORKER_STACK 65536

/* Beim Start meldet der Prozess seinen Port ueber diese Nachricht zurueck.
 * Der Port muss IM Prozess entstehen: sein Signalbit gehoert dem Task, der
 * ihn anlegt, und nur der kann darauf warten. */
struct StartMsg {
    struct Message msg;
    struct MsgPort *port;
};

static struct MsgPort *g_reply = NULL;  /* gehoert der Oberflaeche */
static struct MsgPort *g_port = NULL;   /* gehoert dem Arbeitsprozess */
static int g_pending = 0;               /* abgeschickt, noch nicht zurueck */

static void worker_main(void)
{
    struct Process *me = (struct Process *)FindTask(NULL);
    struct StartMsg *sm;
    struct MsgPort *port;
    struct JobState st;

    WaitPort(&me->pr_MsgPort);
    sm = (struct StartMsg *)GetMsg(&me->pr_MsgPort);

    port = CreateMsgPort();
    sm->port = port;
    if (!port) {
        /* Unter Forbid() antworten und enden: sonst koennte die Oberflaeche
         * das Programm entladen, waehrend dieser Code noch laeuft. Das
         * Ende des Prozesses hebt Forbid() wieder auf. */
        Forbid();
        ReplyMsg(&sm->msg);
        return;
    }
    ReplyMsg(&sm->msg);

    job_state_init(&st);

    for (;;) {
        struct JobMsg *m;

        WaitPort(port);
        while ((m = (struct JobMsg *)GetMsg(port)) != NULL) {
            job_run(&st, &m->d);
            if (m->d.type == JOB_QUIT) {
                /* JOB_QUIT ist immer der letzte Auftrag - danach schickt
                 * die Oberflaeche nichts mehr, der Port ist leer. */
                job_state_free(&st);
                net_cleanup();
                DeleteMsgPort(port);
                Forbid();
                ReplyMsg(&m->msg);
                return;
            }
            ReplyMsg(&m->msg);
        }
    }
}

BOOL worker_start(void)
{
    struct StartMsg sm;
    struct Process *proc;

    g_reply = CreateMsgPort();
    if (!g_reply) {
        return FALSE;
    }

    proc = CreateNewProcTags(NP_Entry,     (ULONG)worker_main,
                             NP_Name,      (ULONG)"AmiFritzi Netz",
                             NP_StackSize, WORKER_STACK,
                             TAG_DONE);
    if (!proc) {
        DeleteMsgPort(g_reply);
        g_reply = NULL;
        return FALSE;
    }

    memset(&sm, 0, sizeof(sm));
    sm.msg.mn_ReplyPort = g_reply;
    sm.msg.mn_Length = sizeof(sm);
    PutMsg(&proc->pr_MsgPort, &sm.msg);
    WaitPort(g_reply);
    GetMsg(g_reply);

    g_port = sm.port;
    if (!g_port) {
        DeleteMsgPort(g_reply);
        g_reply = NULL;
        return FALSE;
    }
    return TRUE;
}

ULONG worker_signal(void)
{
    return g_reply ? (1UL << g_reply->mp_SigBit) : 0;
}

void worker_send(struct JobMsg *m)
{
    m->msg.mn_ReplyPort = g_reply;
    m->msg.mn_Length = sizeof(*m);
    g_pending++;
    PutMsg(g_port, &m->msg);
}

struct JobMsg *worker_done(void)
{
    struct JobMsg *m = (struct JobMsg *)GetMsg(g_reply);

    if (m) {
        g_pending--;
    }
    return m;
}

void worker_stop(void)
{
    static struct JobMsg quit;
    struct JobMsg *m;
    BOOL done = FALSE;

    if (!g_port) {
        return;
    }

    memset(&quit, 0, sizeof(quit));
    quit.d.type = JOB_QUIT;
    worker_send(&quit);

    /* Auch Antworten auf fruehere Auftraege kommen hier noch an; deren
     * Ergebnislisten will niemand mehr haben. */
    while (!done) {
        WaitPort(g_reply);
        while ((m = worker_done()) != NULL) {
            if (m == &quit) {
                done = TRUE;
            } else {
                job_result_free(&m->d);
            }
        }
    }

    g_port = NULL;
    DeleteMsgPort(g_reply);
    g_reply = NULL;
}
