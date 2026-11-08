# LiteCap – Architektur

Ressourcenschonendes Screen-Capture-Tool für Windows 10/11 (ShadowPlay-Alternative).
Zielwerte: < 3 % CPU, < 150 MB RAM im Idle-/Replay-Modus.

## 1. Sprach- und Technologiewahl

| Teil | Wahl | Begründung |
|---|---|---|
| Kern (`litecap.exe`) | **C++20**, Win32, C++/WinRT | Direkter Zugriff auf D3D11, Windows.Graphics.Capture, WASAPI und die FFmpeg-C-API ohne Interop-Schicht, ohne Garbage Collector und ohne Laufzeitumgebung. Basis-RAM eines Win32-Prozesses ≈ 5–10 MB (zum Vergleich: .NET ≈ 30–60 MB). Rust wäre ähnlich schnell, aber die Bindings für WinRT und FFmpeg-Hardware-Frames bringen mehr Reibung als Nutzen. |
| Einstellungs-GUI (`litecap-settings.exe`) | **C# / WPF (.NET 9, Fluent-Theme)**, **eigener Prozess** | Modernes Aussehen bei wenig Aufwand. Die GUI läuft nur, solange das Fenster offen ist; danach wird der Prozess beendet → im Hintergrund **0 MB / 0 % CPU**. Der Kern bleibt frei von GUI-Frameworks. |
| Encoding/Muxing | FFmpeg (libavcodec/libavformat/libswresample) über vcpkg | NVENC, AMF, QSV und libx264 über eine einheitliche API; MP4/MKV-Muxer. |
| Build | CMake + vcpkg (Manifest-Modus), Visual Studio 2022 | Bereits installiert. |

## 2. Module (Kern)

```
src/
  main.cpp               Einstieg, Single-Instance, Message-Loop
  app/                   App            – verbindet alle Module, Zustandsmaschine (Idle/Replay/Recording)
                         TrayIcon       – Shell_NotifyIcon, Kontextmenü, Balloon-Benachrichtigungen
                         Hotkeys        – RegisterHotKey
  config/                Config         – JSON laden/speichern (nlohmann/json), Defaults, Validierung
  capture/               GpuDevice      – D3D11-Device (auf der GPU mit dem Encoder)
                         WgcCapture     – Windows.Graphics.Capture (Monitor oder Fenster)
                         ColorConverter – D3D11 Video Processor: BGRA → NV12 auf der GPU
  audio/                 WasapiCapture  – Loopback (System) + Mikrofon, Event-gesteuert
  encode/                EncoderProbe   – erkennt NVENC/AMF/QSV/x264 und unterstützte Codecs
                         VideoEncoder   – libavcodec mit D3D11-Hardware-Frames (Zero-Copy)
                         AudioEncoder   – AAC (48 kHz, Stereo)
  buffer/                ReplayBuffer   – Ringpuffer für *kodierte* Pakete (RAM-Index + Datei auf Disk)
  output/                Muxer          – MP4/MKV schreiben (libavformat)
                         FileNamer      – Dateinamen-Schema mit Datum/Uhrzeit
                         Transcoder     – HEVC → AV1 (SVT-AV1) im Leerlauf, für GPUs ohne AV1-Encoder
  upload/                OAuth          – Google OAuth 2.0 (Loopback-Redirect + PKCE)
                         CredentialStore– Windows Credential Manager (CredWriteW/CredReadW)
                         YouTubeUploader– Resumable Upload (WinHTTP), Hintergrund-Thread, Retry
                         UploadQueue    – persistente Warteschlange (überlebt Neustarts)
  util/                  Log, Errors, ComPtr-Helfer
settings/                WPF-Projekt (eigener Prozess)
```

## 3. Datenfluss

