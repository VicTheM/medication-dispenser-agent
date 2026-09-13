#include "webportal.h"
#include "config.h"
#include "storage.h"
#include <WiFi.h>
#include <WebServer.h>

static WebServer s_server(80);
static bool s_active = false;
static bool s_saveCompleted = false;
static unsigned long s_lastRequestMs = 0;

static const char PAGE_FORM[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
<title>MedAdhere Setup</title>
<style>body{font-family:sans-serif;max-width:420px;margin:24px auto;padding:0 16px}
h2{margin-bottom:4px}label{display:block;margin-top:14px;font-size:14px;font-weight:600}
input{width:100%;padding:8px;font-size:15px;box-sizing:border-box}
button{margin-top:20px;width:100%;padding:12px;font-size:15px;background:#14213D;color:#fff;border:none;border-radius:6px}
p.hint{color:#666;font-size:13px}</style></head><body>
<h2>MedAdhere Setup</h2>
<p class="hint">This form is only reachable on this device's own protected setup Wi-Fi network.</p>
<form method="POST" action="/save">
<label>Home Wi-Fi SSID</label><input name="ssid" required>
<label>Home Wi-Fi Password</label><input name="pass" type="password">
<label>API base URL</label><input name="api_base" value="%API_BASE%">
<label>Device ID</label><input name="device_uid" value="%DEVICE_UID%">
<label>Device Secret</label><input name="device_secret" value="%DEVICE_SECRET%">
<label>New setup-network password (optional)</label><input name="ap_pass" type="password" placeholder="leave blank to keep current">
<button type="submit">Save &amp; Restart</button>
</form></body></html>
)HTML";

static const char PAGE_SAVED[] PROGMEM =
  "<!DOCTYPE html><html><body style='font-family:sans-serif;max-width:420px;margin:60px auto;text-align:center'>"
  "<h2>Saved</h2><p>Restarting and connecting to your Wi-Fi now...</p></body></html>";

static void handleRoot() {
  s_lastRequestMs = millis();
  DeviceCredentials c = storageLoad();
  String html = FPSTR(PAGE_FORM);
  html.replace("%API_BASE%", c.apiBase);
  html.replace("%DEVICE_UID%", c.deviceUid);
  html.replace("%DEVICE_SECRET%", c.deviceSecret);
  s_server.send(200, "text/html", html);
}

static void handleSave() {
  s_lastRequestMs = millis();
  String ssid = s_server.arg("ssid");
  String pass = s_server.arg("pass");
  String apiBase = s_server.arg("api_base");
  String deviceUid = s_server.arg("device_uid");
  String deviceSecret = s_server.arg("device_secret");
  String apPass = s_server.arg("ap_pass");

  if (ssid.length() > 0) storageSaveWifi(ssid, pass);
  if (apiBase.length() > 0) storageSaveApiBase(apiBase);
  if (deviceUid.length() > 0 && deviceSecret.length() > 0) storageSaveDeviceCreds(deviceUid, deviceSecret);
  if (apPass.length() >= 8) storageSaveApPassword(apPass);

  s_server.send(200, "text/html", FPSTR(PAGE_SAVED));
  s_saveCompleted = true;
}

void webportalStart() {
  DeviceCredentials c = storageLoad();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char suffix[5];
  snprintf(suffix, sizeof(suffix), "%02X%02X", mac[4], mac[5]);
  String apSsid = String(CONFIG_AP_SSID_PREFIX) + suffix;

  WiFi.mode(WIFI_AP);
  WiFi.softAP(apSsid.c_str(), c.apPassword.c_str()); // WPA2 - see DEVICE_BRIEF.md

  s_server.on("/", HTTP_GET, handleRoot);
  s_server.on("/save", HTTP_POST, handleSave);
  s_server.begin();

  s_active = true;
  s_saveCompleted = false;
  s_lastRequestMs = millis();

  Serial.printf("[portal] AP '%s' started, password required, config at http://%s/\n",
                apSsid.c_str(), WiFi.softAPIP().toString().c_str());
}

void webportalLoop() {
  if (!s_active) return;
  s_server.handleClient();
  if (millis() - s_lastRequestMs > CONFIG_PORTAL_TIMEOUT_MS) {
    Serial.println("[portal] idle timeout, exiting config mode");
    webportalStop();
  }
}

void webportalStop() {
  s_server.stop();
  WiFi.softAPdisconnect(true);
  s_active = false;
}

bool webportalIsActive() {
  return s_active;
}

bool webportalSaveCompleted() {
  if (!s_saveCompleted) return false;
  s_saveCompleted = false;
  return true;
}
