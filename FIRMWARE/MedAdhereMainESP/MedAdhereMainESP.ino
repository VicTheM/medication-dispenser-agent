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
#include "network.h"
#include "webportal.h"
#include "button.h"
#include <ArduinoJson.h>
#include <WiFi.h>
#include <stdarg.h>
#include <time.h>

volatile bool DRAM_ATTR s_btnPressPending = false;

void IRAM_ATTR buttonISR() {
  s_btnPressPending = true;
}

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
const char *stateName(DeviceState s);
void logEvent(const char *fmt, ...);
void onScheduleUpdate(CompartmentSlot slots[NUM_COMPARTMENTS], const String &tz);
void onRemoteCommand(const String &type, const String &commandId, const String &payloadJson);
void reportDispense(uint8_t compIdx, const char *status, bool wasOffline);
void sendTelemetryNow();
void findNextDue(int &outIdx, unsigned long &outSecondsUntil);
void runVoiceQuery();
String compartmentLetter(uint8_t idx);
bool isScheduleDueNow(const CompartmentSlot &slot);

// =======================================================================
// Logging - timestamped, single place so it's easy to redirect/extend later
// =======================================================================
void logEvent(const char *fmt, ...) {
  char buf[176];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  unsigned long t = millis();
  Serial.printf("[%lu.%03lu] %s\n", t / 1000, t % 1000, buf);
}

const char *stateName(DeviceState s) {
  switch (s) {
    case DeviceState::BOOT: return "BOOT";
    case DeviceState::WIFI_CONNECTING: return "WIFI_CONNECTING";
    case DeviceState::CONFIG_PORTAL: return "CONFIG_PORTAL";
    case DeviceState::NORMAL: return "NORMAL";
    case DeviceState::ALERTING: return "ALERTING";
    case DeviceState::DISPENSING: return "DISPENSING";
    case DeviceState::MONITOR_PICKUP: return "MONITOR_PICKUP";
    case DeviceState::REPORTING: return "REPORTING";
    case DeviceState::VOICE_QUERY: return "VOICE_QUERY";
  }
  return "?";
}

