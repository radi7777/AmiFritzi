/* AmiFritzi - eigener Stapel, per StackSwap() innerhalb von main().
 *
 * Die Workbench gibt einem Programm ohne Stack-Eintrag im Icon nur 4 KB,
 * zu wenig fuer MUI (Guru 8000 0003 auf dem TF536, siehe Changelog 0.8).
 *
 * 0.8 liess libnix umschalten (__stack plus swapstack.o). Das schaltet vor
 * main() um und erst im Exit-Code zurueck - und von der Workbench gestartet
 * fror der Amiga beim Beenden hart ein, reproduzierbar, ohne Guru. Ohne
 * die Umschaltung nicht. Deshalb jetzt so: umgeschaltet wird erst in
 * main(), zurueck noch in main(). Alles, was libnix danach tut - Exit-
 * Liste, Antwort an die Workbench -, laeuft auf dem Originalstapel, als
 * haette es nie einen anderen gegeben.
 *
 * Zwischen den beiden StackSwap()-Aufrufen darf der Compiler nichts vom
 * Stapel lesen, was er davor dort abgelegt hat - der gehoert dann ja nicht
 * mehr zum aktuellen Stapel. Deshalb stehen alle Werte, die ueber den
 * Wechsel hinweg gebraucht werden, in statischen Variablen, und die
 * Funktion ist noinline, damit sie nicht in main() aufgeht.
 */

#include <exec/types.h>
#include <exec/tasks.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "stack.h"

static struct StackSwapStruct g_sss;
static APTR  g_mem;
static int   g_rc;
static int (*g_fn)(void);

__attribute__((noinline))
int run_with_stack(int (*fn)(void), ULONG need)
{
    struct Task *me = FindTask(NULL);
    ULONG have = (ULONG)me->tc_SPUpper - (ULONG)me->tc_SPLower;

    if (have >= need) {
        return fn();
    }
    g_mem = AllocVec(need, MEMF_ANY);
    if (!g_mem) {
        return fn();                /* lieber knapp als gar nicht */
    }
    g_fn = fn;
    g_sss.stk_Lower   = g_mem;
    g_sss.stk_Upper   = (ULONG)g_mem + need;
    g_sss.stk_Pointer = (APTR)g_sss.stk_Upper;

    StackSwap(&g_sss);
    g_rc = g_fn();
    StackSwap(&g_sss);

    FreeVec(g_mem);
    g_mem = NULL;
    return g_rc;
}
