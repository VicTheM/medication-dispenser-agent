/*
  ESP32-CAM (AI Thinker) -- combined video + audio streaming firmware
  ----------------------------------------------------------------------
  Hardware on one board:
    - RHYX M21-45 camera wired to the AI-Thinker board's standard camera
      header (confirmed-working pin mapping from the CameraWebServer
      AI_THINKER config).
    - MAX4466 mic on GPIO12 (ADC1_CH5, unused by the camera).
    - MAX98357A amp (I2S) driving a 4-ohm speaker.

  Video is raw RGB565 (no JPEG) -- the backend receives full uncompressed
  frames and reconstructs the video itself. No processing happens on the
  ESP32 beyond capturing and forwarding bytes.

  EVERYTHING IS TRIGGERED BY SERIAL COMMANDS. Nothing records or sends
  automatically at boot -- type 'h' after boot for the menu.

  Commands:
    v  - Record 10s of video, buffer it, upload to VIDEO backend
    u  - Re-upload the last video clip without re-recording
    a  - Record 5s of audio, send to AUDIO backend, play the response
    p  - Play the startup .pcm clip from LittleFS (sanity check for the speaker)
    s  - Print status (WiFi, free heap, free PSRAM, buffer states)
    h  - Print this menu

  RELIABILITY DESIGN (why it's built this way):
    - Both video and audio are captured FULLY INTO PSRAM before any
      network call. Capture can never be interrupted or shortened by a
      bad Wi-Fi connection -- only the upload step can fail, and that's
      retried with backoff.
    - Video buffer size auto-adjusts to whatever PSRAM is actually free,
      so it degrades to a shorter/choppier clip instead of crashing.
    - Bad camera frames are patched by duplicating the previous good
      frame, so the video always has the exact expected frame count.

  PROTOCOLS:
    VIDEO -> backend (port 5001):
      1. Connect, send JSON header line:
           {"width":160,"height":120,"format":"rgb565","fps":10}
      2. Per frame: 4-byte BE length + raw RGB565 bytes
      3. 4 bytes of 0x00000000 as end-of-stream marker, then close.

    AUDIO <-> backend (port 5002), same connection both ways:
      1. Connect, send JSON header line:
           {"sample_rate":16000,"channels":1,"bits":16,"samples":80000}
      2. 4-byte BE length + raw PCM bytes (the recording)
      3. Backend replies on the SAME connection:
           4-byte BE length + raw PCM bytes (the response, 16kHz/mono/16-bit)
      4. Close.

  REQUIREMENTS
    - arduino-esp32 core 2.0.x (legacy driver/i2s.h API used for the amp).
    - "esp32-camera" library (usually bundled with the core).
    - PSRAM enabled (Tools > PSRAM: Enabled).
    - Optional: "/startup.pcm" on LittleFS (see notes at bottom of file).
*/

#include <Arduino.h>
#include <WiFi.h>
#include "FS.h"
#include "LittleFS.h"
#include "driver/i2s.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"

// =====================================================================
//  CONFIG -- EDIT THESE
// =====================================================================
const char* WIFI_SSID     = "iPhone";
const char* WIFI_PASSWORD = "burnitup";

const char* VIDEO_BACKEND_HOST = "172.20.10.2";
const uint16_t VIDEO_BACKEND_PORT = 5001;

const char* AUDIO_BACKEND_HOST = "172.20.10.2";
const uint16_t AUDIO_BACKEND_PORT = 5002;

// ---------------- Camera pins (AI-Thinker standard header -- confirmed working) ----------------
#define CAM_PIN_PWDN   32
#define CAM_PIN_RESET  -1
#define CAM_PIN_XCLK    0
#define CAM_PIN_SIOD   26
#define CAM_PIN_SIOC   27
#define CAM_PIN_D7     35
#define CAM_PIN_D6     34
#define CAM_PIN_D5     39
#define CAM_PIN_D4     36
#define CAM_PIN_D3     21
#define CAM_PIN_D2     19
#define CAM_PIN_D1     18
#define CAM_PIN_D0      5
#define CAM_PIN_VSYNC  25
#define CAM_PIN_HREF   23
#define CAM_PIN_PCLK   22

