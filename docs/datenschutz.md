---
title: Datenschutzerklärung
---

# Datenschutzerklärung für LiteCap

Stand: 10. November 2026

LiteCap ist ein Open-Source-Programm für Windows, das vollständig auf deinem PC läuft. Diese
Erklärung beschreibt, welche Daten LiteCap verarbeitet, wo sie gespeichert werden und wann sie
deinen PC verlassen. Der Quellcode ist öffentlich einsehbar:
<https://github.com/fabianpriv4t/litecap>.

## Kurzfassung

- LiteCap sammelt keine Daten über dich und sendet nichts an den Entwickler. Es gibt keine
  Analyse-, Werbe-, Telemetrie- oder Update-Funktionen, die Daten übertragen.
- Aufnahmen, Einstellungen und Protokolle bleiben auf deinem PC.
- Nur wenn du den YouTube-Upload selbst einrichtest, verbindet sich LiteCap mit Google bzw.
  YouTube – direkt von deinem PC aus und über dein eigenes Google-Cloud-Projekt.

## Daten auf deinem PC

LiteCap nimmt Bildschirm und Ton (Systemton, optional Mikrofon) auf, solange eine Aufnahme läuft
oder der Replay-Puffer aktiv ist. Diese und weitere Daten werden ausschließlich lokal gespeichert:

| Daten | Speicherort |
|---|---|
| Aufnahmen und Clips | `Videos\LiteCap` (in den Einstellungen änderbar) |
| Einstellungen | `%APPDATA%\LiteCap\config.json` |
| Protokolle | `%LOCALAPPDATA%\LiteCap\logs` |
| Replay-Puffer | `%LOCALAPPDATA%\LiteCap\replay` (wird beim Beenden gelöscht) |
| Upload-Warteschlange | `%LOCALAPPDATA%\LiteCap\uploads.json` |
| YouTube-Zugangsdaten (Refresh-Token, Client-Secret) | Windows-Anmeldeinformationsverwaltung, verschlüsselt mit deinem Windows-Konto |

## Google- und YouTube-Daten

Diese Abschnitte betreffen dich nur, wenn du den YouTube-Upload einrichtest.

- **Berechtigung:** LiteCap fordert über Google OAuth 2.0 ausschließlich die Berechtigung
  `https://www.googleapis.com/auth/youtube.upload` an und nutzt sie nur, um deine Aufnahmen auf
  deinen YouTube-Kanal hochzuladen. LiteCap liest, ändert oder löscht keine Videos, Kanal- oder
  Kontodaten.
- **Übertragene Daten:** die Videodatei sowie die Angaben aus deinen Einstellungen (Titel,
  Beschreibung, Tags, Sichtbarkeit, Kategorie).
- **Verbindungen:** nur zu Google – `accounts.google.com` (Anmeldung im Browser),
  `oauth2.googleapis.com` (Anmeldung und Widerruf) und `www.googleapis.com` (Upload).
- **Anmeldung:** Du meldest dich in deinem Browser direkt bei Google an; LiteCap erhält dein
  Google-Passwort nie. Zugriffstoken liegen nur im Arbeitsspeicher (etwa eine Stunde gültig), das
  Refresh-Token verschlüsselt in der Windows-Anmeldeinformationsverwaltung.
- **Keine Weitergabe:** Die Daten werden weder an den Entwickler noch an Dritte übermittelt,
  verkauft oder für Werbung genutzt. Die Nutzung von Daten, die LiteCap über Google-APIs erhält,
  hält sich an die [Google API Services User Data Policy](https://developers.google.com/terms/api-services-user-data-policy),
  einschließlich der Anforderungen zur eingeschränkten Nutzung (Limited Use).

LiteCap verwendet die YouTube API Services. Mit dem Upload gelten die
[Nutzungsbedingungen von YouTube](https://www.youtube.com/t/terms) und die
[Datenschutzerklärung von Google](https://policies.google.com/privacy). Was Google mit
hochgeladenen Videos und Angaben macht, richtet sich nach diesen Bedingungen; darauf hat LiteCap
keinen Einfluss.

## Zugriff widerrufen und Daten löschen

- **YouTube-Zugriff widerrufen:** in LiteCap unter Einstellungen → YouTube → **Trennen** (widerruft
  das Token bei Google und löscht es lokal) oder jederzeit unter
  <https://myaccount.google.com/permissions>.
- **Lokale Daten löschen:** LiteCap beenden und die oben genannten Ordner sowie die Einträge
  `LiteCap/YouTube/…` in der Windows-Anmeldeinformationsverwaltung löschen. Zum Entfernen des
  Programms vorher „Mit Windows starten“ ausschalten und dann den Programmordner löschen.

## Kontakt

Fragen zum Datenschutz bitte über die
[Issues des GitHub-Projekts](https://github.com/fabianpriv4t/litecap/issues).

## Änderungen

Änderungen an dieser Erklärung werden auf dieser Seite veröffentlicht; maßgeblich ist der oben
angegebene Stand.
