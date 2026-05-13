#include <WiFi.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h"
#include "credentials.h"

// --- Об'єкти ---
WiFiUDP udp;
Adafruit_BME680 bme;
byte packetBuffer[512]; // Глобальний буфер
uint16_t messageID = 0;
bool timeStatus = false;

// Структура для даних
struct SensorReadings {
  float temperature;
  float humidity;
  float pressure;
  float voc;
  bool success;
};

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Підключення до Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi підключено.");
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  Serial.print("Синхронізація часу...");
  time_t now = time(nullptr);
  while (now < 1735689600) { 
    delay(100);
    now = time(nullptr);
    Serial.print(".");
  }
  Serial.println("\nЧас синхронізовано.");
  timeStatus = true;
}

void sendCoapPutRequest(const char* payload, size_t payloadLen) {
  int bufferIndex = 0;

  packetBuffer[bufferIndex++] = 0b01010000;  // Ver=1, Type=NON, TKL=0
  packetBuffer[bufferIndex++] = 0x03;        // Code=PUT
  packetBuffer[bufferIndex++] = highByte(messageID);
  packetBuffer[bufferIndex++] = lowByte(messageID);
  messageID++;
  
  packetBuffer[bufferIndex++] = 0xB7; // Uri-Path: "climate"
  memcpy(&packetBuffer[bufferIndex], "climate", 7);
  bufferIndex += 7;
  packetBuffer[bufferIndex++] = 0x04; // Uri-Path: "data"
  memcpy(&packetBuffer[bufferIndex], "data", 4);
  bufferIndex += 4;
  
  packetBuffer[bufferIndex++] = 0xFF; // Маркер
  
  memcpy(&packetBuffer[bufferIndex], payload, payloadLen);
  bufferIndex += payloadLen;

  if (udp.beginPacket(coap_server_ip, coap_port)) {
    udp.write(packetBuffer, bufferIndex);
    if (udp.endPacket()) {
      Serial.println("Запит CoAP успішно надіслано.");
    } else {
      Serial.println("Помилка відправки UDP пакета!");
    }
  } else {
    Serial.println("Помилка ініціалізації UDP з'єднання!");
  }
}

SensorReadings readSensorData() {
  SensorReadings readings = {0, 0, 0, 0, false};
  if (bme.performReading()) {
    readings.temperature = bme.temperature;
    readings.humidity = bme.humidity;
    readings.pressure = bme.pressure / 100.0;
    readings.voc = bme.gas_resistance / 1000.0;
    readings.success = true;
  } else {
    Serial.println(F("Помилка зчитування BME680!"));
  }
  return readings;
}

void transmitData() {
  SensorReadings data = readSensorData();
  if (!data.success) return;

  StaticJsonDocument<256> doc;
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["pressure"] = data.pressure;
  doc["voc"] = data.voc;
  if (timeStatus) {
    doc["timestamp"] = time(nullptr);
  }

  char buffer[256];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));

  Serial.printf("Відправка CoAP: %s\n", buffer);
  
  sendCoapPutRequest(buffer, len);
}

void listenForResponse() {
  int packetSize = udp.parsePacket();
  if (packetSize) {
    Serial.print("\n--- Отримано відповідь від шлюзу (");
    Serial.print(packetSize);
    Serial.println(" байт) ---");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- CoAP v1 ---");
  
  if (!bme.begin()) {
    Serial.println(F("Помилка ініціалізації BME680!"));
    while (1);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  connectWiFi();
  syncTime();
}

void loop() {
  // Кнопка для перезавантаження
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Кнопка натиснута. Перезавантаження...");
    delay(1000);
    ESP.restart();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Втрачено Wi-Fi. Спроба перепідключення..."));
    connectWiFi();
  }
  
  transmitData();
  
  // Даємо час на отримання відповіді
  unsigned long wait_start = millis();
  while(millis() - wait_start < 2000) {
    listenForResponse();
    delay(10);
  }
    
  Serial.printf("Очікування %d секунд...\n", MAIN_INTERVAL/1000);
  delay(MAIN_INTERVAL); 
}