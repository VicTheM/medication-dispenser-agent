/*
  MedAdhereMainESP.ino - ESP32-S3 main controller firmware.
  See DEVICE_BRIEF.md for the full operating guide and design rationale.
*/
#include "config.h"
#include "types.h"
#include "storage.h"
#include "display.h"
#include "indicators.h"
#include "sensors.h"
#include "camlink.h"
#include "netlink.h"
#include "webportal.h"
#include "button.h"
#include <ArduinoJson.h>
#include <time.h>

// ---- Globals ----
static DeviceState s_state = DeviceState::BOOT;
static DeviceCredentials s_creds;
static CompartmentSlot s_schedule[NUM_COMPARTMENTS];
static String s_timezone = "UTC";

static uint8_t s_activeCompartment = 255; // 255 = none pending
static unsigned long s_stateEnteredAt = 0;
static unsigned long s_lastTelemetryAt = 0;
static float s_batteryPct = 100.0f;
static String s_lastDispenseEventId = "";

static CachedDispenseEvent s_offlineEvents[OFFLINE_CACHE_MAX_EVENTS];
static uint8_t s_offlineEventCount = 0;
static CachedTelemetry s_offlineTelemetry[OFFLINE_CACHE_MAX_TELEMETRY];
static uint8_t s_offlineTelemetryCount = 0;

// ---- Forward declarations ----
void enterState(DeviceState s);
void onScheduleUpdate(CompartmentSlot slots[NUM_COMPARTMENTS], const String &tz);
void onRemoteCommand(const String &type, const String &commandId, const String &payloadJson);
void reportDispense(uint8_t compIdx, const char *status, bool wasOffline);
void sendTelemetryNow();
void findNextDue(int &outIdx, unsigned long &outSecondsUntil);
void runVoiceQuery();
String compartmentLetter(uint8_t idx);
bool isScheduleDueNow(const CompartmentSlot &slot);

// =======================================================================
// Setup
// =======================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nMedAdhere main controller booting...");

  storageInit();
  displayInit();
  indicatorsInit();
  buttonInit();
  ultrasonicInit();
  beamInit();
  scaleInit();
  carouselInit();
  camlinkInit();

  displayMessage("MedAdhere", "Booting...");

  s_creds = storageLoad();

  if (!s_creds.valid) {
    enterState(DeviceState::CONFIG_PORTAL);
    return;
  }

  enterState(DeviceState::WIFI_CONNECTING);
}

