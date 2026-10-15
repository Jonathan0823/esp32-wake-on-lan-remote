#include "src/services/mqtt_service.h"
#include "config.h"
#include "src/services/log_service.h"
#include "src/services/wake_service.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <stdlib.h>

// Declared in esp32-pc-remote.ino
extern const char *current_reset_reason();

// --- Connection ---
static WiFiClientSecure mqttTlsClient;
static PubSubClient mqttClient;
static bool mqttConfigured = false;
static bool mqttWasConnected = false;

// --- Base topic ---
static String baseTopic;

// --- Reconnect backoff ---
static unsigned long lastReconnectMs = 0;
static int reconnectDelayMs = 1000;
static const int RECONNECT_MAX_MS = 60000;

// --- Log rate limit ---
static unsigned long lastLogMs = 0;
static const unsigned long LOG_RATE_MS = 100;

// --- Wake tracking ---
static bool mqttWakePending = false;
static unsigned long wakeStartMs = 0;
static unsigned long lastWakeCheckMs = 0;
static String wakeTarget = "";
static String wakeId = "";
static String wakeOrigin = "";
static String wakeTargetIp = "";
static int wakeProbePort = 0;

static const unsigned long WAKE_TIMEOUT_MS = 45000;
static const unsigned long WAKE_RETRY_MS = 3000;
static const unsigned long CONFIRM_EXPIRES_MAX_S = 3600;

// --- Pending confirmations ---
static String wakeConfirmToken = "";
static unsigned long wakeConfirmExpiresMs = 0;
static String rebootConfirmToken = "";
static unsigned long rebootConfirmExpiresMs = 0;

// --- State tracking ---
static String lastWakeResult = "";
static unsigned long lastWakeAt = 0;
static bool lastPcOnline = false;
static bool havePcOnline = false;
static unsigned long lastStatePublishMs = 0;
static const unsigned long STATE_PUBLISH_MS = 60000;

// --- Topic helpers ---

static String availTopic()    { return baseTopic + "/availability"; }
static String stateTopic()    { return baseTopic + "/state"; }
static String cmdTopic()      { return baseTopic + "/cmd"; }
static String replyTopic()    { return baseTopic + "/reply"; }
static String eventTopic()    { return baseTopic + "/event"; }
static String logTopic()      { return baseTopic + "/log"; }

// --- Generate a confirm token ---
static String generate_token() {
  uint32_t r = esp_random();
  char buf[20];
  snprintf(buf, sizeof(buf), "cfm-%08x", (unsigned int)r);
  return String(buf);
}

// --- Publish helpers ---

static void mqtt_publish_availability(bool online, const char* reason) {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(128);
  doc["online"] = online;
  doc["reason"] = reason;
  if (online) {
    doc["ts"] = (unsigned long)(millis() / 1000);
  }
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(availTopic().c_str(), payload.c_str(), true);
  log_print("[mqtt] availability online=%d reason=%s\n", online, reason);
}

static bool mqtt_probe_pc_online() {
  if (WiFi.status() != WL_CONNECTED) return false;
  return wake_is_pc_reachable(PC_IP, PC_TCP_PORT);
}

static void mqtt_publish_state(bool probePc = false) {
  if (!mqttClient.connected()) return;
  if (probePc) {
    lastPcOnline = mqtt_probe_pc_online();
    havePcOnline = true;
  }

  DynamicJsonDocument doc(256);
  doc["online"] = true;
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["heap"] = ESP.getFreeHeap();
  doc["uptime_s"] = (unsigned long)(millis() / 1000);
  doc["pc_online"] = havePcOnline ? lastPcOnline : false;
  doc["pc_status"] = (havePcOnline && lastPcOnline) ? "online" : "offline";
  if (lastWakeResult.length() > 0) {
    doc["last_wake_result"] = lastWakeResult;
  }
  if (lastWakeAt > 0) {
    doc["last_wake_at"] = lastWakeAt;
  }
  doc["wake_pending"] = mqttWakePending;
  doc["ts"] = (unsigned long)(millis() / 1000);
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(stateTopic().c_str(), payload.c_str(), true);
  lastStatePublishMs = millis();
}

