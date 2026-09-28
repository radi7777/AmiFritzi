# FRITZ!Box MUI Client für AmigaOS

## Ziel

Eine native MUI-Anwendung für klassisches AmigaOS (68k, C), die sich mit einer AVM FRITZ!Box im lokalen Netzwerk verbindet, einloggt und grundlegende Einstellungen/Status abrufen und ändern kann. Kein Klon der HTML-Weboberfläche — stattdessen Nutzung der offiziellen TR-064-Schnittstelle (SOAP/UPnP über HTTP) sowie optional der einfacheren AHA-HTTP-Schnittstelle für Smart-Home-Geräte.

## Zielplattform

- AmigaOS 3.x (68k), C-Entwicklung
- MUI 3.8+ für die GUI
- TCP/IP über `bsdsocket.library` (Roadshow oder AmiTCP)
- Kein HTTPS nötig — FRITZ!Box TR-064/AHA laufen im LAN über HTTP (Port 49000 für TR-064, Port 80 für AHA)

## Architektur-Entscheidung

Zwei getrennte Nutzungsmöglichkeiten der FRITZ!Box, beide über reines HTTP ohne TLS:

1. **AHA-HTTP-Interface** (`/webservices/homeautoswitch.lua`) — einfache GET-Requests, JSON/Plain-Text-Antworten, für Smart-Home-Steckdosen/Thermostate. Kein SOAP, kein XML-Parsing nötig. **→ Empfohlener Einstiegspunkt.**
2. **TR-064** (SOAP über HTTP, Port 49000) — vollstandardisierte AVM-Schnittstelle für WLAN, DSL-Status, Telefonie, Rufumleitung etc. Erfordert eigenen SOAP-Envelope-Builder und minimalen XML-Parser.

## Phase 1: Netzwerk-Grundgerüst (Priorität 1)

Ziel: Eine funktionierende TCP-Verbindung von Amiga zur FRITZ!Box aufbauen und einen rohen HTTP-Request senden/empfangen.

Aufgaben:
- `bsdsocket.library` öffnen, Socket erstellen, `connect()` zu `fritz.box:80` (AHA) bzw. `:49000` (TR-064)
- Minimaler HTTP/1.1-GET-Request von Hand zusammenbauen und über den Socket schreiben
- Antwort zeilenweise lesen, Header von Body trennen (Content-Length beachten)
- Testfall: `GET /` gegen die Box, HTTP-Statuszeile und Header ausgeben (auf Shell/Konsole, noch ohne GUI)

Referenz: AmigaOS Manual "Network resources" (TCP: device, Socket-Programmierung) — Standard-BSD-Socket-Muster.

## Phase 2: AHA-HTTP Login + einfache Aktion (Priorität 2)

Ziel: Erste echte Funktion — z. B. eine Steckdose/Aktor über die Box schalten.

Der AHA-Login läuft über `login_sid.lua` mit Challenge-Response (separat vom TR-064-Mechanismus, siehe Phase 3 für Details zum Hash-Verfahren — gleiches Grundprinzip, andere Endpoint):

1. `GET /login_sid.lua` → liefert XML mit `<Challenge>`-Wert
2. Antwort berechnen: `response = challenge + "-" + MD5(challenge + "-" + passwort, UTF-16LE-kodiert)`
3. `GET /login_sid.lua?username=<user>&response=<response>` → liefert `<SID>` bei Erfolg
4. SID bei jedem weiteren Aufruf mitschicken: `GET /webservices/homeautoswitch.lua?sid=<SID>&switchcmd=getswitchlist`

Wichtig: Bei AHA-Login wird das Passwort **UTF-16LE** kodiert vor dem MD5-Hash — anders als beim TR-064-Verfahren in Phase 3, wo Klartext-Strings gehasht werden. Nicht verwechseln.

Aufgaben:
- MD5-Implementierung in C (RFC 1321, ca. 150–200 Zeilen, keine externen Abhängigkeiten)
- XML-Mini-Parser nur für `<Challenge>` und `<SID>`-Tags (kein voller XML-Parser nötig, simples String-Scanning reicht)
- Testfall: Steckdose an/aus schalten via `switchcmd=setswitchon` / `setswitchoff`