// ---------------- Mic / speaker pins ----------------
#define I2S_SPK_PORT      I2S_NUM_1
#define I2S_SPK_BCLK      14   // BCLK / SCK
#define I2S_SPK_LRC       15   // WS / LRCLK
#define I2S_SPK_DOUT      13   // DIN on MAX98357A
#define MIC_PIN           33   // ADC1_CH5, safe / unused by camera and Wi-Fi

// ---------------- Video config ----------------
#define CAM_FRAMESIZE FRAMESIZE_QQVGA
const uint16_t VIDEO_WIDTH  = 160;
const uint16_t VIDEO_HEIGHT = 120;
const size_t VIDEO_FRAME_BYTES = (size_t)VIDEO_WIDTH * VIDEO_HEIGHT * 2; // RGB565
const uint32_t VIDEO_RECORD_MS = 10000;
const uint32_t VIDEO_FPS = 10;
const uint32_t VIDEO_DESIRED_FRAMES = (VIDEO_RECORD_MS / 1000) * VIDEO_FPS; // 100
const uint32_t VIDEO_FRAME_INTERVAL_MS = 1000 / VIDEO_FPS;
const uint32_t VIDEO_MIN_FRAMES = 10;
const uint32_t VIDEO_WARMUP_FRAMES = 5;

// ---------------- Audio config ----------------
#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_RECORD_SECONDS 5
#define AUDIO_RECORD_SAMPLES (AUDIO_SAMPLE_RATE * AUDIO_RECORD_SECONDS)
#define STARTUP_FILE "/startup.pcm"
#define PLAYBACK_CHUNK 512
#define MAX_AUDIO_RESPONSE_BYTES (30UL * AUDIO_SAMPLE_RATE * sizeof(int16_t))

// =====================================================================
//  GLOBAL STATE
// =====================================================================
bool cameraReady = false;
bool speakerReady = false;

uint8_t* videoBuffer = nullptr;
uint32_t videoRecordFrames = 0;
bool hasBufferedVideo = false;

int16_t* audioRecordBuffer = nullptr;

// =====================================================================
//  DEBUG HELPERS
// =====================================================================
void printMemStatus(const char* tag) {
  Serial.printf("[MEM][%s] freeHeap=%u bytes", tag, (unsigned)ESP.getFreeHeap());
  if (psramFound()) {
    Serial.printf(", freePsram=%u bytes", (unsigned)ESP.getFreePsram());
  } else {
    Serial.print(", PSRAM not found");
  }
  Serial.println();
}

