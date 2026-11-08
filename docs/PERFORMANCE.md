# Ressourcen messen und optimieren

## Gemessene Werte (Entwicklungs-PC)

RTX 3070 + Intel UHD 770, 20 Kerne, 1080p60, H.264/NVENC, 20 Mbit/s, System-Ton.
Gemessen **während ein GPU-limitiertes Spiel lief (GPU-Auslastung 98 %)**.

| Zustand | CPU (gesamt) | Working Set | Privater Speicher |
|---|---|---|---|
| Tray, kein Replay | 0,0 % | 12 MB | 11 MB |
| Replay-Puffer aktiv (120 s) | **0,18 %** | **90 MB** | 144 MB |
| Aufnahme (+ Ton) | 0,2–0,5 % | 90 MB | 150 MB |
| Einstellungs-GUI offen (eigener Prozess) | – | ~130 MB | – (0 MB, sobald geschlossen) |
| AV1-Umwandlung (eigener Kindprozess, Leerlaufpriorität) | 2 Kerne | – | ~540 MB (nur währenddessen) |

- Bildtakt: 60,0 fps, **0 ausgelassene Frames** trotz 98 % GPU-Last durch das Spiel.
- GPU-Aufwand pro Frame: eine Farbkonvertierung + eine Kopie (≈ 0,1 ms), Übergabe an NVENC Ø 0,22 ms CPU-Zeit.
- Clip speichern (10 s aus dem Replay-Puffer): 11 ms.

**Woher kommt der private Speicher?** Etwa 70 MB belegt allein der NVIDIA-Treiber für das
D3D11-Gerät + Windows.Graphics.Capture, weitere ~70 MB die NVENC-Sitzung im Treiber. FFmpeg nutzt
bereits die minimale Zahl von 4 NVENC-Surfaces (B-Frames aus, kein Lookahead). LiteCaps eigener
Code liegt im einstelligen MB-Bereich; der Replay-Puffer liegt auf der Festplatte (RAM-Index ≈ 3 MB
bei 10 min). Andere NVENC-Programme (OBS, ShadowPlay) haben denselben Treiber-Sockel.

## Selbst messen

### Schnelltests von LiteCap

```powershell
.\build\Release\litecap.exe --test-record 10 hevc    # CPU-Last + Dateiprüfung
.\build\Release\litecap.exe --test-replay 30 40      # Replay füllen, Clip speichern, RAM-Spitze
```

Nach jeder Session stehen im Log (`%LOCALAPPDATA%\LiteCap\logs\litecap.log`) Zeilen wie

```
Bildtakt: 1683 Frames erzeugt, ausgelassen: 0 (später geweckt), 0 (Encoder voll)
Encoder: 1683 Frames übergeben, Ø 0.24 ms, max 2.5 ms pro Frame
```

„Encoder voll“ > 0 bedeutet: Der Hardware-Encoder kommt nicht hinterher (zu hohe Auflösung/FPS
oder GPU vollständig ausgelastet). „Später geweckt“ > 0 bedeutet: Die CPU war so ausgelastet,
dass der Takt-Thread zu spät lief.

### Task-Manager

Details-Registerkarte → Spalten einblenden: **CPU**, **Arbeitsspeicher (privater Arbeitssatz)**,
**GPU**, **GPU-Modul**. Unter „Leistung → GPU“ zeigt der Graph **Video Encode** die NVENC-Last.

### PowerShell: CPU und RAM über eine Minute mitschreiben

```powershell
$p = Get-Process litecap; $c0 = $p.TotalProcessorTime; $t0 = Get-Date
1..60 | % { Start-Sleep 1; $p.Refresh(); "{0:N1} MB WS, {1:N1} MB privat" -f ($p.WorkingSet64/1MB), ($p.PrivateMemorySize64/1MB) }
$p.Refresh(); "CPU: {0:N2} %" -f (($p.TotalProcessorTime - $c0).TotalMilliseconds / ((Get-Date) - $t0).TotalMilliseconds / [Environment]::ProcessorCount * 100)
```

### Leistungsindikatoren (typeperf)

```powershell
typeperf "\Process(litecap)\% Processor Time" "\Process(litecap)\Private Bytes" "\Process(litecap)\IO Write Bytes/sec" -si 1 -sc 60
```

`% Processor Time` bezieht sich auf **einen** Kern (durch die Kernanzahl teilen).

### NVIDIA-Encoder-Auslastung

```powershell
nvidia-smi dmon -s u        # Spalte "enc" = NVENC-Auslastung in %
```

### Einfluss auf das Spiel (das Wichtigste)

Mit [PresentMon](https://github.com/GameTechDev/PresentMon) oder CapFrameX dieselbe Spielszene
zweimal messen – einmal ohne LiteCap, einmal mit aktivem Replay-Puffer – und **durchschnittliche
FPS sowie 1 %-Low-Frametimes** vergleichen. Ziel: kein messbarer Unterschied.

### Tiefenanalyse

```powershell
wpr -start CPU -start GPU -start DiskIO    # als Administrator
# … spielen, Clip speichern …
wpr -stop litecap.etl
```

`litecap.etl` mit dem Windows Performance Analyzer (WPA) öffnen → „CPU Usage (Precise)“ nach
Prozess `litecap.exe` filtern bzw. „GPU Utilization“ für die Engine-Belegung.

## Optimieren

| Maßnahme | Wirkung |
|---|---|
| **NVIDIA-App-Instant-Replay (ShadowPlay) ausschalten** | Läuft es parallel, wird alles doppelt kodiert (auf dem Entwicklungs-PC war der NVENC schon ohne LiteCap zu 14 % belegt). |
| **HEVC statt H.264** | Gleiche Qualität bei ~30 % weniger Bitrate → kleinere Dateien, weniger Schreiblast, schnellere Uploads. |
| **30 statt 60 fps** | Halbiert Encoder- und Schreibarbeit (für Desktop/Tutorials völlig ausreichend). |
| **Auflösung reduzieren** (z. B. 4K → 1440p) | Skalierung passiert kostenlos im Video Processor der GPU; weniger Encoder-Last und Bitrate. |
| **Replay-Puffer auf eine SSD** | Die Ringdatei wird kontinuierlich beschrieben (bei 20 Mbit/s ≈ 2,5 MB/s). |
| **Mikrofon/getrennte Spuren nur bei Bedarf** | Jede Tonspur kostet einen AAC-Encoder (≈ 0,1–0,2 % CPU). |
| **Hardwarebeschleunigte GPU-Planung (HAGS)** an, Windows-Spielmodus an | Bessere Priorisierung kleiner GPU-Aufträge neben Spielen. |
| **x264 vermeiden** | Nur Notlösung ohne Hardware-Encoder: 1080p60 kostet über einen CPU-Kern und schafft neben einem Spiel nicht alle Frames. |

### Mögliche Weiterentwicklungen

- **Variable Bildrate (VFR):** Bei statischem Bild gar nicht kodieren statt Duplikate zu erzeugen
  (spart bei Desktop-Aufnahmen Encoder-Arbeit; YouTube und die meisten Player kommen damit klar).
- **HDR-Aufnahme:** Capture in `R16G16B16A16Float`, HEVC/AV1 Main10 mit PQ.
- **Persistente Nachbearbeitung:** AV1-Umwandlungen, die beim Beenden noch nicht fertig waren, beim
  nächsten Start fortsetzen (derzeit bleibt die Datei dann im Originalformat).
