/*
 * Maker      : Matteman-Automation
 * Project    : Binnen thermometer met SHT41, ST7735 display en bewegingssensor
 * Version    : 1.1
 * Board      : ESP32-WROOM
 *
 * Hardware:
 * - ESP32-WROOM
 * - SHT41 temperatuur- en luchtvochtigheidssensor
 * - ST7735 1.8" TFT display
 * - PIR bewegingssensor
 *
 * Functie:
 * - Meet temperatuur en luchtvochtigheid
 * - Toont moderne dashboardweergave op TFT
 * - Display gaat aan bij beweging
 * - Display dimt vloeiend uit na 5 minuten zonder beweging
 *
 * Verbetering in versie 1.1:
 * - WiFi, MQTT & OTA toegevoegd
 *
 * Opmerking:
 * Deze versie gebruikt ledcAttach() en ledcWrite(pin, value)
 * voor ESP32 Arduino core 3.x.
 */

#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <Arduino_Secrets.h>
 
const char* ssid = YourSSID;
const char* password = YourWiFiPassWord;
const char* HostName = "Demo_Code";  //Naam van het apparaat in het netwerk

const char* mqtt_broker = YourMQTTserver;
const char* mqtt_user = YourMQTTuser;
const char* mqtt_password = YourMQTTpassword;
String sMQTTName;

int iWiFiTry = 0;
int iMQTTTry = 0;

WiFiClient espClient;
PubSubClient MQTTclient(espClient); // MQTT Client

// -----------------------------
// Extra kleuren indien niet aanwezig
// -----------------------------
#ifndef ST77XX_ORANGE
#define ST77XX_ORANGE 0xFD20
#endif

#ifndef ST77XX_DARKGREY
#define ST77XX_DARKGREY 0x7BEF
#endif

// -----------------------------
// Pin instellingen
// -----------------------------
#define TFT_CS     5
#define TFT_DC     16
#define TFT_RST    17
#define TFT_BL     25   // Backlight pin met PWM

#define PIR_PIN    27   // Bewegingssensor uitgang

#define I2C_SDA    21
#define I2C_SCL    22

// -----------------------------
// Display en sensor objecten
// -----------------------------
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
Adafruit_SHT4x sht4 = Adafruit_SHT4x();

// -----------------------------
// Instellingen
// -----------------------------
const unsigned long DISPLAY_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minuten
// const unsigned long DISPLAY_TIMEOUT = 10UL * 1000UL;    // 10 seconden voor test

// Voor een binnenthermometer is 10 seconden rustiger dan 5 seconden.
const unsigned long SENSOR_INTERVAL = 10000;               // elke 10 sec meten

const int BACKLIGHT_MIN = 0;
const int BACKLIGHT_MAX = 255;

const int FADE_STEP = 5;
const int FADE_DELAY = 10; // lager = sneller faden

// ESP32 PWM instellingen
const int PWM_FREQ = 5000;
const int PWM_RESOLUTION = 8; // 8-bit: 0-255

// -----------------------------
// Variabelen
// -----------------------------
unsigned long lastMotionTime = 0;
unsigned long lastSensorRead = 0;

bool displayIsOn = false;
int currentBacklight = 0;

float temperature = 0.0;
float humidity = 0.0;

// Deze variabelen worden gebruikt om alleen te verversen als een waarde echt veranderd is.
char lastTempText[10] = "";
char lastHumText[8] = "";
char lastComfortText[20] = "";
uint16_t lastComfortColor = 0;

void Connect2WiFi() { 
  WiFi.mode(WIFI_STA);  //WiFi mode, Client
  iWiFiTry = 0;
  WiFi.begin(ssid, password);
  WiFi.setHostname(HostName);
  Serial.print("Connecting to WiFi ");
  while (WiFi.status() != WL_CONNECTED && iWiFiTry < 11) { //Probeer 11 keer met WiFi te verbinden
    ++iWiFiTry;
    Serial.print(".");
    delay(500);
  }
  if(iWiFiTry < 11){
    Serial.println("");
    Serial.print("Got IP: ");  Serial.println(WiFi.localIP());
  } else {
    Serial.println("");
    Serial.print("Verbinding mislukt"); 
  }
}

