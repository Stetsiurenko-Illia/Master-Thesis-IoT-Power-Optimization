/**
 * @file http_v3_unified.ino
 * @brief HTTP v3 - Локальна адаптація (Unified).
 */

#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <Adafruit_BME680.h>
#include <Wire.h>
#include "credentials.h"
#include "time.h"
#include "esp_sleep.h"

// --- Адаптація ---
RTC_DATA_ATTR float previous_values[4] = {0, 0, 0, 0}; 
RTC_DATA_ATTR int sleep_interval = 60; 
const float stability_thresholds[] = {0.5, 1.0, 1.0, 25.0}; 
#define uS_TO_S_FACTOR 1000000ULL

Adafruit_BME680 bme;
WiFiClientSecure espClient;
HTTPClient http;

struct SensorReadings {
  float temperature;
  float humidity;
  float pressure;
  float voc;
  bool success;
};

void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  WiFi.disconnect(true);
  http.end();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); 
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
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
    Serial.println(F("\nПомилка Wi-Fi! Сон."));
    enterDeepSleep(sleep_interval);
  }
  Serial.println(F("\nWi-Fi підключено."));
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1735689600 && retries < 10) { 
    delay(500);
    now = time(nullptr);
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

void transmitData(SensorReadings data) {
  StaticJsonDocument<200> doc;
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["pressure"] = data.pressure;
  doc["voc"] = data.voc;
  doc["timestamp"] = time(nullptr); 

  char buffer[256];
  serializeJson(doc, buffer);
  
  Serial.printf("Відправка HTTP: %s\n", buffer);

  espClient.setCACert(aws_root_ca); 
  http.begin(espClient, http_endpoint);
  http.addHeader("Content-Type", "application/json");
  
  int httpCode = http.POST(buffer);
  if (httpCode != 200) {
    Serial.printf("Помилка HTTP: %d\n", httpCode);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- HTTP v3 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Кнопка! Скидання інтервалу.");
      sleep_interval = 30; 
  }

  if (!bme.begin()) { enterDeepSleep(60); }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  // 1. Аналіз
  SensorReadings current = readSensorData();
  if (!current.success) enterDeepSleep(sleep_interval);

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

  // 2. Відправка
  connectWiFi();
  syncTime();
  transmitData(current);
  
  delay(100); 
  enterDeepSleep(sleep_interval);
}

void loop() {}