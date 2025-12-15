#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <Update.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <FastLED.h>

// --------- Hardware configuration ---------
#define LED_PIN 5
#define MAX_LEDS 120
#define DEFAULT_LED_COUNT 12
#define FILE_CONFIG "/config.json"
#define MAX_APPOINTMENTS 10

// --------- LED and effect settings ---------
CRGB leds[MAX_LEDS];

struct DayWindow {
  String start; // format HH:MM
  String end;   // format HH:MM
};

struct DeviceConfig {
  int ledCount = DEFAULT_LED_COUNT;
  uint8_t brightness = 96;
  String mode = "clock"; // clock | status | appointment | effect
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  String icalUrl = "";
  String appointmentTime = ""; // legacy single appointment
  String appointments[MAX_APPOINTMENTS];
  uint8_t appointmentCount = 0;
  uint16_t notifyMinutesBefore = 30;
  String openColor = "00ff00";
  String closedColor = "ff0000";
  String appointmentColor = "00ffff";
  String clockColor = "ffffff";
  String effect = "rainbow";
  String effectColor = "ffffff";
  uint8_t effectSpeed = 4; // increment per frame for rainbow
  DayWindow hours[7];
};

DeviceConfig configState;
WebServer server(80);
unsigned long lastNtpSync = 0;
unsigned long lastIcalFetch = 0;
time_t nextAppointmentIcal = 0;

// Forward declarations
void saveConfig();
void loadConfig();

// --------- Helpers ---------
uint32_t parseHexColor(const String &hex) {
  if (hex.length() != 6) return 0xffffff;
  char buf[7];
  hex.toCharArray(buf, sizeof(buf));
  return strtoul(buf, nullptr, 16);
}

void colorToCrgb(uint32_t c, CRGB &out) {
  out.r = (c >> 16) & 0xFF;
  out.g = (c >> 8) & 0xFF;
  out.b = c & 0xFF;
}

bool parseTimeHM(const String &val, int &minutes) {
  if (val.length() != 5 || val.charAt(2) != ':') return false;
  int h = val.substring(0, 2).toInt();
  int m = val.substring(3).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minutes = h * 60 + m;
  return true;
}

bool parseAppointmentTime(const String &val, time_t &out) {
  if (val.length() < 16) return false; // YYYY-MM-DD HH:MM
  struct tm t = {};
  t.tm_year = val.substring(0, 4).toInt() - 1900;
  t.tm_mon = val.substring(5, 7).toInt() - 1;
  t.tm_mday = val.substring(8, 10).toInt();
  t.tm_hour = val.substring(11, 13).toInt();
  t.tm_min = val.substring(14, 16).toInt();
  t.tm_sec = 0;
  out = mktime(&t);
  return out > 0;
}

time_t nextManualAppointment(time_t nowLocal) {
  time_t best = 0;
  for (int i = 0; i < configState.appointmentCount; ++i) {
    time_t cand;
    if (!parseAppointmentTime(configState.appointments[i], cand)) continue;
    if (difftime(cand, nowLocal) < 0) continue; // past
    if (best == 0 || cand < best) best = cand;
  }
  // legacy single appointment
  time_t legacy;
  if (parseAppointmentTime(configState.appointmentTime, legacy)) {
    if (difftime(legacy, nowLocal) >= 0 && (best == 0 || legacy < best)) best = legacy;
  }
  return best;
}

time_t nextAnyAppointment(time_t nowLocal) {
  time_t manual = nextManualAppointment(nowLocal);
  time_t merged = 0;
  if (manual > 0) merged = manual;
  if (nextAppointmentIcal > 0 && (merged == 0 || nextAppointmentIcal < merged) && difftime(nextAppointmentIcal, nowLocal) >= 0) {
    merged = nextAppointmentIcal;
  }
  return merged;
}

bool addAppointment(const String &val) {
  if (configState.appointmentCount >= MAX_APPOINTMENTS) return false;
  time_t t;
  if (!parseAppointmentTime(val, t)) return false;
  configState.appointments[configState.appointmentCount++] = val;
  saveConfig();
  return true;
}

bool deleteAppointment(int index) {
  if (index < 0 || index >= configState.appointmentCount) return false;
  for (int i = index; i < configState.appointmentCount - 1; ++i) {
    configState.appointments[i] = configState.appointments[i + 1];
  }
  configState.appointmentCount--;
  saveConfig();
  return true;
}