## Phase 3: TR-064 SOAP-Auth (Priorität 3)

Ziel: Vollständiger TR-064-Login für erweiterte Funktionen (WLAN, DSL-Status, Anrufliste).

Ablauf (bereits verifiziert, siehe Testrechnung unten):

1. SOAP-Request ohne Auth-Header an `controlURL` des Zielservices senden
2. Server antwortet mit SOAP-Fault, liefert `realm` (immer `F!Box SOAP-Auth`) und Nonce `sn`
3. Zweistufiger MD5 (auf Hex-String-Ebene, nicht auf Rohbytes!):
   ```
   secret   = MD5(uid + ":" + realm + ":" + pwd)     -> Hex-String
   response = MD5(secret + ":" + sn)                  -> Hex-String
   ```
4. Zweiten SOAP-Request mit Auth-Header (uid, realm, sn, response) senden
5. Bei Erfolg: normales SOAP-Response-XML

Verifiziertes Testbeispiel (bereits durchgerechnet und korrekt):
```
uid    = admin
pwd    = gurkensalat
realm  = F!Box SOAP-Auth
sn     = F758BE72FB999CEA

secret   = 09b67a406f01ca26439fc9c2104aa5da
response = b4f67585f22b0af7c4615db5a18faa14
```

Fehlercodes zum Handling:
- 401 = nicht authentifiziert
- 606 = authentifiziert, aber keine Rechte
- 866/867/868 = Zwei-Faktor-Auth erforderlich
- 503 = Rate-Limit nach zu vielen Fehlversuchen

Aufgaben:
- SOAP-Envelope-Builder (String-Templating reicht, kein generischer XML-Writer nötig)
- Service-Discovery: `GET http://fritz.box:49000/tr64desc.xml` parsen, um verfügbare Services/Actions/controlURLs zu finden
- Minimaler XML-Parser für SOAP-Responses (Tag-basiertes String-Scanning, kein DOM nötig)
- Testfall: `WANIPConnection` Service abfragen (liefert aktuelle WAN-IP — einfacher Lesezugriff ohne Nebenwirkungen)

## Phase 4: MUI-GUI (parallel zu Phase 2/3 entwickelbar)

Ziel: Login-Fenster und erste Funktionsansicht.

- Login-Screen: Textfelder für Host/IP, Username, Passwort, "Verbinden"-Button
- Nach Login: einfache Listview/Gruppen-Ansicht für verfügbare Aktionen (Smart-Home-Geräte-Liste, WLAN-Status)
- **Wichtig:** Netzwerk-Calls müssen in eigenem Task/Prozess laufen, nicht im MUI-Hauptthread — MUI reagiert kooperativ auf Events, ein blockierender HTTP-Call friert sonst die komplette GUI ein, bis die Antwort da ist

## Offene technische Fragen für den Agenten

- Welche TCP/IP-Stack-Variante ist auf der Zielmaschine installiert (Roadshow vs. AmiTCP) — API ist weitgehend kompatibel, aber Include-Pfade können abweichen
- Firmware-Version der FRITZ!Box prüfen — ab FRITZ!OS 7.24 ist immer ein echter Benutzername nötig, nicht nur ein Passwort
- Muss lokal getestet werden (echte Box im LAN) oder gibt es eine Simulator-/Mock-Umgebung für Entwicklung ohne Hardware-Zugriff?

## Nicht-Ziele (bewusst ausgeklammert)

- Kein Fernzugriff über MyFRITZ!/DynDNS in Phase 1–4 — TR-064 und AHA sind nur für lokales Netzwerk spezifiziert
- Kein vollständiger XML/DOM-Parser — Tag-basiertes String-Scanning ist für den begrenzten Funktionsumfang ausreichend
- Keine HTTPS/TLS-Unterstützung nötig, da FRITZ!Box lokale Schnittstellen nur über HTTP laufen

## Nächster konkreter Schritt

Start mit Phase 1: Rohes TCP-Socket-Grundgerüst gegen `fritz.box:80` bauen und einen einfachen HTTP-GET-Request samt Response-Parsing zum Laufen bringen. Danach Phase 2 (AHA-Login + Steckdose schalten) als ersten End-to-End-Erfolg, bevor TR-064/SOAP (Phase 3) angegangen wird.
