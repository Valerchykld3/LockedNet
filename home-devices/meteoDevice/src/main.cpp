#include <Arduino.h>
#include <SPI.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cScd4x.h>

#define CE_PIN 0
#define CSN_PIN 7
#define SCK_PIN 4
#define MISO_PIN 5
#define MOSI_PIN 6

#define SDA_PIN 8
#define SCL_PIN 9

#define BUTTON_PIN 10

RF24 radio(CE_PIN, CSN_PIN);
const byte addressWrite[6] = "LNet2";
const byte addressRead[6] = "LNet1";

#define MODULE_ID "mD"

Adafruit_SSD1306 display(128, 64, &Wire, -1);
SensirionI2cScd4x scd4x;

unsigned long radioCycleStart = 0;
bool isRadioListening = false;
const unsigned long RADIO_SLEEP_TIME = 4850;
const unsigned long RADIO_LISTEN_TIME = 150;

bool abpMonitoring = false;
unsigned long meteoCheckInterval = 600000;
unsigned long lastMeteoCheck = 0;
bool isDisplayActive = false;
unsigned long displayStartTime = 0;

String pendingCommand = "";

String getClimateData()
{
  scd4x.startPeriodicMeasurement();
  delay(5000);

  uint16_t co2 = 0;
  float temperature = 0.0f;
  float humidity = 0.0f;

  uint16_t error = scd4x.readMeasurement(co2, temperature, humidity);

  scd4x.stopPeriodicMeasurement();

  if (error || co2 == 0)
  {
    return "T:0 H:0 C:0";
  }

  return "T:" + String(temperature, 1) + " H:" + String(humidity, 1) + " C:" + String(co2);
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

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  Wire.begin(SDA_PIN, SCL_PIN);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println(F("OLED failed"));
  }
  else
  {
    display.clearDisplay();
    display.display();
  }

  scd4x.begin(Wire, 0x62);
  scd4x.stopPeriodicMeasurement();

  SPI.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CSN_PIN);
  if (!radio.begin())
  {
    Serial.println("[NRF] Radio failed");
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

  if (digitalRead(BUTTON_PIN) == LOW && !isDisplayActive)
  {
    isDisplayActive = true;

    display.clearDisplay();
    display.setTextSize(2);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(10, 25);
    display.println("LockedNet");
    display.display();

    String data = getClimateData();
    int tIdx = data.indexOf("T:");
    int hIdx = data.indexOf("H:");
    int cIdx = data.indexOf("C:");

    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 10);
    display.print("CO2: ");
    display.print(data.substring(cIdx + 2));
    display.println(" ppm");

    display.setCursor(0, 30);
    display.print("Temp: ");
    display.print(data.substring(tIdx + 2, hIdx - 1));
    display.println(" C");

    display.setCursor(0, 50);
    display.print("Hum: ");
    display.print(data.substring(hIdx + 2, cIdx - 1));
    display.println(" %");
    display.display();

    displayStartTime = millis();
  }

  if (isDisplayActive && (currentMillis - displayStartTime >= 15000))
  {
    display.clearDisplay();
    display.display();
    isDisplayActive = false;
  }

  if (abpMonitoring && (currentMillis - lastMeteoCheck >= meteoCheckInterval))
  {
    String data = getClimateData();
    sendPacketWithAck(String(MODULE_ID) + "|" + data + "|hub", "hub");
    lastMeteoCheck = millis();
    radioCycleStart = millis();
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

          if (pCmd == "Measure")
          {
            String data = getClimateData();
            String feedback = String(MODULE_ID) + "|" + data + "|" + pSrc;
            sendPacketWithAck(feedback, pSrc == "beacon" ? "beacon" : "hub");
          }
          else if (pCmd == "alarm")
          {
            abpMonitoring = true;
            lastMeteoCheck = millis() - meteoCheckInterval;
          }
          else if (pCmd == "sleep")
          {
            abpMonitoring = false;
          }
          else if (pCmd.startsWith("setInt:"))
          {
            int minutes = pCmd.substring(7).toInt();
            meteoCheckInterval = minutes * 60000;
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