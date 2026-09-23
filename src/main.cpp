/*
  -----------------------------------------
  9 x ESP32 boards, ALL flashed with THIS SAME firmware.
  A 4-pin DIP switch on each board decides its role at boot:

    S1        OFF = Transmitter          ON = Main (receiver/web server)
    S2,S3,S4  binary device number (1-8) -- only used when S1 = OFF

  Wiring (per switch): one leg -> GND, other leg -> ESP32 GPIO below.
  Internal pull-ups are used, so switch OFF = HIGH, switch ON = LOW.

    S1 -> GPIO32   (role)
    S2 -> GPIO33   (device id bit 2)
    S3 -> GPIO25   (device id bit 1)
    S4 -> GPIO26   (device id bit 0)

  Device number = (S2*4 + S3*2 + S4*1) + 1   -> range 1..8

  Main device also starts its own WiFi Access Point:
    SSID: ESPNOW-Main   PASS: 12345678
  Connect to it and browse to 192.168.4.1 to see the live dashboard.

*/

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <WebServer.h>

// ---------- Pin assignment ----------
#define PIN_S1 32   // role
#define PIN_S2 33   // device id bit 2
#define PIN_S3 25   // device id bit 1
#define PIN_S4 26   // device id bit 0

#define WIFI_CHANNEL 1          // must match on every board
#define MAX_DEVICES  8
#define DATA_TIMEOUT_MS 15000   // mark a device "offline" after this long with no packet

// ---------- Shared packet structure ----------
typedef struct __attribute__((packed)) {
  uint8_t  deviceId;   // 1..8
  float    value1;     // e.g. temperature - replace with your real sensor
  float    value2;     // e.g. humidity    - replace with your real sensor
  uint32_t seq;         // running counter, useful for spotting lost packets
} SensorPacket;

// ---------- Globals ----------
bool isMain = false;
uint8_t myDeviceId = 0;

