# Agentur für Arbeit Logo

- Geburtstagsgeschenk
- 3D Druck gehäuse
- weißer teil des logos ist transparent und soll mit einem led strip hinterleuchtet werden


## Requirements
- Anzeige Features
  - Anzeige Öffnungszeiten Agentur für Arbeit
  - Anzeige Termin bei Agentur für Arbeit 
    - Termine/Öffnungszeiten manuell und per iCal-Link in der WebUI konfigurierbar
  - Uhrzeit Modus: aktuelle Stunde wird durch entsprechende LED's angezeigt je nach minute sollen auch die LEDs daneben entsprechend gedimmt werden um übergänge anzuzeigen, wenn internet nicht verfügbar soll auch berücksichtigt werden. keine led ein 0 Uhr, gesamte strip länge beleuchtet: 12 Uhr. Der strip wird dem A entlang von unten links quasi kreisförmig nach innen gelegt
- Website um Einstellungen zu treffen/features ein- und auszuschalten
- es muss eine Webui geben bei der ich die WLAN Zugangsdaten eingeben können kann. darüber soll dann auch per NTP die Zeit abgerufen werden
- OTA Update mit Release in Github repo
- helligkeit und farben iwie anpassbar
- licht effekte

## Hardware
- WS2812B LED Strip mit xx LEDs in einer linie angeordnet
- ESP32
- keine taster oder ähnliches

## sonstiges
- schreibe ein readme mit geburtstagswünschen, sei kreativ
- soll in ein git repo, soll geburtstagskind in github zur verfügung stehen um selbst änderungen vornehmen zu können
- platformio