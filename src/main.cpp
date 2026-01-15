// ================== LIBRARIES ==================
#include <DHT.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ===== GAS SENSOR STABILITY FILTER =====
#define GAS_FILTER_SIZE 10

int gasSamples[GAS_FILTER_SIZE];
int nh3Samples[GAS_FILTER_SIZE];
uint8_t gasIndex = 0;
bool filterFilled = false;

bool dailyReportEnabled = false;
float lastValidTemp = 25.0;
float lastValidHum  = 50.0;

// ================== ALERT STATE FLAGS ==================
bool smsSentForCurrentAlert = false;
bool lastAlertState = false;

// ================== MULTI CONTACT SUPPORT ==================
#define MAX_CONTACTS 5
#define MAX_ATTEMPTS_PER_NUMBER 2

String phoneNumbers[MAX_CONTACTS] = {
  "+918010845905",
  "+911111111111",
  "+922222222222",
  "+933333333333",
  "+944444444444"
};

int currentContactIndex = 0;
int attemptsForCurrentNumber = 0;
bool alertAcknowledged = false;

// ================== DAILY STATS ==================
struct DailyStats {
  float minTemp;
  float maxTemp;
  float minHum;
  float maxHum;
  bool valid;
};

DailyStats todayStats;
int lastRecordedDay = -1;


void resetDailyStats() {
  todayStats.minTemp = 1000;
  todayStats.maxTemp = -1000;
  todayStats.minHum = 1000;
  todayStats.maxHum = -1000;
  todayStats.valid = true;
}

void updateDailyStats(float t, float h) {
  if (!todayStats.valid) resetDailyStats();

  todayStats.minTemp = min(todayStats.minTemp, t);
  todayStats.maxTemp = max(todayStats.maxTemp, t);
  todayStats.minHum  = min(todayStats.minHum, h);
  todayStats.maxHum  = max(todayStats.maxHum, h);
}

// ===== FUNCTION PROTOTYPES =====
String sendATCommand(const char *cmd, uint32_t waitMs );

bool sendSMS(String phoneNumber, String message);

bool getNetworkTime(int &hour, int &minute, int &day);

bool getNetworkTime(int &hour, int &minute, int &day) {
  String resp = sendATCommand("AT+CCLK?", 2000);
  int q1 = resp.indexOf("\"");
  int q2 = resp.indexOf("\"", q1 + 1);
  if (q1 < 0 || q2 < 0) return false;

  String t = resp.substring(q1 + 1, q2); // yy/MM/dd,hh:mm:ss
  day = t.substring(6, 8).toInt();
  hour = t.substring(9, 11).toInt();
  minute = t.substring(12, 14).toInt();
  return true;
}


void checkDailyReport() {
  if (!dailyReportEnabled) return;

  int hour, minute, day;
  if (!getNetworkTime(hour, minute, day)) return;

  if (day != lastRecordedDay && hour == 8 && minute < 5) {
    String msg = "📊 DAILY REPORT\n";
    msg += "Temp Min: " + String(todayStats.minTemp,1) + "C\n";
    msg += "Temp Max: " + String(todayStats.maxTemp,1) + "C\n";
    msg += "Hum Min: " + String(todayStats.minHum,0) + "%\n";
    msg += "Hum Max: " + String(todayStats.maxHum,0) + "%";

    sendSMS(phoneNumbers[0], msg);

    lastRecordedDay = day;
    resetDailyStats();
  }
}

// ===== SENSOR CALIBRATION =====

// ESP32 ADC
#define ADC_MAX 4095.0
#define ADC_VREF 3.3

// MQ-2 (Gas) Approx Calibration
#define MQ2_RL 10.0      // Load resistance (kΩ)
#define MQ2_R0 9.83      // Calibrated in clean air (adjust once)

// MQ-137 (NH3) Approx Calibration
#define MQ137_RL 10.0
#define MQ137_R0 12.0    // Calibrated in clean air

// Limits (realistic)
#define GAS_MAX_PPM   5000
#define NH3_MAX_PPM   300


// ================== DHT ==================
#define DHTPIN 2
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

// ================== SENSORS ==================
#define MQ_GAS_PIN   34
#define MQ137_PIN    35
#define FLAME_PIN    33

