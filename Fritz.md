# AmiFritz – Übergabe an den lokalen Agenten

## Worum es geht

AmiFritz ist eine MUI-Anwendung für klassisches AmigaOS (3.x, 68020+, ohne FPU).
Sie steuert eine AVM FRITZ!Box im LAN über die offiziellen Schnittstellen:

- **AHA-HTTP** (`login_sid.lua` und `/webservices/homeautoswitch.lua`, Port 80) für die Smart-Home-Steckdosen.
- **TR-064** (SOAP, Port 49000) für Geräteinfo und WAN-IP.

Den vollständigen Plan und die Protokolldetails enthält `PROJECT.md`. Bau, Tests
und Hinweise zum Code stehen in `README.md`. Der Aufbau folgt dem Schwesterprojekt
**AmiHomeassist** (`radi7777/AmiHomeassist`): gleiche Toolchain, gleiche
Makefile-Konventionen, `muistubs.c` von dort.

## 1. Code holen

Der Code liegt im privaten Repo **radi7777/AmiAtari2600** auf dem Branch
`claude/mui-fritzbox-amiga-w8snc7`.

```sh
git clone -b claude/mui-fritzbox-amiga-w8snc7 https://github.com/radi7777/AmiAtari2600.git AmiFritz
cd AmiFritz
```

Weil das Repo privat ist, braucht git eine Anmeldung: `gh auth login`, GitHub
Desktop oder einen Personal Access Token als Passwort.

## 2. In ein eigenes Repo AmiFritz umziehen

Zuerst auf github.com ein **leeres, privates** Repo `radi7777/AmiFritz` anlegen,
ohne README. Oder per CLI: `gh repo create radi7777/AmiFritz --private`.

```sh
git remote rename origin old
git remote add origin https://github.com/radi7777/AmiFritz.git
git push -u origin claude/mui-fritzbox-amiga-w8snc7:main
git branch -m main
git branch -u origin/main
git remote remove old
```

Damit landet die ganze Historie als `main` in AmiFritz.

## 3. Host-Tests (ohne Amiga, ohne Box)

```sh
make test
```

Das baut mit dem normalen `cc` und prüft:

- MD5 gegen RFC 1321, die Login-Rechnungen gegen die Beispiele von AVM und aus PROJECT.md.
- `AmiFritzCLI` gegen `tests/mockbox.py`, eine nachgebaute FRITZ!Box mit AHA und TR-064.
- Die Auftragslogik des Netzwerkprozesses (`jobs.c`), einschließlich stiller Neuanmeldung nach Ablauf der SID.

Auf Linux laufen alle Tests durch. **Auf macOS wurde es noch nie gebaut.** Falls
es dort hakt, liegt es wahrscheinlich am Unix-Zweig in `net.c` (Socket-Header,
`fcntl`, `select`).

Mit dem Host-Build lässt sich auch gegen die echte Box testen:

```sh
build/host/AmiFritzCLI get /
build/host/AmiFritzCLI -u <user> -p <pass> list
build/host/AmiFritzCLI -u <user> -p <pass> info
build/host/AmiFritzCLI -u <user> -p <pass> wanip
```

Für TR-064 muss an der Box eingeschaltet sein:
Heimnetz → Netzwerk → Netzwerkeinstellungen → „Zugriff für Anwendungen zulassen“.

## 4. Amiga-Build

Toolchain: m68k-amigaos-gcc (AmigaPorts), dieselbe wie bei AmiHomeassist.

1. Die MUI-Entwicklerheader nach `vendor/mui/include/` legen. Sie sind aus
   Lizenzgründen nicht im Repo; `libraries/mui.h`, `proto/muimaster.h`,
   `clib/muimaster_protos.h` und `inline/muimaster.h` stehen im
   AmiHomeassist-Setup.
2. Bauen:
   ```sh
   make TOOLCHAIN=~/opt/m68k-amigaos-gcc-16.2
   ```
   Das erzeugt `AmiFritz` (MUI) und `AmiFritzCLI`.

