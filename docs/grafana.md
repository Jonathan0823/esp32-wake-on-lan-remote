# Grafana Cloud Loki setup

## 1. Create a Grafana Cloud account

Free tier at [grafana.com](https://grafana.com).

## 2. Get your Logs credentials

In your Grafana Cloud portal, go to **Details > Loki** and note:
- **URL** — e.g. `https://<instance>.grafana.net`
- **Username** — your Logs instance ID
- **API key** — create one under **Access Policies**

## 3. Fill in `config.h`

```c
#define GRAFANA_LOGS_URL   "https://<instance>.grafana.net"
#define GRAFANA_LOGS_USER  "<logs-instance-id>"
#define GRAFANA_LOGS_TOKEN "<grafana-cloud-api-token>"
```

Leave them blank to skip cloud logging entirely.

## What gets logged

- Boot / reset reason
- WiFi connect / reconnect / disconnect
- Watchdog-triggered resets
- Heartbeat every 60s while healthy
- Wake request received, including Telegram/MQTT source and rejected requests
- WoL packet send result, target MAC, broadcast address, and port
- Wake result (success, timeout, cancelled, or superseded) with elapsed time where available
- Telegram poll failures (after short streak) and recovery
- No routine command usage or probe noise; wake lifecycle events are logged

## Stream label

All logs arrive at `{app="esp32-pc-remote"}`.

## Suggested alerts

- Heartbeat missing > 3 minutes
- WiFi lost for > 1 minute
- Poll fail streak >= 3
- Reset reason = brownout or panic

Wake logs share a `wake_id` across request, packet, and result events. In Explore, select the application stream, parse the JSON log line, then filter on its extracted component field:

```logql
{app="esp32-pc-remote"} | json | component="wake"
```

Use the extracted `wake_id` field to group lifecycle entries in the Logs view, or isolate one attempt by adding `| wake_id="a1b2c3d4"` after the component filter. `wake_id` is an extracted JSON field, not a stream label. Grafana delivery is best-effort, so a missing event alone does not prove the ESP32 did not send a WoL packet.
