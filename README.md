# Dauerhafte Spracherkennung mit whisper.cpp

Dieses Projekt liefert ein komplett lokales C++‑Programm, das kontinuierlich Audio vom Standardmikrofon abgreift, extrem latenzarme Transkripte erzeugt und bestimmte Schlüsselwörter (Lampe/Licht/Farben) sofort im Terminal hervorhebt. Audioaufnahme, Spracherkennung und Terminalausgabe laufen auf separaten Threads, sodass der Datenstrom stabil und ohne Blockaden bleibt.

## Features

- PortAudio-Recorder streamt permanent 16 kHz-Mono-Audio (0,5‑s Blöcke, 8 s Verlauf).
- whisper.cpp (Tiny/Base DE) verarbeitet den Ringpuffer ohne Wartezeit und reagiert quasi live.
- Treffer wie `Lampe`, `Licht`, `Liecht` oder Farbnamen (`rot`, `blau`, `grün`, `magenta`, …) lösen sofort `Erkannt: <Wort>` aus; zusätzlich erkennt das System Helligkeit/Brightness-Kommandos (`heller`, `ganz dunkel`, `30%`, `helligkeit hoch`, …) und Wärmebegriffe (`wärmer`, `zu kalt`, `Temperatur runter`, …)  inklusive grosszügiger Fuzzy-Matches, sobald „Lampe/Licht/…“ im Satz vorkommt (`hello` → `heller`, `pinkwood` → `pink-rot`, …). Die JSON-Ausgabe (`--json`) dient als Feed für den Zigbee-Controller.
- Standardfarbe: gemütliches warmweiss (`standard`, `standardfarbe`, `standartfarbe`) kann jederzeit per Sprachbefehl aktiviert werden.
- Eingebaute VAD (Voice Activity Detection) entfernt Hintergrundrauschen, bevor Audio in Whisper landet. Bei Bedarf lässt sich die VAD mit `--no-vad` deaktivieren.
- Mood-System: Speichere Sprachstimmungen („erstelle Mood Relax mit Farbe pink und 30 % Helligkeit“), wechsle per Sprache zwischen Moods oder lösche/aktualisiere sie.
- Automatisches Nacht-Dimming: an Wochentagen wird jede Lampe nach 22 Uhr sanft auf Schlafzimmer-Helligkeit (max. ca. 35 %) heruntergeregelt; relative Befehle und Einschaltsignale berücksichtigen das automatisch.
- Komplett offline: Nach dem Modell-Download ist weder Internet noch Cloud nötig.

## Voraussetzungen

