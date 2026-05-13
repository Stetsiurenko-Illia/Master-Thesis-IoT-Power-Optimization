#include <WiFi.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h"
#include "credentials.h"
#include "esp_sleep.h"

// --- Об'єкти ---
WiFiUDP udp;
Adafruit_BME680 bme;
byte packetBuffer[512]; 
uint16_t messageID = 0;
bool timeStatus = false;

struct SensorReadings {
  float temperature;
  float humidity;
  float pressure;
  float voc;
  bool success;
};

// Функція переходу в сон (з кнопкою)
void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  // ДОДАНО: Пробудження від кнопки
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); 
  esp_sleep_enable_timer_wakeup(seconds * 1000000ULL);
  esp_deep_sleep_start();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Підключення до Wi-Fi");
  unsigned long start_time = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start_time < wifi_timeout) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("\nПомилка підключення Wi-Fi! Перехід у сон."));
    enterDeepSleep(MAIN_INTERVAL / 1000);
  }
  Serial.println(F("\nWi-Fi підключено."));
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  Serial.print("Синхронізація часу...");
  time_t now = time(nullptr);
  while (now < 1735689600) { // 1 січня 2025
    delay(100);
    now = time(nullptr);
    Serial.print(".");
  }
  Serial.println("\nЧас синхронізовано.");
  timeStatus = true;
}

void sendCoapPutRequest(const char* payload, size_t payloadLen) {
  int bufferIndex = 0;
  packetBuffer[bufferIndex++] = 0b01010000;
  packetBuffer[bufferIndex++] = 0x03;
  packetBuffer[bufferIndex++] = highByte(messageID);
  packetBuffer[bufferIndex++] = lowByte(messageID);
  messageID++;
  
  packetBuffer[bufferIndex++] = 0xB7; memcpy(&packetBuffer[bufferIndex], "climate", 7); bufferIndex += 7;
  packetBuffer[bufferIndex++] = 0x04; memcpy(&packetBuffer[bufferIndex], "data", 4); bufferIndex += 4;
  packetBuffer[bufferIndex++] = 0xFF;
  
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
    Serial.println("Очікування відповіді від шлюзу...");
    unsigned long listenStart = millis();
    while (millis() - listenStart < 2000) { 
        int packetSize = udp.parsePacket();
        if (packetSize) {
            Serial.println("\n--- Отримано відповідь від шлюзу ---");
            byte replyBuffer[packetSize];
            udp.read(replyBuffer, packetSize);
            return; 
        }
        delay(10);
    }
    Serial.println("Час очікування відповіді вийшов.");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- CoAP v2 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  if (!bme.begin()) {
    Serial.println(F("Помилка ініціалізації BME680!"));
    enterDeepSleep(MAIN_INTERVAL / 1000);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  connectWiFi();
  syncTime();
  transmitData();
  listenForResponse();

  enterDeepSleep(MAIN_INTERVAL / 1000);
}

void loop() {
  // Порожній
}