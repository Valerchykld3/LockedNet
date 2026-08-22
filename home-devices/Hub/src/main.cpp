#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>

#define NRF_CE_PIN 4
#define NRF_CS_PIN 5

#define SD_CS_PIN 15

#define BUZZER_PIN 32
#define BUTTON_PIN 33

#define BATTERY_CHARGE_PIN 35

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define HUB_ID "hub"

RTC_DS3231 rtc;

unsigned long bootTime = 0;
int oledState = 0;

bool lastButtonState = HIGH;
unsigned long lastPingTime = 0;

unsigned long radioCycleStart = 0;
bool isRadioListening = false;
const unsigned long RADIO_SLEEP_TIME = 4850;
const unsigned long RADIO_LISTEN_TIME = 150;

bool isAbpActive = false;
bool isBlackoutCondition = false;

unsigned long abpUpsCheckInterval = 5 * 60000;
unsigned long abpMeteoCheckInterval = 10 * 60000;
float abpMinTemp = 15.0;
float abpMaxHumidity = 60.0;
int abpMaxCO2 = 1000;

unsigned long lastAbpMeteoCheck = 0;

float getBatteryCharge()
{
  int rawADC = analogRead(BATTERY_CHARGE_PIN);
  float pinVoltage = (rawADC / 4095.0) * 3.3;
  float batteryVoltage = pinVoltage * 2.0;
  float charge = ((batteryVoltage - 3.0) / 1.2) * 100.0;
  if (charge > 100.0)
    charge = 100.0;
  if (charge < 0.0)
    charge = 0.0;
  return charge;
}

RF24 radio(NRF_CE_PIN, NRF_CS_PIN);

const byte addressWrite[6] = "LNet1";
const byte addressRead[6] = "LNet2";

bool isPcConnected()
{
  return (millis() - lastPingTime < 15000);
}

