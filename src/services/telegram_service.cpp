#include "src/services/telegram_service.h"
#include "config.h"
#include "src/services/log_service.h"
#include "src/services/wake_service.h"
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Declared in esp32-pc-remote.ino
extern const char *current_reset_reason();

struct TelegramUpdate {
  String type;
  String text;
  String chat_id;
  String query_id;
};

static Preferences telegramPrefs;
static long telegramOffset = 1;
static bool telegramRebootPending = false;
static bool telegramConfigured = false;
static unsigned long lastPoll = 0;
static const unsigned long POLL_MS = 1000;
static const int IDLE_LONG_POLL_S = 15;
static const int WAKE_LONG_POLL_S = 5;
static int consecutiveFailures = 0;
static const int MAX_BACKOFF_MS = 30000;
static const int MAX_UPDATES = 1;
static TaskHandle_t telegramPollTaskHandle = nullptr;
static WiFiClientSecure tlsClient; // ponytail: single persistent client

static void telegram_poll_task(void *pv);
static void handleCommand(String chatId, String text);
static void handleCallback(const TelegramUpdate &msg);

static String telegram_url(const String &method) {
  return String("https://api.telegram.org/bot") + BOT_TOKEN + "/" + method;
}

static String method_name(const String &command) {
  int slash = command.lastIndexOf('/');
  return slash >= 0 ? command.substring(slash + 1) : command;
}

static void telegram_log_http_failure(const char *label, int code) {
  String msg = String("HTTP failed code=") + code;
  String desc = HTTPClient::errorToString(code);
  if (desc.length() > 0) {
    msg += " (" + desc + ")";
  }
  log_print("[telegram] %s %s\n", label, msg.c_str());
  log_event("error", "telegram", label, msg.c_str());
}

static void telegram_log_begin_failure(const char *label) {
  log_print("[telegram] %s http.begin failed\n", label);
  log_event("error", "telegram", label, "http.begin failed");
}

static String telegram_post_raw(const String &method, JsonObject payload,
                                const char *label, uint32_t timeoutMs) {
  for (int attempt = 0; attempt < 2; attempt++) {
    esp_task_wdt_reset();
    if (attempt > 0) {
      log_print("[telegram] %s retry\n", label);
      delay(1000); // ponytail: longer delay lets TCP stack clean up
      esp_task_wdt_reset();
    }

    // Force clean close before each attempt
    tlsClient.stop();
    delay(10);

    tlsClient.setInsecure();
    tlsClient.setHandshakeTimeout(10);

    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(5000);
    http.setTimeout((uint16_t)min<uint32_t>(timeoutMs, 65000));
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    String url = telegram_url(method);
    log_print("[telegram] %s attempt %d connecting\n", label, attempt);

    if (!http.begin(tlsClient, url)) {
      telegram_log_begin_failure(label);
      http.end();
      tlsClient.stop();
      continue;
    }

    String body;
    serializeJson(payload, body);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST(body);
    esp_task_wdt_reset();
    String response = code > 0 ? http.getString() : "";
    esp_task_wdt_reset();

    http.end();

    log_print("[telegram] %s HTTP code=%d resp_len=%d\n", label, code,
              response.length());

    if (code > 0)
      return response;

    telegram_log_http_failure(label, code);
  }

  log_print("[telegram] %s failed after retry\n", label);
  return "";
}

static bool telegram_send_once(const char *label, const String &method,
                               JsonObject payload) {
  if (!telegramConfigured) return false;
  esp_task_wdt_reset();
  String response =
      telegram_post_raw(method_name(method), payload, label, 8000);
  esp_task_wdt_reset();

  if (response.length() == 0) {
    log_print("[telegram] %s send failed: empty response\n", label);
    log_event("error", "telegram", label, "empty response from Telegram");
    return false;
  }

  DynamicJsonDocument doc(768);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    log_print("[telegram] %s send failed: %s\n", label, error.c_str());
    log_event("error", "telegram", label, error.c_str());
    return false;
  }

  bool ok = doc["ok"] | false;
  if (!ok) {
    const char *desc = doc["description"] | "Telegram API returned ok=false";
    log_print("[telegram] %s send not_ok: %s\n", label, desc);
    log_event("error", "telegram", label, desc);
  } else {
    log_print("[telegram] %s send ok\n", label);
  }
  return ok;
}

