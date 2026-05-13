/**
 * @file mqtt_v4_unified.ino
 * @brief MQTT v4 - Хмарно-керований Deep Sleep (Unified).
 * * Логіка:
 * 1. Прокидається.
 * 2. Відправляє дані.
 * 3. Чекає 5 секунд на команду від сервера (через callback).
 * 4. Оновлює sleep_interval.
 * 5. Засинає.
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
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); // Пробудження кнопкою
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR); // 1000000ULL визначено в credentials.h або тут
  esp_deep_sleep_start();
}

// Обробник вхідних повідомлень (команд від хмари)
void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print(F("Отримано повідомлення: "));
  String message;
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  Serial.println(message);

  // Перевіряємо, чи це топік команд (визначений в credentials.h або тут локально)
  // У credentials.h ми не визначили mqtt_command_topic, додамо його тут або використовуємо константу
  const char* command_topic = "climate/monitoring/commands"; 

  if (String(topic) == command_topic) {
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, message);
      
      if (!error) {
        if (doc.containsKey("sleep_interval")) {
          int new_interval = doc["sleep_interval"].as<int>();
          // Валідація отриманого значення
          sleep_interval = constrain(new_interval, 30, 1200); 
          Serial.printf("Хмара оновила інтервал сну: %d с\n", sleep_interval);
        }
        if (doc.containsKey("command") && strcmp(doc["command"], "force_transmit") == 0) {
           Serial.println("Отримано команду примусової передачі (для наступного разу).");
        }
      } else {
        Serial.println(F("Помилка JSON"));
      }
  }
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
    // Якщо немає зв'язку, спимо стільки ж, скільки минулого разу
    enterDeepSleep(sleep_interval);
  }
  Serial.println(F("\nWi-Fi підключено."));
}

void syncTime() {
  configTime(ntp_offset, 0, ntp_server);
  Serial.print("Синхронізація часу...");
  time_t now = time(nullptr);
  int retries = 0;
  // Використовуємо вашу правильну дату 2025 року
  while (now < 1735689600 && retries < 10) { 
    delay(500);
    now = time(nullptr);
    Serial.print(".");
    retries++;
  }
  Serial.println();
}

void connectMQTT() {
  espClient.setCACert(aws_root_ca);
  espClient.setCertificate(aws_client_cert);
  espClient.setPrivateKey(aws_private_key);
  mqtt_client.setServer(mqtt_server, 8883);
  mqtt_client.setCallback(callback); // Важливо для v4!

  Serial.print("Підключення до MQTT...");
  if (!mqtt_client.connect(mqtt_client_id)) {
    Serial.print(F(" Помилка! Код: "));
    Serial.println(mqtt_client.state());
    enterDeepSleep(sleep_interval);
  }
  Serial.println(F(" MQTT підключено."));
  
  // Підписуємось на топік команд
  mqtt_client.subscribe("climate/monitoring/commands");
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
  Serial.println("\n--- MQTT v4 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  // Якщо розбудили кнопкою, скидаємо на дефолт (для тестів)
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Кнопка! Скидання інтервалу до 30с.");
      sleep_interval = 30; 
  }

  if (!bme.begin()) {
    Serial.println(F("Помилка BME680!"));
    enterDeepSleep(60);
  }
  // Налаштування сенсора (стандартні)
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
  connectMQTT();

  // 3. Відправка
  transmitData(current);
  
  // 4. Очікування команди (Слухаємо ефір 5 секунд)
  Serial.println("Очікування команди від хмари...");
  unsigned long wait_start = millis();
  while (millis() - wait_start < 5000) {
    if (mqtt_client.connected()) {
        mqtt_client.loop(); // Це дозволяє отримати callback
    }
    delay(10);
  }

  // 5. Сон
  // sleep_interval міг оновитися в callback(), якщо прийшла команда
  enterDeepSleep(sleep_interval);
}

void loop() {
  // Порожній, оскільки вся логіка в setup()
}