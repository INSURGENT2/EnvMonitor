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

bool dailyReportEnabled = true;
bool buzzerEnabled = true;
float lastValidTemp = 25.0;
float lastValidHum  = 50.0;

// ================== BUZZER ==================
#define BUZZER_PIN 25
bool buzzerActive = false;
unsigned long buzzerLastToggle = 0;
const unsigned long BUZZER_ALERT_INTERVAL = 500;
const unsigned long BUZZER_CALL_INTERVAL  = 200;

// ================== POWER DETECTION ==================
#define POWER_DETECT_PIN  32
bool mainsPowerOK    = true;
bool powerAlertSent  = false;

// Cached sensor values for power SMS (updated every loop)
volatile float pwrTemp = 25.0;
volatile float pwrHum  = 50.0;
volatile int   pwrGas  = 0;
volatile int   pwrNH3  = 0;

volatile bool powerStateChanged = false;
volatile bool powerPinHigh      = true;

// 3-minute debounce for power failure
unsigned long powerFailureDetectedAt = 0;
bool          powerFailurePending    = false;
const unsigned long POWER_FAILURE_DELAY = 180000UL;  // 3 minutes

void IRAM_ATTR powerISR() {
  powerPinHigh      = (digitalRead(POWER_DETECT_PIN) == HIGH);
  powerStateChanged = true;
}

// ================== ALERT STATE FLAGS ==================
bool smsSentForCurrentAlert = false;
bool lastAlertState = false;
int  activeContacts = 0;
bool modemReady     = false;

// ================== MULTI CONTACT SUPPORT ==================
#define MAX_CONTACTS           5
#define MAX_ATTEMPTS_PER_NUMBER 2

String activePhoneList[MAX_CONTACTS];
String phoneNumbers[MAX_CONTACTS] = {
  "+918010845905",
  "+911111111111",
  "+922222222222",
  "+933333333333",
  "+944444444444"
};

int  currentContactIndex      = 0;
int  attemptsForCurrentNumber = 0;
bool alertAcknowledged        = false;

// ================== TIME MANAGEMENT ==================
struct NetworkTime {
  int hour, minute, second, day, month, year;
  bool valid;
};

NetworkTime currentTime       = {0, 0, 0, 1, 1, 2025, false};
unsigned long lastTimeUpdate  = 0;
unsigned long lastMillisAtSync = 0;
const unsigned long TIME_UPDATE_INTERVAL = 3600000UL;

void tickSoftwareClock() {
  if (!currentTime.valid) return;
  unsigned long elapsed = (millis() - lastMillisAtSync) / 1000;
  if (elapsed == 0) return;
  lastMillisAtSync += elapsed * 1000;
  currentTime.second += elapsed;
  if (currentTime.second >= 60) { currentTime.minute += currentTime.second / 60; currentTime.second %= 60; }
  if (currentTime.minute >= 60) { currentTime.hour   += currentTime.minute / 60; currentTime.minute %= 60; }
  if (currentTime.hour   >= 24) { currentTime.day    += currentTime.hour   / 24; currentTime.hour   %= 24; }
}

// ================== DAILY STATS ==================
struct DailyStats {
  float minTemp, maxTemp, minHum, maxHum;
  bool valid;
};

DailyStats todayStats;
int  lastRecordedDay       = -1;
bool dailyReportSentToday  = false;

void resetDailyStats() {
  todayStats = {1000, -1000, 1000, -1000, true};
}

void updateDailyStats(float t, float h) {
  if (!todayStats.valid) resetDailyStats();
  todayStats.minTemp = min(todayStats.minTemp, t);
  todayStats.maxTemp = max(todayStats.maxTemp, t);
  todayStats.minHum  = min(todayStats.minHum,  h);
  todayStats.maxHum  = max(todayStats.maxHum,  h);
}

void updateActiveContacts() {
  activeContacts = 0;
  for (int i = 0; i < MAX_CONTACTS; i++) {
    if (phoneNumbers[i].length() >= 10)
      activePhoneList[activeContacts++] = phoneNumbers[i];
  }
}

// ===== FUNCTION PROTOTYPES =====
String sendATCommand(const char *cmd, uint32_t waitMs);
bool   sendSMS(String phoneNumber, String message);
bool   getInternetTime();
void   processModemURC();
void   powerOnModem();
void   initModem();
void   resetCallState();
void   handleAlerts(float temp, float hum, int gas, int nh3, bool powerFailure = false);