bool telegram_send_json_once(const String &command, JsonObject payload,
                             const char *label) {
  return telegram_send_once(label, command, payload);
}

bool telegram_send_text_once(const String &chatId, const String &text,
                             const String &parse_mode) {
  DynamicJsonDocument payload(1024);
  payload["chat_id"] = chatId;
  payload["text"] = text;
  if (parse_mode.length() > 0) {
    payload["parse_mode"] = parse_mode;
  }
  return telegram_send_once("sendMessage", "sendMessage",
                            payload.as<JsonObject>());
}

bool telegram_send_callback_answer_once(const String &query_id,
                                        const String &text, bool show_alert,
                                        const String &url, int cache_time) {
  DynamicJsonDocument payload(256);
  payload["callback_query_id"] = query_id;
  payload["show_alert"] = show_alert;
  payload["cache_time"] = cache_time;

  if (text.length() > 0)
    payload["text"] = text;
  if (url.length() > 0)
    payload["url"] = url;

  return telegram_send_once("answerCallbackQuery", "answerCallbackQuery",
                            payload.as<JsonObject>());
}

void telegram_setup() {
  telegramConfigured = strlen(BOT_TOKEN) > 0;
  if (!telegramConfigured) {
    log_print("[telegram] BOT_TOKEN not configured — skipping\n");
    return;
  }

  telegramPrefs.begin("telegram", false);
  telegramOffset = telegramPrefs.getLong("offset", 1);
  log_print("[telegram] setup longPoll=%d offset=%ld\n", IDLE_LONG_POLL_S,
            telegramOffset);

  tlsClient.setInsecure();
  tlsClient.setHandshakeTimeout(10);

  if (telegramPollTaskHandle == nullptr) {
    BaseType_t ok = xTaskCreate(telegram_poll_task, "telegram_poll", 12288,
                                nullptr, 1, &telegramPollTaskHandle);
    if (ok != pdPASS) {
      telegramPollTaskHandle = nullptr;
      log_print("[telegram] poll task start failed\n");
    }
  }
}

static void telegram_poll_task(void *pv) {
  (void)pv;
  esp_task_wdt_add(NULL);
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      telegram_poll();
    }
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(POLL_MS));
  }
}

static String menuText() {
  String msg = "🤖 <b>ESP32 PC Remote commands</b>\n\n";
  msg += "/start /help — Show this menu\n";
  msg += "/ping — Check bot health &amp; diagnostics\n";
  msg += "/status — ESP32 health + target PC state\n";
  msg += "/wake — Wake the selected PC (inline confirmation)\n";
  msg += "/wake force — Wake the selected PC immediately\n";
  msg += "/reboot — Restart the ESP32\n\n";
  msg += "Current target: ";
  msg += PC_NAME;
  return msg;
}

static bool telegram_wake_id_valid(const String& id) {
  if (id.length() != 8) return false;
  for (unsigned int i = 0; i < id.length(); i++) {
    char c = id[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) return false;
  }
  return true;
}

static void telegram_log_wake(const String& wakeId, const char* event,
                              const char* requestType = nullptr,
                              const char* reason = nullptr,
                              bool packetAttempted = false, bool sent = false) {
  if (!log_enabled()) return;
  DynamicJsonDocument fields(384);
  fields["wake_id"] = wakeId;
  fields["source"] = "telegram";
  fields["target"] = PC_NAME;
  if (requestType) fields["request_type"] = requestType;
  if (reason) fields["reason"] = reason;
  if (packetAttempted) {
    fields["sent"] = sent;
    fields["target_mac"] = PC_MAC;
    fields["broadcast"] = WOL_BCAST;
    fields["port"] = WOL_PORT;
  }
  const char* level = reason || (packetAttempted && !sent) ? "warn" : "info";
  log_event_fields(level, "wake", event, fields.as<JsonObjectConst>());
}