// ================== LCD (WAVESHARE 1.5") ==================
#define TFT_CS   5
#define TFT_DC   16
#define TFT_RST  17
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ================== A7670 MODEM ==================
#define MODEM_PWRKEY   4
#define MODEM_RESET    14
#define MODEM_POWERON  12
#define MODEM_TX       26
#define MODEM_RX       27

// ================== WiFi Configuration ==================
const char* AP_SSID = "EnvMonitor_Config";
const char* AP_PASSWORD = "12345678";
WebServer server(80);
Preferences preferences;

// ================== PHONE NUMBER ==================
String TO_PHONE_NUMBER = "+918010845905";

// ================== THRESHOLDS ==================
int GAS_LIMIT = 1800;
int AMMONIA_LIMIT = 1600;
float TEMP_LOW  = 10.0;
float TEMP_HIGH = 35.0;
float HUM_LOW   = 30.0;
float HUM_HIGH  = 80.0;

// ================== SMS TIMING ==================
unsigned long lastSMSTime = 0;
const unsigned long SMS_INTERVAL = 30000;  // 30 seconds

// ================== DISPLAY TIMING ==================
unsigned long lastDisplayUpdate = 0;
const unsigned long DISPLAY_INTERVAL = 2000;
bool displayReady = false;

// ================== CALL STATE ==================
enum CallState {
  CALL_IDLE,
  CALL_DIALING,
  CALL_RINGING,
  CALL_CONNECTED,
  CALL_FAILED
};

CallState callState = CALL_IDLE;
unsigned long callStartTime = 0;
unsigned long lastCallAttempt = 0;
int callAttempts = 0;
const int MAX_CALL_ATTEMPTS = 5;
const unsigned long CALL_TIMEOUT = 45000;
const unsigned long RETRY_DELAY = 10000;
const unsigned long ALERT_COOLDOWN = 300000;
unsigned long alertCooldownStart = 0;

String currentAlertType = "";
bool callInProgress = false;

// ================== DISPLAY TEST MODE ==================
#define DISPLAY_TEST_MODE false

// ================== DISPLAY OFFSET ==================
#define X_OFFSET 0
  
// ================== WEB SERVER HTML ==================
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Environment Monitor Config</title>
  <style>
    body {
      font-family: Arial, sans-serif;
      max-width: 600px;
      margin: 50px auto;
      padding: 20px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      min-height: 100vh;
    }
    .container {
      background: white;
      padding: 30px;
      border-radius: 15px;
      box-shadow: 0 10px 30px rgba(0,0,0,0.3);
    }
    h1 {
      color: #333;
      text-align: center;
      margin-bottom: 10px;
    }
    h2 {
      color: #667eea;
      font-size: 18px;
      margin-top: 25px;
      margin-bottom: 15px;
      border-bottom: 2px solid #667eea;
      padding-bottom: 5px;
    }
    .form-group { margin-bottom: 20px; }
    label {
      display: block;
      margin-bottom: 8px;
      color: #555;
      font-weight: bold;
      font-size: 14px;
    }
    input {
      width: 100%;
      padding: 12px;
      border: 2px solid #ddd;
      border-radius: 8px;
      font-size: 16px;
      box-sizing: border-box;
      margin-bottom: 8px;
    }
    .hint { font-size: 12px; color: #888; }
    .range-inputs {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 10px;
    }
    button {
      width: 100%;
      padding: 14px;
      background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
      color: white;
      border: none;
      border-radius: 8px;
      font-size: 16px;
      font-weight: bold;
      cursor: pointer;
      margin-top: 10px;
    }
    .current-settings {
      background: #f0f0f0;
      padding: 15px;
      border-radius: 8px;
      margin-bottom: 25px;
    }
    .success {
      background: #d4edda;
      color: #155724;
      padding: 12px;
      border-radius: 8px;
      margin-top: 15px;
      display: none;
      text-align: center;
    }
    .test-btn {
      background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%);
    }
  </style>
</head>

<body>
<div class="container">

<h1>  ]= Environment Monitor</h1>
<p style="text-align:center; color:#888; font-size:14px;">Configuration Panel</p>