// =======================================================================
// Main loop - dispatches to the current state's tick logic. Sensor/motor/
// display/indicator ticks run every iteration regardless of state so they
// never block or get starved.
// =======================================================================
void loop() {
  carouselTick();
  indicatorsTick();
  displayIdleTick();
  if (wifiIsConnected()) wsLoop();

  ButtonEvent btn = buttonTick();

  switch (s_state) {
    case DeviceState::BOOT:
      break;

    case DeviceState::WIFI_CONNECTING: {
      displayMessage("Connecting", "to Wi-Fi...");
      bool ok = wifiConnect(s_creds.wifiSsid, s_creds.wifiPass, WIFI_CONNECT_TIMEOUT_MS);
      if (!ok) {
        displayMessage("Wi-Fi failed", "Hold btn=setup");
        indicatorsSetPattern(IndicatorPattern::OFFLINE);
        delay(3000);
        enterState(DeviceState::WIFI_CONNECTING); // keep retrying
        break;
      }
      configTime(0, 0, "pool.ntp.org", "time.nist.gov");
      networkSetIdentity(s_creds.apiBase, s_creds.deviceUid, s_creds.deviceSecret);

      String tz;
      if (fetchSchedule(s_schedule, tz)) {
        s_timezone = tz;
      }
      wsBegin(onScheduleUpdate, onRemoteCommand);
      enterState(DeviceState::NORMAL);
      break;
    }

    case DeviceState::CONFIG_PORTAL: {
      if (!webportalIsActive()) {
        webportalStart();
        indicatorsSetPattern(IndicatorPattern::CONFIG_MODE);
        displayMessage("Setup mode", "Connect to AP");
      }
      webportalLoop();
      if (!webportalIsActive()) {
        // idle-timed-out out of config mode - fall back to whatever creds we have
        s_creds = storageLoad();
        enterState(s_creds.valid ? DeviceState::WIFI_CONNECTING : DeviceState::CONFIG_PORTAL);
      }
      break;
    }

    case DeviceState::NORMAL: {
      if (!wifiIsConnected()) {
        indicatorsSetPattern(IndicatorPattern::OFFLINE);
      } else {
        indicatorsSetPattern(wsIsConnected() ? IndicatorPattern::IDLE_OK : IndicatorPattern::OFFLINE);
      }

      // periodic telemetry
      if (millis() - s_lastTelemetryAt > TELEMETRY_INTERVAL_MS) {
        sendTelemetryNow();
        s_lastTelemetryAt = millis();
      }

      // idle screen: time to next dose
      int nextIdx; unsigned long secsUntil;
      findNextDue(nextIdx, secsUntil);
      if (nextIdx >= 0) {
        unsigned long mins = secsUntil / 60;
        String line1 = "Next @ " + String(s_schedule[nextIdx].dispenseTime);
        String line2 = String(s_schedule[nextIdx].medicationNames);
        displayShowIdle(line1, line2);

        if (secsUntil == 0) {
          s_activeCompartment = nextIdx;
          enterState(DeviceState::ALERTING);
        }
      } else {
        displayShowIdle("MedAdhere", "No doses set");
      }

      // config button
      if (btn == ButtonEvent::SHORT_PRESS) {
        enterState(DeviceState::CONFIG_PORTAL);
      } else if (btn == ButtonEvent::LONG_PRESS) {
        enterState(DeviceState::VOICE_QUERY);
      }
      break;
    }

    case DeviceState::ALERTING: {
      float dist = ultrasonicReadCM();
      bool approached = (dist > 0 && dist <= APPROACH_RANGE_CM);

      if (approached) {
        indicatorsSetPattern(IndicatorPattern::PERSON_APPROACHED); // "changes the sound"
        displayMessage("Welcome!", "Dispensing...");
        delay(600); // brief, deliberate pause so the tone-change is perceptible before dispensing
        enterState(DeviceState::DISPENSING);
        break;
      }

      indicatorsSetPattern(IndicatorPattern::WAITING_APPROACH);
      String label = "Compartment " + compartmentLetter(s_activeCompartment);
      displayShowIdle("Time for meds!", label);

      if (millis() - s_stateEnteredAt > ALERT_MAX_WAIT_MS) {
        Serial.println("[state] gave up waiting for approach");
        reportDispense(s_activeCompartment, "skipped", !wifiIsConnected());
        enterState(DeviceState::NORMAL);
      }
      break;
    }

    case DeviceState::DISPENSING: {
      if (!carouselIsMoving() && millis() - s_stateEnteredAt < 50) {
        carouselGoTo(s_activeCompartment);
      }
      indicatorsSetPattern(IndicatorPattern::DISPENSING);
      displayMessage("Dispensing", compartmentLetter(s_activeCompartment));

      if (!carouselIsMoving() && millis() - s_stateEnteredAt > 300) {
        reportDispense(s_activeCompartment, "success", !wifiIsConnected());
        camSendControl('V', ADHERENCE_VIDEO_MS);
        enterState(DeviceState::MONITOR_PICKUP);
      }
      break;
    }

    case DeviceState::MONITOR_PICKUP: {
      static float baselineWeight = 0;
      static bool baselineTaken = false;
      static bool doorOpened = false;

      if (!baselineTaken) {
        baselineWeight = scaleReadGrams();
        baselineTaken = true;
        doorOpened = false;
      }

      if (beamObstacleDetected() == false) doorOpened = true; // door out of the way at least once

      float now_g = scaleReadGrams();
      bool weightChanged = fabs(now_g - baselineWeight) >= TRAY_PICKUP_DELTA_G;
      bool pickedUp = doorOpened && weightChanged;

      if (pickedUp) {
        indicatorsSetPattern(IndicatorPattern::PICKUP_OK);
        displayMessage("Great job!", "Medication taken");
        baselineTaken = false;
        enterState(DeviceState::REPORTING);
        break;
      }

      if (millis() - s_stateEnteredAt > PICKUP_MONITOR_MS) {
        indicatorsSetPattern(IndicatorPattern::PICKUP_MISSED);
        displayMessage("Not picked up", "Check on patient");
        baselineTaken = false;
        enterState(DeviceState::REPORTING);
      }
      break;
    }

    case DeviceState::REPORTING: {
      // Wait for the adherence clip the CAM board started recording back in
      // DISPENSING, then relay it straight through to the backend.
      char type, subType;
      uint32_t len;
      displayMessage("Uploading", "adherence clip");

      if (camWaitForFrameHeader(type, subType, len, ADHERENCE_VIDEO_MS + 20000UL)) {
        if (type == 'D' && subType == 'V' && len > 0) {
          uploadAdherenceVideoFromSerial2(s_lastDispenseEventId, len, ADHERENCE_VIDEO_MS / 1000);
        }
      } else {
        Serial.println("[state] no video frame arrived from CAM board in time");
      }

      enterState(DeviceState::NORMAL);
      break;
    }

    case DeviceState::VOICE_QUERY: {
      runVoiceQuery();
      enterState(DeviceState::NORMAL);
      break;
    }
  }
}

