#include "netlink.h"
#include "config.h"
#include "camlink.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>

// =======================================================================
// WiFi
// =======================================================================
bool wifiConnect(const String &ssid, const String &pass, unsigned long timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(100);
  }
  return WiFi.status() == WL_CONNECTED;
}

bool wifiIsConnected() {
  return WiFi.status() == WL_CONNECTED;
}

// =======================================================================
// Identity / URL parsing
// =======================================================================
static String s_apiBase;
static String s_deviceUid;
static String s_deviceSecret;

struct ParsedUrl {
  bool isHttps;
  String host;
  int port;
  String path;
};

void networkSetIdentity(const String &apiBase, const String &deviceUid, const String &deviceSecret) {
  s_apiBase = apiBase;
  s_deviceUid = deviceUid;
  s_deviceSecret = deviceSecret;
}

static bool parseUrl(const String &url, ParsedUrl &out) {
  String rest;
  if (url.startsWith("https://")) {
    out.isHttps = true;
    out.port = 443;
    rest = url.substring(8);
  } else if (url.startsWith("http://")) {
    out.isHttps = false;
    out.port = 80;
    rest = url.substring(7);
  } else {
    return false;
  }

  int slashIdx = rest.indexOf('/');
  String hostPort = (slashIdx == -1) ? rest : rest.substring(0, slashIdx);
  out.path = (slashIdx == -1) ? "/" : rest.substring(slashIdx);

  int colonIdx = hostPort.indexOf(':');
  if (colonIdx != -1) {
    out.host = hostPort.substring(0, colonIdx);
    out.port = hostPort.substring(colonIdx + 1).toInt();
  } else {
    out.host = hostPort;
  }
  return true;
}

// =======================================================================
// WebSocket control channel
// =======================================================================
static WebSocketsClient s_ws;
static ScheduleUpdateCallback s_scheduleCb = nullptr;
static RemoteCommandCallback s_commandCb = nullptr;

static void parseScheduleJson(JsonObject compartments, CompartmentSlot slots[NUM_COMPARTMENTS], String &timezoneOut) {
  const char *letters = "ABCDEFG";
  for (int i = 0; i < NUM_COMPARTMENTS; i++) {
    char letter[2] = {letters[i], 0};
    slots[i] = CompartmentSlot(); // reset to inactive/defaults
    if (!compartments[letter].is<JsonObject>()) continue;

    JsonObject entry = compartments[letter];
    slots[i].active = true;
    strlcpy(slots[i].scheduleId, entry["schedule_id"] | "", sizeof(slots[i].scheduleId));
    strlcpy(slots[i].dispenseTime, entry["dispense_time"] | "", sizeof(slots[i].dispenseTime));
    strlcpy(slots[i].frequency, entry["frequency"] | "daily", sizeof(slots[i].frequency));

    slots[i].daysOfWeekMask = 0;
    if (entry["days_of_week"].is<JsonArray>()) {
      static const char *dayCodes[7] = {"mon", "tue", "wed", "thu", "fri", "sat", "sun"};
      for (JsonVariant d : entry["days_of_week"].as<JsonArray>()) {
        const char *dayStr = d.as<const char *>();
        for (int di = 0; di < 7; di++) {
          if (dayStr && strcmp(dayStr, dayCodes[di]) == 0) slots[i].daysOfWeekMask |= (1 << di);
        }
      }
    }

    String meds = "";
    if (entry["medications"].is<JsonArray>()) {
      for (JsonVariant m : entry["medications"].as<JsonArray>()) {
        if (meds.length() > 0) meds += ", ";
        meds += (const char *)(m["name"] | "");
      }
    }
    strlcpy(slots[i].medicationNames, meds.c_str(), sizeof(slots[i].medicationNames));
  }
  timezoneOut = String((const char *)(compartments["__tz__"] | ""));
}

static void handleWsMessage(const String &msg) {
  StaticJsonDocument<3072> doc;
  DeserializationError err = deserializeJson(doc, msg);
  if (err) {
    Serial.printf("[ws] JSON parse error: %s\n", err.c_str());
    return;
  }

  const char *type = doc["type"] | "";
  String commandId = doc["command_id"] | "";

  if (strcmp(type, "update_schedule") == 0) {
    CompartmentSlot slots[NUM_COMPARTMENTS];
    JsonObject compartments = doc["compartments"];
    String tz = doc["timezone"] | "UTC";
    if (s_scheduleCb) {
      parseScheduleJson(compartments, slots, tz);
      s_scheduleCb(slots, tz);
    }
  } else if (s_commandCb) {
    String payloadJson;
    if (doc["payload"].is<JsonObject>()) {
      serializeJson(doc["payload"], payloadJson);
    }
    s_commandCb(String(type), commandId, payloadJson);
  }

  if (commandId.length() > 0) {
    wsSendAck(commandId);
  }
}