**Wichtig: Der Amiga-Build wurde noch NIE ausgeführt.** Die Cloud-Umgebung hatte
weder Toolchain noch NDK. `gui.c`, `worker.c`, `mem.c` und der Amiga-Zweig von
`net.c` sind nur gegen selbst geschriebene Ersatz-Header syntaxgeprüft. Beim
ersten Build ist mit Fehlern bei MUI-Namen, Includes oder Typen zu rechnen,
etwa bei `socklen_t`, `getsockopt` oder den `WaitSelect`-Casts. Die ersten
Schritte:

1. Kompilierfehler und Warnungen beheben.
2. `AmiFritzCLI` auf dem Amiga gegen die echte Box: `get /`, `login`, `list`, `info`.
3. Danach `AmiFritz` (MUI) starten.

## Aufbau des Codes

| Datei | Inhalt |
|-------|--------|
| `net.c/h` | TCP + HTTP/1.0 über bsdsocket.library, Zeitgrenzen per WaitSelect; läuft auch auf Unix |
| `md5.c/h` | MD5 (RFC 1321), endian-neutral |
| `fritz.c/h` | login_sid.lua (UTF-16LE-MD5), AHA-Steckdosen, XML-Helfer, Latin-1/UTF-8 |
| `tr064.c/h` | TR-064: SOAP-Envelope, AVM SOAP-Auth (InitChallenge/ClientAuth/NextChallenge), tr64desc.xml |
| `mem.c/h` | `AllocVec`-Allocator: libnix-`malloc` ist zwischen zwei Prozessen nicht sicher |
| `jobs.c/h` | Plattformneutrale Auftragslogik des Netzwerkprozesses (connect/refresh/switch/logout) |
| `worker.c/h` | Amiga: Netzwerkprozess, Aufträge als Exec-Messages |
| `gui.c` | Amiga: MUI-Oberfläche (Anmeldefenster, Hauptfenster mit Info und Steckdosen) |
| `cli.c` | `AmiFritzCLI` für die Shell |
| `muistubs.c` | Varargs-Stubs für muimaster (aus amimcp, Apache 2.0); muss eine eigene Übersetzungseinheit bleiben |
| `tests/` | `mockbox.py`, `test_crypto.c`, `test_cli.py`, `test_jobs.c/.py` |

## Konventionen (von AmiHomeassist übernommen)

- **Kein `-lamiga`** beim Linken: dessen `sprintf` hat ein 16-Bit-`%d`. Nur
  `DoMethod.o` wird einzeln aus `libamiga.a` geholt (siehe Makefile).
- `bsdsocket.library` wird pro Request geöffnet, weil die Basis pro Task gilt.
  Nur der Netzwerkprozess öffnet sie.
- Alles, was Netzwerkcode anfordert, geht über `fb_malloc`/`fb_free` (`mem.h`),
  nie über `malloc`/`free`.
- Die Oberfläche spricht nie selbst mit dem Netz, immer über `worker_send()`.
- Zeichensatz: Die Box liefert UTF-8, der Amiga nutzt ISO-8859-1. Gewandelt wird
  in `fritz.c` (`utf8_to_latin1`, `latin1_to_utf8`).
- Kommentare im Code auf Deutsch, ohne Umlaute (wie in AmiHomeassist).
- Vor jedem Commit `make test` laufen lassen.

## Offene Punkte

1. Erster Amiga-Build, Fehler beheben (siehe oben).
2. Test auf echter Hardware gegen eine echte Box, mit Roadshow oder AmiTCP.
3. `getdevicelistinfos` statt `getswitchlist` + 3 Einzelabfragen pro Steckdose,
   also ein Request statt 1 + 3n. Die Mock-Box muss das dann ebenfalls können.
4. Kataloge und Mehrsprachigkeit wie bei AmiHomeassist (`strings.cd`, `locale.py`).
5. Lizenz für AmiFritz festlegen (AmiHomeassist ist MIT).
6. Optional: `make check-fpu` wie bei AmiHomeassist.
