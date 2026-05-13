#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>         
#include <WiFiClientSecure.h> // <-- ДОДАНО ДЛЯ HTTPS
#include <HTTPClient.h>   
#include <ArduinoJson.h> 
#include "credentials.h"   // Потрібні ssid, password, http_endpoint, aws_root_ca
#include "time.h"          // <-- ДОДАНО ДЛЯ time()

// --- НАЛАШТУВАННЯ ПІНІВ (Як у відправника) ---
const int LORA_CS_PIN = 5;    // G5 (NSS/CS)
const int LORA_RST_PIN = 2;   // G2 (Reset)
const int LORA_DIO0_PIN = 15; // G15 (DIO0)

// --- HTTPS ---
WiFiClientSecure client; // <-- ЗМІНЕНО: Використовуємо Secure client
HTTPClient http;

// Структура для пакування даних (має бути ТАКА САМА, як у відправника)
struct __attribute__((packed)) SensorData {
    int16_t temperature; 
    uint16_t humidity;   
    uint16_t pressure;   
    uint16_t voc;        
};
SensorData receivedData;

void setup() {
    Serial.begin(115200);
    while (!Serial);
    delay(1000);
    Serial.println("--- LoRa AWS Gateway (Receiver) ---");

    // Запускаємо Wi-Fi
    setup_wifi();
    
    // Синхронізуємо час
    configTime(ntp_offset, 0, ntp_server); // (ntp_offset, ntp_server з credentials.h)
    time_t now = time(nullptr);
    while (now < 1704067200) { // Чекаємо на коректний час
        delay(100);
        now = time(nullptr);
    }
    Serial.println("Час синхронізовано.");

    // Запускаємо LoRa
    LoRa.setPins(LORA_CS_PIN, LORA_RST_PIN, LORA_DIO0_PIN);
    if (!LoRa.begin(868E6)) { // 868 МГц для Європи
        Serial.println("Помилка запуску LoRa!");
        while (1);
    }
    
    // Вмикаємо режим постійного прослуховування
    LoRa.receive(); 
    Serial.println("Готовий приймати LoRa пакети...");
}

void setup_wifi() {
    delay(10);
    Serial.print("Підключення до Wi-Fi: ");
    Serial.println(ssid);
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");
    Serial.println("Wi-Fi підключено.");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
}

void loop() {
    // Намагаємося отримати пакет
    int packetSize = LoRa.parsePacket();
    
    // Перевіряємо, чи розмір пакета = розміру нашої структури
    if (packetSize == sizeof(SensorData)) {
        Serial.print("Отримано LoRa пакет... ");
        
        int bytesRead = 0;
        uint8_t* buffer = (uint8_t*)&receivedData;
        while (LoRa.available()) {
            buffer[bytesRead] = (uint8_t)LoRa.read();
            bytesRead++;
        }

        // Друкуємо дані
        float temp = (float)receivedData.temperature / 100.0;
        float hum = (float)receivedData.humidity / 100.0;
        int rssi = LoRa.packetRssi();

        Serial.printf("T: %.2f C, H: %.2f %%, P: %d hPa, RSSI: %d\n", 
                        temp, hum, receivedData.pressure, rssi);
        
        // Надсилаємо дані на Lambda
        sendToLambda(temp, hum, receivedData.pressure, (float)receivedData.voc / 10.0, rssi);
    }
}

// Функція надсилання даних на AWS Lambda
void sendToLambda(float temp, float hum, int pressure, float voc, int rssi) {
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("Wi-Fi не підключено. Спроба перепідключення...");
        setup_wifi();
    }

    StaticJsonDocument<256> doc;
    doc["device_id"] = "esp32_lora_node_1"; // ID нашого сенсора
    doc["temperature"] = temp;
    doc["humidity"] = hum;
    doc["pressure"] = pressure;
    doc["voc"] = voc;
    doc["rssi"] = rssi;
    doc["timestamp"] = time(nullptr); // Додаємо мітку часу

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    // --- ВИПРАВЛЕНО: Налаштування для HTTPS ---
    // Встановлюємо сертифікат CA (з credentials.h)
    client.setCACert(aws_root_ca);
    
    // Використовуємо 'client' (WiFiClientSecure) замість 'http.begin(http_endpoint)'
    if (http.begin(client, http_endpoint)) { 
        http.addHeader("Content-Type", "application/json");

        Serial.print("Надсилання JSON на Lambda: ");
        Serial.println(jsonPayload);

        int httpResponseCode = http.POST(jsonPayload);

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.print("HTTP Response code: ");
            Serial.println(httpResponseCode);
            Serial.print("Response body: ");
            Serial.println(response);
        } else {
            Serial.print("Помилка POST-запиту, код: ");
            Serial.println(httpResponseCode);
            Serial.printf("Помилка: %s\n", http.errorToString(httpResponseCode).c_str());
        }
        http.end();
    } else {
      Serial.println("Помилка http.begin()! Невірний URL?");
    }
    // ------------------------------------
}