void printWiFiStatus() {
  Serial.print("[WiFi] status=");
  switch (WiFi.status()) {
    case WL_CONNECTED: Serial.print("CONNECTED"); break;
    case WL_DISCONNECTED: Serial.print("DISCONNECTED"); break;
    case WL_CONNECT_FAILED: Serial.print("CONNECT_FAILED"); break;
    case WL_IDLE_STATUS: Serial.print("IDLE"); break;
    default: Serial.print("OTHER"); break;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf(", IP=%s, RSSI=%d dBm", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  Serial.println();
}

void printHelp() {
  Serial.println("==================================================");
  Serial.println("[MENU]");
  Serial.println("  v - Record 10s video and upload");
  Serial.println("  u - Re-upload last video clip (no re-record)");
  Serial.println("  a - Record 5s audio, send, play response");
  Serial.println("  p - Play startup.pcm from LittleFS");
  Serial.println("  s - Show status (WiFi/memory/buffers)");
  Serial.println("  h - Show this menu");
  Serial.println("==================================================");
}

// =====================================================================
//  WI-FI
// =====================================================================
void connectWiFi() {
  Serial.println("[WiFi] Starting connection...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  printWiFiStatus();
}

// =====================================================================
//  CAMERA INIT
// =====================================================================
bool initCamera() {
  Serial.println("[CAM] Initializing camera...");
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;

  config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;

  config.pin_xclk  = CAM_PIN_XCLK;
  config.pin_pclk  = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href  = CAM_PIN_HREF;
  config.pin_sccb_sda = CAM_PIN_SIOD;
  config.pin_sccb_scl = CAM_PIN_SIOC;
  config.pin_pwdn  = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;

  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size   = CAM_FRAMESIZE;     // QQVGA (160x120)
  config.xclk_freq_hz = 20000000;          // 20MHz -- matches the confirmed-working config
  config.fb_count     = psramFound() ? 2 : 1;
  config.fb_location  = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  Serial.printf("[CAM] pixel_format=RGB565 frame_size=QQVGA(%ux%u) xclk=20MHz fb_count=%d fb_location=%s\n",
                VIDEO_WIDTH, VIDEO_HEIGHT, config.fb_count,
                psramFound() ? "PSRAM" : "DRAM");

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[CAM] ERROR: esp_camera_init failed (0x%x)\n", err);
    return false;
  }
  Serial.println("[CAM] Camera init OK.");
  return true;
}

// =====================================================================
//  I2S SPEAKER
// =====================================================================
void setupI2SSpeaker() {
  Serial.println("[SPK] Initializing I2S speaker...");
  i2s_config_t cfg = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = AUDIO_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
  };

  esp_err_t err = i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
  if (err != ESP_OK) {
    Serial.printf("[SPK] ERROR: i2s_driver_install failed (%d)\n", err);
    speakerReady = false;
    return;
  }

  i2s_pin_config_t pins = {
    .bck_io_num = I2S_SPK_BCLK,
    .ws_io_num = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  i2s_set_pin(I2S_SPK_PORT, &pins);
  i2s_zero_dma_buffer(I2S_SPK_PORT);
  speakerReady = true;
  Serial.printf("[SPK] I2S speaker ready (BCLK=%d LRC=%d DIN=%d)\n",
                I2S_SPK_BCLK, I2S_SPK_LRC, I2S_SPK_DOUT);
}

void i2sWriteMonoBlock(const int16_t *mono, size_t count) {
  static int16_t stereoBuf[PLAYBACK_CHUNK * 2];
  size_t pos = 0;
  while (pos < count) {
    size_t n = min((size_t)PLAYBACK_CHUNK, count - pos);
    for (size_t i = 0; i < n; i++) {
      stereoBuf[2 * i]     = mono[pos + i];
      stereoBuf[2 * i + 1] = mono[pos + i];
    }
    size_t bytesWritten = 0;
    i2s_write(I2S_SPK_PORT, stereoBuf, n * 2 * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    pos += n;
  }
}

bool playPCMFile(const char *path) {
  Serial.printf("[PCM] Opening '%s'...\n", path);
  File f = LittleFS.open(path, "r");
  if (!f) {
    Serial.printf("[PCM] '%s' not found - skipping.\n", path);
    return false;
  }
  size_t fileBytes = f.size();
  Serial.printf("[PCM] Playing '%s' (%u bytes, ~%.2f s)\n",
                path, (unsigned)fileBytes, fileBytes / 2.0f / AUDIO_SAMPLE_RATE);

  int16_t monoBuf[PLAYBACK_CHUNK];
  size_t totalSamples = 0;
  while (f.available()) {
    size_t bytesRead = f.read((uint8_t *)monoBuf, sizeof(monoBuf));
    size_t samples = bytesRead / sizeof(int16_t);
    if (samples == 0) break;
    i2sWriteMonoBlock(monoBuf, samples);
    totalSamples += samples;
  }
  f.close();
  Serial.printf("[PCM] Done, played %u samples.\n", (unsigned)totalSamples);
  return true;
}

bool playAudioBuffer(const int16_t *buffer, size_t numSamples) {
  Serial.printf("[SPK] Playing %u samples (%.2f s)...\n",
                (unsigned)numSamples, numSamples / (float)AUDIO_SAMPLE_RATE);
  uint32_t t0 = millis();
  i2sWriteMonoBlock(buffer, numSamples);
  Serial.printf("[SPK] Playback complete in %lu ms.\n", (unsigned long)(millis() - t0));
  return true;
}

// =====================================================================
//  MIC RECORDING
// =====================================================================
bool recordFromMic(int16_t *buffer, size_t numSamples, uint32_t sampleRate) {
  Serial.printf("[MIC] Recording %u samples @ %lu Hz (%.1f s)...\n",
                (unsigned)numSamples, (unsigned long)sampleRate,
                numSamples / (float)sampleRate);
  uint32_t t0 = millis();

  const uint32_t periodUs = 1000000UL / sampleRate;
  uint32_t nextSampleTime = micros();

  for (size_t i = 0; i < numSamples; i++) {
    while ((int32_t)(micros() - nextSampleTime) < 0) { }
    nextSampleTime += periodUs;

    int raw = analogRead(MIC_PIN);
    buffer[i] = (int16_t)((raw - 2048) * 16);

    if ((i & 0x3FF) == 0) yield();
  }

  Serial.printf("[MIC] Recording complete in %lu ms.\n", (unsigned long)(millis() - t0));
  return true;
}

// =====================================================================
//  NETWORK HELPERS (shared by video + audio)
// =====================================================================
void sendUint32BE(WiFiClient& client, uint32_t value) {
  uint8_t buf[4] = {
    (uint8_t)(value >> 24), (uint8_t)(value >> 16),
    (uint8_t)(value >> 8),  (uint8_t)(value)
  };
  client.write(buf, 4);
}

bool recvExact(WiFiClient& client, uint8_t* dest, size_t n, uint32_t timeoutMs = 15000) {
  size_t got = 0;
  uint32_t start = millis();
  while (got < n) {
    if (!client.connected() && client.available() == 0) return false;
    int avail = client.available();
    if (avail > 0) {
      int toRead = min((size_t)avail, n - got);
      int r = client.read(dest + got, toRead);
      if (r > 0) {
        got += r;
        start = millis();
      }
    } else {
      if (millis() - start > timeoutMs) return false;
      delay(5);
    }
  }
  return true;
}

bool recvUint32BE(WiFiClient& client, uint32_t& value, uint32_t timeoutMs = 15000) {
  uint8_t buf[4];
  if (!recvExact(client, buf, 4, timeoutMs)) return false;
  value = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
          ((uint32_t)buf[2] << 8)  | (uint32_t)buf[3];
  return true;
}

// =====================================================================
//  VIDEO: ALLOCATE / CAPTURE / UPLOAD
// =====================================================================
void freeVideoBuffer() {
  if (videoBuffer) {
    Serial.println("[VIDEO] Freeing previous video buffer.");
    heap_caps_free(videoBuffer);
    videoBuffer = nullptr;
  }
  hasBufferedVideo = false;
}

bool allocateVideoBuffer() {
  uint32_t target = VIDEO_DESIRED_FRAMES;
  Serial.printf("[VIDEO] Want %u frames (%u bytes/frame = %u bytes total)\n",
                target, (unsigned)VIDEO_FRAME_BYTES, (unsigned)(target * VIDEO_FRAME_BYTES));

  if (psramFound()) {
    size_t freePsram = ESP.getFreePsram();
    size_t usable = (size_t)(freePsram * 0.6);
    uint32_t maxFit = usable / VIDEO_FRAME_BYTES;
    Serial.printf("[VIDEO] freePsram=%u usable(60%%)=%u -> maxFit=%u frames\n",
                  (unsigned)freePsram, (unsigned)usable, maxFit);
    if (maxFit < target) {
      Serial.printf("[VIDEO] WARNING: only room for %u frames, shortening clip.\n", maxFit);
      target = maxFit;
    }
  } else {
    Serial.println("[VIDEO] WARNING: no PSRAM found, trying a small internal-RAM clip.");
    target = min(target, (uint32_t)20);
  }

  while (target >= VIDEO_MIN_FRAMES) {
    uint32_t caps = psramFound() ? MALLOC_CAP_SPIRAM : MALLOC_CAP_8BIT;
    videoBuffer = (uint8_t*) heap_caps_malloc((size_t)target * VIDEO_FRAME_BYTES, caps);
    if (videoBuffer != nullptr) {
      videoRecordFrames = target;
      Serial.printf("[VIDEO] Allocated buffer for %u frames (%.1fs @ %u fps).\n",
                    videoRecordFrames, (float)videoRecordFrames / VIDEO_FPS, VIDEO_FPS);
      return true;
    }
    Serial.printf("[VIDEO] Alloc of %u frames failed, backing off...\n", target);
    target /= 2;
  }

  Serial.println("[VIDEO] ERROR: could not allocate any video buffer.");
  return false;
}

void captureVideoClip() {
  if (!cameraReady || videoBuffer == nullptr) {
    Serial.println("[VIDEO] ERROR: camera not ready or buffer not allocated, aborting capture.");
    return;
  }

  Serial.printf("[VIDEO] Discarding %u warm-up frames...\n", VIDEO_WARMUP_FRAMES);
  for (uint32_t i = 0; i < VIDEO_WARMUP_FRAMES; i++) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  Serial.println("[VIDEO] Capturing clip...");
  uint32_t t0 = millis();
  bool haveGoodFrame = false;
  uint32_t badFrames = 0;

  for (uint32_t i = 0; i < videoRecordFrames; i++) {
    uint32_t frameStart = millis();
    uint8_t* slot = videoBuffer + (size_t)i * VIDEO_FRAME_BYTES;

    camera_fb_t* fb = esp_camera_fb_get();
    bool ok = fb && fb->len == VIDEO_FRAME_BYTES;

    if (ok) {
      memcpy(slot, fb->buf, VIDEO_FRAME_BYTES);
      haveGoodFrame = true;
    } else {
      badFrames++;
      if (haveGoodFrame) {
        memcpy(slot, slot - VIDEO_FRAME_BYTES, VIDEO_FRAME_BYTES);
      } else {
        memset(slot, 0x00, VIDEO_FRAME_BYTES);
      }
      Serial.printf("[VIDEO] Frame %u bad (fb=%p len=%u), patched.\n",
                    i, (void*)fb, fb ? (unsigned)fb->len : 0);
    }

    if (fb) esp_camera_fb_return(fb);

    if ((i % 20) == 0) {
      Serial.printf("[VIDEO] ...frame %u/%u captured\n", i, videoRecordFrames);
    }

    uint32_t elapsed = millis() - frameStart;
    if (elapsed < VIDEO_FRAME_INTERVAL_MS) delay(VIDEO_FRAME_INTERVAL_MS - elapsed);
  }

  hasBufferedVideo = true;
  Serial.printf("[VIDEO] Capture complete: %u frames, %u bad/patched, took %lu ms.\n",
                videoRecordFrames, badFrames, (unsigned long)(millis() - t0));
}

bool uploadVideoClip() {
  if (!hasBufferedVideo || videoBuffer == nullptr) {
    Serial.println("[VIDEO] No buffered clip to upload.");
    return false;
  }

  const int MAX_ATTEMPTS = 5;
  uint32_t backoffMs = 500;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[VIDEO][NET] WiFi down, reconnecting...");
      WiFi.reconnect();
      delay(1000);
    }

    WiFiClient client;
    Serial.printf("[VIDEO][NET] Attempt %d/%d: connecting to %s:%u...\n",
                  attempt, MAX_ATTEMPTS, VIDEO_BACKEND_HOST, VIDEO_BACKEND_PORT);

    if (client.connect(VIDEO_BACKEND_HOST, VIDEO_BACKEND_PORT)) {
      Serial.println("[VIDEO][NET] Connected. Sending header + frames...");
      char header[128];
      snprintf(header, sizeof(header),
               "{\"width\":%u,\"height\":%u,\"format\":\"rgb565\",\"fps\":%u}\n",
               VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_FPS);
      client.print(header);
      Serial.printf("[VIDEO][NET] Header sent: %s", header);

      uint32_t t0 = millis();
      bool ok = true;
      for (uint32_t i = 0; i < videoRecordFrames && ok; i++) {
        uint8_t* slot = videoBuffer + (size_t)i * VIDEO_FRAME_BYTES;
        sendUint32BE(client, VIDEO_FRAME_BYTES);
        size_t written = client.write(slot, VIDEO_FRAME_BYTES);
        if (written != VIDEO_FRAME_BYTES || !client.connected()) {
          Serial.printf("[VIDEO][NET] Frame %u send failed (written=%u).\n", i, (unsigned)written);
          ok = false;
        }
        if ((i % 20) == 0) {
          Serial.printf("[VIDEO][NET] ...sent frame %u/%u\n", i, videoRecordFrames);
        }
      }

      if (ok) {
        sendUint32BE(client, 0);
        client.stop();
        Serial.printf("[VIDEO][NET] Upload complete in %lu ms.\n", (unsigned long)(millis() - t0));
        return true;
      }
      client.stop();
      Serial.println("[VIDEO][NET] Upload dropped mid-stream, will retry.");
    } else {
      Serial.println("[VIDEO][NET] Connect failed.");
    }

    Serial.printf("[VIDEO][NET] Backing off %lu ms before retry.\n", (unsigned long)backoffMs);
    delay(backoffMs);
    backoffMs *= 2;
  }

  Serial.println("[VIDEO][NET] Upload failed after all retries. Clip stays buffered -- send 'u' to retry.");
  return false;
}

void doVideoRecordAndUpload() {
  Serial.println("==================================================");
  Serial.println("[VIDEO] Starting record+upload sequence");
  printMemStatus("video-start");

  freeVideoBuffer();
  if (!allocateVideoBuffer()) {
    Serial.println("[VIDEO] Aborting: buffer allocation failed.");
    return;
  }
  captureVideoClip();
  printMemStatus("video-after-capture");
  uploadVideoClip();
  printMemStatus("video-end");
  Serial.println("[VIDEO] Sequence done.");
  Serial.println("==================================================");
}

// =====================================================================
//  AUDIO: RECORD / SEND / RECEIVE / PLAY
// =====================================================================
bool sendAudioAndPlayResponse(const int16_t* buffer, size_t numSamples) {
  const size_t pcmBytes = numSamples * sizeof(int16_t);
  const int MAX_ATTEMPTS = 3;
  uint32_t backoffMs = 500;

  for (int attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[AUDIO][NET] WiFi down, reconnecting...");
      WiFi.reconnect();
      delay(1000);
    }

    WiFiClient client;
    Serial.printf("[AUDIO][NET] Attempt %d/%d: connecting to %s:%u...\n",
                  attempt, MAX_ATTEMPTS, AUDIO_BACKEND_HOST, AUDIO_BACKEND_PORT);

    if (!client.connect(AUDIO_BACKEND_HOST, AUDIO_BACKEND_PORT)) {
      Serial.println("[AUDIO][NET] Connect failed.");
      delay(backoffMs);
      backoffMs *= 2;
      continue;
    }

    char header[128];
    snprintf(header, sizeof(header),
             "{\"sample_rate\":%d,\"channels\":1,\"bits\":16,\"samples\":%u}\n",
             AUDIO_SAMPLE_RATE, (unsigned)numSamples);
    client.print(header);
    Serial.printf("[AUDIO][NET] Header sent: %s", header);

    sendUint32BE(client, (uint32_t)pcmBytes);
    uint32_t t0 = millis();
    size_t written = client.write((const uint8_t*)buffer, pcmBytes);
    Serial.printf("[AUDIO][NET] Sent %u/%u bytes in %lu ms.\n",
                  (unsigned)written, (unsigned)pcmBytes, (unsigned long)(millis() - t0));

    if (written != pcmBytes) {
      Serial.println("[AUDIO][NET] Send incomplete, retrying.");
      client.stop();
      delay(backoffMs);
      backoffMs *= 2;
      continue;
    }

    Serial.println("[AUDIO][NET] Waiting for response length...");
    uint32_t respBytes = 0;
    if (!recvUint32BE(client, respBytes, 20000)) {
      Serial.println("[AUDIO][NET] No response length received, retrying.");
      client.stop();
      delay(backoffMs);
      backoffMs *= 2;
      continue;
    }
    Serial.printf("[AUDIO][NET] Response length = %u bytes\n", (unsigned)respBytes);

    if (respBytes == 0) {
      Serial.println("[AUDIO][NET] Backend sent no response audio.");
      client.stop();
      return true;
    }

    if (respBytes > MAX_AUDIO_RESPONSE_BYTES) {
      Serial.printf("[AUDIO][NET] ERROR: response too large (%u > cap %u), aborting.\n",
                    (unsigned)respBytes, (unsigned)MAX_AUDIO_RESPONSE_BYTES);
      client.stop();
      return false;
    }

    uint8_t* respBuf = (uint8_t*) ps_malloc(respBytes);
    if (!respBuf) {
      Serial.println("[AUDIO][NET] ERROR: could not allocate response buffer.");
      client.stop();
      return false;
    }

    Serial.println("[AUDIO][NET] Reading response body...");
    uint32_t t1 = millis();
    if (!recvExact(client, respBuf, respBytes, 20000)) {
      Serial.println("[AUDIO][NET] Response read incomplete, retrying.");
      free(respBuf);
      client.stop();
      delay(backoffMs);
      backoffMs *= 2;
      continue;
    }
    Serial.printf("[AUDIO][NET] Response received (%u bytes) in %lu ms.\n",
                  (unsigned)respBytes, (unsigned long)(millis() - t1));

    client.stop();
    playAudioBuffer((const int16_t*)respBuf, respBytes / sizeof(int16_t));
    free(respBuf);
    return true;
  }

  Serial.println("[AUDIO][NET] Giving up after all retries.");
  return false;
}