static void onWsEvent(WStype_t type, uint8_t *payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[ws] connected");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[ws] disconnected");
      break;
    case WStype_TEXT: {
      String msg((char *)payload, length);
      handleWsMessage(msg);
      break;
    }
    default:
      break;
  }
}

void wsBegin(ScheduleUpdateCallback onSchedule, RemoteCommandCallback onCommand) {
  s_scheduleCb = onSchedule;
  s_commandCb = onCommand;

  ParsedUrl u;
  parseUrl(s_apiBase, u);
  String path = "/devices/ws?device_uid=" + s_deviceUid + "&secret=" + s_deviceSecret;

  if (u.isHttps) {
    s_ws.beginSSL(u.host.c_str(), u.port, path.c_str());
  } else {
    s_ws.begin(u.host.c_str(), u.port, path.c_str());
  }
  s_ws.onEvent(onWsEvent);
  s_ws.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
}

void wsLoop() {
  s_ws.loop();
}

bool wsIsConnected() {
  return s_ws.isConnected();
}

void wsSendAck(const String &commandId) {
  StaticJsonDocument<128> doc;
  doc["type"] = "ack";
  doc["command_id"] = commandId;
  String out;
  serializeJson(doc, out);
  s_ws.sendTXT(out);
}

// =======================================================================
// Simple JSON REST calls (schedule fetch, dispense event, telemetry, sync)
// =======================================================================
static bool httpJsonRequest(const String &method, const String &path, const String &jsonBody,
                            int &outStatus, String &outBody) {
  ParsedUrl u;
  if (!parseUrl(s_apiBase, u)) return false;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  bool began;

  if (u.isHttps) {
    secureClient.setInsecure(); // see DEVICE_BRIEF.md "TLS trust model" for the tradeoff + hardening path
    began = http.begin(secureClient, u.host, u.port, u.path + path, true);
  } else {
    began = http.begin(plainClient, u.host, u.port, u.path + path);
  }
  if (!began) return false;

  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", s_deviceUid);
  http.addHeader("X-Device-Secret", s_deviceSecret);
  http.setTimeout(15000);

  if (method == "GET") {
    outStatus = http.GET();
  } else {
    outStatus = http.sendRequest(method.c_str(), jsonBody);
  }
  outBody = http.getString();
  http.end();
  return outStatus > 0;
}

bool fetchSchedule(CompartmentSlot slots[NUM_COMPARTMENTS], String &timezoneOut) {
  int status;
  String body;
  if (!httpJsonRequest("GET", "/devices/schedule", "", status, body) || status != 200) {
    Serial.printf("[net] fetchSchedule failed, status=%d\n", status);
    return false;
  }

  StaticJsonDocument<3072> doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject compartments = doc["compartments"];
  timezoneOut = (const char *)(doc["timezone"] | "UTC");
  parseScheduleJson(compartments, slots, timezoneOut);
  return true;
}

bool postDispenseEvent(char compartment, const char *status_, const char *scheduledTimeOrNull,
                        time_t dispensedAtEpoch, bool wasOfflineCached, String &outEventId) {
  StaticJsonDocument<384> doc;
  char compStr[2] = {compartment, 0};
  doc["compartment"] = compStr;
  doc["status"] = status_;
  if (scheduledTimeOrNull) doc["scheduled_time"] = scheduledTimeOrNull;

  // ISO8601 UTC timestamp from epoch seconds
  struct tm tmVal;
  gmtime_r(&dispensedAtEpoch, &tmVal);
  char isoBuf[32];
  strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &tmVal);
  doc["dispensed_at"] = isoBuf;
  doc["was_offline_cached"] = wasOfflineCached;

  String jsonBody;
  serializeJson(doc, jsonBody);

  int status;
  String respBody;
  if (!httpJsonRequest("POST", "/devices/dispense-event", jsonBody, status, respBody) || status != 201) {
    Serial.printf("[net] postDispenseEvent failed, status=%d body=%s\n", status, respBody.c_str());
    return false;
  }

  StaticJsonDocument<512> respDoc;
  if (!deserializeJson(respDoc, respBody)) {
    outEventId = (const char *)(respDoc["id"] | "");
  }
  return true;
}

bool postTelemetry(float batteryPct, const char *trayState, bool personDetected, uint32_t uptimeSeconds) {
  StaticJsonDocument<256> doc;
  doc["battery_level"] = batteryPct;
  doc["tray_state"] = trayState;
  doc["person_detected"] = personDetected;
  doc["uptime_seconds"] = uptimeSeconds;
  doc["wifi_rssi"] = WiFi.RSSI();

  String jsonBody;
  serializeJson(doc, jsonBody);

  int status;
  String respBody;
  bool ok = httpJsonRequest("POST", "/devices/telemetry", jsonBody, status, respBody) && status == 201;
  if (!ok) Serial.printf("[net] postTelemetry failed, status=%d\n", status);
  return ok;
}

