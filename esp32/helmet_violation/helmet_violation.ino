#include <WiFi.h>
#include <esp_camera.h>
#include <HTTPClient.h>
#include <U8g2lib.h>
#include "sxmnath-project-1_inferencing.h"

// ── Config — fill these in ────────────────────────────────────────
#define WIFI_SSID          "YOUR_WIFI_SSID"
#define WIFI_PASS          "YOUR_WIFI_PASSWORD"
#define SERVER_URL         "https://YOUR_RENDER_APP.onrender.com/violation"
#define CONFIDENCE_THRESH  0.75f
#define LOOP_DELAY_MS      2000

// ── OLED: SCL=IO15, SDA=IO14 ─────────────────────────────────────
U8G2_SSD1306_128X64_NONAME_F_SW_I2C
  u8g2(U8G2_R0, 15, 14, U8X8_PIN_NONE);

// ── Camera frame buffer ───────────────────────────────────────────
camera_fb_t* fb = nullptr;

// ── OLED helper ───────────────────────────────────────────────────
void showOLED(const char* l1, const char* l2 = "", const char* l3 = "") {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 12, l1);
  if (strlen(l2)) u8g2.drawStr(0, 28, l2);
  if (strlen(l3)) u8g2.drawStr(0, 44, l3);
  u8g2.sendBuffer();
}

// ── Camera init (your exact pinout) ──────────────────────────────
bool initCamera(pixformat_t fmt, framesize_t size, int fb_count) {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = 5;
  config.pin_d1       = 18;
  config.pin_d2       = 19;
  config.pin_d3       = 21;
  config.pin_d4       = 36;
  config.pin_d5       = 39;
  config.pin_d6       = 34;
  config.pin_d7       = 35;
  config.pin_xclk     = 0;
  config.pin_pclk     = 22;
  config.pin_vsync    = 25;
  config.pin_href     = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn     = 32;
  config.pin_reset    = -1;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = fmt;
  config.frame_size   = size;
  config.jpeg_quality = 10;
  config.fb_count     = fb_count;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }
  return true;
}

// ── Edge Impulse: get image data from frame buffer ────────────────
static camera_fb_t* ei_fb = nullptr;

int ei_camera_get_data(size_t offset, size_t length, float* out_ptr) {
  size_t pixel_ix   = offset * 3;
  size_t pixels_left = length;
  size_t out_ptr_ix  = 0;

  while (pixels_left > 0) {
    out_ptr[out_ptr_ix] = (ei_fb->buf[pixel_ix]     << 16)
                        | (ei_fb->buf[pixel_ix + 1] << 8)
                        |  ei_fb->buf[pixel_ix + 2];
    out_ptr_ix++;
    pixel_ix   += 3;
    pixels_left--;
  }
  return 0;
}

// ── Run helmet inference on current RGB frame ─────────────────────
float runHelmetInference() {
  // Capture RGB888 frame for Edge Impulse
  esp_camera_deinit();
  if (!initCamera(PIXFORMAT_RGB888, FRAMESIZE_QVGA, 1)) return 0.0f;

  ei_fb = esp_camera_fb_get();
  if (!ei_fb) {
    Serial.println("RGB capture failed");
    return 0.0f;
  }

  ei::signal_t signal;
  signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
  signal.get_data     = &ei_camera_get_data;

  ei_impulse_result_t result = {0};
  EI_IMPULSE_ERROR res = run_classifier(&signal, &result, false);

  esp_camera_fb_return(ei_fb);
  ei_fb = nullptr;

  if (res != EI_IMPULSE_OK) {
    Serial.printf("Classifier error: %d\n", res);
    return 0.0f;
  }

  // Print all labels
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    Serial.printf("  %s: %.2f\n",
      result.classification[i].label,
      result.classification[i].value);
  }

  // Find "no helmet" or "without_helmet" label index
  // Check your Edge Impulse label order in the serial output
  // Typically index 1 = no_helmet — adjust if needed
  float no_helmet_score = 0.0f;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
    String lbl = String(result.classification[i].label);
    lbl.toLowerCase();
    if (lbl.indexOf("no") >= 0 || lbl.indexOf("without") >= 0) {
      no_helmet_score = result.classification[i].value;
      break;
    }
  }
  return no_helmet_score;
}