<div class="current-settings">
  <h2 style="margin-top:0; border:none;"> Current Settings</h2>
  <div><strong>Contacts:</strong> <span id="displayPhone">Loading...</span></div>
  <div><strong>Daily Report:</strong> <span id="displayReport">Loading...</span></div>
  <div><strong>Temperature:</strong> <span id="displayTemp">Loading...</span> °C</div>
  <div><strong>Humidity:</strong> <span id="displayHum">Loading...</span> %</div>
</div>

<form id="configForm">

<h2>Emergency Contacts (Call Order)</h2>

<div class="form-group"><input type="tel" id="phone0" placeholder="+918010845905" required></div>
<div class="form-group"><input type="tel" id="phone1" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone2" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone3" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone4" placeholder="+91XXXXXXXXXX"></div>

<p class="hint">System will call each number twice in order until someone answers.</p>

<h2> Temperature Limits</h2>
<div class="form-group">
  <div class="range-inputs">
    <input type="number" step="0.1" id="tlow" placeholder="Min (e.g., 10)" required>
    <input type="number" step="0.1" id="thigh" placeholder="Max (e.g., 35)" required>
  </div>
</div>

<h2> Humidity Limits</h2>
<div class="form-group">
  <div class="range-inputs">
    <input type="number" step="0.1" id="hlow" placeholder="Min (e.g., 30)" required>
    <input type="number" step="0.1" id="hhigh" placeholder="Max (e.g., 80)" required>
  </div>
</div>

<h2>Daily Summary Report</h2>
<div class="form-group">
  <label>
    <input type="checkbox" id="dailyReport">
    Enable Daily Report SMS (8:00 AM)
  </label>
</div>

<button type="submit"> Save All Settings</button>
<button type="button" class="test-btn" onclick="testSMS()"> Test SMS</button>
<button type="button" class="test-btn" onclick="testCall()"> Test Call</button>

</form>

<div class="success" id="successMsg"> Settings saved successfully!</div>

</div>

<script>
function loadSettings() {
  fetch('/getSettings')
    .then(r => r.json())
    .then(data => {
      let contacts = [];
      for (let i = 0; i < 5; i++) {
        const key = "phone" + i;
        document.getElementById(key).value = data[key] || "";
        if (data[key]) contacts.push(data[key]);
      }
      displayPhone.textContent = contacts.length ? contacts.join(', ') : 'Not configured';
      displayTemp.textContent = data.tlow + ' to ' + data.thigh;
      displayHum.textContent = data.hlow + ' to ' + data.hhigh;
      dailyReport.checked = data.dailyReport;
      displayReport.textContent = data.dailyReport ? "Enabled (8:00 AM)" : "Disabled";
    });
}

loadSettings();

document.getElementById('configForm').addEventListener('submit', e => {
  e.preventDefault();

  let data = '';
  for (let i = 0; i < 5; i++) {
    data += 'phone' + i + '=' + encodeURIComponent(document.getElementById('phone' + i).value) + '&';
  }

  data +=
    'tlow=' + tlow.value +
    '&thigh=' + thigh.value +
    '&hlow=' + hlow.value +
    '&hhigh=' + hhigh.value +
    '&dailyReport=' + (dailyReport.checked ? '1' : '0');

  fetch('/setSettings', {
    method: 'POST',
    headers: {'Content-Type':'application/x-www-form-urlencoded'},
    body: data
  })
  .then(() => {
    successMsg.style.display = 'block';
    setTimeout(() => successMsg.style.display = 'none', 3000);
    loadSettings();
  });
});

function testSMS() {
  fetch('/testSMS', {method:'POST'})
    .then(() => alert('Test SMS sent!'));
}

function testCall() {
  fetch('/testCall', {method:'POST'})
    .then(() => alert('Test call initiated!'));
}
</script>

</body>
</html>
)rawliteral";

// ================== A7670 MODEM FUNCTIONS ==================
void powerOnModem() {
  Serial.println("Powering on A7670 modem...");
  pinMode(MODEM_POWERON, OUTPUT);
  digitalWrite(MODEM_POWERON, HIGH);
  pinMode(MODEM_RESET, OUTPUT);
  digitalWrite(MODEM_RESET, LOW);
  delay(100);
  digitalWrite(MODEM_RESET, HIGH);
  delay(2000);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  Serial.println("Modem powered on.");
}

