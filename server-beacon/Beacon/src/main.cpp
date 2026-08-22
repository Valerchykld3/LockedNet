#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <SPI.h>
#include <RF24.h>

#define CE_PIN 0
#define CSN_PIN 7
#define SCK_PIN 4
#define MISO_PIN 5
#define MOSI_PIN 6

RF24 radio(CE_PIN, CSN_PIN);

#define BEACON_ID "beacon"

const byte addressWrite[6] = "LNet2";
const byte addressRead[6] = "LNet1";

AsyncWebServer server(80);

String latestFeedback = "";
String latestLogs = "";
String pendingCommand = "";

#define BATTERY_PIN A1

float getBatteryCharge()
{
    int rawADC = analogRead(BATTERY_PIN);
    float pinVoltage = (rawADC / 4095.0) * 3.3;
    float batteryVoltage = pinVoltage * 2.0;
    float charge = ((batteryVoltage - 3.0) / 1.2) * 100.0;

    if (charge > 100.0)
        charge = 100.0;
    if (charge < 0.0)
        charge = 0.0;

    return charge;
}

bool sendPacketWithAck(String packet, String targetModuleId)
{
    unsigned long shoutStart = millis();
    String expectedAckCmd = "ACK_" + targetModuleId;

    while (millis() - shoutStart < 10000)
    {
        radio.powerUp();
        radio.stopListening();
        radio.write(packet.c_str(), packet.length() + 1);

        radio.startListening();
        unsigned long listenStart = millis();
        while (millis() - listenStart < 50)
        {
            if (radio.available())
            {
                char ackText[32] = "";
                radio.read(&ackText, sizeof(ackText));
                String ackPacket = String(ackText);

                if (ackPacket.indexOf(expectedAckCmd) != -1)
                {
                    Serial.println("   -> [ACK] Підтвердження отримано!");
                    radio.powerDown();
                    isRadioListening = false;
                    radioCycleStart = millis();
                    return true;
                }
            }
        }
    }

    Serial.println("   -> [ACK] ПОМИЛКА: Девайс не відповів.");
    radio.powerDown();
    isRadioListening = false;
    radioCycleStart = millis();
    return false;
}

