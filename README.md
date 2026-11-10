# LiteCap

Ressourcenschonendes Screen-Capture-Tool für Windows 10/11 – eine schlanke Alternative zu
NVIDIA ShadowPlay: Instant Replay, Hardware-Encoding, direkter YouTube-Upload.

- **Aufnahme auf der GPU:** Windows.Graphics.Capture → Farbkonvertierung im Video Processor →
  NVENC / AMF / Quick Sync. Das Bild verlässt den Grafikspeicher nicht (x264 nur als Notlösung).
- **H.264, HEVC, AV1** – AV1 auch auf GPUs ohne AV1-Encoder (z. B. RTX 20/30): Aufnahme in HEVC,
  danach automatische Umwandlung im Leerlauf.
- **Ton:** System (WASAPI-Loopback) und Mikrofon, getrennte oder gemischte Spuren, synchron zum Bild.
- **Instant Replay** 30 s – 10 min; Puffer auf der Festplatte, daher kaum RAM.
- **Ausgabe:** lokal (MP4/MKV), YouTube (resumable, Warteschlange übersteht Neustarts) oder beides.
- **Tray-App** ohne GUI-Framework; die Einstellungs-GUI ist ein eigener Prozess, der nur läuft,
  solange das Fenster offen ist.

Gemessen neben einem GPU-limitierten Spiel: **0,18 % CPU, 90 MB Working Set** im Replay-Betrieb,
60 fps ohne ausgelassene Frames → [docs/PERFORMANCE.md](docs/PERFORMANCE.md)

Architektur: [docs/ARCHITEKTUR.md](docs/ARCHITEKTUR.md) · YouTube einrichten: [docs/YOUTUBE.md](docs/YOUTUBE.md)

## Download und Installation