void sendAT(const char *cmd, uint32_t waitMs = 1000) {
  Serial.print("AT CMD: ");
  Serial.println(cmd);
  Serial1.flush();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    if (Serial1.available()) {
      Serial.write(Serial1.read());
    }
  }
}

void hangupCall() {
  Serial.println("Hanging up call...");
  sendAT("ATH", 1000);
  callInProgress = false;
}

String filterASCII(const String &in) {
  String out = "";
  for (int i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c >= 32 && c <= 126) out += c;
  }
  return out;
}

String sendATCommand(const char *cmd, uint32_t waitMs = 2000) {
  Serial.print("AT CMD: ");
  Serial.println(cmd);
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String response = "";
  
  while (millis() - start < waitMs) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c >= 32 && c <= 126) {
        response += c;
        Serial.write(c);
      }
    }
  }
  
  return response;
}

void initModem() {
  Serial.println("Initializing modem...");
  sendAT("AT", 500);
  sendAT("AT+CFUN=1", 1000);
  sendAT("AT+CMGF=1", 500);
  sendAT("AT+CSCS=\"GSM\"", 500);
  Serial.println("Modem ready.");
}

bool sendSMS(String phoneNumber, String message) {
  Serial.println("Sending SMS to: " + phoneNumber);
  Serial.println("Message: " + message);
  
  sendAT("AT+CMGF=1", 1000);
  
  String cmd = "AT+CMGS=\"" + phoneNumber + "\"";
  Serial1.println(cmd);
  delay(500);
  
  Serial1.print(message);
  delay(100);
  Serial1.write(26);
  
  unsigned long start = millis();
  String response = "";
  while (millis() - start < 10000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;
      Serial.write(c);
      if (response.indexOf("OK") >= 0) {
        Serial.println("SMS sent successfully!");
        return true;
      }
      if (response.indexOf("ERROR") >= 0) {
        Serial.println("SMS failed!");
        return false;
      }
    }
  }
  
  Serial.println("SMS timeout!");
  return false;
}

bool makeDirectCall(String phoneNumber) {
  sendAT("ATH", 500);
  delay(300);

  String cmd = "ATD" + phoneNumber + ";";
  String response = sendATCommand(cmd.c_str(), 3000);

  if (response.indexOf("OK") >= 0) {
    callInProgress = true;
    callStartTime = millis();
    callState = CALL_DIALING;
    return true;
  }

  return false;
}

void checkCallStatus() {
  if (!callInProgress) return;

  String response = sendATCommand("AT+CLCC", 1500);
  response = filterASCII(response);

  int idx = response.indexOf("+CLCC:");
  if (idx < 0) {
 if (millis() - callStartTime > CALL_TIMEOUT) {
  Serial.println("⏱ Call timeout, moving to next number");
  hangupCall();
  callInProgress = false;
  callState = CALL_IDLE;
  attemptsForCurrentNumber = MAX_ATTEMPTS_PER_NUMBER;
}

    return;
  }

  int p1 = response.indexOf(',', idx);
  int p2 = response.indexOf(',', p1 + 1);
  int p3 = response.indexOf(',', p2 + 1);

  if (p1 < 0 || p2 < 0 || p3 < 0) return;

  int stat = response.substring(p2 + 1, p3).toInt();

  switch (stat) {

    
    case 0: // ANSWERED
  Serial.println("✅ CALL ANSWERED – STOPPING ALERTS");
  callState = CALL_CONNECTED;
  alertAcknowledged = true;
  delay(10000);
  hangupCall();
  callInProgress = false;
  break;


    case 3:
      callState = CALL_RINGING;
      break;

    case 6:
      Serial.println("Call rejected/busy");
      hangupCall();
      callInProgress = false;
      callState = CALL_IDLE;
      break;
  }
}