void Connect2MQTT() {
  // Controleer of WiFi verbonden is, zo niet verbind met WiFi.
  if (WiFi.status() != WL_CONNECTED) { 
    Connect2WiFi; 
  }

  iMQTTTry=0;
  sMQTTName = "weerstation-" + String(WiFi.macAddress());  //Unique MQTT Device name
  Serial.print("Connecting to MQTT ");
  MQTTclient.setServer(mqtt_broker, 1883);
  while (!MQTTclient.connect(sMQTTName.c_str(), mqtt_user, mqtt_password) && iMQTTTry < 11) { ///Probeer 11 keer met MQTT te verbinden
    ++iWiFiTry;
    ++iMQTTTry;
    Serial.print(".");
    delay(500);
  }
  if(iMQTTTry < 11) {
    Serial.println("");
    Serial.println("Verbonden met MQTT");
  } else {
    Serial.println("");
    Serial.println("Verbinding mislukt");
    int iErrorCode = MQTTclient.state();
    Serial.print("MQTT connect failed, code = "); Serial.println(iErrorCode);
  }
  
}

void callback(char *topic, byte *payload, unsigned int length) {
  String sStatus;  // Bevat de waarde

  Serial.println("-----------------------");
  // Convert Char* to String
  String STopic = topic;    
  payload[length] = 0;   String recv_payload = String(( char *) payload);
  
  Serial.print("Message arrived in topic: "); Serial.println(topic);
  Serial.print("Message:");
  for (int i = 0; i < length; i++) {
      Serial.print((char) payload[i]);
      sStatus+= (char)payload[i];
  }
  sStatus.trim();
  Serial.println();

  if(strcmp(topic,"[Topic!]")==0) {
    // Doe iets als de [Topic!] binnenkomrt met sStatus
  }
}



// -----------------------------
// Backlight instellen
// -----------------------------
void setBacklight(int value) {
  value = constrain(value, BACKLIGHT_MIN, BACKLIGHT_MAX);
  ledcWrite(TFT_BL, value);
  currentBacklight = value;
}

// -----------------------------
// Vloeiend naar helderheid gaan
// -----------------------------
void fadeBacklightTo(int targetValue) {
  targetValue = constrain(targetValue, BACKLIGHT_MIN, BACKLIGHT_MAX);

  if (currentBacklight < targetValue) {
    for (int i = currentBacklight; i <= targetValue; i += FADE_STEP) {
      setBacklight(i);
      delay(FADE_DELAY);
    }
  } else if (currentBacklight > targetValue) {
    for (int i = currentBacklight; i >= targetValue; i -= FADE_STEP) {
      setBacklight(i);
      delay(FADE_DELAY);
    }
  }

  setBacklight(targetValue);
}

// -----------------------------
// Display aanzetten
// -----------------------------
void displayOn() {
  if (!displayIsOn) {
    fadeBacklightTo(BACKLIGHT_MAX);
    displayIsOn = true;
  }
}

// -----------------------------
// Display uitzetten
// -----------------------------
void displayOff() {
  if (displayIsOn) {
    fadeBacklightTo(BACKLIGHT_MIN);
    displayIsOn = false;
  }
}

// -----------------------------
// SHT41 uitlezen
// -----------------------------
void readSHT41() {
  sensors_event_t humidityEvent, tempEvent;

  if (sht4.getEvent(&humidityEvent, &tempEvent)) {
    temperature = tempEvent.temperature;
    humidity = humidityEvent.relative_humidity;
  }
}

// -----------------------------
// Comforttekst bepalen
// -----------------------------
const char* getComfortText() {
  // Temperatuur heeft prioriteit
  if (temperature >= 28.0) {
    return "Te warm";
  }

  if (temperature >= 26.0) {
    return "Erg warm";
  }

  if (temperature >= 24.0) {
    return "Warm";
  }

  if (temperature < 18.0) {
    return "Fris";
  }

  if (temperature < 20.0) {
    return "Koel";
  }

  // Binnen normale temperatuur kijken we naar luchtvochtigheid
  if (humidity < 35.0) {
    return "Droge lucht";
  }

  if (humidity > 65.0) {
    return "Te vochtig";
  }

  return "Comfortabel";
}

// -----------------------------
// Comfortkleur bepalen
// -----------------------------
uint16_t getComfortColor() {
  if (temperature >= 28.0) {
    return ST77XX_RED;
  }

  if (temperature >= 26.0) {
    return ST77XX_ORANGE;
  }

  if (temperature >= 24.0) {
    return ST77XX_YELLOW;
  }

  if (temperature < 18.0) {
    return ST77XX_CYAN;
  }

  if (temperature < 20.0) {
    return ST77XX_BLUE;
  }

  if (humidity < 35.0 || humidity > 65.0) {
    return ST77XX_YELLOW;
  }

  return ST77XX_GREEN;
}

// -----------------------------
// Helper: simpele thermometer tekenen
// -----------------------------
void drawThermometerIcon(int x, int y, uint16_t color) {
  tft.drawRoundRect(x + 5, y, 5, 20, 3, color);
  tft.fillCircle(x + 7, y + 22, 5, color);
  tft.drawLine(x + 7, y + 4, x + 7, y + 18, color);
}