void writeLogToSD(String logText)
{
  DateTime now = rtc.now();

  char fileName[16];
  sprintf(fileName, "/%04d%02d%02d.txt", now.year(), now.month(), now.day());

  File file = SD.open(fileName, FILE_APPEND);
  if (file)
  {
    char timeBuf[12];
    sprintf(timeBuf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
    file.print("[");
    file.print(timeBuf);
    file.print("] ");
    file.println(logText);
    file.close();
  }
}

void syncAndCleanSD()
{
  File root = SD.open("/");
  if (!root)
    return;

  DateTime now = rtc.now();
  DateTime yesterday = now - TimeSpan(1, 0, 0, 0);

  long yesterdayInt = yesterday.year() * 10000 + yesterday.month() * 100 + yesterday.day();

  Serial.println("[SD SYNC] Перевірка і синхронізація старих логів...");

  while (true)
  {
    File entry = root.openNextFile();
    if (!entry)
      break;

    if (!entry.isDirectory())
    {
      String fName = entry.name();
      if (fName.startsWith("/"))
        fName = fName.substring(1);

      if (fName.length() == 12 && fName.endsWith(".txt"))
      {
        long fileDate = fName.substring(0, 8).toInt();
        if (fileDate > 0 && fileDate < yesterdayInt)
        {
          Serial.println("   -> [SD SYNC] Знайдено старий лог: " + fName + ". Перекидаю на ПК...");
          while (entry.available())
          {
            String line = entry.readStringUntil('\n');
            line.trim();
            if (line.length() > 0)
            {
              Serial.println("SYNC_LOG|" + fName + "|" + line);
            }
          }
          entry.close();
          SD.remove("/" + fName);
          Serial.println("   -> [SD SYNC] Файл " + fName + " успішно перенесено і видалено з SD.");
          continue;
        }
      }
    }
    entry.close();
  }
  Serial.println("[SD SYNC] Очищення завершено.");
}

void handleHubCommand(String cmd, String src)
{
  if (cmd == "PING")
  {
    lastPingTime = millis();
    return;
  }

  String fbText = "Hub OK";

  if (cmd == "SYNC")
  {
    Serial.println("   -> [LOCAL] Виконую: Синхронізація логів");
    syncAndCleanSD();
    fbText = "SD Synced & Cleaned";
  }
  else
  {
    writeLogToSD("[HUB EXEC] Command: " + cmd + " | Source: " + src);

    if (cmd == "Buzz")
    {
      tone(BUZZER_PIN, 2000, 3000);
      fbText = "Buzz ON";
    }
    else if (cmd == "Charge")
    {
      fbText = "Bat: " + String(getBatteryCharge(), 1) + "%";
    }
    else if (cmd == "Time")
    {
      DateTime now = rtc.now();
      char tBuf[10];
      sprintf(tBuf, "%02d:%02d", now.hour(), now.minute());
      fbText = "Time: " + String(tBuf);
    }
    else if (cmd == "Connecting")
    {
      fbText = isPcConnected() ? "PC: Connected" : "PC: Not Found";
    }
    else if (cmd == "OLED")
    {
      display.clearDisplay();
      display.setTextSize(2);
      display.setCursor(30, 0);
      DateTime now = rtc.now();
      display.printf("%02d:%02d", now.hour(), now.minute());
      display.setTextSize(1);
      display.setCursor(0, 30);
      display.printf("Battery: %.1f %%", getBatteryCharge());
      display.setCursor(0, 45);
      display.printf("PC Port: %s", isPcConnected() ? "Connected" : "Not Found");
      display.display();
      oledState = 1;
      bootTime = millis() - 5000;
      fbText = "OLED is ON";
    }
    else if (cmd.startsWith("abp_set_ups:"))
    {
      int minutes = cmd.substring(12).toInt();
      abpUpsCheckInterval = minutes * 60000;
      fbText = "UPS interval: " + String(minutes) + " min";
      sendPacketWithAck("Ud|setInt:" + String(minutes) + "|hub", "Ud");
    }
    else if (cmd.startsWith("abp_set_meteo:"))
    {
      int minutes = cmd.substring(14).toInt();
      abpMeteoCheckInterval = minutes * 60000;
      fbText = "Meteo interval: " + String(minutes) + " min";
      sendPacketWithAck("mD|setInt:" + String(minutes) + "|hub", "mD");
    }
    else if (cmd.startsWith("abp_set_temp:"))
    {
      abpMinTemp = cmd.substring(13).toFloat();
      fbText = "Min temp set: " + String(abpMinTemp, 1) + "C";
    }
    else if (cmd.startsWith("abp_set_hum:"))
    {
      abpMaxHumidity = cmd.substring(12).toFloat();
      fbText = "Max hum set: " + String(abpMaxHumidity, 1) + "%";
    }
    else if (cmd.startsWith("abp_set_co2:"))
    {
      abpMaxCO2 = cmd.substring(12).toInt();
      fbText = "Max CO2 set: " + String(abpMaxCO2) + "ppm";
    }
    else if (cmd == "abp_get_settings")
    {
      fbText = "ABP_SETTINGS:" + String(isAbpActive ? "1" : "0") +
               "|" + String(abpUpsCheckInterval / 60000) +
               "|" + String(abpMeteoCheckInterval / 60000) +
               "|" + String(abpMinTemp, 1) +
               "|" + String(abpMaxHumidity, 1) +
               "|" + String(abpMaxCO2);
      Serial.println("   -> [LOCAL] Віддаю налаштування ABP");
    }
  }

  String fbPacket = String(HUB_ID) + "|" + fbText + "|" + src;

  if (src == "beacon")
  {
    sendPacketWithAck(fbPacket, "beacon");
  }
  else if (src == "wifi")
  {
    Serial.println("   -> [ROUTE] Відправлено фідбек на USB: " + fbPacket);
    Serial.println(fbPacket);
  }
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

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  tone(BUZZER_PIN, 2000, 100);

  delay(500);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println("[ESP32] ПОМИЛКА: OLED-екран не знайдено!");
  }
  else
  {
    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 25);
    display.println("LockedNet");
    display.display();
  }

  if (rtc.begin())
  {
    Serial.println("[ESP32] DS3231 успішно ініціалізовано.");
    if (rtc.lostPower())
    {
      Serial.println("[ESP32] RTC втратив живлення, встановлюємо час компіляції!");
      rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }
  }
  else
  {
    Serial.println("[ESP32] ПОМИЛКА: DS3231 не знайдено!");
  }

  pinMode(NRF_CS_PIN, OUTPUT);
  digitalWrite(NRF_CS_PIN, HIGH);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  if (!SD.begin(SD_CS_PIN))
  {
    Serial.println("\n[ESP32] ПОМИЛКА: SD-карту не знайдено!");
  }
  else
  {
    Serial.println("\n[ESP32] SD-карта успішно ініціалізована.");
  }

  if (!radio.begin())

  {

    Serial.println("\n[ESP32] ПОМИЛКА: Модуль NRF24L01 не підключено або пошкоджено!");
  }

  else
  {
    radio.setAutoAck(false);
    radio.setChannel(100);
    radio.setPALevel(RF24_PA_LOW);
    radio.openWritingPipe(addressWrite);
    radio.openReadingPipe(1, addressRead);

    radio.powerDown();
    isRadioListening = false;
    radioCycleStart = millis();

    Serial.println("\n[ESP32] NRF24L01 ініціалізовано на 100-му каналі (Фоновий режим).");
  }

  Serial.println("\n[ESP32] Hub is ready and waiting for commands...");

  bootTime = millis();
  oledState = 0;

  writeLogToSD("=== HUB SYSTEM BOOT ===");
}