bool postSyncOfflineBatch(CachedDispenseEvent *events, uint8_t eventCount,
                           CachedTelemetry *telemetry, uint8_t telemetryCount) {
  StaticJsonDocument<4096> doc;
  JsonArray evArr = doc.createNestedArray("dispense_events");
  for (uint8_t i = 0; i < eventCount; i++) {
    JsonObject e = evArr.createNestedObject();
    char compStr[2] = {events[i].compartment, 0};
    e["compartment"] = compStr;
    e["status"] = events[i].status;
    e["scheduled_time"] = events[i].scheduledTime;
    struct tm tmVal;
    gmtime_r(&events[i].dispensedAt, &tmVal);
    char isoBuf[32];
    strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%dT%H:%M:%S", &tmVal);
    e["dispensed_at"] = isoBuf;
    e["was_offline_cached"] = true;
  }

  JsonArray telArr = doc.createNestedArray("telemetry");
  for (uint8_t i = 0; i < telemetryCount; i++) {
    JsonObject t = telArr.createNestedObject();
    t["battery_level"] = telemetry[i].batteryLevel;
    t["tray_state"] = telemetry[i].trayState;
    t["person_detected"] = telemetry[i].personDetected;
    t["wifi_rssi"] = telemetry[i].wifiRssi;
    t["uptime_seconds"] = telemetry[i].uptimeSeconds;
  }

  String jsonBody;
  serializeJson(doc, jsonBody);

  int status;
  String respBody;
  bool ok = httpJsonRequest("POST", "/devices/sync-offline", jsonBody, status, respBody) && status == 201;
  if (!ok) Serial.printf("[net] postSyncOfflineBatch failed, status=%d\n", status);
  return ok;
}

// =======================================================================
// Raw multipart upload helper (hand-rolled - Arduino's HTTPClient has no
// built-in multipart support). Used for adherence video + voice audio.
// =======================================================================
static const char *BOUNDARY = "----MedAdhereBoundary7F3A9";

static bool rawMultipartUpload(const String &path,
                                const char *fieldNames[], const char *fieldValues[], int fieldCount,
                                const char *fileFieldName, const char *fileName, const char *fileContentType,
                                uint32_t fileLen, bool streamFromSerial2, const uint8_t *ramBuffer,
                                int &outStatus, String &outBody) {
  ParsedUrl u;
  if (!parseUrl(s_apiBase, u)) return false;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  Client *client;
  if (u.isHttps) {
    secureClient.setInsecure();
    client = &secureClient;
  } else {
    client = &plainClient;
  }

  client->setTimeout(20000);
  if (!client->connect(u.host.c_str(), u.port)) {
    Serial.println("[net] multipart connect failed");
    return false;
  }

  // ---- Build preamble (all simple fields + file field header) ----
  String preamble;
  for (int i = 0; i < fieldCount; i++) {
    preamble += "--"; preamble += BOUNDARY; preamble += "\r\n";
    preamble += "Content-Disposition: form-data; name=\""; preamble += fieldNames[i]; preamble += "\"\r\n\r\n";
    preamble += fieldValues[i]; preamble += "\r\n";
  }
  preamble += "--"; preamble += BOUNDARY; preamble += "\r\n";
  preamble += "Content-Disposition: form-data; name=\""; preamble += fileFieldName;
  preamble += "\"; filename=\""; preamble += fileName; preamble += "\"\r\n";
  preamble += "Content-Type: "; preamble += fileContentType; preamble += "\r\n\r\n";

  String closing = "\r\n--"; closing += BOUNDARY; closing += "--\r\n";

  uint32_t contentLength = preamble.length() + fileLen + closing.length();

  // ---- Request line + headers ----
  String request;
  request += "POST "; request += (u.path + path); request += " HTTP/1.1\r\n";
  request += "Host: "; request += u.host; request += "\r\n";
  request += "X-Device-Id: "; request += s_deviceUid; request += "\r\n";
  request += "X-Device-Secret: "; request += s_deviceSecret; request += "\r\n";
  request += "Content-Type: multipart/form-data; boundary="; request += BOUNDARY; request += "\r\n";
  request += "Content-Length: "; request += String(contentLength); request += "\r\n";
  request += "Connection: close\r\n\r\n";

  client->print(request);
  client->print(preamble);

  // ---- Stream the file content ----
  if (streamFromSerial2) {
    uint32_t remaining = fileLen;
    uint8_t chunk[512];
    unsigned long lastByteAt = millis();
    while (remaining > 0) {
      int avail = Serial2.available();
      if (avail <= 0) {
        if (millis() - lastByteAt > 30000) {
          Serial.println("[net] multipart: Serial2 stalled mid-transfer");
          client->stop();
          return false;
        }
        delay(1);
        continue;
      }
      int toRead = min((uint32_t)avail, (uint32_t)sizeof(chunk));
      toRead = min((uint32_t)toRead, remaining);
      int got = Serial2.readBytes(chunk, toRead);
      if (got > 0) {
        client->write(chunk, got);
        remaining -= got;
        lastByteAt = millis();
      }
    }
    camConsumeTrailingChecksum();
  } else {
    client->write(ramBuffer, fileLen);
  }

  client->print(closing);

  // ---- Read response ----
  unsigned long start = millis();
  while (client->connected() && !client->available() && millis() - start < 20000) {
    delay(5);
  }

  String statusLine = client->readStringUntil('\n');
  outStatus = 0;
  int firstSpace = statusLine.indexOf(' ');
  if (firstSpace != -1) outStatus = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();

  // skip headers
  String line;
  do {
    line = client->readStringUntil('\n');
  } while (line.length() > 1 && client->connected());

  outBody = "";
  while (client->available()) {
    outBody += (char)client->read();
  }
  client->stop();
  return outStatus > 0;
}

