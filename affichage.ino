#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <time.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

// --- Configuration du Wi-Fi ---
const char* ssid     = "VOTRE_WIFI";
const char* password = "VOTRE_MOT_DE_PASSE";

// --- Configuration IP Fixe ---
IPAddress local_IP(192, 168, 1, 121);
IPAddress gateway(192, 168, 1, 254);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(192, 168, 1, 254);

// --- Configuration du Matériel (7 blocs) ---
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 7 
#define CS_PIN D8

MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);
ESP8266WebServer server(80);

// --- Variables d'état ---
enum Mode { MODE_CLOCK, MODE_TEXT, MODE_WEATHER, MODE_POSIX_LIVE, MODE_COUNTDOWN };
Mode currentMode = MODE_CLOCK;

String textToDisplay = "STATUT OK";
String lastDisplayedStr = "";
String weatherString = "LYON -- C";

int ledIntensity = 2;
int scrollSpeed = 60;
unsigned long targetTimestamp = 0;

unsigned long lastWeatherUpdate = 0;
const unsigned long weatherInterval = 3600000; 
unsigned long lastDisplayUpdate = 0;

// --- Interface Web Globale (v3.1) ---
const char HTML_INTERFACE[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Dashboard Ultime v3.1</title>
    <style>
        body { font-family: 'Courier New', Courier, monospace; background-color: #0d1117; color: #58a6ff; text-align: center; padding: 20px; }
        h1 { color: #f0f6fc; border-bottom: 2px solid #21262d; padding-bottom: 10px; }
        .card { background-color: #161b22; border: 1px solid #30363d; border-radius: 6px; padding: 15px; max-width: 500px; margin: 15px auto; box-shadow: 0 4px 10px rgba(0,0,0,0.5); }
        .btn { background-color: #238636; color: white; border: none; padding: 10px 20px; font-size: 15px; font-weight: bold; cursor: pointer; border-radius: 6px; margin: 5px; width: 85%; transition: 0.2s; }
        .btn:hover { background-color: #2ea44f; }
        .btn-alt { background-color: #21262d; border: 1px solid #f0f6fc; color: #c9d1d9; }
        .btn-alt:hover { background-color: #30363d; }
        .btn-weather { background-color: #1f6feb; }
        .btn-cyber { background-color: #8b5cf6; }
        input[type="text"], input[type="datetime-local"] { width: 80%; padding: 10px; background-color: #0d1117; border: 1px solid #30363d; color: #f0f6fc; border-radius: 6px; font-size: 15px; margin-bottom: 10px; font-family: inherit; }
        .slider-container { margin: 15px 0; text-align: left; padding: 0 10px; }
        .slider-container label { display: block; margin-bottom: 5px; color: #c9d1d9; }
        input[type="range"] { width: 100%; accent-color: #58a6ff; }
        .footer { font-size: 12px; color: #8b949e; margin-top: 30px; }
    </style>
</head>
<body>
    <h1>[ NodeMCU Control Center v3.1 ]</h1>
    <div class="card">
        <h3>Sélection du Mode</h3>
        <button class="btn" onclick="location.href='/setmode?mode=clock'">🕒 Horloge NTP</button>
        <button class="btn btn-weather" onclick="location.href='/setmode?mode=weather'">🌤️ Météo LYON</button>
        <button class="btn btn-cyber" onclick="location.href='/setmode?mode=live'">⏱️ POSIX Live</button>
        <button class="btn btn-cyber" onclick="location.href='/setmode?mode=countdown'">⏳ Activer Countdown</button>
    </div>
    <div class="card">
        <h3>Configuration de la Deadline</h3>
        <form action="/settarget" method="get">
            <input type="datetime-local" name="datetime" required><br>
            <button type="submit" class="btn btn-cyber">💾 Fixer la Date Butoire</button>
        </form>
    </div>
    <div class="card">
        <h3>Texte Défilant libre</h3>
        <form action="/setmode" method="get">
            <input type="hidden" name="mode" value="text">
            <input type="text" name="content" placeholder="Votre message..." required maxlength="64"><br>
            <button type="submit" class="btn btn-alt">💾 Envoyer le texte</button>
        </form>
    </div>
    <div class="card">
        <h3>Réglages Matériel</h3>
        <div class="slider-container">
            <label>☀️ Luminosité (<span id="valIntensity">%INT%</span>/15) :</label>
            <input type="range" min="0" max="15" value="%INT%" onchange="fetch('/update?intensity='+this.value); document.getElementById('valIntensity').innerText=this.value;">
        </div>
        <div class="slider-container">
            <label>🏃 Vitesse de défilement :</label>
            <input type="range" min="20" max="150" value="%SPD%" onchange="fetch('/update?speed='+this.value);">
        </div>
    </div>
    <div class="footer">IP: 192.168.1.121</div>
</body>
</html>
)=====";

// --- Récupération API Météo ---
void updateWeather() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    String url = "http://api.open-meteo.com/v1/forecast?latitude=45.764&longitude=4.835&current_weather=true&daily=temperature_2m_max,temperature_2m_min&timezone=Europe/Paris";
    
    http.begin(client, url);
    int httpCode = http.GET();
    if (httpCode > 0) {
      String payload = http.getString();
      DynamicJsonDocument doc(2048);
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        float tempActuelle = doc["current_weather"]["temperature"];
        int code = doc["current_weather"]["weathercode"];
        float tempMax = doc["daily"]["temperature_2m_max"][0];
        float tempMin = doc["daily"]["temperature_2m_min"][0];
        
        String statusText = "";
        if (code == 0) statusText = "SOLEIL";
        else if (code >= 1 && code <= 3) statusText = "NUAGEUX";
        else if (code >= 45 && code <= 48) statusText = "BROUILLARD";
        else if (code >= 51 && code <= 65) statusText = "PLUIE";
        else if (code >= 71 && code <= 77) statusText = "NEIGE";
        else if (code >= 80 && code <= 82) statusText = "AVERSES";
        else if (code >= 95 && code <= 99) statusText = "ORAGE";
        else statusText = "METEO";
        
        char tActBuf[8], tMaxBuf[6], tMinBuf[6];
        dtostrf(tempActuelle, 4, 1, tActBuf);
        dtostrf(tempMax, 2, 0, tMaxBuf);
        dtostrf(tempMin, 2, 0, tMinBuf);
        
        String sAct = String(tActBuf); sAct.trim();
        String sMax = String(tMaxBuf); sMax.trim();
        String sMin = String(tMinBuf); sMin.trim();

        weatherString = "LYON : " + sAct + " C - " + statusText + " - MIN: " + sMin + " C / MAX: " + sMax + " C";
      } else { weatherString = "ERR JSON"; }
    } else { weatherString = "ERR HTTP"; }
    http.end();
  } else { weatherString = "ERR WIFI"; }
}

// --- Serveur Web Handlers ---
void handleRoot() {
  String html = String(HTML_INTERFACE);
  html.replace("%INT%", String(ledIntensity));
  html.replace("%SPD%", String(scrollSpeed));
  server.send(200, "text/html", html);
}

void handleSetMode() {
  if (server.hasArg("mode")) {
    String modeArg = server.arg("mode");
    if (modeArg == "clock") {
      currentMode = MODE_CLOCK;
      P.displayClear();
      P.setTextAlignment(PA_CENTER);
    } else if (modeArg == "weather") {
      currentMode = MODE_WEATHER;
      updateWeather(); 
      P.displayClear();
      P.displayText(weatherString.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    } else if (modeArg == "live") {
      currentMode = MODE_POSIX_LIVE;
      P.displayClear();
      P.setTextAlignment(PA_CENTER);
    } else if (modeArg == "countdown") {
      currentMode = MODE_COUNTDOWN;
      P.displayClear();
      P.setTextAlignment(PA_CENTER);
    } else if (modeArg == "text" && server.hasArg("content")) {
      currentMode = MODE_TEXT;
      textToDisplay = server.arg("content");
      P.displayClear();
      P.displayText(textToDisplay.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    }
    EEPROM.write(4, (int)currentMode);
    EEPROM.commit();
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Redirecting...");
}

void handleSetTarget() {
  if (server.hasArg("datetime")) {
    String dt = server.arg("datetime"); // Format: "YYYY-MM-DDTHH:MM"
    if (dt.length() >= 16) {
      struct tm tm_target;
      // Extraction manuelle ultra-robuste sans sscanf risqué
      int year   = dt.substring(0, 4).toInt();
      int month  = dt.substring(5, 7).toInt();
      int day    = dt.substring(8, 10).toInt();
      int hour   = dt.substring(11, 13).toInt();
      int minute = dt.substring(14, 16).toInt();
      
      tm_target.tm_year = year - 1900;
      tm_target.tm_mon = month - 1;
      tm_target.tm_mday = day;
      tm_target.tm_hour = hour;
      tm_target.tm_min = minute;
      tm_target.tm_sec = 0;
      tm_target.tm_isdst = -1;
      
      time_t t_utc = mktime(&tm_target);
      if (t_utc != -1) {
        targetTimestamp = (unsigned long)t_utc;
        EEPROM.write(0, (targetTimestamp >> 24) & 0xFF);
        EEPROM.write(1, (targetTimestamp >> 16) & 0xFF);
        EEPROM.write(2, (targetTimestamp >> 8) & 0xFF);
        EEPROM.write(3, targetTimestamp & 0xFF);
        
        currentMode = MODE_COUNTDOWN;
        EEPROM.write(4, (int)currentMode);
        EEPROM.commit();
        P.displayClear();
      }
    }
  }
  server.sendHeader("Location", "/");
  server.send(303, "text/plain", "Redirecting...");
}

void handleUpdate() {
  if (server.hasArg("intensity")) {
    ledIntensity = server.arg("intensity").toInt();
    P.setIntensity(ledIntensity);
    EEPROM.write(5, ledIntensity);
  }
  if (server.hasArg("speed")) {
    scrollSpeed = server.arg("speed").toInt();
    P.setSpeed(scrollSpeed);
    EEPROM.write(6, scrollSpeed);
  }
  EEPROM.commit();
  server.send(200, "text/plain", "OK");
}

// --- Les deux fonctions reines qu'Arduino réclame ---
void setup() {
  EEPROM.begin(512);
  targetTimestamp = ((unsigned long)EEPROM.read(0) << 24) |
                    ((unsigned long)EEPROM.read(1) << 16) |
                    ((unsigned long)EEPROM.read(2) << 8)  |
                    ((unsigned long)EEPROM.read(3));
                    
  int savedMode = EEPROM.read(4);
  currentMode = (savedMode <= 4 && savedMode >= 0) ? (Mode)savedMode : MODE_CLOCK;
  
  ledIntensity = EEPROM.read(5);
  if (ledIntensity > 15 || ledIntensity < 0) ledIntensity = 2;
  
  scrollSpeed = EEPROM.read(6);
  if (scrollSpeed > 150 || scrollSpeed < 20) scrollSpeed = 60;

  P.begin();
  P.setIntensity(ledIntensity);
  P.setSpeed(scrollSpeed);
  P.setTextAlignment(PA_CENTER);
  P.print("WIFI...");

  WiFi.config(local_IP, gateway, subnet, dns);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); }

  if (MDNS.begin("led")) { MDNS.addService("http", "tcp", 80); }

  configTime("CET-1CEST,M3.5.0,M10.5.0/3", "fr.pool.ntp.org", "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 8 * 3600 * 2) { delay(500); now = time(nullptr); }

  server.on("/", handleRoot);
  server.on("/setmode", handleSetMode);
  server.on("/settarget", handleSetTarget);
  server.on("/update", handleUpdate);
  server.begin();
  
  updateWeather();
  lastWeatherUpdate = millis();
  P.displayClear();
  
  if (currentMode == MODE_WEATHER) {
    P.displayText(weatherString.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  } else if (currentMode == MODE_TEXT) {
    P.displayText(textToDisplay.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }
}

void loop() {
  server.handleClient();
  MDNS.update();

  unsigned long currentMillis = millis();

  if (currentMillis - lastWeatherUpdate >= weatherInterval) {
    lastWeatherUpdate = currentMillis;
    updateWeather();
    if (currentMode == MODE_WEATHER) {
      P.displayText(weatherString.c_str(), PA_CENTER, scrollSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    }
  }

  if (currentMode == MODE_CLOCK) {
    if (currentMillis - lastDisplayUpdate >= 1000) {
      lastDisplayUpdate = currentMillis;
      time_t now = time(nullptr);
      struct tm* timeinfo = localtime(&now);
      char timeStr[6];
      strftime(timeStr, sizeof(timeStr), (now % 2 == 0) ? "%H:%M" : "%H %M", timeinfo);
      String toDisplay = String(timeStr);
      if (toDisplay != lastDisplayedStr) {
        P.print(toDisplay);
        lastDisplayedStr = toDisplay;
      }
    }
  } 
  else if (currentMode == MODE_POSIX_LIVE || currentMode == MODE_COUNTDOWN) {
    if (currentMillis - lastDisplayUpdate >= 1000) {
      lastDisplayUpdate = currentMillis;
      time_t now = time(nullptr);
      char displayBuf[16];

      if (currentMode == MODE_POSIX_LIVE) {
        snprintf(displayBuf, sizeof(displayBuf), "%ld", (long)now);
      } else {
        if (targetTimestamp > (unsigned long)now) {
          snprintf(displayBuf, sizeof(displayBuf), "%lu", targetTimestamp - (unsigned long)now);
        } else {
          snprintf(displayBuf, sizeof(displayBuf), "0000000000");
        }
      }
      P.print(displayBuf);
    }
  } 
  else if (currentMode == MODE_TEXT || currentMode == MODE_WEATHER) {
    if (P.displayAnimate()) {
      P.displayReset();
    }
  }
}
