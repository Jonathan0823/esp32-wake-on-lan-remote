/*
 * ESP32 Telegram Bot — esp32-pc-remote
 *
 * Dependencies (install via Arduino CLI):
 *   arduino-cli core install esp32:esp32
 *
 * Setup:
 *   1. Copy config.example.h → config.h
 *   2. Fill in WIFI_SSID, WIFI_PASS, BOT_TOKEN and/or MQTT settings
 *   3. Compile & upload:
 *      arduino-cli compile --fqbn esp32:esp32:esp32 .
 *      arduino-cli upload --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 .
 */

#include <WiFi.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "src/services/wifi_service.h"
#include "src/services/telegram_service.h"
#include "src/services/wake_service.h"
#include "src/services/mqtt_service.h"
#include "src/services/log_service.h"

const char* reset_reason_label(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "other_wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    default: return "unknown";
  }
}

const char* current_reset_reason() {
  return reset_reason_label(esp_reset_reason());
}

void setup() {
  Serial.begin(115200);
  delay(50);
  log_print("\n=== ESP32 PC Remote ===\n");
  log_print("[boot] reset=%s heap=%u\n", current_reset_reason(), ESP.getFreeHeap());
  // ponytail: Telegram long-poll blocks loopTask for ~60s.
  //           MUST reconfigure WDT before any blocking call (wifi_connect)
  //           or the default 5s timeout will fire a boot-loop.
  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdtConfig = {120000, 0, true};
  esp_task_wdt_init(&wdtConfig);
  esp_task_wdt_add(NULL);
  log_print("[wdt] configured 120s\n");

  log_print("[boot] target=%s\n", PC_NAME);
  wifi_connect();
  log_init();
  telegram_setup();
  mqtt_setup();

  // log once if previous run was watchdog-triggered (WiFi is up after wifi_connect)
  if (esp_reset_reason() == ESP_RST_TASK_WDT) {
    log_event("warn", "wdt", "triggered", "Task watchdog triggered reset");
  }
}

void loop() {
  wifi_ensure();
  log_heartbeat(PC_NAME);

  if (WiFi.status() == WL_CONNECTED) {
    mqtt_loop();
    wake_poll();
  }

  esp_task_wdt_reset();
}
