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
#define DEFAULT_LED_COUNT 12
#define DEFAULT_APPOINT_COLOR "00ffff"
#define FILE_CONFIG "/config.json"
#define MAX_APPOINTMENTS 10
#define MAX_ICALS 5
static const char *FW_VERSION = "v0.7.3";

// --------- LED and effect settings ---------
CRGB leds[DEFAULT_LED_COUNT];

struct DayWindow {
  String start; // format HH:MM
  String end;   // format HH:MM
};

struct AppointmentEntry {
  String time;  // YYYY-MM-DD HH:MM
  String color; // hex RGB (no #)
};

struct IcalSource {
  String url;
  String color; // hex RGB
};

struct DeviceConfig {
  int ledCount = DEFAULT_LED_COUNT;
  uint8_t brightness = 96;
  String mode = "clock"; // clock | status | appointment | effect
  String tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  String icalUrl = "";  // legacy single iCal URL
  String icalColor = "00ffff"; // legacy single iCal color
  IcalSource icals[MAX_ICALS];
  uint8_t icalCount = 0;
  bool enableAppointments = true;
  bool enableOpenHours = true;
  String appointmentTime = ""; // legacy single appointment
  AppointmentEntry appointments[MAX_APPOINTMENTS];
  uint8_t appointmentCount = 0;
  uint16_t notifyMinutesBefore = 30;
  String openColor = "00ff00";
  String closedColor = "ff0000";
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
time_t nextIcalTimes[MAX_ICALS] = {0};
WiFiManager *wmPortal = nullptr;
bool portalActive = false;
bool tzInitialized = false;

void showOtaProgress(size_t written, int total, bool isFs) {
  int half = configState.ledCount / 2;
  int segStart = isFs ? 0 : half;
  int segLen = isFs ? half : (configState.ledCount - half);
  if (segLen <= 0) segLen = configState.ledCount; // fallback if only few LEDs

  float pct;
  if (total > 0) {
    pct = constrain((float)written / (float)total, 0.0f, 1.0f);
  } else {
    // Unknown size: wrap a simple ramp to show activity
    pct = fmodf((float)written / 65536.0f, 1.0f);
  }

  int lit = (int)roundf(pct * segLen);
  CRGB color = isFs ? CRGB::Blue : CRGB::Orange;
  fill_solid(leds, configState.ledCount, CRGB::Black);
  for (int i = 0; i < segLen; ++i) {
    int idx = segStart + i;
    if (idx >= configState.ledCount) break;
    if (i < lit) {
      leds[idx] = color;
    } else if (i == lit && pct < 1.0f) {
      leds[idx] = color;
      leds[idx].nscale8_video(80);
    }
  }
  FastLED.setBrightness(configState.brightness);
  FastLED.show();
}

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

struct AppointmentHit {
  time_t when = 0;
  String color;
};

AppointmentHit nextManualAppointment(time_t nowLocal) {
  AppointmentHit hit;
  for (int i = 0; i < configState.appointmentCount; ++i) {
    time_t cand;
    if (!parseAppointmentTime(configState.appointments[i].time, cand)) continue;
    if (difftime(cand, nowLocal) < 0) continue; // past
    if (hit.when == 0 || cand < hit.when) {
      hit.when = cand;
      hit.color = configState.appointments[i].color;
    }
  }
  // legacy single appointment
  time_t legacy;
  if (parseAppointmentTime(configState.appointmentTime, legacy)) {
    if (difftime(legacy, nowLocal) >= 0 && (hit.when == 0 || legacy < hit.when)) {
      hit.when = legacy;
      hit.color = DEFAULT_APPOINT_COLOR;
    }
  }
  return hit;
}

AppointmentHit nextAnyAppointment(time_t nowLocal) {
  AppointmentHit manual = nextManualAppointment(nowLocal);
  AppointmentHit merged = manual;
  for (int i = 0; i < configState.icalCount; ++i) {
    time_t cand = nextIcalTimes[i];
    if (cand > 0 && difftime(cand, nowLocal) >= 0) {
      if (merged.when == 0 || cand < merged.when) {
        merged.when = cand;
        merged.color = configState.icals[i].color.length() == 6 ? configState.icals[i].color : configState.icalColor;
      }
    }
  }
  return merged;
}

bool addAppointment(const String &val, const String &color) {
  if (configState.appointmentCount >= MAX_APPOINTMENTS) return false;
  time_t t;
  if (!parseAppointmentTime(val, t)) return false;
  configState.appointments[configState.appointmentCount].time = val;
  configState.appointments[configState.appointmentCount].color = color.length() == 6 ? color : DEFAULT_APPOINT_COLOR;
  configState.appointmentCount++;
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
  DynamicJsonDocument doc(2048);
  doc["brightness"] = configState.brightness;
  doc["mode"] = configState.mode;
  doc["tz"] = configState.tz;
  doc["icalUrl"] = configState.icalUrl;
  doc["icalColor"] = configState.icalColor;
  JsonArray icalsSave = doc["icals"].to<JsonArray>();
  for (int i = 0; i < configState.icalCount; ++i) {
    JsonObject ic = icalsSave.add<JsonObject>();
    ic["url"] = configState.icals[i].url;
    ic["color"] = configState.icals[i].color;
  }
  doc["appointmentTime"] = configState.appointmentTime;
  doc["notifyMinutesBefore"] = configState.notifyMinutesBefore;
  JsonArray appointments = doc["appointments"].to<JsonArray>();
  for (int i = 0; i < configState.appointmentCount; ++i) {
    JsonObject a = appointments.add<JsonObject>();
    a["time"] = configState.appointments[i].time;
    a["color"] = configState.appointments[i].color;
  }
  doc["openColor"] = configState.openColor;
  doc["closedColor"] = configState.closedColor;
  doc["clockColor"] = configState.clockColor;
  doc["effect"] = configState.effect;
  doc["effectColor"] = configState.effectColor;
  doc["effectSpeed"] = configState.effectSpeed;
  doc["icalColor"] = configState.icalColor;
  doc["enableAppointments"] = configState.enableAppointments;
  doc["enableOpenHours"] = configState.enableOpenHours;

  JsonArray hours = doc["hours"].to<JsonArray>();
  for (int i = 0; i < 7; ++i) {
    JsonObject h = hours.add<JsonObject>();
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
  // LED count is fixed and not configurable
  configState.ledCount = DEFAULT_LED_COUNT;

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
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("Failed to parse config, using defaults");
    return;
  }
  configState.brightness = doc["brightness"] | 96;
  if (const char *v = doc["mode"]) configState.mode = v; else configState.mode = "clock";
  if (const char *v = doc["tz"]) configState.tz = v; else configState.tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  if (const char *v = doc["icalUrl"]) configState.icalUrl = v; else configState.icalUrl = "";
  if (const char *v = doc["icalColor"]) configState.icalColor = v; else configState.icalColor = DEFAULT_APPOINT_COLOR;
  configState.icalCount = 0;
  JsonArray icalsLoad = doc["icals"].as<JsonArray>();
  if (!icalsLoad.isNull()) {
    for (JsonVariant v : icalsLoad) {
      if (configState.icalCount >= MAX_ICALS) break;
      const char *url = v["url"].as<const char *>();
      const char *color = v["color"].as<const char *>();
      if (url && strlen(url) > 0) {
        configState.icals[configState.icalCount].url = url;
        configState.icals[configState.icalCount].color = color ? color : configState.icalColor;
        configState.icalCount++;
      }
    }
  }
  if (configState.icalCount == 0 && configState.icalUrl.length() > 0) {
    configState.icals[0].url = configState.icalUrl;
    configState.icals[0].color = configState.icalColor;
    configState.icalCount = 1;
  }
  for (int i = 0; i < MAX_ICALS; ++i) nextIcalTimes[i] = 0;
  lastIcalFetch = 0;
  configState.enableAppointments = doc["enableAppointments"].is<bool>() ? doc["enableAppointments"].as<bool>() : true;
  configState.enableOpenHours = doc["enableOpenHours"].is<bool>() ? doc["enableOpenHours"].as<bool>() : true;
  if (const char *v = doc["appointmentTime"]) configState.appointmentTime = v; else configState.appointmentTime = "";
  configState.notifyMinutesBefore = doc["notifyMinutesBefore"] | 30;
  configState.appointmentCount = 0;
  JsonArray appointments = doc["appointments"].as<JsonArray>();
  if (!appointments.isNull()) {
    for (JsonVariant v : appointments) {
      if (configState.appointmentCount >= MAX_APPOINTMENTS) break;
      const char *t = v["time"].as<const char *>();
      const char *c = v["color"].as<const char *>();
      if (t) {
        configState.appointments[configState.appointmentCount].time = t;
        configState.appointments[configState.appointmentCount].color = c ? c : DEFAULT_APPOINT_COLOR;
        configState.appointmentCount++;
      }
    }
  }
  if (const char *v = doc["openColor"]) configState.openColor = v; else configState.openColor = "00ff00";
  if (const char *v = doc["closedColor"]) configState.closedColor = v; else configState.closedColor = "ff0000";
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

void sendJsonErrorTo(WebServer &ws, const String &msg) {
  DynamicJsonDocument doc(128);
  doc["error"] = msg;
  String out;
  serializeJson(doc, out);
  ws.send(400, "application/json", out);
}

void sendJsonError(const String &msg) {
  sendJsonErrorTo(server, msg);
}

String buildConfigJson() {
  DynamicJsonDocument doc(2048);
  doc["brightness"] = configState.brightness;
  doc["mode"] = configState.mode;
  doc["tz"] = configState.tz;
  doc["icalUrl"] = configState.icalUrl;
  doc["icalColor"] = configState.icalColor;
  JsonArray icalsConfig = doc["icals"].to<JsonArray>();
  for (int i = 0; i < configState.icalCount; ++i) {
    JsonObject ic = icalsConfig.add<JsonObject>();
    ic["url"] = configState.icals[i].url;
    ic["color"] = configState.icals[i].color;
  }
  doc["appointmentTime"] = configState.appointmentTime;
  doc["enableAppointments"] = configState.enableAppointments;
  doc["enableOpenHours"] = configState.enableOpenHours;
  doc["notifyMinutesBefore"] = configState.notifyMinutesBefore;
  doc["openColor"] = configState.openColor;
  doc["closedColor"] = configState.closedColor;
  doc["clockColor"] = configState.clockColor;
  doc["effect"] = configState.effect;
  doc["effectColor"] = configState.effectColor;
  doc["effectSpeed"] = configState.effectSpeed;
  JsonArray hours = doc["hours"].to<JsonArray>();
  for (int i = 0; i < 7; ++i) {
    JsonObject h = hours.add<JsonObject>();
    h["start"] = configState.hours[i].start;
    h["end"] = configState.hours[i].end;
  }
  String out;
  serializeJson(doc, out);
  return out;
}

String buildStatusJson() {
  DynamicJsonDocument doc(256);
  time_t nowLocal = time(nullptr);
  AppointmentHit next = nextAnyAppointment(nowLocal);
  doc["wifi"] = WiFi.isConnected();
  doc["ip"] = WiFi.localIP().toString();
  doc["mode"] = configState.mode;
  doc["enableAppointments"] = configState.enableAppointments;
  doc["enableOpenHours"] = configState.enableOpenHours;
  doc["open"] = isOpenNow(nowLocal);
  doc["nextAppointment"] = (uint32_t)next.when;
  JsonArray icalNextArr = doc["icalNext"].to<JsonArray>();
  for (int i = 0; i < configState.icalCount; ++i) {
    JsonObject o = icalNextArr.add<JsonObject>();
    o["url"] = configState.icals[i].url;
    o["color"] = configState.icals[i].color;
    o["next"] = (uint32_t)nextIcalTimes[i];
  }
  doc["notifyMinutesBefore"] = configState.notifyMinutesBefore;
  doc["notifyActive"] = (next.when > 0) && (difftime(next.when, nowLocal) <= configState.notifyMinutesBefore * 60);
  doc["version"] = FW_VERSION;
  String out;
  serializeJson(doc, out);
  return out;
}

bool applyConfigJson(const String &body, String &errOut) {
  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    errOut = "JSON parse error";
    return false;
  }
  if (doc["brightness"].is<int>()) {
    configState.brightness = doc["brightness"].as<int>();
  }
  if (const char *v = doc["mode"]) configState.mode = v;
  if (const char *v = doc["tz"]) configState.tz = v;
  if (const char *v = doc["icalUrl"]) configState.icalUrl = v; else configState.icalUrl = "";
  if (const char *v = doc["icalColor"]) configState.icalColor = v; else configState.icalColor = DEFAULT_APPOINT_COLOR;
  configState.enableAppointments = doc["enableAppointments"].is<bool>() ? doc["enableAppointments"].as<bool>() : true;
  configState.enableOpenHours = doc["enableOpenHours"].is<bool>() ? doc["enableOpenHours"].as<bool>() : true;
  if (const char *v = doc["appointmentTime"]) configState.appointmentTime = v;
  if (doc["notifyMinutesBefore"].is<int>()) configState.notifyMinutesBefore = doc["notifyMinutesBefore"].as<int>();
  if (const char *v = doc["openColor"]) configState.openColor = v;
  if (const char *v = doc["closedColor"]) configState.closedColor = v;
  if (const char *v = doc["clockColor"]) configState.clockColor = v;
  if (const char *v = doc["effect"]) configState.effect = v;
  if (const char *v = doc["effectColor"]) configState.effectColor = v;
  if (doc["effectSpeed"].is<int>()) configState.effectSpeed = constrain(doc["effectSpeed"].as<int>(), 1, 20);

  JsonArray appts = doc["appointments"].as<JsonArray>();
  if (!appts.isNull()) {
    configState.appointmentCount = 0;
    for (JsonVariant v : appts) {
      if (configState.appointmentCount >= MAX_APPOINTMENTS) break;
      const char *t = v["time"].as<const char *>();
      const char *c = v["color"].as<const char *>();
      if (t) {
        configState.appointments[configState.appointmentCount].time = t;
        configState.appointments[configState.appointmentCount].color = c ? c : DEFAULT_APPOINT_COLOR;
        configState.appointmentCount++;
      }
    }
  }

  JsonArray hours = doc["hours"].as<JsonArray>();
  for (int i = 0; i < 7 && i < hours.size(); ++i) {
    if (const char *s = hours[i]["start"]) configState.hours[i].start = s;
    if (const char *e = hours[i]["end"]) configState.hours[i].end = e;
  }

  // iCal list
  configState.icalCount = 0;
  JsonArray icalsApply = doc["icals"].as<JsonArray>();
  if (!icalsApply.isNull()) {
    for (JsonVariant v : icalsApply) {
      if (configState.icalCount >= MAX_ICALS) break;
      const char *url = v["url"].as<const char *>();
      const char *color = v["color"].as<const char *>();
      if (url && strlen(url) > 0) {
        configState.icals[configState.icalCount].url = url;
        configState.icals[configState.icalCount].color = color ? color : configState.icalColor;
        configState.icalCount++;
      }
    }
  }
  if (configState.icalCount == 0 && configState.icalUrl.length() > 0) {
    configState.icals[0].url = configState.icalUrl;
    configState.icals[0].color = configState.icalColor;
    configState.icalCount = 1;
  }

  for (int i = 0; i < MAX_ICALS; ++i) nextIcalTimes[i] = 0;
  lastIcalFetch = 0;

  return true;
}

// --------- OTA update from URL ---------
bool performUpdate(const String &url, bool isFs = false) {
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  Serial.printf("[OTA] Starte %s-Update: %s\n", isFs ? "FS" : "FW", url.c_str());
  http.setTimeout(20000); // 20s socket timeout to survive slow links
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Update HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }

  int len = http.getSize();
  Serial.printf("[OTA] HTTP OK, size=%d Bytes (kann -1 sein)\n", len);
  WiFiClient *stream = http.getStreamPtr();
  if (!Update.begin(len, isFs ? U_SPIFFS : U_FLASH)) {
    Serial.println("Not enough space for update");
    http.end();
    return false;
  }

  const size_t bufSize = 1024;
  uint8_t buf[bufSize];
  size_t written = 0;
  unsigned long lastProgress = millis();
  const unsigned long timeoutMs = 60000; // 60s ohne Fortschritt => Abbruch

  while (stream->connected() && (len < 0 || written < (size_t)len)) {
    size_t avail = stream->available();
    if (avail) {
      size_t toRead = avail > bufSize ? bufSize : avail;
      int r = stream->readBytes(buf, toRead);
      if (r > 0) {
        if (Update.write(buf, r) != r) {
          Serial.println("[OTA] Write error");
          http.end();
          return false;
        }
        written += r;
        lastProgress = millis();
        showOtaProgress(written, len, isFs);
        if (written % 262144 < (size_t)r) { // alle ~256KB
          Serial.printf("[OTA] Fortschritt: %u Bytes\n", (unsigned)written);
        }
      }
    } else {
      delay(25);
    }
    if (millis() - lastProgress > timeoutMs) {
      Serial.println("[OTA] Timeout ohne Fortschritt");
      http.end();
      return false;
    }
  }

