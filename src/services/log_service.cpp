#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <time.h>
#include <stdarg.h>
#include "config.h"
#include "src/services/log_service.h"

static bool grafanaConfigured = false;
static unsigned long lastHeartbeatMs = 0;
const unsigned long HEARTBEAT_MS = 60000;
static log_callback_t externalLogCb = NULL;

static void escape_json(const char* in, char* out, size_t out_len) {
  size_t pos = 0;
  for (; *in && pos < out_len - 6; in++) {
    char c = *in;
    switch (c) {
      case '"': out[pos++] = '\\'; out[pos++] = '"'; break;
      case '\\': out[pos++] = '\\'; out[pos++] = '\\'; break;
      case '\n': out[pos++] = '\\'; out[pos++] = 'n'; break;
      case '\r': out[pos++] = '\\'; out[pos++] = 'r'; break;
      case '\t': out[pos++] = '\\'; out[pos++] = 't'; break;
      default: out[pos++] = c; break;
    }
  }
  out[pos] = '\0';
}

static void make_ts(char* buf, size_t len) {
  time_t now = time(nullptr);
  if (now > 100000) {
    snprintf(buf, len, "%llu", (unsigned long long)now * 1000000000ULL);
  } else {
    buf[0] = '\0';
  }
}

static void send_loki_line(const char* log_line) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String endpoint = String(GRAFANA_LOGS_URL) + "/loki/api/v1/push";
  if (!http.begin(client, endpoint)) return;

  // ponytail: Grafana logging is best-effort; cap wait so it can't stall the bot.
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  http.setReuse(false);
  http.setAuthorization(GRAFANA_LOGS_USER, GRAFANA_LOGS_TOKEN);
  http.addHeader("Content-Type", "application/json");

  char ts[32];
  make_ts(ts, sizeof(ts));

  char* escaped_log = (char*)malloc(1024);
  char* payload = (char*)malloc(2048);
  if (!escaped_log || !payload) {
    free(escaped_log);
    free(payload);
    return;
  }

  escape_json(log_line, escaped_log, 1024);

  int plen = snprintf(payload, 2048,
    "{\"streams\":[{\"stream\":{\"app\":\"esp32-pc-remote\"},\"values\":[[\"%s\",\"%s\"]]}]}",
    ts[0] ? ts : "0", escaped_log);

  int code = http.POST((uint8_t*)payload, plen);
  http.end();
  // ponytail: ignore HTTP errors — best-effort logging
  (void)code;
  free(escaped_log);
  free(payload);
}

bool log_enabled() {
  return grafanaConfigured;
}

void log_init() {
  grafanaConfigured =
    strlen(GRAFANA_LOGS_URL) > 0 &&
    strlen(GRAFANA_LOGS_USER) > 0 &&
    strlen(GRAFANA_LOGS_TOKEN) > 0;

  if (!grafanaConfigured) {
    log_print("[log] Grafana Cloud not configured — skipping\n");
    return;
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  log_print("[log] init NTP + Grafana Cloud logging\n");
  log_event("info", "boot", "start", "ESP32 PC Remote started");
}

void log_event(const char* level, const char* component,
               const char* event, const char* msg) {
  if (!grafanaConfigured) return;

  char* escaped = (char*)malloc(1024);
  char* log_line = (char*)malloc(1536);
  if (!escaped || !log_line) {
    free(escaped);
    free(log_line);
    return;
  }
  escape_json(msg, escaped, 1024);

  String ipStr;
  int rssi = 0;
  if (WiFi.status() == WL_CONNECTED) {
    ipStr = WiFi.localIP().toString();
    rssi = WiFi.RSSI();
  }

  int needed = snprintf(log_line, 1536,
    "{\"level\":\"%s\",\"component\":\"%s\",\"event\":\"%s\",\"msg\":\"%s\","
    "\"uptime_s\":%lu,\"heap\":%u,\"rssi\":%d,\"ip\":\"%s\"}",
    level, component, event, escaped,
    (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
    rssi, ipStr.c_str());

  if (needed > 0 && needed < 1536) {
    send_loki_line(log_line);
  } else {
    log_print("[log] truncated line skipped (grafana)\n");
  }

  free(escaped);
  free(log_line);
}

void log_event_fields(const char* level, const char* component,
                      const char* event, JsonObjectConst fields) {
  if (!grafanaConfigured) return;

  DynamicJsonDocument doc(1024);
  for (JsonPairConst field : fields) {
    doc[field.key()] = field.value();
  }
  doc["level"] = level;
  doc["component"] = component;
  doc["event"] = event;

  String ipStr;
  int rssi = 0;
  if (WiFi.status() == WL_CONNECTED) {
    ipStr = WiFi.localIP().toString();
    rssi = WiFi.RSSI();
  }
  doc["uptime_s"] = (unsigned long)(millis() / 1000);
  doc["heap"] = ESP.getFreeHeap();
  doc["rssi"] = rssi;
  doc["ip"] = ipStr;

  String logLine;
  serializeJson(doc, logLine);
  if (logLine.length() < 1024) {
    send_loki_line(logLine.c_str());
  } else {
    log_print("[log] truncated line skipped (grafana)\n");
  }
}

void log_print(const char* fmt, ...) {
  char buf[512];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.printf("[%lums] %s", millis(), buf);
  if (externalLogCb) {
    externalLogCb(buf);
  }
}

void log_set_callback(log_callback_t cb) {
  externalLogCb = cb;
}

void log_heartbeat(const String& targetName) {
  if (!grafanaConfigured) return;
  if (millis() - lastHeartbeatMs < HEARTBEAT_MS) return;
  lastHeartbeatMs = millis();
  String msg = "alive target=" + targetName;
  log_event("info", "heartbeat", "tick", msg.c_str());
}