// -----------------------------
// Helper: simpele waterdruppel tekenen
// -----------------------------
void drawDropIcon(int x, int y, uint16_t color) {
  tft.fillTriangle(x + 8, y, x, y + 16, x + 16, y + 16, color);
  tft.fillCircle(x + 8, y + 16, 8, color);
}

// -----------------------------
// Statische display layout tekenen
// Deze functie maakt het scherm één keer op.
// Daardoor hoeft het scherm niet bij elke meting volledig zwart gemaakt te worden.
// -----------------------------
void drawStaticDisplay() {
  tft.fillScreen(ST77XX_BLACK);

  // Header
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_DARKGREY, ST77XX_BLACK);
  tft.setCursor(110, 4);
  tft.print("Kantoor");

  tft.drawFastHLine(0, 18, 160, ST77XX_DARKGREY);

  // Temperatuur links
  drawThermometerIcon(10, 38, ST77XX_RED);

  tft.setTextColor(ST77XX_RED, ST77XX_BLACK);
  tft.setCursor(12, 78);
  tft.print("Temp");

  tft.drawFastHLine(12, 90, 50, ST77XX_RED);

  // Luchtvochtigheid rechts
  drawDropIcon(90, 38, ST77XX_CYAN);

  tft.setTextColor(ST77XX_CYAN, ST77XX_BLACK);
  tft.setCursor(90, 78);
  tft.print("Vocht");

  tft.drawFastHLine(90, 90, 50, ST77XX_CYAN);

  // Footer
  tft.drawFastHLine(0, 104, 160, ST77XX_DARKGREY);

  // Laat de eerste update altijd alles tekenen
  lastTempText[0] = '\0';
  lastHumText[0] = '\0';
  lastComfortText[0] = '\0';
  lastComfortColor = 0;
}

// -----------------------------
// Header status verversen
// -----------------------------
void updateHeaderStatus(const char* comfortText, uint16_t comfortColor) {
  // Alleen het linker headergebied wissen. "Kantoor" rechts blijft staan.
  tft.fillRect(0, 0, 105, 17, ST77XX_BLACK);

  tft.fillCircle(8, 8, 3, comfortColor);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(16, 4);
  tft.print(comfortText);
}

