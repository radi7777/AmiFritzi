#!/usr/bin/env python3
"""Prueft ein fertiges Binary darauf, dass keine FPU-Instruktion darin steckt.

Auf dem 68k faengt jeder 68881/2-Befehl mit 'f' an. Das allein als Kriterium
zu nehmen geht schief, sobald gcc eine Sprungtabelle in den Code legt: deren
Bytes zerlegt objdump als Befehle, und irgendwann sieht ein Eintrag aus wie
ein 'ftstp'. Genau das ist am 01.09.2026 passiert, als die Klima-Umschaltung
dazukam - zwei harmlose Tabellenbytes 'f222'.

Schlimmer noch: hinter einer Tabelle laeuft die Zerlegung um ein paar Bytes
versetzt weiter, bis sie sich zufaellig wieder faengt. Es reicht also nicht,
den Tabellenbereich zu ueberspringen.

Deshalb wird hier stueckweise zerlegt: erst die Tabellen finden, dann jeden
Codeabschnitt ZWISCHEN den Tabellen einzeln mit --start-address zerlegen. So
faengt jede Zerlegung auf einer echten Befehlsgrenze an.

Tabellen sind an ihrem Zugriff zu erkennen:

    movew %pc@(0x475a,a3:l:2),d0     Eintrag holen
    jmp   %pc@(0x475a,d0:w)          und dorthin springen

Sie beginnen bei der genannten Adresse, und ihre Laenge steht in ihrem ersten
Eintrag: der zeigt auf den ersten Fall, und der liegt unmittelbar hinter der
Tabelle.

    python3 checkfpu.py <objdump> <datei> ...
"""

import os
import re
import subprocess
import sys

LINE = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)")
TABLE = re.compile(r"jmp\s+%pc@\(0x([0-9a-f]+),")


def disasm(objdump, path, start=None, stop=None):
    """Zerlegt einen Bereich. Liefert [(adr, mnemonik, zeile), ...]."""
    cmd = [objdump, "-d"]
    if start is not None:
        cmd.append("--start-address=0x%x" % start)
    if stop is not None:
        cmd.append("--stop-address=0x%x" % stop)
    cmd.append(path)
    # encoding ausdruecklich: im Projektordner liegt eine eigene locale.py,
    # die Pythons Standardmodul verdeckt - ohne die Angabe stirbt subprocess
    # in locale.getencoding().
    out = subprocess.run(cmd, capture_output=True, encoding="utf-8",
                         errors="replace", check=True).stdout

    rows, raw = [], {}
    for line in out.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        adr = int(m.group(1), 16)
        rows.append((adr, m.group(3), line))
        for i, b in enumerate(bytes.fromhex(m.group(2).replace(" ", ""))):
            raw[adr + i] = b
    return rows, raw


def find_tables(rows, raw, known):
    """Sprungtabellen aus einem Abschnitt, als (anfang, ende)."""
    found = []
    for _, _, line in rows:
        m = TABLE.search(line)
        if not m:
            continue
        start = int(m.group(1), 16)
        if start in [t[0] for t in known] + [t[0] for t in found]:
            continue
        if start in raw and start + 1 in raw:
            length = (raw[start] << 8) | raw[start + 1]
            if 0 < length < 8192:
                found.append((start, start + length))
    return found


def merge(tables):
    """Ueberlappende Tabellen zu einer zusammenfassen. Seit 0.9.1 legt der
    Compiler fuer die Zeichenumwandlung eine zweite Tabelle an, die in die
    erste hineinreicht - ohne Zusammenfassen wollte check() einen Abschnitt
    von hinten nach vorn zerlegen, und objdump brach ab."""
    out = []
    for s, e in tables:
        if out and s <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], e))
        else:
            out.append((s, e))
    return out


def check(objdump, path):
    rows, raw = disasm(objdump, path)
    tables = []

    # Die erste Zerlegung findet die erste Tabelle sicher, die spaeteren
    # womoeglich nicht - hinter einer Tabelle ist sie ja verschoben. Also
    # abschnittsweise nachfassen, bis nichts Neues mehr dazukommt.
    for _ in range(64):
        neu = find_tables(rows, raw, tables)
        if not neu:
            break
        tables = merge(sorted(tables + neu))

        rows, raw = [], {}
        grenzen = [None] + [t[1] for t in tables]
        enden = [t[0] for t in tables] + [None]
        for start, stop in zip(grenzen, enden):
            if start is not None and stop is not None and start >= stop:
                continue            # zwei Tabellen stossen aneinander
            r, x = disasm(objdump, path, start, stop)
            rows += r
            raw.update(x)

    hits = [(a, l) for a, mn, l in rows if mn.startswith("f")]
    return hits, tables


def main(argv):
    objdump, files = argv[0], argv[1:]
    rc = 0
    for f in files:
        hits, tables = check(objdump, f)
        if hits:
            print("FEHLER: %s enthaelt %d FPU-Instruktionen:" % (f, len(hits)))
            for _, line in hits[:5]:
                print("   " + line)
            rc = 1
        else:
            print("OK: %s ist FPU-frei (%d Bytes, %d Sprungtabellen "
                  "uebersprungen)" % (f, os.path.getsize(f), len(tables)))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
