/* AmiFritzi - eigener Stapel. Siehe stack.c. */

#ifndef STACK_H
#define STACK_H

#include <exec/types.h>

/* Ruft fn() auf einem Stapel von mindestens 'need' Bytes auf und gibt
 * dessen Ergebnis zurueck. Reicht der vorhandene schon, wird nicht
 * umgeschaltet. */
int run_with_stack(int (*fn)(void), ULONG need);

#endif
