/**
 * ESP32 Master — peer JSON over TCP (dual-server model).
 *
 * Network: Master runs a SoftAP; Slave joins it.
 *   Master IP: 192.168.4.1  (AP gateway)
 *   Slave  IP: 192.168.4.2  (static STA on Slave sketch)
 *
 * Each board runs a TCP server and maintains a TCP client to the peer.
 * On first successful link, Master sends a "start" JSON message.
 *
 * Dependencies (Arduino Library Manager):
 *   - ArduinoJson by Benoit Blanchon (v7+)
 *
 * Sensor fields are placeholders; fill readSensors() when hardware is ready.
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <ArduinoJson.h>

// --- SoftAP (Master creates the link) ---------------------------------------
static const char* kApSsid = "ESP32-Peer-Link";
static const char* kApPassword = "peerlink123";  // min 8 chars for WPA2

// Master AP address (fixed by SoftAP; do not change unless Slave IP changes too)
static const IPAddress kLocalIp(192, 168, 4, 1);
static const IPAddress kPeerIp(192, 168, 4, 2);

static const uint16_t kTcpPort = 8080;

// --- TCP endpoints ------------------------------------------------------------
WiFiServer peerServer(kTcpPort);
WiFiClient peerClient;

// Incoming client accepted by our server (Slave may connect here)
WiFiClient serverClient;

// --- Session state ------------------------------------------------------------
bool sessionStarted = false;
bool startMessageSent = false;
unsigned long lastReconnectMs = 0;
static const unsigned long kReconnectIntervalMs = 3000;

// --- Sensor placeholders (add ADC / I2C / etc. later) -------------------------
struct SensorReadings {
  float temperatureC = NAN;
  float humidityPct = NAN;
  int lightLevel = -1;
};

/**
 * Read all sensors and write values into the JSON "sensors" object.
 * Return false if a required sensor failed (optional; currently always true).
 */
bool readSensors(JsonObject sensorsOut) {
  SensorReadings r;
  // TODO: replace with real drivers, e.g. DHT22, BME280, analog LDR, etc.
  // r.temperatureC = dht.readTemperature();
  // r.humidityPct = dht.readHumidity();

  sensorsOut["temperature_c"] = isnan(r.temperatureC) ? nullptr : r.temperatureC;
  sensorsOut["humidity_pct"] = isnan(r.humidityPct) ? nullptr : r.humidityPct;
  sensorsOut["light_level"] = r.lightLevel < 0 ? nullptr : r.lightLevel;
  return true;
}

/** Build a JSON line (newline-terminated for framing). */
String buildJsonLine(const char* cmd) {
  JsonDocument doc;
  doc["cmd"] = cmd;
  doc["role"] = "master";
  doc["ts_ms"] = millis();

  JsonObject sensors = doc["sensors"].to<JsonObject>();
  readSensors(sensors);

  String line;
  serializeJson(doc, line);
  line += '\n';
  return line;
}

bool sendJsonLine(WiFiClient& client, const char* cmd) {
  if (!client || !client.connected()) {
    return false;
  }
  const String line = buildJsonLine(cmd);
  const size_t written = client.print(line);
  return written == line.length();
}

void logIncoming(const String& line) {
  Serial.print(F("[RX] "));
  Serial.println(line);
}

void handlePeerMessage(const String& line) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print(F("[WARN] Bad JSON: "));
    Serial.println(err.c_str());
    return;
  }

  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "receive") == 0) {
    Serial.println(F("[OK] Slave acknowledged with receive"));
    if (!doc["sensors"].isNull()) {
      Serial.print(F("       Slave sensors snapshot: "));
      serializeJson(doc["sensors"], Serial);
      Serial.println();
    }
  }
}

bool readLineFromClient(WiFiClient& client, String& outLine) {
  while (client.available()) {
    const char c = static_cast<char>(client.read());
    if (c == '\n') {
      outLine.trim();
      return outLine.length() > 0;
    }
    if (c != '\r') {
      outLine += c;
    }
  }
  return false;
}

void tryConnectToPeer() {
  if (peerClient.connected()) {
    return;
  }

  const unsigned long now = millis();
  if (now - lastReconnectMs < kReconnectIntervalMs) {
    return;
  }
  lastReconnectMs = now;

  Serial.print(F("[NET] Connecting to Slave "));
  Serial.print(kPeerIp);
  Serial.print(':');
  Serial.println(kTcpPort);

  peerClient.stop();
  if (peerClient.connect(kPeerIp, kTcpPort, 5000)) {
    Serial.println(F("[NET] Outbound client connected"));
  } else {
    Serial.println(F("[NET] Outbound client not ready yet"));
  }
}

void acceptInboundClient() {
  if (serverClient && serverClient.connected()) {
    return;
  }

  WiFiClient incoming = peerServer.available();
  if (!incoming) {
    return;
  }

  serverClient = incoming;
  Serial.println(F("[NET] Inbound client connected (Slave -> Master server)"));
}

/** First link up: Master sends "start" once. */
void onFirstConnect() {
  if (startMessageSent) {
    return;
  }

  WiFiClient* active = nullptr;
  if (peerClient.connected()) {
    active = &peerClient;
  } else if (serverClient && serverClient.connected()) {
    active = &serverClient;
  }

  if (!active) {
    return;
  }

  if (sendJsonLine(*active, "start")) {
    startMessageSent = true;
    sessionStarted = true;
    Serial.println(F("[TX] Sent start (first connect)"));
  }
}

void setupWiFiAp() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(kLocalIp, kLocalIp, IPAddress(255, 255, 255, 0));
  const bool apOk = WiFi.softAP(kApSsid, kApPassword);
  if (!apOk) {
    Serial.println(F("[ERR] SoftAP failed"));
    return;
  }

  Serial.println(F("[NET] Master SoftAP started"));
  Serial.print(F("       SSID: "));
  Serial.println(kApSsid);
  Serial.print(F("       IP:   "));
  Serial.println(WiFi.softAPIP());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("=== ESP32 Master (peer JSON) ==="));

  setupWiFiAp();
  peerServer.begin();
  peerServer.setNoDelay(true);
  Serial.print(F("[NET] TCP server listening on "));
  Serial.println(kTcpPort);
}

void loop() {
  acceptInboundClient();
  tryConnectToPeer();

  const bool linked =
      peerClient.connected() || (serverClient && serverClient.connected());
  if (linked) {
    onFirstConnect();
  }

  String line;
  if (peerClient.connected() && readLineFromClient(peerClient, line)) {
    logIncoming(line);
    handlePeerMessage(line);
  }
  if (serverClient && serverClient.connected() &&
      readLineFromClient(serverClient, line)) {
    logIncoming(line);
    handlePeerMessage(line);
  }

  if (!linked && sessionStarted) {
    Serial.println(F("[NET] Peer disconnected — will retry"));
    sessionStarted = false;
    startMessageSent = false;
    peerClient.stop();
    if (serverClient) {
      serverClient.stop();
    }
  }

  delay(10);
}
