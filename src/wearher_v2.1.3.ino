/**
 * @file mqtt_v3_unified.ino
 * @brief MQTT v3 - Локальна адаптація (Unified Wake-and-Sleep).
 * * Логіка: Прокинувся -> Аналіз -> Розрахунок сну -> Відправка -> Сон.
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h"
#include "credentials.h"
#include "esp_sleep.h"

// --- НАЛАШТУВАННЯ АДАПТАЦІЇ (RTC) ---
RTC_DATA_ATTR float previous_values[4] = {0, 0, 0, 0}; // T, H, P, VOC
RTC_DATA_ATTR int sleep_interval = 30; // Початковий інтервал

// Пороги стабільності: T(0.5C), H(1%), P(1hPa), VOC(25kOhm)
const float stability_thresholds[] = {0.5, 1.0, 1.0, 25.0}; 
#define uS_TO_S_FACTOR 1000000ULL

// --- Об'єкти ---
Adafruit_BME680 bme;
WiFiClientSecure espClient;
PubSubClient mqtt_client(espClient);

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
  mqtt_client.disconnect();
  // Пробудження від кнопки
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
  Serial.print("Синхронізація часу");
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 1735689600 && retries < 10) { // 1 січня 2025
    delay(500);
    now = time(nullptr);
    Serial.print(".");
    retries++;
  }
  Serial.println();
}

// Порожній callback (v3 не слухає команди)
void callback(char* topic, byte* payload, unsigned int length) {}

void connectMQTT() {
  espClient.setCACert(aws_root_ca);
  espClient.setCertificate(aws_client_cert);
  espClient.setPrivateKey(aws_private_key);
  mqtt_client.setServer(mqtt_server, 8883);
  mqtt_client.setCallback(callback);

  if (!mqtt_client.connect(mqtt_client_id)) {
    Serial.print(F("Помилка MQTT: "));
    Serial.println(mqtt_client.state());
    enterDeepSleep(sleep_interval);
  }
  Serial.println(F("MQTT підключено."));
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

void transmitData(SensorReadings data) {
  StaticJsonDocument<200> doc;
  doc["temperature"] = data.temperature;
  doc["humidity"] = data.humidity;
  doc["pressure"] = data.pressure;
  doc["voc"] = data.voc;
  doc["timestamp"] = time(nullptr); 

  char buffer[256];
  serializeJson(doc, buffer);
  
  Serial.printf("Відправка MQTT: %s\n", buffer);

  if (mqtt_client.connected()) {
    mqtt_client.publish(mqtt_topic, buffer);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- MQTT v3 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Якщо розбудили кнопкою - скидаємо інтервал
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Кнопка! Скидання інтервалу.");
      sleep_interval = 30; 
  }

  if (!bme.begin()) {
    Serial.println(F("Помилка BME680!"));
    enterDeepSleep(60);
  }
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  // 1. Зчитування
  SensorReadings current = readSensorData();
  if (!current.success) enterDeepSleep(sleep_interval);

  // 2. Адаптація (порівняння з RTC)
  bool is_stable = true;
  if (previous_values[0] != 0) { 
      if (abs(current.temperature - previous_values[0]) > stability_thresholds[0]) is_stable = false;
      if (abs(current.humidity - previous_values[1]) > stability_thresholds[1]) is_stable = false;
      if (abs(current.pressure - previous_values[2]) > stability_thresholds[2]) is_stable = false;
      if (abs(current.voc - previous_values[3]) > stability_thresholds[3]) is_stable = false;
  } else {
      is_stable = false; // Перший запуск
  }

  // 3. Розрахунок нового інтервалу (для НАСТУПНОГО разу)
  if (is_stable) {
    sleep_interval = min(sleep_interval * 2, 600); // Макс 10 хв
    Serial.printf("Стабільно. Інтервал збільшено до %d с.\n", sleep_interval);
  } else {
    sleep_interval = max(sleep_interval / 2, 30);  // Мін 30 сек
    Serial.printf("Зміни! Інтервал зменшено до %d с.\n", sleep_interval);
  }

  // 4. Збереження в RTC
  previous_values[0] = current.temperature;
  previous_values[1] = current.humidity;
  previous_values[2] = current.pressure;
  previous_values[3] = current.voc;

  // 5. Відправка (Завжди)
  connectWiFi();
  syncTime();
  connectMQTT();
  transmitData(current);
  
  delay(200); // Час на передачу пакету

  // 6. Сон
  enterDeepSleep(sleep_interval);
}

void loop() {}