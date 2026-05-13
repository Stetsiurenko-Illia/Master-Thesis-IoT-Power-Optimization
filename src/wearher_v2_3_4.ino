#include <WiFi.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "time.h"
#include "credentials.h"
#include "esp_sleep.h"

// --- ЗМІННІ ---
RTC_DATA_ATTR int sleep_interval = 30; 
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
    udp.endPacket();
  }
}

void listenForResponse() {
    Serial.println("Очікування відповіді від шлюзу (5 сек)...");
    unsigned long listenStart = millis();
    bool responseReceived = false;

    while (millis() - listenStart < 5000) { 
        int packetSize = udp.parsePacket();
        if (packetSize) {
            responseReceived = true;
            Serial.println("\n--- Отримано відповідь від шлюзу ---");
            
            byte replyBuffer[packetSize];
            udp.read(replyBuffer, packetSize);

            // Знаходимо початок payload (0xFF)
            int payloadStart = -1;
            for (int i = 0; i < packetSize; i++) {
              if (replyBuffer[i] == 0xFF) {
                payloadStart = i + 1;
                break;
              }
            }

            if (payloadStart != -1 && payloadStart < packetSize) {
                char payload[packetSize - payloadStart + 1];
                memcpy(payload, &replyBuffer[payloadStart], packetSize - payloadStart);
                payload[packetSize - payloadStart] = '\0';
                Serial.printf("Payload: %s\n", payload);

                StaticJsonDocument<200> doc;
                DeserializationError error = deserializeJson(doc, payload);

                if (!error) {
                    if (doc.containsKey("sleep_interval")) {
                        int new_interval = doc["sleep_interval"].as<int>();
                        sleep_interval = constrain(new_interval, 30, 1200); 
                        Serial.printf("Хмара оновила інтервал: %d с\n", sleep_interval);
                    }
                } else {
                    Serial.println("Помилка JSON відповіді.");
                }
            }
            break; 
        }
        delay(10);
    }
    if (!responseReceived) Serial.println("Таймаут відповіді. Використовуємо старий інтервал.");
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
  Serial.println("\n--- CoAP v4 ---");

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

  // 3. Відправка
  StaticJsonDocument<256> doc;
  doc["temperature"] = current.temperature;
  doc["humidity"] = current.humidity;
  doc["pressure"] = current.pressure;
  doc["voc"] = current.voc;
  doc["timestamp"] = time(nullptr);
  char buffer[256];
  size_t len = serializeJson(doc, buffer, sizeof(buffer)); 
  
  Serial.printf("Відправка CoAP: %s\n", buffer);
  sendCoapPutRequest(buffer, len);
  
  // 4. Очікування команди
  listenForResponse();

  // 5. Сон
  enterDeepSleep(sleep_interval);
}

void loop() {}