// -----------------------------
// Temperatuurwaarde verversen
// -----------------------------
void updateTemperatureValue(const char* tempText) {
  // Alleen het gebied met de temperatuur wissen
  tft.fillRect(30, 34, 52, 28, ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(32, 38);
  tft.print(tempText);

  // Je had de graden-aanduiding uitgezet. Daarom laat ik deze standaard uit.
  // Wil je hem toch weer tonen, haal dan de comments hieronder weg.
  /*
  int unitX = 32 + strlen(tempText) * 12 + 3;

  tft.setTextSize(1);
  tft.setCursor(unitX, 42);
  tft.print("o");
  tft.setCursor(unitX + 6, 42);
  tft.print("C");
  */
}

// -----------------------------
// Luchtvochtigheid verversen
// -----------------------------
void updateHumidityValue(const char* humText) {
  // Alleen het gebied met de luchtvochtigheid wissen
  tft.fillRect(112, 36, 48, 28, ST77XX_BLACK);

  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setTextSize(2);
  tft.setCursor(114, 40);
  tft.print(humText);

  // Zet het %-teken dynamisch achter de waarde.
  int unitX = 114 + strlen(humText) * 12 + 2;

  tft.setTextSize(1);
  tft.setCursor(unitX, 44);
  tft.print("%");
}

// -----------------------------
// Footer/status verversen
// -----------------------------
void updateFooterStatus(const char* comfortText, uint16_t comfortColor) {
  tft.fillRect(0, 106, 160, 20, ST77XX_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE, ST77XX_BLACK);
  tft.setCursor(28, 114);
  tft.print("Comfort: ");

  tft.setTextColor(comfortColor, ST77XX_BLACK);
  tft.print(comfortText);
}

// -----------------------------
// Alleen veranderende waarden verversen
// Deze functie voorkomt het knipperen dat ontstaat door tft.fillScreen().
// -----------------------------
void updateDisplayValues() {
  char tempText[10];
  char humText[8];

  snprintf(tempText, sizeof(tempText), "%.1f", temperature);
  snprintf(humText, sizeof(humText), "%.0f", humidity);

  const char* comfortText = getComfortText();
  uint16_t comfortColor = getComfortColor();

  bool comfortChanged = strcmp(comfortText, lastComfortText) != 0 || comfortColor != lastComfortColor;
  bool tempChanged = strcmp(tempText, lastTempText) != 0;
  bool humChanged = strcmp(humText, lastHumText) != 0;

  if (comfortChanged) {
    updateHeaderStatus(comfortText, comfortColor);
    updateFooterStatus(comfortText, comfortColor);

    strncpy(lastComfortText, comfortText, sizeof(lastComfortText));
    lastComfortText[sizeof(lastComfortText) - 1] = '\0';
    lastComfortColor = comfortColor;
  }

  if (tempChanged) {
    updateTemperatureValue(tempText);

    strncpy(lastTempText, tempText, sizeof(lastTempText));
    lastTempText[sizeof(lastTempText) - 1] = '\0';
  }

  if (humChanged) {
    updateHumidityValue(humText);

    strncpy(lastHumText, humText, sizeof(lastHumText));
    lastHumText[sizeof(lastHumText) - 1] = '\0';
  }
}

// -----------------------------
// Setup
// -----------------------------
void setup() {
  Serial.begin(115200);
  delay(500);

  Connect2WiFi();
  Connect2MQTT();

  // (optioneel) naam en wachtwoord voor OTA
  ArduinoOTA.setHostname(HostName);
  ArduinoOTA.setPassword(OTAPassword);  // Wachtwoord opgeslagen in Arduino_Secrets.h

  // OTA starten
  ArduinoOTA.begin();

  MQTTclient.setCallback(callback);
  // MQTTclient.subscribe("[Topic!]");

  Serial.println();
  Serial.println("Binnen thermometer gestart");

  pinMode(PIR_PIN, INPUT);
  // Eventueel proberen als de PIR onrustig is:
  // pinMode(PIR_PIN, INPUT_PULLDOWN);

  // PWM voor backlight - ESP32 Arduino core 3.x
  ledcAttach(TFT_BL, PWM_FREQ, PWM_RESOLUTION);
  setBacklight(0);

  // I2C starten
  Wire.begin(I2C_SDA, I2C_SCL);

  // SHT41 starten
  if (!sht4.begin()) {
    Serial.println("SHT41 niet gevonden!");
  } else {
    Serial.println("SHT41 gevonden.");
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);
  }

  // Display starten
  tft.initR(INITR_BLACKTAB);
  // Bij problemen met kleuren of schermoffset eventueel proberen:
  // tft.initR(INITR_REDTAB);
  // tft.initR(INITR_GREENTAB);

  tft.setRotation(1); // Landscape: ongeveer 160 x 128

  // Eerste meting en eerste schermopbouw
  readSHT41();
  drawStaticDisplay();
  updateDisplayValues();

  lastMotionTime = millis();
  lastSensorRead = millis();

  // Scherm bij opstart kort aanzetten
  displayOn();
}

// -----------------------------
// Loop
// -----------------------------
void loop() {
  unsigned long now = millis();

  int iRSSI;

   MQTTclient.loop();
   ArduinoOTA.handle();

  

  // Bewegingssensor controleren
  bool motionDetected = digitalRead(PIR_PIN) == HIGH;

  // Debug eventueel aanzetten:
  // Serial.print("PIR status: ");
  // Serial.println(motionDetected ? "BEWEGING" : "geen beweging");

  if (motionDetected) {
    bool wasDisplayOff = !displayIsOn;

    lastMotionTime = now;
    displayOn();

    // Als het scherm uit stond, meteen de actuele waarden tonen.
    // Anders zou je maximaal SENSOR_INTERVAL moeten wachten.
    if (wasDisplayOff) {
      readSHT41();
      updateDisplayValues();
      lastSensorRead = now;
    }
  }

  // Na 5 minuten zonder beweging display uit
  if (displayIsOn && now - lastMotionTime > DISPLAY_TIMEOUT) {
    displayOff();
  }

  // Sensor periodiek uitlezen
  if (now - lastSensorRead > SENSOR_INTERVAL) {
    lastSensorRead = now;

    readSHT41();

    if (displayIsOn) {
      updateDisplayValues();
    }

    Serial.print("Temperatuur: ");
    Serial.print(temperature, 1);
    Serial.print(" C, Vocht: ");
    Serial.print(humidity, 0);
    Serial.print(" %, Status: ");
    Serial.println(getComfortText());
    // Is MQTT nog verbonden?
    if (!MQTTclient.connect(sMQTTName.c_str())) {
      Connect2MQTT();
    }
    if (iWiFiTry < 11) {  
      
      iRSSI = WiFi.RSSI();
      MQTTclient.publish("binnenthermometer/kantoor/rssi", String(iRSSI).c_str());
      MQTTclient.publish("binnenthermometer/kantoor/temperatuur", String(temperature).c_str());
      MQTTclient.publish("binnenthermometer/kantoor/vocht", String(humidity).c_str());


    } else {
      Serial.println("*** ERROR *** Geen WiFi.");
    }
  }  
}
