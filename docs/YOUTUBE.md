# YouTube-Upload einrichten

LiteCap lädt über die **YouTube Data API v3** hoch. Dafür braucht es ein eigenes (kostenloses)
Google-Cloud-Projekt mit einer OAuth-Client-ID. Das dauert einmalig etwa 10 Minuten.

## 1. Google-Cloud-Projekt anlegen

1. <https://console.cloud.google.com/> öffnen und mit dem Google-Konto anmelden, dem der
   YouTube-Kanal gehört.
2. Oben links auf die Projektauswahl → **Neues Projekt** → Name z. B. `LiteCap` → **Erstellen**.
   Danach sicherstellen, dass das neue Projekt oben ausgewählt ist.

## 2. YouTube Data API v3 aktivieren

1. Menü **APIs und Dienste → Bibliothek**.
2. Nach **YouTube Data API v3** suchen → öffnen → **Aktivieren**.

## 3. OAuth-Zustimmungsbildschirm („Google Auth Platform“)

1. Menü **APIs und Dienste → OAuth-Zustimmungsbildschirm** (bzw. **Google Auth Platform**).
2. **Branding:**
   - App-Name `LiteCap`, Support-E-Mail = deine Adresse, Kontakt-E-Mail des Entwicklers eintragen.
   - **Startseite der Anwendung:** `https://fabianpriv4t.github.io/litecap/`
   - **Link zur Datenschutzerklärung:** `https://fabianpriv4t.github.io/litecap/datenschutz.html`
   - **Autorisierte Domains:** `fabianpriv4t.github.io`
   - **Logo leer lassen** – mit Logo verlangt Google eine Überprüfung der App.
   - Nutzungsbedingungen sind optional.

   Startseite und Datenschutzerklärung braucht Google für den Status „In Produktion“ (Schritt 5);
   beide Seiten gehören zu LiteCap und dürfen in jedem eigenen Projekt eingetragen werden.
3. **Zielgruppe:** Nutzertyp **Extern**.
4. **Datenzugriff → Bereiche hinzufügen:** `https://www.googleapis.com/auth/youtube.upload`
   (Anzeigename „Videos auf YouTube verwalten“ / „youtube.upload“).
5. Veröffentlichungsstatus – zwei Möglichkeiten:
   - **Testen** (Standard): unter **Testnutzer** dein Google-Konto hinzufügen.
     ⚠ Im Testmodus laufen Refresh-Tokens nach **7 Tagen** ab → dann in LiteCap neu verbinden.
   - **In Produktion** (empfohlen für Dauerbetrieb): unter **Zielgruppe** → „App veröffentlichen“
     (setzt die Branding-Angaben aus Schritt 2 voraus). Den Hinweis auf eine nötige Überprüfung
     einfach bestätigen: Ohne Google-Prüfung erscheint bei der Anmeldung ein Warnhinweis („Google
     hat diese App nicht überprüft“), die Anmeldung bleibt aber dauerhaft gültig. Für den
     Eigengebrauch ist das unproblematisch (Grenze: 100 Nutzer).
     Warst du vorher im Testmodus verbunden: in LiteCap einmal **Trennen** und neu verbinden,
     sonst läuft das alte Token trotzdem nach 7 Tagen ab.

## 4. OAuth-Client-ID erstellen

1. Menü **APIs und Dienste → Anmeldedaten** (bzw. **Google Auth Platform → Clients**).
2. **Anmeldedaten erstellen → OAuth-Client-ID**.
3. Anwendungstyp: **Desktop-App** (wichtig – nicht „Webanwendung“), Name z. B. `LiteCap Desktop`.
4. **Erstellen** → **Client-ID** und **Clientschlüssel (Secret)** kopieren
   (oder die JSON-Datei herunterladen und die Werte daraus nehmen).

> Bei Desktop-Apps ist das „Secret“ laut Google nicht wirklich geheim; LiteCap speichert es
> trotzdem verschlüsselt im Windows Credential Manager und nicht in der `config.json`.

## 5. In LiteCap verbinden

1. Tray-Icon → **Einstellungen…** → **YouTube**.
2. **OAuth-Client-ID** und **OAuth-Client-Secret** einfügen.
3. **Mit YouTube verbinden** → der Browser öffnet die Google-Anmeldung.
   - Konto (und ggf. Kanal) auswählen.
   - Bei „Google hat diese App nicht überprüft“: **Erweitert → Zu LiteCap wechseln (unsicher)**
     – es ist deine eigene App.
   - Zugriff erlauben. Die Seite meldet „LiteCap ist mit YouTube verbunden ✔“.
4. Unter **Ausgabe** den Modus **YouTube** oder **Beides** wählen und **Speichern**.

Ab jetzt landet jede Aufnahme bzw. jeder Clip nach dem Speichern in der Upload-Warteschlange
(sichtbar unter Einstellungen → YouTube und im Tooltip des Tray-Icons).

## Wichtige Einschränkungen von Google

| Thema | Auswirkung |
|---|---|
| **Ungeprüfte API-Projekte** | Videos, die über die API aus einem nicht auditierten Projekt (angelegt nach dem 28.07.2020) hochgeladen werden, sperrt YouTube auf **„Privat“** – auch wenn „Öffentlich“ eingestellt ist. Freischaltung nur über Googles [YouTube-API-Audit](https://support.google.com/youtube/contact/yt_api_form). Man kann die Sichtbarkeit danach im YouTube Studio von Hand ändern, sofern YouTube das zulässt. |
| **Kontingent** | Standard 10 000 Einheiten pro Tag; ein Upload kostet ca. 1 600 → **etwa 6 Uploads pro Tag**. Danach wartet LiteCap automatisch und versucht es stündlich erneut. |
| **Dateigröße** | max. 256 GB bzw. 12 Stunden pro Video. |

## Sicherheit

- **Refresh-Token** und **Client-Secret** liegen ausschließlich im Windows Credential Manager
  (Systemsteuerung → Anmeldeinformationsverwaltung → Windows-Anmeldeinformationen,
  Einträge `LiteCap/YouTube/…`), verschlüsselt mit deinem Windows-Konto.
- Access-Tokens existieren nur im Arbeitsspeicher (ca. 1 Stunde gültig).
- Die Anmeldung nutzt PKCE und eine Rückleitung an `127.0.0.1` (nur lokal erreichbar).
- Angefordert wird nur `youtube.upload` – LiteCap kann nichts löschen oder lesen.
- **Trennen** in den Einstellungen widerruft das Token bei Google und löscht es lokal.
  Alternativ: <https://myaccount.google.com/permissions>.

## Fehlerbehebung

| Meldung | Ursache / Lösung |
|---|---|
| `redirect_uri_mismatch` | Client-Typ ist nicht „Desktop-App“ → neuen Client vom Typ Desktop-App anlegen. |
| `access_denied` / „App wird getestet“ | Eigenes Konto nicht als **Testnutzer** eingetragen. |
| „Anmeldung abgelaufen oder widerrufen (invalid_grant)“ | Testmodus: 7-Tage-Grenze erreicht, oder Zugriff entzogen → neu verbinden. |
| „Tageskontingent erschöpft“ | Quota aufgebraucht; LiteCap versucht es automatisch später erneut. |
| `youtubeSignupRequired` | Das Google-Konto hat noch keinen YouTube-Kanal → auf youtube.com einen anlegen. |
| Upload hängt bei „Wartet“ | Log prüfen (`%LOCALAPPDATA%\LiteCap\logs\litecap.log`); Netzwerkfehler werden mit wachsendem Abstand (bis 30 min) wiederholt und beim letzten bestätigten Byte fortgesetzt. |
