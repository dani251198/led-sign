# Agentur-AoA LED Geschenk 🎉

ESP32 + WS2812B LED-Streifen, der das Agentur-für-Arbeit-Logo zum Leben erweckt: Uhrzeitmodus, Öffnungszeiten-/Termin-Anzeige, Web-UI, OTA-Updates.

## Quickstart
1. Installiere [VS Code](https://code.visualstudio.com/) + [PlatformIO](https://platformio.org/install).
2. `git clone` dieses Repo, öffne es in VS Code.
3. Abhängigkeiten sind in `platformio.ini` hinterlegt (WiFiManager, FastLED, ArduinoJson).
4. **LittleFS Web-UI hochladen**: `PlatformIO: Upload File System Image` (LittleFS muss in Board-Einstellungen verfügbar sein).
5. **Firmware flashen**: `PlatformIO: Upload` (Board: `env:esp32dev`).
6. Nach dem Boot startet ein WLAN "Felix-AoA-Setup". Verbinde dich, öffne `192.168.4.1`, trage dein WLAN ein. Danach verbindet sich das Gerät ins Heimnetz.
7. Öffne die Web-UI unter der angezeigten IP (`/`): Farben, Helligkeit, Modus, Öffnungszeiten, Termine, iCal-URL, OTA-URL.
8. WLAN später ändern: In der Web-UI auf „WLAN zurücksetzen“ klicken → Gerät rebootet, öffnet wieder das Setup-WLAN.

## Features
- **LED-Modi**
  - `clock`: 12h-Anzeige entlang des Streifens, Minuten werden weich überblendet.
  - `status`: Grün = offen, Rot = geschlossen (per Öffnungszeiten).
  - `appointment`: Pulsierender Termin-Farbton, wenn Termin gesetzt ist.
  - `effect`: Rainbow-Lauflicht (kleines Party-Ei).
- **Web-UI** (`/`)
  - LED-Anzahl, Helligkeit (0-100% Slider), Farben via Color-Picker, Modus, Effekt
  - Öffnungszeiten pro Wochentag (`HH:MM-HH:MM`)
  - iCal-URL (einfacher DTSTART-Parser) + mehrere manuelle Termine (`YYYY-MM-DD HH:MM`, Eingabe auch in deutschem Format `TT.MM.JJJJ HH:MM`) mit Hinzufügen/Löschen-Liste und Vorwarnzeit (Minuten vor Termin)
  - OTA: Firmware-URL (z.B. GitHub-Release-Asset), Gerät flasht & rebootet
  - WLAN Reset: Button in der Web-UI löscht gespeicherte WLAN-Creds, Gerät startet neu und öffnet erneut das Setup-WLAN
- **NTP**: Zeit via `pool.ntp.org`, Zeitzone konfigurierbar (POSIX-String).
- **Config**: Persistenz in `LittleFS` (`/config.json`).

## Pinout & Annahmen
- LED-Datenpin: `GPIO5` (falls anders, in `src/main.cpp` ändern).
- Max. LEDs: 120 (anpassbar via Config, hartes Limit `MAX_LEDS`).
- Versorgung: je nach LED-Anzahl ausreichendes 5V-Netzteil einplanen (ca. 60mA pro LED bei Vollweiß 100%).

## OTA aus GitHub-Release
1. Lege in GitHub ein Release mit Asset `firmware.bin` an (PlatformIO erzeugt `firmware.bin` im `.pio/build/esp32dev/`).
2. Kopiere die direkte Download-URL des Assets ("Right click copy link").
3. Trage sie in der Web-UI unter "Firmware URL" ein → `Update & Reboot`.

## API (kurz)
- `GET /api/config` → aktuelle Config
- `POST /api/config` (JSON) → speichern
- `GET /api/status` → wifi + modus + open + nextAppointment
- `POST /api/update` `{ "url": "https://.../firmware.bin" }`
- `GET /api/appointments` → Liste manueller Termine
- `POST /api/appointments` `{ "time": "YYYY-MM-DD HH:MM" }` → anfügen (max 10)
- `DELETE /api/appointments` `{ "index": <n> }` → Termin per Index löschen
- Statische Dateien aus `LittleFS` (`/data` → `index.html`).

## Ordner
- `src/main.cpp` – Firmware
- `data/index.html` – Web-UI (LittleFS)
- `req.md` – ursprüngliche Wunschliste

## Offene Punkte / Weiterführend
- iCal-Parser ist minimal (erstes `DTSTART`). Für echte Kalender: robusten Parser oder dedizierte API nutzen.
- Mehr Effekte? FastLED bietet reichlich (Noise, Palette, Beats).
- Hardware-Taster für Moduswechsel optional (nicht verbaut laut Vorgabe).

## Geburtstagsgruß an Felix 🎂
Lieber Felix,

möge dein Agentur-A-Logo nie rot blinken, deine Termine pünktlich auftauchen und dein LED-Ring immer im Takt deiner Lieblingssongs pulsieren. Viel Spaß beim Tüfteln, Umbauen und eigenen Ideen – das Repo gehört jetzt dir. Happy Birthday und viel Freude mit deinem neuen Spielzeug! 🎁

Cheers & viel Spaß beim Hacken!
