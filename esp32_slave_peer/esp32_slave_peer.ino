/**
 * ESP32 Slave — peer JSON over TCP (dual-server model).
 *
 * Network: Connects to Master's SoftAP with a static IP.
 *   Master IP: 192.168.4.1
 *   Slave  IP: 192.168.4.2  (must differ from Master)
 *
 * Each board runs a TCP server and a TCP client to the peer.
 * When Slave receives "start", it replies with "receive".
 *
 * Dependencies (Arduino Library Manager):
 *   - ArduinoJson by Benoit Blanchon (v7+)
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiServer.h>
#include <ArduinoJson.h>

// Must match Master sketch
static const char* kApSsid = "ESP32-Peer-Link";
static const char* kApPassword = "peerlink123";

static const IPAddress kLocalIp(192, 168, 4, 2);
static const IPAddress kGateway(192, 168, 4, 1);
static const IPAddress kSubnet(255, 255, 255, 0);
static const IPAddress kPeerIp(192, 168, 4, 1);

static const uint16_t kTcpPort = 8080;

WiFiServer peerServer(kTcpPort);
WiFiClient peerClient;
WiFiClient serverClient;

bool startHandled = false;
unsigned long lastReconnectMs = 0;
static const unsigned long kReconnectIntervalMs = 3000;

struct SensorReadings {
  float temperatureC = NAN;
  float humidityPct = NAN;
  int lightLevel = -1;
};

bool readSensors(JsonObject sensorsOut) {
  SensorReadings r;
  // TODO: add real sensor reads on Slave board

  sensorsOut["temperature_c"] = isnan(r.temperatureC) ? nullptr : r.temperatureC;
  sensorsOut["humidity_pct"] = isnan(r.humidityPct) ? nullptr : r.humidityPct;
  sensorsOut["light_level"] = r.lightLevel < 0 ? nullptr : r.lightLevel;
  return true;
}

String buildJsonLine(const char* cmd) {
  JsonDocument doc;
  doc["cmd"] = cmd;
  doc["role"] = "slave";
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

/**
 * Reply on whichever socket received "start" (outbound or inbound client).
 */
void sendReceiveOnActiveLink(WiFiClient& sourceSocket) {
  if (startHandled) {
    return;
  }
  if (sendJsonLine(sourceSocket, "receive")) {
    startHandled = true;
    Serial.println(F("[TX] Sent receive (reply to start)"));
  }
}

void handlePeerMessage(const String& line, WiFiClient& replySocket) {
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, line);
  if (err) {
    Serial.print(F("[WARN] Bad JSON: "));
    Serial.println(err.c_str());
    return;
  }

  const char* cmd = doc["cmd"] | "";
  if (strcmp(cmd, "start") == 0) {
    Serial.println(F("[OK] Master start received"));
    sendReceiveOnActiveLink(replySocket);
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

  Serial.print(F("[NET] Connecting to Master "));
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
  Serial.println(F("[NET] Inbound client connected (Master -> Slave server)"));
}

void setupWiFiSta() {
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(kLocalIp, kGateway, kSubnet)) {
    Serial.println(F("[WARN] Static IP config rejected"));
  }

  WiFi.begin(kApSsid, kApPassword);
  Serial.print(F("[NET] Joining AP "));
  Serial.println(kApSsid);

  const uint8_t maxAttempts = 40;
  uint8_t attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
    delay(500);
    Serial.print('.');
    ++attempts;
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[ERR] WiFi connect failed — check Master is powered first"));
    return;
  }

  Serial.println(F("[NET] Slave connected"));
  Serial.print(F("       IP: "));
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("=== ESP32 Slave (peer JSON) ==="));

  setupWiFiSta();
  peerServer.begin();
  peerServer.setNoDelay(true);
  Serial.print(F("[NET] TCP server listening on "));
  Serial.println(kTcpPort);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    setupWiFiSta();
    delay(1000);
    return;
  }

  acceptInboundClient();
  tryConnectToPeer();

  String line;
  if (peerClient.connected() && readLineFromClient(peerClient, line)) {
    logIncoming(line);
    handlePeerMessage(line, peerClient);
  }
  if (serverClient && serverClient.connected() &&
      readLineFromClient(serverClient, line)) {
    logIncoming(line);
    handlePeerMessage(line, serverClient);
  }

  if (!peerClient.connected() && !(serverClient && serverClient.connected())) {
    startHandled = false;
  }

  delay(10);
}