uint8_t broadcastMac[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

SensorPacket   lastData[MAX_DEVICES + 1];   // index 1..8
unsigned long  lastSeenMs[MAX_DEVICES + 1];
bool           everSeen[MAX_DEVICES + 1];

WebServer server(80);

// ---------- Helpers ----------
uint8_t readDeviceIdFromSwitches() {
  bool b2 = (digitalRead(PIN_S2) == LOW);
  bool b1 = (digitalRead(PIN_S3) == LOW);
  bool b0 = (digitalRead(PIN_S4) == LOW);
  uint8_t val = (b2 << 2) | (b1 << 1) | b0; // 0..7
  return val + 1; // 1..8
}

// ---------- ESP-NOW callbacks ----------
void onDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {
  if (len != sizeof(SensorPacket)) return;
  SensorPacket pkt;
  memcpy(&pkt, incomingData, sizeof(pkt));
  if (pkt.deviceId < 1 || pkt.deviceId > MAX_DEVICES) return;

  lastData[pkt.deviceId]   = pkt;
  lastSeenMs[pkt.deviceId] = millis();
  everSeen[pkt.deviceId]   = true;

  Serial.printf("RX from device %u: v1=%.2f v2=%.2f seq=%lu\n",
                pkt.deviceId, pkt.value1, pkt.value2, pkt.seq);
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
  Serial.printf("Send status: %s\n", status == ESP_NOW_SEND_SUCCESS ? "OK" : "FAIL");
}

// ---------- Web dashboard (main device only) ----------
String buildHtml() {
  String html = F(
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>ESP-NOW Dashboard</title>"
    "<style>"
    "body{font-family:sans-serif;background:#111;color:#eee;margin:0;padding:16px}"
    "h1{font-size:1.3rem}"
    "table{width:100%;border-collapse:collapse;margin-top:12px}"
    "th,td{padding:8px;border-bottom:1px solid #333;text-align:left}"
    "th{color:#888;font-weight:normal;font-size:0.85rem}"
    ".online{color:#4caf50}.offline{color:#e53935}"
    "</style></head><body>"
    "<h1>ESP-NOW Devices</h1>"
    "<table><thead><tr><th>Device</th><th>Value 1</th><th>Value 2</th>"
    "<th>Seq</th><th>Last seen (s)</th><th>Status</th></tr></thead>"
    "<tbody id='tb'></tbody></table>"
    "<script>"
    "async function refresh(){"
    "const r=await fetch('/data');const d=await r.json();"
    "let rows='';"
    "d.forEach(x=>{"
    "rows+=`<tr><td>#${x.id}</td><td>${x.seen?x.v1.toFixed(2):'-'}</td>"
    "<td>${x.seen?x.v2.toFixed(2):'-'}</td><td>${x.seen?x.seq:'-'}</td>"
    "<td>${x.seen?x.age:'-'}</td>"
    "<td class='${x.online?\"online\":\"offline\"}'>${x.seen?(x.online?'Online':'Offline'):'No data yet'}</td></tr>`;"
    "});"
    "document.getElementById('tb').innerHTML=rows;"
    "}"
    "refresh();setInterval(refresh,2000);"
    "</script></body></html>"
  );
  return html;
}

void handleRoot() {
  server.send(200, "text/html", buildHtml());
}

void handleData() {
  String json = "[";
  for (int i = 1; i <= MAX_DEVICES; i++) {
    unsigned long age = everSeen[i] ? (millis() - lastSeenMs[i]) / 1000 : 0;
    bool online = everSeen[i] && ((millis() - lastSeenMs[i]) < DATA_TIMEOUT_MS);
    json += "{";
    json += "\"id\":" + String(i) + ",";
    json += "\"seen\":" + String(everSeen[i] ? "true" : "false") + ",";
    json += "\"v1\":" + String(lastData[i].value1, 2) + ",";
    json += "\"v2\":" + String(lastData[i].value2, 2) + ",";
    json += "\"seq\":" + String(lastData[i].seq) + ",";
    json += "\"age\":" + String(age) + ",";
    json += "\"online\":" + String(online ? "true" : "false");
    json += "}";
    if (i < MAX_DEVICES) json += ",";
  }
  json += "]";
  server.send(200, "application/json", json);
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(PIN_S1, INPUT_PULLUP);
  pinMode(PIN_S2, INPUT_PULLUP);
  pinMode(PIN_S3, INPUT_PULLUP);
  pinMode(PIN_S4, INPUT_PULLUP);
  delay(20); // let pins settle

  isMain     = (digitalRead(PIN_S1) == LOW); // ON = main
  myDeviceId = readDeviceIdFromSwitches();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed, restarting...");
    delay(1000);
    ESP.restart();
  }

  if (isMain) {
    Serial.println("Role: MAIN (receiver)");
    esp_now_register_recv_cb(onDataRecv);

    // Own Access Point so a phone/laptop can reach the dashboard directly
    WiFi.softAP("ESPNOW-Main", "12345678", WIFI_CHANNEL);
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());

    server.on("/", handleRoot);
    server.on("/data", handleData);
    server.begin();
  } else {
    Serial.printf("Role: TRANSMITTER, device #%u\n", myDeviceId);
    esp_now_register_send_cb(onDataSent);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, broadcastMac, 6);
    peerInfo.channel = WIFI_CHANNEL;
    peerInfo.encrypt = false;
    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
      Serial.println("Failed to add broadcast peer");
    }
  }
}

// ---------- Loop ----------
unsigned long lastSendMs = 0;
uint32_t seqCounter = 0;

void loop() {
  if (isMain) {
    server.handleClient();
  } else {
    if (millis() - lastSendMs > 3000) { // send every 3s
      lastSendMs = millis();

      SensorPacket pkt;
      pkt.deviceId = myDeviceId;
      // TODO: replace these two lines with real sensor readings
      pkt.value1 = 20.0 + random(0, 100) / 10.0;
      pkt.value2 = 40.0 + random(0, 200) / 10.0;
      pkt.seq = seqCounter++;

      esp_now_send(broadcastMac, (uint8_t*)&pkt, sizeof(pkt));
    }
  }
}