```
            GPU (keine Kopie in den RAM)                                      CPU / Disk
 ┌───────────────────────────────────────────────────────┐
 │ WGC FramePool ──► ID3D11Texture2D (BGRA)               │
 │        │                                               │
 │        ▼                                               │
 │ D3D11 VideoProcessor  BGRA → NV12 (+ Skalierung)       │
 │        │                                               │
 │        ▼                                               │
 │ AVFrame (AV_PIX_FMT_D3D11) ──► NVENC / AMF / QSV ──────┼──► AVPacket (H.264/HEVC/AV1, ~KB)
 └───────────────────────────────────────────────────────┘          │
                                                                    │
 WASAPI Loopback ─┐                                                  │
 WASAPI Mikrofon ─┴► Resample 48 kHz ► AAC-Encoder ─────────────────►┤
                                                                    ▼
                                                  ┌─────────────────────────────────┐
                                    Replay-Modus  │ ReplayBuffer (Ring)             │
                                                  │  RAM: Paket-Index (wenige KB)   │
                                                  │  Disk: Ring-Datei (vorbelegt)   │
                                                  └────────────┬────────────────────┘
                                                Hotkey „Clip"   │ ab letztem Keyframe
                                                                ▼
                               Aufnahme-Modus ──────────► Muxer (MP4/MKV) ──► Datei
                                                                               │
                                       Ausgabe = Lokal / YouTube / Beides      ▼
                                                                 UploadQueue ─► YouTubeUploader
                                                                               (resumable, Retry)
                                                                               │
                                                     optional: lokale Datei nach Erfolg löschen
```

**Fallback x264 (kein HW-Encoder):** Textur → Staging-Textur → `sws_scale` → libx264
(`preset=veryfast`, `tune=zerolatency`). Nur hier wandern Frames durch den RAM.

## 4. Wichtige Designentscheidungen für niedrige Last

1. **Zero-Copy-Videopfad:** Capture, Farbkonvertierung und Encoding bleiben auf der GPU.
   Das D3D11-Device wird mit FFmpeg geteilt (`AV_HWDEVICE_TYPE_D3D11VA`), die NV12-Texturen
   stammen direkt aus FFmpegs Hardware-Frame-Pool.
2. **Kodierte Pakete puffern, nicht Rohbilder:** 1 min 1080p60 bei 20 Mbit/s ≈ 150 MB.
   Deshalb liegen die Paketdaten in einer **vorbelegten Ring-Datei** im Temp-Ordner,
   im RAM nur ein Index (≈ 40 Byte/Paket) → RAM-Verbrauch unabhängig von der Replay-Länge.
   Bei kurzen Puffern (≤ 64 MB) optional reiner RAM-Ring.
3. **Ereignisgesteuert statt Polling:** WGC-`FrameArrived`, WASAPI-Events,
   `MsgWaitForMultipleObjects` im Hauptthread. Kein Thread „dreht leer“.
4. **Frame-Pacing:** WGC liefert nur bei Bildänderung; fehlende Frames werden durch
   Wiederholung des letzten Frames (CFR) oder variable Timestamps (VFR in MKV) behandelt.
   Frames über dem FPS-Ziel werden verworfen, bevor sie die GPU-Konvertierung erreichen.
5. **Encoder-Einstellungen:** NVENC `p4`/`ll`, AMF `speed`, QSV `veryfast`;
   Rate-Control CBR/VBR oder CQP (Qualitätsstufe). B-Frames aus → weniger Latenz/VRAM.
6. **GUI außerhalb des Prozesses:** Der Kern lädt keine UI-Frameworks. Die GUI schreibt
   `config.json` und sendet `WM_COPYDATA` / ein benanntes Event → der Kern lädt neu.
7. **GPU-Priorität:** `D3DKMTSetProcessSchedulingPriorityClass` + `SetGPUThreadPriority`, damit
   unsere winzigen GPU-Befehle nicht hinter mehreren Frames eines GPU-limitierten Spiels warten.
8. **Thread-Priorität:** Capture-/Encoder-Thread `ABOVE_NORMAL`, Upload-Thread `BELOW_NORMAL`
   + Bandbreitenlimit, damit das Spiel nicht leidet.

## 4a. AV1 auf GPUs ohne AV1-Hardware-Encoder (z. B. RTX 20/30, Intel UHD)

Die NVENC-Einheit von RTX 20/30 (7. Gen.) kann AV1 nicht kodieren. Damit AV1 trotzdem
wählbar ist, bietet das Tool je nach erkannter Hardware folgende Modi an:

| Einstellung `video.codec` | RTX 40+ / Arc / RX 7000 | RTX 30 und älter |
|---|---|---|
| `h264`, `hevc` | NVENC/AMF/QSV live | NVENC live |
| `av1` | Hardware-AV1 live | **AV1 (nachträglich):** Live-Aufnahme in HEVC per NVENC, danach Umwandlung nach AV1 mit SVT-AV1 |