void loop()
{
  unsigned long currentMillis = millis();

  bool reading = digitalRead(BUTTON_PIN);
  if (reading == LOW && lastButtonState == HIGH)
  {
    delay(50);
    if (digitalRead(BUTTON_PIN) == LOW)
    {
      Serial.println("\n[HUB] Натиснуто фізичну кнопку!");
      handleHubCommand("OLED", "local");
    }
  }
  lastButtonState = reading;

  if (oledState == 0 && currentMillis - bootTime >= 5000)
  {
    display.clearDisplay();
    display.setTextSize(2);
    display.setCursor(30, 0);
    DateTime now = rtc.now();
    display.printf("%02d:%02d", now.hour(), now.minute());
    display.setTextSize(1);
    display.setCursor(0, 30);
    display.printf("Battery: %.1f %%", getBatteryCharge());
    display.setCursor(0, 45);
    display.printf("PC Port: %s", isPcConnected() ? "Connected" : "Not Found");
    display.display();
    oledState = 1;
  }
  else if (oledState == 1 && currentMillis - bootTime >= 25000)
  {
    display.clearDisplay();
    display.display();
    oledState = 2;
  }

  if (Serial.available() > 0)
  {
    String incomingData = Serial.readStringUntil('\n');
    incomingData.trim();

    if (incomingData.length() > 0)
    {
      if (incomingData != "1|PING|wifi")
      {
        Serial.print("[ESP32] Отримано пакет даних: ");
        Serial.println(incomingData);
        writeLogToSD("[USB IN] " + incomingData);
      }

      int firstPipe = incomingData.indexOf('|');
      int secondPipe = incomingData.indexOf('|', firstPipe + 1);

      if (firstPipe != -1 && secondPipe != -1)
      {
        String moduleId = incomingData.substring(0, firstPipe);
        String command = incomingData.substring(firstPipe + 1, secondPipe);
        String src = incomingData.substring(secondPipe + 1);

        if (moduleId == "Ud" && command == "alarm")
          isAbpActive = true;
        if (moduleId == "Ud" && command == "sleep")
          isAbpActive = false;

        if (src == "beacon")
        {
          Serial.println("   -> [ROUTE] Відправляю пакет від ПК на Брєлок");
          sendPacketWithAck(incomingData, "beacon");
        }
        else if (moduleId == HUB_ID || moduleId == "1")
        {
          handleHubCommand(command, src);
        }
        else
        {
          bool isSuccess = sendPacketWithAck(incomingData, moduleId);
          if (isSuccess)
          {
            Serial.println("   -> [RADIO] Пакет успішно доставлено в ефір!");
          }
          else
          {
            Serial.println("   -> [RADIO] ПОМИЛКА: Не вдалося передати пакет.");
          }
        }
      }
      else
      {
        Serial.println("[ESP32] Помилка: Отримано пакет невідомого формату!");
      }
    }
  }

  if (!isRadioListening)
  {
    if (currentMillis - radioCycleStart >= RADIO_SLEEP_TIME)
    {
      radio.powerUp();
      radio.startListening();
      isRadioListening = true;
      radioCycleStart = currentMillis;
    }
  }
  else
  {
    if (radio.available())
    {
      char receivedText[32] = "";
      radio.read(&receivedText, sizeof(receivedText));

      String packet = String(receivedText);
      writeLogToSD("[NRF IN] " + packet);

      int fPipe = packet.indexOf('|');
      int sPipe = packet.indexOf('|', fPipe + 1);

      if (fPipe != -1 && sPipe != -1)
      {
        String pId = packet.substring(0, fPipe);
        String pCmd = packet.substring(fPipe + 1, sPipe);
        String pSrc = packet.substring(sPipe + 1);

        if (pId == "Ud" && pCmd == "alarm")
          isAbpActive = true;
        if (pId == "Ud" && pCmd == "sleep")
          isAbpActive = false;

        Serial.println(packet);

        Serial.print("[RADIO ПРИЙОМ] Від: ");
        Serial.print(pId);
        Serial.print(" | Текст: ");
        Serial.print(pCmd);
        Serial.print(" | Шлях: ");
        Serial.println(pSrc);

        if (pId == HUB_ID || pId == "1")
        {
          if (pCmd == "alarm" && pSrc == "Ud")
          {
            if (isAbpActive)
            {
              isBlackoutCondition = true;
              Serial.println("[ABP] БЛЕКАУТ! Відправляю alarm на meteoDevice.");
              writeLogToSD("[ABP] БЛЕКАУТ! Відправляю alarm на meteoDevice.");
              sendPacketWithAck("mD|alarm|hub", "mD");
            }
          }
          else if (pCmd == "stop" && pSrc == "Ud")
          {
            isBlackoutCondition = false;
            Serial.println("[ABP] Світло відновлено! Відбій тривоги.");
            writeLogToSD("[ABP] Світло відновлено! Відбій тривоги.");
            sendPacketWithAck("mD|sleep|hub", "mD");
            sendPacketWithAck("eH|sleep|hub", "eH");
          }
          else if (pSrc == "mD" && isBlackoutCondition)
          {
            writeLogToSD("[ABP] Клімат: " + pCmd);

            int tempIndex = pCmd.indexOf("T:");
            int humIndex = pCmd.indexOf("H:");
            int co2Index = pCmd.indexOf("C:");

            bool actionTaken = false;

            if (tempIndex != -1 && pCmd.substring(tempIndex + 2).toFloat() < abpMinTemp)
            {
              writeLogToSD("[ABP] Temp low. Вмикаю обігрів.");
              sendPacketWithAck("eH|Warm|hub", "eH");
              actionTaken = true;
            }
            if (humIndex != -1 && pCmd.substring(humIndex + 2).toFloat() > abpMaxHumidity)
            {
              writeLogToSD("[ABP] Hum high. Вмикаю вентиляцію.");
              sendPacketWithAck("eH|Fresh|hub", "eH");
              actionTaken = true;
            }
            if (co2Index != -1 && pCmd.substring(co2Index + 2).toInt() > abpMaxCO2)
            {
              writeLogToSD("[ABP] CO2 high. Вмикаю провітрювання.");
              sendPacketWithAck("eH|Fresh|hub", "eH");
              actionTaken = true;
            }

            if (!actionTaken)
            {
              writeLogToSD("[ABP] Клімат в нормі. Режим очікування.");
              sendPacketWithAck("eH|sleep|hub", "eH");
            }

            Serial.println("   -> [ROUTE] Транслюю дані мікроклімату на ПК та Брєлок");

            Serial.println(packet);

            sendPacketWithAck(packet, "beacon");
          }

          else if (pCmd == "log")
          {
            Serial.println("   -> [ROUTE] Запит логів перенаправлено на ПК");
          }
          else if (pCmd == "Buzz" || pCmd == "Charge" || pCmd == "Time" || pCmd == "OLED" || pCmd == "Connecting" || pCmd.startsWith("abp_"))
          {
            handleHubCommand(pCmd, pSrc);
          }
          else if (!pCmd.startsWith("ACK_"))
          {
            if (pSrc == "wifi")
              Serial.println("   -> [ROUTE] Фідбек для USB (Python Server)");
            else if (pSrc == "beacon")
            {
              Serial.println("   -> [ROUTE] Перенаправляю фідбек на Help-брєлок!");
              sendPacketWithAck(packet, "beacon");
            }
          }
        }
        else
        {
          Serial.println("   -> [ROUTE] Перенаправляю команду на Девайс!");
          sendPacketWithAck(packet, pId);
        }
      }
      else
      {
        Serial.println("[RADIO ПРИЙОМ] Невідомий формат.");
      }
    }

    if (currentMillis - radioCycleStart >= RADIO_LISTEN_TIME)
    {
      radio.stopListening();
      radio.powerDown();
      isRadioListening = false;
      radioCycleStart = currentMillis;
    }
  }
}