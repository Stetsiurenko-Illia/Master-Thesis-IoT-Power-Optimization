#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h> 
#include <BLEAdvertising.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include "esp_sleep.h" 
#include "credentials.h"

// --- Адаптація ---
RTC_DATA_ATTR float previous_values[4] = {0, 0, 0, 0};
RTC_DATA_ATTR int sleep_interval = 60; 
const float stability_thresholds[] = {0.5, 1.0, 1.0, 25.0}; 
#define uS_TO_S_FACTOR 1000000ULL

Adafruit_BME680 bme;
BLEAdvertising *pAdvertising;

struct SensorData {
    int16_t temperature; 
    uint16_t humidity;   
    uint16_t pressure;   
    uint16_t voc;        
} __attribute__((packed));

void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  Serial.flush(); // Чекаємо завершення виводу в консоль
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0);
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n--- BLE v3 ---");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      Serial.println("Пробудження від кнопки! Скидання інтервалу.");
      sleep_interval = 30;
  }

  if (!bme.begin()) { 
      Serial.println("ERROR: BME680 not found!");
      enterDeepSleep(60); 
  }
  
  // Налаштування датчика
  bme.setTemperatureOversampling(BME680_OS_8X);
  bme.setHumidityOversampling(BME680_OS_2X);
  bme.setPressureOversampling(BME680_OS_4X);
  bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
  bme.setGasHeater(320, 150);

  // 1. Зчитування
  delay(500); 
  float t = 0, h = 0, p = 0, v = 0;
  if (bme.performReading()) {
      t = bme.temperature;
      h = bme.humidity;
      p = bme.pressure / 100.0;
      v = bme.gas_resistance / 1000.0;
      Serial.printf("Зчитано: T=%.2f, H=%.2f, P=%.2f, VOC=%.2f\n", t, h, p, v);
  } else {
      Serial.println("ERROR: Помилка читання BME680!");
      enterDeepSleep(sleep_interval);
  }

  // 2. Адаптація
  bool is_stable = true;            
  if (previous_values[0] != 0) {
      if (abs(t - previous_values[0]) > stability_thresholds[0]) { is_stable = false; }
      if (abs(h - previous_values[1]) > stability_thresholds[1]) { is_stable = false; }
      if (abs(p - previous_values[2]) > stability_thresholds[2]) { is_stable = false; }
      if (abs(v - previous_values[3]) > stability_thresholds[3]) { is_stable = false; }
  } else { 
      is_stable = false; 
  }

  if (is_stable) {
    sleep_interval = min(sleep_interval * 2, 600); 
    Serial.printf("Дані стабільні. Інтервал збільшено до %d\n", sleep_interval);
  } else {
    sleep_interval = max(sleep_interval / 2, 30);
    Serial.printf("Зміни! Інтервал зменшено до %d\n", sleep_interval);
  }

  previous_values[0] = t; previous_values[1] = h; previous_values[2] = p; previous_values[3] = v;

  // 3. Відправка (Advertising)
  BLEDevice::init("ESP32_BME_v3");
  pAdvertising = BLEDevice::getAdvertising();

  SensorData data;
  data.temperature = (int16_t)(t * 100);
  data.humidity = (uint16_t)(h * 100);
  data.pressure = (uint16_t)(p);
  data.voc = (uint16_t)(v * 10);

  uint8_t manufacturerData[2 + sizeof(data)];
  manufacturerData[0] = MANUFACTURER_ID & 0xFF;
  manufacturerData[1] = (MANUFACTURER_ID >> 8) & 0xFF;
  memcpy(&manufacturerData[2], &data, sizeof(data));

  BLEAdvertisementData advData;
  // Використовуємо std::string, якщо у вас все ще стара бібліотека, або String, якщо нова
  // Спробуйте String спочатку, як ми з'ясували
  advData.setManufacturerData(String((char*)manufacturerData, sizeof(manufacturerData)));
  pAdvertising->setAdvertisementData(advData);
  
  Serial.println("Початок BLE Advertising...");
  pAdvertising->start();
  
  Serial.println("Зупинка BLE Advertising.");
  pAdvertising->stop();
  BLEDevice::deinit(false);

  // 4. Сон
  enterDeepSleep(sleep_interval);
}

void loop() {}