bool uploadAdherenceVideoFromSerial2(const String &dispenseEventId, uint32_t fileLen, uint16_t durationSeconds) {
  const char *names[] = {"dispense_event_id", "duration_seconds"};
  String durStr = String(durationSeconds);
  const char *values[] = {dispenseEventId.c_str(), durStr.c_str()};

  int status;
  String body;
  bool ok = rawMultipartUpload("/devices/adherence-video", names, values, 2,
                                "video", "adherence.mp4", "video/mp4",
                                fileLen, true, nullptr, status, body);
  if (!ok || status != 201) {
    Serial.printf("[net] uploadAdherenceVideo failed, status=%d body=%s\n", status, body.c_str());
    return false;
  }
  return true;
}

bool postVoiceQuery(const uint8_t *audioBuf, uint32_t audioLen, const char *audioFormat,
                     String &outTranscript, String &outAnswerText, String &outInteractionId, String &outAudioFormat) {
  const char *names[] = {"audio_format"};
  const char *values[] = {audioFormat};

  String fileName = String("query.") + audioFormat;
  int status;
  String body;
  bool ok = rawMultipartUpload("/devices/voice-query", names, values, 1,
                                "audio", fileName.c_str(), "application/octet-stream",
                                audioLen, false, audioBuf, status, body);
  if (!ok || status != 201) {
    Serial.printf("[net] postVoiceQuery failed, status=%d body=%s\n", status, body.c_str());
    return false;
  }

  StaticJsonDocument<2048> doc;
  if (deserializeJson(doc, body)) return false;
  outTranscript = (const char *)(doc["transcript"] | "");
  outAnswerText = (const char *)(doc["answer_text"] | "");
  outInteractionId = (const char *)(doc["id"] | "");
  outAudioFormat = (const char *)(doc["audio_format"] | "wav");
  return true;
}

bool downloadVoiceAudio(const String &interactionId, uint8_t **outBuf, uint32_t *outLen) {
  ParsedUrl u;
  if (!parseUrl(s_apiBase, u)) return false;

  WiFiClientSecure secureClient;
  WiFiClient plainClient;
  HTTPClient http;
  bool began;
  String path = u.path + "/devices/voice-query/" + interactionId + "/audio";

  if (u.isHttps) {
    secureClient.setInsecure();
    began = http.begin(secureClient, u.host, u.port, path, true);
  } else {
    began = http.begin(plainClient, u.host, u.port, path);
  }
  if (!began) return false;

  http.addHeader("X-Device-Id", s_deviceUid);
  http.addHeader("X-Device-Secret", s_deviceSecret);
  int status = http.GET();
  if (status != 200) {
    Serial.printf("[net] downloadVoiceAudio failed, status=%d\n", status);
    http.end();
    return false;
  }

  int len = http.getSize();
  if (len <= 0) { http.end(); return false; }

  uint8_t *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!buf) buf = (uint8_t *)malloc(len); // fall back if PSRAM unavailable/full
  if (!buf) { http.end(); return false; }

  WiFiClient *stream = http.getStreamPtr();
  size_t readTotal = 0;
  unsigned long start = millis();
  while (readTotal < (size_t)len && millis() - start < 20000) {
    if (stream->available()) {
      int got = stream->readBytes(buf + readTotal, len - readTotal);
      readTotal += got;
    } else {
      delay(1);
    }
  }
  http.end();

  *outBuf = buf;
  *outLen = readTotal;
  return readTotal > 0;
}