  Serial.printf("[OTA] Geschrieben: %u Bytes\n", (unsigned)written);
  if (len >= 0 && written != (size_t)len) {
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
  Serial.printf("[OTA] %s-Update erfolgreich, reboot folgt.\n", isFs ? "FS" : "FW");
  return Update.isFinished();
}

// Perform FS update but restore config afterwards so user settings survive.
bool updateFsPreserveConfig(const String &url) {
  String backup;
  if (LittleFS.exists(FILE_CONFIG)) {
    File f = LittleFS.open(FILE_CONFIG, "r");
    if (f) {
      backup = f.readString();
      f.close();
    }
  }

  if (!performUpdate(url, true)) return false;

  LittleFS.end();
  if (!LittleFS.begin()) {
    Serial.println("LittleFS remount failed after update");
    return false;
  }

  if (backup.length() > 0) {
    File f = LittleFS.open(FILE_CONFIG, "w");
    if (!f) {
      Serial.println("Failed to restore config after FS update");
      return false;
    }
    f.print(backup);
    f.close();
  }
  return true;
}

// --------- ICAL fetch placeholder ---------
void fetchIcalIfNeeded() {
  if (configState.icalCount == 0) return;
  if (millis() - lastIcalFetch < 30UL * 60UL * 1000UL) return; // every 30 min
  lastIcalFetch = millis();

  auto parseDtstart = [](String line) -> time_t {
    int colon = line.indexOf(":");
    if (colon < 0) return 0;
    String ts = line.substring(colon + 1);
    ts.trim();
    ts.replace("Z", ""); // ignore UTC marker, treat as local for simplicity
    ts.replace("T", "");
    if (ts.length() < 8) return 0;
    while (ts.length() < 14) ts += "0"; // pad missing hhmmss if only date present

    struct tm t = {};
    t.tm_year = ts.substring(0, 4).toInt() - 1900;
    t.tm_mon  = ts.substring(4, 6).toInt() - 1;
    t.tm_mday = ts.substring(6, 8).toInt();
    t.tm_hour = ts.substring(8, 10).toInt();
    t.tm_min  = ts.substring(10, 12).toInt();
    t.tm_sec  = ts.substring(12, 14).toInt();
    time_t parsed = mktime(&t);
    return parsed > 0 ? parsed : 0;
  };

  for (int idx = 0; idx < configState.icalCount; ++idx) {
    const String &url = configState.icals[idx].url;
    if (url.length() == 0) {
      nextIcalTimes[idx] = 0;
      continue;
    }

    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(url);
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      Serial.printf("iCal fetch failed (%d) for %s\n", code, url.c_str());
      http.end();
      nextIcalTimes[idx] = 0;
      continue;
    }

    String payload = http.getString();
    http.end();
    time_t nowLocal = time(nullptr);
    time_t bestFuture = 0;
    time_t bestAny = 0;
    int pos = 0;
    while (true) {
      pos = payload.indexOf("DTSTART", pos);
      if (pos < 0) break;
      int nl = payload.indexOf('\n', pos);
      String line = payload.substring(pos, nl < 0 ? payload.length() : nl);
      if (line.endsWith("\r")) line.remove(line.length() - 1);
      // consume folded continuation lines starting with space/tab
      int contPos = nl;
      while (contPos > 0 && contPos < (int)payload.length()) {
        int nextNl = payload.indexOf('\n', contPos + 1);
        String cont = payload.substring(contPos + 1, nextNl < 0 ? payload.length() : nextNl);
        if (!(cont.startsWith(" ") || cont.startsWith("\t"))) break;
        cont.trim();
        line += cont;
        contPos = nextNl;
        nl = nextNl;
      }

      time_t cand = parseDtstart(line);
      if (cand > 0 && difftime(cand, nowLocal) >= 0) {
        if (bestFuture == 0 || cand < bestFuture) bestFuture = cand;
      }
      if (cand > 0) {
        if (bestAny == 0 || cand < bestAny) bestAny = cand;
      }

      if (nl < 0) break;
      pos = nl + 1;
    }
    nextIcalTimes[idx] = bestFuture > 0 ? bestFuture : bestAny;
  }
}

