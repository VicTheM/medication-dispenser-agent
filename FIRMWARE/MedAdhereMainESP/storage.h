#pragma once
#include <Arduino.h>

struct DeviceCredentials {
  String wifiSsid;
  String wifiPass;
  String apiBase;
  String deviceUid;
  String deviceSecret;
  String apPassword;
  bool valid = false; // true once wifi + device creds are all non-empty
};

void storageInit();
DeviceCredentials storageLoad();
void storageSaveWifi(const String &ssid, const String &pass);
void storageSaveApiBase(const String &apiBase);
void storageSaveDeviceCreds(const String &uid, const String &secret);
void storageSaveApPassword(const String &pass);
void storageClearAll();

int8_t storageLoadCarouselPos();     // -1 if never calibrated
void storageSaveCarouselPos(int8_t pos);
