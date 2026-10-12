# AmiFritzi — Agent Guidelines & Best Practices

## Referenz-Dokumente
Für grundlegende Amiga-Entwicklung und getestete Code-Vorlagen nutzen wir die zentralen Dokumente:
* `/Users/radi/Documents/CLAUDE/amiga.md` (Regeln, Fallen, Konventionen, Toolchain)
* `/Users/radi/Documents/CLAUDE/amigacode.md` (Kopierfertige Code-Muster für Makefile, Stack, MUI, Sockets, Worker, etc.)

---

## Die Kernregeln für dieses Projekt

1. **Zielprofil:**
   * `m68k-amigaos-gcc` mit `-mcpu=68020 -Os -fomit-frame-pointer -noixemul`.
   * **Keine FPU**, keine Fließkomma-Operationen. Softfloat-Multilib.
   * Toolchain-Pfad: `~/opt/m68k-amigaos-gcc-16.2`

2. **Linker & Bibliotheken:**
   * **Niemals `-lamiga` linken** (dessen `sprintf` hat ein fehlerhaftes 16-Bit-`%d`).
   * Nur benötigte Objekte wie `DoMethod.o` / `DoSuperMethod.o` einzeln per `ar x` aus `libamiga.a` entpacken.

3. **MUI:**
   * `muistubs.c` **muss** eine eigene Übersetzungseinheit bleiben (bei `-Os` verwerfen Inline-Stubs sonst Taglisten).
   * Eigene Klassen / Hooks / Dispatcher erfordern Registerargumente (`a0`/`a2`/`a1` via `__asm __saveds`).

4. **Multi-Threading & Speicher:**
   * `libnix`-`malloc` ist **nicht threadsicher**. Für Daten zwischen Haupt- und Worker-Prozess ausschließlich `AllocVec` / `FreeVec` (bzw. `fb_malloc` / `fb_free` in `mem.c`) nutzen.
   * Eigener Stack via `StackSwap()` (`run_with_stack()`), nicht libnix `__stack`.
   * Großer Stack für Worker-Prozesse (`NP_StackSize` >= 32 KB).

5. **Netzwerk:**
   * Niemals blockierende Socket-Aufrufe im GUI-Task.
   * Netzwerk läuft isoliert im Worker-Prozess (`worker.c` / `jobs.c`).
   * Sockets mit Timeout / `WaitSelect()`.
   * `bsdsocket.library` Basis gilt pro Task/Prozess.

6. **Zeichensatz & Text:**
   * FRITZ!Box sendet UTF-8; AmigaOS 3.x erwartet ISO-8859-1 (Latin-1).
   * Konvertierung erfolgt über `utf8_to_latin1` / `latin1_to_utf8` in `fritz.c`.

7. **Testen & Qualitätssicherung:**
   * Vor Änderungen und vor jedem Commit: `make test` ausführen.
   * Keine Annahmen treffen, Verhalten über Logs / Testprogramme verifizieren.
   * Keine sensiblen Daten (Passwörter, Tokens, private IPs) in Git committen.

8. **Autonome Ausführung & Kommunikation:**
   * Befehle (Kompilieren, Tests, Git, Push, Hardware-Checks) werden vollständig autonom ausgeführt — keine Bestätigungsfragen für Standard-Entwicklungsaktionen.
   * Rückfragen an den Nutzer ausschließlich bei Design-Entscheidungen, neuen Features oder fachlichen Unklarheiten.