void sendParametersSMS(float temp, float hum, int gas, int nh3, bool fire) {
  String message = "ENV MONITOR:\n";
  message += "Temp: " + String(temp, 1) + "C\n";
  message += "Humidity: " + String(hum, 0) + "%\n";
  message += "Gas: " + String(gas) + " PPM\n";
  message += "NH3: " + String(nh3) + " PPM\n";
  message += "Fire: " + String(fire ? "YES" : "NO");
  
 sendSMS(phoneNumbers[0], message);

}

void handleAlerts(float temp, float hum, int gas, int nh3, bool fire) {

  if (alertAcknowledged) return;

  bool shouldAlert =
    fire ||
    (temp < TEMP_LOW || temp > TEMP_HIGH) ||
    (hum < HUM_LOW || hum > HUM_HIGH) ||
    (gas > GAS_LIMIT) ||
    (nh3 > AMMONIA_LIMIT);

if (!shouldAlert) {
  currentContactIndex = 0;
  attemptsForCurrentNumber = 0;
  callAttempts = 0;
  alertAcknowledged = false;
  return;
}


  if (callInProgress) {
    checkCallStatus();
    return;
  }

  if (currentContactIndex >= MAX_CONTACTS) {
    Serial.println("❌ All contacts tried. No answer.");
    return;
  }

  if (millis() - lastCallAttempt < RETRY_DELAY) return;

  Serial.printf("📞 Calling %s (Attempt %d)\n",
                phoneNumbers[currentContactIndex].c_str(),
                attemptsForCurrentNumber + 1);

  makeDirectCall(phoneNumbers[currentContactIndex]);
  lastCallAttempt = millis();
  attemptsForCurrentNumber++;
  callAttempts++;

  if (attemptsForCurrentNumber >= MAX_ATTEMPTS_PER_NUMBER) {
    attemptsForCurrentNumber = 0;
    currentContactIndex++;
  }
}


  
// ================== WEB SERVER HANDLERS ==================
void handleRoot() {
  server.send_P(200, "text/html", CONFIG_PAGE);
}

void handleGetSettings() {
  String json = "{";

  for (int i = 0; i < MAX_CONTACTS; i++) {
    json += "\"phone" + String(i) + "\":\"" + phoneNumbers[i] + "\",";
  }

  json += "\"tlow\":" + String(TEMP_LOW, 1) + ",";
  json += "\"thigh\":" + String(TEMP_HIGH, 1) + ",";
  json += "\"hlow\":" + String(HUM_LOW, 1) + ",";
  json += "\"hhigh\":" + String(HUM_HIGH, 1) + ",";
  json += "\"dailyReport\":" + String(dailyReportEnabled ? "true" : "false");

  json += "}";
  server.send(200, "application/json", json);
}


void handleSetSettings() {

  for (int i = 0; i < MAX_CONTACTS; i++) {
    String key = "phone" + String(i);
    phoneNumbers[i] = server.arg(key);
    preferences.putString(key.c_str(), phoneNumbers[i]);
  }

  TEMP_LOW  = server.arg("tlow").toFloat();
  TEMP_HIGH = server.arg("thigh").toFloat();
  HUM_LOW   = server.arg("hlow").toFloat();
  HUM_HIGH  = server.arg("hhigh").toFloat();

  preferences.putFloat("tlow", TEMP_LOW);
  preferences.putFloat("thigh", TEMP_HIGH);
  preferences.putFloat("hlow", HUM_LOW);
  preferences.putFloat("hhigh", HUM_HIGH);

  dailyReportEnabled = server.arg("dailyReport") == "1";
  preferences.putBool("dailyReport", dailyReportEnabled);

  server.send(200, "application/json", "{\"success\":true}");
}

void handleTestSMS() {
  sendSMS(phoneNumbers[0], "✅ Test SMS from ESP32");
  server.send(200, "text/plain", "OK");
}

void handleTestCall() {
  makeDirectCall(phoneNumbers[0]);
  server.send(200, "text/plain", "OK");
}

// ================== DISPLAY UTILITIES ==================
void drawRoundedCard(int x, int y, int w, int h, uint16_t bgColor, uint16_t borderColor) {
  tft.fillRoundRect(x, y, w, h, 6, bgColor);
  tft.drawRoundRect(x, y, w, h, 6, borderColor);
}

