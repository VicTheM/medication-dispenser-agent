// /*
//   ESP32-S3 + OV7670 -> capture 10 seconds of raw RGB565 frames INTO PSRAM FIRST,
//   then upload to a backend over TCP. This decouples capture (which must never
//   fail) from network transmission (which is allowed to fail and retry).

//   RELIABILITY STRATEGY:
//     - Capture the whole clip into a pre-allocated PSRAM buffer. This loop has
//       no network dependency, so it always finishes with a full clip.
//     - Frame count / resolution auto-adjust to whatever PSRAM is actually free,
//       so it degrades quality/length instead of crashing on low-memory boards.
//     - A bad/short camera read never shortens the video: the previous good
//       frame is duplicated into that slot instead of being skipped.
//     - Upload happens afterwards with retries + backoff. If it still fails,
//       the clip stays in RAM and can be re-sent later by sending 'u' over
//       Serial -- no need to record again.

//   PROTOCOL (unchanged from before, backend script needs no changes):
//     1. Connect via TCP to RECORD_HOST:RECORD_PORT
//     2. Send one line of JSON + '\n':
//          {"width":160,"height":120,"format":"rgb565","fps":10}
//     3. For each frame: 4 bytes big-endian length, then N bytes raw RGB565 data
//     4. 4 bytes of 0x00000000 as end-of-stream marker
//     5. Close the socket
// */

// #include "esp_camera.h"
// #include <WiFi.h>
// #include "esp_heap_caps.h"

// // ---------------- Wi-Fi config ----------------
// const char* WIFI_SSID     = "iPhone";
// const char* WIFI_PASSWORD = "burnitup";

// // ---------------- Backend config ----------------
// const char* RECORD_HOST = "172.20.10.3";
// const uint16_t RECORD_PORT = 5001;

// // ---------------- Recording config ----------------
// const uint32_t RECORD_DURATION_MS = 10000;
// const uint32_t DESIRED_FPS = 10;
// const uint32_t DESIRED_FRAMES = (RECORD_DURATION_MS / 1000) * DESIRED_FPS; // 100
// const uint32_t FRAME_INTERVAL_MS = 1000 / DESIRED_FPS;
// const uint32_t MIN_FRAMES = 10;      // absolute floor: at least ~1s of video
// const uint32_t WARMUP_FRAMES = 5;    // discarded, lets AEC/AWB settle

// // ---------------- Frame geometry ----------------
// // QQVGA chosen deliberately: needs ~4x less memory than QVGA, so it comfortably
// // fits in PSRAM on most ESP32-S3 boards. Bump to FRAMESIZE_QVGA (320x240) only
// // if you've confirmed you have plenty of free PSRAM (8MB module, minimal other use).
// #define CAM_FRAMESIZE FRAMESIZE_QQVGA
// const uint16_t FRAME_WIDTH  = 160;
// const uint16_t FRAME_HEIGHT = 120;
// const size_t FRAME_BYTES = (size_t)FRAME_WIDTH * FRAME_HEIGHT * 2; // RGB565

// // ---------------- Camera pin map (EDIT THESE for your wiring) ----------------
// #define CAM_PIN_PWDN   21
// #define CAM_PIN_RESET  47
// #define CAM_PIN_XCLK   15
// #define CAM_PIN_SIOD    4
// #define CAM_PIN_SIOC    5
// #define CAM_PIN_D7     16
// #define CAM_PIN_D6     17
// #define CAM_PIN_D5     18
// #define CAM_PIN_D4     12
// #define CAM_PIN_D3     10
// #define CAM_PIN_D2      8
// #define CAM_PIN_D1      9
// #define CAM_PIN_D0     11
// #define CAM_PIN_VSYNC   6
// #define CAM_PIN_HREF    7
// #define CAM_PIN_PCLK   13

// bool cameraReady = false;

// // Buffer state
// uint8_t* frameBuffer = nullptr;   // holds recordFrames * FRAME_BYTES
// uint32_t recordFrames = 0;        // how many frames actually captured this session
// bool hasBufferedClip = false;

