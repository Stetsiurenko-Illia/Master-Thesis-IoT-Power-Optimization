#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "lorawan_config.h"

Adafruit_BME680 bme;

// Структура для пакування даних
struct __attribute__((packed)) SensorData {
    int16_t temperature; 
    uint16_t humidity;   
    uint16_t pressure;   
    uint16_t voc;        
};
SensorData dataToSend;

// Уніфікована функція зчитування
bool readSensorData() {
  // Даємо датчику час "прогрітися"
  delay(1000); 
  if (bme.performReading()) {
      dataToSend.temperature = (int16_t)(bme.temperature * 100.0);
      dataToSend.humidity = (uint16_t)(bme.humidity * 100.0);
      dataToSend.pressure = (uint16_t)(bme.pressure / 100.0);
      dataToSend.voc = (uint16_t)(bme.gas_resistance / 1000.0); // kOhm
      return true;
  } else {
      Serial.println("Помилка зчитування BME!");
      return false;
  }
}

void transmitData() {
  if (!readSensorData()) return;

  Serial.printf("Зчитано дані: T=%.2f C, H=%.2f %%, P=%.0f hPa, VOC=%.1f kOhm\n", 
                (float)dataToSend.temperature / 100.0, 
                (float)dataToSend.humidity / 100.0, 
                (float)dataToSend.pressure, 
                (float)dataToSend.voc); 

  // Відправляємо пакет
  Serial.print("Відправка LoRa пакета... ");
  LoRa.beginPacket();
  LoRa.write((uint8_t*)&dataToSend, sizeof(dataToSend));
  LoRa.endPacket();
  Serial.println("Пакет відправлено.");
}

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(1000);
    
    // Вимикаємо Wi-Fi та BT для максимальної економії
    WiFi.mode(WIFI_OFF);
    btStop();
    
    Serial.println("--- LoRa v1 ---");
    
    // Уніфікована ініціалізація кнопки
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    if (!bme.begin()) {
        Serial.println(F("Помилка ініціалізації BME680!"));
        while(1); 
    }
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3); 
    bme.setGasHeater(320, 150); 
    Serial.println(F("BME680 ініціалізовано."));

    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    if (!LoRa.begin(LORA_FREQUENCY)) { 
        Serial.println("Помилка запуску LoRa!");
        while (1);
    }
    Serial.println("LoRa ініціалізовано.");
}

void loop() {
  // Перевірка кнопки для перезавантаження
  if (digitalRead(BUTTON_PIN) == LOW) {
    Serial.println("Кнопка натиснута. Перезавантаження...");
    delay(1000);
    ESP.restart();
  }

  transmitData();
  
  Serial.printf("Очікування %d секунд...\n", MAIN_INTERVAL / 1000);
  delay(MAIN_INTERVAL);
}