void doAudioRecordSendPlay() {
  Serial.println("==================================================");
  Serial.println("[AUDIO] Starting record+send+respond sequence");
  printMemStatus("audio-start");

  if (audioRecordBuffer == nullptr) {
    Serial.println("[AUDIO] ERROR: record buffer not allocated (setup issue). Aborting.");
    return;
  }
  if (!speakerReady) {
    Serial.println("[AUDIO] WARNING: speaker not ready, response won't be audible.");
  }

  recordFromMic(audioRecordBuffer, AUDIO_RECORD_SAMPLES, AUDIO_SAMPLE_RATE);
  printMemStatus("audio-after-record");
  sendAudioAndPlayResponse(audioRecordBuffer, AUDIO_RECORD_SAMPLES);
  printMemStatus("audio-end");
  Serial.println("[AUDIO] Sequence done.");
  Serial.println("==================================================");
}

// =====================================================================
//  SETUP
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("==================================================");
  Serial.println("[BOOT] ESP32-CAM (AI Thinker) combined video+audio firmware");
  Serial.println("==================================================");

  if (!psramFound()) {
    Serial.println("[BOOT] WARNING: PSRAM not detected. Video buffering will be very limited.");
  } else {
    Serial.printf("[BOOT] PSRAM OK, %u bytes free.\n", (unsigned)ESP.getFreePsram());
  }

  if (!LittleFS.begin(true)) {
    Serial.println("[BOOT] ERROR: LittleFS mount failed.");
  } else {
    Serial.println("[BOOT] LittleFS mounted.");
  }

  cameraReady = initCamera();

  analogReadResolution(12);
  analogSetPinAttenuation(MIC_PIN, ADC_11db);
  Serial.printf("[BOOT] Mic input ready on GPIO%d\n", MIC_PIN);

  setupI2SSpeaker();

  connectWiFi();

  Serial.println("[BOOT] Allocating audio record buffer in PSRAM...");
  audioRecordBuffer = (int16_t *)ps_malloc(AUDIO_RECORD_SAMPLES * sizeof(int16_t));
  if (!audioRecordBuffer) {
    Serial.println("[BOOT] ERROR: failed to allocate audio buffer. Audio commands will fail.");
  } else {
    Serial.printf("[BOOT] Audio buffer OK (%u bytes).\n",
                  (unsigned)(AUDIO_RECORD_SAMPLES * sizeof(int16_t)));
  }

  printMemStatus("boot-complete");
  Serial.printf("[BOOT] cameraReady=%d speakerReady=%d\n", cameraReady, speakerReady);

  printHelp();
}