// // ---------------- Camera init ----------------
// bool initCamera() {
//   camera_config_t config;
//   config.ledc_channel = LEDC_CHANNEL_0;
//   config.ledc_timer   = LEDC_TIMER_0;

//   config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1;
//   config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
//   config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5;
//   config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;

//   config.pin_xclk  = CAM_PIN_XCLK;
//   config.pin_pclk  = CAM_PIN_PCLK;
//   config.pin_vsync = CAM_PIN_VSYNC;
//   config.pin_href  = CAM_PIN_HREF;
//   config.pin_sccb_sda = CAM_PIN_SIOD;
//   config.pin_sccb_scl = CAM_PIN_SIOC;
//   config.pin_pwdn  = CAM_PIN_PWDN;
//   config.pin_reset = CAM_PIN_RESET;

//   config.pixel_format = PIXFORMAT_RGB565;  // OV7670 has no hardware JPEG
//   config.frame_size   = CAM_FRAMESIZE;
//   config.xclk_freq_hz = 8000000;
//   config.fb_count     = psramFound() ? 2 : 1;
//   config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
//   config.grab_mode    = CAMERA_GRAB_LATEST;

//   esp_err_t err = esp_camera_init(&config);
//   if (err != ESP_OK) {
//     Serial.printf("Camera init failed: 0x%x\n", err);
//     return false;
//   }
//   return true;
// }

// // ---------------- Wi-Fi ----------------
// void connectWiFi() {
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
//   Serial.print("Connecting to WiFi");
//   uint32_t start = millis();
//   while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
//     delay(300);
//     Serial.print(".");
//   }
//   Serial.println();
//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.print("WiFi connected, IP: ");
//     Serial.println(WiFi.localIP());
//   } else {
//     Serial.println("WiFi not connected yet -- will keep retrying at upload time.");
//   }
// }

// // ---------------- Allocate the frame buffer, degrading gracefully ----------------
// // Tries PSRAM first, backing off the frame count until it fits. If no PSRAM
// // exists at all, makes one last small attempt in internal RAM so you still
// // get *something* rather than nothing.
// bool allocateFrameBuffer() {
//   uint32_t target = DESIRED_FRAMES;

//   if (psramFound()) {
//     size_t freePsram = ESP.getFreePsram();
//     size_t usable = (size_t)(freePsram * 0.6); // leave headroom for WiFi/other allocs
//     uint32_t maxFit = usable / FRAME_BYTES;
//     if (maxFit < target) {
//       Serial.printf("Only enough PSRAM for %u frames (wanted %u) -> shorter/choppier clip.\n",
//                     maxFit, target);
//       target = maxFit;
//     }
//   } else {
//     Serial.println("No PSRAM found -- falling back to a very short internal-RAM clip.");
//     target = min(target, (uint32_t)20); // small ceiling attempt for no-PSRAM boards
//   }

//   while (target >= MIN_FRAMES) {
//     uint32_t caps = psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
//     frameBuffer = (uint8_t*) heap_caps_malloc((size_t)target * FRAME_BYTES, caps);
//     if (frameBuffer != nullptr) {
//       recordFrames = target;
//       Serial.printf("Allocated buffer for %u frames (%.1f sec @ %u fps).\n",
//                     recordFrames, (float)recordFrames / DESIRED_FPS, DESIRED_FPS);
//       return true;
//     }
//     target /= 2; // back off and try a smaller clip
//   }

//   Serial.println("Could not allocate any frame buffer. No video possible this attempt.");
//   return false;
// }

// void freeFrameBuffer() {
//   if (frameBuffer) {
//     heap_caps_free(frameBuffer);
//     frameBuffer = nullptr;
//   }
//   hasBufferedClip = false;
// }

// // ---------------- Capture phase (network-independent, always completes) ----------------
// void captureClip() {
//   if (!cameraReady || frameBuffer == nullptr) return;