void setup()
{
    Serial.begin(115200);
    delay(2000);

    Serial.println("\n=== LockedNet: Help Beacon (v0.2) ===");

    if (!LittleFS.begin())
    {
        Serial.println("[FS] ПОМИЛКА: Файлову систему не знайдено!");
        Serial.println("Переконайтеся, що ви завантажили 'Upload Filesystem Image'");
        return;
    }
    Serial.println("[FS] LittleFS успішно змонтовано.");

    SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);
    if (!radio.begin())
    {
        Serial.println("[NRF] ПОМИЛКА: Модуль NRF24L01 не знайдено!");
    }
    else
    {
        radio.setAutoAck(false);
        radio.setChannel(100);
        radio.setPALevel(RF24_PA_LOW);
        radio.openWritingPipe(addressWrite);
        radio.openReadingPipe(1, addressRead);
        radio.startListening();
        Serial.println("[NRF] Модуль ініціалізовано на 100 каналі.");
    }

    WiFi.softAP("LockedNet_Beacon", "12345678");
    Serial.print("[WiFi] Beacon Wi-Fi піднято! IP адреса брєлка: ");
    Serial.println(WiFi.softAPIP());

    server.serveStatic("/", LittleFS, "/").setDefaultFile("login.html");
    server.on("/login.html", HTTP_POST, [](AsyncWebServerRequest *request)
              {
        if (request->hasParam("username", true) && request->hasParam("password", true)) {
            String user = request->getParam("username", true)->value();
            String pass = request->getParam("password", true)->value();
            
            if (user == "u1" && pass == "1114") {
                request->redirect("/menu.html");
            } else {
                request->redirect("/login.html");
            }
        } else {
            request->redirect("/login.html");
        } });

    server.on("/devices.html", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  if (request->hasParam("device_action", true))
                  {
                      // ПРОСТО ЗБЕРІГАЄМО КОМАНДУ, А НЕ ВІДПРАВЛЯЄМО ОДРАЗУ
                      pendingCommand = request->getParam("device_action", true)->value();
                  }
                  request->send(LittleFS, "/devices.html", "text/html"); });

    server.on("/hub.html", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  if (request->hasParam("hub_action", true))
                  {
                      pendingCommand = request->getParam("hub_action", true)->value();
                  }
                  request->send(LittleFS, "/hub.html", "text/html"); });

    server.on("/abprotocol.html", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  String payload = "";
                  if (request->hasParam("action")) {
                      String action = request->getParam("action")->value();
                      if (action == "set_ups_interval" && request->hasParam("ups_interval", true)) {
                          payload = "1|abp_set_ups:" + request->getParam("ups_interval", true)->value() + "|beacon";
                      } else if (action == "set_meteo_interval" && request->hasParam("meteo_interval", true)) {
                          payload = "1|abp_set_meteo:" + request->getParam("meteo_interval", true)->value() + "|beacon";
                      } else if (action == "set_min_temp" && request->hasParam("min_temp", true)) {
                          payload = "1|abp_set_temp:" + request->getParam("min_temp", true)->value() + "|beacon";
                      } else if (action == "set_max_hum" && request->hasParam("max_hum", true)) {
                          payload = "1|abp_set_hum:" + request->getParam("max_hum", true)->value() + "|beacon";
                      } else if (action == "set_max_co2" && request->hasParam("max_co2", true)) {
                          payload = "1|abp_set_co2:" + request->getParam("max_co2", true)->value() + "|beacon";
                      }
                  } 
                  else if (request->hasParam("ab_action", true))
                  {
                      payload = request->getParam("ab_action", true)->value();
                  }

                  if (payload != "") {
                      pendingCommand = payload; // ЗБЕРІГАЄМО КОМАНДУ
                  }
                  request->send(LittleFS, "/abprotocol.html", "text/html"); });

    server.on("/log.html", HTTP_POST, [](AsyncWebServerRequest *request)
              {
                  if (request->hasParam("log_action", true))
                  {
                      pendingCommand = request->getParam("log_action", true)->value();
                  }
                  request->send(LittleFS, "/log.html", "text/html"); });

    server.on("/api/feedback", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                  request->send(200, "text/plain", latestFeedback);
                  latestFeedback = ""; });

    server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                  request->send(200, "text/plain", latestLogs);
                  latestLogs = ""; });

    server.on("/api/charge", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                  String charge = String(getBatteryCharge(), 1);
                  request->send(200, "text/plain", charge); });

    server.begin();
    Serial.println("[WEB] Асинхронний сервер успішно запущено.");
}

void loop()
{
    if (pendingCommand != "")
    {
        Serial.print("[WEB] Отримана команда з інтерфейсу. Відправляю: ");
        Serial.println(pendingCommand);

        sendPacketWithAck(pendingCommand, "1");
        pendingCommand = "";
    }

    String incoming = "";
    if (latestFeedback.length() > 0)
    {
        incoming = latestFeedback;
        latestFeedback = "";
    }
    else if (radio.available())
    {
        char text[32] = "";
        radio.read(&text, sizeof(text));
        incoming = String(text);
    }

    if (incoming.length() > 0)
    {
        Serial.println(incoming);

        int fPipe = incoming.indexOf('|');
        int sPipe = incoming.indexOf('|', fPipe + 1);

        if (fPipe != -1 && sPipe != -1)
        {
            String fbCmd = incoming.substring(fPipe + 1, sPipe);
            String fbSrc = incoming.substring(sPipe + 1);

            if (fbSrc == BEACON_ID)
            {
                if (fbCmd.startsWith("LOG:"))
                {
                    latestLogs += fbCmd.substring(4) + "<br>";
                }
                else if (fbCmd.startsWith("ABP_SETTINGS:"))
                {
                    latestFeedback = fbCmd;
                }
                else if (latestFeedback == "")
                {
                    latestFeedback = fbCmd;
                }
            }
            else if (fbCmd.startsWith("ACK_"))
            {
            }
        }
        else
        {
            latestLogs += incoming + "<br>";
        }
    }
    delay(10);
}