// --------- LED rendering ---------
void showClock(time_t nowLocal, uint32_t colorHex, bool alert = false) {
  fill_solid(leds, configState.ledCount, CRGB::Black);

  // Map local 12h range onto strip as a progressive fill (e.g., 20:30 -> 8 full, 9th half)
  struct tm *tmNow = localtime(&nowLocal);
  double hours12 = (tmNow->tm_hour % 12) + (tmNow->tm_min / 60.0); // 0..12
  double pos = (hours12 / 12.0) * configState.ledCount;   // 0..ledCount
  int full = floor(pos);
  double frac = pos - full; // 0..1

  CRGB base;
  colorToCrgb(colorHex, base);
  if (alert && ((millis() / 400) % 2 == 0)) {
    base = CRGB::White; // blink for appointment alert
  }

  for (int i = 0; i < configState.ledCount; ++i) {
    if (i < full) {
      leds[i] = base;
    } else if (i == full && frac > 0.0 && i < configState.ledCount) {
      leds[i] = base;
      leds[i].nscale8_video((uint8_t)round(frac * 255));
    } else {
      leds[i] = CRGB::Black;
    }
  }

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
  static unsigned long lastTheaterStep = 0;
  static unsigned long lastXmasStep = 0;
  const unsigned long nowMs = millis();
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
    uint8_t speed = constrain(configState.effectSpeed, 1, 20);
    uint16_t stepMs = map(speed, 1, 20, 250, 40);
    if (nowMs - lastTheaterStep >= stepMs) {
      lastTheaterStep = nowMs;
      chase = (chase + 1) % 3;
    }
    for (int i = chase % 3; i < configState.ledCount; i += 3) {
      leds[i] = c;
    }
  } else if (configState.effect == "twinkle") {
    for (int i = 0; i < configState.ledCount; ++i) {
      leds[i].fadeToBlackBy(20);
      if (random8() < constrain(configState.effectSpeed, 1, 20)) {
        CRGB c;
        colorToCrgb(parseHexColor(configState.effectColor), c);
        leds[i] = c;
      }
    }
  } else if (configState.effect == "xmas") {
    // Colorful blinking string: red, green, gold, blue with gentle decay
    static const CRGB palette[] = {CRGB::Red, CRGB::Green, CRGB(255,215,0), CRGB::Blue};
    uint8_t speed = constrain(configState.effectSpeed, 1, 20);
    uint16_t stepMs = map(speed, 1, 20, 320, 80);
    uint8_t chance = map(speed, 1, 20, 20, 120); // more hits when faster
    if (nowMs - lastXmasStep >= stepMs) {
      lastXmasStep = nowMs;
      for (int i = 0; i < configState.ledCount; ++i) {
        leds[i].fadeToBlackBy(40);
        if (random8() < chance) {
          leds[i] = palette[random8(4)];
        }
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
    AppointmentHit next = nextAnyAppointment(nowLocal);
    double diff = next.when > 0 ? difftime(next.when, nowLocal) : 1e9;
    bool appointmentActive = configState.enableAppointments && next.when > 0 && diff >= 0 && diff <= (configState.notifyMinutesBefore * 60);
    if (appointmentActive) {
      uint32_t color = parseHexColor(next.color.length() == 6 ? next.color : DEFAULT_APPOINT_COLOR);
      showClock(nowLocal, color, true);
    } else {
      showEffect();
    }
    return;
  }

  AppointmentHit next = nextAnyAppointment(nowLocal);
  double diff = next.when > 0 ? difftime(next.when, nowLocal) : 1e9;
  bool appointmentActive = configState.enableAppointments && next.when > 0 && diff >= 0 && diff <= (configState.notifyMinutesBefore * 60);

  if (appointmentActive) {
    uint32_t color = parseHexColor(next.color.length() == 6 ? next.color : DEFAULT_APPOINT_COLOR);
    showClock(nowLocal, color, true);
    return;
  }

  // Clock base with optional open/closed overlay
  bool open = isOpenNow(nowLocal);
  uint32_t baseColor = parseHexColor(configState.clockColor);
  if (configState.enableOpenHours) {
    baseColor = parseHexColor(open ? configState.openColor : configState.closedColor);
  }
  showClock(nowLocal, baseColor);
}

// --------- Web API ---------
void handleConfigGet() {
  server.send(200, "application/json", buildConfigJson());
}

void handleConfigPost() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  String err;
  if (!applyConfigJson(server.arg("plain"), err)) return sendJsonError(err);
  saveConfig();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleStatus() {
  server.send(200, "application/json", buildStatusJson());
}

void handleUpdate() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String url = doc["url"].as<String>();
  if (url.length() == 0) return sendJsonError("url missing");
  Serial.printf("[OTA] API /update FW: %s\n", url.c_str());
  bool ok = performUpdate(url, false);
  if (!ok) return sendJsonError("update failed");
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleUpdateFs() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  DynamicJsonDocument doc(256);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String url = doc["url"].as<String>();
  if (url.length() == 0) return sendJsonError("url missing");
  Serial.printf("[OTA] API /updatefs FS: %s\n", url.c_str());
  bool ok = updateFsPreserveConfig(url);
  if (!ok) return sendJsonError("update failed");
  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleUpdateBundle() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  DynamicJsonDocument doc(384);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String fwUrl = doc["fwUrl"].as<String>();
  String fsUrl = doc["fsUrl"].as<String>();
  if (fwUrl.length() == 0) return sendJsonError("fwUrl missing");

  Serial.printf("[OTA] API /update_bundle FW: %s\n", fwUrl.c_str());
  if (fsUrl.length() > 0) {
    Serial.printf("[OTA] API /update_bundle FS: %s\n", fsUrl.c_str());
    if (!updateFsPreserveConfig(fsUrl)) return sendJsonError("fs update failed");
  }

  if (!performUpdate(fwUrl, false)) return sendJsonError("fw update failed");

  server.send(200, "application/json", "{\"status\":\"rebooting\"}");
  delay(500);
  ESP.restart();
}