void drawSensorCard(int x, int y, int w, int h, const char* label, String value, 
                    const char* unit, uint16_t valueColor, bool alert) {
  uint16_t bgColor = alert ? 0x2000 : 0x1082;
  uint16_t borderColor = alert ? ST77XX_RED : 0x4208;
  drawRoundedCard(x, y, w, h, bgColor, borderColor);
  
  tft.setTextSize(1);
  tft.setTextColor(0x8410);
  tft.setCursor(x + 6, y + 6);
  tft.print(label);
  
  tft.setTextSize(3);
  tft.setTextColor(valueColor);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(value.c_str(), 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + (w - tw) / 2, y + h/2 - 8);
  tft.print(value);
  
  tft.setTextSize(1);
  tft.setTextColor(0xC618);
  tft.getTextBounds(unit, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + w - tw - 6, y + h - th - 6);
  tft.print(unit);
}

void drawStatusBar(int y, const char* text, uint16_t bgColor, uint16_t textColor) {
  tft.fillRect(0, y, 240, 32, bgColor);
  tft.setTextSize(2);
  tft.setTextColor(textColor);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((240 - tw) / 2, y + 8);
  tft.print(text);
  
  if (callState == CALL_DIALING || callState == CALL_RINGING) {
    tft.fillCircle(15, y + 16, 5, ST77XX_ORANGE);
    tft.fillCircle(225, y + 16, 5, ST77XX_ORANGE);
  } else if (callState == CALL_CONNECTED) {
    tft.fillCircle(15, y + 16, 5, ST77XX_GREEN);
    tft.fillCircle(225, y + 16, 5, ST77XX_GREEN);
  }
}

void updateDisplay(float t, float h, int gas, int nh3, int flame) {
  tft.fillScreen(0x0000);
  
  tft.fillRect(0, 0, 240, 28, 0x0349);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(20, 6);
  tft.print("ENVIRONMENT");
  
  uint16_t indicatorColor = ST77XX_GREEN;
  if (callState == CALL_DIALING || callState == CALL_RINGING) {
    indicatorColor = ST77XX_ORANGE;
  } else if (callState == CALL_CONNECTED) {
    indicatorColor = ST77XX_CYAN;
  } else if (callAttempts > 0) {
    indicatorColor = ST77XX_YELLOW;
  }
  tft.fillCircle(220, 14, 5, indicatorColor);
  
  bool tempAlert = (t < TEMP_LOW || t > TEMP_HIGH);
  String tempStr = String(t, 1);
  drawSensorCard(5, 35, 110, 70, "TEMPERATURE", tempStr, "C", 
                 tempAlert ? ST77XX_RED : ST77XX_CYAN, tempAlert);
  
  bool humAlert = (h < HUM_LOW || h > HUM_HIGH);
  String humStr = String(h, 0);
  drawSensorCard(125, 35, 110, 70, "HUMIDITY", humStr, "%", 
                 humAlert ? ST77XX_RED : ST77XX_CYAN, humAlert);
  
  bool gasAlert = gas > GAS_LIMIT;
  String gasStr = String(gas);
  drawSensorCard(5, 112, 110, 70, "GAS LEVEL", gasStr, "PPM", 
                 gasAlert ? ST77XX_RED : ST77XX_GREEN, gasAlert);
  
  bool nh3Alert = nh3 > AMMONIA_LIMIT;
  String nh3Str = String(nh3);
  drawSensorCard(125, 112, 110, 70, "AMMONIA", nh3Str, "PPM", 
                 nh3Alert ? ST77XX_RED : ST77XX_GREEN, nh3Alert);
  
  if (flame == LOW) {
    drawStatusBar(189, "! FIRE DETECTED !", ST77XX_RED, ST77XX_WHITE);
    tft.fillCircle(15, 205, 6, ST77XX_YELLOW);
    tft.fillCircle(225, 205, 6, ST77XX_YELLOW);
  }
  else if (tempAlert || gasAlert || nh3Alert || humAlert) {
    if (callState == CALL_CONNECTED) {
      drawStatusBar(189, "CALL CONNECTED", ST77XX_GREEN, ST77XX_WHITE);
    } else if (callState == CALL_RINGING) {
      drawStatusBar(189, "CALLING...", 0xFD20, ST77XX_WHITE);
    } else if (callAttempts > 0) {
      drawStatusBar(189, "ALERT - CALLING", ST77XX_RED, ST77XX_WHITE);
    } else {
      drawStatusBar(189, "ALERT ACTIVE", 0xF800, ST77XX_WHITE);
    }
    tft.drawTriangle(15, 211, 20, 199, 25, 211, ST77XX_YELLOW);
    tft.drawTriangle(215, 211, 220, 199, 225, 211, ST77XX_YELLOW);
  }
  else {
    drawStatusBar(189, "ALL SYSTEMS OK", 0x0560, ST77XX_WHITE);
    tft.fillCircle(15, 205, 4, ST77XX_GREEN);
    tft.fillCircle(225, 205, 4, ST77XX_GREEN);
  }
  
  tft.drawFastHLine(0, 28, 240, 0x4208);
  tft.drawFastHLine(0, 188, 240, 0x4208);
}