Live-AV1 in Software wird bewusst **nicht** angeboten (zu hohe CPU-Last beim Spielen).

**AV1 (nachträglich)** läuft im `Transcoder`-Modul (eigener Thread, `IDLE_PRIORITY_CLASS`,
begrenzte Thread-Anzahl). Optional startet die Umwandlung erst, wenn kein Vollbild-Spiel läuft
(`SHQueryUserNotificationState`). Erst danach wird die Datei gespeichert oder hochgeladen.
Während des Spielens bleibt die Last so niedrig wie bei normalem HEVC-Recording.

## 5. Threads

| Thread | Aufgabe |
|---|---|
| Main/UI | Message-Loop, Tray, Hotkeys, Zustandswechsel |
| Capture (WGC-Threadpool) | Neues Bild → BGRA→NV12 in die „aktuelles Bild“-Textur (nur GPU-Befehle) |
| Bildtakt (MMCSS) | Weckt exakt 1/fps, kopiert das aktuelle Bild in einen Encoder-Frame, reiht ihn ein |
| Encoder (MMCSS) | `avcodec_send_frame` / `receive_packet`, verteilt Pakete an Aufnahme/Replay. Getrennt vom Takt, weil NVENC warten kann, bis die (vom Spiel ausgelastete) GPU unsere Kopie ausgeführt hat |
| Audio (je Quelle, MMCSS „Audio“) | WASAPI-Events → Lücken mit Stille füllen → Spur mischen → AAC. Windows wandelt per `AUTOCONVERTPCM` direkt in 48 kHz/Stereo/float, daher kein eigener Resampler. Video-Frame 0 und Audio-Sample 0 haben denselben QPC-Nullpunkt → synchron |
| Writer (unter Normalpriorität) | Nimmt alle Pakete (Video + Ton) aus einer Warteschlange und schreibt sie in Aufnahme-Datei und Replay-Ringdatei. Der Encoder wartet so nie auf die Festplatte – ein kurz hängender Datenträger (Virenscanner, Cloud-Sync) kostet keine Frames. Alle 10 s Statistik ins Log |
| Upload | OAuth-Refresh, resumable Upload, Retry mit Backoff |

## 6. Fehlerbehandlung

| Situation | Verhalten |
|---|---|
| Kein HW-Encoder | Automatisch libx264 + Tray-Hinweis „Software-Encoding aktiv (höhere CPU-Last)“ |
| Codec (HEVC/AV1) nicht unterstützt | Rückfall auf H.264, Hinweis |
| WGC nicht verfügbar (vor Windows 10 1903) | Klare Fehlermeldung; Mindestversion ist Windows 10 1903 |
| Fenster geschlossen | Aufnahme stoppt sauber, Datei wird finalisiert |
| Speicher voll / Schreibfehler | Vorab-Prüfung `GetDiskFreeSpaceEx`; bei Fehler Aufnahme abbrechen, Datei finalisieren, Hinweis |
| Upload fehlgeschlagen | Exponentielles Backoff, Wiederaufnahme per `Content-Range`; Warteschlange bleibt persistent |
| Token abgelaufen/widerrufen | Refresh; bei `invalid_grant` → Hinweis „YouTube neu verbinden“ |
| GPU-Gerät verloren (Treiber-Reset) | Device + Encoder neu erzeugen, Aufnahme fortsetzen |

## 7. Umsetzungsschritte

1. ✅ Architektur (dieses Dokument)
2. ✅ Projektgerüst: CMake, vcpkg, Config, Logging, Tray-Icon, Hotkeys
3. ✅ Video-Capture (WGC) + GPU-Farbkonvertierung
4. ✅ Hardware-Encoder + Muxer → manuelle Aufnahme lauffähig
5. ✅ Audio (WASAPI Loopback + Mikrofon)
6. ✅ Replay-Buffer + Clip speichern (+ AV1-Nachbearbeitung)
7. ✅ YouTube: OAuth, Credential Manager, resumable Upload
8. ✅ Einstellungs-GUI (WPF)
9. ✅ Anleitung (README, docs/YOUTUBE.md) + Messen/Optimieren (docs/PERFORMANCE.md)