// ================== NTP TIME SYNC ==================
bool getInternetTime() {
  Serial.println("Syncing NTP time...");
  sendATCommand("AT+CGATT=1",  3000);
  sendATCommand("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 3000);
  sendATCommand("AT+CGACT=1,1", 5000);
  sendATCommand("AT+CNTP=\"time.google.com\",0", 2000);
  sendATCommand("AT+CNTP", 3000);
  Serial.println("Waiting for NTP response...");
  delay(10000);

  String resp = sendATCommand("AT+CCLK?", 2000);
  if (resp.indexOf("1970") >= 0) { Serial.println("Invalid time"); return false; }

  int q = resp.indexOf('"');
  if (q < 0) { Serial.println("Time parse error"); return false; }

  String ts = resp.substring(q + 1);
  int yr = ts.substring(0, 2).toInt();
  if (yr < 24) { Serial.println("Year invalid, rejecting"); return false; }  // reject default 70/01/01
  yr += 2000;
  int mo = ts.substring(3, 5).toInt();
  int dy = ts.substring(6, 8).toInt();
  int hr = ts.substring(9, 11).toInt();
  int mn = ts.substring(12, 14).toInt();
  int sc = ts.substring(15, 17).toInt();

  // UTC → IST (+5:30)
  mn += 30; if (mn >= 60) { mn -= 60; hr++; }
  hr += 5;  if (hr >= 24) { hr -= 24; dy++; }

  currentTime    = {hr, mn, sc, dy, mo, yr, true};
  lastMillisAtSync = millis();
  Serial.printf("IST Time: %02d/%02d/%04d %02d:%02d:%02d\n", dy, mo, yr, hr, mn, sc);
  return true;
}

// ================== DAILY REPORT ==================
void checkDailyReport(float curTemp, float curHum, int curGas, int curNH3) {
  if (!dailyReportEnabled) return;
  if (!currentTime.valid)  return;

  if (currentTime.day != lastRecordedDay) {
    dailyReportSentToday = false;
    lastRecordedDay = currentTime.day;
  }

  if (!dailyReportSentToday && currentTime.hour == 8) {
    dailyReportSentToday = true;

    String msg = "DAILY REPORT ";
    msg += String(currentTime.day) + "/" + String(currentTime.month) + "/" + String(currentTime.year) + "\r\n";
    msg += "Temp : "       + String(curTemp, 1) + "C (min " + String(todayStats.minTemp, 1) + " max " + String(todayStats.maxTemp, 1) + ")\r\n";
    msg += "Hum : "        + String((int)curHum) + "% (min " + String((int)todayStats.minHum) + " max " + String((int)todayStats.maxHum) + ")\r\n";
    msg += "Carbon Gas : " + String(curGas) + " PPM\r\n";
    msg += "Ammonia : "    + String(curNH3) + " PPM";

    Serial.println("Daily report length: " + String(msg.length()));
    for (int i = 0; i < activeContacts; i++) {
      sendSMS(activePhoneList[i], msg);
      delay(2000);
    }
    resetDailyStats();
  }
}

// ===== SENSOR CALIBRATION =====
#define ADC_MAX       4095.0
#define ADC_VREF      3.3
#define MQ2_RL        10.0
#define MQ2_R0        9.83
#define MQ137_RL      10.0
#define MQ137_R0      12.0
#define GAS_MAX_PPM   5000
#define NH3_MAX_PPM   300

// ================== DHT ==================
#define DHTPIN    2
#define DHTTYPE   DHT11
DHT dht(DHTPIN, DHTTYPE);
#define TEMP_OFFSET  -6.5f

// ================== SENSORS ==================
#define MQ_GAS_PIN  34
#define MQ137_PIN   35

// ================== TFT DISPLAY ==================
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

// ================== WiFi / Web ==================
const char* AP_SSID     = "EnvMonitor_Config";
const char* AP_PASSWORD = "12345678";
WebServer   server(80);
Preferences preferences;

String TO_PHONE_NUMBER = "+918010845905";

// ================== THRESHOLDS ==================
int   GAS_LIMIT    = 1800;
int   AMMONIA_LIMIT = 200;
float TEMP_LOW     = 10.0;
float TEMP_HIGH    = 35.0;
float HUM_LOW      = 30.0;
float HUM_HIGH     = 80.0;

// ================== TIMING ==================
unsigned long lastSMSTime       = 0;
const unsigned long SMS_INTERVAL = 30000;
unsigned long lastDisplayUpdate  = 0;
const unsigned long DISPLAY_INTERVAL = 2000;
bool displayReady = false;

// ================== CALL STATE ==================
enum CallState { CALL_IDLE, CALL_DIALING, CALL_RINGING, CALL_CONNECTED, CALL_FAILED };
CallState callState = CALL_IDLE;

unsigned long callStartTime   = 0;
unsigned long lastCallAttempt = 0;
int  callAttempts             = 0;
const int MAX_CALL_ATTEMPTS   = 5;
const unsigned long CALL_TIMEOUT  = 45000;
const unsigned long RETRY_DELAY   = 3000;
const unsigned long ALERT_COOLDOWN = 300000;
unsigned long alertCooldownStart  = 0;

String currentAlertType = "";
bool   callInProgress   = false;

// ================== BUZZER UPDATE ==================
void buzzerUpdate(bool alertActive) {
  if (!buzzerEnabled) {
    if (buzzerActive) { buzzerActive = false; digitalWrite(BUZZER_PIN, LOW); }
    return;
  }
  if (callState == CALL_CONNECTED && buzzerActive) {
    buzzerActive = false; digitalWrite(BUZZER_PIN, LOW); return;
  }
  if (alertActive) {
    if (!buzzerActive) { buzzerActive = true; buzzerLastToggle = millis(); digitalWrite(BUZZER_PIN, HIGH); }
    unsigned long interval = (callState == CALL_DIALING || callState == CALL_RINGING)
                             ? BUZZER_CALL_INTERVAL : BUZZER_ALERT_INTERVAL;
    if (millis() - buzzerLastToggle >= interval) {
      buzzerLastToggle = millis();
      digitalWrite(BUZZER_PIN, !digitalRead(BUZZER_PIN));
    }
  } else {
    if (buzzerActive) { buzzerActive = false; digitalWrite(BUZZER_PIN, LOW); }
  }
}

// ================== WEB SERVER HTML ==================
const char CONFIG_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Environment Monitor Config</title>
  <style>
    body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px;
           background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; }
    .container { background: white; padding: 30px; border-radius: 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.3); }
    h1 { color: #333; text-align: center; margin-bottom: 10px; }
    h2 { color: #667eea; font-size: 18px; margin-top: 25px; margin-bottom: 15px;
         border-bottom: 2px solid #667eea; padding-bottom: 5px; }
    .form-group { margin-bottom: 20px; }
    label { display: block; margin-bottom: 8px; color: #555; font-weight: bold; font-size: 14px; }
    input { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 8px;
            font-size: 16px; box-sizing: border-box; margin-bottom: 8px; }
    .hint { font-size: 12px; color: #888; }
    .range-inputs { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }
    button { width: 100%; padding: 14px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
             color: white; border: none; border-radius: 8px; font-size: 16px;
             font-weight: bold; cursor: pointer; margin-top: 10px; }
    .current-settings { background: #f0f0f0; padding: 15px; border-radius: 8px; margin-bottom: 25px; }
    .success { background: #d4edda; color: #155724; padding: 12px; border-radius: 8px;
               margin-top: 15px; display: none; text-align: center; }
    .test-btn { background: linear-gradient(135deg, #f093fb 0%, #f5576c 100%); }
  </style>
</head>
<body>
<div class="container">
<h1>Environment Monitor</h1>
<p style="text-align:center;color:#888;font-size:14px;">Configuration Panel</p>
<div class="current-settings">
  <h2 style="margin-top:0;border:none;">Current Settings</h2>
  <div><strong>Contacts:</strong> <span id="displayPhone">Loading...</span></div>
  <div><strong>Daily Report:</strong> Enabled (8:00 AM)</div>
  <div><strong>Temperature:</strong> <span id="displayTemp">Loading...</span> °C</div>
  <div><strong>Humidity:</strong> <span id="displayHum">Loading...</span> %</div>
  <div><strong>CO Limit:</strong> <span id="displayGas">Loading...</span> PPM</div>
  <div><strong>NH3 Limit:</strong> <span id="displayNH3">Loading...</span> PPM</div>
  <div><strong>Buzzer:</strong> <span id="displayBuzzer">Loading...</span></div>
</div>
<form id="configForm">
<h2>Emergency Contacts (Call Order)</h2>
<div class="form-group"><input type="tel" id="phone0" placeholder="+91XXXXXXXXXX" required></div>
<div class="form-group"><input type="tel" id="phone1" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone2" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone3" placeholder="+91XXXXXXXXXX"></div>
<div class="form-group"><input type="tel" id="phone4" placeholder="+91XXXXXXXXXX"></div>
<p class="hint">System will call each number twice in order until someone answers.</p>
<h2>Temperature Limits</h2>
<div class="form-group">
  <div class="range-inputs">
    <input type="number" step="0.1" id="tlow"  placeholder="Min (e.g., 10)" required>
    <input type="number" step="0.1" id="thigh" placeholder="Max (e.g., 35)" required>
  </div>
</div>
<h2>Humidity Limits</h2>
<div class="form-group">
  <div class="range-inputs">
    <input type="number" step="0.1" id="hlow"  placeholder="Min (e.g., 30)" required>
    <input type="number" step="0.1" id="hhigh" placeholder="Max (e.g., 80)" required>
  </div>
</div>
<h2>Gas Thresholds</h2>
<div class="form-group">
  <label>Carbon Monoxide Limit (PPM)</label>
  <input type="number" id="gasLimit" placeholder="e.g., 1800" required>
</div>
<div class="form-group">
  <label>Ammonia Limit (PPM)</label>
  <input type="number" id="nh3Limit" placeholder="e.g., 200" required>
</div>
<h2>Alert Settings</h2>
<div class="form-group">
  <label style="display:flex;align-items:center;cursor:pointer;">
    <input type="checkbox" id="buzzerEnabled" style="width:auto;margin-right:10px;">
    <span>Enable Buzzer Alerts</span>
  </label>
  <p class="hint">Uncheck to disable buzzer (calls and SMS will still work)</p>
</div>
<button type="submit">Save All Settings</button>
<button type="button" class="test-btn" onclick="testSMS()">Test SMS</button>
<button type="button" class="test-btn" onclick="testCall()">Test Call</button>
</form>
<div class="success" id="successMsg">Settings saved successfully!</div>
</div>
<script>
function loadSettings() {
  fetch('/getSettings').then(r => r.json()).then(data => {
    let contacts = [];
    for (let i = 0; i < 5; i++) {
      const key = "phone" + i;
      document.getElementById(key).value = data[key] || "";
      if (data[key]) contacts.push(data[key]);
    }
    displayPhone.textContent = contacts.length ? contacts.join(', ') : 'Not configured';
    document.getElementById('tlow').value  = data.tlow  ?? '';
    document.getElementById('thigh').value = data.thigh ?? '';
    document.getElementById('hlow').value  = data.hlow  ?? '';
    document.getElementById('hhigh').value = data.hhigh ?? '';
    document.getElementById('gasLimit').value = data.gasLimit ?? '';
    document.getElementById('nh3Limit').value = data.nh3Limit ?? '';
    document.getElementById('buzzerEnabled').checked = data.buzzerEnabled !== false;
    displayTemp.textContent = (data.tlow !== undefined && data.thigh !== undefined) ? data.tlow + ' to ' + data.thigh : 'Not configured';
    displayHum.textContent  = (data.hlow !== undefined && data.hhigh !== undefined) ? data.hlow + ' to ' + data.hhigh : 'Not configured';
    displayGas.textContent  = data.gasLimit !== undefined ? data.gasLimit : 'Not configured';
    displayNH3.textContent  = data.nh3Limit !== undefined ? data.nh3Limit : 'Not configured';
    displayBuzzer.textContent = (data.buzzerEnabled !== false) ? 'Enabled' : 'Disabled';
  });
}
loadSettings();
document.getElementById('configForm').addEventListener('submit', e => {
  e.preventDefault();
  let data = '';
  for (let i = 0; i < 5; i++)
    data += 'phone' + i + '=' + encodeURIComponent(document.getElementById('phone' + i).value) + '&';
  data += 'tlow=' + tlow.value + '&thigh=' + thigh.value +
          '&hlow=' + hlow.value + '&hhigh=' + hhigh.value +
          '&gasLimit=' + gasLimit.value + '&nh3Limit=' + nh3Limit.value +
          '&buzzerEnabled=' + (buzzerEnabled.checked ? '1' : '0');
  fetch('/setSettings', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body: data })
    .then(() => { successMsg.style.display='block'; setTimeout(()=>successMsg.style.display='none',3000); loadSettings(); });
});
function testSMS()  { fetch('/testSMS',  {method:'POST'}).then(()=>alert('Test SMS sent!')); }
function testCall() { fetch('/testCall', {method:'POST'}).then(()=>alert('Test call initiated!')); }
</script>
</body>
</html>
)rawliteral";

// ================== BUZZER INIT ==================
void buzzerInit() {
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
}

// ================== POWER DETECT INIT ==================
void initPowerDetect() {
  pinMode(POWER_DETECT_PIN, INPUT);
  mainsPowerOK = (digitalRead(POWER_DETECT_PIN) == HIGH);
  powerPinHigh = mainsPowerOK;
  attachInterrupt(digitalPinToInterrupt(POWER_DETECT_PIN), powerISR, CHANGE);
  Serial.printf("Power detect GPIO%d — %s\n", POWER_DETECT_PIN, mainsPowerOK ? "MAINS ON" : "NO POWER");
}

// ================== POWER SMS ==================
void sendPowerFailureSMS(float temp, float hum, int gas, int nh3) {
  String msg = "POWER FAILURE!\r\n";
  msg += "Main supply disconnected\r\n";
  msg += "Running on battery backup\r\n";
  msg += "Temp: "       + String(temp, 1)  + "C\r\n";
  msg += "Humidity: "   + String((int)hum) + "%\r\n";
  msg += "Carbon Gas: " + String(gas)      + " PPM\r\n";
  msg += "Ammonia: "    + String(nh3)      + " PPM";
  for (int i = 0; i < activeContacts; i++) {
    Serial.println("Power failure SMS -> " + activePhoneList[i]);
    sendSMS(activePhoneList[i], msg);
    delay(2000);
  }
  if (buzzerEnabled) {
    digitalWrite(BUZZER_PIN, HIGH); delay(800);
    digitalWrite(BUZZER_PIN, LOW);  delay(400);
    digitalWrite(BUZZER_PIN, HIGH); delay(800);
    digitalWrite(BUZZER_PIN, LOW);
  }
}

void sendPowerRestoredSMS(float temp, float hum, int gas, int nh3) {
  String msg = "POWER RESTORED\r\n";
  msg += "Mains supply reconnected\r\n";
  msg += "Temp: "       + String(temp, 1)  + "C\r\n";
  msg += "Humidity: "   + String((int)hum) + "%\r\n";
  msg += "Carbon Gas: " + String(gas)      + " PPM\r\n";
  msg += "Ammonia: "    + String(nh3)      + " PPM";
  for (int i = 0; i < activeContacts; i++) {
    Serial.println("Power restored SMS -> " + activePhoneList[i]);
    sendSMS(activePhoneList[i], msg);
    delay(2000);
  }
}

// ================== POWER STATE HANDLER ==================
// ISR fires instantly. This handler only starts the 3-minute timer.
// Actual alert fires from loop() after the timer expires.
void handlePowerStateChange() {
  if (!powerStateChanged) return;
  bool newState = powerPinHigh;

  if (!newState && mainsPowerOK && !powerFailurePending) {
    // Power just dropped — start 3-minute timer
    powerFailurePending      = true;
    powerFailureDetectedAt   = millis();
    powerStateChanged        = false;
    Serial.println("Power drop detected — waiting 3 min before alert");
    return;
  }

  if (newState && powerFailurePending && mainsPowerOK) {
    // Power restored within 3-minute window — cancel, it was a blip
    powerFailurePending = false;
    powerStateChanged   = false;
    Serial.println("Power blip — restored before 3 min, no alert");
    return;
  }

  if (newState && !mainsPowerOK) {
    // Power restored after alert had already fired
    powerFailurePending = false;
    mainsPowerOK        = true;
    powerStateChanged   = false;
    Serial.println("POWER RESTORED");
    sendPowerRestoredSMS(pwrTemp, pwrHum, pwrGas, pwrNH3);
    powerAlertSent = false;
    resetCallState();
    return;
  }

  powerStateChanged = false;
}

// ================== A7670 MODEM ==================
void powerOnModem() {
  Serial.println("Powering on A7670 modem...");
  pinMode(MODEM_POWERON, OUTPUT); digitalWrite(MODEM_POWERON, HIGH);
  pinMode(MODEM_RESET,   OUTPUT); digitalWrite(MODEM_RESET,   LOW);
  delay(100);
  digitalWrite(MODEM_RESET, HIGH);
  delay(2000);
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, HIGH); delay(1000);
  digitalWrite(MODEM_PWRKEY, LOW);
  Serial.println("Modem powered on.");
}

void sendAT(const char *cmd, uint32_t waitMs = 1000) {
  Serial.print("AT CMD: "); Serial.println(cmd);
  Serial1.flush();
  Serial1.println(cmd);
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    if (Serial1.available()) Serial.write(Serial1.read());
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
  Serial.print("AT CMD: "); Serial.println(cmd);
  Serial1.println(cmd);
  unsigned long start = millis();
  String response = "";
  while (millis() - start < waitMs) {
    while (Serial1.available()) {
      char c = Serial1.read();
      if (c >= 32 && c <= 126) { response += c; Serial.write(c); }
    }
  }
  return response;
}

void initModem() {
  Serial.println("Initializing modem...");
  sendAT("AT", 500);
  sendAT("AT+CFUN=1", 2000); delay(2000);
  sendAT("AT+CGDCONT=1,\"IP\",\"airtelgprs.com\"", 1000); delay(500);
  sendAT("AT+CGACT=1,1", 2000); delay(500);
  sendAT("AT+CRC=1",  500);
  sendAT("AT+CLIP=1", 500);
  sendAT("AT+CGATT=1", 2000); delay(1000);
  sendAT("AT+CVOLTE=1", 1000); delay(500);
  sendAT("AT+QCFG=\"ims\",1", 1000); delay(500);
  sendAT("AT+CNMP=2", 1000);

  Serial.println("Waiting for network...");
  bool registered = false;
  for (int i = 0; i < 60; i++) {
    String resp = sendATCommand("AT+CREG?", 1000);
    if (resp.indexOf("+CREG: 0,1") >= 0 ||
        resp.indexOf("+CREG: 0,5") >= 0 ||
        resp.indexOf("+CREG: 0,6") >= 0) {
      Serial.println("\nNetwork registered!");
      registered = true; break;
    }
    Serial.print("."); delay(1000);
  }
  if (!registered) Serial.println("\nWarning: Network registration incomplete");

  Serial.println("Signal: "   + sendATCommand("AT+CSQ",      1000));
  Serial.println("Operator: " + sendATCommand("AT+COPS?",     2000));
  Serial.println("APN: "      + sendATCommand("AT+CGDCONT?",  1000));

  sendAT("AT+CMGF=1", 500);
  sendAT("AT+CSCS=\"GSM\"", 500);
  sendAT("AT+CLIP=1", 500);
  sendAT("AT+CLCC=1", 500);
  Serial.println("Modem ready.");
}

bool sendSMS(String phoneNumber, String message) {
  Serial.println("Sending SMS to: " + phoneNumber);
  Serial.println("Message length: " + String(message.length()));

  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+CMGF=1"); delay(500);
  while (Serial1.available()) Serial1.read();
  Serial1.println("AT+CSCS=\"GSM\""); delay(300);
  while (Serial1.available()) Serial1.read();

  Serial1.println("AT+CMGS=\"" + phoneNumber + "\"");
  delay(1000);

  unsigned long promptStart = millis();
  bool gotPrompt = false;
  while (millis() - promptStart < 3000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      Serial.write(c);
      if (c == '>') { gotPrompt = true; break; }
    }
  }
  if (!gotPrompt) { Serial.println("ERROR: No > prompt"); return false; }

  Serial1.print(message);
  delay(100);
  Serial1.write(26);
  Serial1.flush();

  unsigned long start = millis();
  String response = "";
  bool success = false;
  while (millis() - start < 15000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      response += c;
      Serial.write(c);
      if (response.indexOf("+CMGS:") >= 0) success = true;
      if (response.indexOf("OK") >= 0 && success) { Serial.println("\nSMS sent!"); return true; }
      if (response.indexOf("ERROR") >= 0 || response.indexOf("+CMS ERROR") >= 0) {
        Serial.println("\nSMS failed"); return false;
      }
    }
  }
  Serial.println("\nSMS timeout!"); return false;
}

bool makeDirectCall(String phoneNumber) {
  String creg = sendATCommand("AT+CREG?", 1000);
  if (creg.indexOf("+CREG: 0,1") < 0 &&
      creg.indexOf("+CREG: 0,5") < 0 &&
      creg.indexOf("+CREG: 0,6") < 0) {
    Serial.println("No network - cannot call. Status: " + creg); return false;
  }
  sendAT("ATH", 1500); delay(800);
  String response = sendATCommand(("ATD" + phoneNumber + ";").c_str(), 3000);
  if (response.indexOf("OK") >= 0) {
    callInProgress = true;
    callStartTime  = millis();
    callState      = CALL_DIALING;
    Serial.println("Call initiated"); return true;
  }
  Serial.println("Call failed: " + response); return false;
}

void checkCallStatus() {
  if (!callInProgress) return;
  String response = filterASCII(sendATCommand("AT+CLCC", 1500));
  int idx = response.indexOf("+CLCC:");

  if (idx < 0) {
    if (millis() - callStartTime > 10000) { Serial.println("Call declined/no answer"); hangupCall(); return; }
    if (millis() - callStartTime > CALL_TIMEOUT) { Serial.println("Call timeout"); hangupCall(); }
    return;
  }

  int p2 = response.indexOf(',', idx);
  p2 = response.indexOf(',', p2 + 1);
  int p3 = response.indexOf(',', p2 + 1);
  if (p2 < 0 || p3 < 0) return;
  int stat = response.substring(p2 + 1, p3).toInt();

  switch (stat) {
    case 0:
      Serial.println("CALL ANSWERED - ALERT ACKNOWLEDGED");
      alertAcknowledged = true; callState = CALL_CONNECTED;
      delay(5000); hangupCall(); break;
    case 3: callState = CALL_RINGING; break;
    case 6: Serial.println("Call rejected/busy"); hangupCall(); break;
  }
}

void resetCallState() {
  currentContactIndex      = 0;
  attemptsForCurrentNumber = 0;
  callInProgress           = false;
  alertAcknowledged        = false;
  callState                = CALL_IDLE;
}

// ================== ALERT SMS ==================
void sendAlertSMS(String phone, float temp, float hum, int gas, int nh3, bool powerFailure) {
  String msg = "ALERT\n";
  msg += "Temperature: " + String(temp, 1) + "C\n";
  msg += "Humidity: "    + String((int)hum) + "%\n";
  msg += "Carbon gas: "  + String(gas) + "\n";
  msg += "Ammonia: "     + String(nh3) + "\n";
  msg += "Reason:";
  if (powerFailure)         msg += " Power failure";
  if (temp < TEMP_LOW)      msg += " Temp below "     + String((int)TEMP_LOW);
  if (temp > TEMP_HIGH)     msg += " Temp above "     + String((int)TEMP_HIGH);
  if (hum  < HUM_LOW)       msg += " Humidity below " + String((int)HUM_LOW);
  if (hum  > HUM_HIGH)      msg += " Humidity above " + String((int)HUM_HIGH);
  if (gas  > GAS_LIMIT)     msg += " Carbon gas above " + String(GAS_LIMIT);
  if (nh3  > AMMONIA_LIMIT) msg += " Ammonia above "    + String(AMMONIA_LIMIT);
  sendSMS(phone, msg);
}

// ================== HANDLE ALERTS ==================
uint8_t lastAlertMask = 0;

void handleAlerts(float temp, float hum, int gas, int nh3, bool powerFailure) {

  // Suppress sensor alerts during 3-minute power debounce window
  if (powerFailurePending && !powerFailure) return;

  bool alertActive =
    powerFailure ||
    (temp < TEMP_LOW  || temp > TEMP_HIGH) ||
    (hum  < HUM_LOW   || hum  > HUM_HIGH)  ||
    (gas  > GAS_LIMIT) ||
    (nh3  > AMMONIA_LIMIT);

  // Build bitmask (powerFailure = bit 7, no fire bit)
  uint8_t currentMask = 0;
  if (powerFailure)         currentMask |= (1 << 7);
  if (temp < TEMP_LOW)      currentMask |= (1 << 1);
  if (temp > TEMP_HIGH)     currentMask |= (1 << 2);
  if (hum  < HUM_LOW)       currentMask |= (1 << 3);
  if (hum  > HUM_HIGH)      currentMask |= (1 << 4);
  if (gas  > GAS_LIMIT)     currentMask |= (1 << 5);
  if (nh3  > AMMONIA_LIMIT) currentMask |= (1 << 6);

  if (currentMask != lastAlertMask && currentMask != 0) {
    Serial.printf("Alert mask changed: 0x%02X -> 0x%02X, resetting\n", lastAlertMask, currentMask);
    resetCallState();
    smsSentForCurrentAlert = false;
  }
  lastAlertMask = currentMask;

  buzzerUpdate(alertActive);

  if (!alertActive) {
    resetCallState();
    smsSentForCurrentAlert = false;
    lastAlertMask = 0;
    return;
  }

  if (activeContacts == 0) return;
  if (alertAcknowledged)   return;

  if (callInProgress) { checkCallStatus(); return; }

  if (currentContactIndex >= activeContacts) {
    currentContactIndex = 0;
    Serial.println("All contacts tried, looping back");
  }

  if (millis() - lastCallAttempt < RETRY_DELAY) return;

  Serial.printf("Calling contact %d/%d (Attempt %d/2)\n",
    currentContactIndex + 1, activeContacts, attemptsForCurrentNumber + 1);

  sendAlertSMS(activePhoneList[currentContactIndex], temp, hum, gas, nh3, powerFailure);
  makeDirectCall(activePhoneList[currentContactIndex]);

  lastCallAttempt = millis();
  attemptsForCurrentNumber++;

  if (attemptsForCurrentNumber >= MAX_ATTEMPTS_PER_NUMBER) {
    attemptsForCurrentNumber = 0;
    currentContactIndex++;
  }
}

// ================== WEB SERVER HANDLERS ==================
void handleRoot() { server.send_P(200, "text/html", CONFIG_PAGE); }

void handleGetSettings() {
  String json = "{";
  for (int i = 0; i < MAX_CONTACTS; i++)
    json += "\"phone" + String(i) + "\":\"" + phoneNumbers[i] + "\",";
  json += "\"tlow\":"      + String(TEMP_LOW, 1)  + ",";
  json += "\"thigh\":"     + String(TEMP_HIGH, 1) + ",";
  json += "\"hlow\":"      + String(HUM_LOW, 1)   + ",";
  json += "\"hhigh\":"     + String(HUM_HIGH, 1)  + ",";
  json += "\"gasLimit\":"  + String(GAS_LIMIT)     + ",";
  json += "\"nh3Limit\":"  + String(AMMONIA_LIMIT) + ",";
  json += "\"buzzerEnabled\":" + String(buzzerEnabled ? "true" : "false") + ",";
  json += "\"dailyReport\":"   + String(dailyReportEnabled ? "true" : "false");
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetSettings() {
  for (int i = 0; i < MAX_CONTACTS; i++) {
    String key = "phone" + String(i);
    phoneNumbers[i] = server.arg(key);
    preferences.putString(key.c_str(), phoneNumbers[i]);
  }
  TEMP_LOW      = server.arg("tlow").toFloat();
  TEMP_HIGH     = server.arg("thigh").toFloat();
  HUM_LOW       = server.arg("hlow").toFloat();
  HUM_HIGH      = server.arg("hhigh").toFloat();
  GAS_LIMIT     = server.arg("gasLimit").toInt();
  AMMONIA_LIMIT = server.arg("nh3Limit").toInt();
  buzzerEnabled = (server.arg("buzzerEnabled") == "1");
  preferences.putFloat("tlow",  TEMP_LOW);
  preferences.putFloat("thigh", TEMP_HIGH);
  preferences.putFloat("hlow",  HUM_LOW);
  preferences.putFloat("hhigh", HUM_HIGH);
  preferences.putInt("gasLimit",  GAS_LIMIT);
  preferences.putInt("nh3Limit",  AMMONIA_LIMIT);
  preferences.putBool("buzzerEnabled", buzzerEnabled);
  updateActiveContacts();
  server.send(200, "application/json", "{\"success\":true}");
}

void handleTestSMS()  { sendSMS(phoneNumbers[0], "Test SMS from ESP32"); server.send(200, "text/plain", "OK"); }
void handleTestCall() { makeDirectCall(phoneNumbers[0]); server.send(200, "text/plain", "OK"); }

// ================== DISPLAY ==================
void drawRoundedCard(int x, int y, int w, int h, uint16_t bgColor, uint16_t borderColor) {
  tft.fillRoundRect(x, y, w, h, 6, bgColor);
  tft.drawRoundRect(x, y, w, h, 6, borderColor);
}

void drawSensorCard(int x, int y, int w, int h, const char* label, String value,
                    const char* unit, uint16_t valueColor, bool alert) {
  uint16_t bgColor     = alert ? 0x2000 : 0x1082;
  uint16_t borderColor = alert ? ST77XX_RED : 0x4208;
  drawRoundedCard(x, y, w, h, bgColor, borderColor);
  tft.setTextSize(1); tft.setTextColor(0x8410);
  tft.setCursor(x + 6, y + 6); tft.print(label);
  tft.setTextSize(3); tft.setTextColor(valueColor);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(value.c_str(), 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + (w - tw) / 2, y + h / 2 - 8); tft.print(value);
  tft.setTextSize(1); tft.setTextColor(0xC618);
  tft.getTextBounds(unit, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor(x + w - tw - 6, y + h - th - 6); tft.print(unit);
}

void drawStatusBar(int y, int h, const char* text, uint16_t bgColor, uint16_t textColor) {
  tft.fillRect(0, y, 280, h, bgColor);
  tft.setTextSize(2); tft.setTextColor(textColor);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds(text, 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((280 - tw) / 2, y + (h - th) / 2); tft.print(text);
  if (callState == CALL_DIALING || callState == CALL_RINGING) {
    tft.fillCircle(10, y + h / 2, 5, ST77XX_ORANGE);
    tft.fillCircle(270, y + h / 2, 5, ST77XX_ORANGE);
  } else if (callState == CALL_CONNECTED) {
    tft.fillCircle(10, y + h / 2, 5, ST77XX_GREEN);
    tft.fillCircle(270, y + h / 2, 5, ST77XX_GREEN);
  }
}

void updateDisplay(float t, float h, int gas, int nh3) {
  tft.fillScreen(0x0000);

  // Header
  tft.fillRect(0, 0, 280, 30, 0x0349);
  tft.setTextSize(2); tft.setTextColor(ST77XX_WHITE);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds("ENVIRONMENT", 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((280 - tw) / 2, 7); tft.print("ENVIRONMENT");

  uint16_t dotColor = ST77XX_GREEN;
  if      (callState == CALL_DIALING || callState == CALL_RINGING) dotColor = ST77XX_ORANGE;
  else if (callState == CALL_CONNECTED)                             dotColor = ST77XX_CYAN;
  else if (callAttempts > 0)                                        dotColor = ST77XX_YELLOW;
  tft.fillCircle(255, 15, 6, dotColor);
  tft.drawFastHLine(0, 30, 280, 0x4208);

  // Sensor cards row 1
  bool tempAlert = (t < TEMP_LOW || t > TEMP_HIGH);
  bool humAlert  = (h < HUM_LOW  || h > HUM_HIGH);
  drawSensorCard(2,   31, 136, 86, "TEMPERATURE", String(t, 1), "C",   tempAlert ? ST77XX_RED : ST77XX_CYAN, tempAlert);
  drawSensorCard(142, 31, 136, 86, "HUMIDITY",    String(h, 0),  "%",  humAlert  ? ST77XX_RED : ST77XX_CYAN, humAlert);
  tft.drawFastHLine(0, 117, 280, 0x4208);

  // Sensor cards row 2
  bool gasAlert = (gas > GAS_LIMIT);
  bool nh3Alert = (nh3 > AMMONIA_LIMIT);
  drawSensorCard(2,   118, 136, 86, "GAS (CO)", String(gas), "PPM", gasAlert ? ST77XX_RED : ST77XX_GREEN, gasAlert);
  drawSensorCard(142, 118, 136, 86, "AMMONIA",  String(nh3), "PPM", nh3Alert ? ST77XX_RED : ST77XX_GREEN, nh3Alert);
  tft.drawFastHLine(0, 204, 280, 0x4208);

  // Status bar
  if (!mainsPowerOK) {
    drawStatusBar(205, 35, "POWER FAILURE", 0xF800, ST77XX_WHITE);
  } else if (powerFailurePending) {
    drawStatusBar(205, 35, "POWER CHECK...", 0xFD20, ST77XX_WHITE);
  } else if (tempAlert || gasAlert || nh3Alert || humAlert) {
    if      (callState == CALL_CONNECTED) drawStatusBar(205, 35, "CALL CONNECTED",  ST77XX_GREEN, ST77XX_WHITE);
    else if (callState == CALL_RINGING)   drawStatusBar(205, 35, "CALLING...",       0xFD20,       ST77XX_WHITE);
    else if (callAttempts > 0)            drawStatusBar(205, 35, "ALERT - CALLING",  ST77XX_RED,   ST77XX_WHITE);
    else                                  drawStatusBar(205, 35, "ALERT ACTIVE",     0xF800,       ST77XX_WHITE);
  } else {
    drawStatusBar(205, 35, "ALL SYSTEMS OK", 0x0560, ST77XX_WHITE);
  }
}

// ================== SETUP ==================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== ENVIRONMENT MONITOR STARTING ===");

  dht.begin();
  buzzerInit();
  initPowerDetect();
  Serial.println("Sensors initialized");

  tft.init(240, 280);
  tft.setRotation(1);
  tft.setAddrWindow(0, 0, 280, 240);
  tft.fillScreen(ST77XX_BLACK);
  for (int y = 0; y < 240; y += 4) {
    tft.drawFastHLine(0, y, 280, ST77XX_CYAN); delay(5);
    tft.drawFastHLine(0, y, 280, ST77XX_BLACK);
  }
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(3); tft.setTextColor(ST77XX_CYAN);
  int16_t x1, y1; uint16_t tw, th;
  tft.getTextBounds("ENV",     0, 0, &x1, &y1, &tw, &th); tft.setCursor((280 - tw) / 2, 85);  tft.print("ENV");
  tft.getTextBounds("MONITOR", 0, 0, &x1, &y1, &tw, &th); tft.setCursor((280 - tw) / 2, 125); tft.print("MONITOR");
  delay(1200);
  displayReady = true;
  Serial.println("Display ready");

  preferences.begin("envmonitor", false);
  for (int i = 0; i < MAX_CONTACTS; i++) {
    String key = "phone" + String(i);
    phoneNumbers[i] = preferences.getString(key.c_str(), phoneNumbers[i]);
  }
  dailyReportEnabled = true;
  TEMP_LOW      = preferences.getFloat("tlow",  10.0);
  TEMP_HIGH     = preferences.getFloat("thigh", 35.0);
  HUM_LOW       = preferences.getFloat("hlow",  30.0);
  HUM_HIGH      = preferences.getFloat("hhigh", 80.0);
  GAS_LIMIT     = preferences.getInt("gasLimit",  1800);
  AMMONIA_LIMIT = preferences.getInt("nh3Limit",   200);
  buzzerEnabled = preferences.getBool("buzzerEnabled", true);
  updateActiveContacts();
  resetDailyStats();
  Serial.println("Preferences loaded");

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());
  server.on("/",           handleRoot);
  server.on("/getSettings", handleGetSettings);
  server.on("/setSettings", HTTP_POST, handleSetSettings);
  server.on("/testSMS",     HTTP_POST, handleTestSMS);
  server.on("/testCall",    HTTP_POST, handleTestCall);
  server.begin();
  Serial.println("Web server started");

  Serial1.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  powerOnModem();
  delay(5000);
  initModem();
  modemReady = true;
  Serial.println("Modem initialized");

  if (getInternetTime()) Serial.println("Time synced");
  else                   Serial.println("Time sync failed, will retry");

  Serial.println("=== MONITORING ACTIVE ===");
}

// ================== SENSOR HELPERS ==================
float getSensorResistance(int adcValue, float RL) {
  float voltage = (adcValue / ADC_MAX) * ADC_VREF;
  if (voltage <= 0.01) voltage = 0.01;
  return ((ADC_VREF - voltage) * RL) / voltage;
}
int getGasPPM(int adc) { return constrain(map(adc, 300, 3800, 0, 5000), 0, 5000); }
int getNH3PPM(int adc)  { return constrain(map(adc, 300, 3800, 0,  300), 0,  300); }

int smoothValue(int *buffer, int newValue) {
  buffer[gasIndex] = newValue;
  int sum = 0, count = filterFilled ? GAS_FILTER_SIZE : gasIndex + 1;
  for (int i = 0; i < count; i++) sum += buffer[i];
  return sum / count;
}

void processModemURC() {
  static String urc = "";
  while (Serial1.available()) {
    char c = Serial1.read();
    if (c == '\n') {
      urc.trim();
      if (urc.length()) {
        Serial.println("URC: " + urc);
        if (urc.indexOf("NO CARRIER") >= 0 || urc.indexOf("BUSY") >= 0 || urc.indexOf("CALL END") >= 0) {
          Serial.println("Call ended by remote");
          hangupCall(); callState = CALL_FAILED;
        }
      }
      urc = "";
    } else { urc += c; }
  }
}

// ================== MAIN LOOP ==================
void loop() {

  // 1. Handle instant power state change from ISR
  if (powerStateChanged && modemReady) handlePowerStateChange();

  // 2. Check 3-minute power failure timer
  if (powerFailurePending && modemReady &&
    (millis() - powerFailureDetectedAt >= POWER_FAILURE_DELAY)) {
  powerFailurePending = false;
  mainsPowerOK        = false;
  powerAlertSent      = false;
  Serial.println("POWER FAILURE CONFIRMED — 3 min elapsed, sending alert");
  sendPowerFailureSMS(pwrTemp, pwrHum, pwrGas, pwrNH3);
  resetCallState();
  smsSentForCurrentAlert = true;
  handleAlerts(pwrTemp, pwrHum, pwrGas, pwrNH3, true);
  powerAlertSent = true;
}

// 3. Keep power failure call rotation going if not yet acknowledged
if (!mainsPowerOK && !alertAcknowledged && modemReady) {
  handleAlerts(pwrTemp, pwrHum, pwrGas, pwrNH3, true);
}


  if (modemReady) processModemURC();
  server.handleClient();
  tickSoftwareClock();

  // 3. Periodic NTP re-sync
  unsigned long syncInterval = currentTime.valid ? TIME_UPDATE_INTERVAL : 300000UL;
  if (modemReady && millis() - lastTimeUpdate >= syncInterval) {
    lastTimeUpdate = millis();
    getInternetTime();
  }

  // 4. Main sensor + display loop (every 2 seconds)
  if (displayReady && (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL)) {
    lastDisplayUpdate = millis();

    float temperature = dht.readTemperature();
    float humidity    = dht.readHumidity();
    if (!isnan(temperature)) temperature += TEMP_OFFSET;

    int gasValue = smoothValue(gasSamples, getGasPPM(analogRead(MQ_GAS_PIN)));
    int nh3Value = smoothValue(nh3Samples, getNH3PPM(analogRead(MQ137_PIN)));

    gasIndex++;
    if (gasIndex >= GAS_FILTER_SIZE) { gasIndex = 0; filterFilled = true; }

    if (isnan(temperature) || temperature < -10 || temperature > 55) temperature = lastValidTemp;
    if (isnan(humidity)    || humidity    < 0   || humidity    > 100) humidity    = lastValidHum;
    lastValidTemp = temperature;
    lastValidHum  = humidity;

    // Update cached values for power SMS
    pwrTemp = temperature;
    pwrHum  = humidity;
    pwrGas  = gasValue;
    pwrNH3  = nh3Value;

    updateDailyStats(temperature, humidity);
    checkDailyReport(temperature, humidity, gasValue, nh3Value);
    updateDisplay(temperature, humidity, gasValue, nh3Value);

    Serial.println("--- Sensor Readings ---");
    Serial.printf("Temperature: %.1f C\n", temperature);
    Serial.printf("Humidity: %.0f %%\n",   humidity);
    Serial.printf("Gas: %d PPM\n",         gasValue);
    Serial.printf("Ammonia: %d PPM\n",     nh3Value);
    Serial.println();

    // Only call handleAlerts for sensor conditions here.
    // Power failure alert is handled separately above.
    if (!powerFailurePending && mainsPowerOK) {
      handleAlerts(temperature, humidity, gasValue, nh3Value, false);
    }
  }

  delay(10);
}