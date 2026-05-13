#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <Adafruit_BME680.h>
#include <Wire.h>
#include "credentials.h"
#include "time.h"

// --- Об'єкти ---
Adafruit_BME680 bme;
WiFiClientSecure espClient;
HTTPClient http;

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
  unsigned long start_time = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start_time < wifi_timeout) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("\nПомилка підключення Wi-Fi! Перезавантаження..."));
    ESP.restart();
  }
  Serial.println(F("\nWi-Fi підключено."));
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
  http.setTimeout(10000);    
  http.setReuse(false);       

  int httpCode = http.POST(buffer); 

  if (httpCode == 200) {
    Serial.println(F("Дані опубліковано (HTTP 200 OK)"));
  } else {
    Serial.printf("Помилка HTTP! Код: %d\n", httpCode);
    Serial.println(http.getString());
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("--- HTTP v1 ---");

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
  
  Serial.printf("Очікування %d секунд...\n", MAIN_INTERVAL / 1000);
  delay(MAIN_INTERVAL);
}