void handleAppointmentsGet() {
  DynamicJsonDocument doc(1024);
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < configState.appointmentCount; ++i) {
    JsonObject o = arr.add<JsonObject>();
    o["time"] = configState.appointments[i].time;
    o["color"] = configState.appointments[i].color;
  }
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleAppointmentsPost() {
  if (server.method() != HTTP_POST) return sendJsonError("POST required");
  DynamicJsonDocument doc(384);
  DeserializationError err = deserializeJson(doc, server.arg("plain"));
  if (err) return sendJsonError("JSON parse error");
  String t = doc["time"].as<String>();
  String c = doc["color"].as<String>();
  if (t.length() == 0) return sendJsonError("time missing");
  if (!addAppointment(t, c)) return sendJsonError("invalid time or full");
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleAppointmentsDelete() {
  if (server.method() != HTTP_DELETE) return sendJsonError("DELETE required");
  DynamicJsonDocument doc(128);
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

String contentTypeForPath(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".ico")) return "image/x-icon";
  return "application/octet-stream";
}

void setupServer() {
  server.on("/api/config", HTTP_GET, handleConfigGet);
  server.on("/api/config", HTTP_POST, handleConfigPost);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/update", HTTP_POST, handleUpdate);
  server.on("/api/updatefs", HTTP_POST, handleUpdateFs);
  server.on("/api/update_bundle", HTTP_POST, handleUpdateBundle);
  server.on("/api/appointments", HTTP_GET, handleAppointmentsGet);
  server.on("/api/appointments", HTTP_POST, handleAppointmentsPost);
  server.on("/api/appointments", HTTP_DELETE, handleAppointmentsDelete);
  server.on("/api/wifi/reset", HTTP_POST, handleWifiReset);

  server.on("/app", [](){
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
      server.send(404, "text/plain", "index.html not found");
      return;
    }
    server.streamFile(f, "text/html");
    f.close();
  });

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
    if (path == "/") path = "/index.html";
    File f;
    if (LittleFS.exists(path)) {
      f = LittleFS.open(path, "r");
      if (f) {
        server.streamFile(f, contentTypeForPath(path));
        f.close();
        return;
      }
    }
    f = LittleFS.open("/index.html", "r");
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
  static WiFiManager wm;
  wmPortal = &wm;
  std::vector<const char*> menu = {"wifi", "info", "custom", "exit"};
  wm.setMenu(menu);
  wm.setConfigPortalBlocking(false);
  wm.setCustomMenuHTML(
    "<li class=\"menu-item\">"
    "<a href=\"/app\" style=\"display:block;padding:10px 12px;margin:6px 0;"
    "background:#2563eb;color:#fff;border-radius:6px;text-decoration:none;font-weight:600;\">"
    "LED Panel Konfiguration öffnen</a>"
    "</li>"
  );
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

    wm.server->on("/api/config", HTTP_GET, [&wm]() {
      wm.server->send(200, "application/json", buildConfigJson());
    });

    wm.server->on("/api/config", HTTP_POST, [&wm]() {
      String err;
      if (!applyConfigJson(wm.server->arg("plain"), err)) return sendJsonErrorTo(*wm.server, err);
      saveConfig();
      wm.server->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    wm.server->on("/api/status", HTTP_GET, [&wm]() {
      wm.server->send(200, "application/json", buildStatusJson());
    });

    wm.server->on("/api/appointments", HTTP_GET, [&wm]() {
      DynamicJsonDocument doc(1024);
      JsonArray arr = doc.to<JsonArray>();
      for (int i = 0; i < configState.appointmentCount; ++i) {
        JsonObject o = arr.add<JsonObject>();
        o["time"] = configState.appointments[i].time;
        o["color"] = configState.appointments[i].color;
      }
      String out;
      serializeJson(doc, out);
      wm.server->send(200, "application/json", out);
    });

    wm.server->on("/api/appointments", HTTP_POST, [&wm]() {
      DynamicJsonDocument doc(384);
      DeserializationError err = deserializeJson(doc, wm.server->arg("plain"));
      if (err) return sendJsonErrorTo(*wm.server, "JSON parse error");
      String t = doc["time"].as<String>();
      String c = doc["color"].as<String>();
      if (t.length() == 0) return sendJsonErrorTo(*wm.server, "time missing");
      if (!addAppointment(t, c)) return sendJsonErrorTo(*wm.server, "invalid time or full");
      wm.server->send(200, "application/json", "{\"status\":\"ok\"}");
    });

    wm.server->on("/api/appointments", HTTP_DELETE, [&wm]() {
      DynamicJsonDocument doc(128);
      DeserializationError err = deserializeJson(doc, wm.server->arg("plain"));
      if (err) return sendJsonErrorTo(*wm.server, "JSON parse error");
      int idx = doc["index"] | -1;
      if (!deleteAppointment(idx)) return sendJsonErrorTo(*wm.server, "invalid index");
      wm.server->send(200, "application/json", "{\"status\":\"ok\"}");
    });
  });
  wm.setConfigPortalTimeout(0); // no auto-timeout; stay in portal until connected
  bool connected = wm.autoConnect("Agentur-für-Felix");
  if (!connected) {
    portalActive = true;
    Serial.println("Config portal active, non-blocking mode");
  } else {
    portalActive = false;
    Serial.print("Connected: ");
    Serial.println(WiFi.localIP());
    configTzTime(configState.tz.c_str(), "pool.ntp.org");
    lastNtpSync = millis();
    tzInitialized = true;
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  }

  loadConfig();

  FastLED.addLeds<NEOPIXEL, LED_PIN>(leds, DEFAULT_LED_COUNT);
  FastLED.setBrightness(configState.brightness);

  setupWifiAndTime();
  setupServer();
}

void loop() {
  if (wmPortal && portalActive) {
    wmPortal->process();
  }

  server.handleClient();

  // In AP/portal mode default to the configured effect for a simple visual indicator
  if (portalActive) {
    FastLED.setBrightness(configState.brightness);
    showEffect();
    delay(30);
    return;
  }

  time_t nowLocal = time(nullptr);
  if (tzInitialized && millis() - lastNtpSync > 6UL * 60UL * 60UL * 1000UL) {
    configTzTime(configState.tz.c_str(), "pool.ntp.org");
    lastNtpSync = millis();
  }
  fetchIcalIfNeeded();
  handleLeds(nowLocal);
  delay(30);
}
