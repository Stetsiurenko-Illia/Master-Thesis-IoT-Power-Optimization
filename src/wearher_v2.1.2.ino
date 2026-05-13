#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h" // Уніфіковано
#include "credentials.h"
#include "esp_sleep.h"

// --- Об'єкти ---
Adafruit_BME680 bme;
WiFiClientSecure espClient;
PubSubClient mqtt_client(espClient);
bool timeStatus = false;

// Уніфікована структура для даних
struct SensorReadings {
  float temperature;
  float humidity;
  float pressure;
  float voc;
  bool success;
};

// Функція переходу в сон (тепер з кнопкою)
void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  WiFi.disconnect(true);
  mqtt_client.disconnect();
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

void connectMQTT() {
  espClient.setCACert(aws_root_ca);
  espClient.setCertificate(aws_client_cert);
  espClient.setPrivateKey(aws_private_key);
  mqtt_client.setServer(mqtt_server, 8883);

  Serial.print("Підключення до MQTT...");
  if (!mqtt_client.connect(mqtt_client_id)) {
    Serial.print(F(" Помилка! Код: "));
    Serial.println(mqtt_client.state());
    enterDeepSleep(MAIN_INTERVAL / 1000);
  }
  Serial.println(F(" MQTT підключено."));
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
  if (timeStatus) {
    doc["timestamp"] = time(nullptr); 
  }

  char buffer[256];
  serializeJson(doc, buffer);
  
  Serial.printf("Відправка MQTT: %s\n", buffer);

  if (mqtt_client.connected()) {
    if (mqtt_client.publish(mqtt_topic, buffer)) {
      Serial.println(F("Дані опубліковано."));
    } else {
      Serial.println(F("Помилка публікації."));
    }
  } else {
    Serial.println(F("MQTT не підключено."));
  }
}

// --- ГОЛОВНА ЛОГІКА V2 ---
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- MQTT v2 ---");

  // Кнопка тут потрібна лише для esp_sleep_enable_ext0_wakeup
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

  // Безумовне підключення та відправка
  connectWiFi();
  syncTime();
  connectMQTT();
  transmitData();
  
  delay(200); // Невелика затримка, щоб MQTT встиг відправити пакет

  // Перехід у сон на фіксований час
  enterDeepSleep(MAIN_INTERVAL / 1000);
}

void loop() {
  // Порожній
}