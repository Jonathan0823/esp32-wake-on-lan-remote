#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_system.h>
#include "config.h"
#include "src/services/wake_service.h"
#include "src/services/telegram_service.h"
#include "src/services/log_service.h"

static WiFiUDP wakeUdp;

static bool wakePending = false;
static unsigned long wakeStartMs = 0;
static unsigned long lastWakeCheck = 0;
static String wakeChatId = "";
static String wakeDeviceName = "";
static String wakeDeviceIp = "";
static int wakeDeviceProbePort = 0;
static String wakeId;

static const unsigned long WAKE_TIMEOUT_MS = 90000;
static const unsigned long WAKE_RETRY_MS = 3000;

static void wake_clear() {
  wakePending = false;
  wakeChatId = "";
  wakeDeviceName = "";
  wakeDeviceIp = "";
  wakeDeviceProbePort = 0;
  wakeId = "";
  wakeStartMs = 0;
  lastWakeCheck = 0;
}

String wake_new_id() {
  char id[9];
  snprintf(id, sizeof(id), "%08lx", (unsigned long)esp_random());
  return String(id);
}

static void log_telegram_wake_result(const char* result) {
  if (!log_enabled()) return;
  DynamicJsonDocument fields(256);
  fields["wake_id"] = wakeId;
  fields["source"] = "telegram";
  fields["target"] = wakeDeviceName;
  fields["result"] = result;
  fields["elapsed_ms"] = millis() - wakeStartMs;
  log_event_fields(strcmp(result, "success") == 0 ? "info" : "warn",
                   "wake", "result", fields.as<JsonObjectConst>());
}

bool wake_is_pending() {
  return wakePending;
}

void wake_poll() {
  if (!wakePending) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (millis() - lastWakeCheck < WAKE_RETRY_MS) return;

  if (wake_is_pc_reachable(wakeDeviceIp, wakeDeviceProbePort)) {
    log_telegram_wake_result("success");
    telegram_send_text_once(wakeChatId,
                            "🖥 " + wakeDeviceName + " is now online! (took "
                            + String((millis() - wakeStartMs) / 1000) + "s)",
                            "");
    wake_clear();
    return;
  }

  if (millis() - wakeStartMs >= WAKE_TIMEOUT_MS) {
    log_telegram_wake_result("timeout");
    telegram_send_text_once(wakeChatId,
                            "⚠️ " + wakeDeviceName + " did not wake within "
                            + String(WAKE_TIMEOUT_MS / 1000)
                            + "s. Check BIOS WoL settings.",
                            "");
    wake_clear();
    return;
  }

  lastWakeCheck = millis();
}

bool wake_send_magic(const String& mac, const String& bcast, int port) {
  byte macBytes[6];
  sscanf(mac.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
         &macBytes[0], &macBytes[1], &macBytes[2],
         &macBytes[3], &macBytes[4], &macBytes[5]);
  byte packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(&packet[6 + i * 6], macBytes, 6);
  IPAddress bcastAddr;
  bcastAddr.fromString(bcast);
  bool began = wakeUdp.beginPacket(bcastAddr, port) == 1;
  size_t written = began ? wakeUdp.write(packet, sizeof(packet)) : 0;
  bool sent = began && wakeUdp.endPacket() == 1 && written == sizeof(packet);
  log_print("WoL %s to %s via %s:%d\n", sent ? "sent" : "send failed",
            mac.c_str(), bcast.c_str(), port);
  return sent;
}

bool wake_is_pc_reachable(const String& ip, int port) {
  WiFiClient probe;
  uint32_t start = millis();
  log_print("[wake] probe %s:%d start\n", ip.c_str(), port);
  // ponytail: local network TCP connect should either succeed (<10ms)
  // or fail with RST (<100ms); 200ms is a safe ceiling
  bool r = probe.connect(ip.c_str(), port, 200);
  probe.stop();
  log_print("[wake] probe %s:%d %s in %lums\n",
                ip.c_str(),
                port,
                r ? "up" : "down",
                millis() - start);
  return r;
}

void wake_start_polling(String chatId, const String& deviceName,
                        const String& ip, int probePort, const String& id) {
  if (wakePending) {
    log_telegram_wake_result("superseded");
  }
  wakePending = true;
  wakeStartMs = millis();
  lastWakeCheck = millis();
  wakeChatId = chatId;
  wakeDeviceName = deviceName;
  wakeDeviceIp = ip;
  wakeDeviceProbePort = probePort;
  wakeId = id;
  log_print("[wake] pending target=%s chat=%s\n", deviceName.c_str(), chatId.c_str());
}
