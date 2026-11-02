#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

const char* ssid = "Lucky";
const char* password = "22222222";

// Optional static IP section
// Uncomment and edit only if you want a fixed IP
/*
IPAddress local_IP(10, 223, 31, 130);
IPAddress gateway(10, 223, 31, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);
*/

httpd_handle_t server = NULL;
volatile bool captureBusy = false;

static esp_err_t root_handler(httpd_req_t *req) {
  const char* msg = "ESP32-CAM is running. Use /capture";
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_send(req, msg, strlen(msg));
}

static esp_err_t health_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"ok\":true,\"device\":\"esp32-cam\"}", -1);
}

static esp_err_t capture_handler(httpd_req_t *req) {
  if (captureBusy) {
    httpd_resp_set_status(req, "429 Too Many Requests");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":false,\"message\":\"capture already in progress\"}", -1);
  }

  captureBusy = true;

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");

  camera_fb_t *fb = NULL;
  camera_fb_t *fb2 = NULL;

  // Throw away first frame after idle, then use second frame
  fb = esp_camera_fb_get();
  if (fb) {
    esp_camera_fb_return(fb);
    fb = NULL;
    delay(30);
  }

  fb2 = esp_camera_fb_get();
  if (!fb2) {
    Serial.println("Camera capture failed");
    captureBusy = false;
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  esp_err_t res = httpd_resp_set_type(req, "image/jpeg");
  if (res == ESP_OK) {
    res = httpd_resp_send(req, (const char*)fb2->buf, fb2->len);
  }

  esp_camera_fb_return(fb2);
  captureBusy = false;

  if (res == ESP_OK) {
    Serial.println("Image captured and sent");
  } else {
    Serial.println("Failed to send image");
  }

  return res;
}

void startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32768;
  config.recv_wait_timeout = 10;
  config.send_wait_timeout = 15;
  config.max_uri_handlers = 8;

  httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_handler,
    .user_ctx = NULL
  };

  httpd_uri_t health_uri = {
    .uri = "/health",
    .method = HTTP_GET,
    .handler = health_handler,
    .user_ctx = NULL
  };

  httpd_uri_t capture_uri = {
    .uri = "/capture",
    .method = HTTP_GET,
    .handler = capture_handler,
    .user_ctx = NULL
  };

  if (httpd_start(&server, &config) == ESP_OK) {
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &health_uri);
    httpd_register_uri_handler(server, &capture_uri);
    Serial.println("HTTP server started");
  } else {
    Serial.println("Failed to start HTTP server");
  }
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (psramFound()) {
    config.frame_size = FRAMESIZE_QVGA;   // stable
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 0);
    s->set_contrast(s, 0);
    s->set_saturation(s, 0);
  }

  // Warm-up captures
  for (int i = 0; i < 2; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      esp_camera_fb_return(fb);
    }
    delay(50);
  }

  return true;
}

bool connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  // Optional static IP
  /*
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("Static IP config failed");
  }
  */

  WiFi.begin(ssid, password);

  Serial.print("Connecting to WiFi");
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startAttemptTime > 20000) {
      Serial.println("\nWiFi connection timeout");
      return false;
    }
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected");
  Serial.print("ESP32-CAM IP: ");
  Serial.println(WiFi.localIP());

  Serial.print("Health URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/health");

  Serial.print("Capture URL: http://");
  Serial.print(WiFi.localIP());
  Serial.println("/capture");

  return true;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  if (!initCamera()) {
    Serial.println("Restarting due to camera error...");
    delay(3000);
    ESP.restart();
  }

  if (!connectWiFi()) {
    Serial.println("Restarting due to WiFi error...");
    delay(3000);
    ESP.restart();
  }

  startServer();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnected. Restarting...");
    delay(1000);
    ESP.restart();
  }

  delay(1000);
}