//   // Warm-up: let AEC/AWB settle, discard these frames
//   for (uint32_t i = 0; i < WARMUP_FRAMES; i++) {
//     camera_fb_t* fb = esp_camera_fb_get();
//     if (fb) esp_camera_fb_return(fb);
//   }

//   Serial.println("Capturing clip...");
//   bool haveGoodFrame = false;

//   for (uint32_t i = 0; i < recordFrames; i++) {
//     uint32_t frameStart = millis();
//     uint8_t* slot = frameBuffer + (size_t)i * FRAME_BYTES;

//     camera_fb_t* fb = esp_camera_fb_get();
//     bool ok = fb && fb->len == FRAME_BYTES;

//     if (ok) {
//       memcpy(slot, fb->buf, FRAME_BYTES);
//       haveGoodFrame = true;
//     } else if (haveGoodFrame) {
//       // Duplicate the previous good frame instead of leaving a gap
//       memcpy(slot, slot - FRAME_BYTES, FRAME_BYTES);
//     } else {
//       // No good frame yet at all -- fill with black rather than garbage
//       memset(slot, 0x00, FRAME_BYTES);
//     }

//     if (fb) esp_camera_fb_return(fb);

//     uint32_t elapsed = millis() - frameStart;
//     if (elapsed < FRAME_INTERVAL_MS) delay(FRAME_INTERVAL_MS - elapsed);
//   }

//   hasBufferedClip = true;
//   Serial.printf("Capture complete: %u frames buffered.\n", recordFrames);
// }

// // ---------------- Send helpers ----------------
// void sendUint32BE(WiFiClient& client, uint32_t value) {
//   uint8_t buf[4] = {
//     (uint8_t)(value >> 24), (uint8_t)(value >> 16),
//     (uint8_t)(value >> 8),  (uint8_t)(value)
//   };
//   client.write(buf, 4);
// }

// // ---------------- Upload phase (retryable, doesn't touch the camera) ----------------
// bool uploadClip() {
//   if (!hasBufferedClip || frameBuffer == nullptr) {
//     Serial.println("No buffered clip to upload.");
//     return false;
//   }

//   const int MAX_ATTEMPTS = 5;
//   uint32_t backoffMs = 500;

//   for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
//     if (WiFi.status() != WL_CONNECTED) {
//       Serial.println("WiFi down, reconnecting...");
//       WiFi.reconnect();
//       delay(1000);
//     }

//     WiFiClient client;
//     Serial.printf("Upload attempt %d/%d...\n", attempt, MAX_ATTEMPTS);
//     if (client.connect(RECORD_HOST, RECORD_PORT)) {
//       char header[128];
//       snprintf(header, sizeof(header),
//                "{\"width\":%u,\"height\":%u,\"format\":\"rgb565\",\"fps\":%u}\n",
//                FRAME_WIDTH, FRAME_HEIGHT, DESIRED_FPS);
//       client.print(header);

//       bool ok = true;
//       for (uint32_t i = 0; i < recordFrames && ok; i++) {
//         uint8_t* slot = frameBuffer + (size_t)i * FRAME_BYTES;
//         sendUint32BE(client, FRAME_BYTES);
//         size_t written = client.write(slot, FRAME_BYTES);
//         if (written != FRAME_BYTES || !client.connected()) ok = false;
//       }

//       if (ok) {
//         sendUint32BE(client, 0); // end-of-stream marker
//         client.stop();
//         Serial.println("Upload complete.");
//         return true;
//       }
//       client.stop();
//       Serial.println("Upload dropped mid-stream, will retry.");
//     } else {
//       Serial.println("Connect failed.");
//     }

//     delay(backoffMs);
//     backoffMs *= 2;
//   }

//   Serial.println("Upload failed after all retries. Clip is still buffered -- send 'u' to retry.");
//   return false;
// }