// ================== SETUP ==================
void setup() {
  
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== ENVIRONMENT MONITOR STARTING ===");
  
  pinMode(FLAME_PIN, INPUT);
  dht.begin();
  Serial.println("✓ Sensors initialized");
  
  Serial.println("Initializing display...");
  tft.init(240, 280);
  tft.setRotation(1);
  tft.setAddrWindow(X_OFFSET, 0, 240 + X_OFFSET, 240);
  tft.fillScreen(0x0000);
  displayReady = true;
  Serial.println("✓ Display ready");
  
  if (DISPLAY_TEST_MODE) {
    tft.fillScreen(ST77XX_RED);
    tft.setTextSize(3);
    tft.setTextColor(ST77XX_WHITE);
    tft.setCursor(30, 90);
    tft.print("DISPLAY");
    tft.setCursor(55, 120);
    tft.print("TEST");
    while(1) delay(1000);
  }
  
  tft.fillScreen(0x0349);
  tft.setTextSize(3);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(35, 90);
  tft.print("ENV");
  tft.setCursor(20, 120);
  tft.print("MONITOR");
  
  for (int i = 0; i < 200; i += 10) {
    tft.fillRect(20 + i, 165, 8, 6, ST77XX_GREEN);
    delay(50);
  }
  
  tft.fillScreen(0x0000);

  // Load preferences
  preferences.begin("envmonitor", false);
 

  for (int i = 0; i < MAX_CONTACTS; i++) {
  String key = "phone" + String(i);
  phoneNumbers[i] = preferences.getString(key.c_str(), phoneNumbers[i]);
}

 dailyReportEnabled = preferences.getBool("dailyReport", false);


  TEMP_LOW = preferences.getFloat("tlow", 10.0);
  TEMP_HIGH = preferences.getFloat("thigh", 35.0);
  HUM_LOW = preferences.getFloat("hlow", 30.0);
  HUM_HIGH = preferences.getFloat("hhigh", 80.0);
  Serial.println("✓ Preferences loaded");
  resetDailyStats();

  
  // Start WiFi AP
  Serial.println("Starting WiFi Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("✓ AP IP address: ");
  Serial.println(IP);
  
  // Configure web server routes
  server.on("/", handleRoot);

server.on("/getSettings", handleGetSettings);
server.on("/setSettings", HTTP_POST, handleSetSettings);
server.on("/testSMS", HTTP_POST, handleTestSMS);
server.on("/testCall", HTTP_POST, handleTestCall);

server.begin();
Serial.println("✓ Web server started");

  
  // Display WiFi info on screen
  tft.fillScreen(0x0000);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.setCursor(20, 40);
  tft.print("WiFi Ready");
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(10, 70);
  tft.print("SSID: ");
  tft.print(AP_SSID);
  tft.setCursor(10, 85);
  tft.print("Password: ");
  tft.print(AP_PASSWORD);
  tft.setCursor(10, 100);
  tft.print("IP: ");
  tft.print(IP);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(10, 130);
  tft.print("Connect to configure");
  tft.setCursor(10, 145);
  tft.print("thresholds & phone");
  delay(5000);
  
  // Initialize A7670 modem
  Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  powerOnModem();
  delay(5000);
  initModem();
  Serial.println("✓ Modem initialized");
  
  // Display ready message
  tft.fillScreen(0x0000);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(30, 100);
  tft.print("SYSTEM READY");
  delay(2000);
  
  Serial.println("\n=== MONITORING ACTIVE ===\n");
}

float getSensorResistance(int adcValue, float RL) {
  float voltage = (adcValue / ADC_MAX) * ADC_VREF;
  if (voltage <= 0.01) voltage = 0.01;
  return ((ADC_VREF - voltage) * RL) / voltage;
}

// MQ-2 → GAS (Approx PPM)
int getGasPPM(int adc) {
  // Linear approximation from working raw ADC
  return constrain(map(adc, 300, 3800, 0, 5000), 0, 5000);
}

int getNH3PPM(int adc) {
  // Linear approximation for MQ-137
  return constrain(map(adc, 300, 3800, 0, 300), 0, 300);
}

int smoothValue(int *buffer, int newValue) {
  buffer[gasIndex] = newValue;

  int sum = 0;
  int count = filterFilled ? GAS_FILTER_SIZE : gasIndex + 1;

  for (int i = 0; i < count; i++) {
    sum += buffer[i];
  }

  return sum / count;
}


// ================== MAIN LOOP ==================
void loop() {
  // Handle web server requests
  server.handleClient();
  
  // Update display at regular intervals
  if (displayReady && (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL)) {
    lastDisplayUpdate = millis();
    



    // Read sensors
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    int gasADC = analogRead(MQ_GAS_PIN);
    int nh3ADC = analogRead(MQ137_PIN);

    int gasRaw = getGasPPM(gasADC);
    int nh3Raw = getNH3PPM(nh3ADC);

// Moving average smoothing
    int gasValue = smoothValue(gasSamples, gasRaw);
    int nh3Value = smoothValue(nh3Samples, nh3Raw);

// Advance filter index
gasIndex++;
if (gasIndex >= GAS_FILTER_SIZE) {
  gasIndex = 0;
  filterFilled = true;
}

    int flameValue = digitalRead(FLAME_PIN);
    

    if (isnan(temperature) || temperature < 0 || temperature > 60) temperature = lastValidTemp;
    if (isnan(humidity) || humidity < 0 || humidity > 100) humidity = lastValidHum;

lastValidTemp = temperature;
lastValidHum  = humidity;


    updateDailyStats(temperature, humidity);
    checkDailyReport();

    
    // Update display
    updateDisplay(temperature, humidity, gasValue, nh3Value, flameValue);
    
    // Print to serial
    Serial.println("--- Sensor Readings ---");
    Serial.print("Temperature: "); Serial.print(temperature); Serial.println(" °C");
    Serial.print("Humidity: "); Serial.print(humidity); Serial.println(" %");
    Serial.print("Gas: "); Serial.print(gasValue); Serial.println(" PPM");
    Serial.print("Ammonia: "); Serial.print(nh3Value); Serial.println(" PPM");
    Serial.print("Flame: "); Serial.println(flameValue == LOW ? "DETECTED" : "None");
    Serial.println();
    
    // Handle alerts (calls)
    handleAlerts(temperature, humidity, gasValue, nh3Value, flameValue == LOW);
    
    // Send SMS if threshold exceeded and cooldown passed
    bool alertCondition = 
      (flameValue == LOW) ||
      (temperature < TEMP_LOW || temperature > TEMP_HIGH) ||
      (humidity < HUM_LOW || humidity > HUM_HIGH) ||
      (gasValue > GAS_LIMIT) ||
      (nh3Value > AMMONIA_LIMIT);
    
 // ===== ONE-TIME SMS PER ALERT =====
if (alertCondition && !lastAlertState) {
  Serial.println("🚨 ALERT STARTED → Sending SMS");
  sendParametersSMS(temperature, humidity, gasValue, nh3Value, flameValue == LOW);
  smsSentForCurrentAlert = true;
}

if (!alertCondition && lastAlertState) {
  Serial.println("✅ ALERT CLEARED");
  smsSentForCurrentAlert = false;
}

lastAlertState = alertCondition;

  }
  
  // Small delay to prevent watchdog issues
  delay(10);
}