- C++17-Compiler (g++ 11+, clang 13+, MSVC 2022).
- CMake ≥ 3.20.
- [PortAudio](http://www.portaudio.com/download.html) Entwicklungs-Pakete.
- Git + Curl/Wget für den Modell-Download.

### Linux

```bash
sudo apt update
sudo apt install build-essential cmake portaudio19-dev pkg-config git curl
# Arch: sudo pacman -S base-devel cmake portaudio git curl
```

### Windows (MSVC + vcpkg)

```powershell
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
& C:\dev\vcpkg\bootstrap-vcpkg.bat
C:\dev\vcpkg\vcpkg.exe install portaudio:x64-windows
```

Beim Konfigurieren später das Toolchain-File übergeben (`-DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake`).

## whisper.cpp & Modell laden

Die CMake-Konfiguration zieht `whisper.cpp` automatisch per `FetchContent`. Du musst nur noch ein deutsches GGML-Modell in den `models/` Ordner legen.

### Tiny (minimalste Latenz)

```bash
mkdir -p models
curl -L -o models/ggml-tiny.de.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.de.bin
```

### Base (etwas genauer, immer noch schnell)

Das allgemeine Base-Modell versteht auch Deutsch und liefert spürbar bessere Genauigkeit als Tiny:

```bash
curl -L -o models/ggml-base.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.bin
```

Unter Windows kann dasselbe mit PowerShell laufen:

```powershell
Invoke-WebRequest -Uri https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny.de.bin `
  -OutFile models\ggml-tiny.de.bin
```

## Bauen

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Windows (MSVC + vcpkg):

```powershell
cmake -S . -B build `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=C:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

Das Target heisst `continuous_listening` und landet unter `build/bin/`.

## Nutzung

```bash
./build/bin/continuous_listening --model models/ggml-base.bin
```

Optionen:

- `--device <index>`  explizit ein Eingabegerät wählen (Index siehe `pa_devs` oder `python3 -m sounddevice`).
- `--threads <n>`  Threadanzahl für Whisper (Standard = Anzahl CPU-Kerne).
- `--json`  statt Klartext werden Zeilen mit `{"text": "..."}` ausgegeben (für Automationen).
- `--no-vad`  deaktiviert die eingebaute Voice Activity Detection (standardmässig aktiv).

Die Erkennung startet sofort, schreibt neue Wörter direkt in den Stream und druckt bei Begriffen aus den Kategorien **Farben/Licht**, **Helligkeit (inkl. Prozentangaben)** sowie **Wärme/Temperatur** zusätzlich `Erkannt: <Wort>` in einer neuen Zeile. Mit `--json` erscheint jedes Fragment als eigenständiges JSON-Objekt  ideal für die Zigbee-Bridge. Der Dienst endet mit `Strg+C`.

## Aurora Voice Controller (Zigbee2MQTT)

Das Python-Skript `controller/aurora_controller.py` startet die C++-Binary mit `--json`, parst den fortlaufenden Textstrom und sendet MQTT-Kommandos an Zigbee2MQTT. Beispiele:

- „Stelle die Lampe auf rot.“
- „Mach die Lampe ein bisschen heller.“
- „Lampe Fenster auf blau und die Helligkeit auf 30 %.“
- „Lampe aus“ / „Lampe an“.
- „Stelle die Lampe auf Standard-Farbe.“ (lädt das warme Default-Weiss)
- Mischfarben wie „pink-rot“, „blau grün“ oder „cyan/magenta“ werden automatisch durch Mittelwertbildung gemischt  so lässt sich die Palette beliebig kombinieren.
- Relative Farbanpassungen („mach die Lampe etwas grüner“, „weniger rot“, „mehr blau“) verändern die aktuelle Farbe, ohne den zuvor gesetzten Farbton komplett zu ersetzen.
- Der Parser akzeptiert Befehle nur, wenn ein Aktivierungswort (Lampe/Licht/…) und ein klarer Intent („stelle/mache/…“) im selben Satz vorkommen  zufällige Hintergrund-Erwähnungen lösen daher keine Aktionen aus.

Jedes Kommando wird nur ausgeführt, wenn derselbe Satz `Lampe`, `Licht` oder `Liecht` enthält. Lampen können einzeln (z. B. „Fenster“) oder gemeinsam angesprochen werden.
Nach 22 Uhr an Wochentagen dimmt der Controller neue Befehle automatisch auf die konfigurierte Schlafzimmerhelligkeit  auch wenn du „Hell“ oder einen hohen Prozentwert sagst, wird höchstens die Nachtgrenze gesetzt.
Wenn kein Zigbee-Stick/MQTT-Broker bereitsteht, wechselt das Skript automatisch in einen Simulationsmodus: Alle erkannten Befehle tauchen als `[AKTION]`/`[SIM]` Logzeilen mit Farbe (Hex) und Helligkeit auf, sodass du die Sprachlogik ohne Hardware testen kannst.

### Einrichtung

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

Trage in `Config.py` deine Lampen-IDs, Wunschfarben sowie MQTT-Zugangsdaten ein (siehe unten).

### Start

```bash
python3 controller/aurora_controller.py \
  --binary build/bin/continuous_listening \
  --model models/ggml-base.bin
```

Optionale Flags:

- `--threads <n>`  Whisper-Threads überschreiben (Default aus `Config.ASR`).
- `--device <index>`  PortAudio-Gerät festlegen (Standard = Default Input).

Der Controller verbindet sich zu `MQTT_HOST:MQTT_PORT`, veröffentlicht nach `Z2M_TOPIC_PREFIX/<LampId>/set` und verwaltet lokale Helligkeitswerte, damit relative Befehle („ein bisschen heller“) funktionieren.

### HTTP-API für GUIs/Touchscreens

Für das geplante Touch-Display steht ein FastAPI-Server bereit. Er läuft auf macOS und Raspberry Pi und nimmt farb-/helligkeitsbezogene Kommandos in Echtzeit entgegen.

```bash
python3 controller/api_server.py --host 0.0.0.0 --port 8080
```

Wichtige Endpunkte:

- `GET /api/lamps`  aktueller Zustand aller Lampen (`color`, `brightness`, `device_id`).
- `POST /api/lamps/apply`  Farbe/Helligkeit/PWR setzen, z. B.:

  ```json
  {
    "lamps": ["Fenster"],
    "color_rgb": [255, 0, 128],
    "brightness_percent": 35
  }
  ```

- `GET /api/moods`  alle definierten Moods.
- `POST /api/moods`  Mood speichern/aktualisieren (`name`, `color_rgb` oder `color_name`, `brightness_percent`).
- `POST /api/moods/<name>/activate`  Mood anwenden.
- `DELETE /api/moods/<name>`  Mood entfernen.

Alle API-Kommandos greifen direkt auf den selben Zigbee-Client zu wie die Sprachsteuerung; Änderungen wirken daher sofort auf die Lampen, selbst wenn du im Millisekundentakt am Colorwheel drehst.

### Komplettstart mit Touch-GUI

Ein Kommando startet nun Sprachsteuerung, HTTP-API und die neue Touch-GUI gleichzeitig:

```bash
./run_aurora.sh
```

- Seite 1: schwarze Uhr (immer an).
- Seite 2: grosses Farbrad + Helligkeit/Wärme-Slider (jede Bewegung sendet sofort API-Kommandos).
- Seite 3: Mood-Manager inkl. Liste, Touch-Keyboard-Feld und Live-Steuerung.
- Seite 4: Status-Panel der verbundenen Lampen (Farbe, Brightness, Device-ID).

Die GUI ist mit PySide6/QtQuick implementiert und läuft auf macOS genauso wie auf dem Raspberry Pi mit Touch-Display.

### Mood-Funktion

- **Anlegen:** „Erstelle Mood *Relax* mit Farbe pink und 30 % Helligkeit“ oder „Erstelle Mood *Chill* mit 255.120.80“. Farbe und Helligkeit sind optional; ohne Angaben werden `DEFAULT_COLOR` und `DEFAULT_BRIGHTNESS` genutzt.
- **Aktivieren:** „Wechsle zu Mood *Relax*“ (kein Lampe-Stichwort nötig) oder „Stelle die Lampe auf *Relax*“ (Lampe + Mood-Name).
- **Ändern:** „Ändere die Farbe der Mood *Relax* auf 255.200.150“ bzw. „Ändere die Helligkeit der Mood *Relax* auf 45 %“.
- **Löschen:** „Lösche Mood *Relax*“.
- Synonyme wie „Mut“ (für Mood) oder „Helikai“ (für Helligkeit) werden automatisch normalisiert, sodass typische ASR-Fehler kein Problem darstellen.

Alle Moods landen persistiert in `MOODS_FILE` und können beliebig oft aufgerufen werden. Beim Aktivieren werden Farbe/Helligkeit wie gespeichert auf alle konfigurierten Lampen übertragen.

### Config.py

`Config.py` liefert alle Bausteine:

- `COLOR_NAMES`: Farbnamen → RGB.
- Mischfarben: Mehrere Farbnamen in einem Satz (z. B. „pink-rot“) werden automatisch gemittelt, solange mindestens zwei Einzelfarben bekannt sind.
- `BRIGHTNESS_STEPS`: Verstärkungen für „ein bisschen“, „viel“, …
- `CONTEXT_NORMALIZATIONS`: häufige ASR-Fehler → Zielwort.
- `LAMPS`: Anzeigename → Zigbee2MQTT-Geräte-ID.
- `NIGHT_DIM_*`: Uhrzeit, Wochentage und Zielhelligkeit für das automatische Dimming (Default: Wochentags ab 22 Uhr max. 35 % und Standard-Lichtton 0.25).
- `MOODS_FILE`: Speicherort der definierten Moods (JSON), wird automatisch angelegt/aktualisiert.
- MQTT-Parameter (`MQTT_HOST`, `MQTT_PORT`, `MQTT_USERNAME`, `MQTT_PASSWORD`, `Z2M_TOPIC_PREFIX`).
- `ASR["whisper"]`: Standardmodell + Threads für den ASR-Aufruf.

Passe diese Werte an deine EGLO/Zigbee-Installation an; anschliessend reicht der Start des Controllers für eine komplette Sprachsteuerung ohne Wake Word.

## Aufbau & Anpassung

- `src/main.cpp`
  - `SharedAudioBuffer`: Thread-sicherer Ringpuffer, hält 8 s Audio für den Rolling-Context.
  - `PortAudioCapturer`: Worker-Thread, der alle 512 Frames liest und in den Buffer schiebt.
  - `RecognitionLoop`: Fräst jede ~0,6 s die letzten 4 s Audio durch `whisper_full`, streamt nur die Differenz und löst Keyword-Hooks aus (einfach unten in `keywords` erweitern).
  - `VoiceActivityDetector`: 10 ms-Rahmenanalyse (VAD) mit Hangover/Pre-Roll, um Rauschen bereits vor Whisper zu entfernen (`--no-vad` deaktiviert sie).
- `CMakeLists.txt`: Bindet whisper.cpp (Commit v1.5.1) automatisch ein und linkt gegen PortAudio.
- `controller/aurora_controller.py`: Liest den JSON-Stream der Binary, normalisiert Sprachbefehle und verschickt MQTT-Payloads.
- `Config.py`: zentrale Ablage für Farben, Lampen-IDs, MQTT-Host und ASR-Defaults.

### Performance-Tuning

1. Verwende `ggml-tiny.de.bin` für maximale Reaktivität oder `ggml-base.bin` für bessere Genauigkeit.
2. Passe `kChunkSeconds` und `kContextSeconds` in `src/main.cpp` an, wenn du kleinere Blöcke (noch schnellere Antworten) oder mehr Kontext brauchst.
3. Auf Maschinen mit GPU-Unterstützung für whisper.cpp kannst du `ctx_params.use_gpu = true;` aktivieren.

## Stabilität

- Threads werden über ein gemeinsames Atomar-Flag und Condition-Variables sauber beendet.
- Audio-Überläufe werden protokolliert, blockieren aber nicht die Pipeline.
- Keine Internetverbindung nötig, sobald Modell + abhängige Bibliotheken lokal liegen.

## Lizenz

- Diese Quellen stehen unter derselben Lizenz wie das Repository (bitte ergänzen).
- whisper.cpp selbst steht unter MIT; die Modelle stammen aus dem offiziellen whisper.cpp-Repo.