// =======================================================================
// Helpers
// =======================================================================
void enterState(DeviceState s) {
  s_state = s;
  s_stateEnteredAt = millis();
}

String compartmentLetter(uint8_t idx) {
  if (idx >= NUM_COMPARTMENTS) return "?";
  char letters[] = "ABCDEFG";
  return String(letters[idx]);
}

bool isScheduleDueNow(const CompartmentSlot &slot) {
  if (!slot.active || slot.dispensedToday) return false;
  if (strcmp(slot.frequency, "as_needed") == 0) return false;

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);

  if (strcmp(slot.frequency, "specific_days") == 0) {
    int wd = (tmNow.tm_wday == 0) ? 6 : tmNow.tm_wday - 1; // convert to mon=0..sun=6
    if (!(slot.daysOfWeekMask & (1 << wd))) return false;
  }

  char nowHHMM[6];
  snprintf(nowHHMM, sizeof(nowHHMM), "%02d:%02d", tmNow.tm_hour, tmNow.tm_min);
  return strcmp(nowHHMM, slot.dispenseTime) == 0;
}

void findNextDue(int &outIdx, unsigned long &outSecondsUntil) {
  outIdx = -1;
  outSecondsUntil = 0;

  time_t now = time(nullptr);
  struct tm tmNow;
  localtime_r(&now, &tmNow);
  int nowMinutes = tmNow.tm_hour * 60 + tmNow.tm_min;

  int bestIdx = -1;
  int bestDelta = 24 * 60 + 1;

  for (int i = 0; i < NUM_COMPARTMENTS; i++) {
    if (!s_schedule[i].active || strcmp(s_schedule[i].frequency, "as_needed") == 0) continue;

    if (isScheduleDueNow(s_schedule[i])) {
      s_schedule[i].dispensedToday = true; // will be reported by reportDispense() below
      outIdx = i;
      outSecondsUntil = 0;
      return;
    }

    int h, m;
    sscanf(s_schedule[i].dispenseTime, "%d:%d", &h, &m);
    int slotMinutes = h * 60 + m;
    int delta = slotMinutes - nowMinutes;
    if (delta < 0) delta += 24 * 60;
    if (delta < bestDelta) {
      bestDelta = delta;
      bestIdx = i;
    }
  }

  if (bestIdx >= 0) {
    outIdx = bestIdx;
    outSecondsUntil = (unsigned long)bestDelta * 60UL;
    if (outSecondsUntil == 0) outSecondsUntil = 1; // avoid re-triggering same-second
  }
}

void reportDispense(uint8_t compIdx, const char *status, bool wasOffline) {
  time_t now = time(nullptr);
  char letter = compartmentLetter(compIdx)[0];

  if (wasOffline) {
    if (s_offlineEventCount < OFFLINE_CACHE_MAX_EVENTS) {
      CachedDispenseEvent &e = s_offlineEvents[s_offlineEventCount++];
      e.compartment = letter;
      strlcpy(e.status, status, sizeof(e.status));
      strlcpy(e.scheduledTime, s_schedule[compIdx].dispenseTime, sizeof(e.scheduledTime));
      e.dispensedAt = now;
    }
    return;
  }

  String eventId;
  bool ok = postDispenseEvent(letter, status, s_schedule[compIdx].dispenseTime, now, false, eventId);
  if (ok) {
    s_lastDispenseEventId = eventId;
  } else {
    // fall back to the offline cache so it isn't lost
    if (s_offlineEventCount < OFFLINE_CACHE_MAX_EVENTS) {
      CachedDispenseEvent &e = s_offlineEvents[s_offlineEventCount++];
      e.compartment = letter;
      strlcpy(e.status, status, sizeof(e.status));
      strlcpy(e.scheduledTime, s_schedule[compIdx].dispenseTime, sizeof(e.scheduledTime));
      e.dispensedAt = now;
    }
  }
}