// // ---------------- Orchestration ----------------
// void recordAndUpload() {
//   freeFrameBuffer();
//   if (!allocateFrameBuffer()) return;
//   captureClip();
//   uploadClip(); // failure here is fine -- clip stays buffered for 'u' retry
// }

// void setup() {
//   Serial.begin(115200);
//   delay(500);

//   cameraReady = initCamera();
//   if (!cameraReady) {
//     Serial.println("Halting: camera init failed. Check wiring/pins.");
//   }

//   connectWiFi();

//   if (cameraReady) {
//     recordAndUpload();
//   }

//   Serial.println("Send 'r' to record a new clip, 'u' to re-upload the last one.");
// }

// void loop() {
//   if (Serial.available()) {
//     char c = Serial.read();
//     if (c == 'r' || c == 'R') {
//       recordAndUpload();
//     } else if (c == 'u' || c == 'U') {
//       uploadClip();
//     }
//   }
// }

// // void setup() {
// //   Serial.begin(115200);
// //   delay(1000);
// //   Serial.printf("Total Flash Size: %d MB\n", ESP.getFlashChipSize() / (1024 * 1024));
// // }
// // void loop() {}

#include "esp_camera.h"

// ===========================
// OV7670 + ESP32-S3
// ===========================

#define CAM_PIN_PWDN   -1
#define CAM_PIN_RESET  -1

#define CAM_PIN_XCLK   15

#define CAM_PIN_SIOD    4
#define CAM_PIN_SIOC    5

#define CAM_PIN_D7     16
#define CAM_PIN_D6     17
#define CAM_PIN_D5     18
#define CAM_PIN_D4     12
#define CAM_PIN_D3     10
#define CAM_PIN_D2      8
#define CAM_PIN_D1      9
#define CAM_PIN_D0     11

#define CAM_PIN_VSYNC   6
#define CAM_PIN_HREF    7
#define CAM_PIN_PCLK   13

void setup()
{
    Serial.begin(115200);
    delay(2000);

    Serial.println();
    Serial.println("OV7670 ESP32-S3 Camera Test");
    Serial.println("============================");

    camera_config_t config;

    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;

    config.pin_d0 = CAM_PIN_D0;
    config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2;
    config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4;
    config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6;
    config.pin_d7 = CAM_PIN_D7;

    config.pin_xclk  = CAM_PIN_XCLK;
    config.pin_pclk  = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC;
    config.pin_href  = CAM_PIN_HREF;

    config.pin_sccb_sda = CAM_PIN_SIOD;
    config.pin_sccb_scl = CAM_PIN_SIOC;

    config.pin_pwdn  = CAM_PIN_PWDN;
    config.pin_reset = CAM_PIN_RESET;

    config.xclk_freq_hz = 20000000;

    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size   = FRAMESIZE_QVGA;

    config.jpeg_quality = 12;

    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    Serial.println("Initializing camera...");

    esp_err_t err = esp_camera_init(&config);

    if (err != ESP_OK)
    {
        Serial.printf(
            "Camera initialization FAILED: 0x%x (%s)\n",
            err,
            esp_err_to_name(err)
        );

        return;
    }

    Serial.println("Camera initialized successfully!");

    sensor_t *sensor = esp_camera_sensor_get();

    if (sensor)
    {
        Serial.printf("Sensor PID: 0x%04X\n", sensor->id.PID);
        Serial.printf("Sensor VER: 0x%02X\n", sensor->id.VER);
        Serial.printf("Sensor MIDH: 0x%02X\n", sensor->id.MIDH);
        Serial.printf("Sensor MIDL: 0x%02X\n", sensor->id.MIDL);
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb)
    {
        Serial.println("Failed to capture frame!");
        return;
    }

    Serial.printf(
        "Frame captured: %ux%u, %u bytes\n",
        fb->width,
        fb->height,
        fb->len
    );

    esp_camera_fb_return(fb);

    Serial.println("OV7670 TEST PASSED");
}

void loop()
{
    delay(5000);
}
