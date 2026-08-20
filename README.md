# C++ Spracherkennung & Smart Home Integration

## Übersicht
Das Projekt **spracherkennung_cpp** ist ein lokales Spracherkennungssystem auf Basis von Whisper.cpp mit Anbindung an Zigbee2MQTT zur Steuerung von Smart-Home-Komponenten.

## Projektstruktur & Architektur
- `CMakeLists.txt`: CMake-Build-System Konfiguration.
- `src/`: C++ Logik für Audio-Verarbeitung und Befehlsevaluation.
- `grammar.gbnf`: GBNF-Grammatik zur präzisen Erkennung von Sprachbefehlen.
- `moods.json`: Konfiguration von Lichtstimmungen und Aktionen.
- `run_aurora.sh`: Skript zur Initialisierung des Gesammtsystems.

## Hauptfunktionalitäten
- **Lokale Spracherkennung**: Datenschutzkonforme Verarbeitung ohne Cloud-Anbindung.
- **GBNF-Grammatiken**: Eingrenzung der Spracherkennung für erhöhte Genauigkeit.
- **MQTT-Schnittstelle**: Direkte Übertragung von Befehlen an Zigbee2MQTT.

## Ausführung & Nutzung
Kompilierung über CMake (`cmake -B build && cmake --build build`). Der Systemstart erfolgt über das Skript `./run_aurora.sh`.

## Lizenz
Dieses Projekt steht unter der MIT-Lizenz.
