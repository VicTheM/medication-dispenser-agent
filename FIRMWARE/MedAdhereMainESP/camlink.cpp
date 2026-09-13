#include "camlink.h"
#include "config.h"

void camlinkInit() {
  Serial2.begin(CAM_SERIAL_BAUD, SERIAL_8N1, PIN_CAM_RX, PIN_CAM_TX);
}

void camSendConfig(const String &wifiSsid, const String &wifiPass, const String &apiBase,
                    const String &deviceUid, const String &deviceSecret, int utcOffsetHours) {
  Serial2.println("CFGSTART");
  Serial2.print("WIFI_SSID="); Serial2.println(wifiSsid);
  Serial2.print("WIFI_PASS="); Serial2.println(wifiPass);
  Serial2.print("API_BASE="); Serial2.println(apiBase);
  Serial2.print("DEVICE_UID="); Serial2.println(deviceUid);
  Serial2.print("DEVICE_SECRET="); Serial2.println(deviceSecret);
  Serial2.print("UTC_OFFSET="); Serial2.println(utcOffsetHours);
  Serial2.println("CFGEND");
  Serial2.flush();
}

void camTriggerAudio() {
  Serial2.write('a');
  Serial2.flush();
}

void camTriggerVideo() {
  Serial2.write('v');
  Serial2.flush();
}

bool camWaitForResult(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    if (Serial2.available()) {
      int b = Serial2.read();
      if (b == 0) return true;
      if (b == 1) return false;
      // anything else (stray debug bytes) - keep waiting
    } else {
      delay(5);
    }
  }
  return false; // timeout counts as failure
}
