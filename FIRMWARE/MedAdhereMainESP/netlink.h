#pragma once
#include <Arduino.h>
#include "config.h"
#include "types.h"

// ---- WiFi ----
bool wifiConnect(const String &ssid, const String &pass, unsigned long timeoutMs);
bool wifiIsConnected();

// ---- Device identity (set once after loading from storage) ----
void networkSetIdentity(const String &apiBase, const String &deviceUid, const String &deviceSecret);

// ---- WebSocket control channel ----
// onScheduleUpdate/onCommand are simple function-pointer callbacks (kept as
// plain C-style callbacks rather than std::function to keep this friendly
// for constrained builds).
typedef void (*ScheduleUpdateCallback)(CompartmentSlot slots[NUM_COMPARTMENTS], const String &timezone);
typedef void (*RemoteCommandCallback)(const String &commandType, const String &commandId, const String &payloadJson);

void wsBegin(ScheduleUpdateCallback onSchedule, RemoteCommandCallback onCommand);
void wsLoop();
bool wsIsConnected();
void wsSendAck(const String &commandId);

// ---- REST: schedule ----
bool fetchSchedule(CompartmentSlot slots[NUM_COMPARTMENTS], String &timezoneOut);

// ---- REST: dispense + telemetry (JSON, small) ----
bool postDispenseEvent(char compartment, const char *status, const char *scheduledTimeOrNull,
                        time_t dispensedAtEpoch, bool wasOfflineCached, String &outEventId);
bool postTelemetry(float batteryPct, const char *trayState, bool personDetected, uint32_t uptimeSeconds);
bool postSyncOfflineBatch(CachedDispenseEvent *events, uint8_t eventCount,
                           CachedTelemetry *telemetry, uint8_t telemetryCount);

// ---- REST: multipart uploads ----
// Streams exactly fileLen bytes FROM Serial2 (the CAM board link) directly
// into the HTTPS request body - used for the adherence video, which can be
// too large to buffer fully in RAM. See DEVICE_BRIEF.md for the bandwidth
// discussion that makes this the right approach.
bool uploadAdherenceVideoFromSerial2(const String &dispenseEventId, uint32_t fileLen, uint16_t durationSeconds);

// Voice question audio is small enough to buffer in PSRAM first; posts it
// and returns the AI's answer text/transcript plus enough info to fetch the
// spoken-answer audio afterward.
bool postVoiceQuery(const uint8_t *audioBuf, uint32_t audioLen, const char *audioFormat,
                     String &outTranscript, String &outAnswerText, String &outInteractionId, String &outAudioFormat);

// Downloads the interaction's spoken-answer audio into a caller-provided
// buffer (caller must size/allocate it - use a PSRAM allocation).
bool downloadVoiceAudio(const String &interactionId, uint8_t **outBuf, uint32_t *outLen);