static void mqtt_publish_event(const char* event, const char* target) {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(128);
  doc["event"] = event;
  if (target && strlen(target) > 0) {
    doc["target"] = target;
  }
  doc["ts"] = (unsigned long)(millis() / 1000);
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(eventTopic().c_str(), payload.c_str(), false);
  log_print("[mqtt] event %s target=%s\n", event, target ? target : "");
}

static String mqtt_log_value(const String& value) {
  String safe;
  for (unsigned int i = 0; i < value.length() && safe.length() < 32; i++) {
    char c = value[i];
    safe += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')
                ? c
                : '_';
  }
  return safe;
}

static void mqtt_log_wake(const String& id, const char* event,
                          const String& target, const char* requestType = nullptr,
                          const char* result = nullptr, const char* reason = nullptr,
                          bool packetAttempted = false, bool sent = false,
                          unsigned long elapsedMs = 0,
                          const char* origin = nullptr) {
  if (!log_enabled()) return;
  DynamicJsonDocument fields(512);
  fields["wake_id"] = id;
  fields["source"] = "mqtt";
  fields["target"] = mqtt_log_value(target.length() > 0 ? target : String(PC_NAME));
  if (origin) fields["origin"] = mqtt_log_value(origin);
  if (requestType) fields["request_type"] = requestType;
  if (result) fields["result"] = result;
  if (reason) fields["reason"] = reason;
  if (packetAttempted) {
    fields["sent"] = sent;
    fields["target_mac"] = PC_MAC;
    fields["broadcast"] = WOL_BCAST;
    fields["port"] = WOL_PORT;
  }
  if (elapsedMs > 0) fields["elapsed_ms"] = elapsedMs;
  const char* level = reason || (packetAttempted && !sent) ? "warn" : "info";
  log_event_fields(level, "wake", event, fields.as<JsonObjectConst>());
}

static void mqtt_publish_reply(const String& reqId, const String& cmdName,
                                bool ok, DynamicJsonDocument& extra) {
  if (!mqttClient.connected()) return;
  DynamicJsonDocument doc(512);
  doc["id"] = reqId;
  doc["cmd"] = cmdName;
  doc["ok"] = ok;
  doc["ts"] = (unsigned long)(millis() / 1000);
  for (JsonPair kv : extra.as<JsonObject>()) {
    doc[kv.key()] = kv.value();
  }
  String payload;
  serializeJson(doc, payload);
  mqttClient.publish(replyTopic().c_str(), payload.c_str(), false);
}

// --- Command handlers ---

static void handle_ping(const String& reqId) {
  DynamicJsonDocument extra(256);
  extra["online"] = true;
  extra["ip"] = WiFi.localIP().toString();
  extra["rssi"] = WiFi.RSSI();
  extra["heap"] = ESP.getFreeHeap();
  extra["uptime_s"] = (unsigned long)(millis() / 1000);
  extra["reset_reason"] = current_reset_reason();
  mqtt_publish_reply(reqId, "ping", true, extra);
  // ponytail: ping also updates state so frontend gets fresh data
  mqtt_publish_state(true);
  log_print("[mqtt] cmd ping handled\n");
}