bool isOpenNow(time_t nowLocal) {
  struct tm *tmNow = localtime(&nowLocal);
  int wday = tmNow->tm_wday; // 0 = Sunday
  int minutesNow = tmNow->tm_hour * 60 + tmNow->tm_min;
  DayWindow &dw = configState.hours[wday];
  int startM = 0, endM = 0;
  if (!parseTimeHM(dw.start, startM) || !parseTimeHM(dw.end, endM)) return false;
  return minutesNow >= startM && minutesNow <= endM;
}

void saveConfig() {
  StaticJsonDocument<2048> doc;
  doc["ledCount"] = configState.ledCount;
  doc["brightness"] = configState.brightness;
  doc["mode"] = configState.mode;
  doc["tz"] = configState.tz;
  doc["icalUrl"] = configState.icalUrl;
  doc["appointmentTime"] = configState.appointmentTime;
  doc["notifyMinutesBefore"] = configState.notifyMinutesBefore;
  JsonArray appointments = doc.createNestedArray("appointments");
  for (int i = 0; i < configState.appointmentCount; ++i) {
    appointments.add(configState.appointments[i]);
  }
  doc["openColor"] = configState.openColor;
  doc["closedColor"] = configState.closedColor;
  doc["appointmentColor"] = configState.appointmentColor;
  doc["clockColor"] = configState.clockColor;
  doc["effect"] = configState.effect;
  doc["effectColor"] = configState.effectColor;
  doc["effectSpeed"] = configState.effectSpeed;

  JsonArray hours = doc.createNestedArray("hours");
  for (int i = 0; i < 7; ++i) {
    JsonObject h = hours.createNestedObject();
    h["start"] = configState.hours[i].start;
    h["end"] = configState.hours[i].end;
  }

  File f = LittleFS.open(FILE_CONFIG, "w");
  if (!f) {
    Serial.println("Failed to open config for writing");
    return;
  }
  serializeJson(doc, f);
  f.close();
}

void loadConfig() {
  if (!LittleFS.exists(FILE_CONFIG)) {
    Serial.println("Config file missing, using defaults.");
    // default opening hours 08:00-16:00 Mon-Fri
    for (int i = 0; i < 7; ++i) {
      configState.hours[i].start = (i == 0 || i == 6) ? "00:00" : "08:00";
      configState.hours[i].end = (i == 0 || i == 6) ? "00:00" : "16:00";
    }
    saveConfig();
    return;
  }

  File f = LittleFS.open(FILE_CONFIG, "r");
  if (!f) {
    Serial.println("Failed to open config, using defaults");
    return;
  }
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("Failed to parse config, using defaults");
    return;
  }
  configState.ledCount = doc["ledCount"] | DEFAULT_LED_COUNT;
  configState.ledCount = constrain(configState.ledCount, 1, MAX_LEDS);
  configState.brightness = doc["brightness"] | 96;
  if (const char *v = doc["mode"]) configState.mode = v; else configState.mode = "clock";
  if (const char *v = doc["tz"]) configState.tz = v; else configState.tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  if (const char *v = doc["icalUrl"]) configState.icalUrl = v; else configState.icalUrl = "";
  if (const char *v = doc["appointmentTime"]) configState.appointmentTime = v; else configState.appointmentTime = "";
  configState.notifyMinutesBefore = doc["notifyMinutesBefore"] | 30;
  configState.appointmentCount = 0;
  JsonArray appointments = doc["appointments"].as<JsonArray>();
  if (!appointments.isNull()) {
    for (JsonVariant v : appointments) {
      if (configState.appointmentCount >= MAX_APPOINTMENTS) break;
      const char *t = v.as<const char *>();
      if (t) configState.appointments[configState.appointmentCount++] = t;
    }
  }
  if (const char *v = doc["openColor"]) configState.openColor = v; else configState.openColor = "00ff00";
  if (const char *v = doc["closedColor"]) configState.closedColor = v; else configState.closedColor = "ff0000";
  if (const char *v = doc["appointmentColor"]) configState.appointmentColor = v; else configState.appointmentColor = "00ffff";
  if (const char *v = doc["clockColor"]) configState.clockColor = v; else configState.clockColor = "ffffff";
  if (const char *v = doc["effect"]) configState.effect = v; else configState.effect = "rainbow";
  if (const char *v = doc["effectColor"]) configState.effectColor = v; else configState.effectColor = "ffffff";
  configState.effectSpeed = doc["effectSpeed"] | 4;
  configState.effectSpeed = constrain(configState.effectSpeed, 1, 20);

  JsonArray hours = doc["hours"].as<JsonArray>();
  for (int i = 0; i < 7 && i < hours.size(); ++i) {
    if (const char *s = hours[i]["start"]) configState.hours[i].start = s; else configState.hours[i].start = "00:00";
    if (const char *e = hours[i]["end"]) configState.hours[i].end = e; else configState.hours[i].end = "00:00";
  }
}