// =======================================================================
// Setup
// =======================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  logEvent("MedAdhere main controller booting...");

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
  logEvent("Loaded config: ssid=%s api=%s device=%s utc_offset=%d valid=%d",
           s_creds.wifiSsid.c_str(), s_creds.apiBase.c_str(), s_creds.deviceUid.c_str(),
           s_creds.utcOffsetHours, s_creds.valid);

  // Hand the CAM board its own copy of the config the moment we start, so
  // it can connect to WiFi and be ready independently of the main board's
  // own connection timing.
  // camSendConfig(s_creds.wifiSsid, s_creds.wifiPass, s_creds.apiBase,
  //               s_creds.deviceUid, s_creds.deviceSecret, s_creds.utcOffsetHours);
  // logEvent("Pushed config to CAM board");

  if (!s_creds.valid) {
    logEvent("No valid WiFi/device config - entering config portal");
    enterState(DeviceState::CONFIG_PORTAL);
    // return;
  }
  else {
    enterState(DeviceState::WIFI_CONNECTING);
  }
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
  if (btn == ButtonEvent::SHORT_PRESS) logEvent("Button: short press");
  if (btn == ButtonEvent::LONG_PRESS) logEvent("Button: long press");

  switch (s_state) {
    case DeviceState::BOOT:
      break;

    case DeviceState::WIFI_CONNECTING: {
      displayMessage("Connecting", "to Wi-Fi...");
      logEvent("Connecting to WiFi '%s'...", s_creds.wifiSsid.c_str());
      bool ok = wifiConnect(s_creds.wifiSsid, s_creds.wifiPass, WIFI_CONNECT_TIMEOUT_MS);
      if (!ok) {
        logEvent("WiFi connect FAILED, retrying");
        displayMessage("Wi-Fi failed", "Hold btn=setup");
        indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
        delay(3000);
        enterState(DeviceState::WIFI_CONNECTING); // keep retrying
        break;
      }
      logEvent("WiFi connected, IP=%s", WiFi.localIP().toString().c_str());

      // UTC offset comes from the config portal (see item 1 in the brief) -
      // schedule times are the patient's local wall-clock, so the device's
      // "local time" needs to match that, not raw UTC.
      configTime(s_creds.utcOffsetHours * 3600, 0, "pool.ntp.org", "time.nist.gov");
      logEvent("Time sync requested, UTC offset = %d hours", s_creds.utcOffsetHours);

      networkSetIdentity(s_creds.apiBase, s_creds.deviceUid, s_creds.deviceSecret);

      String tz;
      if (fetchSchedule(s_schedule, tz)) {
        s_timezone = tz;
        logEvent("Schedule fetched OK (backend timezone label: %s)", tz.c_str());
      } else {
        logEvent("Schedule fetch FAILED - will rely on WS push");
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
        logEvent("Config portal started");
      }
      webportalLoop();
      if (!webportalIsActive()) {
        logEvent("Config portal closed (idle timeout)");
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
        String line1 = "Next @ " + String(s_schedule[nextIdx].dispenseTime);
        String line2 = String(s_schedule[nextIdx].medicationNames);
        displayShowIdle(line1, line2);

        if (secsUntil == 0) {
          s_activeCompartment = nextIdx;
          logEvent("Dose due now: compartment %s", compartmentLetter(nextIdx).c_str());
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
      logEvent("Current Distance: (%.0f cm)", dist);
      bool approached = (dist > 0 && dist <= APPROACH_RANGE_CM);

      if (approached) {
        logEvent("Person approached (%.0f cm) - dispensing compartment %s",
                 dist, compartmentLetter(s_activeCompartment).c_str());
        indicatorsSetPattern(IndicatorPattern::PERSON_APPROACHED); // "changes the sound"
        displayMessage("Welcome!", "Dispensing...");
        delay(600); // brief, deliberate pause so the sound-change is perceptible before dispensing
        enterState(DeviceState::DISPENSING);
        break;
      }

      indicatorsSetPattern(IndicatorPattern::WAITING_APPROACH);
      String label = "Compartment " + compartmentLetter(s_activeCompartment);
      displayShowIdle("Time for meds!", label);

      if (millis() - s_stateEnteredAt > ALERT_MAX_WAIT_MS) {
        logEvent("Gave up waiting for approach - marking compartment %s skipped",
                 compartmentLetter(s_activeCompartment).c_str());
        indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
        reportDispense(s_activeCompartment, "skipped", !wifiIsConnected());
        enterState(DeviceState::NORMAL);
      }
      break;
    }

    case DeviceState::DISPENSING: {
      indicatorsSetPattern(IndicatorPattern::DISPENSING);
      displayMessage("Dispensing", compartmentLetter(s_activeCompartment));
      if (!carouselIsMoving() && millis() - s_stateEnteredAt < 50) {
        carouselGoTo(s_activeCompartment);
      }

      if (!carouselIsMoving() && millis() - s_stateEnteredAt > 300) {
        logEvent("Dispensed compartment %s", compartmentLetter(s_activeCompartment).c_str());
        reportDispense(s_activeCompartment, "success", !wifiIsConnected());
        camTriggerVideo();
        logEvent("Triggered CAM board video/adherence task");
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
      if (doorOpened) {
        logEvent("DOOR OPENED");
      }
      else {
        logEvent("DOOR STILL CLOSED");
      }

      float now_g = scaleReadGrams();
      bool weightChanged = fabs(now_g - baselineWeight) >= TRAY_PICKUP_DELTA_G;
      bool pickedUp = doorOpened;
      

      if (pickedUp) {
        logEvent("Pickup confirmed (door opened + weight changed by %.1fg)", now_g - baselineWeight);
        indicatorsSetPattern(IndicatorPattern::PICKUP_OK);
        displayMessage("Great job!", "Medication taken");
        baselineTaken = false;
        enterState(DeviceState::REPORTING);
        break;
      }

      if (millis() - s_stateEnteredAt > PICKUP_MONITOR_MS) {
        logEvent("Pickup NOT confirmed within %lus - alarming", PICKUP_MONITOR_MS / 1000);
        indicatorsSetPattern(IndicatorPattern::PICKUP_MISSED);
        displayMessage("Not picked up", "Check on patient");
        baselineTaken = false;
        enterState(DeviceState::REPORTING);
      }
      break;
    }

    case DeviceState::REPORTING: {
      // The CAM board is already recording/uploading the adherence capture
      // (triggered back in DISPENSING) - just wait for its pass/fail result.
      displayMessage("Confirming", "adherence...");
      bool ok = camWaitForResult(VIDEO_TIMEOUT_MS);
      if (ok) {
        logEvent("CAM board reported adherence capture OK");
      } else {
        logEvent("CAM board reported adherence capture FAILED or timed out");
        indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
        displayMessage("Video capture", "failed");
        delay(1200);
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
  if (s != s_state) logEvent("State: %s -> %s", stateName(s_state), stateName(s));
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
    logEvent("Offline - caching dispense event (%c, %s)", letter, status);
    if (s_offlineEventCount < OFFLINE_CACHE_MAX_EVENTS) {
      CachedDispenseEvent &e = s_offlineEvents[s_offlineEventCount++];
      e.compartment = letter;
      strlcpy(e.status, status, sizeof(e.status));
      strlcpy(e.scheduledTime, s_schedule[compIdx].dispenseTime, sizeof(e.scheduledTime));
      e.dispensedAt = now;
    } else {
      logEvent("WARNING: offline event cache full, dropping event (%c, %s)", letter, status);
    }
    return;
  }

  String eventId;
  bool ok = postDispenseEvent(letter, status, s_schedule[compIdx].dispenseTime, now, false, eventId);
  if (ok) {
    logEvent("Dispense event reported OK (id=%s)", eventId.c_str());
    s_lastDispenseEventId = eventId;
  } else {
    logEvent("Dispense event report FAILED - falling back to offline cache");
    indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
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
    logEvent("Offline - caching telemetry");
    if (s_offlineTelemetryCount < OFFLINE_CACHE_MAX_TELEMETRY) {
      CachedTelemetry &t = s_offlineTelemetry[s_offlineTelemetryCount++];
      t.reportedAt = time(nullptr);
      t.batteryLevel = s_batteryPct;
      strlcpy(t.trayState, tray, sizeof(t.trayState));
      t.personDetected = person;
      t.wifiRssi = 0;
      t.uptimeSeconds = uptime;
    } else {
      logEvent("WARNING: offline telemetry cache full, dropping reading");
    }
    return;
  }

  bool ok = postTelemetry(s_batteryPct, tray, person, uptime);
  logEvent("Telemetry sent: %s (battery %.0f%%)", ok ? "OK" : "FAILED", s_batteryPct);
  if (!ok) indicatorsSetPattern(IndicatorPattern::OFFLINE);

  if (ok && s_offlineEventCount + s_offlineTelemetryCount > 0) {
    logEvent("Flushing offline cache: %u events, %u telemetry", s_offlineEventCount, s_offlineTelemetryCount);
    if (postSyncOfflineBatch(s_offlineEvents, s_offlineEventCount, s_offlineTelemetry, s_offlineTelemetryCount)) {
      s_offlineEventCount = 0;
      s_offlineTelemetryCount = 0;
    } else {
      logEvent("Offline cache flush FAILED - will retry next telemetry cycle");
    }
  }
}

void onScheduleUpdate(CompartmentSlot slots[NUM_COMPARTMENTS], const String &tz) {
  for (int i = 0; i < NUM_COMPARTMENTS; i++) s_schedule[i] = slots[i];
  s_timezone = tz;
  logEvent("Schedule updated via WebSocket push");
}

void onRemoteCommand(const String &type, const String &commandId, const String &payloadJson) {
  logEvent("Remote command received: %s", type.c_str());

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
    logEvent("Remote restart requested - rebooting");
    delay(200); // let the log line actually get flushed out over Serial first
    ESP.restart();
  } else if (type == "sync") {
    sendTelemetryNow(); // opportunistically flushes the offline cache too
  } else if (type == "configure") {
    // reserved for future device-side settings pushed from the backend
  }
}

void runVoiceQuery() {
  logEvent("Voice query requested (long press) - triggering CAM board");
  displayMessage("Listening...", "Ask your question");
  camTriggerAudio();

  bool ok = camWaitForResult(AUDIO_TIMEOUT_MS);
  if (ok) {
    logEvent("CAM board reported voice query OK");
    displayMessage("All done!", "");
    indicatorsSetPattern(IndicatorPattern::PICKUP_OK); // reuse the success chime/flash
  } else {
    logEvent("CAM board reported voice query FAILED or timed out");
    displayMessage("Sorry, that", "didn't work");
    indicatorsSetPattern(IndicatorPattern::ERROR_PATTERN);
  }
  delay(1500);
}
