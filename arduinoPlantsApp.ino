#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"

HardwareSerial RS485(2);

uint16_t modbusCRC(const uint8_t* buf, uint16_t len) {
  uint16_t crc = 0xFFFF;
  for (uint16_t pos = 0; pos < len; pos++) {
    crc ^= (uint16_t)buf[pos];
    for (int i = 0; i < 8; i++) {
      if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
      else { crc >>= 1; }
    }
  }
  return crc;
}

void rs485TxEnable(bool en) {
  digitalWrite(PIN_DERE, en ? HIGH : LOW);
  delayMicroseconds(80);
}

void flushInput() { while (RS485.available()) RS485.read(); }

int modbusTransceive(const uint8_t* req, uint16_t reqLen,
                     uint8_t* resp, uint16_t respMax,
                     uint32_t timeoutMs) {
  flushInput();

  rs485TxEnable(true);
  RS485.write(req, reqLen);
  RS485.flush();
  rs485TxEnable(false);

  uint32_t start = millis();
  uint16_t idx = 0;

  while ((millis() - start) < timeoutMs && idx < respMax) {
    if (RS485.available()) {
      resp[idx++] = RS485.read();
      start = millis();
    }
  }
  return idx;
}

bool read8Regs(uint16_t regs[8]) {
  uint8_t req[8];
  req[0] = SLAVE_ID;
  req[1] = 0x03;
  req[2] = 0x00; req[3] = 0x00;
  req[4] = 0x00; req[5] = 0x08;

  uint16_t crc = modbusCRC(req, 6);
  req[6] = (uint8_t)(crc & 0xFF);
  req[7] = (uint8_t)(crc >> 8);

  uint8_t resp[64];
  int rlen = modbusTransceive(req, 8, resp, sizeof(resp), 450);

  if (rlen < 5) return false;
  if (resp[0] != SLAVE_ID) return false;
  if (resp[1] & 0x80) return false;
  if (resp[1] != 0x03) return false;

  uint8_t byteCount = resp[2];
  if (byteCount != 16) return false;
  if (rlen < (3 + byteCount + 2)) return false;

  uint16_t gotCrc  = (uint16_t)resp[rlen - 2] | ((uint16_t)resp[rlen - 1] << 8);
  uint16_t calcCrc = modbusCRC(resp, rlen - 2);
  if (gotCrc != calcCrc) return false;

  for (int i = 0; i < 8; i++) {
    int base = 3 + i * 2;
    regs[i] = ((uint16_t)resp[base] << 8) | (uint16_t)resp[base + 1];
  }
  return true;
}

// cache ostatniego pomiaru
struct SoilData {
  bool ok = false;
  unsigned long ts = 0;
  float tempC = NAN;
  float moistPct = NAN;
  uint16_t ec = 0;
  float ph = NAN;
  uint16_t n=0,p=0,k=0,salt=0;
} last;

void updateMeasurement() {
  uint16_t r[8];
  if (!read8Regs(r)) {
    last.ok = false;
    last.ts = millis();
    return;
  }
  // potwierdzony mapping:
  // r[0] Temp x10
  // r[1] Moist x10
  // r[2] EC
  // r[3] Salt
  // r[4] N
  // r[5] P
  // r[6] K
  // r[7] pH x100
  last.tempC = ((int16_t)r[0]) / 10.0f;
  last.moistPct = r[1] / 10.0f;
  last.ec = r[2];
  last.salt = r[3];
  last.n = r[4];
  last.p = r[5];
  last.k = r[6];
  last.ph = r[7] / 100.0f;
  last.ok = true;
  last.ts = millis();
}

String soilJson() {
  updateMeasurement();

  String body = "{";
  body += "\"ok\":" + String(last.ok ? "true" : "false") + ",";
  body += "\"ts_ms\":" + String(last.ts) + ",";
  if (last.ok) {
    body += "\"temp_c\":" + String(last.tempC, 1) + ",";
    body += "\"moist_pct\":" + String(last.moistPct, 1) + ",";
    body += "\"ec_uscm\":" + String(last.ec) + ",";
    body += "\"ph\":" + String(last.ph, 2) + ",";
    body += "\"n_mgkg\":" + String(last.n) + ",";
    body += "\"p_mgkg\":" + String(last.p) + ",";
    body += "\"k_mgkg\":" + String(last.k) + ",";
    body += "\"salt_mgl\":" + String(last.salt);
  }
  body += "}";
  return body;
}

void handleRoot() {
  String msg = "ESP32 soil sensor\n";
  msg += "Use: /soil\n";
  msg += "IP: " + WiFi.localIP().toString() + "\n";
  msg += "Mode: " + String(WiFi.getMode() == WIFI_AP ? "AP" : "STA") + "\n";
  server.send(200, "text/plain", msg);
}

void handleSoil() {
  server.send(200, "application/json", soilJson());
}

void startHttpServer() {
  server.on("/", handleRoot);
  server.on("/soil", handleSoil);
  server.begin();
  Serial.println("HTTP server started.");
  Serial.println("Open /soil in browser.");
}

bool connectSTA_withTimeout(uint32_t timeoutMs) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeoutMs) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("WiFi connect failed (timeout).");
  return false;
}

void startAPFallback() {
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("AP mode: ");
  Serial.println(ok ? "started" : "failed");
  Serial.print("AP SSID: "); Serial.println(AP_SSID);
  Serial.print("AP PASS: "); Serial.println(AP_PASS);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP()); // zwykle 192.168.4.1
}

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println();
  Serial.println("BOOT: starting...");

  pinMode(PIN_DERE, OUTPUT);
  rs485TxEnable(false);
  RS485.begin(MODBUS_BAUD, SERIAL_8N1, PIN_RX, PIN_TX);

  Serial.println("RS485 initialized.");

  // 1) próbuj STA (Twoje Wi-Fi)
  bool staOk = connectSTA_withTimeout(15000);

  // 2) jeśli nie wyszło — uruchom AP
  if (!staOk) {
    startAPFallback();
  }

  startHttpServer();

  Serial.println("Ready.");
  Serial.print("Try in browser: http://");
  Serial.print((WiFi.getMode() == WIFI_AP) ? WiFi.softAPIP() : WiFi.localIP());
  Serial.println("/soil");
}

void loop() {
  server.handleClient();
}