void sendJsonError(const String &msg) {
  StaticJsonDocument<128> doc;
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  server.send(400, "application/json", out);
}

// --------- OTA update from URL ---------
bool performUpdate(const String &url, bool isFs = false) {
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Update HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  WiFiClient *stream = http.getStreamPtr();
  if (!Update.begin(len, isFs ? U_SPIFFS : U_FLASH)) {
    Serial.println("Not enough space for update");
    http.end();
    return false;
  }

  size_t written = Update.writeStream(*stream);
  if (written != (size_t)len) {
    Serial.println("Update incomplete");
    http.end();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("Update failed: %s\n", Update.errorString());
    http.end();
    return false;
  }

  http.end();
  return Update.isFinished();
}

// --------- ICAL fetch placeholder ---------
void fetchIcalIfNeeded() {
  if (configState.icalUrl.length() == 0) return;
  if (millis() - lastIcalFetch < 30UL * 60UL * 1000UL) return; // every 30 min
  lastIcalFetch = millis();

  HTTPClient http;
  http.begin(configState.icalUrl);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("iCal fetch failed: %d\n", code);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  // Minimal parse: find first DTSTART
  int pos = payload.indexOf("DTSTART");
  if (pos < 0) return;
  int colon = payload.indexOf(":", pos);
  if (colon < 0) return;
  String ts = payload.substring(colon + 1, colon + 16); // YYYYMMDDTHHMMSS
  if (ts.length() < 15) return;

  struct tm t = {};
  t.tm_year = ts.substring(0, 4).toInt() - 1900;
  t.tm_mon = ts.substring(4, 6).toInt() - 1;
  t.tm_mday = ts.substring(6, 8).toInt();
  t.tm_hour = ts.substring(9, 11).toInt();
  t.tm_min = ts.substring(11, 13).toInt();
  t.tm_sec = ts.substring(13, 15).toInt();
  time_t parsed = mktime(&t);
  if (parsed > 0) nextAppointmentIcal = parsed;
}

// --------- LED rendering ---------
void showClock(time_t nowLocal, uint32_t colorHex, bool alert = false) {
  fill_solid(leds, configState.ledCount, CRGB::Black);
  double hours = (double)(nowLocal % 43200) / 3600.0; // 12h wrap
  double pos = (hours / 12.0) * (configState.ledCount - 1);
  int idx = floor(pos);
  double frac = pos - idx;
  CRGB c;
  colorToCrgb(colorHex, c);
  if (alert && ((millis() / 400) % 2 == 0)) {
    c = CRGB::White; // blink for appointment alert
  }
  leds[idx] = c;
  if (idx + 1 < configState.ledCount) leds[idx + 1] = c.fadeToBlackBy((1.0 - frac) * 255);
  if (idx > 0) leds[idx - 1] = c.fadeToBlackBy(frac * 255);
  FastLED.show();
}

void showStatus(time_t nowLocal) {
  bool open = isOpenNow(nowLocal);
  CRGB c;
  colorToCrgb(parseHexColor(open ? configState.openColor : configState.closedColor), c);
  fill_solid(leds, configState.ledCount, c);
  FastLED.show();
}

void showEffect() {
  static uint8_t hue = 0;
  static uint16_t chase = 0;
  if (configState.effect == "solid") {
    CRGB c;
    colorToCrgb(parseHexColor(configState.effectColor), c);
    fill_solid(leds, configState.ledCount, c);
  } else if (configState.effect == "breathe") {
    CRGB c;
    colorToCrgb(parseHexColor(configState.effectColor), c);
    uint8_t bpm = map(constrain(configState.effectSpeed, 1, 20), 1, 20, 6, 30);
    uint8_t val = beatsin8(bpm, 10, 255);
    c.nscale8_video(val);
    fill_solid(leds, configState.ledCount, c);
  } else if (configState.effect == "theater") {
    CRGB c;
    colorToCrgb(parseHexColor(configState.effectColor), c);
    fill_solid(leds, configState.ledCount, CRGB::Black);
    for (int i = chase % 3; i < configState.ledCount; i += 3) {
      leds[i] = c;
    }
    chase = (chase + constrain(configState.effectSpeed, 1, 20)) % 3;
  } else if (configState.effect == "twinkle") {
    for (int i = 0; i < configState.ledCount; ++i) {
      leds[i].fadeToBlackBy(20);
      if (random8() < constrain(configState.effectSpeed, 1, 20)) {
        CRGB c;
        colorToCrgb(parseHexColor(configState.effectColor), c);
        leds[i] = c;
      }
    }
  } else {
    uint8_t step = constrain(configState.effectSpeed, 1, 20);
    for (int i = 0; i < configState.ledCount; ++i) {
      leds[i] = CHSV(hue + i * 3, 255, 255);
    }
    hue += step;
  }
  FastLED.show();
}

