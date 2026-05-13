#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <HTTPClient.h>
#include <ArduinoJson.h> 
#include <Adafruit_BME680.h>
#include <Wire.h>
#include "credentials.h"
#include "time.h"
#include "esp_sleep.h"

 

// --- Об'єкти ---
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
    Serial.println(F("\nПомилка Wi-Fi! Сон."));
    enterDeepSleep(sleep_interval);
  }
  Serial.println(F("\nWi-Fi підключено."));
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  Serial.print("Синхронізація часу...");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1735689600 && retries < 10) { 
    delay(500);
    now = time(nullptr);
    Serial.print(".");
    retries++;
  }
  Serial.println();
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

void transmitDataAndReceiveCommand(SensorReadings data) {
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
  // Додаємо ?mode=control, щоб Lambda знала, що треба повернути команду
  String url = String(http_endpoint) + "?mode=control";
  http.begin(espClient, url);
  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(buffer); 

  if (httpCode == 200) {
    String response = http.getString();
    Serial.println(F("Успішна відповідь від хмари:"));
    Serial.println(response);

    // Парсинг відповіді для отримання команди
    StaticJsonDocument<200> respDoc;
    DeserializationError error = deserializeJson(respDoc, response);
    if (!error) {
        if (respDoc.containsKey("sleep_interval")) {
            int new_interval = respDoc["sleep_interval"].as<int>();
            sleep_interval = constrain(new_interval, 30, 1200);
            Serial.printf("Хмара оновила інтервал: %d с\n", sleep_interval);
        }
    } else {
        Serial.println("Помилка парсингу JSON відповіді.");
    }

  } else {
    Serial.printf("Помилка HTTP! Код: %d\n", httpCode);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- HTTP v4 ---");

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

  // 1. Зчитування
  SensorReadings current = readSensorData();
  if (!current.success) enterDeepSleep(sleep_interval);

  // 2. Підключення
  connectWiFi();
  syncTime();

  // 3. Відправка та отримання команди (в одному кроці для HTTP)
  transmitDataAndReceiveCommand(current);
  
  delay(100); 
  enterDeepSleep(sleep_interval);
}

void loop() {}