1. **[LiteCap-win-x64.zip](https://github.com/fabianpriv4t/litecap/releases/latest/download/LiteCap-win-x64.zip)**
   herunterladen (neueste Version; alle Versionen unter [Releases](https://github.com/fabianpriv4t/litecap/releases)).
2. Das ZIP komplett in einen beliebigen Ordner entpacken – nicht direkt aus dem ZIP starten.
3. `litecap.exe` starten → Icon im Infobereich der Taskleiste (ggf. über ^ einblenden).

Eine Installation ist nicht nötig: .NET und die Visual-C++-Laufzeit sind im ZIP enthalten.
Die Ordnerstruktur muss erhalten bleiben (die Einstellungs-GUI liegt im Unterordner `settings`).
Die Programmdateien sind nicht signiert; bei der SmartScreen-Warnung „Weitere Informationen“ →
„Trotzdem ausführen“ wählen. Aus demselben Grund können Virenscanner – vor allem die
Verhaltens-Heuristiken von Windows Defender – LiteCap fälschlich als Bedrohung einstufen: Bildschirm-
und Tonaufnahme, Upload und Autostart ähneln dem Verhalten von Spionage-Software. Der komplette
Quellcode ist hier einsehbar, die SHA256-Prüfsummen der Dateien stehen beim jeweiligen Release.

**Systemvoraussetzungen:** Windows 10 1903 oder neuer, 64 Bit (Windows 11 empfohlen: kein gelber
Aufnahmerahmen).

### Unterstützte Grafikkarten

LiteCap nimmt mit der Grafikkarte auf, an der der Monitor hängt, und nutzt deren Hardware-Encoder –
das Bild bleibt dabei auf der GPU, die CPU-Last liegt unter 1 %. Fehlt dieser Encoder (oder ist in
den Einstellungen einer gewählt, den der PC nicht hat), probiert LiteCap automatisch die anderen
Hardware-Encoder und erst ganz zum Schluss x264 auf der CPU. Laptops mit zwei Grafikchips
funktionieren genauso.

| Hersteller | Encoder | Voraussetzung |
|---|---|---|
| NVIDIA | NVENC | GeForce GTX 900 oder neuer, Treiber 531.61 oder neuer |
| AMD | AMF | Radeon-Grafikkarte oder Ryzen mit integrierter Grafik, aktueller Treiber |
| Intel | Quick Sync | Intel HD/UHD/Iris Xe, Core Ultra oder Arc, aktueller Treiber |
| ohne passende GPU | x264 (CPU) | läuft überall, belastet die CPU aber deutlich stärker |

AV1 direkt in Hardware können GeForce RTX 40/50, Radeon RX 7000/9000 sowie Intel Arc und Core Ultra.
Alle anderen GPUs nehmen in HEVC auf; die Umwandlung in AV1 passiert danach automatisch im Leerlauf.

## Starten und bedienen

`litecap.exe` starten → Icon im Infobereich (ggf. über ^ einblenden).

| Standard-Hotkey | Aktion |
|---|---|
| `Strg+Alt+F9` | Aufnahme starten/stoppen |
| `Strg+Alt+F10` | Replay-Clip speichern |
| `Strg+Alt+F11` | Replay-Puffer an/aus |

(`Alt+F9`/`Alt+F10` sind meist von der NVIDIA App belegt.)

- **Rechtsklick** aufs Icon → Menü · **Doppelklick** → Einstellungen
- Icon-Farben: grau = bereit, grün = Replay aktiv, rot = Aufnahme läuft
- Tooltip: Replay-Füllstand, Aufnahmedauer/-größe, Upload-Fortschritt
- „Mit Windows starten“ im Menü oder in den Einstellungen

## Dateien

| Was | Wo |
|---|---|
| Einstellungen | `%APPDATA%\LiteCap\config.json` (wird bei Änderung sofort neu geladen) |
| Aufnahmen/Clips | `Videos\LiteCap` (einstellbar) |
| Log | `%LOCALAPPDATA%\LiteCap\logs\litecap.log` |
| Replay-Ringdatei | `%LOCALAPPDATA%\LiteCap\replay\` (wird beim Beenden gelöscht) |
| Upload-Warteschlange | `%LOCALAPPDATA%\LiteCap\uploads.json` |
| YouTube-Token, Client-Secret | Windows Credential Manager (`LiteCap/YouTube/…`), nie im Klartext |

## Wichtige Einstellungen (`config.json`)

| Schlüssel | Werte | Bedeutung |
|---|---|---|
| `capture.source` | `monitor` / `window` | Monitor (`monitorIndex`, 0 = primär) oder Fenster (`windowTitle` und/oder `windowExe`) |
| `video.codec` | `h264` / `hevc` / `av1` | AV1 ohne Hardware-Encoder → HEVC live + Umwandlung |
| `video.encoder` | `auto` / `nvenc` / `amf` / `qsv` / `x264` | `auto` nimmt den Encoder der GPU, an der der Monitor hängt |
| `video.fps` | 30 / 60 / 120 | konstante Bildrate |
| `video.width`/`height` | 0 = wie Quelle | Skalierung erfolgt auf der GPU |
| `video.rateControl` | `vbr` / `cbr` / `cqp` | mit `bitrateKbps` bzw. `quality` (0–51, kleiner = besser) |
| `audio.system`/`microphone` | true/false | `separateTracks`: eigene Spuren oder gemischt |
| `replay.seconds` | 30–600 | Länge des Instant Replay |
| `output.mode` | `local` / `youtube` / `both` | `deleteAfterUpload` verschiebt die Datei nach dem Upload in den Papierkorb |
| `output.fileNamePattern` | z. B. `LiteCap_{type}_{date}_{time}` | Platzhalter `{date}` `{time}` `{source}` `{type}` |
| `youtube.*` | | `clientId`, `titleTemplate`, `description`, `tags`, `privacy`, `categoryId`, `maxUploadKbps` |

Ungültige Werte werden beim Laden korrigiert und im Log vermerkt.

## Kommandozeile

| Befehl | Zweck |
|---|---|
| `litecap.exe` | normaler Start (Tray) |
| `litecap.exe --test-capture 5` | Capture-Selbsttest, speichert ein Testbild |
| `litecap.exe --test-record 6 hevc [nvenc]` | Aufnahme-Selbsttest mit Dateiprüfung und CPU-Messung |
| `litecap.exe --test-replay 30 40` | Replay-Selbsttest (Puffer füllen, Clip speichern) |
| `litecap.exe --list-devices` | Monitore, Fenster, Mikrofone, Encoder als JSON (nutzt die GUI) |
| `litecap.exe --youtube-login` / `--youtube-logout` | YouTube verbinden/trennen |

## Selbst bauen

Nur nötig, wenn du den Quellcode ändern willst – zum Benutzen reicht das Release-ZIP.

**Voraussetzungen**

- [Git](https://git-scm.com/download/win)
- Visual Studio 2022 (Community reicht) mit Workload **„Desktopentwicklung mit C++“**
  (enthält CMake, Ninja, vcpkg)
- [.NET 9 SDK](https://dotnet.microsoft.com/download/dotnet/9.0) (für die Einstellungs-GUI)
- Internetzugang beim ersten Build: vcpkg lädt und baut FFmpeg (einmalig ca. 20–40 min)

**Bauen**

```powershell
git clone https://github.com/fabianpriv4t/litecap.git
cd litecap
.\build.cmd release
```

`build.cmd` sucht Visual Studio selbst, baut den Kern (`litecap.exe`) und die GUI
(`litecap-settings.exe`) nach `build\Release\`. Beide Dateien gehören in denselben Ordner.

**Manuell** (z. B. mit den reinen *Build Tools for Visual Studio 2022*, die `build.cmd` nicht findet):
in der „Developer PowerShell for VS 2022“

```powershell
$env:VCPKG_ROOT = "<Visual-Studio-Ordner>\VC\vcpkg"   # falls nicht gesetzt
cmake --preset x64
cmake --build --preset release
dotnet publish settings\LiteCapSettings.csproj -c Release -r win-x64 --self-contained false -p:PublishSingleFile=true -o build\Release
```

So gebaut braucht die Einstellungs-GUI die .NET 9 Desktop Runtime. Für das Release-ZIP wird sie
stattdessen mit eigenem .NET in den Unterordner `settings` gebaut – bewusst als normale Dateien
statt als selbstentpackende Einzeldatei, die Virenscanner gern als verdächtig einstufen:

```powershell
dotnet publish settings\LiteCapSettings.csproj -c Release -r win-x64 --self-contained true -p:DebugType=none -o <ZIP-Ordner>\settings
```

`msvcp140.dll`, `vcruntime140.dll` und `vcruntime140_1.dll` (aus `VC\Redist\MSVC\<Version>\x64\Microsoft.VC143.CRT`)
liegen neben `litecap.exe`. Release-Builds am besten aus einem Ordner außerhalb des Benutzerprofils
(z. B. `C:\litecap-build`) erstellen, sonst landen lokale Pfade samt Benutzername in der exe.

## Projektstruktur

```
src/app        Tray, Hotkeys, Zustandssteuerung        src/encode    FFmpeg-Encoder (NVENC/AMF/QSV/x264)
src/capture    WGC, D3D11, BGRA→NV12                    src/audio     WASAPI, AAC-Spuren
src/pipeline   Session, Aufnahme, Replay-Ringpuffer     src/output    Muxer, Dateinamen, AV1, Nachbearbeitung
src/upload     WinHTTP, OAuth/PKCE, Credential Manager, YouTube-Uploader
src/config     JSON-Konfiguration                       settings/     WPF-Einstellungs-GUI (.NET 9)
ports/         vcpkg-Overlay (AMD-AMF-Header, korrigierte Prüfsumme)
```

## Lizenzhinweis

Der Build bindet FFmpeg mit **x264** (GPL) statisch ein; das Ergebnis unterliegt damit der
GPL v2+. Für eine Weitergabe unter anderer Lizenz in `vcpkg.json` die Features `gpl` und `x264`
entfernen (dann fehlt nur der Software-Fallback). SVT-AV1 (BSD), AMF/NVENC/oneVPL-Header (MIT)
sind unkritisch.