// ── POST plate crop JPEG to Flask server ──────────────────────────
String postViolation(float confidence) {
  // Switch to JPEG for HTTP POST
  esp_camera_deinit();
  if (!initCamera(PIXFORMAT_JPEG, FRAMESIZE_VGA, 1)) return "CAM_ERR";

  camera_fb_t* jpeg_fb = esp_camera_fb_get();
  if (!jpeg_fb) return "CAPTURE_ERR";

  String plate = "UNKNOWN";

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.setTimeout(15000);

    String boundary = "----ESP32Boundary7MA4YWxkTrZu0gW";
    String part_head = "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"plate_crop\"; filename=\"plate.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
    String part_conf = "\r\n--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"confidence\"\r\n\r\n"
      + String(confidence, 4) +
      "\r\n--" + boundary + "--\r\n";

    size_t   total = part_head.length() + jpeg_fb->len + part_conf.length();
    uint8_t* body  = (uint8_t*)malloc(total);

    if (body) {
      size_t pos = 0;
      memcpy(body + pos, part_head.c_str(), part_head.length()); pos += part_head.length();
      memcpy(body + pos, jpeg_fb->buf,      jpeg_fb->len);        pos += jpeg_fb->len;
      memcpy(body + pos, part_conf.c_str(), part_conf.length());

      http.addHeader("Content-Type",
                     "multipart/form-data; boundary=" + boundary);
      int code = http.POST(body, total);
      free(body);

      if (code == 200) {
        String resp = http.getString();
        Serial.println("Server resp: " + resp);
        int start = resp.indexOf("\"plate\":\"") + 9;
        if (start > 8) {
          int end = resp.indexOf("\"", start);
          plate = resp.substring(start, end);
        }
      } else {
        Serial.printf("POST failed: HTTP %d\n", code);
      }
    } else {
      Serial.println("malloc failed");
    }
    http.end();
  } else {
    Serial.println("WiFi not connected");
  }

  esp_camera_fb_return(jpeg_fb);
  return plate;
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  u8g2.begin();
  showOLED("Booting...");

  // Connect Wi-Fi
  showOLED("Connecting WiFi", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - t > 15000) {
      showOLED("WiFi FAILED", "Check creds");
      while (true) delay(1000);
    }
    delay(300);
    Serial.print(".");
  }
  Serial.println("\nWiFi: " + WiFi.localIP().toString());
  showOLED("WiFi OK", WiFi.localIP().toString().c_str());
  delay(1000);

  showOLED("System Ready", "Detecting...");
}

// ── Main loop ─────────────────────────────────────────────────────
void loop() {
  // Reconnect Wi-Fi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    showOLED("WiFi lost...", "Reconnecting");
    WiFi.reconnect();
    delay(3000);
    return;
  }

  // Keepalive ping every 10 mins to prevent Render cold start
  static unsigned long lastPing = 0;
  if (millis() - lastPing > 600000) {
    HTTPClient http;
    http.begin(String(SERVER_URL).substring(0,
      String(SERVER_URL).lastIndexOf('/')) + "/health");
    http.GET();
    http.end();
    lastPing = millis();
    Serial.println("Keepalive ping sent");
  }

  showOLED("Scanning...");
  Serial.println("\n--- New detection cycle ---");

  float score = runHelmetInference();
  Serial.printf("No-helmet score: %.3f\n", score);

  if (score >= CONFIDENCE_THRESH) {
    // VIOLATION
    showOLED("NO HELMET",
             "Uploading...",
             (String(score * 100, 0) + "% conf").c_str());
    Serial.println("VIOLATION — posting to server");

    String plate = postViolation(score);

    String plateLine = "Plate: " + plate;
    String confLine  = String(score * 100, 0) + "% conf";
    showOLED("VIOLATION!", plateLine.c_str(), confLine.c_str());
    Serial.println("Plate: " + plate);
    delay(4000);   // hold result on screen

  } else {
    showOLED("HELMET OK",
             (String(score * 100, 0) + "%").c_str());
  }

  // Switch back to RGB for next inference cycle
  esp_camera_deinit();
  initCamera(PIXFORMAT_RGB888, FRAMESIZE_QVGA, 1);

  delay(LOOP_DELAY_MS);
}