static void mqtt_begin_wake(const char* target, const String& reqId,
                            const char* cmdName, bool forced,
                            const String& id) {
  bool sent = wake_send_magic(PC_MAC, WOL_BCAST, WOL_PORT);
  wakeId = id;
  mqtt_log_wake(wakeId, "packet_sent", target, nullptr,
                sent ? "sent" : "send_failed", nullptr, true, sent, 0,
                wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);

  lastWakeResult = "wol_sent";
  lastWakeAt = millis() / 1000;

  mqttWakePending = true;
  wakeStartMs = millis();
  lastWakeCheckMs = millis();
  wakeTarget = target;
  wakeTargetIp = PC_IP;
  wakeProbePort = PC_TCP_PORT;
  wakeConfirmToken = "";
  wakeConfirmExpiresMs = 0;

  DynamicJsonDocument extra(128);
  extra["target"] = target;
  extra["result"] = "wol_sent";
  if (forced) {
    extra["forced"] = true;
  }
  mqtt_publish_reply(reqId, cmdName, true, extra);
  mqtt_publish_state(true);
  mqtt_publish_event("wol_sent", target);
  log_print("[mqtt] wake sent target=%s forced=%d\n", target, forced);
}

static void handle_wake_request(const String& reqId, DynamicJsonDocument& doc) {
  const char* target = doc["target"] | "";
  unsigned long expiresIn = doc["expires_in_s"] | 30;
  bool forceWake = doc["force"] | false;
  String origin = doc["origin"] | "";
  if (origin.length() > 32) origin.remove(32);
  if (wakeId.length() > 0 &&
      (wakeConfirmToken.length() > 0 || (forceWake && mqttWakePending))) {
    mqtt_log_wake(wakeId, "result", wakeTarget, "wake_request", "superseded",
                  nullptr, false, false, 0,
                  wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);
  }
  String requestWakeId = wake_new_id();
  mqtt_log_wake(requestWakeId, "request_received", target, "wake_request",
                nullptr, nullptr, false, false, 0,
                origin.length() > 0 ? origin.c_str() : nullptr);

  if (forceWake) {
    wakeOrigin = origin;
    mqtt_begin_wake(target, reqId, "wake_request", true, requestWakeId);
    return;
  }

  if (mqttWakePending) {
    mqtt_log_wake(requestWakeId, "request_rejected", target, "wake_request",
                  nullptr, "already_pending", false, false, 0,
                  origin.length() > 0 ? origin.c_str() : nullptr);
    DynamicJsonDocument extra(64);
    extra["status"] = "already_pending";
    extra["target"] = wakeTarget;
    mqtt_publish_reply(reqId, "wake_request", false, extra);
    log_print("[mqtt] wake_request ignored: already pending target=%s\n", wakeTarget.c_str());
    return;
  }

  // ponytail: cap confirmation TTL to avoid overflow and keep requests short-lived
  if (expiresIn > CONFIRM_EXPIRES_MAX_S) {
    expiresIn = CONFIRM_EXPIRES_MAX_S;
  }

  wakeConfirmToken = generate_token();
  wakeConfirmExpiresMs = millis() + (expiresIn * 1000UL);
  wakeTarget = target;
  wakeId = requestWakeId;
  wakeOrigin = origin;
  mqtt_log_wake(wakeId, "confirmation_required", target, "wake_request",
                nullptr, nullptr, false, false, 0,
                wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);

  DynamicJsonDocument extra(128);
  extra["status"] = "confirmation_required";
  extra["confirm_token"] = wakeConfirmToken;
  extra["expires_at"] = (unsigned long)((millis() + (expiresIn * 1000UL)) / 1000UL);
  mqtt_publish_reply(reqId, "wake_request", true, extra);
  mqtt_publish_state(true);
  log_print("[mqtt] wake_request target=%s token=%s expires=%lus\n",
            target, wakeConfirmToken.c_str(), expiresIn);
}

