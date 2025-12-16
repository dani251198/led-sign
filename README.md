# Agentur-für-Felix LED 🎉

ESP32 + WS2812B LED-Streifen mit Web-UI, Terminen, Öffnungszeiten, Effekten, OTA-Updates und (experimentellem) iCal-Import.

## Quickstart
1. Installiere [VS Code](https://code.visualstudio.com/) + [PlatformIO](https://platformio.org/install).
2. `git clone` dieses Repo, öffne es in VS Code.
3. Abhängigkeiten sind in `platformio.ini` hinterlegt (WiFiManager, FastLED, ArduinoJson).
4. **LittleFS Web-UI hochladen**: `PlatformIO: Upload File System Image` (LittleFS muss in Board-Einstellungen verfügbar sein).
5. **Firmware flashen**: `PlatformIO: Upload` (Board/Env: `esp32-wroom32`).
6. Nach dem Boot startet ein WLAN "Agentur-für-Felix". Verbinde dich, öffne `192.168.4.1`, trage dein WLAN ein. Danach verbindet sich das Gerät ins Heimnetz.
7. Öffne die Web-UI unter der angezeigten IP (`/`): Farben, Helligkeit, Modus, Öffnungszeiten, Termine, iCal-URL, OTA-URL.
8. WLAN später ändern: In der Web-UI auf „WLAN zurücksetzen“ klicken → Gerät rebootet, öffnet wieder das Setup-WLAN.

## Features
- **LED-Modi & Priorität**
  - `clock`: 12h-Anzeige als fortlaufender Füllstand über den Streifen, Minuten werden weich überblendet.
  - `status`: Offen/zu-Farbe per Öffnungszeiten (grün/rot standard), optional übersteuert durch Termine.
  - `effect`: Rainbow, Solid, Breathe, Theater Chase, Twinkle, Xmas (Geschwindigkeit regelbar). Bei aktivem Termin wird für die Vorwarnzeit auf Terminfarbe umgeschaltet.
  - Priorität: Termin > Effekt/Uhr; bei Effekt ist Öffnungsstatus deaktiviert.
- **Termine & iCal**
  - Manuelle Termine (bis 10), Eingabe `YYYY-MM-DD HH:MM` oder deutsch `TT.MM.JJJJ HH:MM`, eigene Farbe je Termin, Vorwarnzeit (Minuten) mit Blink.
  - Mehrere iCal-Quellen (bis 5) mit eigener Farbe; parser sucht früheste zukünftige DTSTART (mit Zeilen-Unfold). **Aktuell unzuverlässig**, UI zeigt Warnung.
- **Öffnungszeiten**: Pro Wochentag (`HH:MM-HH:MM`), optional deaktivierbar; beeinflusst Uhr-Farbe im Statusmodus.
- **Farben & Helligkeit**: Color-Picker für open/closed/appointment/clock/effect, Helligkeit 0–100, LED-Anzahl fix 12.
- **OTA & Releases**
  - `/api/update` Firmware, `/api/updateFs` Filesystem, `/api/updateBundle` für FW+FS. FS-Update sichert `/config.json` und spielt es zurück (Einstellungen bleiben).
  - Web-UI Release-Knopf lädt GitHub-Latest-Release-Info und kann FW(+FS)-Asset flashen.
- **WLAN-Setup**: WiFiManager AP "Agentur-für-Felix" bei Erststart/Reset; Web-Button „WLAN zurücksetzen“ entfernt nur WLAN-Creds.
- **NTP & Zeitzone**: Zeit via `pool.ntp.org`, Zeitzone als POSIX-String konfigurierbar.
- **Persistenz**: `/config.json` in LittleFS; wird nach FS-Update automatisch wiederhergestellt.

## Pinout & Annahmen
- LED-Datenpin: `GPIO5` (falls anders, in `src/main.cpp` ändern).
- LED-Anzahl: 12 fest verdrahtet.
- Versorgung: je nach LED-Anzahl ausreichendes 5V-Netzteil einplanen (ca. 60mA pro LED bei Vollweiß 100%).

## OTA aus GitHub-Release

### Automatisch
1. passe `static const char *FW_VERSION = "v0.7.6";` entsprechend an
2. tagge entsprechend beginnend mit v* `git tag -a v0.7.6 -m "v0.7.6"`
3. pushe branch und tag `git push origin main --tags`
4. CI baut software automatisch innerhalb von 1min 30s, danach ist release via OTA verfügbar.

### Manuell
1. Lege in GitHub ein Release mit Asset `firmware.bin` an (PlatformIO erzeugt `firmware.bin` im `.pio/build/esp32dev/`).
2. Kopiere die direkte Download-URL des Assets ("Right click copy link").
3. Trage sie in der Web-UI unter "Firmware URL" ein → `Update & Reboot`.
4. Hinweis: Bei FS-Updates wird `/config.json` automatisch gesichert und danach zurückgeschrieben, damit Einstellungen bleiben.

## API (kurz)
- `GET /api/config` → aktuelle Config
- `POST /api/config` (JSON) → speichern
- `GET /api/status` → wifi + modus + open + nextAppointment + icalNext[] + notifyActive + version
- `POST /api/update` `{ "url": "https://.../firmware.bin" }`
- `POST /api/updateFs` `{ "url": "https://.../littlefs.bin" }` (Config wird gesichert/wiederhergestellt)
- `POST /api/updateBundle` `{ "fwUrl": "...", "fsUrl": "..." }`
- `GET /api/appointments` → Liste manueller Termine
- `POST /api/appointments` `{ "time": "YYYY-MM-DD HH:MM", "color": "RRGGBB" }` → anfügen (max 10)
- `DELETE /api/appointments` `{ "index": <n> }` → Termin per Index löschen
- `POST /api/wifireset` → löscht nur WLAN-Creds, rebootet
- Statische Dateien aus LittleFS (`/data` → `index.html`).

## Ordner
- `src/main.cpp` – Firmware
- `data/index.html` – Web-UI (LittleFS)
- `req.md` – ursprüngliche Wunschliste

## Offene Punkte / Weiterführend
- iCal-Parser ist minimal und aktuell unzuverlässig (trotz unfolded DTSTART). Für echte Kalender: robusten Parser oder dedizierte API nutzen.
- Mehr Effekte? FastLED bietet reichlich (Noise, Palette, Beats).
- Hardware-Taster für Moduswechsel optional (nicht verbaut laut Vorgabe).

## Geburtstagsgruß an Felix 🎂
Lieber Felix,

möge dein Agentur-für-Felix-Logo nie rot blinken, deine Termine pünktlich auftauchen und dein LED-Ring immer im Takt deiner Lieblingssongs pulsieren. Viel Spaß beim Tüfteln, Umbauen und eigenen Ideen – das Repo gehört jetzt dir. Happy Birthday und viel Freude mit deinem neuen Spielzeug! 🎁

Cheers & viel Spaß beim Hacken!