static void handleCommand(String chatId, String text) {
  text.trim();
  log_print("[telegram] handleCommand chat=%s text=%s\n", chatId.c_str(),
            text.c_str());

  int sp = text.indexOf(' ');
  String cmd = (sp >= 0) ? text.substring(0, sp) : text;
  String args = (sp >= 0) ? text.substring(sp + 1) : "";
  cmd.toLowerCase();
  args.toLowerCase();
  int botName = cmd.indexOf('@');
  if (botName >= 0)
    cmd = cmd.substring(0, botName);

  if (cmd == "/start" || cmd == "/help") {
    log_print("[telegram] /help reply start\n");
    telegram_send_text_once(chatId, menuText(), "HTML");
    log_print("[telegram] /help reply done\n");
    return;
  }

  if (cmd == "/ping") {
    String msg = "🤖 <b>Bot alive</b>\n";
    msg +=
        "📶 Wi-Fi: " +
        String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") +
        "\n";
    msg += "📡 RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    msg += "🌐 IP: " + WiFi.localIP().toString() + "\n";
    msg += "⏱ Uptime: " + String(millis() / 1000) + "s\n";
    msg += "💾 Heap: " + String(ESP.getFreeHeap() / 1024) + " KB free\n";
    msg += "🔄 Reset: " + String(current_reset_reason()) + "\n";

    log_print("[telegram] /ping reply start\n");
    telegram_send_text_once(chatId, msg, "HTML");
    log_print("[telegram] /ping reply done\n");
    return;
  }

  if (cmd == "/status") {
    log_print("[telegram] /status probe start\n");
    bool reachable = wake_is_pc_reachable(PC_IP, PC_TCP_PORT);
    log_print("[telegram] /status probe done reachable=%d\n", reachable);
    String msg = "✅ <b>ESP32 healthy</b>\n";
    msg +=
        "📶 Wi-Fi: " +
        String(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected") +
        "\n";
    msg += "📡 RSSI: " + String(WiFi.RSSI()) + " dBm\n";
    msg += "🌐 IP: " + WiFi.localIP().toString() + "\n";
    msg += "⏱ Uptime: " + String(millis() / 1000) + "s\n";
    msg += "💾 Heap: " + String(ESP.getFreeHeap() / 1024) + " KB free\n\n";
    msg += "🖥 <b>Target: " + String(PC_NAME) + "</b>\n";
    msg += "   MAC: " + String(PC_MAC) + "\n";
    msg += "   IP: " + String(PC_IP) + "\n";
    msg += "   Status: " + String(reachable ? "online" : "offline / sleeping");
    log_print("[telegram] /status reply start\n");
    telegram_send_text_once(chatId, msg, "HTML");
    log_print("[telegram] /status reply done\n");
    return;
  }

  if (cmd == "/wake") {
    String wakeId = wake_new_id();
    telegram_log_wake(wakeId, "request_received", "command");
    bool forceWake = args.indexOf("force") >= 0 || args.indexOf("--force") >= 0 ||
                     args.indexOf("-f") >= 0;
    if (forceWake) {
      log_print("[telegram] /wake force start\n");
      bool sent = wake_send_magic(PC_MAC, WOL_BCAST, WOL_PORT);
      telegram_log_wake(wakeId, "packet_sent", nullptr, nullptr, true, sent);
      wake_start_polling(chatId, PC_NAME, PC_IP, PC_TCP_PORT, wakeId);
      telegram_send_text_once(chatId,
                              "⚡ Wake signal sent to " + String(PC_NAME) +
                                  " — waiting up to 90s for PC to respond...",
                              "");
      log_print("[telegram] /wake force done\n");
      return;
    }

    DynamicJsonDocument payload(1024);
    payload["chat_id"] = chatId;
    payload["text"] = "Wake " + String(PC_NAME) + "?";
    JsonObject markup = payload.createNestedObject("reply_markup");
    JsonArray rows = markup.createNestedArray("inline_keyboard");
    JsonArray row = rows.createNestedArray();
    JsonObject yes = row.createNestedObject();
    yes["text"] = "✅ Yes";
    yes["callback_data"] = "wake_confirm:" + wakeId;
    JsonObject no = row.createNestedObject();
    no["text"] = "❌ No";
    no["callback_data"] = "wake_cancel:" + wakeId;
    log_print("[telegram] /wake reply start\n");
    bool prompted = telegram_send_json_once("sendMessage", payload.as<JsonObject>(),
                                            "sendMessage");
    if (!prompted) {
      telegram_log_wake(wakeId, "request_rejected", "command",
                        "confirmation_prompt_failed");
    }
    log_print("[telegram] /wake reply done\n");
    return;
  }

  if (cmd == "/reboot") {
    log_print("[telegram] /reboot reply start\n");
    telegram_send_text_once(chatId, "🔄 Rebooting ESP32...", "");
    log_print("[telegram] /reboot reply done\n");
    telegramRebootPending = true;
    return;
  }

  log_print("[telegram] unknown command reply start\n");
  telegram_send_text_once(
      chatId, "Unknown command. Type /help for available commands.", "");
  log_print("[telegram] unknown command reply done\n");
}

static void handleCallback(const TelegramUpdate &msg) {
  log_print("[telegram] handleCallback chat=%s data=%s\n", msg.chat_id.c_str(),
            msg.text.c_str());
  if (msg.text == "wake_confirm" || msg.text.startsWith("wake_confirm:")) {
    bool legacyConfirmation = msg.text == "wake_confirm";
    String wakeId = legacyConfirmation
                        ? wake_new_id()
                        : msg.text.substring(String("wake_confirm:").length());
    if (legacyConfirmation) {
      telegram_log_wake(wakeId, "request_received", "legacy_confirmation");
    } else if (!telegram_wake_id_valid(wakeId)) {
      if (wakeId.length() == 0) wakeId = wake_new_id();
      telegram_log_wake(wakeId, "request_received", "confirmation");
      telegram_log_wake(wakeId, "request_rejected", "confirmation",
                        "invalid_confirmation_id");
      telegram_send_callback_answer_once(msg.query_id, "Invalid request", true, "", 0);
      return;
    }
    telegram_log_wake(wakeId, "confirmation_received");
    log_print("[telegram] callback confirm start\n");
    telegram_send_callback_answer_once(msg.query_id, "", false, "", 0);
    bool sent = wake_send_magic(PC_MAC, WOL_BCAST, WOL_PORT);
    telegram_log_wake(wakeId, "packet_sent", nullptr, nullptr, true, sent);
    wake_start_polling(msg.chat_id, PC_NAME, PC_IP, PC_TCP_PORT, wakeId);
    telegram_send_text_once(msg.chat_id,
                            "⚡ Wake signal sent to " + String(PC_NAME) +
                                " — waiting up to 90s for PC to respond...",
                            "");
    log_print("[telegram] callback confirm done\n");
  } else if (msg.text == "wake_cancel" || msg.text.startsWith("wake_cancel:")) {
    bool legacyCancellation = msg.text == "wake_cancel";
    String wakeId = legacyCancellation
                        ? wake_new_id()
                        : msg.text.substring(String("wake_cancel:").length());
    if (legacyCancellation) {
      telegram_log_wake(wakeId, "request_received", "legacy_cancellation");
    } else if (!telegram_wake_id_valid(wakeId)) {
      if (wakeId.length() == 0) wakeId = wake_new_id();
      telegram_log_wake(wakeId, "request_received", "cancellation");
      telegram_log_wake(wakeId, "request_rejected", "cancellation",
                        "invalid_confirmation_id");
      telegram_send_callback_answer_once(msg.query_id, "Invalid request", true, "", 0);
      return;
    }
    telegram_log_wake(wakeId, "request_rejected", "confirmation",
                      "cancelled_by_user");
    log_print("[telegram] callback cancel start\n");
    telegram_send_callback_answer_once(msg.query_id, "Cancelled", false, "", 0);
    log_print("[telegram] callback cancel done\n");
  }
}

static int parse_updates(const String &response, TelegramUpdate *updates,
                         int maxUpdates, long *nextOffset) {
  log_print("[telegram] parse_updates len=%d\n", response.length());
  if (response.length() > 0 && response.length() < 300) {
    log_print("[telegram] raw: %s\n", response.c_str());
  }
  DynamicJsonDocument doc(16384);
  DeserializationError error = deserializeJson(doc, response);
  if (error) {
    log_print("[telegram] getUpdates parse failed: %s\n", error.c_str());
    return -1;
  }

  if (!(doc["ok"] | false)) {
    const char *desc = doc["description"] | "Telegram API returned ok=false";
    log_print("[telegram] getUpdates not_ok: %s\n", desc);
    return -1;
  }

  JsonArray result = doc["result"].as<JsonArray>();
  int count = 0;
  for (JsonObject item : result) {
    long updateId = item["update_id"] | 0;
    if (updateId >= *nextOffset)
      *nextOffset = updateId + 1;
    if (count >= maxUpdates)
      continue;

    TelegramUpdate &msg = updates[count];
    msg = TelegramUpdate{};
    if (item.containsKey("callback_query")) {
      JsonObject cb = item["callback_query"];
      msg.type = "callback_query";
      msg.query_id = cb["id"].as<String>();
      msg.text = cb["data"].as<String>();
      msg.chat_id = cb["message"]["chat"]["id"].as<String>();
      count++;
    } else if (item.containsKey("message")) {
      JsonObject m = item["message"];
      if (!m.containsKey("text"))
        continue;
      msg.type = "message";
      msg.text = m["text"].as<String>();
      msg.chat_id = m["chat"]["id"].as<String>();
      count++;
    }
  }
  return count;
}

void telegram_poll() {
  if (!telegramConfigured) return;
  if (millis() - lastPoll < POLL_MS)
    return;

  // WiFi health check
  if (WiFi.status() != WL_CONNECTED) {
    log_print("[telegram] WiFi disconnected, attempting reconnect\n");
    WiFi.disconnect();
    WiFi.reconnect();
    delay(1000);
    if (WiFi.status() != WL_CONNECTED) {
      log_print("[telegram] WiFi reconnect failed, skipping poll\n");
      lastPoll = millis();
      return;
    }
    log_print("[telegram] WiFi reconnected\n");
    tlsClient.stop(); // ponytail: force clean socket after reconnect
  }

  // Exponential backoff after failures (capped at 30s)
  if (consecutiveFailures > 0) {
    int backoffMs =
        min(1000 * (1 << min(consecutiveFailures, 4)), MAX_BACKOFF_MS);
    if (millis() - lastPoll < (uint32_t)backoffMs) {
      esp_task_wdt_reset();
      return;
    }
    log_print("[telegram] backoff %dms after %d failures\n", backoffMs,
              consecutiveFailures);
  }

  int effectiveLongPoll =
      wake_is_pending() ? WAKE_LONG_POLL_S : IDLE_LONG_POLL_S;
  DynamicJsonDocument payload(256);
  payload["offset"] = telegramOffset;
  payload["limit"] = MAX_UPDATES;
  payload["timeout"] = effectiveLongPoll;

  esp_task_wdt_reset();
  uint32_t pollStart = millis();
  log_print("[telegram] getUpdates offset=%ld longPoll=%d\n", telegramOffset,
            effectiveLongPoll);
  String response =
      telegram_post_raw("getUpdates", payload.as<JsonObject>(), "getUpdates",
                        (effectiveLongPoll + 10) * 1000);
  esp_task_wdt_reset();

  TelegramUpdate updates[MAX_UPDATES];
  long nextOffset = telegramOffset;
  int newCount = response.length() > 0 ? parse_updates(response, updates,
                                                       MAX_UPDATES, &nextOffset)
                                       : -1;

  log_print("[telegram] getUpdates done updates=%d elapsed=%lums next=%ld\n",
            newCount, millis() - pollStart, nextOffset);

  if (newCount < 0) {
    consecutiveFailures++;
    log_event("error", "telegram", "getUpdates", "getUpdates failed");
    lastPoll = millis();
    return;
  }

  // Success — reset failure counter
  consecutiveFailures = 0;

  for (int i = 0; i < newCount; i++) {
    log_print("[telegram] update[%d] type=%s text=%s\n", i,
              updates[i].type.c_str(), updates[i].text.c_str());
    if (updates[i].type == "callback_query") {
      handleCallback(updates[i]);
    } else {
      handleCommand(updates[i].chat_id, updates[i].text);
    }
    log_print("[telegram] update[%d] handled\n", i);
    esp_task_wdt_reset();
  }

  if (nextOffset != telegramOffset) {
    telegramOffset = nextOffset;
    telegramPrefs.putLong("offset", telegramOffset);
  }
  if (telegramRebootPending) {
    telegramPrefs.putLong("offset", telegramOffset);
    log_print("[telegram] reboot pending; offset=%ld\n", telegramOffset);
    delay(100);
    ESP.restart();
  }
  lastPoll = millis();
}