static void handle_wake_confirm(const String& reqId, DynamicJsonDocument& doc) {
  const char* token = doc["confirm_token"] | "";
  const char* target = doc["target"] | "";
  String origin = doc["origin"] | "";
  if (origin.length() > 32) origin.remove(32);
  bool validToken = String(token) == wakeConfirmToken && wakeConfirmToken.length() > 0;
  String confirmWakeId = validToken ? wakeId : wake_new_id();
  const char* eventOrigin = validToken
                                ? (wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr)
                                : (origin.length() > 0 ? origin.c_str() : nullptr);
  mqtt_log_wake(confirmWakeId, "request_received", target, "wake_confirm",
                nullptr, nullptr, false, false, 0, eventOrigin);

  if (!mqttWakePending && wakeConfirmToken.length() == 0) {
    mqtt_log_wake(confirmWakeId, "request_rejected", target, "wake_confirm",
                  nullptr, "no_pending_request", false, false, 0, eventOrigin);
    DynamicJsonDocument extra(64);
    extra["status"] = "no_pending_request";
    mqtt_publish_reply(reqId, "wake_confirm", false, extra);
    log_print("[mqtt] wake_confirm ignored: no pending request\n");
    return;
  }

  if (!validToken) {
    mqtt_log_wake(confirmWakeId, "request_rejected", target, "wake_confirm",
                  nullptr, "invalid_token", false, false, 0, eventOrigin);
    DynamicJsonDocument extra(64);
    extra["status"] = "invalid_token";
    mqtt_publish_reply(reqId, "wake_confirm", false, extra);
    log_print("[mqtt] wake_confirm ignored: invalid token\n");
    return;
  }

  if (millis() > wakeConfirmExpiresMs) {
    mqtt_log_wake(confirmWakeId, "request_rejected", target, "wake_confirm",
                  nullptr, "token_expired", false, false, 0, eventOrigin);
    DynamicJsonDocument extra(64);
    extra["status"] = "token_expired";
    mqtt_publish_reply(reqId, "wake_confirm", false, extra);
    wakeConfirmToken = "";
    wakeId = "";
    wakeOrigin = "";
    log_print("[mqtt] wake_confirm ignored: token expired\n");
    return;
  }

  // Token valid — send WoL
  mqtt_log_wake(confirmWakeId, "confirmation_accepted", target, "wake_confirm",
                nullptr, nullptr, false, false, 0, eventOrigin);
  mqtt_begin_wake(target, reqId, "wake_confirm", false, confirmWakeId);
  log_print("[mqtt] wake confirmed WOL sent target=%s\n", target);
}

static void handle_wake_cancel(const String& reqId) {
  if (wakeId.length() > 0) {
    mqtt_log_wake(wakeId, "result", wakeTarget, "wake_cancel", "cancelled",
                  nullptr, false, false, 0,
                  wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);
  }
  wakeConfirmToken = "";
  mqttWakePending = false;
  wakeId = "";
  wakeOrigin = "";
  DynamicJsonDocument extra(64);
  extra["status"] = "cancelled";
  mqtt_publish_reply(reqId, "wake_cancel", true, extra);
  mqtt_publish_state(true);
  log_print("[mqtt] wake cancelled\n");
}

static void handle_reboot_request(const String& reqId, DynamicJsonDocument& doc) {
  unsigned long expiresIn = doc["expires_in_s"] | 30;

  // ponytail: cap confirmation TTL to avoid overflow and keep requests short-lived
  if (expiresIn > CONFIRM_EXPIRES_MAX_S) {
    expiresIn = CONFIRM_EXPIRES_MAX_S;
  }

  rebootConfirmToken = generate_token();
  rebootConfirmExpiresMs = millis() + (expiresIn * 1000UL);

  DynamicJsonDocument extra(96);
  extra["status"] = "confirmation_required";
  extra["confirm_token"] = rebootConfirmToken;
  extra["expires_at"] = (unsigned long)((millis() + (expiresIn * 1000UL)) / 1000UL);
  mqtt_publish_reply(reqId, "reboot_request", true, extra);
  log_print("[mqtt] reboot_request token=%s expires=%lus\n",
            rebootConfirmToken.c_str(), expiresIn);
}

