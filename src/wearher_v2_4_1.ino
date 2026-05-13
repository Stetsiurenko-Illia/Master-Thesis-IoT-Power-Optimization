#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLEAdvertising.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "credentials.h"

Adafruit_BME680 bme;
BLEAdvertising *pAdvertising;

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
  float temp_sum = 0, hum_sum = 0, pres_sum = 0, voc_sum = 0;
  int readings = 0;
  for (int i = 0; i < 3; i++) { 
      if (bme.performReading()) {
          temp_sum += bme.temperature;
          hum_sum += bme.humidity;
          pres_sum += bme.pressure / 100.0; // hPa
          voc_sum += bme.gas_resistance / 1000.0; // kOhm
          readings++;
      }
      delay(50); 
  }

  if (readings > 0) {
      dataToSend.temperature = (int16_t)((temp_sum / readings) * 100.0);
      dataToSend.humidity = (uint16_t)((hum_sum / readings) * 100.0);
      dataToSend.pressure = (uint16_t)(pres_sum / readings);
      dataToSend.voc = (uint16_t)((voc_sum / readings) * 10.0);
      return true;
  } else {
      Serial.println("Помилка зчитування BME680!");
      return false;
  }
}

void transmitData() {
  if (!readSensorData()) return;

  Serial.printf("Зчитано дані: T=%.2f C, H=%.2f %%, P=%.0f hPa, VOC=%.1f kOhm\n", 
                (float)dataToSend.temperature / 100.0, 
                (float)dataToSend.humidity / 100.0, 
                (float)dataToSend.pressure, 
                (float)dataToSend.voc / 10.0);

  // Формуємо Manufacturer Data
  uint8_t manufacturerDataBytes[2 + sizeof(dataToSend)];
  manufacturerDataBytes[0] = MANUFACTURER_ID & 0xFF;        
  manufacturerDataBytes[1] = (MANUFACTURER_ID >> 8) & 0xFF; 
  memcpy(&manufacturerDataBytes[2], &dataToSend, sizeof(dataToSend));    

  BLEAdvertisementData advertisementData;
  advertisementData.setManufacturerData(String((char*)manufacturerDataBytes, sizeof(manufacturerDataBytes)));
  pAdvertising->setAdvertisementData(advertisementData);

  Serial.println("Початок BLE Advertising...");
  pAdvertising->start();

  delay(ADVERTISING_DURATION); 

  Serial.println("Зупинка BLE Advertising.");
  pAdvertising->stop();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- BLE v1 ---");
  
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

  BLEDevice::init("ESP32_BME_Sensor_V1"); 
  pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->setMinInterval(0x20); 
  pAdvertising->setMaxInterval(0x40); 
  pAdvertising->setScanResponse(false);
  
  Serial.println("Налаштування завершено.");
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