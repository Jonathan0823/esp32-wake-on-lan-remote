#ifndef WAKE_SERVICE_H
#define WAKE_SERVICE_H

#include <Arduino.h>

String wake_new_id();
bool wake_send_magic(const String& mac, const String& bcast, int port);
bool wake_is_pc_reachable(const String& ip, int port);
void wake_start_polling(String chatId, const String& deviceName, const String& ip,
                        int probePort, const String& wakeId);
bool wake_is_pending();
void wake_poll();

#endif
