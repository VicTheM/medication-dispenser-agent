#include "storage.h"
#include "config.h"
#include <Preferences.h>

static Preferences prefs;

void storageInit() {
  // Nothing to do up front - Preferences opens/closes per call below to
  // avoid holding an NVS handle open the whole time.
}

DeviceCredentials storageLoad() {
  DeviceCredentials c;
  prefs.begin(NVS_NAMESPACE, true); // read-only
  c.wifiSsid     = prefs.getString(NVS_KEY_WIFI_SSID, "");
  c.wifiPass     = prefs.getString(NVS_KEY_WIFI_PASS, "");
  c.apiBase      = prefs.getString(NVS_KEY_API_BASE, DEFAULT_API_BASE);
  c.deviceUid    = prefs.getString(NVS_KEY_DEVICE_UID, "");
  c.deviceSecret = prefs.getString(NVS_KEY_DEV_SECRET, "");
  c.apPassword   = prefs.getString(NVS_KEY_AP_PASSWORD, CONFIG_AP_PASSWORD_DEFAULT);
  c.utcOffsetHours = prefs.getInt(NVS_KEY_UTC_OFFSET, DEFAULT_UTC_OFFSET_HOURS);
  prefs.end();

  c.valid = c.wifiSsid.length() > 0 && c.deviceUid.length() > 0 && c.deviceSecret.length() > 0;
  return c;
}

void storageSaveWifi(const String &ssid, const String &pass) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_WIFI_SSID, ssid);
  prefs.putString(NVS_KEY_WIFI_PASS, pass);
  prefs.end();
}

void storageSaveApiBase(const String &apiBase) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_API_BASE, apiBase);
  prefs.end();
}

void storageSaveDeviceCreds(const String &uid, const String &secret) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_DEVICE_UID, uid);
  prefs.putString(NVS_KEY_DEV_SECRET, secret);
  prefs.end();
}

void storageSaveApPassword(const String &pass) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putString(NVS_KEY_AP_PASSWORD, pass);
  prefs.end();
}

void storageSaveUtcOffset(int hours) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putInt(NVS_KEY_UTC_OFFSET, hours);
  prefs.end();
}

void storageClearAll() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
}

int8_t storageLoadCarouselPos() {
  prefs.begin(NVS_NAMESPACE, true);
  int8_t pos = prefs.getChar(NVS_KEY_CAROUSEL_POS, -1);
  prefs.end();
  return pos;
}

void storageSaveCarouselPos(int8_t pos) {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.putChar(NVS_KEY_CAROUSEL_POS, pos);
  prefs.end();
}
