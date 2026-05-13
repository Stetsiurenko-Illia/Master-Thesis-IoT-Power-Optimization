/**
 * @file coap_v3_unified.ino
 * @brief CoAP v3 - Локальна адаптація (Unified).
 */

#include <WiFi.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h"
#include "credentials.h"
#include "esp_sleep.h"

// --- Адаптація ---
RTC_DATA_ATTR float previous_values[4] = {0, 0, 0, 0}; 
RTC_DATA_ATTR int sleep_interval = 60; 
const float stability_thresholds[] = {0.5, 1.0, 1.0, 25.0}; 
#define uS_TO_S_FACTOR 1000000ULL

WiFiUDP udp;
Adafruit_BME680 bme;
byte packetBuffer[512]; 
uint16_t messageID = 0;

struct SensorReadings {
  float temperature;
  float humidity;
  float pressure;
  float voc;
  bool success;
};

void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); 
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < wifi_timeout) {
    delay(500);
  }
  if (WiFi.status() != WL_CONNECTED) enterDeepSleep(sleep_interval);
  Serial.println("\nWi-Fi підключено.");
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1735689600 && retries < 10) { 
    delay(500);
    now = time(nullptr);
    retries++;
  }
}

void sendCoap(const char* payload, size_t len) {
  int idx = 0;
  packetBuffer[idx++] = 0b01010000; packetBuffer[idx++] = 0x03; // PUT
  packetBuffer[idx++] = highByte(messageID); packetBuffer[idx++] = lowByte(messageID++);
  packetBuffer[idx++] = 0xB7; memcpy(&packetBuffer[idx], "climate", 7); idx += 7;
  packetBuffer[idx++] = 0x04; memcpy(&packetBuffer[idx], "data", 4); idx += 4;
  packetBuffer[idx++] = 0xFF;
  memcpy(&packetBuffer[idx], payload, len); idx += len;

  if (udp.beginPacket(coap_server_ip, coap_port)) {
    udp.write(packetBuffer, idx);
    udp.endPacket();
    Serial.println("CoAP пакет відправлено.");
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
  }
  return readings;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- CoAP v3 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) sleep_interval = 30; 

  if (!bme.begin()) enterDeepSleep(60);
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  SensorReadings current = readSensorData();
  if (!current.success) enterDeepSleep(sleep_interval);

  // Адаптація
  bool is_stable = true;
  if (previous_values[0] != 0) { 
      if (abs(current.temperature - previous_values[0]) > stability_thresholds[0]) is_stable = false;
      if (abs(current.humidity - previous_values[1]) > stability_thresholds[1]) is_stable = false;
      if (abs(current.pressure - previous_values[2]) > stability_thresholds[2]) is_stable = false;
      if (abs(current.voc - previous_values[3]) > stability_thresholds[3]) is_stable = false;
  } else { is_stable = false; }

  if (is_stable) {
    sleep_interval = min(sleep_interval * 2, 600); 
    Serial.printf("Стабільно. Інтервал: %d\n", sleep_interval);
  } else {
    sleep_interval = max(sleep_interval / 2, 30);
    Serial.printf("Зміни. Інтервал: %d\n", sleep_interval);
  }

  previous_values[0] = current.temperature;
  previous_values[1] = current.humidity;
  previous_values[2] = current.pressure;
  previous_values[3] = current.voc;

  connectWiFi();
  syncTime();

  StaticJsonDocument<256> doc;
  doc["temperature"] = current.temperature;
  doc["humidity"] = current.humidity;
  doc["pressure"] = current.pressure;
  doc["voc"] = current.voc;
  doc["timestamp"] = time(nullptr);
  char buffer[256];
  size_t len = serializeJson(doc, buffer);

  sendCoap(buffer, len);

  // Чекаємо трохи, щоб пакет точно пішов (UDP)
  delay(200);
  enterDeepSleep(sleep_interval);
}

void loop() {}