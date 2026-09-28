/* AmiFritz - der Netzwerkprozess.
 *
 * Ein HTTP-Aufruf kann bis zu 15 Sekunden blockieren (Zeitgrenzen in
 * net.c). Liefe er im Task der Oberflaeche, stuende so lange jedes Fenster.
 * Deshalb erledigt ein eigener Prozess alle Netzwerkarbeit:
 *
 *   Oberflaeche --PutMsg(JobMsg)--> Arbeitsprozess
 *               <--ReplyMsg---------  (Ergebnisse in derselben JobMsg)
 *
 * Die Oberflaeche nimmt das Signal von worker_signal() in ihr Wait() auf
 * und holt fertige Auftraege mit worker_done(). */

#ifndef FB_WORKER_H
#define FB_WORKER_H

#include <exec/types.h>
#include <exec/ports.h>

#include "jobs.h"

struct JobMsg {
    struct Message msg;
    struct JobData d;
};

/* Startet den Prozess. FALSE, wenn das nicht geht. */
BOOL worker_start(void);

/* Signalmaske des Antwort-Ports - fuer Wait() bzw. MUIs sigs. */
ULONG worker_signal(void);

/* Schickt einen Auftrag. Die JobMsg gehoert bis zur Antwort dem Prozess -
 * nicht anfassen, nicht freigeben. */
void worker_send(struct JobMsg *m);

/* Naechster erledigter Auftrag oder NULL. */
struct JobMsg *worker_done(void);

/* Beendet den Prozess und wartet darauf. Laeuft gerade ein Auftrag, kann
 * das bis zur Zeitgrenze des Netzes dauern. */
void worker_stop(void);

#endif
