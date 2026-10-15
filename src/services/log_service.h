#ifndef LOG_SERVICE_H
#define LOG_SERVICE_H

#include <Arduino.h>
#include <ArduinoJson.h>

void log_init();
bool log_enabled();
void log_event(const char* level, const char* component, const char* event, const char* msg);
void log_event_fields(const char* level, const char* component, const char* event,
                      JsonObjectConst fields);
void log_heartbeat(const String& targetName);
void log_print(const char* fmt, ...);

typedef void (*log_callback_t)(const char* line);
void log_set_callback(log_callback_t cb);

#endif
