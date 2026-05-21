# ESP32 peer link (JSON over TCP)

Two ESP32 boards talk to each other as **peers**: each runs a **TCP server** and a **TCP client** to the other. Messages are **newline-framed JSON** so you can add sensor fields later without changing the wire format.

## Roles and IPs

| Board  | Role  | Static IP     | TCP port |
|--------|-------|---------------|----------|
| ESP32 #1 | Master (SoftAP) | `192.168.4.1` | `8080` |
| ESP32 #2 | Slave (STA)     | `192.168.4.2` | `8080` |

IPs must stay different on the same subnet. The Slave uses `WiFi.config()`; the Master sets the AP range via `WiFi.softAPConfig()`.

**Power order:** flash both sketches, then power **Master first**, then **Slave** (Slave joins AP `ESP32-Peer-Link`).

## First-connect handshake

1. Either TCP path comes up (Master→Slave client, or Slave→Master client).
2. **Master** sends once:

```json
{"cmd":"start","role":"master","ts_ms":1234,"sensors":{"temperature_c":null,"humidity_pct":null,"light_level":null}}
```

3. **Slave** replies:

```json
{"cmd":"receive","role":"slave","ts_ms":5678,"sensors":{...}}
```

## Arduino setup

1. Install [Arduino ESP32](https://docs.espressif.com/projects/arduino-esp32/en/latest/) board support.
2. Library Manager: **ArduinoJson** (v7+).
3. Open and upload:
   - `esp32_master_peer/esp32_master_peer.ino` → first board
   - `esp32_slave_peer/esp32_slave_peer.ino` → second board
4. Serial Monitor **115200** on both.

Change AP password in **both** files together (`kApPassword`, min 8 characters).

## Adding sensors later

Edit `readSensors()` in each sketch. Values are published under `"sensors"` on every message. Use `null` in JSON when a reading is not available yet (already done for placeholders).

## Optional: home router instead of SoftAP

If both boards join the same WiFi router, set in **both** sketches:

- `WiFi.begin("YourSSID", "YourPassword");`
- Master: `192.168.1.100`, Slave: `192.168.1.101` (example)
- Update `kPeerIp` on each side to the other board’s IP

Reserve those addresses in the router DHCP settings to avoid clashes.

## Example serial log

**Master**

```
[NET] Master SoftAP started
[NET] TCP server listening on 8080
[NET] Outbound client connected
[TX] Sent start (first connect)
[RX] {"cmd":"receive",...}
[OK] Slave acknowledged with receive
```

**Slave**

```
[NET] Slave connected
[RX] {"cmd":"start",...}
[TX] Sent receive (reply to start)
```
