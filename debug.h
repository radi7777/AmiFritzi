/* AmiFritzi - Diagnose-Log, nur mit "make DEBUG=1".
 *
 * Schreibt nach RAM:AmiFritzi.log. Laeuft ueber dos.library statt ueber
 * stdio von libnix: dessen fopen() holt Speicher mit dem nicht
 * threadsicheren malloc() und haengt die Datei in eine globale Liste -
 * aus Oberflaeche und Netzwerkprozess zugleich gaebe das Kleinholz.
 * Im normalen Build verschwinden alle Aufrufe samt Argumenten. */

#ifndef DEBUG_H
#define DEBUG_H

#if defined(DEBUG) && (defined(__amigaos__) || defined(AMIGA) || defined(__AMIGA__))
void dbg_log(const char *fmt, ...);
#else
#define dbg_log(...) ((void)0)
#endif

#endif
