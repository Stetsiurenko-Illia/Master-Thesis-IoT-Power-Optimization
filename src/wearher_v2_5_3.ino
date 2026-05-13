#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_BME680.h>
#include <WiFi.h> 
#include "esp_sleep.h"
#include "lorawan_config.h"

// --- Адаптація ---
RTC_DATA_ATTR float previous_values[4] = {0, 0, 0, 0}; 
RTC_DATA_ATTR int sleep_interval = 60; 
const float stability_thresholds[] = {0.5, 1.0, 1.0, 25.0}; 
#define uS_TO_S_FACTOR 1000000ULL

Adafruit_BME680 bme;

struct SensorData {
    int16_t temperature; 
    uint16_t humidity;   
    uint16_t pressure;   
    uint16_t voc;        
} __attribute__((packed));

void enterDeepSleep(int seconds) {
  Serial.printf("Перехід у Deep Sleep на %d секунд...\n", seconds);
  LoRa.sleep();
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_4, 0); 
  esp_sleep_enable_timer_wakeup(seconds * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    WiFi.mode(WIFI_OFF);
    btStop();
    Serial.println("\n--- LoRa v3 ---");
    
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    if(esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
      sleep_interval = 30;
    }

    if (!bme.begin()) { enterDeepSleep(60); }
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
    } else {
        enterDeepSleep(sleep_interval);
    }

    // 2. Адаптація
    bool is_stable = true;
    if (previous_values[0] != 0) {
        if (abs(t - previous_values[0]) > stability_thresholds[0]) is_stable = false;
        if (abs(h - previous_values[1]) > stability_thresholds[1]) is_stable = false;
        if (abs(p - previous_values[2]) > stability_thresholds[2]) is_stable = false;
        if (abs(v - previous_values[3]) > stability_thresholds[3]) is_stable = false;
    } else { is_stable = false; }

    if (is_stable) {
      sleep_interval = min(sleep_interval * 2, 600); 
      Serial.printf("Стабільно. Інтервал: %d\n", sleep_interval);
    } else {
      sleep_interval = max(sleep_interval / 2, 30);
      Serial.printf("Зміни. Інтервал: %d\n", sleep_interval);
    }

    previous_values[0] = t; previous_values[1] = h; previous_values[2] = p; previous_values[3] = v;

    // 3. Відправка
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    if (!LoRa.begin(LORA_FREQUENCY)) { enterDeepSleep(sleep_interval); }

    SensorData data;
    data.temperature = (int16_t)(t * 100);
    data.humidity = (uint16_t)(h * 100);
    data.pressure = (uint16_t)(p);
    data.voc = (uint16_t)(v);

    LoRa.beginPacket();
    LoRa.write((uint8_t*)&data, sizeof(data));
    LoRa.endPacket();
    Serial.println("Пакет LoRa відправлено.");
    
    // 4. Сон
    enterDeepSleep(sleep_interval);
}

void loop() {}