// =====================================================================
//  LOOP -- serial command dispatch
// =====================================================================
void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'v': case 'V':
        doVideoRecordAndUpload();
        break;
      case 'u': case 'U':
        Serial.println("[CMD] Re-upload requested.");
        uploadVideoClip();
        break;
      case 'a': case 'A':
        doAudioRecordSendPlay();
        break;
      case 'p': case 'P':
        Serial.println("[CMD] Play startup clip requested.");
        playPCMFile(STARTUP_FILE);
        break;
      case 's': case 'S':
        Serial.println("[CMD] Status requested.");
        printWiFiStatus();
        printMemStatus("status");
        Serial.printf("[STATUS] cameraReady=%d speakerReady=%d\n", cameraReady, speakerReady);
        Serial.printf("[STATUS] hasBufferedVideo=%d videoRecordFrames=%u\n",
                      hasBufferedVideo, videoRecordFrames);
        break;
      case 'h': case 'H': case '?':
        printHelp();
        break;
      case '\n': case '\r':
        break; // ignore
      default:
        Serial.printf("[CMD] Unknown command '%c'. Type 'h' for help.\n", c);
        break;
    }
  }
  delay(10);
}

/*
  PREPARING THE STARTUP CLIP ("/startup.pcm")
  ----------------------------------------------------------------------
  1. ffmpeg -i input.mp3 -ar 16000 -ac 1 -f s16le startup.pcm
  2. Put startup.pcm inside a "data" folder next to this .ino file.
  3. Upload via Tools > "ESP32 LittleFS Data Upload" (Arduino IDE) or
     `pio run --target uploadfs` (PlatformIO).
*/