static void handle_reboot_confirm(const String& reqId, DynamicJsonDocument& doc) {
  const char* token = doc["confirm_token"] | "";

  if (rebootConfirmToken.length() == 0) {
    DynamicJsonDocument extra(64);
    extra["status"] = "no_pending_request";
    mqtt_publish_reply(reqId, "reboot_confirm", false, extra);
    return;
  }

  if (String(token) != rebootConfirmToken) {
    DynamicJsonDocument extra(64);
    extra["status"] = "invalid_token";
    mqtt_publish_reply(reqId, "reboot_confirm", false, extra);
    return;
  }

  if (millis() > rebootConfirmExpiresMs) {
    DynamicJsonDocument extra(64);
    extra["status"] = "token_expired";
    mqtt_publish_reply(reqId, "reboot_confirm", false, extra);
    rebootConfirmToken = "";
    return;
  }

  // Valid — publish event then reboot
  DynamicJsonDocument extra(64);
  extra["result"] = "rebooting";
  mqtt_publish_reply(reqId, "reboot_confirm", true, extra);
  mqtt_publish_event("reboot", "");

  // Allow MQTT to flush before restart
  mqttClient.loop();
  delay(100);
  log_print("[mqtt] reboot confirmed — restarting\n");
  ESP.restart();
}

static void handle_reboot_cancel(const String& reqId) {
  rebootConfirmToken = "";
  DynamicJsonDocument extra(64);
  extra["status"] = "cancelled";
  mqtt_publish_reply(reqId, "reboot_cancel", true, extra);
  log_print("[mqtt] reboot cancelled\n");
}

// --- MQTT callback (called from client.loop) ---
static void mqtt_on_message(char* topic, byte* payload, unsigned int length) {
  // Only subscribed to BASE/cmd, but verify anyway
  String cmdTopicStr = cmdTopic();
  if (cmdTopicStr != topic) {
    log_print("[mqtt] msg on unexpected topic: %s\n", topic);
    return;
  }

  // Build string from payload (not null-terminated)
  String jsonStr;
  jsonStr.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) {
    jsonStr += (char)payload[i];
  }

  log_print("[mqtt] cmd received len=%d\n", length);
  if (length < 300) {
    log_print("[mqtt] raw: %s\n", jsonStr.c_str());
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, jsonStr);
  if (err) {
    String cmdName = doc["cmd"] | "";
    if (cmdName == "wake_request" || cmdName == "wake_confirm") {
      String rejectedWakeId = wake_new_id();
      String target = doc["target"] | "";
      String origin = doc["origin"] | "";
      if (origin.length() > 32) origin.remove(32);
      const char* originValue = origin.length() > 0 ? origin.c_str() : nullptr;
      mqtt_log_wake(rejectedWakeId, "request_received", target,
                    cmdName.c_str(), nullptr, nullptr, false, false, 0,
                    originValue);
      mqtt_log_wake(rejectedWakeId, "request_rejected", target,
                    cmdName.c_str(), nullptr, "malformed_json", false, false,
                    0, originValue);
    }
    log_print("[mqtt] cmd parse failed: %s\n", err.c_str());
    // ponytail: silent drop on parse errors, no error topic reply
    return;
  }

  String reqId = doc["id"] | "";
  String cmdName = doc["cmd"] | "";

  if (reqId.length() == 0 || cmdName.length() == 0) {
    if (cmdName == "wake_request" || cmdName == "wake_confirm") {
      String rejectedWakeId = wake_new_id();
      String target = doc["target"] | "";
      String origin = doc["origin"] | "";
      if (origin.length() > 32) origin.remove(32);
      const char* originValue = origin.length() > 0 ? origin.c_str() : nullptr;
      mqtt_log_wake(rejectedWakeId, "request_received", target,
                    cmdName.c_str(), nullptr, nullptr, false, false, 0,
                    originValue);
      mqtt_log_wake(rejectedWakeId, "request_rejected", target,
                    cmdName.c_str(), nullptr,
                    reqId.length() == 0 ? "missing_request_id" : "missing_command",
                    false, false, 0, originValue);
    }
    log_print("[mqtt] cmd missing id or cmd field\n");
    return;
  }

  esp_task_wdt_reset();

  if (cmdName == "ping") {
    handle_ping(reqId);
  } else if (cmdName == "wake_request") {
    handle_wake_request(reqId, doc);
  } else if (cmdName == "wake_confirm") {
    handle_wake_confirm(reqId, doc);
  } else if (cmdName == "wake_cancel") {
    handle_wake_cancel(reqId);
  } else if (cmdName == "reboot_request") {
    handle_reboot_request(reqId, doc);
  } else if (cmdName == "reboot_confirm") {
    handle_reboot_confirm(reqId, doc);
  } else if (cmdName == "reboot_cancel") {
    handle_reboot_cancel(reqId);
  } else {
    log_print("[mqtt] unknown cmd: %s\n", cmdName.c_str());
    DynamicJsonDocument extra(64);
    extra["status"] = "unknown_command";
    extra["received_cmd"] = cmdName;
    mqtt_publish_reply(reqId, cmdName, false, extra);
  }

  esp_task_wdt_reset();
}