void sendTelemetryNow() {
  const char *tray = "full"; // TODO: derive from a dedicated fill-level sensor if added
  bool person = ultrasonicReadCM() > 0 && ultrasonicReadCM() <= APPROACH_RANGE_CM;
  uint32_t uptime = millis() / 1000;

  if (!wifiIsConnected()) {
    if (s_offlineTelemetryCount < OFFLINE_CACHE_MAX_TELEMETRY) {
      CachedTelemetry &t = s_offlineTelemetry[s_offlineTelemetryCount++];
      t.reportedAt = time(nullptr);
      t.batteryLevel = s_batteryPct;
      strlcpy(t.trayState, tray, sizeof(t.trayState));
      t.personDetected = person;
      t.wifiRssi = 0;
      t.uptimeSeconds = uptime;
    }
    return;
  }

  bool ok = postTelemetry(s_batteryPct, tray, person, uptime);
  if (ok && s_offlineEventCount + s_offlineTelemetryCount > 0) {
    postSyncOfflineBatch(s_offlineEvents, s_offlineEventCount, s_offlineTelemetry, s_offlineTelemetryCount);
    s_offlineEventCount = 0;
    s_offlineTelemetryCount = 0;
  }
}

void onScheduleUpdate(CompartmentSlot slots[NUM_COMPARTMENTS], const String &tz) {
  for (int i = 0; i < NUM_COMPARTMENTS; i++) s_schedule[i] = slots[i];
  s_timezone = tz;
  Serial.println("[state] schedule updated");
}

void onRemoteCommand(const String &type, const String &commandId, const String &payloadJson) {
  Serial.printf("[state] remote command: %s\n", type.c_str());

  if (type == "manual_dispense") {
    StaticJsonDocument<128> doc;
    deserializeJson(doc, payloadJson);
    const char *compStr = doc["compartment"] | "A";
    uint8_t idx = compStr[0] - 'A';
    if (idx < NUM_COMPARTMENTS) {
      s_activeCompartment = idx;
      enterState(DeviceState::DISPENSING);
    }
  } else if (type == "restart") {
    ESP.restart();
  } else if (type == "sync") {
    sendTelemetryNow(); // opportunistically flushes the offline cache too
  } else if (type == "configure") {
    // reserved for future device-side settings pushed from the backend
  }
}

void runVoiceQuery() {
  displayMessage("Listening...", "Ask your question");
  camSendControl('L', VOICE_MAX_RECORD_MS);

  char type, subType;
  uint32_t len;
  if (!camWaitForFrameHeader(type, subType, len, VOICE_MAX_RECORD_MS + 5000UL) || type != 'D' || subType != 'A') {
    displayMessage("No question", "heard - sorry");
    indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
    delay(1500);
    return;
  }

  uint8_t *audioBuf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
  if (!audioBuf) audioBuf = (uint8_t *)malloc(len);
  if (!audioBuf) {
    displayMessage("Out of memory", "try again");
    return;
  }

  size_t got = 0;
  unsigned long start = millis();
  while (got < len && millis() - start < 15000) {
    if (Serial2.available()) {
      got += Serial2.readBytes(audioBuf + got, len - got);
    } else {
      delay(1);
    }
  }
  camConsumeTrailingChecksum();

  displayMessage("Thinking...", "");
  String transcript, answer, interactionId, audioFormat;
  bool ok = postVoiceQuery(audioBuf, got, "wav", transcript, answer, interactionId, audioFormat);
  free(audioBuf);

  if (!ok) {
    displayMessage("Couldn't reach", "the assistant");
    indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
    delay(1500);
    return;
  }

  displayMessage("Got answer:", answer.substring(0, LCD_COLS));

  uint8_t *respBuf; uint32_t respLen;
  if (downloadVoiceAudio(interactionId, &respBuf, &respLen)) {
    camSendPlaybackAudio(respBuf, respLen);
    free(respBuf);
  }
  delay(2000);
}
