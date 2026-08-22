#include <Arduino.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>

#define CE_PIN 0
#define CSN_PIN 7
#define SCK_PIN 4
#define MISO_PIN 5
#define MOSI_PIN 6

#define ZMPT_VCC_PIN 10
#define ZMPT_OUT_PIN 1

RF24 radio(CE_PIN, CSN_PIN);
const byte addressWrite[6] = "LNet2";
const byte addressRead[6] = "LNet1";

#define MODULE_ID "Ud"
#define HUB_ID "hub"

unsigned long radioCycleStart = 0;
bool isRadioListening = false;
const unsigned long RADIO_SLEEP_TIME = 4850;
const unsigned long RADIO_LISTEN_TIME = 150;

bool gridMonitoring = false;
bool isGridDown = false;
unsigned long lastGridCheck = 0;

unsigned long upsCheckInterval = 5000;

bool getGridStatus()
{
  digitalWrite(ZMPT_VCC_PIN, HIGH);
  delay(15);

  int max_v = 0;
  int min_v = 4095;
  unsigned long start = millis();

  while (millis() - start < 25)
  {
    int val = analogRead(ZMPT_OUT_PIN);
    if (val > max_v)
      max_v = val;
    if (val < min_v)
      min_v = val;
  }

  digitalWrite(ZMPT_VCC_PIN, LOW);

  return (max_v - min_v) > 200;
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

  pinMode(ZMPT_VCC_PIN, OUTPUT);
  digitalWrite(ZMPT_VCC_PIN, LOW);

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);
  if (!radio.begin())
  {
    Serial.println("[NRF] Помилка: модуль SI24R1 не знайдено!");
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
  }
}

void loop()
{
  unsigned long currentMillis = millis();

  if (gridMonitoring && (currentMillis - lastGridCheck >= upsCheckInterval))
  {
    bool gridAlive = getGridStatus();

    if (!gridAlive && !isGridDown)
    {
      isGridDown = true;
      sendPacketWithAck(String(MODULE_ID) + "|alarm|" + String(HUB_ID), HUB_ID);
    }
    else if (gridAlive && isGridDown)
    {
      isGridDown = false;
      sendPacketWithAck(String(MODULE_ID) + "|stop|" + String(HUB_ID), HUB_ID);
    }
    lastGridCheck = currentMillis;
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
      char text[32] = "";
      radio.read(&text, sizeof(text));
      String incoming = String(text);

      int fPipe = incoming.indexOf('|');
      int sPipe = incoming.indexOf('|', fPipe + 1);

      if (fPipe != -1 && sPipe != -1)
      {
        String pId = incoming.substring(0, fPipe);
        String pCmd = incoming.substring(fPipe + 1, sPipe);
        String pSrc = incoming.substring(sPipe + 1);

        if (pId == MODULE_ID)
        {
          String ackMsg = String(MODULE_ID) + "|ACK_" + String(MODULE_ID) + "|" + pSrc;
          radio.stopListening();
          for (int i = 0; i < 5; i++)
          {
            radio.write(ackMsg.c_str(), ackMsg.length() + 1);
            delay(10);
          }

          if (pCmd == "Voltage")
          {
            String result = getGridStatus() ? "220V" : "0V";
            String feedback = String(MODULE_ID) + "|" + result + "|" + pSrc;
            sendPacketWithAck(feedback, pSrc == "beacon" ? "beacon" : HUB_ID);
          }
          else if (pCmd == "alarm")
          {
            gridMonitoring = true;
          }
          else if (pCmd == "sleep")
          {
            gridMonitoring = false;
          }
          else if (pCmd.startsWith("setInt:"))
          {
            int minutes = pCmd.substring(7).toInt();
            upsCheckInterval = minutes * 60000;
            Serial.println("[SET] Новий інтервал моніторингу: " + String(minutes) + " хв");
          }
        }
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