// --- Log callback (registered in log_service) ---
static void mqtt_log_callback(const char* line) {
  if (!mqttClient.connected()) return;
  if (millis() - lastLogMs < LOG_RATE_MS) return;
  lastLogMs = millis();
  mqttClient.publish(logTopic().c_str(), line, false);
}

// --- Connection management ---
static void mqtt_connect() {
  if (!mqttConfigured) return;

  String clientId = "esp32-" + WiFi.macAddress();
  clientId.replace(":", "");

  // Build availability topic for Last Will
  String willTopicStr = availTopic();
  DynamicJsonDocument willDoc(64);
  willDoc["online"] = false;
  willDoc["reason"] = "mqtt_disconnected";
  String willPayload;
  serializeJson(willDoc, willPayload);

  log_print("[mqtt] connecting to %s:%d client=%s\n", MQTT_BROKER, MQTT_PORT, clientId.c_str());

  if (strlen(MQTT_USER) > 0) {
    bool ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASS,
                                 willTopicStr.c_str(), 1, true, willPayload.c_str());
    if (!ok) {
      log_print("[mqtt] connect failed (auth) state=%d\n", mqttClient.state());
      return;
    }
  } else {
    bool ok = mqttClient.connect(clientId.c_str(),
                                 willTopicStr.c_str(), 1, true, willPayload.c_str());
    if (!ok) {
      log_print("[mqtt] connect failed state=%d\n", mqttClient.state());
      return;
    }
  }

  log_print("[mqtt] connected\n");
  reconnectDelayMs = 1000;

  // Subscribe to command topic
  String cmdTopicStr = cmdTopic();
  mqttClient.subscribe(cmdTopicStr.c_str(), 1);
  log_print("[mqtt] subscribed to %s\n", cmdTopicStr.c_str());

  // Publish online availability (retained)
  mqtt_publish_availability(true, "mqtt_connected");

  // Publish initial state (retained)
  mqtt_publish_state(true);

  // Publish boot event
  mqtt_publish_event("boot", "");

  mqttWasConnected = true;
}

// --- Public API ---

bool mqtt_enabled() {
  return mqttConfigured;
}

bool mqtt_connected() {
  return mqttClient.connected();
}

void mqtt_setup() {
  mqttConfigured = strlen(MQTT_BROKER) > 0;

  if (!mqttConfigured) {
    log_print("[mqtt] MQTT broker not configured — skipping\n");
    return;
  }

  baseTopic = String(MQTT_BASE_TOPIC);
  if (baseTopic.length() == 0) {
    // ponytail: fall back to chip MAC for base topic if not configured
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    baseTopic = "esp32-" + mac;
    log_print("[mqtt] base topic not configured, using mac: %s\n", baseTopic.c_str());
  }

  mqttTlsClient.setInsecure();
  mqttTlsClient.setHandshakeTimeout(10);
  mqttClient.setClient(mqttTlsClient);
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqtt_on_message);
  // ponytail: keepalive 60s, reasonable for cloud broker
  mqttClient.setKeepAlive(60);
  // ponytail: socket timeout matches WDT-safe window
  mqttClient.setSocketTimeout(30);

  log_print("[mqtt] setup broker=%s:%d topic=%s\n", MQTT_BROKER, MQTT_PORT, baseTopic.c_str());

  // Register log callback
  log_set_callback(mqtt_log_callback);

  // Attempt initial connection (WiFi should already be up)
  mqtt_connect();
}