void handleLeds(time_t nowLocal) {
  FastLED.setBrightness(configState.brightness);
  if (configState.mode == "effect") {
    showEffect();
    return;
  }

  time_t target = nextAnyAppointment(nowLocal);
  double diff = target > 0 ? difftime(target, nowLocal) : 1e9;
  bool appointmentActive = target > 0 && diff >= 0 && diff <= (configState.notifyMinutesBefore * 60);

  if (appointmentActive) {
    showClock(nowLocal, parseHexColor(configState.appointmentColor), true);
    return;
  }

  if (configState.mode == "status") {
    bool open = isOpenNow(nowLocal);
    showClock(nowLocal, parseHexColor(open ? configState.openColor : configState.closedColor));
    return;
  }

  // default clock mode
  showClock(nowLocal, parseHexColor(configState.clockColor));
}

// --------- Web API ---------
void handleConfigGet() {
  StaticJsonDocument<2048> doc;
  doc["ledCount"] = configState.ledCount;
  doc["brightness"] = configState.brightness;
  doc["mode"] = configState.mode;
  doc["tz"] = configState.tz;
  doc["icalUrl"] = configState.icalUrl;
  doc["appointmentTime"] = configState.appointmentTime;
  doc["openColor"] = configState.openColor;
  doc["closedColor"] = configState.closedColor;
  doc["appointmentColor"] = configState.appointmentColor;
  doc["clockColor"] = configState.clockColor;
  doc["effect"] = configState.effect;
  doc["effectColor"] = configState.effectColor;
  doc["effectSpeed"] = configState.effectSpeed;
  JsonArray hours = doc.createNestedArray("hours");
  for (int i = 0; i < 7; ++i) {
    JsonObject h = hours.createNestedObject();
    h["start"] = configState.hours[i].start;
    h["end"] = configState.hours[i].end;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleConfigPost() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  StaticJsonDocument<2048> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");

  configState.ledCount = constrain((int)doc["ledCount"].as<int>(), 1, MAX_LEDS);
  configState.brightness = doc["brightness"].as<int>();
  if (const char *v = doc["mode"]) configState.mode = v;
  if (const char *v = doc["tz"]) configState.tz = v;
  if (const char *v = doc["icalUrl"]) configState.icalUrl = v;
  if (const char *v = doc["appointmentTime"]) configState.appointmentTime = v;
  if (doc["notifyMinutesBefore"].is<int>()) configState.notifyMinutesBefore = doc["notifyMinutesBefore"].as<int>();
  if (const char *v = doc["openColor"]) configState.openColor = v;
  if (const char *v = doc["closedColor"]) configState.closedColor = v;
  if (const char *v = doc["appointmentColor"]) configState.appointmentColor = v;
  if (const char *v = doc["clockColor"]) configState.clockColor = v;
  if (const char *v = doc["effect"]) configState.effect = v;
  if (const char *v = doc["effectColor"]) configState.effectColor = v;
  if (doc["effectSpeed"].is<int>()) configState.effectSpeed = constrain(doc["effectSpeed"].as<int>(), 1, 20);

  JsonArray appts = doc["appointments"].as<JsonArray>();
  if (!appts.isNull()) {
    configState.appointmentCount = 0;
    for (JsonVariant v : appts) {
      if (configState.appointmentCount >= MAX_APPOINTMENTS) break;
      const char *t = v.as<const char *>();
      if (t) configState.appointments[configState.appointmentCount++] = t;
    }
  }

  JsonArray hours = doc["hours"].as<JsonArray>();
  for (int i = 0; i < 7 && i < hours.size(); ++i) {
    if (const char *s = hours[i]["start"]) configState.hours[i].start = s;
    if (const char *e = hours[i]["end"]) configState.hours[i].end = e;
  }

  saveConfig();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleStatus() {
  StaticJsonDocument<256> doc;
  time_t nowLocal = time(nullptr);
  time_t next = nextAnyAppointment(nowLocal);
  doc["wifi"] = WiFi.isConnected();
  doc["ip"] = WiFi.localIP().toString();
  doc["mode"] = configState.mode;
  doc["open"] = isOpenNow(nowLocal);
  doc["nextAppointment"] = (uint32_t)next;
  doc["notifyMinutesBefore"] = configState.notifyMinutesBefore;
  doc["notifyActive"] = (next > 0) && (difftime(next, nowLocal) <= configState.notifyMinutesBefore * 60);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleUpdate() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String url = doc["url"].as<String>();
  if (url.length() == 0) return sendJsonError("url missing");
  bool ok = performUpdate(url, false);
  if (!ok) return sendJsonError("update failed");
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleUpdateFs() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String url = doc["url"].as<String>();
  if (url.length() == 0) return sendJsonError("url missing");
  bool ok = performUpdate(url, true);
  if (!ok) return sendJsonError("update failed");
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleAppointmentsGet() {
  StaticJsonDocument<768> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < configState.appointmentCount; ++i) arr.add(configState.appointments[i]);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAppointmentsPost() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String t = doc["time"].as<String>();
  if (t.length() == 0) return sendJsonError("time missing");
  if (!addAppointment(t)) return sendJsonError("invalid time or full");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleAppointmentsDelete() {
  if (server.method() != HTTP_DELETE) return sendJsonError("DELETE required");
  StaticJsonDocument<128> doc;
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  int idx = doc["index"] | -1;
  if (!deleteAppointment(idx)) return sendJsonError("invalid index");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleWifiReset() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  // Erase WiFi credentials so WiFiManager opens AP on next boot.
  WiFi.disconnect(true, true);
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void setupServer() {
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/updatefs", HTTP_POST, handleUpdateFs);
  server.on("/api/appointments", HTTP_GET, handleAppointmentsGet);
  server.on("/api/appointments", HTTP_POST, handleAppointmentsPost);
  server.on("/api/appointments", HTTP_DELETE, handleAppointmentsDelete);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);
  // Serve static files; handle root explicitly
  server.serveStatic("/", LittleFS, "/");
  server.on("/", [](){
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
      server.send(404, "text/plain", "index.html not found");
      return;
    }
    server.streamFile(f, "text/html");
    f.close();
  });
  server.onNotFound([](){
    String path = server.uri();
    if (LittleFS.exists(path)) {
      File f = LittleFS.open(path, "r");
      if (f) {
        server.streamFile(f, "application/octet-stream");
        f.close();
        return;
      }
    }
    // fallback to index for SPA-like routing
    File f = LittleFS.open("/index.html", "r");
    if (f) {
      server.streamFile(f, "text/html");
      f.close();
    } else {
      server.send(404, "text/plain", "Not Found");
    }
  });
  server.begin();
}

void setupWifiAndTime() {
  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setCustomMenuHTML("<li class=\"menu-item\"><a href=\"/app\">LED Panel</a></li>");
  wm.setWebServerCallback([&]() {
    if (!wm.server) return;
    wm.server->on("/app", [&wm]() {
      if (!LittleFS.exists("/index.html")) {
        wm.server->send(404, "text/plain", "index.html not found");
        return;
      }
      File f = LittleFS.open("/index.html", "r");
      wm.server->streamFile(f, "text/html");
      f.close();
    });
  });
  wm.setConfigPortalTimeout(180);
  if (!wm.autoConnect("Felix-AoA-Setup")) {
    Serial.println("Failed to connect, restarting");
    delay(1000);
    ESP.restart();
  }
  Serial.print("Connected: ");
  Serial.println(WiFi.localIP());
  configTzTime(configState.tz.c_str(), "pool.ntp.org");
  lastNtpSync = millis();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  loadConfig();

  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, MAX_LEDS);
  FastLED.setBrightness(configState.brightness);

  setupWifiAndTime();
  setupServer();
}

void loop() {
  server.handleClient();
  time_t nowLocal = time(nullptr);
  if (millis() - lastNtpSync > 6UL * 60UL * 60UL * 1000UL) {
    configTzTime(configState.tz.c_str(), "pool.ntp.org");
    lastNtpSync = millis();
  }
  fetchIcalIfNeeded();
  handleLeds(nowLocal);
  delay(30);
}
