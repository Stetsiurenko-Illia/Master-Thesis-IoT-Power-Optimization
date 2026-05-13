#include <WiFi.h>
#include <WiFiClientSecure.h> 
#include <HTTPClient.h>
#include "credentials.h" 
#include "time.h"

// Додано бібліотеки для BLE
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>

// NTP сервер і часовий пояс
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 3 * 3600;
const int daylightOffset_sec = 0; 

// HTTPS
WiFiClientSecure espClient;
HTTPClient http;

// +++ Глобальні змінні для BLE +++
BLEScan* pBLEScan;
const int SCAN_TIME = 5; // Час сканування (5 секунд)

// *** ID виробника, який ми шукаємо ***
#define TARGET_MANUFACTURER_ID 0x0118

// +++ Структура даних +++
struct __attribute__((packed)) SensorData {
    int16_t temperature; 
    uint16_t humidity;   
    uint16_t pressure;   
    uint16_t voc;        
}; 

// Глобальні змінні для зберігання даних з BLE
volatile float ble_temp = 0.0;
volatile float ble_hum = 0.0;
volatile float ble_press = 0.0;
volatile float ble_voc = 0.0; 
volatile bool newDataAvailable = false; 

// +++ Клас зворотного виклику для BLE +++
class MyAdvertisedDeviceCallbacks : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice advertisedDevice) {
        if (advertisedDevice.haveManufacturerData()) {
            
            // 1. Змінюємо std::string на String (Arduino тип)
            String mfgData = advertisedDevice.getManufacturerData();
            
            // String коректно зберігає довжину навіть з нульовими байтами
            int len = mfgData.length();

            // ДЕБАГ
            // Serial.printf("Device: %s, Len: %d\n", advertisedDevice.getAddress().toString().c_str(), len);

            if (len == 10) {
                // 2. Отримуємо вказівник на масив байтів через .c_str()
                uint8_t* dataPtr = (uint8_t*)mfgData.c_str();

                // Перевірка ID (Little Endian)
                uint16_t mfgId = (uint16_t)((dataPtr[1] << 8) | dataPtr[0]);

                if (mfgId == TARGET_MANUFACTURER_ID) {
                    Serial.print("!!! ЗНАЙДЕНО СЕНСОР !!! Адреса: ");
                    Serial.println(advertisedDevice.getAddress().toString().c_str());

                    SensorData data;
                    // Копіюємо дані
                    memcpy(&data, &dataPtr[2], sizeof(data));

                    ble_temp = (float)data.temperature / 100.0;
                    ble_hum = (float)data.humidity / 100.0;
                    ble_press = (float)data.pressure; 
                    ble_voc = (float)data.voc / 10.0;   

                    Serial.printf("Отримано дані: T=%.2f, H=%.2f, P=%.0f, VOC=%.1f\n",
                                  ble_temp, ble_hum, ble_press, ble_voc);
                    
                    newDataAvailable = true; 
                    pBLEScan->stop(); 
                }
            }
        }
    }
};

// +++ ФУНКЦІЯ setup() +++
void setup() {
    Serial.begin(115200);
    delay(100);

    Serial.println("Запуск BLE-HTTP Шлюзу v1.0");
    Serial.println("Ініціалізація BLE буде в циклі loop().");

    // Підключення до Wi-Fi
    connectWiFi();
    
    // Налаштування часу
    Serial.println("Налаштування часу (NTP)...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer); 
    
    // +++ ЧЕКАЄМО НА СИНХРОНІЗАЦІЮ ЧАСУ +++
    time_t now;
    time(&now);
    while (now < 1704067200) { // 1 січня 2024
        delay(500);
        Serial.print(".");
        time(&now);
    }
    Serial.println("\nЧас успішно синхронізовано!");
}

// +++ ФІНАЛЬНА ФУНКЦІЯ loop() З ОПТИМІЗАЦІЄЮ +++
void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("Спроба перепідключення до Wi-Fi..."));
        connectWiFi();
        return; 
    }

    // +++ ПРИМУСОВА ІНІЦІАЛІЗАЦІЯ BLE +++
    Serial.println("Ініціалізація BLE для сканування..."); // Можна закоментувати
    BLEDevice::init("ESP32_Gateway");
    pBLEScan = BLEDevice::getScan(); 
    pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
    pBLEScan->setActiveScan(true); 
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    // +++++++++++++++++++++++++++++++++++++

    // Скидаємо прапор перед новим скануванням
    newDataAvailable = false; 

    Serial.println("Початок BLE сканування..."); // Можна закоментувати
    pBLEScan->start(SCAN_TIME, false); 
    
    if (newDataAvailable) {
        Serial.println("Є нові дані. Зупинка BLE для звільнення радіо...");
        
        // +++ ПРИМУСОВА ДЕІНІЦІАЛІЗАЦІЯ BLE (КЛЮЧОВЕ ВИПРАВЛЕННЯ) +++
        BLEDevice::deinit();
        delay(100); // Даємо час на звільнення ресурсів
        // +++++++++++++++++++++++++++++++++++++++

        Serial.println("Спроба відправки на AWS...");
        
        if (transmitData(ble_temp, ble_hum, ble_press, ble_voc)) {
            Serial.println("Відправлено успішно.");
        } else {
            Serial.println("Помилка відправки.");
        }
    } else {
        // Serial.println("Нових даних з сенсора не знайдено."); // Можна закоментувати
        BLEDevice::deinit(); 
    }

    // +++ КЛЮЧОВА ЗМІНА ЛОГІКИ +++
    // Завжди робимо коротку паузу, щоб постійно бути в циклі сканування.
    // Прибираємо довгий delay(30000).
    delay(1000); // Коротка пауза 1 сек перед наступним скануванням
}

// Функція підключення Wi-Fi (без змін)
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Підключення до Wi-Fi");
    unsigned long start_time = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_time < 10000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("\nПомилка підключення Wi-Fi!"));
        while (1); 
    }
    Serial.println(F("\nWi-Fi підключено"));
}


// +++ ФІНАЛЬНА ФУНКЦІЯ transmitData() (ЧИСТА) +++
bool transmitData(float temp, float hum, float press, float voc) {

    time_t now;
    time(&now);

    Serial.printf("Дані для відправки: T: %.2f, H: %.2f, P: %.0f, VOC: %.1f, Time: %ld\n", 
                  temp, hum, press, voc, now);

    // Вмикаємо коректну перевірку сертифіката
    espClient.setCACert(aws_root_ca); 
    
    http.begin(espClient, http_endpoint); 
    http.addHeader("Content-Type", "application/json");
    http.addHeader("User-Agent", "curl/7.64.1"); // Залишаємо
    http.setTimeout(15000); // 15 секунд (про всяк випадок для холодного старту)
    http.setReuse(false);

    String payload = "{\"temperature\":" + String(temp, 2) +
                     ",\"humidity\":" + String(hum, 2) +
                     ",\"pressure\":" + String(press, 0) +
                     ",\"voc\":" + String(voc, 1) +
                     ",\"timestamp\":" + String(now) + "}";

    Serial.println("Payload: " + payload); // Можна закоментувати

    // Відправка POST
    int httpCode = http.POST(payload);
    Serial.printf("HTTP %d: %s\n", httpCode, http.errorToString(httpCode).c_str());

    if (httpCode == 200) {
        Serial.println(F("Відповідь HTTP 200 OK"));
        Serial.println(http.getString()); // Розкоментуйте, якщо хочете бачити відповідь
        http.end();
        return true;
    } else {
        Serial.println(F("Помилка HTTP!"));
        Serial.println(http.getString());
        http.end();
        return false;
    }
}