void mqtt_loop() {
  if (!mqttConfigured) return;

  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    if (mqttWasConnected) {
      mqttWasConnected = false;
      log_print("[mqtt] WiFi lost\n");
    }
    return;
  }

  // Maintain connection
  if (!mqttClient.connected()) {
    // Exponential backoff
    if (millis() - lastReconnectMs < (unsigned long)reconnectDelayMs) {
      return;
    }
    lastReconnectMs = millis();
    reconnectDelayMs = min(reconnectDelayMs * 2, RECONNECT_MAX_MS);

    mqtt_connect();
    return;
  }

  // Connected — reset backoff
  reconnectDelayMs = 1000;
  if (!mqttWasConnected) {
    mqttWasConnected = true;
  }

  // Process MQTT incoming messages
  mqttClient.loop();

  if (millis() - lastStatePublishMs >= STATE_PUBLISH_MS) {
    mqtt_publish_state(true);
  }

  // Handle MQTT wake polling
  if (mqttWakePending) {
    if (millis() - lastWakeCheckMs < WAKE_RETRY_MS) {
      return;
    }
    lastWakeCheckMs = millis();

    if (wake_is_pc_reachable(wakeTargetIp, wakeProbePort)) {
      // PC is online!
      mqtt_log_wake(wakeId, "result", wakeTarget, "wake_request", "success",
                    nullptr, false, false, millis() - wakeStartMs,
                    wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);
      mqttWakePending = false;
      lastWakeResult = "pc_online";
      lastWakeAt = millis() / 1000;
      mqtt_publish_state(true);
      mqtt_publish_event("pc_online", wakeTarget.c_str());
      wakeId = "";
      wakeOrigin = "";
      log_print("[mqtt] wake complete: %s online in %lums\n",
                wakeTarget.c_str(), millis() - wakeStartMs);
    } else if (millis() - wakeStartMs >= WAKE_TIMEOUT_MS) {
      // Timeout
      mqtt_log_wake(wakeId, "result", wakeTarget, "wake_request", "timeout",
                    nullptr, false, false, millis() - wakeStartMs,
                    wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);
      mqttWakePending = false;
      lastWakeResult = "timeout";
      lastWakeAt = millis() / 1000;
      mqtt_publish_state(true);
      mqtt_publish_event("pc_timeout", wakeTarget.c_str());
      wakeId = "";
      wakeOrigin = "";
      log_print("[mqtt] wake timeout: %s not online after %lums\n",
                wakeTarget.c_str(), WAKE_TIMEOUT_MS);
    }
  }

  // Clean up expired confirmation tokens
  if (wakeConfirmToken.length() > 0 && millis() > wakeConfirmExpiresMs) {
    mqtt_log_wake(wakeId, "request_rejected", wakeTarget, "wake_request",
                  nullptr, "confirmation_expired", false, false, 0,
                  wakeOrigin.length() > 0 ? wakeOrigin.c_str() : nullptr);
    wakeConfirmToken = "";
    wakeId = "";
    wakeOrigin = "";
    log_print("[mqtt] wake confirm token expired\n");
  }
  if (rebootConfirmToken.length() > 0 && millis() > rebootConfirmExpiresMs) {
    rebootConfirmToken = "";
    log_print("[mqtt] reboot confirm token expired\n");
  }
}
