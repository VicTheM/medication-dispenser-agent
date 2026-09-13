#pragma once
/*
  Link to the audio/video board over Serial2. That board now handles its
  own WiFi connection and talks to the AI service directly - this board's
  job is just to trigger it and wait for a pass/fail result:

    Main -> CAM : single ASCII byte 'a' (do the audio/voice task) or
                  'v' (do the video/adherence task)
    CAM  -> Main: single byte, 0 = success, 1 = error (any other byte
                  seen while waiting is ignored, in case of stray debug
                  output leaking onto the line)

  Separately, once at boot, the main board pushes its NVS config (WiFi
  creds, API base, device identity, UTC offset) to the CAM board as plain
  text lines, since that board needs WiFi credentials too now. See
  DEVICE_BRIEF.md "Config handoff to the CAM board".
*/
#include <Arduino.h>

void camlinkInit();

// Sends the device's current config as simple text lines the CAM board can
// parse (key=value, one per line, wrapped in CFGSTART/CFGEND markers).
void camSendConfig(const String &wifiSsid, const String &wifiPass, const String &apiBase,
                    const String &deviceUid, const String &deviceSecret, int utcOffsetHours);

void camTriggerAudio(); // sends 'a'
void camTriggerVideo(); // sends 'v'

// Waits up to timeoutMs for a single result byte. Returns true on 0
// (success), false on 1 or on timeout.
bool camWaitForResult(unsigned long timeoutMs);
